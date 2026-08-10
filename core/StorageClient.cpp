#include "StorageClient.hpp"

#include "Crypto.hpp"
#include "ErasureCodec.hpp"
#include "ShardTransport.hpp"

#include <botan/auto_rng.h>
#include <botan/mem_ops.h>
#include <botan/pk_algs.h>
#include <httplib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// =========================
// Constants
// =========================

const std::string kDefaultKekFilePath = "/tmp/myc_kek.json";

// Sentinel node address recorded for shards that were missing at rebuild time
// (ADR-0008). It is structurally valid ("host:port") so it round-trips through
// metadata and parsing, but is unreachable, so health/repair treat it as a
// missing shard and repair relocates it to a live node.
const std::string kUnknownNode = "0.0.0.0:0";

std::string parseKekFilePath() {
    const char* env = std::getenv("MYC_KEYSTORE_PATH");
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

std::uint64_t now_unix_nanos() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
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

bool is_node_healthy(const std::string& node_address) {
    try {
        auto& client = get_node_client(node_address);

        auto res = client.Get("/health");
        return res && res->status == 200;
    } catch (...) {
        return false;
    }
}

std::array<uint8_t, 32> recover_dek(
    const Bytes& encrypted_dek,
    const std::string& kek_id,
    const std::unordered_map<
        std::string,
        std::unique_ptr<Botan::Private_Key>
    >& kek_ring,
    std::shared_mutex& kek_mu
) {
    std::array<uint8_t, 32> dek{};

    std::shared_lock<std::shared_mutex> kek_lock(kek_mu);

    auto try_decrypt = [&](const Botan::Private_Key& key) -> bool {
        try {
            dek = decrypt_dek(encrypted_dek, key);
            return true;
        } catch (...) {
            return false;
        }
    };

    auto it = kek_ring.find(kek_id);
    if (it != kek_ring.end()) {
        if (try_decrypt(*it->second)) {
            return dek;
        }
    }

    for (const auto& [id, key] : kek_ring) {
        if (id == kek_id) {
            continue;
        }

        if (try_decrypt(*key)) {
            return dek;
        }
    }

    throw std::runtime_error(
        "recover_dek: cannot decrypt DEK with any available KEK"
    );
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
    for (const auto& node_address : metadata_store_.list_nodes()) {
        auto& client = get_node_client(node_address);

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

    // Validate erasure parameters
    if (erasure_spec.data_shards == 0) {
        throw std::invalid_argument("StorageClient::put: k (data_shards) must be > 0");
    }

    if (erasure_spec.parity_shards == 0) {
        throw std::invalid_argument("StorageClient::put: m (parity_shards) must be > 0");
    }

    if (erasure_spec.data_shards + erasure_spec.parity_shards > 255) {
        throw std::invalid_argument("StorageClient::put: k + m must be <= 255");
    }

    if (erasure_spec.shard_size == 0) {
        throw std::invalid_argument("StorageClient::put: shard_size must be > 0");
    }

    if (bytes.size() > static_cast<std::size_t>(erasure_spec.data_shards) * erasure_spec.shard_size) {
        throw std::invalid_argument("StorageClient::put: bytes.size() exceeds k * shard_size");
    }

    // Validate eligible node count
    const std::vector<std::string> eligible_nodes = metadata_store_.list_nodes();
    const std::size_t required_nodes =
        static_cast<std::size_t>(erasure_spec.data_shards) + erasure_spec.parity_shards;

    if (eligible_nodes.size() < required_nodes) {
        throw std::invalid_argument(
            "StorageClient::put: eligible nodes (" + std::to_string(eligible_nodes.size()) +
            ") < k + m (" + std::to_string(required_nodes) + ")"
        );
    }

    // 1) Generate checksum from bytes
    const Bytes checksum_bytes = checksum_sha256(bytes);
    const std::string checksum_hex = bytes_to_hex(checksum_bytes);

    // Metrics: phase timers
    double crypto_ms = 0.0;
    double transport_ms = 0.0;
    double metadata_ms = 0.0;
    auto put_start = std::chrono::steady_clock::now();

    // 3) Generate plain shards from bytes
    std::vector<PlainShard> plain_shards;
    std::vector<EncryptedShard> encrypted_shards;
    Bytes wrapped_dek;
    std::vector<Bytes> serialized_shards;

    // Version token for this write; embedded in shard keys and in each shard's
    // self-describing manifest (ADR-0008) so a rebuild can group and disambiguate.
    const std::string version = generateVersion();
    std::string used_kek_id;

    {
        ScopedTimer crypto_timer(&crypto_ms);

        plain_shards = encode(bytes, erasure_spec);

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

        encrypted_shards = encrypt_shards(plain_shards, dek);

        if (encrypted_shards.size() != plain_shards.size()) {
            throw std::runtime_error(
                "StorageClient::put: encrypt_shards() returned unexpected number of shards"
            );
        }

        {
            std::shared_lock<std::shared_mutex> kek_lock(kek_mu_);
            used_kek_id = active_kek_id_;
            wrapped_dek = encrypt_dek(dek, *kek_ring_.at(used_kek_id)->public_key());
        }
        Botan::secure_scrub_memory(dek.data(), dek.size());

        // Build the per-object manifest replicated onto every shard so the
        // metadata catalog can be rebuilt from the nodes alone (ADR-0008).
        ShardManifest manifest;
        manifest.object_id = object_id;
        manifest.version = version;
        manifest.size = bytes.size();
        manifest.checksum = checksum_hex;
        manifest.erasure = erasure_spec;
        manifest.encrypted_dek = wrapped_dek;
        manifest.kek_id = used_kek_id;
        manifest.written_at = now_unix_nanos();

        serialized_shards.reserve(encrypted_shards.size());
        for (const auto& encrypted_shard : encrypted_shards) {
            StoredShard stored;
            stored.manifest = manifest;
            stored.shard = encrypted_shard;
            serialized_shards.push_back(stored.serialize());
        }
    }

    // 5) placement_strategy(serialized_shards)
    // Shards are placed on their HRW (rendezvous) intended nodes so placement is
    // deterministic and membership-stable; placement_strategy assigns shard i to
    // intended_nodes[i].
    const std::vector<std::string> intended_nodes =
        hrw_intended_nodes(object_id, eligible_nodes, serialized_shards.size());

    PlacementMap placement = placement_strategy(object_id, version, serialized_shards, intended_nodes);

    // 6) apply_placement(map) over HTTP
    {
        ScopedTimer transport_timer(&transport_ms);
        apply_placement(placement);
    }

    // 7) Generate metadata and store using MetadataStore
    ObjectMetadata metadata;
    metadata.id = object_id;
    metadata.size = bytes.size();
    metadata.checksum = checksum_hex;
    metadata.erasure = erasure_spec;
    metadata.shard_locations = build_shard_locations(placement);
    metadata.encrypted_dek = wrapped_dek;
    metadata.kek_id = used_kek_id;

    {
        ScopedTimer metadata_timer(&metadata_ms);
        metadata_store_.put(metadata);
    }

    // Record metrics
    if (collector_) {
        double total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - put_start).count();

        std::size_t total_stored = 0;
        for (const auto& s : serialized_shards) {
            total_stored += s.size();
        }

        OperationRecord rec;
        rec.operation = "PUT";
        rec.params.k = erasure_spec.data_shards;
        rec.params.m = erasure_spec.parity_shards;
        rec.params.shard_size = erasure_spec.shard_size;
        rec.params.object_size = bytes.size();
        rec.params.total_nodes = static_cast<uint32_t>(eligible_nodes.size());
        rec.timing.crypto_ms = crypto_ms;
        rec.timing.transport_ms = transport_ms;
        rec.timing.metadata_ms = metadata_ms;
        rec.timing.total_ms = total_ms;
        rec.success = true;
        rec.storage_overhead = static_cast<double>(total_stored) / static_cast<double>(bytes.size());
        collector_->record(std::move(rec));
    }
}

Bytes StorageClient::get(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::get: object_id cannot be empty");
    }

    // Metrics: phase timers
    double crypto_ms = 0.0;
    double transport_ms = 0.0;
    double metadata_ms = 0.0;
    auto get_start = std::chrono::steady_clock::now();

    // 1) Load metadata
    std::optional<ObjectMetadata> metadata_opt;
    {
        ScopedTimer metadata_timer(&metadata_ms);
        metadata_opt = metadata_store_.get(object_id);
    }
    if (!metadata_opt.has_value()) {
        throw std::runtime_error("StorageClient::get: object not found: " + object_id);
    }

    const ObjectMetadata& metadata = *metadata_opt;

    // 2) Parse shard locations into:
    //    { "host:port" -> vector<"object_id/version/shard_index"> }
    const ShardLocationMap shard_location_map =
        parse_shard_locations(metadata.shard_locations);

    // 3) Fetch encrypted shard payloads from nodes over HTTP
    std::vector<EncryptedShard> encrypted_shards;
    {
        ScopedTimer transport_timer(&transport_ms);
        encrypted_shards = fetch_encrypted_shards(shard_location_map);
    }

    if (encrypted_shards.size() < metadata.erasure.data_shards) {
        throw std::runtime_error(
            "StorageClient::get: not enough shards to reconstruct object"
        );
    }

    // 4) Recover DEK, decrypt, decode
    std::vector<PlainShard> plain_shards;
    Bytes decoded;
    {
        ScopedTimer crypto_timer(&crypto_ms);

        std::array<uint8_t, 32> dek = recover_dek(
            metadata.encrypted_dek,
            metadata.kek_id,
            kek_ring_,
            kek_mu_
        );

        plain_shards = decrypt_shards(encrypted_shards, dek);
        Botan::secure_scrub_memory(dek.data(), dek.size());

        // 5) Decode original bytes
        if (plain_shards.size() < metadata.erasure.data_shards) {
            throw std::runtime_error(
                "StorageClient::get: not enough valid shards to reconstruct object"
            );
        }

        decoded = decode(plain_shards, metadata.erasure, metadata.size);
    }

    // 6) Verify checksum
    const std::string checksum_hex = bytes_to_hex(checksum_sha256(decoded));
    if (checksum_hex != metadata.checksum) {
        throw std::runtime_error("StorageClient::get: checksum mismatch");
    }

    // Record metrics
    if (collector_) {
        double total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - get_start).count();

        OperationRecord rec;
        rec.operation = "GET";
        rec.params.k = metadata.erasure.data_shards;
        rec.params.m = metadata.erasure.parity_shards;
        rec.params.shard_size = metadata.erasure.shard_size;
        rec.params.object_size = metadata.size;
        rec.params.total_nodes = static_cast<uint32_t>(metadata.shard_locations.size());
        rec.timing.crypto_ms = crypto_ms;
        rec.timing.transport_ms = transport_ms;
        rec.timing.metadata_ms = metadata_ms;
        rec.timing.total_ms = total_ms;
        rec.success = true;
        collector_->record(std::move(rec));
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
    //    { "host:port" -> vector<"object_id/version/shard_index"> }
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

ObjectHealth StorageClient::health(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::health: object_id cannot be empty");
    }

    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        throw std::runtime_error("StorageClient::health: object not found: " + object_id);
    }

    const ObjectMetadata& metadata = *metadata_opt;

    ObjectHealth result;
    result.object_id = object_id;
    result.total_shards = metadata.erasure.data_shards + metadata.erasure.parity_shards;
    result.required_shards = metadata.erasure.data_shards;
    result.available_shards = 0;

    for (std::size_t i = 0; i < metadata.shard_locations.size(); ++i) {
        const auto& shard_location = metadata.shard_locations[i];
        const auto pos = shard_location.find('/');
        if (pos == std::string::npos) {
            result.missing_indices.push_back(static_cast<uint32_t>(i));
            continue;
        }

        const std::string node_address = shard_location.substr(0, pos);
        const std::string location = shard_location.substr(pos + 1);

        if (probe_shard(node_address, location)) {
            ++result.available_shards;
        } else {
            result.missing_indices.push_back(static_cast<uint32_t>(i));
        }
    }

    result.healthy = result.available_shards >= result.required_shards;
    result.fully_replicated = result.available_shards == result.total_shards;

    return result;
}

