#define DUCKDB_EXTENSION_MAIN

#include "ducktorrent_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

// System includes
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>

// DHT includes
extern "C" {
#include "dht.h"
}

using namespace duckdb;

// Constants
constexpr int DEFAULT_DHT_PORT = 6881;
constexpr size_t MAX_BOOTSTRAP_NODES = 20;
constexpr size_t ID_SIZE = 20;
constexpr int SEARCH_TIMEOUT_MS = 5000;  // 2 seconds timeout for peer search

// Bootstrap nodes - well-known DHT nodes
struct BootstrapNode {
    const char* host;
    int port;
};

static const BootstrapNode BOOTSTRAP_NODES[] = {
    {"router.bittorrent.com", 6881},
    {"router.utorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
    {"dht.libtorrent.org", 25401},
    {"dht.aelitis.com", 6881}
};

// Add periodic DHT maintenance function
static void dht_maintain() {
    time_t now;
    time(&now);
    dht_periodic(NULL, 0, NULL, 0, &now, NULL, NULL);
}

// Peer information structure
struct PeerInfo {
    std::string ip;
    int port;
    
    PeerInfo(const std::string& i, int p) : ip(i), port(p) {}
};

// Cache structure
struct HashCache {
    std::string info_hash;
    std::vector<PeerInfo> peers;
    
    HashCache(const std::string& hash) : info_hash(hash) {}
};

// Global state
static int dht_socket = -1;
static unsigned char node_id[ID_SIZE];
static bool dht_initialized = false;
static std::vector<HashCache> peer_cache;

static time_t last_peer_received = 0;
// Enhanced callback for DHT events
static void dht_callback(void *closure, int event, 
                        const unsigned char *info_hash,
                        const void *data, size_t data_len) {

    if(event == DHT_EVENT_SEARCH_DONE)
        printf("Search done.\n");
    else if(event == DHT_EVENT_SEARCH_DONE6)
        printf("IPv6 search done.\n");
    else if(event == DHT_EVENT_VALUES)
        printf("Received %d values.\n", (int)(data_len / 6));
    else if(event == DHT_EVENT_VALUES6)
        printf("Received %d IPv6 values.\n", (int)(data_len / 18));
    else
        printf("Unknown DHT event %d.\n", event);


    if (event == DHT_EVENT_VALUES || event == DHT_EVENT_VALUES6) {
        // Update last received timestamp
        time(&last_peer_received);
        
        // Convert info_hash to string for cache key
        char hex_hash[41];
        for(int i = 0; i < 20; i++)
            sprintf(hex_hash + i * 2, "%02x", info_hash[i]);
        hex_hash[40] = '\0';
        std::string hash_str(hex_hash);
        
        // Process peer data
        const struct sockaddr_in *peers = (const struct sockaddr_in*)data;
        size_t num_peers = data_len / sizeof(struct sockaddr_in);
        
        // Cache handling remains the same...
        std::vector<HashCache>::iterator it;
        for (it = peer_cache.begin(); it != peer_cache.end(); ++it) {
            if (it->info_hash == hash_str) {
                break;
            }
        }
        
        if (it == peer_cache.end()) {
            peer_cache.push_back(HashCache(hash_str));
            it = peer_cache.end() - 1;
        }
        
        for (size_t i = 0; i < num_peers; i++) {
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peers[i].sin_addr, addr, sizeof(addr));
            int port = ntohs(peers[i].sin_port);
            
            bool found = false;
            for (const PeerInfo& existing : it->peers) {
                if (existing.ip == addr && existing.port == port) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                it->peers.push_back(PeerInfo(addr, port));
            }
        }
    }
}

