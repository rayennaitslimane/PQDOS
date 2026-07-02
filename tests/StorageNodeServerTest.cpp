#include "Models.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultHost = "localhost";
constexpr int kDefaultPort = 9001;

std::string testHost() {
    if (const char* env = std::getenv("STORAGE_NODE_TEST_HOST")) {
        return std::string{env};
    }
    return kDefaultHost;
}

int testPort() {
    if (const char* env = std::getenv("STORAGE_NODE_TEST_PORT")) {
        return std::atoi(env);
    }
    return kDefaultPort;
}

class StorageNodeServerTest : public ::testing::Test {
protected:
    httplib::Client client_{testHost(), testPort()};

    static std::string shardUri(const std::string& location) {
        return "/shards?location=" + location;
    }

    void cleanup(const std::string& location) {
        (void)client_.Delete(shardUri(location));
    }
};

TEST_F(StorageNodeServerTest, HealthCheckReturnsOk) {
    auto res = client_.Get("/health");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
}

TEST_F(StorageNodeServerTest, PutShardReturnsOk) {
    const std::string location = "test-put-shard";
    const std::string payload = "hello binary data";

    auto res = client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");

    cleanup(location);
}

TEST_F(StorageNodeServerTest, GetShardReturnsStoredPayload) {
    const std::string location = "test-get-shard";
    const std::string payload = "binary shard content";

    client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    auto res = client_.Get(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, payload);

    cleanup(location);
}

TEST_F(StorageNodeServerTest, GetMissingShardReturns404) {
    auto res = client_.Get(shardUri("nonexistent-location"));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "not found");
}

TEST_F(StorageNodeServerTest, DeleteShardReturnsPayload) {
    const std::string location = "test-delete-shard";
    const std::string payload = "to be deleted";

    client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    auto res = client_.Delete(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, payload);
}

TEST_F(StorageNodeServerTest, DeleteMissingShardReturns404) {
    auto res = client_.Delete(shardUri("nonexistent-delete"));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "not found");
}

TEST_F(StorageNodeServerTest, GetAfterDeleteReturns404) {
    const std::string location = "test-get-after-delete";
    const std::string payload = "ephemeral data";

    client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    client_.Delete(shardUri(location));

    auto res = client_.Get(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(StorageNodeServerTest, PutOverwritesExistingPayload) {
    const std::string location = "test-overwrite";
    const std::string payload1 = "first version";
    const std::string payload2 = "second version";

    client_.Put(
        shardUri(location),
        payload1,
        "application/octet-stream"
    );

    client_.Put(
        shardUri(location),
        payload2,
        "application/octet-stream"
    );

    auto res = client_.Get(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, payload2);

    cleanup(location);
}

TEST_F(StorageNodeServerTest, PutAndGetBinaryPayloadWithNullBytes) {
    const std::string location = "test-binary-null";
    const std::string payload = std::string("\x00\x01\x02\x00\xFF\x10\x00\x7F", 8);

    client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    auto res = client_.Get(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->body, payload);

    cleanup(location);
}

TEST_F(StorageNodeServerTest, PutAndGetSerializedEncryptedShard) {
    EncryptedShard shard;
    shard.index = 5;
    shard.nonce = {10, 20, 30};
    shard.ciphertext = {40, 50, 60, 70, 80};

    const Bytes serialized = shard.serialize();
    const std::string payload(serialized.begin(), serialized.end());

    const std::string location = "test-serialized-shard";

    client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    auto res = client_.Get(shardUri(location));

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    Bytes fetched(res->body.begin(), res->body.end());
    EncryptedShard decoded = EncryptedShard::deserialize(fetched);

    EXPECT_EQ(decoded.index, shard.index);
    EXPECT_EQ(decoded.nonce, shard.nonce);
    EXPECT_EQ(decoded.ciphertext, shard.ciphertext);

    cleanup(location);
}

TEST_F(StorageNodeServerTest, PutAndGetShardWithStructuredKey) {
    const std::string location =
        "00000000-0000-0000-0000-00000000abcd/0123abcd4567ef89/2";
    const std::string payload = "structured-key-payload";

    auto put_res = client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );

    ASSERT_TRUE(put_res);
    ASSERT_EQ(put_res->status, 200);

    auto get_res = client_.Get(shardUri(location));
    ASSERT_TRUE(get_res);
    ASSERT_EQ(get_res->status, 200);
    EXPECT_EQ(get_res->body, payload);

    auto del_res = client_.Delete(shardUri(location));
    ASSERT_TRUE(del_res);
    ASSERT_EQ(del_res->status, 200);
}

TEST_F(StorageNodeServerTest, ListShardsReturnsStoredKeys) {
    const std::string location =
        "11111111-1111-1111-1111-111111111111/deadbeefdeadbeef/0";
    const std::string payload = "list-endpoint-payload";

    auto put_res = client_.Put(
        shardUri(location),
        payload,
        "application/octet-stream"
    );
    ASSERT_TRUE(put_res);
    ASSERT_EQ(put_res->status, 200);

    auto res = client_.Get("/shards/list");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    ASSERT_TRUE(body.is_array());

    bool found = false;
    for (const auto& entry : body) {
        if (entry.is_string() && entry.get<std::string>() == location) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);

    cleanup(location);
}

} // namespace
