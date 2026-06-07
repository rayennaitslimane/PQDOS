#include "StorageClient.hpp"

#include "Crypto.hpp"
#include "ErasureCodec.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

namespace {

// =========================
// Constants
// =========================

constexpr std::size_t kNumNodes = 3;
constexpr uint32_t kDataShards = 2;
constexpr uint32_t kParityShards = 1;
constexpr std::size_t kShardSize = 20;

// Replace these paths with your real node paths.
const std::array<std::string, kNumNodes> kNodePaths = {
    "./data/node1",
    "./data/node2",
    "./data/node3"
};

// Demo / MVP master key (32 bytes)
const std::array<uint8_t, 32> kMasterKey = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};

// =========================
// Internal types
// =========================

// Placement map:
// {
//   "nodepath" -> [
//      ("object_id-shard_id", serialized_shard_bytes),
//      ...
//   ]
// }
using PlacementMap =
    std::unordered_map<std::string, std::vector<std::pair<std::string, Bytes>>>;

using ShardLocationMap =
    std::unordered_map<std::string, std::vector<std::string>>;

// =========================
// Helpers
// =========================

std::string bytes_to_hex(const Bytes& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }

    return oss.str();
}

std::string make_shard_location(const std::string& object_id, uint32_t shard_index) {
    return object_id + "-" + std::to_string(shard_index);
}

ErasureSpec make_erasure_spec() {
    ErasureSpec spec;
    spec.data_shards = kDataShards;
    spec.parity_shards = kParityShards;
    spec.shard_size = kShardSize;
    return spec;
}

// MVP placement strategy:
// - one shard per node
// - shard i goes to node i
PlacementMap placement_strategy(
    const std::string& object_id,
    const std::vector<Bytes>& serialized_shards
) {
    const std::size_t total_shards = serialized_shards.size();

    if (total_shards > kNodePaths.size()) {
        throw std::runtime_error(
            "placement_strategy: not enough nodes for number of shards"
        );
    }

    PlacementMap placement;

    for (std::size_t i = 0; i < total_shards; ++i) {
        const std::string& node_path = kNodePaths[i];
        const std::string location = make_shard_location(
            object_id,
            static_cast<uint32_t>(i)
        );

        placement[node_path].push_back({location, serialized_shards[i]});
    }

    return placement;
}

void apply_placement(
    const PlacementMap& placement,
    const std::unordered_map<std::string, std::unique_ptr<StorageNode>>& nodes
) {
    for (const auto& [node_path, entries] : placement) {
        auto it = nodes.find(node_path);
        if (it == nodes.end() || it->second == nullptr) {
            throw std::runtime_error(
                "apply_placement: node not initialized for path: " + node_path
            );
        }

        std::vector<std::string> locations;
        std::vector<Bytes> payloads;
        locations.reserve(entries.size());
        payloads.reserve(entries.size());

        for (const auto& [location, payload] : entries) {
            locations.push_back(location);
            payloads.push_back(payload);
        }

        it->second->put(locations, payloads);
    }
}

// Build metadata.shard_locations as an ordered vector of node paths.
// The index in the vector equals the shard index.
std::vector<std::string> build_shard_locations(const PlacementMap& placement) {
    std::size_t total_shards = 0;
    for (const auto& [node_path, entries] : placement) {
        total_shards += entries.size();
    }

    std::vector<std::string> shard_locations(total_shards);

    for (const auto& [node_path, entries] : placement) {
        for (const auto& [location, payload] : entries) {
            const auto pos = location.rfind('-');
            if (pos == std::string::npos) {
                throw std::runtime_error("build_shard_locations: invalid location");
            }

            const std::size_t shard_index = std::stoul(location.substr(pos + 1));
            shard_locations.at(shard_index) = node_path + "/" + location;
        }
    }

    return shard_locations;
}

ShardLocationMap parse_shard_locations(const std::vector<std::string>& shard_locations) {
    ShardLocationMap result;

    for (const auto& shard_location : shard_locations) {
        const auto pos = shard_location.rfind('/');
        if (pos == std::string::npos) {
            throw std::runtime_error("parse_shard_locations: invalid shard location");
        }

        const std::string node_path = shard_location.substr(0, pos);
        const std::string location = shard_location.substr(pos + 1);

        result[node_path].push_back(location);
    }

    return result;
}

std::vector<EncryptedShard> fetch_encrypted_shards(
    const ShardLocationMap& shard_location_map,
    const std::unordered_map<std::string, std::unique_ptr<StorageNode>>& nodes
) {
    std::vector<EncryptedShard> encrypted_shards;

    for (const auto& [node_path, locations] : shard_location_map) {
        auto it = nodes.find(node_path);

        // Missing node -> tolerate and continue
        if (it == nodes.end() || it->second == nullptr) {
            continue;
        }

        try {
            const auto payloads = it->second->get(locations);

            const std::size_t count = std::min(payloads.size(), locations.size());

            for (std::size_t i = 0; i < count; ++i) {
                if (!payloads[i].has_value()) {
                    continue; // missing shard
                }

                try {
                    encrypted_shards.push_back(
                        EncryptedShard::deserialize(*payloads[i])
                    );
                } catch (...) {
                    // malformed/corrupt serialized shard -> ignore
                    continue;
                }
            }
        } catch (...) {
            // node unavailable, filesystem path deleted, etc.
            continue;
        }
    }

    return encrypted_shards;
}