// Enhanced bootstrap function
static bool bootstrap_dht() {
    int successful_connections = 0;

    for (size_t i = 0; i < sizeof(BOOTSTRAP_NODES)/sizeof(BOOTSTRAP_NODES[0]); i++) {
        const BootstrapNode& node = BOOTSTRAP_NODES[i];
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        if (getaddrinfo(node.host, std::to_string(node.port).c_str(), &hints, &res) == 0) {
            struct sockaddr_in *addr = (struct sockaddr_in*)res->ai_addr;
            if (dht_ping_node((struct sockaddr*)addr, sizeof(*addr)) >= 0) {
                successful_connections++;
            }
            freeaddrinfo(res);
            if (successful_connections >= 2) {  // At least 2 successful bootstrap nodes
                return true;
            }
        }
    }
    return successful_connections > 0;
}

// Helper function to set socket non-blocking
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Initialize node ID
static void init_node_id() {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, node_id, ID_SIZE);
        close(fd);
    } else {
        // Fallback to using time-based random
        struct timeval tv;
        gettimeofday(&tv, NULL);
        for (size_t i = 0; i < ID_SIZE; i++) {
            node_id[i] = (tv.tv_usec >> (i % 4)) & 0xFF;
        }
    }
}

// DuckDB functions
void DhtStartFunction(DataChunk &input, ExpressionState &state, Vector &result) {

    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    if (input_value.IsNull()) {
        input_value = DEFAULT_DHT_PORT;
    }

    try {
        if (dht_initialized) {
            throw std::runtime_error("DHT Node already running");
        }

        // Create socket
        dht_socket = socket(PF_INET, SOCK_DGRAM, 0);
        if (dht_socket < 0) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }

        if (set_nonblocking(dht_socket) < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to set non-blocking: " + std::string(strerror(errno)));
        }

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
	uint16_t port_value = static_cast<uint16_t>(input_value.GetValue<int32_t>());
	sin.sin_port = htons(port_value);
        // sin.sin_port = htons(DEFAULT_DHT_PORT);
        sin.sin_addr.s_addr = INADDR_ANY;

        if (bind(dht_socket, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to bind socket: " + std::string(strerror(errno)));
        }

        init_node_id();
        if (dht_init(dht_socket, -1, node_id, (unsigned char*)"DC\0\0") < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to initialize DHT");
        }

        // Bootstrap the DHT network
        if (!bootstrap_dht()) {
            dht_uninit();
            close(dht_socket);
            throw std::runtime_error("Failed to bootstrap DHT network");
        }

        dht_initialized = true;
        result.SetValue(0, Value("DHT Node Started Successfully"));
    } catch (const std::exception& e) {
        if (dht_socket >= 0) {
            close(dht_socket);
            dht_socket = -1;
        }
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}


void DhtStopFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    try {
        if (!dht_initialized) {
            result.SetValue(0, Value("DHT Node Not Running"));
            return;
        }

        dht_uninit();
        close(dht_socket);
        dht_socket = -1;
        dht_initialized = false;

        result.SetValue(0, Value("DHT Node Stopped Successfully"));
    } catch (const std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}

void AnnouncePresenceFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    if (!dht_initialized) {
        result.SetValue(0, Value("Error: DHT node not started"));
        return;
    }

    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    if (input_value.IsNull()) {
        result.SetValue(0, Value("Error: input is null"));
        return;
    }

    try {
        std::string hex_hash = input_value.ToString();
        unsigned char info_hash[20];
        // Convert hex string to binary
        for (int i = 0; i < 20; i++) {
            int value;
            sscanf(hex_hash.c_str() + i * 2, "%02x", &value);
            info_hash[i] = value;
        }

        // Announce on random port
        int port = 1024 + (rand() % (65535 - 1024));
        if (dht_search(info_hash, port, AF_INET, dht_callback, nullptr) < 0) {
            throw std::runtime_error("Failed to announce presence");
        }

        result.SetValue(0, Value("Successfully announced peer with port " + std::to_string(port)));
    } catch (const std::exception& e) {
        result.SetValue(0, Value("Error during announce: " + std::string(e.what())));
    }
}

