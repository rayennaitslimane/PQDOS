#pragma once

#include "Models.hpp"

#include <array>
#include <cstdint>
#include <vector>

Bytes checksum_sha256(const Bytes& bytes);

std::vector<EncryptedShard> encrypt_shards(
    const std::vector<PlainShard>& shards,
    const std::array<uint8_t, 32>& key
);

std::vector<PlainShard> decrypt_shards(
    const std::vector<EncryptedShard>& encrypted_shards,
    const std::array<uint8_t, 32>& key
);