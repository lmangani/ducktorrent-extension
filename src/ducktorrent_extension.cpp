#define DUCKDB_EXTENSION_MAIN

#include "ducktorrent_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"

// Include system libs
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <sys/time.h>    // Added for gettimeofday
#include <fcntl.h>       // Added for fcntl
#include <stdexcept>     // Added for std::runtime_error

// Include libdht headers
extern "C" {
#include <dht/node.h>
#include <dht/peers.h>
#include <dht/utils.h>
}

using namespace duckdb;

// Constants
constexpr int DEFAULT_DHT_PORT = 12021;
constexpr int DEFAULT_TIMEOUT_MS = 1000;
constexpr size_t MAX_PACKET_SIZE = 2048;

// Global DHT node object
struct dht_node dht_node;
int global_socket = -1;  // Track the socket globally for cleanup


// Enhanced error handling
class DHTError : public std::runtime_error {
public:
    explicit DHTError(const std::string& message) : std::runtime_error(message) {}
};

// Enhanced callback structure
struct get_peers_priv {
    int done;
    unsigned char info_hash[20];
    struct sockaddr_storage *peers;
    size_t count;
    int error_code;
    
    get_peers_priv() : done(0), peers(nullptr), count(0), error_code(0) {
        memset(info_hash, 0, sizeof(info_hash));
    }
    
    ~get_peers_priv() {
        if (peers) {
            free(peers);
            peers = nullptr;
        }
    }
};

// Function to send data with improved error handling
static void sock_send(const unsigned char *data, size_t len,
                     const struct sockaddr *dest, socklen_t addrlen,
                     void *opaque) {
    int sock = *(int *)opaque;
    ssize_t sent = sendto(sock, data, len, 0, dest, addrlen);
    if (sent < 0) {
        fprintf(stderr, "sendto error: %s\n", strerror(errno));
    } else if (static_cast<size_t>(sent) < len) {
        fprintf(stderr, "partial send: %zd of %zu bytes\n", sent, len);
    }
}

// Improved callback function with better memory management
static void get_peers_complete(unsigned char const *info_hash,
                             struct sockaddr_storage const *peer,
                             unsigned long token, void *opaque) {
    auto *priv = static_cast<get_peers_priv *>(opaque);

    if (!priv || !peer) {
        if (priv) priv->error_code = EINVAL;
        return;
    }

    size_t new_size = (priv->count + 1) * sizeof(struct sockaddr_storage);
    void *tmp = realloc(priv->peers, new_size);
    if (!tmp) {
        priv->error_code = ENOMEM;
        return;
    }

    priv->peers = static_cast<struct sockaddr_storage *>(tmp);
    memcpy(&priv->peers[priv->count], peer, sizeof(struct sockaddr_storage));
    if (info_hash) {
        memcpy(priv->info_hash, info_hash, 20);
    }
    priv->count++;
}

// Improved process_dht_events with better error handling and timeout management
static void process_dht_events(struct dht_node *node, get_peers_priv *priv, int timeout_ms = DEFAULT_TIMEOUT_MS) {
    struct timeval start_time, current_time, elapsed_time;
    if (gettimeofday(&start_time, NULL) < 0) {
        priv->error_code = errno;
        return;
    }

    while (!priv->done && priv->error_code == 0) {
        struct timeval tv;
        fd_set rfds;

        if (gettimeofday(&current_time, NULL) < 0) {
            priv->error_code = errno;
            break;
        }

        timersub(&current_time, &start_time, &elapsed_time);
        int elapsed_ms = elapsed_time.tv_sec * 1000 + elapsed_time.tv_usec / 1000;
        if (elapsed_ms >= timeout_ms) {
            priv->error_code = ETIMEDOUT;
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(global_socket, &rfds);

        dht_node_timeout(node, &tv);

        struct timeval remaining;
        remaining.tv_sec = (timeout_ms - elapsed_ms) / 1000;
        remaining.tv_usec = ((timeout_ms - elapsed_ms) % 1000) * 1000;

        if (timercmp(&tv, &remaining, >)) {
            tv = remaining;
        }

        int rc = select(global_socket + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            priv->error_code = errno;
            break;
        }

        if (rc > 0 && FD_ISSET(global_socket, &rfds)) {
            unsigned char buf[MAX_PACKET_SIZE];
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);

            rc = recvfrom(global_socket, buf, sizeof(buf), 0,
                         (struct sockaddr *)&ss, &sl);
            if (rc < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                priv->error_code = errno;
                break;
            }

            dht_node_input(node, buf, rc, (struct sockaddr *)&ss, sl);
        }

        dht_node_work(node);
    }
}

