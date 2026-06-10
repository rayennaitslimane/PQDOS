#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

namespace {

constexpr const char* kDefaultHost = "localhost";
constexpr int kDefaultPort = 8080;

std::string testHost() {
    if (const char* env = std::getenv("STORAGE_CLIENT_SERVER_TEST_HOST")) {
        return std::string{env};
    }
    return kDefaultHost;
}

int testPort() {
    if (const char* env = std::getenv("STORAGE_CLIENT_SERVER_TEST_PORT")) {
        return std::atoi(env);
    }
    return kDefaultPort;
}

class StorageClientServerTest : public ::testing::Test {
protected:
    httplib::Client client_{testHost(), testPort()};

    void cleanup(const std::string& id) {
        (void)client_.Delete("/objects/" + id);
    }
};

TEST_F(StorageClientServerTest, PutObjectReturnsOk) {
    const std::string id = "00000000-0000-0000-0000-000000000201";
    const std::string data = "hello world";

    nlohmann::json payload;
    payload["data"] = data;

    auto res = client_.Put(
        "/objects/" + id,
        payload.dump(),
        "application/json"
    );

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");

    cleanup(id);
}

TEST_F(StorageClientServerTest, GetObjectReturnsStoredData) {
    const std::string id = "00000000-0000-0000-0000-000000000202";
    const std::string data = "stored content";

    nlohmann::json payload;
    payload["data"] = data;

    client_.Put("/objects/" + id, payload.dump(), "application/json");

    auto res = client_.Get("/objects/" + id);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    ASSERT_TRUE(body.contains("data"));

    std::string result = body["data"].get<std::string>();
    EXPECT_EQ(result, data);

    cleanup(id);
}

TEST_F(StorageClientServerTest, DeleteObjectReturnsOk) {
    const std::string id = "00000000-0000-0000-0000-000000000203";
    const std::string data = "to be deleted";

    nlohmann::json payload;
    payload["data"] = data;

    client_.Put("/objects/" + id, payload.dump(), "application/json");

    auto res = client_.Delete("/objects/" + id);

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["status"], "ok");
}

TEST_F(StorageClientServerTest, DeleteMissingObjectReturns404) {
    auto res = client_.Delete("/objects/00000000-0000-0000-0000-ffffffffffff");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "not found");
}

TEST_F(StorageClientServerTest, ListObjectsReturnsArray) {
    const std::string id = "00000000-0000-0000-0000-000000000204";
    const std::string data = "list me";

    nlohmann::json payload;
    payload["data"] = data;

    client_.Put("/objects/" + id, payload.dump(), "application/json");

    auto res = client_.Get("/objects");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    ASSERT_TRUE(body.is_array());
    EXPECT_GE(body.size(), 1u);

    bool found = false;
    for (const auto& item : body) {
        if (item["id"] == id) {
            found = true;
            EXPECT_TRUE(item.contains("size"));
            EXPECT_TRUE(item.contains("checksum"));
            break;
        }
    }
    EXPECT_TRUE(found);

    cleanup(id);
}

TEST_F(StorageClientServerTest, PutInvalidJsonReturns400) {
    auto res = client_.Put(
        "/objects/00000000-0000-0000-0000-000000000205",
        "not json at all",
        "application/json"
    );

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "invalid JSON body");
}

TEST_F(StorageClientServerTest, PutMissingDataFieldReturns400) {
    nlohmann::json payload;
    payload["wrong_field"] = "value";

    auto res = client_.Put(
        "/objects/00000000-0000-0000-0000-000000000206",
        payload.dump(),
        "application/json"
    );

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"], "missing or invalid 'data' field");
}

} // namespace
