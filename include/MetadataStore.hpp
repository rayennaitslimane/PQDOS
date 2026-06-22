#pragma once

#include "Models.hpp"

#include <pqxx/pqxx>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

class MetadataStore {
public:
    explicit MetadataStore(const std::string& conn_str, std::size_t pool_size = 4);

    void put(const ObjectMetadata& metadata);

    bool conditional_put(const ObjectMetadata& metadata, int expected_version);

    std::optional<ObjectMetadata> get(const std::string& id);

    bool remove(const std::string& id);

    std::vector<ObjectMetadata> list();

    void register_node(const std::string& address);

    bool unregister_node(const std::string& address);

    std::vector<std::string> list_nodes();

private:
    // Bounded pool of libpqxx connections. A pqxx::connection is not thread-safe
    // even for read-only work (ADR-0006), so each connection is checked out to
    // exactly one thread at a time via the free list below. Different
    // connections execute transactions concurrently, replacing the previous
    // single-connection + global-mutex serialization.
    std::vector<std::unique_ptr<pqxx::connection>> connections_;
    std::queue<pqxx::connection*> available_;
    std::mutex pool_mu_;
    std::condition_variable pool_cv_;

    // RAII checkout handle: pops a connection on construction (blocking until one
    // is free) and returns it to the free list on destruction.
    class PooledConnection {
    public:
        PooledConnection(MetadataStore& store, pqxx::connection* conn)
            : store_(store), conn_(conn) {}

        ~PooledConnection() { store_.release(conn_); }

        PooledConnection(const PooledConnection&) = delete;
        PooledConnection& operator=(const PooledConnection&) = delete;

        pqxx::connection& operator*() const { return *conn_; }

    private:
        MetadataStore& store_;
        pqxx::connection* conn_;
    };

    PooledConnection acquire();

    void release(pqxx::connection* conn);

    static void init_schema(pqxx::connection& conn);

    static void prepare_statements(pqxx::connection& conn);

    static ObjectMetadata row_to_object_metadata(const pqxx::row_ref& row);

    static std::vector<std::string> get_shard_locations(
        pqxx::transaction_base& tx,
        const std::string& object_id
    );

    static std::int64_t checked_size_to_i64(std::size_t value);

    static void validate_metadata(const ObjectMetadata& metadata);
};