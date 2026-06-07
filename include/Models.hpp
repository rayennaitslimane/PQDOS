#pragma once

#include <cstdint>
#include <string>
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
