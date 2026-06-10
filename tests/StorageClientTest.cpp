#include <gtest/gtest.h>
#include <httplib.h>
#include <pqxx/pqxx>

#include "StorageClient.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string testConnectionString() {
    if (const char* env = std::getenv("METADATASTORE_TEST_CONN")) {
        return std::string{env};
    }

    return "host=localhost "
           "port=5433 "
           "dbname=pqdos_test "
           "user=test_user "
           "password=test_password";
}

Bytes ToBytes(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

std::string ToString(const Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

bool ContainsObjectId(
    const std::vector<ObjectMetadata>& items,
    const std::string& object_id
) {
    return std::any_of(items.begin(), items.end(), [&](const ObjectMetadata& m) {
        return m.id == object_id;
    });
}

const ObjectMetadata* FindObjectMetadata(
    const std::vector<ObjectMetadata>& items,
    const std::string& object_id
) {
    auto it = std::find_if(items.begin(), items.end(), [&](const ObjectMetadata& m) {
        return m.id == object_id;
    });
    return it == items.end() ? nullptr : &(*it);
}

class StorageClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        connStr_ = testConnectionString();

        clearDatabase();

        client_ = std::make_unique<StorageClient>(connStr_);
        client_->init();
    }

    void TearDown() override {
        client_.reset();
        clearDatabase();
    }

    void clearDatabase() {
        pqxx::connection conn{connStr_};
        pqxx::work tx{conn};

        tx.exec(R"SQL(
            TRUNCATE TABLE
                object_shard_locations,
                object_metadata
            RESTART IDENTITY
            CASCADE
        )SQL");

        tx.commit();
    }

    StorageClient& client() { return *client_; }

    std::string connStr_;
    std::unique_ptr<StorageClient> client_;
};

TEST_F(StorageClientTest, PutTest) {
    const std::string object_id = "00000000-0000-0000-0000-000000000101";
    const Bytes payload = ToBytes("Hello from put test");

    ASSERT_NO_THROW(client().put(object_id, payload));

    const auto items = client().list();
    ASSERT_TRUE(ContainsObjectId(items, object_id));

    const ObjectMetadata* meta = FindObjectMetadata(items, object_id);
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->id, object_id);
    EXPECT_EQ(meta->size, payload.size());
    EXPECT_EQ(meta->shard_locations.size(), 3u);
}

TEST_F(StorageClientTest, GetTest) {
    const std::string object_id = "00000000-0000-0000-0000-000000000102";
    const std::string original = "Hello from get test 1234567890";

    client().put(object_id, ToBytes(original));

    const Bytes restored = client().get(object_id);
    EXPECT_EQ(ToString(restored), original);
}

TEST_F(StorageClientTest, ListTest) {
    const std::string id1 = "00000000-0000-0000-0000-000000000103";
    const std::string id2 = "00000000-0000-0000-0000-000000000104";
    const std::string id3 = "00000000-0000-0000-0000-000000000105";

    client().put(id1, ToBytes("payload-1"));
    client().put(id2, ToBytes("payload-2"));
    client().put(id3, ToBytes("payload-3"));

    const auto items = client().list();

    EXPECT_EQ(items.size(), 3u);
    EXPECT_TRUE(ContainsObjectId(items, id1));
    EXPECT_TRUE(ContainsObjectId(items, id2));
    EXPECT_TRUE(ContainsObjectId(items, id3));
}

TEST_F(StorageClientTest, DeleteTest) {
    const std::string object_id = "00000000-0000-0000-0000-000000000107";
    client().put(object_id, ToBytes("to-be-deleted"));

    EXPECT_TRUE(client().remove(object_id));
    EXPECT_FALSE(client().remove(object_id));

    const auto items = client().list();
    EXPECT_FALSE(ContainsObjectId(items, object_id));
    EXPECT_TRUE(items.empty());

    EXPECT_THROW(
        {
            const auto _ = client().get(object_id);
            (void)_;
        },
        std::runtime_error
    );
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}