#pragma once

// Concurrency contract:
// 1. MetadataStore access is serialized (per-instance mutex on pqxx::connection).
// 2. KEK ring reads (put/get) are shared-locked; writes (rotate) are exclusive-locked.
// 3. StorageNode requires no application-level locking (LMDB handles it).
// 4. Per-object ordering is NOT guaranteed. Concurrent mutations on the same object
//    may interleave; last metadata write wins. Orphaned shards are tolerated.

#include "MetadataStore.hpp"
#include "Models.hpp"

#include <botan/pk_keys.h>

#include <array>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class StorageClient {
public:
    explicit StorageClient(const std::string& metadata_conn_str);
    ~StorageClient();

    void init();
    void put(const std::string& object_id, const Bytes& bytes, const ErasureSpec& erasure_spec);
    Bytes get(const std::string& object_id);
    bool remove(const std::string& object_id);
    std::vector<ObjectMetadata> list();
    void rotate();

    StorageClient(const StorageClient&) = delete;
    StorageClient& operator=(const StorageClient&) = delete;

    StorageClient(StorageClient&&) = delete;
    StorageClient& operator=(StorageClient&&) = delete;

private:
    MetadataStore metadata_store_;
    mutable std::shared_mutex kek_mu_;
    std::unordered_map<std::string, std::unique_ptr<Botan::Private_Key>> kek_ring_;
    std::string active_kek_id_;
    std::string kek_file_path_;
};
