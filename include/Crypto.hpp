#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Botan { class Public_Key; class Private_Key; }

Bytes checksum_sha256(const Bytes& bytes);

void save_kek_file(
    const std::string& path,
    const std::unordered_map<std::string, std::unique_ptr<Botan::Private_Key>>& keys,
    const std::string& active_id
);

struct KekFileData {
    std::unordered_map<std::string, std::unique_ptr<Botan::Private_Key>> keys;
    std::string active_kek_id;
};

KekFileData load_kek_file(const std::string& path);

std::vector<EncryptedShard> encrypt_shards(
    const std::vector<PlainShard>& shards,
    const std::array<uint8_t, 32>& key
);

std::vector<PlainShard> decrypt_shards(
    const std::vector<EncryptedShard>& encrypted_shards,
    const std::array<uint8_t, 32>& key,
    bool best_effort = true
);

Bytes encrypt_dek(
    const std::array<uint8_t, 32>& dek,
    const Botan::Public_Key& kek_pub
);

std::array<uint8_t, 32> decrypt_dek(
    const Bytes& encrypted_dek,
    const Botan::Private_Key& kek_priv
);