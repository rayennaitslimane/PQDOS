#include "StorageClient.hpp"

#include "Crypto.hpp"
#include "ErasureCodec.hpp"

#include <botan/auto_rng.h>
#include <botan/mem_ops.h>
#include <botan/pk_algs.h>
#include <httplib.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <future>
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

std::array<std::string, kNumNodes> parseNodeAddresses() {
    const char* env = std::getenv("NODE_ADDRESSES");
    if (!env) {
        return {"localhost:9001", "localhost:9002", "localhost:9003"};
    }
    std::array<std::string, kNumNodes> addresses;
    std::istringstream stream(env);
    std::string token;
    std::size_t i = 0;
    while (std::getline(stream, token, ',') && i < kNumNodes) {
        addresses[i++] = token;
    }
    if (i != kNumNodes) {
        throw std::runtime_error(
            "NODE_ADDRESSES must contain exactly 3 comma-separated addresses"
        );
    }
    return addresses;
}

const std::array<std::string, kNumNodes> kNodeAddresses = parseNodeAddresses();

const std::string kDefaultKekFilePath = "/tmp/pqdos_test_kek.json";

std::string parseKekFilePath() {
    const char* env = std::getenv("PQDOS_KEYSTORE_PATH");
    if (env) {
        return env;
    }
    return kDefaultKekFilePath;
}

std::string generateKekId() {
    Botan::AutoSeeded_RNG rng;
    std::array<uint8_t, 16> bytes{};
    rng.randomize(bytes.data(), bytes.size());

    // RFC4122 UUIDv4: set version and variant bits.
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    constexpr char kHex[] = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid.push_back('-');
        }
        uuid.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        uuid.push_back(kHex[bytes[i] & 0x0F]);
    }

    return uuid;
}

std::string generateVersion() {
    Botan::AutoSeeded_RNG rng;
    std::array<uint8_t, 8> bytes{};
    rng.randomize(bytes.data(), bytes.size());

    constexpr char kHex[] = "0123456789abcdef";
    std::string version;
    version.reserve(16);

    for (uint8_t b : bytes) {
        version.push_back(kHex[(b >> 4) & 0x0F]);
        version.push_back(kHex[b & 0x0F]);
    }

    return version;
}

// =========================
// Internal types
// =========================

// Placement map:
// {
//   "host:port" -> [
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

std::string make_shard_location(const std::string& object_id, const std::string& version, uint32_t shard_index) {
    return object_id + "-" + version + "-" + std::to_string(shard_index);
}

ErasureSpec make_erasure_spec() {
    ErasureSpec spec;
    spec.data_shards = kDataShards;
    spec.parity_shards = kParityShards;
    spec.shard_size = kShardSize;
    return spec;
}

std::pair<std::string, int> parse_address(const std::string& address) {
    const auto pos = address.rfind(':');
    if (pos == std::string::npos) {
        throw std::runtime_error("parse_address: invalid address: " + address);
    }

    return {address.substr(0, pos), std::stoi(address.substr(pos + 1))};
}

// MVP placement strategy:
// - one shard per node
// - shard i goes to node i
PlacementMap placement_strategy(
    const std::string& object_id,
    const std::string& version,
    const std::vector<Bytes>& serialized_shards
) {
    const std::size_t total_shards = serialized_shards.size();

    if (total_shards > kNodeAddresses.size()) {
        throw std::runtime_error(
            "placement_strategy: not enough nodes for number of shards"
        );
    }

    PlacementMap placement;

    for (std::size_t i = 0; i < total_shards; ++i) {
        const std::string& node_address = kNodeAddresses[i];
        const std::string location = make_shard_location(
            object_id,
            version,
            static_cast<uint32_t>(i)
        );

        placement[node_address].push_back({location, serialized_shards[i]});
    }

    return placement;
}

void apply_placement(const PlacementMap& placement) {
    std::vector<std::future<void>> futures;

    for (const auto& [node_address, entries] : placement) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &entries]() {
                auto [host, port] = parse_address(node_address);
                httplib::Client client(host, port);

                for (const auto& [location, payload] : entries) {
                    auto res = client.Put(
                        "/shards/" + location,
                        reinterpret_cast<const char*>(payload.data()),
                        payload.size(),
                        "application/octet-stream"
                    );

                    if (!res || res->status != 200) {
                        throw std::runtime_error(
                            "apply_placement: failed to PUT shard to " +
                            node_address + "/shards/" + location
                        );
                    }
                }
            }
        ));
    }

    for (auto& f : futures) {
        f.get();
    }
}

