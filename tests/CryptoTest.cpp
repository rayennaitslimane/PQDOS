#include <gtest/gtest.h>

#include "Models.hpp"
#include "Crypto.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

// Helper to build deterministic plaintext shards
std::vector<PlainShard> MakePlainShards() {
    std::vector<PlainShard> shards;

    PlainShard s0;
    s0.index = 0;
    s0.bytes = {1, 2, 3, 4, 5, 6, 7, 8};
    shards.push_back(s0);

    PlainShard s1;
    s1.index = 1;
    s1.bytes = {10, 20, 30, 40, 50, 60, 70, 80};
    shards.push_back(s1);

    PlainShard s2;
    s2.index = 2;
    s2.bytes = {100, 101, 102, 103, 104, 105, 106, 107};
    shards.push_back(s2);

    return shards;
}

// Fixed test key (32 bytes for AES-256)
std::array<uint8_t, 32> MakeTestKey() {
    return {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    };
}

} // namespace

TEST(CryptoTest, HappySerializeDeserializePath) {
    EncryptedShard original;
    original.index = 42;
    original.nonce = {1, 2, 3, 4, 5};
    original.ciphertext = {10, 20, 30, 40, 50, 60};

    Bytes serialized = original.serialize();
    EncryptedShard decoded = EncryptedShard::deserialize(serialized);

    EXPECT_EQ(decoded.index, original.index);
    EXPECT_EQ(decoded.nonce, original.nonce);
    EXPECT_EQ(decoded.ciphertext, original.ciphertext);
}

TEST(CryptoTest, DeserializeFailsOnEmptyBuffer) {
    Bytes empty;

    EXPECT_THROW(
        EncryptedShard::deserialize(empty),
        std::invalid_argument
    );
}

TEST(CryptoTest, ChecksumIsDeterministic) {
    Bytes data = {1, 2, 3, 4, 5};

    Bytes c1 = checksum_sha256(data);
    Bytes c2 = checksum_sha256(data);

    EXPECT_EQ(c1, c2);
    EXPECT_EQ(c1.size(), 32u); // SHA-256 digest size
}

TEST(CryptoTest, ChecksumChangesWhenDataChanges) {
    Bytes d1 = {1, 2, 3, 4, 5};
    Bytes d2 = {1, 2, 3, 4, 6};

    Bytes c1 = checksum_sha256(d1);
    Bytes c2 = checksum_sha256(d2);

    EXPECT_NE(c1, c2);
}

TEST(CryptoTest, EncryptDecryptRoundTripSucceeds) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);
    std::vector<PlainShard> decrypted = decrypt_shards(encrypted, key);

    ASSERT_EQ(decrypted.size(), original_shards.size());

    for (size_t i = 0; i < original_shards.size(); ++i) {
        EXPECT_EQ(decrypted[i].index, original_shards[i].index);
        EXPECT_EQ(decrypted[i].bytes, original_shards[i].bytes);
    }
}

TEST(CryptoTest, EncryptionPreservesShardIndexes) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);

    ASSERT_EQ(encrypted.size(), original_shards.size());

    for (size_t i = 0; i < original_shards.size(); ++i) {
        EXPECT_EQ(encrypted[i].index, original_shards[i].index);
    }
}

TEST(CryptoTest, CiphertextDiffersFromPlaintext) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);

    ASSERT_EQ(encrypted.size(), original_shards.size());

    for (size_t i = 0; i < original_shards.size(); ++i) {
        EXPECT_NE(encrypted[i].ciphertext, original_shards[i].bytes);
    }
}

TEST(CryptoTest, DecryptFailsWhenCiphertextIsTampered) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);
    ASSERT_FALSE(encrypted.empty());
    ASSERT_FALSE(encrypted[0].ciphertext.empty());

    // Tamper with ciphertext
    encrypted[0].ciphertext[0] ^= 0xFF;

    EXPECT_THROW(
        decrypt_shards(encrypted, key),
        std::runtime_error
    );
}

TEST(CryptoTest, DecryptFailsWhenNonceIsTampered) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);
    ASSERT_FALSE(encrypted.empty());
    ASSERT_FALSE(encrypted[0].nonce.empty());

    // Tamper with nonce
    encrypted[0].nonce[0] ^= 0xFF;

    EXPECT_THROW(
        decrypt_shards(encrypted, key),
        std::runtime_error
    );
}

TEST(CryptoTest, DecryptFailsWithWrongKey) {
    const auto correct_key = MakeTestKey();
    auto wrong_key = MakeTestKey();
    wrong_key[0] ^= 0xAA; // make key different

    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, correct_key);

    EXPECT_THROW(
        decrypt_shards(encrypted, wrong_key),
        std::runtime_error
    );
}

TEST(CryptoTest, DecryptFailsWhenIndexAADIsTampered) {
    const auto key = MakeTestKey();
    const auto original_shards = MakePlainShards();

    std::vector<EncryptedShard> encrypted = encrypt_shards(original_shards, key);
    ASSERT_FALSE(encrypted.empty());

    // Tamper with authenticated metadata (AAD)
    encrypted[0].index ^= 1u;

    EXPECT_THROW(
        decrypt_shards(encrypted, key),
        std::runtime_error
    );
}

TEST(CryptoTest, EncryptDecryptEmptyShardListSucceeds) {
    const auto key = MakeTestKey();
    std::vector<PlainShard> empty;

    std::vector<EncryptedShard> encrypted = encrypt_shards(empty, key);
    std::vector<PlainShard> decrypted = decrypt_shards(encrypted, key);

    EXPECT_TRUE(encrypted.empty());
    EXPECT_TRUE(decrypted.empty());
}