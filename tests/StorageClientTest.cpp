#include <gtest/gtest.h>
#include <httplib.h>
#include <pqxx/pqxx>

#include "StorageClient.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
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

        // Use a temp keystore file per test
        kek_path_ = "/tmp/pqdos_test_kek_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".json";
        setenv("PQDOS_KEYSTORE_PATH", kek_path_.c_str(), 1);

        clearDatabase();

        client_ = std::make_unique<StorageClient>(connStr_);
        client_->init();
    }

    void TearDown() override {
        client_.reset();
        clearDatabase();
        std::filesystem::remove(kek_path_);
        unsetenv("PQDOS_KEYSTORE_PATH");
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
    std::string kek_path_;
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

TEST_F(StorageClientTest, RotationTest) {
    const std::string id1 = "00000000-0000-0000-0000-000000000201";
    const std::string id2 = "00000000-0000-0000-0000-000000000202";
    const std::string payload1 = "payload-before-rotation";
    const std::string payload2 = "payload-after-rotation";

    // Write object with initial KEK
    client().put(id1, ToBytes(payload1));

    // Rotate KEK
    client().rotate();

    // Write object with new active KEK
    client().put(id2, ToBytes(payload2));

    // Destroy and recreate client (simulates restart, loads keyring from file)
    client_.reset();
    client_ = std::make_unique<StorageClient>(connStr_);
    client_->init();

    // Both objects must be readable
    EXPECT_EQ(ToString(client().get(id1)), payload1);
    EXPECT_EQ(ToString(client().get(id2)), payload2);
}

TEST_F(StorageClientTest, RapidConsecutiveRotationsPreserveReadability) {
    const std::string id1 = "00000000-0000-0000-0000-000000000301";
    const std::string id2 = "00000000-0000-0000-0000-000000000302";
    const std::string id3 = "00000000-0000-0000-0000-000000000303";

    const std::string payload1 = "payload-before-any-rotation";
    const std::string payload2 = "payload-after-first-rotation";
    const std::string payload3 = "payload-after-second-rotation";

    client().put(id1, ToBytes(payload1));
    client().rotate();
    client().put(id2, ToBytes(payload2));

    client().rotate();
    client().put(id3, ToBytes(payload3));

    // Restart client and verify all generations remain readable.
    client_.reset();
    client_ = std::make_unique<StorageClient>(connStr_);
    client_->init();

    EXPECT_EQ(ToString(client().get(id1)), payload1);
    EXPECT_EQ(ToString(client().get(id2)), payload2);
    EXPECT_EQ(ToString(client().get(id3)), payload3);
}

// =============================================================================
// Stress / Concurrency Tests
// =============================================================================

// Helper: create N independent StorageClient instances, each with a unique KEK
// file. Must be called from the main (test) thread because setenv is not
// thread-safe.
struct MultiClientEnv {
    std::string connStr;
    std::vector<std::unique_ptr<StorageClient>> clients;

    MultiClientEnv(const std::string& conn, int count) : connStr(conn) {
        for (int i = 0; i < count; ++i) {
            clients.push_back(std::make_unique<StorageClient>(connStr));
            clients.back()->init();
        }
    }

    ~MultiClientEnv() = default;

    MultiClientEnv(const MultiClientEnv&) = delete;
    MultiClientEnv& operator=(const MultiClientEnv&) = delete;
};

// ---------------------------------------------------------------------------
// T1: Race conditions — repeated PUT/GET under load
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, ConcurrentWriteReadRoundTrip) {
    constexpr int kNumClients = 8;

    MultiClientEnv env(connStr_, kNumClients);

    std::vector<std::string> ids(kNumClients);
    std::vector<std::string> payloads(kNumClients);
    for (int i = 0; i < kNumClients; ++i) {
        // IDs: 00000000-0000-0000-0000-000000000401 .. 0408
        char id_buf[48];
        snprintf(id_buf, sizeof(id_buf),
                 "00000000-0000-0000-0000-0000000004%02d", i + 1);
        ids[i] = id_buf;
        payloads[i] = "stress-t1-" + std::to_string(i);
    }

    std::vector<Bytes> results(kNumClients);
    std::atomic<bool> any_failure{false};

    std::vector<std::thread> threads;
    threads.reserve(kNumClients);
    for (int i = 0; i < kNumClients; ++i) {
        threads.emplace_back([&, i]() {
            try {
                env.clients[i]->put(ids[i], ToBytes(payloads[i]));
                results[i] = env.clients[i]->get(ids[i]);
            } catch (...) {
                any_failure.store(true);
            }
        });
    }
    for (auto& t : threads) t.join();

    ASSERT_FALSE(any_failure.load()) << "At least one thread threw an exception";

    for (int i = 0; i < kNumClients; ++i) {
        EXPECT_EQ(ToString(results[i]), payloads[i])
            << "Mismatch for client " << i;
    }
}

