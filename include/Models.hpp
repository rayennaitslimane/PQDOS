#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <msgpack.hpp>

using Bytes = std::vector<uint8_t>;

struct ErasureSpec {
    uint32_t data_shards = 0;
    uint32_t parity_shards = 0;
    std::size_t shard_size = 0;

    MSGPACK_DEFINE(data_shards, parity_shards, shard_size);

    Bytes serialize() const {
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, *this);

        return Bytes(
            reinterpret_cast<const uint8_t*>(sbuf.data()),
            reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size()
        );
    }

    static ErasureSpec deserialize(const Bytes& data) {
        if (data.empty()) {
            throw std::invalid_argument("ErasureSpec::deserialize: empty buffer");
        }

        msgpack::object_handle handle = msgpack::unpack(
            reinterpret_cast<const char*>(data.data()),
            data.size()
        );

        ErasureSpec result;
        handle.get().convert(result);
        return result;
    }
};

struct ObjectMetadata {
    std::string id;
    size_t size = 0;
    std::string checksum;
    ErasureSpec erasure;
    std::vector<std::string> shard_locations;
    Bytes encrypted_dek;
    std::string kek_id;
    int version = 0;
};

struct PlainShard {
    uint32_t index = 0;
    Bytes bytes;
};

struct EncryptedShard {
    uint32_t index = 0;
    Bytes nonce;
    Bytes ciphertext;

    MSGPACK_DEFINE(index, nonce, ciphertext);

    // Serialize method
    Bytes serialize() const {
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, *this);

        return Bytes(
            reinterpret_cast<const uint8_t*>(sbuf.data()),
            reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size()
        );
    }

    // Deserialize method
    static EncryptedShard deserialize(const Bytes& data) {
        if (data.empty()) {
            throw std::invalid_argument("deserialize: empty buffer");
        }

        msgpack::object_handle handle = msgpack::unpack(
            reinterpret_cast<const char*>(data.data()),
            data.size()
        );

        EncryptedShard result;
        handle.get().convert(result);
        return result;
    }
};

// Per-object reconstruction metadata replicated onto every stored shard so the
// authoritative PostgreSQL catalog can be rebuilt from the storage nodes alone
// (ADR-0008). The manifest is identical across all shards of an object and does
// NOT carry shard_locations: physical placement is distributed knowledge that a
// rebuild rediscovers by scanning node keyspaces. Any surviving `k` shards
// therefore fully describe the object.
struct ShardManifest {
    std::string object_id;
    std::string version;
    uint64_t size = 0;
    std::string checksum;
    ErasureSpec erasure;
    Bytes encrypted_dek;
    std::string kek_id;
    // Client wall-clock (nanoseconds since epoch) used only to break ties
    // between competing versions of the same object during a rebuild.
    uint64_t written_at = 0;

    MSGPACK_DEFINE(
        object_id,
        version,
        size,
        checksum,
        erasure,
        encrypted_dek,
        kek_id,
        written_at
    );
};

// The unit actually persisted on a StorageNode: a self-describing shard bundling
// the object manifest with the encrypted shard payload. Replaces the bare
// EncryptedShard on the wire/at rest.
struct StoredShard {
    ShardManifest manifest;
    EncryptedShard shard;

    MSGPACK_DEFINE(manifest, shard);

    Bytes serialize() const {
        msgpack::sbuffer sbuf;
        msgpack::pack(sbuf, *this);

        return Bytes(
            reinterpret_cast<const uint8_t*>(sbuf.data()),
            reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size()
        );
    }

    static StoredShard deserialize(const Bytes& data) {
        if (data.empty()) {
            throw std::invalid_argument("StoredShard::deserialize: empty buffer");
        }

        msgpack::object_handle handle = msgpack::unpack(
            reinterpret_cast<const char*>(data.data()),
            data.size()
        );

        StoredShard result;
        handle.get().convert(result);
        return result;
    }

    // Tolerant decode of a stored payload into an EncryptedShard. Accepts the
    // current StoredShard envelope and, for backward compatibility, a legacy
    // bare EncryptedShard payload. Returns the shard and, when present, its
    // manifest (nullopt for legacy payloads with no embedded metadata).
    static std::pair<EncryptedShard, std::optional<ShardManifest>>
    decode_shard(const Bytes& data) {
        try {
            StoredShard stored = deserialize(data);
            return {std::move(stored.shard), std::move(stored.manifest)};
        } catch (...) {
            // Legacy payload written before ADR-0008: bare EncryptedShard.
            return {EncryptedShard::deserialize(data), std::nullopt};
        }
    }
};

struct ObjectHealth {
    std::string object_id;
    uint32_t total_shards = 0;
    uint32_t available_shards = 0;
    uint32_t required_shards = 0;
    std::vector<uint32_t> missing_indices;
    bool healthy = false;
    bool fully_replicated = false;
};
