#include "StorageClient.hpp"

#include "Crypto.hpp"
#include "ErasureCodec.hpp"
#include "ShardTransport.hpp"

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
#include <vector>

namespace {

// =========================
// Constants
// =========================

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
    for (const auto& node_address : node_addresses()) {
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

void StorageClient::put(const std::string& object_id, const Bytes& bytes, const ErasureSpec& erasure_spec) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::put: object_id cannot be empty");
    }

    if (bytes.empty()) {
        throw std::invalid_argument("StorageClient::put: bytes cannot be empty");
    }

    // 1) Generate checksum from bytes
    const Bytes checksum_bytes = checksum_sha256(bytes);
    const std::string checksum_hex = bytes_to_hex(checksum_bytes);

    // 2) Validate erasure spec against node count
    if (erasure_spec.data_shards + erasure_spec.parity_shards > kNumNodes) {
        throw std::invalid_argument(
            "StorageClient::put: data_shards + parity_shards exceeds available nodes"
        );
    }

    // 3) Generate plain shards from bytes
    Bytes mutable_input = bytes; // encode() takes non-const Bytes&
    ErasureSpec mutable_spec = erasure_spec; // encode() takes non-const ErasureSpec&
    std::vector<PlainShard> plain_shards = encode(mutable_input, mutable_spec);

    const std::size_t expected_total_shards =
        static_cast<std::size_t>(erasure_spec.data_shards + erasure_spec.parity_shards);

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

    Bytes wrapped_dek;
    {
        std::shared_lock<std::shared_mutex> kek_lock(kek_mu_);
        wrapped_dek = encrypt_dek(dek, *kek_ring_.at(active_kek_id_)->public_key());
    }
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

    {
        std::shared_lock<std::shared_mutex> kek_lock(kek_mu_);

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
    std::unique_lock<std::shared_mutex> kek_lock(kek_mu_);

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