bool StorageClient::repair(const std::string& object_id) {
    if (object_id.empty()) {
        throw std::invalid_argument("StorageClient::repair: object_id cannot be empty");
    }

    // Metrics: phase timers
    double crypto_ms = 0.0;
    double transport_fetch_ms = 0.0;
    double transport_apply_ms = 0.0;
    double metadata_get_ms = 0.0;
    double metadata_put_ms = 0.0;
    auto repair_start = std::chrono::steady_clock::now();

    // 1) Load metadata and record version
    std::optional<ObjectMetadata> metadata_opt;
    {
        ScopedTimer metadata_timer(&metadata_get_ms);
        metadata_opt = metadata_store_.get(object_id);
    }
    if (!metadata_opt.has_value()) {
        throw std::runtime_error("StorageClient::repair: object not found: " + object_id);
    }

    const ObjectMetadata& metadata = *metadata_opt;
    const int expected_version = metadata.version;

    // Metrics: number of shards needing repair (set after probing). A REPAIR
    // record is emitted on both success and failure so that recovery success
    // rate is derivable from REPAIR records (ADR-0007). Non-exception early
    // returns (nothing to repair, concurrent mutation) are not recorded.
    uint32_t failed_count = 0;

    auto record_repair = [&](bool success) {
        if (!collector_) {
            return;
        }

        double total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - repair_start).count();

        OperationRecord rec;
        rec.operation = "REPAIR";
        rec.params.k = metadata.erasure.data_shards;
        rec.params.m = metadata.erasure.parity_shards;
        rec.params.shard_size = metadata.erasure.shard_size;
        rec.params.object_size = metadata.size;
        rec.params.failed_nodes = failed_count;
        rec.params.total_nodes = static_cast<uint32_t>(metadata.shard_locations.size());
        rec.timing.crypto_ms = crypto_ms;
        rec.timing.transport_ms = transport_fetch_ms + transport_apply_ms;
        rec.timing.metadata_ms = metadata_get_ms + metadata_put_ms;
        rec.timing.total_ms = total_ms;
        rec.success = success;
        collector_->record(std::move(rec));
    };

    try {
        // 2) Probe shards to identify missing/surviving
        std::vector<uint32_t> missing_indices;
        std::vector<uint32_t> surviving_indices;

        for (std::size_t i = 0; i < metadata.shard_locations.size(); ++i) {
            const auto& shard_location = metadata.shard_locations[i];
            const auto pos = shard_location.find('/');
            if (pos == std::string::npos) {
                missing_indices.push_back(static_cast<uint32_t>(i));
                continue;
            }

            const std::string node_address = shard_location.substr(0, pos);
            const std::string location = shard_location.substr(pos + 1);

            if (probe_shard(node_address, location)) {
                surviving_indices.push_back(static_cast<uint32_t>(i));
            } else {
                missing_indices.push_back(static_cast<uint32_t>(i));
            }
        }

        failed_count = static_cast<uint32_t>(missing_indices.size());

        // 3) Nothing to repair
        if (missing_indices.empty()) {
            return true;
        }

        // 4) Not enough surviving shards
        if (surviving_indices.size() < metadata.erasure.data_shards) {
            throw std::runtime_error(
                "StorageClient::repair: not enough surviving shards to reconstruct"
            );
        }

        // 5) Recover DEK
        std::array<uint8_t, 32> dek = recover_dek(
            metadata.encrypted_dek,
            metadata.kek_id,
            kek_ring_,
            kek_mu_
        );

        // 6) Fetch surviving encrypted shards
        ShardLocationMap surviving_location_map;
        for (uint32_t idx : surviving_indices) {
            const auto& shard_location = metadata.shard_locations[idx];
            const auto pos = shard_location.find('/');
            const std::string node_address = shard_location.substr(0, pos);
            const std::string location = shard_location.substr(pos + 1);
            surviving_location_map[node_address].push_back(location);
        }

        std::vector<EncryptedShard> encrypted_shards;
        {
            ScopedTimer transport_timer(&transport_fetch_ms);
            encrypted_shards = fetch_encrypted_shards(surviving_location_map);
        }

        if (encrypted_shards.size() < metadata.erasure.data_shards) {
            throw std::runtime_error(
                "StorageClient::repair: failed to fetch enough surviving shards"
            );
        }

        // 7) Decrypt surviving shards, reconstruct, re-encrypt missing
        std::vector<EncryptedShard> repaired_encrypted;
        {
            ScopedTimer crypto_timer(&crypto_ms);

            std::vector<PlainShard> plain_shards = decrypt_shards(encrypted_shards, dek);

            if (plain_shards.size() < metadata.erasure.data_shards) {
                throw std::runtime_error(
                    "StorageClient::repair: not enough valid decrypted shards"
                );
            }

            // 8) Reconstruct all shards
            Bytes decoded = decode(plain_shards, metadata.erasure, metadata.size);

            const std::string checksum_hex =
                bytes_to_hex(checksum_sha256(decoded));

            if (checksum_hex != metadata.checksum) {
                throw std::runtime_error(
                    "StorageClient::repair: checksum mismatch after reconstruction"
                );
            }

            std::vector<PlainShard> all_plain_shards = encode(decoded, metadata.erasure);

            // 9) Re-encrypt only the missing shards
            std::vector<PlainShard> missing_plain_shards;
            for (uint32_t idx : missing_indices) {
                for (const auto& shard : all_plain_shards) {
                    if (shard.index == idx) {
                        missing_plain_shards.push_back(shard);
                        break;
                    }
                }
            }

            repaired_encrypted = encrypt_shards(missing_plain_shards, dek);
        }

        Botan::secure_scrub_memory(dek.data(), dek.size());

        // 10) Place repaired shards on eligible nodes
        const std::vector<std::string> eligible_nodes = metadata_store_.list_nodes();
        std::vector<std::string> healthy_nodes;
        std::unordered_set<std::string> healthy_set;
        healthy_nodes.reserve(eligible_nodes.size());

        for (const auto& node : eligible_nodes) {
            if (is_node_healthy(node)) {
                healthy_nodes.push_back(node);
                healthy_set.insert(node);
            }
        }

        const std::size_t total_shards =
            static_cast<std::size_t>(metadata.erasure.data_shards) +
            metadata.erasure.parity_shards;

        // HRW-intended placement over all registered nodes (deterministic and
        // membership-stable). Repaired shards prefer their intended node so
        // repair heals toward the same layout put() and rebalance() target.
        // Only computable when at least k+m nodes are registered.
        std::vector<std::string> intended;
        if (eligible_nodes.size() >= total_shards) {
            intended = hrw_intended_nodes(object_id, eligible_nodes, total_shards);
        }

        // Build set of nodes already used by surviving shards
        std::unordered_set<std::string> used_nodes;
        for (uint32_t idx : surviving_indices) {
            const auto& shard_location = metadata.shard_locations[idx];
            const auto pos = shard_location.find('/');
            if (pos != std::string::npos) {
                used_nodes.insert(shard_location.substr(0, pos));
            }
        }

        // Fallback candidate pool: healthy nodes not holding a surviving shard
        // first, then healthy nodes that do (last resort, may co-locate two
        // shards of the same object) so repair never fails when enough healthy
        // nodes exist - preserving prior repair behaviour.
        std::vector<std::string> fallback_pool;
        for (const auto& node : healthy_nodes) {
            if (used_nodes.count(node) == 0) {
                fallback_pool.push_back(node);
            }
        }
        for (const auto& node : healthy_nodes) {
            if (used_nodes.count(node) > 0) {
                fallback_pool.push_back(node);
            }
        }

        // Assign one distinct target per missing shard: prefer the HRW-intended
        // node when it is healthy and still free, otherwise draw from the pool.
        std::vector<std::string> available_for_repair;
        available_for_repair.reserve(missing_indices.size());
        std::unordered_set<std::string> consumed;
        std::size_t pool_cursor = 0;

        for (std::size_t i = 0; i < missing_indices.size(); ++i) {
            const uint32_t idx = missing_indices[i];
            std::string target;

            if (!intended.empty()) {
                const std::string& want = intended[idx];
                if (healthy_set.count(want) > 0 && consumed.count(want) == 0) {
                    target = want;
                }
            }

            if (target.empty()) {
                while (pool_cursor < fallback_pool.size() &&
                       consumed.count(fallback_pool[pool_cursor]) > 0) {
                    ++pool_cursor;
                }
                if (pool_cursor < fallback_pool.size()) {
                    target = fallback_pool[pool_cursor];
                    ++pool_cursor;
                }
            }

            if (target.empty()) {
                throw std::runtime_error(
                    "StorageClient::repair: not enough healthy eligible nodes for repaired shards"
                );
            }

            consumed.insert(target);
            available_for_repair.push_back(target);
        }

        // Extract version from existing shard keys.
        std::string version;
        for (const auto& loc : metadata.shard_locations) {
            const auto slash_pos = loc.find('/');
            if (slash_pos != std::string::npos) {
                const std::string key = loc.substr(slash_pos + 1);
                try {
                    const ParsedShardKey parsed = parse_shard_key(key);
                    version = parsed.version;
                    break;
                } catch (...) {
                    continue;
                }
            }
        }

        if (version.empty()) {
            version = generateVersion();
        }

        // Write repaired shards
        std::vector<Bytes> serialized_repaired;
        serialized_repaired.reserve(repaired_encrypted.size());

        // Repaired shards stay self-describing: rebuild the manifest from the
        // loaded metadata so a future rebuild can recover this object even from
        // the replacement shards alone (ADR-0008).
        ShardManifest repaired_manifest;
        repaired_manifest.object_id = object_id;
        repaired_manifest.version = version;
        repaired_manifest.size = metadata.size;
        repaired_manifest.checksum = metadata.checksum;
        repaired_manifest.erasure = metadata.erasure;
        repaired_manifest.encrypted_dek = metadata.encrypted_dek;
        repaired_manifest.kek_id = metadata.kek_id;
        repaired_manifest.written_at = now_unix_nanos();

        for (const auto& es : repaired_encrypted) {
            StoredShard stored;
            stored.manifest = repaired_manifest;
            stored.shard = es;
            serialized_repaired.push_back(stored.serialize());
        }

        PlacementMap repair_placement;
        for (std::size_t i = 0; i < missing_indices.size(); ++i) {
            const std::string& node_address = available_for_repair[i];
            const std::string location = make_shard_key(
                object_id, version, missing_indices[i]);
            repair_placement[node_address].push_back({location, serialized_repaired[i]});
        }

        // 11) Write repaired shard payloads first
        {
            ScopedTimer transport_timer(&transport_apply_ms);
            apply_placement(repair_placement);
        }

        // 12) Build updated shard_locations
        std::vector<std::string> updated_shard_locations = metadata.shard_locations;
        for (std::size_t i = 0; i < missing_indices.size(); ++i) {
            uint32_t idx = missing_indices[i];
            const std::string& node_address = available_for_repair[i];
            const std::string location = make_shard_key(object_id, version, idx);
            updated_shard_locations[idx] = node_address + "/" + location;
        }

        // 13) Conditional metadata commit
        ObjectMetadata updated_metadata = metadata;
        updated_metadata.shard_locations = updated_shard_locations;

        bool committed = false;
        {
            ScopedTimer metadata_timer(&metadata_put_ms);
            committed = metadata_store_.conditional_put(updated_metadata, expected_version);
        }
        if (!committed) {
            return false; // version changed - concurrent mutation
        }

        // Record metrics
        record_repair(true);

        return true;
    } catch (...) {
        // Record the failed recovery attempt, then propagate.
        record_repair(false);
        throw;
    }
}