// ---------------------------------------------------------------------------
// T2: Partial failure — missing/unavailable node (shard deleted)
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, GetSucceedsWithOneShardDeleted) {
    const std::string object_id = "00000000-0000-0000-0000-000000000410";
    const std::string original = "erasure-tolerance-test!";

    client().put(object_id, ToBytes(original));

    // Look up the actual shard location for shard 0
    const auto items = client().list();
    const ObjectMetadata* meta = FindObjectMetadata(items, object_id);
    ASSERT_NE(meta, nullptr);
    ASSERT_GE(meta->shard_locations.size(), 1u);
    // shard_locations[0] is "host:port/location" — extract path after '/'
    const std::string& loc0 = meta->shard_locations[0];
    const std::string shard_path = loc0.substr(loc0.find('/') + 1);

    // Directly delete shard 0 from node on port 9001
    {
        httplib::Client node0("localhost", 9001);
        auto res = node0.Delete("/shards/" + shard_path);
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
    }

    // GET must still succeed — k=2 shards from nodes 9002 and 9003 suffice
    Bytes restored;
    ASSERT_NO_THROW(restored = client().get(object_id));
    EXPECT_EQ(ToString(restored), original);
}

// ---------------------------------------------------------------------------
// T3: Aggregation bugs — large payload (max size) reconstruction
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, ConcurrentMaxSizePayloadFidelity) {
    constexpr int kNumClients = 6;
    constexpr std::size_t kMaxPayload = 40; // kDataShards(2) * kShardSize(20)

    MultiClientEnv env(connStr_, kNumClients);

    std::vector<std::string> ids(kNumClients);
    std::vector<Bytes> payloads(kNumClients);
    for (int i = 0; i < kNumClients; ++i) {
        char id_buf[48];
        snprintf(id_buf, sizeof(id_buf),
                 "00000000-0000-0000-0000-0000000004%02d", 20 + i);
        ids[i] = id_buf;
        // Fill 40 bytes with repeating byte value i
        payloads[i] = Bytes(kMaxPayload, static_cast<uint8_t>(i));
    }

    // Concurrent writes
    std::atomic<bool> write_fail{false};
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < kNumClients; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    env.clients[i]->put(ids[i], payloads[i]);
                } catch (...) {
                    write_fail.store(true);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    ASSERT_FALSE(write_fail.load());

    // Concurrent reads
    std::vector<Bytes> results(kNumClients);
    std::atomic<bool> read_fail{false};
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < kNumClients; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    results[i] = env.clients[i]->get(ids[i]);
                } catch (...) {
                    read_fail.store(true);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    ASSERT_FALSE(read_fail.load());

    for (int i = 0; i < kNumClients; ++i) {
        ASSERT_EQ(results[i].size(), kMaxPayload)
            << "Wrong size for client " << i;
        // Check shard-boundary bytes (byte 19 = end of shard 0, byte 20 = start of shard 1)
        EXPECT_EQ(results[i][0], static_cast<uint8_t>(i));
        EXPECT_EQ(results[i][19], static_cast<uint8_t>(i));
        EXPECT_EQ(results[i][20], static_cast<uint8_t>(i));
        EXPECT_EQ(results[i][39], static_cast<uint8_t>(i));
        EXPECT_EQ(results[i], payloads[i])
            << "Full byte mismatch for client " << i;
    }
}