// WIP DHT Bootstrapping
static int bootstrap_status = 0;

// Add callback function
static void bootstrap_status_cb(int complete, void *data) {
    int *status = (int *)data;
    *status = complete;
    fprintf(stderr, "DHT bootstrap status: %s\n", complete ? "complete" : "in progress");
}

void DhtStartFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    try {
        if (global_socket != -1) {
            throw DHTError("DHT Node already running");
        }

        // First try IPv6 socket which can handle both IPv6 and IPv4
        global_socket = socket(AF_INET6, SOCK_DGRAM, 0);
        bool ipv6_socket = true;

        // Fall back to IPv4 if IPv6 is not supported
        if (global_socket < 0) {
            global_socket = socket(AF_INET, SOCK_DGRAM, 0);
            ipv6_socket = false;
            
            if (global_socket < 0) {
                throw DHTError("Error creating socket: " + std::string(strerror(errno)));
            }
        }

        // Make socket non-blocking
        int flags = fcntl(global_socket, F_GETFL, 0);
        if (flags < 0 || fcntl(global_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(global_socket);
            global_socket = -1;
            throw DHTError("Error setting non-blocking mode: " + std::string(strerror(errno)));
        }

        // Set socket options
        int reuse = 1;
        if (setsockopt(global_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            close(global_socket);
            global_socket = -1;
            throw DHTError("Error setting SO_REUSEADDR: " + std::string(strerror(errno)));
        }

        // For IPv6 socket, ensure it can handle IPv4 connections (IPv6_V6ONLY = 0)
        if (ipv6_socket) {
            int v6only = 0;
            if (setsockopt(global_socket, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
                // If this fails, we'll still try to use the socket
                fprintf(stderr, "Warning: Could not set IPV6_V6ONLY=0: %s\n", strerror(errno));
            }
        }

        // Bind socket
        if (ipv6_socket) {
            struct sockaddr_in6 sin6 = {};
            sin6.sin6_family = AF_INET6;
            sin6.sin6_addr = in6addr_any;
            sin6.sin6_port = htons(DEFAULT_DHT_PORT);

            if (bind(global_socket, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
                close(global_socket);
                global_socket = -1;
                throw DHTError("Error binding IPv6 socket: " + std::string(strerror(errno)));
            }
        } else {
            struct sockaddr_in sin = {};
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = INADDR_ANY;
            sin.sin_port = htons(DEFAULT_DHT_PORT);

            if (bind(global_socket, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
                close(global_socket);
                global_socket = -1;
                throw DHTError("Error binding IPv4 socket: " + std::string(strerror(errno)));
            }
        }

        // Initialize DHT node
        if (dht_node_init(&dht_node, NULL, sock_send, &global_socket) < 0) {
            close(global_socket);
            global_socket = -1;
            throw DHTError("Error initializing DHT node");
        }

        // Set bootstrap callback before starting node
        bootstrap_status = 0;
        dht_node_set_bootstrap_callback(&dht_node, bootstrap_status_cb, &bootstrap_status);
        
        dht_node_start(&dht_node);

        // Wait for bootstrap to complete with timeout
        struct timeval start, now;
        gettimeofday(&start, NULL);
        
        while (!bootstrap_status) {
            gettimeofday(&now, NULL);
            if (now.tv_sec - start.tv_sec > 10) { // 10 second timeout
                fprintf(stderr, "DHT bootstrap timed out\n");
                break;
            }
            
            // Process DHT events while waiting
            get_peers_priv priv = {};
            process_dht_events(&dht_node, &priv, 1000); // Check every second
        }

        if (bootstrap_status) {
            result.SetValue(0, Value("DHT Node Started and Bootstrapped Successfully"));
        } else {
            result.SetValue(0, Value("DHT Node Started but Bootstrap Incomplete"));
        }
    } catch (const std::exception& e) {
        // If we failed at any point, ensure socket is closed
        if (global_socket != -1) {
            close(global_socket);
            global_socket = -1;
        }
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}


// Function to stop the DHT node
void DhtStopFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    try {
        if (global_socket != -1) {
            dht_node_cleanup(&dht_node);
            close(global_socket);
            global_socket = -1;
            result.SetValue(0, Value("DHT Node Stopped Successfully"));
        } else {
            result.SetValue(0, Value("DHT Node Not Running"));
        }
    } catch (std::exception& e) {
        result.SetValue(0, Value("Error stopping DHT node: " + std::string(e.what())));
    }
}

// Function to announce presence
void AnnouncePresenceFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    if (global_socket == -1) {
        result.SetValue(0, Value("Error: DHT node not started"));
        return;
    }

    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    if (input_value.IsNull()) {
        result.SetValue(0, Value("Error: input is null"));
        return;
    }

    std::string hex_value = input_value.ToString();
    unsigned char info_hash[20];
    if (from_hex(hex_value.c_str(), info_hash) != 0) {
        result.SetValue(0, Value("Error: invalid hex input"));
        return;
    }

    get_peers_priv priv = {};
    priv.done = 0;
    priv.error_code = 0;
    
    // Use random port between 1024 and 65535
    int portnum = 1024 + (rand() % (65535 - 1024));
    
    int rc = dht_announce_peer(&dht_node, info_hash, portnum, get_peers_complete, &priv, nullptr);
    if (rc != 0) {
        result.SetValue(0, Value("Error: Failed to announce peer - " + std::string(strerror(errno))));
        return;
    }

    process_dht_events(&dht_node, &priv);

    if (priv.error_code) {
        std::string error_msg = "Error during announce: " + std::string(strerror(priv.error_code));
        result.SetValue(0, Value(error_msg));
    } else if (priv.count > 0) {
        result.SetValue(0, Value("Successfully announced peer with port " + std::to_string(portnum)));
    } else {
        result.SetValue(0, Value("Announcement completed but no peers found"));
    }

    if (priv.peers) {
        free(priv.peers);
    }
}

// Function to find peers
void FindPeersFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    if (global_socket == -1) {
        result.SetValue(0, Value("Error: DHT node not started"));
        return;
    }

    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    if (input_value.IsNull()) {
        result.SetValue(0, Value("Error: input is null"));
        return;
    }

    std::string hex_value = input_value.ToString();
    unsigned char info_hash[20];
    if (from_hex(hex_value.c_str(), info_hash) != 0) {
        result.SetValue(0, Value("Error: invalid hex input"));
        return;
    }

    get_peers_priv priv = {};
    priv.done = 0;
    priv.error_code = 0;

    int rc = dht_get_peers(&dht_node, info_hash, get_peers_complete, &priv, nullptr);
    if (rc != 0) {
        result.SetValue(0, Value("Error: Failed to find peers - " + std::string(strerror(errno))));
        return;
    }

    process_dht_events(&dht_node, &priv);

    if (priv.error_code) {
        std::string error_msg = "Error during peer search: " + std::string(strerror(priv.error_code));
        result.SetValue(0, Value(error_msg));
        if (priv.peers) free(priv.peers);
        return;
    }

    std::stringstream json_array;
    json_array << "{\"info_hash\":\"" << hex_value << "\",\"peers\":[";
    
    for (size_t i = 0; i < priv.count; i++) {
        if (i > 0) json_array << ",";
        
        char ip_str[INET6_ADDRSTRLEN];
        uint16_t port = 0;
        
        struct sockaddr_storage *peer = &priv.peers[i];
        if (peer->ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)peer;
            inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(sin->sin_port);
        } else if (peer->ss_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)peer;
            inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(sin6->sin6_port);
        }
        
        json_array << "{\"ip\":\"" << ip_str << "\",\"port\":" << port << "}";
    }
    json_array << "]}";

    result.SetValue(0, Value(json_array.str()));
    
    if (priv.peers) {
        free(priv.peers);
    }
}

