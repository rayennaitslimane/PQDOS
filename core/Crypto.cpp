#include "Models.hpp"
#include "Crypto.hpp"

#include <botan/hash.h>
#include <botan/aead.h>
#include <botan/auto_rng.h>
#include <botan/exceptn.h>
#include <botan/mem_ops.h>
#include <botan/pubkey.h>

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