// ---------------------------------------------------------------------------
// T4: DB consistency — many objects written/read concurrently
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, HighVolumeWriteListConsistency) {
    constexpr int kNumClients = 4;
    constexpr int kObjectsPerClient = 5;
    constexpr int kTotalObjects = kNumClients * kObjectsPerClient;

    MultiClientEnv env(connStr_, kNumClients);

    // Prepare IDs and payloads: range 0501..0520
    std::vector<std::vector<std::string>> ids(kNumClients);
    std::vector<std::vector<std::string>> payloads(kNumClients);
    int counter = 1;
    for (int c = 0; c < kNumClients; ++c) {
        for (int o = 0; o < kObjectsPerClient; ++o) {
            char id_buf[48];
            snprintf(id_buf, sizeof(id_buf),
                     "00000000-0000-0000-0000-0000000005%02d", counter);
            ids[c].push_back(id_buf);
            payloads[c].push_back("vol-" + std::to_string(counter));
            ++counter;
        }
    }

    // Concurrent writes: each client writes its 5 objects sequentially
    std::atomic<bool> write_fail{false};
    {
        std::vector<std::thread> threads;
        for (int c = 0; c < kNumClients; ++c) {
            threads.emplace_back([&, c]() {
                try {
                    for (int o = 0; o < kObjectsPerClient; ++o) {
                        env.clients[c]->put(ids[c][o], ToBytes(payloads[c][o]));
                    }
                } catch (...) {
                    write_fail.store(true);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    ASSERT_FALSE(write_fail.load());

    // Fresh client for verification (new DB connection)
    std::string verify_kek = "/tmp/pqdos_stress_verify_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".json";
    setenv("PQDOS_KEYSTORE_PATH", verify_kek.c_str(), 1);
    StorageClient verify_client(connStr_);
    verify_client.init();
    setenv("PQDOS_KEYSTORE_PATH", kek_path_.c_str(), 1);

    // list() must return all 20 objects
    const auto items = verify_client.list();
    EXPECT_EQ(items.size(), static_cast<std::size_t>(kTotalObjects));

    // Each object must be individually readable with correct content
    for (int c = 0; c < kNumClients; ++c) {
        for (int o = 0; o < kObjectsPerClient; ++o) {
            EXPECT_TRUE(ContainsObjectId(items, ids[c][o]));
            Bytes got = env.clients[c]->get(ids[c][o]);
            EXPECT_EQ(ToString(got), payloads[c][o]);
        }
    }

    // Remove half the objects (client 0 and client 1's objects)
    for (int c = 0; c < 2; ++c) {
        for (int o = 0; o < kObjectsPerClient; ++o) {
            EXPECT_TRUE(env.clients[c]->remove(ids[c][o]));
        }
    }

    const auto remaining = verify_client.list();
    EXPECT_EQ(remaining.size(), static_cast<std::size_t>(kTotalObjects / 2));

    std::filesystem::remove(verify_kek);
}

// ---------------------------------------------------------------------------
// T5: Timing / non-blocking I/O — slow node does not block fast nodes
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, GetCompletesWithMissingShardInBoundedTime) {
    const std::string object_id = "00000000-0000-0000-0000-000000000430";
    const std::string original = "timing-test-payload-abc";

    client().put(object_id, ToBytes(original));

    // Look up the actual shard location for shard 2
    const auto items = client().list();
    const ObjectMetadata* meta = FindObjectMetadata(items, object_id);
    ASSERT_NE(meta, nullptr);
    ASSERT_GE(meta->shard_locations.size(), 3u);
    // shard_locations[2] is "host:port/location" — extract path after '/'
    const std::string& loc2 = meta->shard_locations[2];
    const std::string shard_path = loc2.substr(loc2.find('/') + 1);

    // Delete shard 2 from node on port 9003 — simulates unavailable shard
    {
        httplib::Client node2("localhost", 9003);
        auto res = node2.Delete("/shards/" + shard_path);
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
    }

    // Time the GET — parallel fetch should not be blocked by the missing shard
    auto start = std::chrono::steady_clock::now();
    Bytes restored;
    ASSERT_NO_THROW(restored = client().get(object_id));
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Correctness first
    EXPECT_EQ(ToString(restored), original);

    // Timing: must complete well under 3 seconds on loopback.
    // A sequential implementation with per-node timeouts would exceed this.
    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "GET took too long — parallel dispatch may be broken";
}

// ---------------------------------------------------------------------------
// T6: Race conditions + aggregation — concurrent overwrite same object
// ---------------------------------------------------------------------------
TEST_F(StorageClientTest, ConcurrentOverwriteSameObjectYieldsConsistentState) {
    constexpr int kNumClients = 4;
    const std::string shared_id = "00000000-0000-0000-0000-000000000601";

    MultiClientEnv env(connStr_, kNumClients);

    // Each client writes a distinct 12-byte payload to the SAME object ID
    std::vector<std::string> candidate_payloads = {
        "aaaaaaaaaaaa",
        "bbbbbbbbbbbb",
        "cccccccccccc",
        "dddddddddddd"
    };

    // Concurrent overwrites
    std::atomic<bool> write_fail{false};
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < kNumClients; ++i) {
            threads.emplace_back([&, i]() {
                try {
                    env.clients[i]->put(shared_id, ToBytes(candidate_payloads[i]));
                } catch (...) {
                    write_fail.store(true);
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    ASSERT_FALSE(write_fail.load());

    // The system must reach a consistent readable state.
    // If metadata from client A is stored but shards from client B won the
    // LMDB write race, the DEK will not decrypt correctly — this surfaces
    // the known atomicity gap (ADR-0004 / ADR-0005).
    Bytes result;
    ASSERT_NO_THROW(result = client().get(shared_id))
        << "GET threw — possible shard/metadata atomicity mismatch";

    // The result must be exactly one of the four candidates (no garbled mix)
    const std::string result_str = ToString(result);
    bool matches_one = std::any_of(
        candidate_payloads.begin(), candidate_payloads.end(),
        [&](const std::string& p) { return p == result_str; }
    );
    EXPECT_TRUE(matches_one)
        << "Result '" << result_str << "' does not match any candidate payload";

    // Metadata must show exactly 1 entry for this ID
    const auto items = client().list();
    int count = std::count_if(items.begin(), items.end(),
        [&](const ObjectMetadata& m) { return m.id == shared_id; });
    EXPECT_EQ(count, 1);
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}