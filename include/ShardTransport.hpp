#pragma once

#include "Models.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

void remove_from_nodes(const ShardLocationMap& shard_location_map);

bool probe_shard(const std::string& node_address, const std::string& location);