// Build metadata.shard_locations as an ordered vector of "host:port/location".
// The index in the vector equals the shard index.
std::vector<std::string> build_shard_locations(const PlacementMap& placement) {
    std::size_t total_shards = 0;
    for (const auto& [node_address, entries] : placement) {
        total_shards += entries.size();
    }

    std::vector<std::string> shard_locations(total_shards);

    for (const auto& [node_address, entries] : placement) {
        for (const auto& [location, payload] : entries) {
            const auto pos = location.rfind('-');
            if (pos == std::string::npos) {
                throw std::runtime_error("build_shard_locations: invalid location");
            }

            const std::size_t shard_index = std::stoul(location.substr(pos + 1));
            shard_locations.at(shard_index) = node_address + "/" + location;
        }
    }

    return shard_locations;
}

ShardLocationMap parse_shard_locations(const std::vector<std::string>& shard_locations) {
    ShardLocationMap result;

    for (const auto& shard_location : shard_locations) {
        const auto pos = shard_location.find('/');
        if (pos == std::string::npos) {
            throw std::runtime_error("parse_shard_locations: invalid shard location");
        }

        const std::string node_address = shard_location.substr(0, pos);
        const std::string location = shard_location.substr(pos + 1);

        result[node_address].push_back(location);
    }

    return result;
}

std::vector<EncryptedShard> fetch_encrypted_shards(
    const ShardLocationMap& shard_location_map
) {
    std::vector<std::future<std::vector<EncryptedShard>>> futures;

    for (const auto& [node_address, locations] : shard_location_map) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &locations]() -> std::vector<EncryptedShard> {
                std::vector<EncryptedShard> shards;

                std::pair<std::string, int> addr;
                try {
                    addr = parse_address(node_address);
                } catch (...) {
                    return shards;
                }

                httplib::Client client(addr.first, addr.second);

                for (const auto& location : locations) {
                    try {
                        auto res = client.Get("/shards/" + location);

                        if (!res || res->status != 200) {
                            continue; // missing shard
                        }

                        Bytes payload(res->body.begin(), res->body.end());

                        try {
                            shards.push_back(
                                EncryptedShard::deserialize(payload)
                            );
                        } catch (...) {
                            // malformed/corrupt serialized shard -> ignore
                            continue;
                        }
                    } catch (...) {
                        // node unavailable
                        continue;
                    }
                }

                return shards;
            }
        ));
    }

    std::vector<EncryptedShard> encrypted_shards;
    for (auto& f : futures) {
        auto shards = f.get();
        encrypted_shards.insert(
            encrypted_shards.end(),
            std::make_move_iterator(shards.begin()),
            std::make_move_iterator(shards.end())
        );
    }

    return encrypted_shards;
}

void remove_from_nodes(const ShardLocationMap& shard_location_map) {
    std::vector<std::future<void>> futures;

    for (const auto& [node_address, locations] : shard_location_map) {
        futures.push_back(std::async(std::launch::async,
            [&node_address, &locations]() {
                std::pair<std::string, int> addr;
                try {
                    addr = parse_address(node_address);
                } catch (...) {
                    return;
                }

                httplib::Client client(addr.first, addr.second);

                for (const auto& location : locations) {
                    try {
                        (void)client.Delete("/shards/" + location);
                    } catch (...) {
                        // Ignore missing node / already-deleted shard / backend failure
                        continue;
                    }
                }
            }
        ));
    }

    for (auto& f : futures) {
        try {
            f.get();
        } catch (...) {
            // best-effort deletion
        }
    }
}

} // namespace

// =========================
// StorageClient
// =========================

StorageClient::StorageClient(const std::string& metadata_conn_str)
    : metadata_store_(metadata_conn_str),
      kek_file_path_(parseKekFilePath()) {
    if (std::filesystem::exists(kek_file_path_)) {
        auto data = load_kek_file(kek_file_path_);
        kek_ring_ = std::move(data.keys);
        active_kek_id_ = std::move(data.active_kek_id);
    } else {
        Botan::AutoSeeded_RNG rng;
        auto key = Botan::create_private_key("ML-KEM", rng, "ML-KEM-768");
        if (!key) {
            throw std::runtime_error("StorageClient: failed to create ML-KEM-768 key");
        }

        do {
            active_kek_id_ = generateKekId();
        } while (kek_ring_.count(active_kek_id_) != 0);

        const auto inserted = kek_ring_.emplace(active_kek_id_, std::move(key));
        if (!inserted.second) {
            throw std::runtime_error("StorageClient: failed to insert initial KEK");
        }

        save_kek_file(kek_file_path_, kek_ring_, active_kek_id_);
    }
}

StorageClient::~StorageClient() = default;

void StorageClient::init() {
    for (const auto& node_address : kNodeAddresses) {
        auto [host, port] = parse_address(node_address);
        httplib::Client client(host, port);

        auto res = client.Get("/health");

        if (!res || res->status != 200) {
            throw std::runtime_error(
                "StorageClient::init: node not healthy: " + node_address
            );
        }
    }
}

