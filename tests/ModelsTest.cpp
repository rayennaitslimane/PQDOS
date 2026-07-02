#include "Models.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

ShardManifest SampleManifest() {
    ShardManifest manifest;
    manifest.object_id = "00000000-0000-0000-0000-0000000000ab";
    manifest.version = "0123abcd4567ef89";
    manifest.size = 4096;
    manifest.checksum = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    manifest.erasure.data_shards = 4;
    manifest.erasure.parity_shards = 2;
    manifest.erasure.shard_size = 1024;
    manifest.encrypted_dek = Bytes{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    manifest.kek_id = "kek-abcdef";
    manifest.written_at = 1751414400000000000ULL;
    return manifest;
}

EncryptedShard SampleShard() {
    EncryptedShard shard;
    shard.index = 3;
    shard.nonce = Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    shard.ciphertext = Bytes{100, 101, 102, 103, 104, 105};
    return shard;
}

TEST(ModelsTest, StoredShardRoundTrip) {
    StoredShard stored;
    stored.manifest = SampleManifest();
    stored.shard = SampleShard();

    const Bytes serialized = stored.serialize();
    ASSERT_FALSE(serialized.empty());

    StoredShard decoded = StoredShard::deserialize(serialized);

    EXPECT_EQ(decoded.manifest.object_id, stored.manifest.object_id);
    EXPECT_EQ(decoded.manifest.version, stored.manifest.version);
    EXPECT_EQ(decoded.manifest.size, stored.manifest.size);
    EXPECT_EQ(decoded.manifest.checksum, stored.manifest.checksum);
    EXPECT_EQ(decoded.manifest.erasure.data_shards, stored.manifest.erasure.data_shards);
    EXPECT_EQ(decoded.manifest.erasure.parity_shards, stored.manifest.erasure.parity_shards);
    EXPECT_EQ(decoded.manifest.erasure.shard_size, stored.manifest.erasure.shard_size);
    EXPECT_EQ(decoded.manifest.encrypted_dek, stored.manifest.encrypted_dek);
    EXPECT_EQ(decoded.manifest.kek_id, stored.manifest.kek_id);
    EXPECT_EQ(decoded.manifest.written_at, stored.manifest.written_at);

    EXPECT_EQ(decoded.shard.index, stored.shard.index);
    EXPECT_EQ(decoded.shard.nonce, stored.shard.nonce);
    EXPECT_EQ(decoded.shard.ciphertext, stored.shard.ciphertext);
}

TEST(ModelsTest, DecodeShardAcceptsStoredEnvelope) {
    StoredShard stored;
    stored.manifest = SampleManifest();
    stored.shard = SampleShard();

    auto [shard, manifest] = StoredShard::decode_shard(stored.serialize());

    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->object_id, stored.manifest.object_id);
    EXPECT_EQ(shard.index, stored.shard.index);
    EXPECT_EQ(shard.ciphertext, stored.shard.ciphertext);
}

TEST(ModelsTest, DecodeShardFallsBackToLegacyBareShard) {
    // Payloads written before ADR-0008 are bare EncryptedShard msgpack.
    const EncryptedShard legacy = SampleShard();
    const Bytes serialized = legacy.serialize();

    auto [shard, manifest] = StoredShard::decode_shard(serialized);

    EXPECT_FALSE(manifest.has_value());
    EXPECT_EQ(shard.index, legacy.index);
    EXPECT_EQ(shard.nonce, legacy.nonce);
    EXPECT_EQ(shard.ciphertext, legacy.ciphertext);
}

} // namespace
