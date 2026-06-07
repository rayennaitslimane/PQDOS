#include <gtest/gtest.h>

#include "Models.hpp"
#include "ErasureCodec.hpp"

#include <algorithm>
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

TEST(ErasureCodecBlackBoxTest, HappyEncodeDecodePath) {
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

TEST(ErasureCodecBlackBoxTest, DecodeSucceedsWhenSomeDataShardsAreDestroyed) {
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

TEST(ErasureCodecBlackBoxTest, StringRoundTripSucceedsAfterShardLoss) {
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

TEST(ErasureCodecBlackBoxTest, DecodeSucceedsForLargeDataWithPartialDestruction) {
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