// Load function for DuckDB
static void LoadInternal(DatabaseInstance &instance) {
    // Register DHT start function
    auto dht_start_function = ScalarFunction("dht_start", {}, LogicalType::VARCHAR, DhtStartFunction);
    ExtensionUtil::RegisterFunction(instance, dht_start_function);

    // Register DHT stop function
    auto dht_stop_function = ScalarFunction("dht_stop", {}, LogicalType::VARCHAR, DhtStopFunction);
    ExtensionUtil::RegisterFunction(instance, dht_stop_function);

    // Register announce presence function
    auto announce_presence_function = ScalarFunction("announce_presence", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AnnouncePresenceFunction);
    ExtensionUtil::RegisterFunction(instance, announce_presence_function);

    // Register find peers function
    auto find_peers_function = ScalarFunction("find_peers", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FindPeersFunction);
    ExtensionUtil::RegisterFunction(instance, find_peers_function);
}

// DucktorrentExtension methods
void DucktorrentExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string DucktorrentExtension::Name() {
    return "ducktorrent";
}

std::string DucktorrentExtension::Version() const {
#ifdef EXT_VERSION_DUCKTORRENT
    return EXT_VERSION_DUCKTORRENT;
#else
    return "1.1.2";
#endif
}

// Extension entry point
extern "C" void Load(duckdb::DatabaseInstance &instance) {
    LoadInternal(instance);
}