ReindexReport StorageClient::reindex() {
    ReindexReport report;

    const std::vector<std::string> nodes = metadata_store_.list_nodes();

    // 1) Scan every node's keyspace in parallel. Keys alone reconstruct shard
    //    placement, so no payloads are read in this phase.
    std::vector<std::future<std::pair<std::string, std::vector<std::string>>>> scan_futures;
    scan_futures.reserve(nodes.size());
    for (const auto& node : nodes) {
        scan_futures.push_back(std::async(std::launch::async,
            [node]() {
                return std::make_pair(node, list_node_shards(node));
            }
        ));
    }

    // Group discovered shards by (object_id, version). Each group tracks the
    // full location per shard index plus a representative shard to fetch the
    // manifest from.
    struct VersionGroup {
        std::string object_id;
        std::string version;
        std::map<uint32_t, std::string> shard_locations; // index -> "node/key"
        std::string rep_node;
        std::string rep_key;
    };

    std::unordered_map<std::string, VersionGroup> groups;

    for (auto& future : scan_futures) {
        auto [node, keys] = future.get();
        for (const auto& key : keys) {
            ParsedShardKey parsed;
            try {
                parsed = parse_shard_key(key);
            } catch (...) {
                continue; // ignore keys that are not object shards
            }

            const std::string gid = parsed.object_id + "/" + parsed.version;
            VersionGroup& group = groups[gid];
            if (group.object_id.empty()) {
                group.object_id = parsed.object_id;
                group.version = parsed.version;
                group.rep_node = node;
                group.rep_key = key;
            }
            group.shard_locations[parsed.shard_index] = node + "/" + key;
        }
    }

    report.versions_scanned = groups.size();

    // 2) Fetch exactly one manifest per version group in parallel (all shards of
    //    a group carry the same manifest).
    std::vector<std::string> gids;
    gids.reserve(groups.size());
    for (const auto& [gid, group] : groups) {
        gids.push_back(gid);
    }

    std::vector<std::future<std::optional<ShardManifest>>> manifest_futures;
    manifest_futures.reserve(gids.size());
    for (const auto& gid : gids) {
        const VersionGroup& group = groups[gid];
        const std::string node = group.rep_node;
        const std::string key = group.rep_key;
        manifest_futures.push_back(std::async(std::launch::async,
            [node, key]() {
                return fetch_shard_manifest(node, key);
            }
        ));
    }

    // 3) Choose one winning version per object: prefer the newest reconstructable
    //    version (>= k surviving shards). If none is reconstructable, fall back to
    //    the version with the most surviving shards, newest breaking ties.
    struct Candidate {
        ShardManifest manifest;
        const VersionGroup* group = nullptr;
        std::size_t shard_count = 0;
    };
    std::unordered_map<std::string, Candidate> winners;

    for (std::size_t i = 0; i < gids.size(); ++i) {
        std::optional<ShardManifest> manifest = manifest_futures[i].get();
        if (!manifest.has_value()) {
            ++report.unreadable_versions;
            continue;
        }

        // Guard against corrupt or hostile manifests (a compromised node could
        // return a crafted payload). Reject anything outside the system's own
        // erasure bounds before it is used to size allocations.
        const uint32_t k = manifest->erasure.data_shards;
        const uint32_t m = manifest->erasure.parity_shards;
        if (k == 0 || m == 0 ||
            static_cast<std::size_t>(k) + m > 255 ||
            manifest->erasure.shard_size == 0) {
            ++report.unreadable_versions;
            continue;
        }

        const VersionGroup& group = groups[gids[i]];
        const std::size_t count = group.shard_locations.size();

        auto it = winners.find(group.object_id);
        if (it == winners.end()) {
            winners.emplace(group.object_id, Candidate{*manifest, &group, count});
            continue;
        }

        // Selection policy: prefer the newest version that is still
        // reconstructable (>= k surviving shards) so a rebuild restores the most
        // recent valid data rather than resurrecting a stale overwritten
        // version. Only when no version reaches k do we fall back to the one
        // with the most surviving shards (best effort), newest breaking ties.
        const Candidate& current = it->second;
        const bool new_recoverable = count >= manifest->erasure.data_shards;
        const bool cur_recoverable =
            current.shard_count >= current.manifest.erasure.data_shards;

        bool better;
        if (new_recoverable != cur_recoverable) {
            better = new_recoverable;
        } else if (new_recoverable) {
            better = manifest->written_at > current.manifest.written_at;
        } else {
            better =
                count > current.shard_count ||
                (count == current.shard_count &&
                 manifest->written_at > current.manifest.written_at);
        }

        if (better) {
            it->second = Candidate{*manifest, &group, count};
        }
    }

    // 4) Rebuild each object's metadata and insert it without clobbering any
    //    existing (live or newer) catalog entry.
    for (const auto& [object_id, candidate] : winners) {
        const ShardManifest& manifest = candidate.manifest;
        const std::size_t total = static_cast<std::size_t>(
            manifest.erasure.data_shards + manifest.erasure.parity_shards);

        ObjectMetadata metadata;
        metadata.id = object_id;
        metadata.size = static_cast<std::size_t>(manifest.size);
        metadata.checksum = manifest.checksum;
        metadata.erasure = manifest.erasure;
        metadata.encrypted_dek = manifest.encrypted_dek;
        metadata.kek_id = manifest.kek_id;

        // Full positional location vector (index == position). Missing shards get
        // an unreachable sentinel so the object stays valid and repairable.
        metadata.shard_locations.assign(total, std::string());
        for (std::size_t idx = 0; idx < total; ++idx) {
            auto found = candidate.group->shard_locations.find(
                static_cast<uint32_t>(idx));
            if (found != candidate.group->shard_locations.end()) {
                metadata.shard_locations[idx] = found->second;
            } else {
                metadata.shard_locations[idx] =
                    kUnknownNode + "/" +
                    make_shard_key(object_id, manifest.version,
                                   static_cast<uint32_t>(idx));
            }
        }

        bool inserted = false;
        try {
            inserted = metadata_store_.insert_if_absent(metadata);
        } catch (...) {
            ++report.errors;
            continue;
        }

        if (!inserted) {
            ++report.objects_skipped_existing;
            continue;
        }

        ++report.objects_recovered;
        if (candidate.shard_count < total) {
            ++report.degraded_objects;
        }
    }

    return report;
}

