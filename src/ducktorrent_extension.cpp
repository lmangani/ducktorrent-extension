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

// Include libdht headers
extern "C" {
#include <dht/node.h>
#include <dht/peers.h>
#include <dht/utils.h>
}

using namespace duckdb;

// Global DHT node object
struct dht_node dht_node;

// Function to send data
static void sock_send(const unsigned char *data, size_t len,
                      const struct sockaddr *dest, socklen_t addrlen,
                      void *opaque)
{
    int sock = *(int *)opaque;

    if (sendto(sock, data, len, 0, dest, addrlen) < 0) {
        fprintf(stderr, "sendto: %s\n", strerror(errno));
    }
}

// Function to start the DHT node
void DhtStartFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    int sock;
    struct sockaddr_in sin;
    
    // Create a UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        result.SetValue(0, Value("Error creating socket"));
        return;
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(12021); // Use a valid port

    // Bind the socket to the port
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        result.SetValue(0, Value("Error binding socket"));
        return;
    }

    // Initialize the DHT node
    if (dht_node_init(&dht_node, NULL, sock_send, &sock) < 0) {
        result.SetValue(0, Value("Error initializing DHT node"));
        return;
    }

    // Start the DHT node
    dht_node_start(&dht_node);
    result.SetValue(0, Value("DHT Node Started"));
}

// Function to stop the DHT node
void DhtStopFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    try {
        // Check if the function name is correct
        // Assuming you want to stop the node, ensure the right call is used if available
        dht_node_cleanup(&dht_node); // If this function doesn't exist, consult libdht documentation
        result.SetValue(0, Value("DHT Node Stopped"));
    } catch (std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}

// Function to announce presence
void AnnouncePresenceFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    // Assuming info_hash is provided in input
    // Modify the following line based on how your input is structured
    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    try {
        // Here we need a proper info_hash
        unsigned char info_hash[20];
        // Assume input_value can be converted or you have a way to get a valid info_hash
        // memcpy(info_hash, some_source, 20);  // Fill this with a valid 20-byte hash

        // Announce presence using the DHT node
        if (dht_announce_peer(&dht_node, info_hash, 12021, nullptr, nullptr, nullptr) != 0) {
            throw std::runtime_error("Failed to announce presence");
        }
        result.SetValue(0, Value("Announced Peer"));
    } catch (std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}

// Function to find peers
void FindPeersFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    try {
        // Implementation for discovering peers should go here
        std::stringstream json_array;
        json_array << "[]"; // Placeholder for peer discovery logic

        result.SetValue(0, Value(json_array.str()));
    } catch (std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
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
    auto find_peers_function = ScalarFunction("find_peers", {}, LogicalType::VARCHAR, FindPeersFunction);
    ExtensionUtil::RegisterFunction(instance, find_peers_function);
}

// DucktorrentExtension methods
void DucktorrentExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
}

std::string DucktorrentExtension::Name() {
    return "ducktorrent"; // Updated name
}

std::string DucktorrentExtension::Version() const {
#ifdef EXT_VERSION_DUCKTORRENT
    return EXT_VERSION_DUCKTORRENT;
#else
    return "unknown";
#endif
}

// Extension entry point
extern "C" void Load(duckdb::DatabaseInstance &instance) {
    LoadInternal(instance);
}