void StorageClient::put(const std::string& object_id, const Bytes& bytes) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::put: object_id cannot be empty");
    }

    if (bytes.empty()) {
        throw std::invalid_argument("StorageClient::put: bytes cannot be empty");
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

    // 4) Generate per-object DEK, encrypt shards, wrap DEK with KEK
    Botan::AutoSeeded_RNG rng;
    std::array<uint8_t, 32> dek;
    rng.randomize(dek.data(), dek.size());

    std::vector<EncryptedShard> encrypted_shards =
        encrypt_shards(plain_shards, dek);

    if (encrypted_shards.size() != plain_shards.size()) {
        throw std::runtime_error(
            "StorageClient::put: encrypt_shards() returned unexpected number of shards"
        );
    }

    Bytes wrapped_dek = encrypt_dek(dek, *kek_ring_.at(active_kek_id_)->public_key());
    Botan::secure_scrub_memory(dek.data(), dek.size());

    std::vector<Bytes> serialized_shards;
    serialized_shards.reserve(encrypted_shards.size());

    for (const auto& encrypted_shard : encrypted_shards) {
        serialized_shards.push_back(encrypted_shard.serialize());
    }

    // 5) placement_strategy(serialized_shards)
    const std::string version = generateVersion();
    PlacementMap placement = placement_strategy(object_id, version, serialized_shards);

    // 6) apply_placement(map) over HTTP
    apply_placement(placement);

    // 7) Generate metadata and store using MetadataStore
    ObjectMetadata metadata;
    metadata.id = object_id;
    metadata.size = bytes.size();
    metadata.checksum = checksum_hex;
    metadata.erasure = erasure_spec;
    metadata.shard_locations = build_shard_locations(placement);
    metadata.encrypted_dek = wrapped_dek;
    metadata.kek_id = active_kek_id_;

    metadata_store_.put(metadata);
}

Bytes StorageClient::get(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::get: object_id cannot be empty");
    }

    // 1) Load metadata
    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        throw std::runtime_error("StorageClient::get: object not found: " + object_id);
    }

    const ObjectMetadata& metadata = *metadata_opt;

    // 2) Parse shard locations into:
    //    { "host:port" -> vector<"object_id-shard_id"> }
    const ShardLocationMap shard_location_map =
        parse_shard_locations(metadata.shard_locations);

    // 3) Fetch encrypted shard payloads from nodes over HTTP
    std::vector<EncryptedShard> encrypted_shards =
        fetch_encrypted_shards(shard_location_map);

    if (encrypted_shards.size() < metadata.erasure.data_shards) {
        throw std::runtime_error(
            "StorageClient::get: not enough shards to reconstruct object"
        );
    }

    // 4) Decrypt DEK — try matching kek_id first, then fallback to all
    std::array<uint8_t, 32> dek{};
    bool dek_recovered = false;

    auto try_decrypt = [&](const Botan::Private_Key& key) -> bool {
        try {
            dek = decrypt_dek(metadata.encrypted_dek, key);
            return true;
        } catch (...) {
            return false;
        }
    };

    // Try the key indicated by metadata first
    auto it = kek_ring_.find(metadata.kek_id);
    if (it != kek_ring_.end()) {
        dek_recovered = try_decrypt(*it->second);
    }

    // Fallback: try remaining keys
    if (!dek_recovered) {
        for (const auto& [id, key] : kek_ring_) {
            if (id == metadata.kek_id) continue;
            if (try_decrypt(*key)) {
                dek_recovered = true;
                break;
            }
        }
    }

    if (!dek_recovered) {
        throw std::runtime_error(
            "StorageClient::get: cannot decrypt DEK with any available KEK"
        );
    }

    const std::vector<PlainShard> plain_shards =
        decrypt_shards(encrypted_shards, dek);

    Botan::secure_scrub_memory(dek.data(), dek.size());

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

    // 1) Load metadata
    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        return false;
    }

    const ObjectMetadata& metadata = *metadata_opt;

    // 2) Parse metadata shard locations into:
    //    { "host:port" -> vector<"object_id-shard_id"> }
    const ShardLocationMap shard_location_map =
        parse_shard_locations(metadata.shard_locations);

    // 3) Remove shard payloads from storage nodes over HTTP
    remove_from_nodes(shard_location_map);

    // 4) Remove metadata
    return metadata_store_.remove(object_id);
}

std::vector<ObjectMetadata> StorageClient::list() {
    return metadata_store_.list();
}

void StorageClient::rotate() {
    Botan::AutoSeeded_RNG rng;
    auto key = Botan::create_private_key("ML-KEM", rng, "ML-KEM-768");
    if (!key) {
        throw std::runtime_error("StorageClient::rotate: failed to create ML-KEM-768 key");
    }

    do {
        active_kek_id_ = generateKekId();
    } while (kek_ring_.count(active_kek_id_) != 0);

    const auto inserted = kek_ring_.emplace(active_kek_id_, std::move(key));
    if (!inserted.second) {
        throw std::runtime_error("StorageClient::rotate: generated duplicate KEK ID");
    }

    save_kek_file(kek_file_path_, kek_ring_, active_kek_id_);
}