RebalanceObjectResult StorageClient::rebalance_object(
    const std::string& object_id,
    const RebalancePolicy& policy
) {
    if (object_id.empty()) {
        throw std::invalid_argument(
            "StorageClient::rebalance_object: object_id cannot be empty");
    }

    RebalanceObjectResult result;
    result.object_id = object_id;

    // 1) Load metadata and record the fencing version (same fence as repair()).
    const auto metadata_opt = metadata_store_.get(object_id);
    if (!metadata_opt.has_value()) {
        result.status = RebalanceStatus::SkippedNotFound;
        result.detail = "object not found";
        return result;
    }

    const ObjectMetadata& metadata = *metadata_opt;
    const int expected_version = metadata.version;

    const std::size_t total_shards =
        static_cast<std::size_t>(metadata.erasure.data_shards) +
        metadata.erasure.parity_shards;
    result.shards_total = total_shards;

    // 2) Safety: need a full, distinct placement of healthy nodes to move onto.
    const std::vector<std::string> eligible_nodes = metadata_store_.list_nodes();
    if (eligible_nodes.size() < total_shards) {
        result.status = RebalanceStatus::SkippedUnsafe;
        result.detail = "fewer than k+m registered nodes";
        return result;
    }

    std::unordered_set<std::string> healthy_set;
    for (const auto& node : eligible_nodes) {
        if (is_node_healthy(node)) {
            healthy_set.insert(node);
        }
    }
    if (healthy_set.size() < total_shards) {
        result.status = RebalanceStatus::SkippedUnsafe;
        result.detail = "fewer than k+m healthy nodes";
        return result;
    }

    // The metadata must describe a full positional placement (one entry per
    // shard index) for per-index move reasoning.
    if (metadata.shard_locations.size() != total_shards) {
        result.status = RebalanceStatus::SkippedDegraded;
        result.detail = "shard location count does not match k+m";
        return result;
    }

    // 3) HRW-intended placement over all registered nodes (deterministic,
    //    membership-stable). Matches the layout put() and repair() target.
    const std::vector<std::string> intended =
        hrw_intended_nodes(object_id, eligible_nodes, total_shards);

    // 4) Degraded check + parse current placement. Rebalancing a degraded object
    //    is unsafe (a move could drop live copies below k). Repair heals first.
    std::vector<std::string> current_nodes(total_shards);
    std::vector<std::string> current_keys(total_shards);
    for (std::size_t i = 0; i < total_shards; ++i) {
        const std::string& loc = metadata.shard_locations[i];
        const auto pos = loc.find('/');
        if (pos == std::string::npos) {
            result.status = RebalanceStatus::SkippedDegraded;
            result.detail = "malformed shard location";
            return result;
        }

        current_nodes[i] = loc.substr(0, pos);
        current_keys[i] = loc.substr(pos + 1);

        if (!probe_shard(current_nodes[i], current_keys[i])) {
            result.status = RebalanceStatus::SkippedDegraded;
            result.detail = "shard missing or unreachable; repair first";
            return result;
        }
    }

    // 5) Plan moves: a shard moves only if its intended node differs from its
    //    current node and that intended node is healthy.
    struct PlannedMove {
        std::size_t index = 0;
        std::string from;
        std::string to;
        std::string key;
    };
    std::vector<PlannedMove> planned;

    for (std::size_t i = 0; i < total_shards; ++i) {
        if (current_nodes[i] == intended[i]) {
            continue;
        }
        ++result.shards_misplaced;

        // Intended target down: leave the shard for a later pass rather than
        // moving it onto an unhealthy node.
        if (healthy_set.count(intended[i]) == 0) {
            continue;
        }
        planned.push_back({i, current_nodes[i], intended[i], current_keys[i]});
    }

    // Bound the number of moves for this object.
    if (policy.max_moves > 0 && planned.size() > policy.max_moves) {
        planned.resize(policy.max_moves);
    }

    if (result.shards_misplaced == 0) {
        result.status = RebalanceStatus::Balanced;
        result.detail = "already on intended placement";
        return result;
    }

    if (planned.empty()) {
        // Misplaced shards exist but none can move now (intended targets down or
        // the move budget is zero).
        result.status = RebalanceStatus::Balanced;
        result.detail = "misplaced shards present but no eligible move now";
        return result;
    }

    if (policy.dry_run) {
        result.status = RebalanceStatus::DryRun;
        result.detail =
            std::to_string(planned.size()) + " shard(s) would move";
        return result;
    }

    // 6) Copy each shard to its intended node BEFORE the metadata commit so
    //    durability is never reduced. The shard key is identical on source and
    //    target, so the self-describing manifest is preserved verbatim (ADR-0008).
    PlacementMap move_placement;
    std::vector<PlannedMove> fetched_moves;
    for (const auto& mv : planned) {
        std::optional<Bytes> payload = fetch_raw_shard(mv.from, mv.key);
        if (!payload.has_value()) {
            continue;  // could not read source; skip this shard, keep going
        }
        move_placement[mv.to].push_back({mv.key, std::move(*payload)});
        fetched_moves.push_back(mv);
    }

    if (fetched_moves.empty()) {
        result.status = RebalanceStatus::SkippedDegraded;
        result.detail = "failed to read source shards";
        return result;
    }

    // Write the new copies to the intended nodes (may throw on PUT failure;
    // callers map that to an error). Partial writes are inert orphans.
    apply_placement(move_placement);

    // 7) Build updated shard_locations for only the shards we relocated.
    std::vector<std::string> updated_locations = metadata.shard_locations;
    for (const auto& mv : fetched_moves) {
        updated_locations[mv.index] = mv.to + "/" + mv.key;
    }

    // 8) Version-fenced commit - the exact mechanism repair() relies on. If a
    //    concurrent put/remove/repair changed the version, the conditional_put
    //    affects zero rows and no metadata changes.
    ObjectMetadata updated = metadata;
    updated.shard_locations = updated_locations;

    const bool committed =
        metadata_store_.conditional_put(updated, expected_version);
    if (!committed) {
        // Concurrent mutation won the race. The freshly-written copies are inert
        // orphans reclaimed by a future GC sweep; no metadata changed.
        result.status = RebalanceStatus::SkippedConflict;
        result.detail = "metadata version changed concurrently";
        return result;
    }

    // Old copies on their previous nodes are intentionally left as inert orphans
    // (reclaimed by a future GC sweep); they are not deleted inline.
    result.status = RebalanceStatus::Moved;
    result.shards_moved = fetched_moves.size();
    result.detail = "relocated " + std::to_string(fetched_moves.size()) + " shard(s)";
    return result;
}