void remove_from_nodes(
    const ShardLocationMap& shard_location_map,
    const std::unordered_map<std::string, std::unique_ptr<StorageNode>>& nodes
) {
    for (const auto& [node_path, locations] : shard_location_map) {
        auto it = nodes.find(node_path);
        if (it == nodes.end() || it->second == nullptr) {
            continue;
        }

        try {
            (void)it->second->remove(locations);
        } catch (...) {
            // Ignore missing node / already-deleted shard / backend failure
            continue;
        }
    }
}

} // namespace

// =========================
// StorageClient
// =========================

StorageClient::StorageClient(const std::string& metadata_conn_str)
    : metadata_store_(metadata_conn_str) {
}

void StorageClient::init() {
    nodes_.clear();

    for (const auto& node_path : kNodePaths) {
        nodes_.emplace(
            node_path,
            std::make_unique<StorageNode>(node_path.c_str())
        );
    }
}

void StorageClient::put(const std::string& object_id, const Bytes& bytes) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::put: object_id cannot be empty");
    }

    if (bytes.empty()) {
        throw std::invalid_argument("StorageClient::put: bytes cannot be empty");
    }

    if (nodes_.size() != kNumNodes) {
        throw std::runtime_error(
            "StorageClient::put: nodes are not initialized; call init() first"
        );
    }

    // 1) Generate checksum from bytes
    const Bytes checksum_bytes = checksum_sha256(bytes);
    const std::string checksum_hex = bytes_to_hex(checksum_bytes);

    // 2) Generate erasure spec using constants
    ErasureSpec erasure_spec = make_erasure_spec();

    // 3) Generate plain shards from bytes
    Bytes mutable_input = bytes; // encode() takes non-const Bytes&
    std::vector<PlainShard> plain_shards = encode(mutable_input, erasure_spec);

    const std::size_t expected_total_shards =
        static_cast<std::size_t>(kDataShards + kParityShards);

    if (plain_shards.size() != expected_total_shards) {
        throw std::runtime_error(
            "StorageClient::put: encode() returned unexpected number of shards"
        );
    }

    // 4) Encrypt and serialize each plain shard into ordered vector of bytes
    std::vector<EncryptedShard> encrypted_shards =
        encrypt_shards(plain_shards, kMasterKey);

    if (encrypted_shards.size() != plain_shards.size()) {
        throw std::runtime_error(
            "StorageClient::put: encrypt_shards() returned unexpected number of shards"
        );
    }

    std::vector<Bytes> serialized_shards;
    serialized_shards.reserve(encrypted_shards.size());

    for (const auto& encrypted_shard : encrypted_shards) {
        serialized_shards.push_back(encrypted_shard.serialize());
    }

    // 5) placement_strategy(serialized_shards)
    PlacementMap placement = placement_strategy(object_id, serialized_shards);

    // 6) apply_placement(map)
    apply_placement(placement, nodes_);

    // 7) Generate metadata and store using MetadataStore
    ObjectMetadata metadata;
    metadata.id = object_id;
    metadata.size = bytes.size();
    metadata.checksum = checksum_hex;
    metadata.erasure = erasure_spec;
    metadata.shard_locations = build_shard_locations(placement);

    metadata_store_.put(metadata);
}

Bytes StorageClient::get(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::get: object_id cannot be empty");
    }

    if (nodes_.size() != kNumNodes) {
        throw std::runtime_error(
            "StorageClient::get: nodes are not initialized; call init() first"
        );
    }

    // 1) Load metadata
    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        throw std::runtime_error("StorageClient::get: object not found: " + object_id);
    }

    const ObjectMetadata& metadata = *metadata_opt;

    // 2) Parse shard locations into:
    //    { "node_id" -> vector<"object_id-shard_id"> }
    const ShardLocationMap shard_location_map =
        parse_shard_locations(metadata.shard_locations);

    // 3) Fetch encrypted shard payloads from nodes
    std::vector<EncryptedShard> encrypted_shards =
        fetch_encrypted_shards(shard_location_map, nodes_);

    if (encrypted_shards.size() < metadata.erasure.data_shards) {
        throw std::runtime_error(
            "StorageClient::get: not enough shards to reconstruct object"
        );
    }

    // 4) Decrypt shards
    const std::vector<PlainShard> plain_shards =
        decrypt_shards(encrypted_shards, kMasterKey);

    // 5) Decode original bytes
    if (plain_shards.size() < metadata.erasure.data_shards) {
        throw std::runtime_error(
            "StorageClient::get: not enough valid shards to reconstruct object"
        );
    }

    Bytes decoded = decode(plain_shards, metadata.erasure, metadata.size);

    // 6) Verify checksum
    const std::string checksum_hex = bytes_to_hex(checksum_sha256(decoded));
    if (checksum_hex != metadata.checksum) {
        throw std::runtime_error("StorageClient::get: checksum mismatch");
    }

    return decoded;
}

bool StorageClient::remove(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::remove: object_id cannot be empty");
    }

    if (nodes_.size() != kNumNodes) {
        throw std::runtime_error(
            "StorageClient::remove: nodes are not initialized; call init() first"
        );
    }

    // 1) Load metadata
    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        return false;
    }

    const ObjectMetadata& metadata = *metadata_opt;

    // 2) Parse metadata shard locations into:
    //    { "node_id" -> vector<"object_id-shard_id"> }
    const ShardLocationMap shard_location_map =
        parse_shard_locations(metadata.shard_locations);

    // 3) Remove shard payloads from storage nodes
    remove_from_nodes(shard_location_map, nodes_);

    // 4) Remove metadata
    return metadata_store_.remove(object_id);
}

std::vector<ObjectMetadata> StorageClient::list() {
    return metadata_store_.list();
}