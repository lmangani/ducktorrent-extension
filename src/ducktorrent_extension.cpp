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

// Global state
static int dht_socket = -1;
static unsigned char node_id[ID_SIZE];
static bool dht_initialized = false;

// Callback for DHT events
static void dht_callback(void *closure, int event, 
                        const unsigned char *info_hash,
                        const void *data, size_t data_len) {
    switch(event) {
        case DHT_EVENT_VALUES:
        case DHT_EVENT_VALUES6:
            // Store values for retrieval by find_peers
            break;
        case DHT_EVENT_SEARCH_DONE:
        case DHT_EVENT_SEARCH_DONE6:
            break;
    }
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
    try {
        if (dht_initialized) {
            throw std::runtime_error("DHT Node already running");
        }

        // Create socket
        dht_socket = socket(PF_INET, SOCK_DGRAM, 0);
        if (dht_socket < 0) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }

        // Set non-blocking
        if (set_nonblocking(dht_socket) < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to set non-blocking: " + std::string(strerror(errno)));
        }

        // Bind socket
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(DEFAULT_DHT_PORT);
        sin.sin_addr.s_addr = INADDR_ANY;

        if (bind(dht_socket, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to bind socket: " + std::string(strerror(errno)));
        }

        // Initialize node ID and DHT
        init_node_id();
        if (dht_init(dht_socket, -1, node_id, (unsigned char*)"DC\0\0") < 0) {
            close(dht_socket);
            throw std::runtime_error("Failed to initialize DHT");
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

        if (dht_search(info_hash, 0, AF_INET, dht_callback, nullptr) < 0) {
            throw std::runtime_error("Failed to search for peers");
        }

        // Get current peers
        struct sockaddr_in sins[100];
        struct sockaddr_in6 sin6s[100];
        int num = 100, num6 = 100;
        int total = dht_get_nodes(sins, &num, sin6s, &num6);

        // Format response as JSON
        std::stringstream json;
        json << "{\"info_hash\":\"" << hex_hash << "\",\"peers\":[";
        for (int i = 0; i < num; i++) {
            if (i > 0) json << ",";
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sins[i].sin_addr, addr, sizeof(addr));
            json << "{\"ip\":\"" << addr << "\",\"port\":" << ntohs(sins[i].sin_port) << "}";
        }
        json << "]}";

        result.SetValue(0, Value(json.str()));
    } catch (const std::exception& e) {
        result.SetValue(0, Value("Error during peer search: " + std::string(e.what())));
    }
}

void DucktorrentExtension::Load(DuckDB &db) {
    // Register functions
    auto dht_start = ScalarFunction("dht_start", {}, LogicalType::VARCHAR, DhtStartFunction);
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

