#include "StorageNode.hpp"
#include "Models.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace {

namespace fs = std::filesystem;

class StorageNodeTest : public ::testing::Test {
protected:
    fs::path test_dir_;

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("storage_node_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                     "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));

        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    std::string dbPath() const {
        return test_dir_.string();
    }

    static Bytes bytes(std::initializer_list<uint8_t> values) {
        return Bytes(values);
    }

    static void expectBytesEq(const std::optional<Bytes>& actual, const Bytes& expected) {
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual.value(), expected);
    }
};

TEST_F(StorageNodeTest, PutAndGetSinglePayload) {
    StorageNode node(dbPath().c_str());

    const std::vector<std::string> locations = {"object-1/shard-0"};
    const std::vector<Bytes> payloads = {
        bytes({1, 2, 3, 4, 5})
    };

    node.put(locations, payloads);

    auto result = node.get(locations);

    ASSERT_EQ(result.size(), 1u);
    expectBytesEq(result[0], payloads[0]);
}

TEST_F(StorageNodeTest, PutAndGetMultiplePayloadsPreservesOrder) {
    StorageNode node(dbPath().c_str());

    const std::vector<std::string> locations = {
        "object-1/shard-0",
        "object-1/shard-1",
        "object-1/shard-2"
    };

    const std::vector<Bytes> payloads = {
        bytes({10, 11, 12}),
        bytes({20, 21, 22}),
        bytes({30, 31, 32})
    };

    node.put(locations, payloads);

    auto result = node.get({
        "object-1/shard-2",
        "object-1/shard-0",
        "object-1/shard-1"
    });

    ASSERT_EQ(result.size(), 3u);
    expectBytesEq(result[0], payloads[2]);
    expectBytesEq(result[1], payloads[0]);
    expectBytesEq(result[2], payloads[1]);
}

TEST_F(StorageNodeTest, GetReturnsNulloptForMissingLocation) {
    StorageNode node(dbPath().c_str());

    auto result = node.get({"missing-key"});

    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].has_value());
}

TEST_F(StorageNodeTest, GetMixesExistingAndMissingLocations) {
    StorageNode node(dbPath().c_str());

    node.put(
        {"existing-1", "existing-2"},
        {
            bytes({1, 2, 3}),
            bytes({4, 5, 6})
        }
    );

    auto result = node.get({"existing-1", "missing", "existing-2"});

    ASSERT_EQ(result.size(), 3u);
    expectBytesEq(result[0], bytes({1, 2, 3}));
    EXPECT_FALSE(result[1].has_value());
    expectBytesEq(result[2], bytes({4, 5, 6}));
}

TEST_F(StorageNodeTest, PutOverwritesExistingPayload) {
    StorageNode node(dbPath().c_str());

    node.put({"same-key"}, {bytes({1, 2, 3})});
    node.put({"same-key"}, {bytes({9, 8, 7, 6})});

    auto result = node.get({"same-key"});

    ASSERT_EQ(result.size(), 1u);
    expectBytesEq(result[0], bytes({9, 8, 7, 6}));
}

TEST_F(StorageNodeTest, RemoveReturnsDeletedPayloadAndDeletesIt) {
    StorageNode node(dbPath().c_str());

    const std::string location = "object/remove-me";
    const Bytes payload = bytes({42, 43, 44});

    node.put({location}, {payload});

    auto removed = node.remove({location});

    ASSERT_EQ(removed.size(), 1u);
    expectBytesEq(removed[0], payload);

    auto afterRemove = node.get({location});

    ASSERT_EQ(afterRemove.size(), 1u);
    EXPECT_FALSE(afterRemove[0].has_value());
}

TEST_F(StorageNodeTest, RemoveReturnsNulloptForMissingLocation) {
    StorageNode node(dbPath().c_str());

    auto removed = node.remove({"does-not-exist"});

    ASSERT_EQ(removed.size(), 1u);
    EXPECT_FALSE(removed[0].has_value());
}

TEST_F(StorageNodeTest, RemoveHandlesExistingAndMissingLocations) {
    StorageNode node(dbPath().c_str());

    node.put(
        {"key-1", "key-2"},
        {
            bytes({1}),
            bytes({2})
        }
    );

    auto removed = node.remove({"key-1", "missing", "key-2"});

    ASSERT_EQ(removed.size(), 3u);
    expectBytesEq(removed[0], bytes({1}));
    EXPECT_FALSE(removed[1].has_value());
    expectBytesEq(removed[2], bytes({2}));

    auto afterRemove = node.get({"key-1", "key-2"});

    ASSERT_EQ(afterRemove.size(), 2u);
    EXPECT_FALSE(afterRemove[0].has_value());
    EXPECT_FALSE(afterRemove[1].has_value());
}

