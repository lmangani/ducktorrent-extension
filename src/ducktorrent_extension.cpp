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

// Function to announce presence
void AnnouncePresenceFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    // Assuming input is a single VARCHAR column
    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0); // Get first value

    // Create parameters for the peer
    udpdiscovery::PeerParameters parameters;
    parameters.set_port(12021); // Use the designated port
    parameters.set_application_id(7681412); // Set your application ID
    parameters.set_can_discover(true);
    parameters.set_can_be_discovered(true);

    // Start the discovery peer with input data
    peer.Start(parameters, input_value.ToString().c_str());

    // Set the result to be the same as the input
    // result.SetValue(0, input_value);
    result.SetValue(0, "Announced to local");
}

// Function to find peers
void FindPeersFunction(DataChunk &input, ExpressionState &state, Vector &result) {
    // Assuming input is a single VARCHAR column
    auto &input_column = input.data[0];
    auto input_value = input_column.GetValue(0); // Get first value

    // List discovered peers
    auto new_discovered_peers = peer.ListDiscovered();

    // Construct a string of peer IPs
    std::string peers;
    for (const auto &discovered_peer : new_discovered_peers) {
        if (!peers.empty()) {
            peers += ", "; // Separate peers by comma
        }
        peers += discovered_peer.ip_port().ip(); // Add the peer IP to the list
    }

    // Set the result to be the found peers
    result.SetValue(0, peers.empty() ? "no peers" : peers);
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
