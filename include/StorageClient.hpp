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

// Outcome of rebalancing a single object toward its HRW-intended placement.
enum class RebalanceStatus {
    Balanced,         // already on intended nodes; nothing to move
    Moved,            // one or more shards relocated and committed
    DryRun,           // misplaced shards detected but no move performed (dry run)
    SkippedDegraded,  // an object shard is missing/unreachable; repair must heal first
    SkippedUnsafe,    // fewer than k+m healthy nodes; moving is unsafe
    SkippedConflict,  // metadata version changed concurrently; commit fenced off
    SkippedNotFound,  // object metadata does not exist
    Errored           // a transport/store error aborted this object's rebalance
};

// Policy bounding a single object's rebalance.
struct RebalancePolicy {
    bool dry_run = false;      // when true, plan only - never write or commit
    std::size_t max_moves = 0; // 0 = unlimited; caps shards moved for this object
};

// Scope + policy bounding a cluster-wide rebalance pass.
struct RebalanceScope {
    std::vector<std::string> object_ids;  // empty = every object in the catalog
    std::size_t max_objects = 0;          // 0 = unlimited objects processed
    std::size_t max_moves = 0;            // 0 = unlimited total shards moved
    bool dry_run = false;                 // when true, plan only - never write or commit
};

// Result of rebalancing a single object.
struct RebalanceObjectResult {
    std::string object_id;
    RebalanceStatus status = RebalanceStatus::Balanced;
    std::size_t shards_total = 0;      // k + m
    std::size_t shards_misplaced = 0;  // shards not on their HRW-intended node
    std::size_t shards_moved = 0;      // shards actually relocated (0 on dry run/skip)
    std::string detail;                // human-readable explanation
};

// Aggregate summary of a cluster-wide rebalance pass. Counts are per invocation.
struct RebalanceReport {
    std::size_t objects_scanned = 0;
    std::size_t objects_balanced = 0;          // already on intended placement
    std::size_t objects_moved = 0;             // at least one shard relocated
    std::size_t objects_skipped_degraded = 0;
    std::size_t objects_skipped_unsafe = 0;
    std::size_t objects_skipped_conflict = 0;
    std::size_t objects_not_found = 0;
    std::size_t objects_errored = 0;           // aborted by a transport/store error
    std::size_t shards_moved = 0;              // total shards relocated across objects
    bool dry_run = false;
    std::vector<RebalanceObjectResult> results;// per-object detail
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

    // Move a single object's shards toward their HRW-intended placement, reusing
    // the version-fenced commit that repair() relies on (conditional_put with the
    // version read at the start). Shards are copied to their intended node before
    // the metadata commit, so durability is never reduced; on a version conflict
    // the commit is fenced off and no metadata changes. Old shard copies are left
    // as inert orphans for a future GC sweep to reclaim (ADR-0006/0008). Objects
    // that are degraded (a shard missing/unreachable) are skipped so repair heals
    // them first. Safe to call concurrently with put/get/remove/repair.
    RebalanceObjectResult rebalance_object(
        const std::string& object_id,
        const RebalancePolicy& policy = {}
    );

    // Rebalance a bounded set of objects toward HRW-intended placement. Iterates
    // the objects named in the scope (or the whole catalog when none are named),
    // honouring the max_objects and max_moves budgets, and aggregates a report.
    RebalanceReport rebalance(const RebalanceScope& scope = {});

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
