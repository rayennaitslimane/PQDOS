#include <gtest/gtest.h>

#include "Models.hpp"
#include "ErasureCodec.hpp"
#include "Crypto.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Small helper to generate deterministic test data.
Bytes MakeTestData(size_t size) {
    Bytes data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    return data;
}

// Convert a normal text string into Bytes.
Bytes StringToBytes(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

// Convert Bytes back into a normal text string.
std::string BytesToString(const Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST(ErasureCodecTest, HappyEncodeDecodePath) {
    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 8;

    // 25 bytes => fits in 4 * 8 = 32 bytes total capacity, with zero padding
    // inside the last data shard. decode(..., original_size) must trim back.
    Bytes original = MakeTestData(25);

    std::vector<PlainShard> shards = encode(original, spec);

    // Basic black-box expectations on the encoded result.
    ASSERT_EQ(shards.size(), spec.data_shards + spec.parity_shards);
    for (uint32_t i = 0; i < shards.size(); ++i) {
        EXPECT_EQ(shards[i].index, i);
        EXPECT_EQ(shards[i].bytes.size(), spec.shard_size);
    }

    Bytes decoded = decode(shards, spec, original.size());

    EXPECT_EQ(decoded, original);
}

TEST(ErasureCodecTest, DecodeSucceedsWhenSomeDataShardsAreDestroyed) {
    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 8;

    // 29 bytes still fits in 32 bytes total capacity.
    Bytes original = MakeTestData(29);

    std::vector<PlainShard> all_shards = encode(original, spec);
    ASSERT_EQ(all_shards.size(), 6u);

    // Simulate destruction of two DATA shards.
    // This forces decode() to go through reconstruction logic.
    std::vector<PlainShard> surviving_shards;
    for (const auto& shard : all_shards) {
        if (shard.index == 1 || shard.index == 3) {
            continue;  // destroyed
        }
        surviving_shards.push_back(shard);
    }

    // We still have exactly k shards left: 0, 2, 4, 5
    ASSERT_EQ(surviving_shards.size(), spec.data_shards);

    Bytes decoded = decode(surviving_shards, spec, original.size());

    EXPECT_EQ(decoded, original);
}

TEST(ErasureCodecTest, StringRoundTripSucceedsAfterShardLoss) {
    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 20;

    const std::string original_text = "J'aime ma femme et mon tchou-tchou, et mon gugu, et ma titouille !";
    Bytes original = StringToBytes(original_text);

    // Sanity check: message must fit in total data capacity.
    ASSERT_LE(original.size(), spec.data_shards * spec.shard_size);

    std::vector<PlainShard> all_shards = encode(original, spec);
    ASSERT_EQ(all_shards.size(), spec.data_shards + spec.parity_shards);

    // Simulate destruction of two shards.
    // With 4 data + 2 parity, losing up to 2 shards should still be recoverable.
    std::vector<PlainShard> surviving_shards;
    for (const auto& shard : all_shards) {
        if (shard.index == 1 || shard.index == 4) {
            continue;  // destroyed
        }
        surviving_shards.push_back(shard);
    }

    ASSERT_EQ(surviving_shards.size(), spec.data_shards);

    // Print surviving shards for debugging
    printf("Surviving shards:\n");
    for (const auto& shard : surviving_shards) {
        printf("  Shard %u: %zu bytes : %s\n", shard.index, shard.bytes.size(), shard.bytes.data());
    }
    
    Bytes decoded = decode(surviving_shards, spec, original.size());
    std::string decoded_text = BytesToString(decoded);
    
    EXPECT_EQ(decoded_text, original_text);

    // Print decoded text for debugging
    printf("Decoded text: %s\n", decoded_text.c_str());
}

TEST(ErasureCodecTest, DecodeSucceedsForLargeDataWithPartialDestruction) {
    ErasureSpec spec;
    spec.data_shards = 20;
    spec.parity_shards = 10;
    spec.shard_size = 8;

    // 29 bytes still fits in 32 bytes total capacity.
    Bytes original = MakeTestData(160);

    std::vector<PlainShard> all_shards = encode(original, spec);
    ASSERT_EQ(all_shards.size(), spec.data_shards + spec.parity_shards);

    // Simulate destruction of two DATA shards.
    // This forces decode() to go through reconstruction logic.
    std::vector<PlainShard> surviving_shards;
    for (const auto& shard : all_shards) {
        if (shard.index == 1 || shard.index == 3) {
            continue;  // destroyed
        }
        surviving_shards.push_back(shard);
    }

    // We still have exactly k >= 20 shards left:
    ASSERT_EQ(surviving_shards.size(), all_shards.size() - 2);

    Bytes decoded = decode(surviving_shards, spec, original.size());

    EXPECT_EQ(decoded, original);
}

// =========================================================
// Robustness tests: pipeline, duplicates, corruption
// =========================================================

namespace {

std::array<uint8_t, 32> MakeFixedKey() {
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < 32; ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    return key;
}

}  // namespace

TEST(ErasureCodecTest, FullPipelineDecryptDecodeWithCorruption) {
    // End-to-end: encrypt shards, corrupt one, best-effort decrypt, decode.
    ErasureSpec spec;
    spec.data_shards = 2;
    spec.parity_shards = 1;
    spec.shard_size = 20;

    Bytes original = MakeTestData(35);

    std::vector<PlainShard> plain_shards = encode(original, spec);
    ASSERT_EQ(plain_shards.size(), 3u);

    const auto key = MakeFixedKey();
    std::vector<EncryptedShard> encrypted = encrypt_shards(plain_shards, key);
    ASSERT_EQ(encrypted.size(), 3u);

    // Corrupt shard 0 ciphertext (simulates storage node returning garbage)
    encrypted[0].ciphertext[0] ^= 0xFF;

    // best_effort=true drops corrupted shard
    std::vector<PlainShard> decrypted = decrypt_shards(encrypted, key, true);
    ASSERT_EQ(decrypted.size(), 2u); // shards 1 and 2 survive

    // Decode with 2 surviving shards (k=2 needed, have data[1] + parity[2])
    Bytes decoded = decode(decrypted, spec, original.size());
    EXPECT_EQ(decoded, original);
}

TEST(ErasureCodecTest, FaultToleranceBoundaryExactlyMCorruptions) {
    // With k=2, m=1: exactly 1 corruption should succeed,
    // 2 corruptions should fail.
    ErasureSpec spec;
    spec.data_shards = 2;
    spec.parity_shards = 1;
    spec.shard_size = 20;

    Bytes original = MakeTestData(35);

    std::vector<PlainShard> plain_shards = encode(original, spec);
    const auto key = MakeFixedKey();
    std::vector<EncryptedShard> encrypted = encrypt_shards(plain_shards, key);
    ASSERT_EQ(encrypted.size(), 3u);

    // Case 1: exactly m=1 corrupted → should reconstruct
    {
        auto corrupted = encrypted;
        corrupted[0].ciphertext[0] ^= 0xFF;

        std::vector<PlainShard> decrypted = decrypt_shards(corrupted, key, true);
        ASSERT_EQ(decrypted.size(), 2u);

        Bytes decoded = decode(decrypted, spec, original.size());
        EXPECT_EQ(decoded, original);
    }

    // Case 2: m+1=2 corrupted → decode should fail (not enough shards)
    {
        auto corrupted = encrypted;
        corrupted[0].ciphertext[0] ^= 0xFF;
        corrupted[1].nonce[0] ^= 0xFF;

        std::vector<PlainShard> decrypted = decrypt_shards(corrupted, key, true);
        ASSERT_EQ(decrypted.size(), 1u); // only parity shard survives

        EXPECT_THROW(
            decode(decrypted, spec, original.size()),
            std::runtime_error
        );
    }
}

TEST(ErasureCodecTest, DecodeWithDuplicateShardIndices) {
    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 8;

    Bytes original = MakeTestData(25);

    std::vector<PlainShard> all_shards = encode(original, spec);
    ASSERT_EQ(all_shards.size(), 6u);

    // Simulate a retry/replication: duplicate shard 0 and shard 2
    std::vector<PlainShard> with_duplicates = all_shards;
    with_duplicates.push_back(all_shards[0]); // duplicate shard 0
    with_duplicates.push_back(all_shards[2]); // duplicate shard 2

    Bytes decoded = decode(with_duplicates, spec, original.size());
    EXPECT_EQ(decoded, original);
}

TEST(ErasureCodecTest, DecodeWithShuffledShardOrder) {
    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 8;

    Bytes original = MakeTestData(25);

    std::vector<PlainShard> all_shards = encode(original, spec);

    // Shuffle to simulate out-of-order arrival from distributed nodes
    std::vector<PlainShard> shuffled = all_shards;
    // Reverse is a deterministic shuffle for test reproducibility
    std::reverse(shuffled.begin(), shuffled.end());

    Bytes decoded = decode(shuffled, spec, original.size());
    EXPECT_EQ(decoded, original);
}

TEST(ErasureCodecTest, ChecksumDetectsSilentPostDecryptionCorruption) {
    // If a bit flips in plaintext after decryption but before decode,
    // Reed-Solomon will produce garbage. The checksum must catch it.
    ErasureSpec spec;
    spec.data_shards = 2;
    spec.parity_shards = 1;
    spec.shard_size = 20;

    Bytes original = MakeTestData(35);
    Bytes original_checksum = checksum_sha256(original);

    std::vector<PlainShard> plain_shards = encode(original, spec);

    // Flip a bit in a data shard (simulates memory corruption post-decrypt)
    plain_shards[0].bytes[0] ^= 0x01;

    // Decode still "succeeds" (RS doesn't detect corruption in data shards
    // when all data shards are present - fast path)
    Bytes decoded = decode(plain_shards, spec, original.size());

    // The output is silently wrong
    EXPECT_NE(decoded, original);

    // But the checksum catches it
    Bytes decoded_checksum = checksum_sha256(decoded);
    EXPECT_NE(decoded_checksum, original_checksum);
}

// =========================================================
// Multi-scenario: varying specs, data sizes, destruction patterns
// =========================================================

struct ErasureScenario {
    std::string name;
    uint32_t data_shards;
    uint32_t parity_shards;
    size_t shard_size;
    size_t data_size;
    std::vector<uint32_t> destroyed_indices;
};

class ErasureCodecMultiScenarioTest
    : public ::testing::TestWithParam<ErasureScenario> {};

TEST_P(ErasureCodecMultiScenarioTest, EncodeDestroyDecode) {
    const auto& scenario = GetParam();

    ErasureSpec spec;
    spec.data_shards = scenario.data_shards;
    spec.parity_shards = scenario.parity_shards;
    spec.shard_size = scenario.shard_size;

    Bytes original = MakeTestData(scenario.data_size);
    ASSERT_LE(original.size(), spec.data_shards * spec.shard_size);

    std::vector<PlainShard> all_shards = encode(original, spec);
    ASSERT_EQ(all_shards.size(), spec.data_shards + spec.parity_shards);

    // Remove specified shards to simulate node failures
    std::vector<PlainShard> surviving_shards;
    for (const auto& shard : all_shards) {
        bool destroyed = std::find(scenario.destroyed_indices.begin(),
                                   scenario.destroyed_indices.end(),
                                   shard.index) != scenario.destroyed_indices.end();
        if (!destroyed) {
            surviving_shards.push_back(shard);
        }
    }

    ASSERT_GE(surviving_shards.size(), spec.data_shards)
        << "Scenario '" << scenario.name << "' must leave at least k shards";

    Bytes decoded = decode(surviving_shards, spec, original.size());
    EXPECT_EQ(decoded, original)
        << "Failed for scenario: " << scenario.name;
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios,
    ErasureCodecMultiScenarioTest,
    ::testing::Values(
        ErasureScenario{"k2_p1_no_loss", 2, 1, 20, 35, {}},
        ErasureScenario{"k2_p1_lose_parity", 2, 1, 20, 35, {2}},
        ErasureScenario{"k2_p1_lose_data", 2, 1, 20, 35, {0}},
        ErasureScenario{"k4_p2_no_loss", 4, 2, 16, 60, {}},
        ErasureScenario{"k4_p2_lose_2_data", 4, 2, 16, 60, {0, 3}},
        ErasureScenario{"k4_p2_lose_2_parity", 4, 2, 16, 60, {4, 5}},
        ErasureScenario{"k4_p2_lose_1_data_1_parity", 4, 2, 16, 60, {1, 5}},
        ErasureScenario{"k8_p4_lose_4_mixed", 8, 4, 10, 75, {0, 3, 8, 11}},
        ErasureScenario{"k8_p4_no_loss_exact_fit", 8, 4, 10, 80, {}},
        ErasureScenario{"k8_p4_lose_all_parity", 8, 4, 10, 80, {8, 9, 10, 11}},
        ErasureScenario{"k16_p8_lose_8_scattered", 16, 8, 8, 120, {1, 4, 7, 10, 16, 18, 20, 22}}
    ),
    [](const ::testing::TestParamInfo<ErasureScenario>& info) {
        return info.param.name;
    }
);