TEST_F(StorageNodeTest, DataPersistsAfterReopeningStorageNode) {
    const std::string location = "persistent-key";
    const Bytes payload = bytes({100, 101, 102, 103});

    {
        StorageNode node(dbPath().c_str());
        node.put({location}, {payload});
    }

    {
        StorageNode reopened(dbPath().c_str());

        auto result = reopened.get({location});

        ASSERT_EQ(result.size(), 1u);
        expectBytesEq(result[0], payload);
    }
}

TEST_F(StorageNodeTest, SupportsBinaryPayloadWithNullBytes) {
    StorageNode node(dbPath().c_str());

    const std::string location = "binary-payload";
    const Bytes payload = bytes({
        0x00, 0x01, 0x02, 0x00,
        0xFF, 0x10, 0x00, 0x7F
    });

    node.put({location}, {payload});

    auto result = node.get({location});

    ASSERT_EQ(result.size(), 1u);
    expectBytesEq(result[0], payload);
}

TEST_F(StorageNodeTest, SupportsEmptyPayload) {
    StorageNode node(dbPath().c_str());

    const std::string location = "empty-payload";
    const Bytes payload = {};

    node.put({location}, {payload});

    auto result = node.get({location});

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].has_value());
    EXPECT_TRUE(result[0]->empty());
}

TEST_F(StorageNodeTest, StoresSerializedEncryptedShard) {
    StorageNode node(dbPath().c_str());

    EncryptedShard shard;
    shard.index = 3;
    shard.nonce = bytes({1, 2, 3, 4});
    shard.ciphertext = bytes({10, 20, 30, 40, 50});

    const Bytes serialized = shard.serialize();

    node.put({"encrypted-shard-3"}, {serialized});

    auto result = node.get({"encrypted-shard-3"});

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].has_value());

    EncryptedShard decoded = EncryptedShard::deserialize(result[0].value());

    EXPECT_EQ(decoded.index, shard.index);
    EXPECT_EQ(decoded.nonce, shard.nonce);
    EXPECT_EQ(decoded.ciphertext, shard.ciphertext);
}

TEST_F(StorageNodeTest, StoresSerializedErasureSpec) {
    StorageNode node(dbPath().c_str());

    ErasureSpec spec;
    spec.data_shards = 4;
    spec.parity_shards = 2;
    spec.shard_size = 1024;

    const Bytes serialized = spec.serialize();

    node.put({"erasure-spec"}, {serialized});

    auto result = node.get({"erasure-spec"});

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].has_value());

    ErasureSpec decoded = ErasureSpec::deserialize(result[0].value());

    EXPECT_EQ(decoded.data_shards, spec.data_shards);
    EXPECT_EQ(decoded.parity_shards, spec.parity_shards);
    EXPECT_EQ(decoded.shard_size, spec.shard_size);
}

TEST_F(StorageNodeTest, GetWithEmptyLocationsReturnsEmptyVector) {
    StorageNode node(dbPath().c_str());

    auto result = node.get({});

    EXPECT_TRUE(result.empty());
}

TEST_F(StorageNodeTest, RemoveWithEmptyLocationsReturnsEmptyVector) {
    StorageNode node(dbPath().c_str());

    auto result = node.remove({});

    EXPECT_TRUE(result.empty());
}

TEST_F(StorageNodeTest, PutWithEmptyInputsDoesNotThrow) {
    StorageNode node(dbPath().c_str());

    EXPECT_NO_THROW(node.put({}, {}));
}

TEST_F(StorageNodeTest, PutThrowsWhenLocationsAndPayloadsSizesDiffer) {
    StorageNode node(dbPath().c_str());

    EXPECT_THROW(
        node.put(
            {"key-1", "key-2"},
            {bytes({1, 2, 3})}
        ),
        std::exception
    );

    EXPECT_THROW(
        node.put(
            {"key-1"},
            {
                bytes({1}),
                bytes({2})
            }
        ),
        std::exception
    );
}

TEST_F(StorageNodeTest, ListLocationsReturnsAllStoredKeys) {
    StorageNode node(dbPath().c_str());

    const std::vector<std::string> locations = {
        "obj-a/v1/0",
        "obj-a/v1/1",
        "obj-b/v2/0"
    };
    const std::vector<Bytes> payloads = {
        bytes({1}),
        bytes({2}),
        bytes({3})
    };

    node.put(locations, payloads);

    std::vector<std::string> listed = node.list_locations();
    std::sort(listed.begin(), listed.end());

    std::vector<std::string> expected = locations;
    std::sort(expected.begin(), expected.end());

    EXPECT_EQ(listed, expected);
}

TEST_F(StorageNodeTest, ListLocationsIsEmptyForFreshNode) {
    StorageNode node(dbPath().c_str());

    EXPECT_TRUE(node.list_locations().empty());
}

TEST_F(StorageNodeTest, ListLocationsExcludesRemovedKeys) {
    StorageNode node(dbPath().c_str());

    node.put({"keep/v/0", "drop/v/0"}, {bytes({1}), bytes({2})});
    node.remove({"drop/v/0"});

    const std::vector<std::string> listed = node.list_locations();

    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0], "keep/v/0");
}

} // namespace