#include "Models.hpp"
#include "Crypto.hpp"

#include <botan/hash.h>
#include <botan/aead.h>
#include <botan/auto_rng.h>
#include <botan/exceptn.h>

#include <vector>
#include <array>
#include <stdexcept>
#include <cstdint>

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

    auto enc = Botan::AEAD_Mode::create_or_throw(
        "AES-256/GCM", Botan::Cipher_Dir::Encryption
    );
    enc->set_key(key.data(), key.size());

    std::vector<EncryptedShard> out;
    out.reserve(shards.size());

    for (const auto& shard : shards) {
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
    const std::array<uint8_t, 32>& key
) {
    auto dec = Botan::AEAD_Mode::create_or_throw(
        "AES-256/GCM", Botan::Cipher_Dir::Decryption
    );
    dec->set_key(key.data(), key.size());

    std::vector<PlainShard> plain;
    plain.reserve(encrypted_shards.size());

    for (const auto& e : encrypted_shards) {
        PlainShard shard;
        shard.index = e.index;

        auto buf = e.ciphertext;

        // same AAD must be used
        dec->set_associated_data(
            reinterpret_cast<const uint8_t*>(&e.index),
            sizeof(e.index)
        );

        dec->start(e.nonce.data(), e.nonce.size());

        try {
            dec->finish(buf); // verifies integrity
        } catch (const Botan::Integrity_Failure&) {
            throw std::runtime_error("Shard integrity verification failed");
        }

        shard.bytes = std::move(buf);
        plain.push_back(std::move(shard));
    }

    return plain;
}
