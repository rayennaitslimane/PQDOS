#include "Models.hpp"
#include "Crypto.hpp"

#include <botan/hash.h>
#include <botan/aead.h>
#include <botan/auto_rng.h>
#include <botan/exceptn.h>
#include <botan/mem_ops.h>
#include <botan/pk_algs.h>
#include <botan/pkcs8.h>
#include <botan/data_src.h>
#include <botan/pubkey.h>
#include <nlohmann/json.hpp>

#include <vector>
#include <array>
#include <stdexcept>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

// =========================
// KEK File Persistence
// =========================

void save_kek_file(
    const std::string& path,
    const std::unordered_map<std::string, std::unique_ptr<Botan::Private_Key>>& keys,
    const std::string& active_id
) {
    namespace fs = std::filesystem;

    // Ensure parent directory exists with 0700
    fs::path file_path(path);
    fs::path parent = file_path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
        fs::permissions(parent,
            fs::perms::owner_all,
            fs::perm_options::replace);
    }

    // Build JSON
    nlohmann::json j;
    j["version"] = 1;
    j["active_kek_id"] = active_id;

    nlohmann::json keys_array = nlohmann::json::array();
    for (const auto& [id, key] : keys) {
        nlohmann::json entry;
        entry["id"] = id;
        entry["pem"] = Botan::PKCS8::PEM_encode(*key);
        keys_array.push_back(std::move(entry));
    }
    j["keys"] = std::move(keys_array);

    // Write file
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        throw std::runtime_error("save_kek_file: cannot open file: " + path);
    }
    file << j.dump(2);
    file.close();

    // Set file permissions to 0600 immediately
    fs::permissions(file_path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

KekFileData load_kek_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("load_kek_file: cannot open file: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            std::string("load_kek_file: invalid JSON: ") + e.what()
        );
    }

    if (!j.contains("version") || j["version"] != 1) {
        throw std::runtime_error("load_kek_file: unsupported version");
    }

    if (!j.contains("active_kek_id") || !j.contains("keys")) {
        throw std::runtime_error("load_kek_file: missing required fields");
    }

    KekFileData data;
    data.active_kek_id = j["active_kek_id"].get<std::string>();

    Botan::AutoSeeded_RNG rng;
    for (const auto& entry : j["keys"]) {
        std::string id = entry["id"].get<std::string>();
        std::string pem = entry["pem"].get<std::string>();

        Botan::DataSource_Memory src(pem);
        auto key = Botan::PKCS8::load_key(src);
        if (!key) {
            throw std::runtime_error(
                "load_kek_file: failed to load key: " + id
            );
        }
        data.keys.emplace(std::move(id), std::move(key));
    }

    if (data.keys.find(data.active_kek_id) == data.keys.end()) {
        throw std::runtime_error(
            "load_kek_file: active_kek_id not found in keys"
        );
    }

    return data;
}

// Checksum generated and validated before erasure encoding takes place
Bytes checksum_sha256(const Bytes& bytes) {
    auto hash = Botan::HashFunction::create_or_throw("SHA-256");
    hash->update(bytes);
    return hash->final_stdvec();
}

std::vector<EncryptedShard> encrypt_shards(
    const std::vector<PlainShard>& shards,
    const std::array<uint8_t, 32>& key
) {
    Botan::AutoSeeded_RNG rng;

    std::vector<EncryptedShard> out;
    out.reserve(shards.size());

    for (const auto& shard : shards) {
        auto enc = Botan::AEAD_Mode::create_or_throw(
            "AES-256/GCM", Botan::Cipher_Dir::Encryption
        );
        enc->set_key(key.data(), key.size());

        EncryptedShard e;
        e.index = shard.index;

        // Nonce
        const size_t nonce_len = enc->default_nonce_length();
        e.nonce.resize(nonce_len);
        rng.randomize(e.nonce.data(), e.nonce.size());

        // Copy plaintext
        e.ciphertext = shard.bytes;

        // AAD (protect index)
        enc->set_associated_data(
            reinterpret_cast<const uint8_t*>(&e.index),
            sizeof(e.index)
        );

        enc->start(e.nonce.data(), e.nonce.size());
        enc->finish(e.ciphertext); // ciphertext + tag

        out.push_back(std::move(e));
    }

    return out;
}

