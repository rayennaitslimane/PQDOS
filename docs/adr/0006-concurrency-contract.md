# ADR-0006: Concurrency Contract

**Status:** Accepted  
**Date:** 2026-06-12

## Context

`StorageClientServer` and `StorageNodeServer` both embed a `httplib::Server` (see ADR-0005), which dispatches incoming requests from an internal thread pool. This means every public method of `StorageClient`, `MetadataStore`, and `StorageNode` can be invoked concurrently by multiple handler threads.

Prior to this decision, two concrete data races existed:

1. **`pqxx::connection` shared across threads** - `MetadataStore` holds a single `pqxx::connection conn_`. libpqxx explicitly documents the connection as non-thread-safe: even logically read-only operations mutate internal socket and buffer state. Concurrent calls to `put`, `get`, `remove`, or `list` from the thread pool result in undefined behaviour.

2. **`kek_ring_` and `active_kek_id_` read/write race** - `StorageClient::rotate()` inserts into `kek_ring_` and reassigns `active_kek_id_` without synchronisation. Concurrent `put()` or `get()` reading these fields while `rotate()` mutates them is a data race under the C++ memory model.

`StorageNode`'s LMDB backend does not have this problem: LMDB enforces single-writer / multiple-reader semantics internally and is safe to call from concurrent threads sharing the same `MDB_env*`.

The fix must:
- Eliminate both data races without rewriting the architecture
- Preserve the single-connection model for `MetadataStore` (no connection pool)
- Not hold any lock across HTTP I/O (shard dispatch, node fetches) or cryptographic computation
- Not centralise concurrency coordination across components

## Decision

### 1. `MetadataStore`: per-instance mutex serialising all operations

A `std::mutex mu_` is added as a private member of `MetadataStore`. Every public method acquires a `std::lock_guard<std::mutex>` at entry, held for the full duration of the method including the `pqxx` transaction commit.

```cpp
void MetadataStore::put(const ObjectMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mu_);
    // ... pqxx::work tx{conn_}; ...
}
```

This serialises all access to the single `pqxx::connection`, matching the one-transaction-at-a-time constraint imposed by the library. Parallelism across different `MetadataStore` instances (i.e., different `StorageClient` processes) is unaffected.

A `std::shared_mutex` was considered but rejected: `pqxx` mutates connection state even inside `pqxx::read_transaction`, so shared-reader semantics would not be correct here.

### 2. `StorageClient`: `std::shared_mutex` guarding the KEK ring

A `mutable std::shared_mutex kek_mu_` is added to `StorageClient`. Reads of `kek_ring_` and `active_kek_id_` in `put()` and `get()` acquire a `std::shared_lock` (allowing concurrent readers). `rotate()` acquires a `std::unique_lock` (exclusive) for its full body.

The shared lock in `put()` is scoped narrowly around the single `encrypt_dek` call - it is **not** held during erasure encoding, shard encryption, or HTTP dispatch:

```cpp
Bytes wrapped_dek;
{
    std::shared_lock<std::shared_mutex> kek_lock(kek_mu_);
    wrapped_dek = encrypt_dek(dek, *kek_ring_.at(active_kek_id_)->public_key());
}
// ... apply_placement(placement); -- no lock held here ...
```

Likewise in `get()`, the shared lock covers only the KEK lookup loop through `decrypt_dek`, releasing before erasure decoding and checksum verification.

### 3. `StorageNode`: no application-level locking

LMDB's internal MVCC handles concurrent readers and serialises writers. No mutex is added to `StorageNode`.

### 4. Per-object ordering is NOT guaranteed (explicitly tolerated)

Concurrent mutations on the **same** object ID (e.g., two simultaneous `put(X)` calls, or a `put(X)` racing with `remove(X)`) are **not** serialised by this contract. The decision is to tolerate these races rather than introduce per-object locking, for the following reasons:

- Neither race causes undefined behaviour: the MetadataStore mutex prevents UB on the `pqxx::connection`, and the Postgres `ON CONFLICT … DO UPDATE` upsert is atomic per-row.
- Orphaned shards (shards written to nodes but whose metadata is subsequently overwritten or deleted) are inert encrypted blobs and cause no correctness problem for other objects.
- Per-object locking would require a lifetime-managed map of per-ID mutexes, adding complexity and a new failure mode (unbounded lock map growth) for a scenario that real callers are expected to avoid.

The caller is responsible for not issuing concurrent conflicting mutations on the same object ID.

## Concurrency Contract

The following invariants hold under the implementation described above:

| # | Invariant |
|---|-----------|
| 1 | **MetadataStore serialization** - All operations on a single `MetadataStore` instance are serialized. No two threads hold an active `pqxx` transaction on the same connection simultaneously. |
| 2 | **KEK ring coherence** - Reads of `kek_ring_` and `active_kek_id_` are mutually exclusive with writes (`rotate()`). Multiple concurrent readers are permitted. |
| 3 | **StorageNode safety** - LMDB provides internal MVCC; no application-level locking is required or applied. |
| 4 | **Per-object ordering is NOT guaranteed** - Concurrent `put()`/`remove()` on the same object ID may interleave arbitrarily. Last metadata write wins. Orphaned shards may result. No UB occurs. |

