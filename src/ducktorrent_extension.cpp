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

#include "udp_discovery_peer.hpp"
#include "udp_discovery_peer_parameters.hpp"
#include "udp_discovery_ip_port.hpp"
#include "udp_discovery_protocol.hpp"

using namespace duckdb;

// Global peer object
udpdiscovery::Peer peer; 

// Structure to hold peer information
struct PeerInfo {
    std::string ip;
    int port;
    std::string user_data;
};

// Function to format peer info into a string
std::string FormatPeerInfo(const PeerInfo& peer) {
    std::stringstream ss;
    ss << "{\"ip\":\"" << peer.ip << "\",\"port\":" << peer.port 
       << ",\"user_data\":\"" << peer.user_data << "\"}";
    return ss.str();
}

// Function to announce presence
void AnnouncePresenceFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    udpdiscovery::PeerParameters parameters;
    parameters.set_port(12021);
    parameters.set_application_id(7681411);
    parameters.set_can_discover(true);
    parameters.set_can_be_discovered(true);
    
    // Enable both broadcast and multicast
    parameters.set_can_use_broadcast(true);
    parameters.set_can_use_multicast(true);
    parameters.set_multicast_group_address((224 << 24) + (0 << 16) + (0 << 8) + 123); // 224.0.0.123

    try {
        if (!peer.Start(parameters, input_value.ToString().c_str())) {
            throw std::runtime_error("Failed to start peer discovery");
        }
        result.SetValue(0, Value("Announced"));
    } catch (std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}

// Function to find peers
void FindPeersFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0);

    try {
        // Get list of discovered peers
        auto discovered_peers = peer.ListDiscovered();

        std::vector<PeerInfo> peer_list;
        std::stringstream json_array;

        json_array << "[";
        bool first = true;
        // Process each discovered peer
        for (const auto &discovered_peer : discovered_peers) {
            PeerInfo info;
            info.ip = discovered_peer.ip_port().ip();
            info.port = discovered_peer.ip_port().port();
            info.user_data = discovered_peer.user_data();

            if (!first) {
                json_array << ",";
            }
            json_array << FormatPeerInfo(info);
            first = false;
        }

        json_array << "]";

        // Set the result to be the JSON array of peers
        std::string peers_json = json_array.str();
        result.SetValue(0, Value(peers_json));

    } catch (std::exception& e) {
        result.SetValue(0, Value("Error: " + std::string(e.what())));
    }
}

// LoadInternal remains unchanged
static void LoadInternal(DatabaseInstance &instance) {
    auto announce_presence_function = ScalarFunction("announce_presence", {LogicalType::VARCHAR},
                                                    LogicalType::VARCHAR, AnnouncePresenceFunction);
    ExtensionUtil::RegisterFunction(instance, announce_presence_function);

    auto find_peers_function = ScalarFunction("find_peers", {LogicalType::VARCHAR},
                                              LogicalType::VARCHAR, FindPeersFunction);
    ExtensionUtil::RegisterFunction(instance, find_peers_function);
}


void DucktorrentExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string DucktorrentExtension::Name() {
	return "quack";
}

std::string DucktorrentExtension::Version() const {
#ifdef EXT_VERSION_DUCKTORRENT
	return EXT_VERSION_DUCKTORRENT;
#else
	return "";
#endif
}

// Extension entry point
extern "C" void Load(duckdb::DatabaseInstance &instance) {
    LoadInternal(instance);
}
