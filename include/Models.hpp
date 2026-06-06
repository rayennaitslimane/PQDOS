#pragma once

#include <cstdint>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

struct ErasureSpec {
    uint32_t data_shards = 0;
    uint32_t parity_shards = 0;
    size_t shard_size = 0;
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
};
