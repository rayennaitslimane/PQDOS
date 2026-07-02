#pragma once

// Concurrency contract:
// 1. MetadataStore access uses a bounded pqxx::connection pool; each connection
//    is checked out to one thread at a time, so operations run concurrently.
// 2. KEK ring reads (put/get) are shared-locked; writes (rotate) are exclusive-locked.
// 3. StorageNode requires no application-level locking (LMDB handles it).
// 4. Per-object ordering is NOT guaranteed. Concurrent mutations on the same object
//    may interleave; last metadata write wins. Orphaned shards are tolerated.

#include "MetadataStore.hpp"
#include "MetricsCollector.hpp"
#include "Models.hpp"

#include <botan/pk_keys.h>

#include <array>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

// Summary of a metadata rebuild pass (ADR-0008). All counts are per invocation.
struct ReindexReport {
    std::size_t versions_scanned = 0;        // distinct (object_id, version) groups found
    std::size_t objects_recovered = 0;       // new catalog rows inserted
    std::size_t objects_skipped_existing = 0;// objects whose metadata already existed
    std::size_t degraded_objects = 0;        // recovered with fewer than k shards present
    std::size_t unreadable_versions = 0;     // groups whose manifest could not be fetched
    std::size_t errors = 0;                  // objects that failed to persist
};

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
    ObjectHealth health(const std::string& object_id);
    bool repair(const std::string& object_id);

    // Rebuild the PostgreSQL metadata catalog from the self-describing shards on
    // the storage nodes (ADR-0008). Scans every node's keyspace, groups shards
    // by object/version, recovers each object's manifest, and inserts any
    // missing catalog entries without clobbering existing ones. Intended as an
    // administrative disaster-recovery operation.
    ReindexReport reindex();

    MetadataStore& metadata_store() { return metadata_store_; }
    void set_collector(MetricsCollector* c) { collector_ = c; }

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
    MetricsCollector* collector_ = nullptr;
};