// Enhanced FindPeersFunction
void FindPeersFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    if (!dht_initialized) {
        result.SetValue(0, Value("Error: DHT node not started"));
        return;
    }

    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    if (input_value.IsNull()) {
        result.SetValue(0, Value("Error: input is null"));
        return;
    }

    try {
        std::string hex_hash = input_value.ToString();
        unsigned char info_hash[20];
        for (int i = 0; i < 20; i++) {
            int value;
            sscanf(hex_hash.c_str() + i * 2, "%02x", &value);
            info_hash[i] = value;
        }

        // Reset last received timestamp
        time(&last_peer_received);
        
        // Start DHT search
        if (dht_search(info_hash, 0, AF_INET, dht_callback, nullptr) < 0) {
            throw std::runtime_error("Failed to search for peers");
        }

        // Wait for responses with timeout
        time_t start_time = time(NULL);
        time_t current_time;
        
        while (true) {
            current_time = time(NULL);
            
            // Check timeout
            if (current_time - start_time >= SEARCH_TIMEOUT_MS/1000) {
                break;
            }
            
            // Process DHT messages
            unsigned char buf[4096];
            struct sockaddr_storage from;
            socklen_t fromlen = sizeof(from);
            
            // Non-blocking read from socket
            int rc = recvfrom(dht_socket, buf, sizeof(buf) - 1, 0,
                            (struct sockaddr*)&from, &fromlen);
            
            if (rc > 0) {
                buf[rc] = '\0';
                time_t now;
                time(&now);
                dht_periodic(buf, rc, (struct sockaddr*)&from, fromlen, &now, NULL, NULL);
            }
            
            // Run DHT maintenance
            dht_maintain();
            
            // Small sleep to prevent CPU spinning
            usleep(50000);  // 50ms sleep
            
            // If we received peers recently, wait a bit more for additional responses
            if (current_time - last_peer_received < 1) {
                start_time = current_time;
            }
        }

        // Format results
        std::vector<HashCache>::const_iterator it;
        for (it = peer_cache.begin(); it != peer_cache.end(); ++it) {
            if (it->info_hash == hex_hash) {
                break;
            }
        }

        std::stringstream json;
        json << "{\"info_hash\":\"" << hex_hash << "\",\"peers\":[";
        
        if (it != peer_cache.end()) {
            bool first = true;
            for (const PeerInfo& peer : it->peers) {
                if (!first) json << ",";
                json << "{\"ip\":\"" << peer.ip << "\",\"port\":" << peer.port << "}";
                first = false;
            }
        }
        
        json << "]}";

        result.SetValue(0, Value(json.str()));
    } catch (const std::exception& e) {
        result.SetValue(0, Value("Error during peer search: " + std::string(e.what())));
    }
}

void DucktorrentExtension::Load(DuckDB &db) {
    // Register functions
    auto dht_start = ScalarFunction("dht_start", {LogicalType::VARCHAR}, LogicalType::VARCHAR, DhtStartFunction);
    auto dht_stop = ScalarFunction("dht_stop", {}, LogicalType::VARCHAR, DhtStopFunction);
    auto announce = ScalarFunction("announce_presence", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AnnouncePresenceFunction);
    auto find_peers = ScalarFunction("find_peers", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FindPeersFunction);

    ExtensionUtil::RegisterFunction(*db.instance, dht_start);
    ExtensionUtil::RegisterFunction(*db.instance, dht_stop);
    ExtensionUtil::RegisterFunction(*db.instance, announce);
    ExtensionUtil::RegisterFunction(*db.instance, find_peers);
}

std::string DucktorrentExtension::Name() {
    return "ducktorrent";
}

std::string DucktorrentExtension::Version() const {
#ifdef EXT_VERSION_DUCKTORRENT
	return EXT_VERSION_DUCKTORRENT;
#else
	return "v1.1.1";
#endif
}

