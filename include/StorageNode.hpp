#pragma once

#include "Models.hpp"

#include <lmdb.h>

#include <optional>
#include <string>
#include <vector>

class StorageNode {
private:
    MDB_env* env_ = nullptr;

    static void check_lmdb(int rc, const char* operation);

    static Bytes bytes_from_mdb_value(const MDB_val& value);

    MDB_dbi open_database(MDB_txn* txn);

public:
    explicit StorageNode(const char* path);

    ~StorageNode();

    StorageNode(const StorageNode&) = delete;
    StorageNode& operator=(const StorageNode&) = delete;

    StorageNode(StorageNode&&) = delete;
    StorageNode& operator=(StorageNode&&) = delete;

    void put(
        const std::vector<std::string>& locations,
        const std::vector<Bytes>& payloads
    );

    std::vector<std::optional<Bytes>> get(
        const std::vector<std::string>& locations
    );

    std::vector<std::optional<Bytes>> remove(
        const std::vector<std::string>& locations
    );
};