### Not guaranteed (tolerated races - no UB)

| Scenario | Consequence | Why safe |
|----------|-------------|----------|
| Concurrent `put(X)` / `remove(X)` | New shards written but metadata deleted, leaving orphans | No UB; shards are inert encrypted data; a future GC sweep can reclaim them |
| Concurrent `put(X)` / `put(X)` | Last metadata UPSERT wins; first writer's shards are orphaned | No UB; Postgres UPSERT is atomic per-row; both shard sets are valid encrypted data |
| `get(X)` during `put(X)` | May read old or new metadata (Postgres snapshot isolation) | No UB; either version is self-consistent and fully decryptable |
| Stale KEK read during `rotate()` | A `put()` that read `active_kek_id_` before the exclusive lock is taken encrypts with the previous KEK | Correct - the old KEK is retained in the ring; the object's `kek_id` field records which KEK was used |
| Partial shard write followed by crash | Some shards stored on nodes, metadata never written | No UB; the incomplete object is invisible (no metadata entry); shards are orphans |
| Multi-process access to the same database | Not coordinated | Out of scope - single-process deployment is assumed; the keystore file and `pqxx::connection` are both process-local |
| Concurrent `repair(X)` / `repair(X)` | Exactly one succeeds; the other detects version mismatch and returns false | No UB; optimistic version fencing ensures only one commit wins; the losing repair's shards become orphans |
| Concurrent `repair(X)` / `put(X)` | `repair()` aborts if `put()` commits first (version changed); if `repair()` commits first, `put()` overwrites with a new version | No UB; `repair()` uses conditional_put which checks version; `put()` uses unconditional upsert that increments version |
| Concurrent `repair(X)` / `remove(X)` | `repair()` aborts if metadata is deleted before commit; repaired shards become orphans | No UB; conditional_put fails gracefully; repaired shards are inert encrypted data |

### 5. Repair: optimistic metadata version fencing

`StorageClient::repair()` is a conflicting mutation on the same object. It is resolved through optimistic metadata version fencing:

1. At repair start, the current metadata `version` is read.
2. After reconstructing and placing repaired shards, the metadata commit uses `conditional_put(metadata, expected_version)` - a SQL `UPDATE ... WHERE version = $expected` that atomically increments the version.
3. If any concurrent mutation (`put`, `remove`, or another `repair`) has incremented the version in the meantime, `conditional_put` affects zero rows and returns `false`.
4. The repair caller receives `false` and may retry. The already-written repaired shards are orphaned (inert encrypted data).

Repair target liveness filtering is orthogonal to version fencing: `repair()` first filters registered nodes by `/health` and only writes replacements to healthy targets. A repair returns `false` only for version-mismatch conflicts; transport/liveness insufficiency remains an exception path.

This ensures that repair never silently overwrites a concurrent mutation's metadata.

## Consequences

**Positive:**
- Both data races (UB) are eliminated with approximately 20 lines of new synchronisation code and no architectural changes.
- The MetadataStore mutex is scoped to a single instance; different `StorageClient` processes connecting to the same PostgreSQL database are not affected.
- The narrow scope of the `shared_lock` in `put()` and `get()` (covering only the KEK map lookup) means the KEK ring lock is never held during HTTP I/O or cryptographic computation - contention is minimal.
- The concurrency contract is explicit and testable: `SameClientConcurrentAccess`, `ConcurrentRotateWithReadWrite`, `ThreadSanitizerCleanRun`, and `ConcurrentMetadataStoreAccessOnSingleClient` validate each invariant.
- Repair operations are safely composable with other mutations via version fencing - no distributed locks required.

**Negative / Risks:**
- Callers that issue concurrent mutations on the same object ID will observe last-writer-wins semantics and may produce orphaned shards. There is currently no GC sweep to reclaim them.
- The single-connection model for `MetadataStore` means all metadata operations are serialised through one mutex. Under high concurrent load this becomes a bottleneck. A connection pool with one connection per thread is the natural next step if throughput becomes a constraint.
- Multi-process deployments (e.g., multiple replicas sharing one PostgreSQL instance) are not safe under this contract. They would require distributed locking (e.g., Postgres advisory locks) or a leader-election mechanism.
- `rotate()` holds an exclusive lock for its full body including the `save_kek_file` disk write. Under normal conditions this is fast, but on a slow filesystem it temporarily blocks all `put()` and `get()` operations.
- A failed `repair()` leaves orphaned repaired shards on nodes. These are inert encrypted data and will be reclaimed by a future GC sweep.
