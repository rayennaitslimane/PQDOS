#pragma once

#include "Models.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib {
class Client;
}

using PlacementMap =
    std::unordered_map<std::string, std::vector<std::pair<std::string, Bytes>>>;

using ShardLocationMap =
    std::unordered_map<std::string, std::vector<std::string>>;

struct ParsedShardKey {
    std::string object_id;
    std::string version;
    uint32_t shard_index = 0;
};

std::pair<std::string, int> parse_address(const std::string& address);

httplib::Client& get_node_client(const std::string& node_address);

std::string make_shard_key(
    const std::string& object_id,
    const std::string& version,
    uint32_t shard_index
);

ParsedShardKey parse_shard_key(const std::string& location);

PlacementMap placement_strategy(
    const std::string& object_id,
    const std::string& version,
    const std::vector<Bytes>& serialized_shards,
    const std::vector<std::string>& eligible_nodes
);

void apply_placement(const PlacementMap& placement);

std::vector<std::string> build_shard_locations(const PlacementMap& placement);

ShardLocationMap parse_shard_locations(
    const std::vector<std::string>& shard_locations
);

std::vector<EncryptedShard> fetch_encrypted_shards(
    const ShardLocationMap& shard_location_map
);

// Enumerate every shard key stored on a node via GET /shards/list. Returns the
// shard keys (object_id/version/shard_index) without the node prefix. Used by
// the metadata rebuild path (ADR-0008). Returns empty on transport failure.
std::vector<std::string> list_node_shards(const std::string& node_address);

// Fetch a single stored shard and return its embedded manifest, if present.
// Returns nullopt on transport failure or for legacy payloads with no manifest.
std::optional<ShardManifest> fetch_shard_manifest(
    const std::string& node_address,
    const std::string& location
);

void remove_from_nodes(const ShardLocationMap& shard_location_map);

bool probe_shard(const std::string& node_address, const std::string& location);

// Fetch the raw stored payload for a single shard via GET /shards. Returns the
// bytes verbatim (a self-describing StoredShard envelope, ADR-0008) so a caller
// can copy a shard to another node without decoding/re-encrypting it. Returns
// nullopt on transport failure or a non-200 response.
std::optional<Bytes> fetch_raw_shard(
    const std::string& node_address,
    const std::string& location
);

// Compute the HRW (Highest Random Weight / rendezvous) intended placement for an
// object's shards. Returns a vector of size total_shards where element i is the
// intended node address for shard index i. Nodes are distinct across shards.
//
// Each shard independently ranks nodes by a rendezvous weight derived from
// (object_id, shard_index, node_address) and greedily takes the highest-weighted
// node not already assigned to a lower shard index. This yields a deterministic,
// membership-stable placement: adding or removing a node only reshuffles the
// shards for which that node would win, minimising data movement.
//
// Throws if eligible_nodes.size() < total_shards.
std::vector<std::string> hrw_intended_nodes(
    const std::string& object_id,
    const std::vector<std::string>& eligible_nodes,
    std::size_t total_shards
);
