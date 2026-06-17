#pragma once

#include "Models.hpp"

#include <pqxx/pqxx>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class MetadataStore {
public:
    explicit MetadataStore(const std::string& conn_str);

    void put(const ObjectMetadata& metadata);

    bool conditional_put(const ObjectMetadata& metadata, int expected_version);

    std::optional<ObjectMetadata> get(const std::string& id);

    bool remove(const std::string& id);

    std::vector<ObjectMetadata> list();

    void register_node(const std::string& address);

    bool unregister_node(const std::string& address);

    std::vector<std::string> list_nodes();

private:
    pqxx::connection conn_;
    std::mutex mu_;

    void init_schema();

    void prepare_statements();

    static ObjectMetadata row_to_object_metadata(const pqxx::row_ref& row);

    static std::vector<std::string> get_shard_locations(
        pqxx::transaction_base& tx,
        const std::string& object_id
    );

    static std::int64_t checked_size_to_i64(std::size_t value);

    static void validate_metadata(const ObjectMetadata& metadata);
};