RebalanceReport StorageClient::rebalance(const RebalanceScope& scope) {
    RebalanceReport report;
    report.dry_run = scope.dry_run;

    // Resolve the object set: the named ids, or the whole catalog when none given.
    std::vector<std::string> object_ids = scope.object_ids;
    if (object_ids.empty()) {
        for (const auto& meta : metadata_store_.list()) {
            object_ids.push_back(meta.id);
        }
    }

    const bool bounded_moves = scope.max_moves > 0;
    std::size_t remaining_moves = scope.max_moves;  // meaningful only if bounded

    for (const auto& object_id : object_ids) {
        if (scope.max_objects > 0 && report.objects_scanned >= scope.max_objects) {
            break;
        }
        if (bounded_moves && remaining_moves == 0) {
            break;
        }

        ++report.objects_scanned;

        RebalancePolicy policy;
        policy.dry_run = scope.dry_run;
        policy.max_moves = bounded_moves ? remaining_moves : 0;

        RebalanceObjectResult r;
        try {
            r = rebalance_object(object_id, policy);
        } catch (const std::exception& e) {
            ++report.objects_errored;
            RebalanceObjectResult err;
            err.object_id = object_id;
            err.status = RebalanceStatus::Errored;
            err.detail = std::string("error: ") + e.what();
            report.results.push_back(std::move(err));
            continue;
        }

        switch (r.status) {
            case RebalanceStatus::Balanced:
                ++report.objects_balanced;
                break;
            case RebalanceStatus::Moved:
                ++report.objects_moved;
                report.shards_moved += r.shards_moved;
                if (bounded_moves) {
                    remaining_moves -= std::min(remaining_moves, r.shards_moved);
                }
                break;
            case RebalanceStatus::SkippedDegraded:
                ++report.objects_skipped_degraded;
                break;
            case RebalanceStatus::SkippedUnsafe:
                ++report.objects_skipped_unsafe;
                break;
            case RebalanceStatus::SkippedConflict:
                ++report.objects_skipped_conflict;
                break;
            case RebalanceStatus::SkippedNotFound:
                ++report.objects_not_found;
                break;
            case RebalanceStatus::DryRun:
            case RebalanceStatus::Errored:
                break;  // DryRun carried in results; Errored counted above
        }

        report.results.push_back(std::move(r));
    }

    return report;
}