std::vector<PlainShard> decrypt_shards(
    const std::vector<EncryptedShard>& encrypted_shards,
    const std::array<uint8_t, 32>& key,
    bool best_effort
) {
    std::vector<PlainShard> plain;
    plain.reserve(encrypted_shards.size());

    for (const auto& e : encrypted_shards) {
        PlainShard shard;
        shard.index = e.index;

        auto buf = e.ciphertext;

        try {
            auto dec = Botan::AEAD_Mode::create_or_throw(
                "AES-256/GCM", Botan::Cipher_Dir::Decryption
            );
            dec->set_key(key.data(), key.size());

            dec->set_associated_data(
                reinterpret_cast<const uint8_t*>(&e.index),
                sizeof(e.index)
            );

            dec->start(e.nonce.data(), e.nonce.size());
            dec->finish(buf);

            shard.bytes = std::move(buf);
            plain.push_back(std::move(shard));
        } catch (const Botan::Integrity_Failure&) {
            if (!best_effort) {
                throw std::runtime_error("decrypt_shards: integrity check failed");
            }
            continue;
        } catch (...) {
            if (!best_effort) {
                throw std::runtime_error("decrypt_shards: decryption failed");
            }
            continue;
        }
    }

    return plain;
}

Bytes encrypt_dek(
    const std::array<uint8_t, 32>& dek,
    const Botan::Public_Key& kek_pub
) {
    Botan::AutoSeeded_RNG rng;

    // KEM encapsulate to derive shared secret
    Botan::PK_KEM_Encryptor kem_enc(kek_pub, "HKDF(SHA-256)");
    auto kem_result = kem_enc.encrypt(rng);

    const auto& shared_secret = kem_result.shared_key();
    const auto& encapsulated_key = kem_result.encapsulated_shared_key();

    // AES-256/GCM wrap DEK with shared_secret
    auto enc = Botan::AEAD_Mode::create_or_throw(
        "AES-256/GCM", Botan::Cipher_Dir::Encryption
    );
    enc->set_key(shared_secret);

    const size_t nonce_len = enc->default_nonce_length();
    Bytes nonce(nonce_len);
    rng.randomize(nonce.data(), nonce.size());

    Bytes buf(dek.begin(), dek.end());

    enc->start(nonce.data(), nonce.size());
    enc->finish(buf);

    // Return encapsulated_key || nonce || ciphertext+tag
    Bytes result;
    result.reserve(encapsulated_key.size() + nonce.size() + buf.size());
    result.insert(result.end(), encapsulated_key.begin(), encapsulated_key.end());
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), buf.begin(), buf.end());

    return result;
}

std::array<uint8_t, 32> decrypt_dek(
    const Bytes& encrypted_dek,
    const Botan::Private_Key& kek_priv
) {
    Botan::AutoSeeded_RNG rng;

    // KEM decapsulate to recover shared secret
    Botan::PK_KEM_Decryptor kem_dec(kek_priv, rng, "HKDF(SHA-256)");
    const size_t encap_key_len = kem_dec.encapsulated_key_length();

    if (encrypted_dek.size() <= encap_key_len) {
        throw std::runtime_error("decrypt_dek: encrypted_dek too short");
    }

    auto shared_secret = kem_dec.decrypt(
        std::span<const uint8_t>(encrypted_dek.data(), encap_key_len)
    );

    // AES-256/GCM unwrap DEK with shared_secret
    auto dec = Botan::AEAD_Mode::create_or_throw(
        "AES-256/GCM", Botan::Cipher_Dir::Decryption
    );
    dec->set_key(shared_secret);

    const size_t nonce_len = dec->default_nonce_length();
    const size_t remaining = encrypted_dek.size() - encap_key_len;

    if (remaining <= nonce_len) {
        throw std::runtime_error("decrypt_dek: encrypted_dek too short");
    }

    const uint8_t* after_encap = encrypted_dek.data() + encap_key_len;
    Bytes nonce(after_encap, after_encap + nonce_len);
    Bytes buf(after_encap + nonce_len, encrypted_dek.data() + encrypted_dek.size());

    dec->start(nonce.data(), nonce.size());
    dec->finish(buf);

    if (buf.size() != 32) {
        throw std::runtime_error("decrypt_dek: decrypted key has unexpected size");
    }

    std::array<uint8_t, 32> dek_out;
    std::copy(buf.begin(), buf.end(), dek_out.begin());
    Botan::secure_scrub_memory(buf.data(), buf.size());
    return dek_out;
}