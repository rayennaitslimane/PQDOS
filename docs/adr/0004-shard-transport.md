# ADR-0004: Shard Transport

**Status:** Accepted (amended)  
**Date:** 2026-06-11  
**Amended:** 2026-06-22

## Context

Encrypted shards must be distributed to and retrieved from independent storage nodes. The transport layer must:
- Map each shard to the correct node address deterministically
- Be resilient to individual node failures on reads (erasure coding tolerates missing shards)
- Be simple enough to verify correctness in integration tests without a production-grade scheduler
- Remain easy to reason about for a POC where correctness takes priority over throughput

The primary alternatives considered were synchronous sequential HTTP calls, async/parallel HTTP calls, and a message queue. A message queue introduces operational complexity (a broker) that is out of scope for this milestone.

## Decision

### Placement strategy: dynamic node registry

~~The original placement algorithm assigned shard `i` to a static `kNodeAddresses[i]` array parsed from the `NODE_ADDRESSES` environment variable at process start.~~

**Current implementation:** Node addresses are stored in a PostgreSQL `nodes` table managed by `MetadataStore`. The placement algorithm selects the first `k + m` eligible nodes (ordered by `registered_at`) and assigns shard `i` to `eligible_nodes[i]`:

```cpp
PlacementMap placement_strategy(
    const std::string& object_id,
    const std::string& version,
    const std::vector<Bytes>& serialized_shards,
    const std::vector<std::string>& eligible_nodes
) {
    for (std::size_t i = 0; i < total_shards; ++i) {
        placement[eligible_nodes[i]].push_back({location, serialized_shards[i]});
    }
    return placement;
}
```

Nodes can be added and removed at runtime via `MetadataStore::register_node()` / `unregister_node()`. `StorageClient::put()` validates that `eligible_nodes.size() >= k + m` before proceeding.

For `StorageClient::repair()`, candidate targets are the **healthy subset** of registered nodes at repair time (HTTP `GET /health` with the standard transport timeouts). Down-but-registered nodes are excluded from repair placement to avoid failing the repair write path due only to stale registry entries. Repair still prefers unused healthy nodes first, then reuses healthy nodes already holding surviving shards if needed.

### Write atomicity model: versioned shard keyspace

Each `put` generates a new random `version` token and writes shards under keys of the form:

`object_id/version/shard_index`

This prevents concurrent writers for the same `object_id` from clobbering each other's shard payloads.

Per [ADR-0006](0006-concurrency-contract.md), concurrent same-object operations are intentionally not globally ordered. Versioning prevents key clobbering at the shard store level, but metadata remains last-writer-wins and `put/remove` interleavings may still leave orphaned shard payloads.

Write ordering is:
1. Encode and encrypt shards.
2. PUT all versioned shard payloads in parallel (`apply_placement`).
3. Upsert metadata to reference only that written version.

If shard placement fails, metadata is not updated. This keeps metadata references aligned with successfully written shard sets.

### Transport: parallel HTTP via cpp-httplib using std::async

All node communication is performed over HTTP/1.1 using **cpp-httplib** (`httplib::Client`). Shards are PUT/GET/DELETE using `/shards?location=<shard_key>` with `application/octet-stream` content type for the payload.

**Parallelism granularity:** Each node gets one independent async task (`std::async(std::launch::async, ...)`). Within a single node's task, shards are dispatched sequentially over one `httplib::Client` connection.

**Client lifecycle (hot path):** The transport uses a thread-local client cache keyed by `node_address`:

```cpp
thread_local std::unordered_map<std::string, std::unique_ptr<httplib::Client>> clients;
```

`get_node_client(node_address)` lazily creates and configures a client on first use in that thread and reuses it on subsequent calls in the same thread. This yields **one client per thread per node** with no cross-thread sharing and no locking.

On writes (`apply_placement`), one async task is launched per node address and uses the thread-local client accessor:

```cpp
std::vector<std::future<void>> futures;
for (const auto& [node_address, entries] : placement) {
    futures.push_back(std::async(std::launch::async, [node_address, &entries]() {
        auto& client = get_node_client(node_address);
        for (const auto& [location, payload] : entries) {
            auto res = client.Put("/shards?location=" + location, ...);
            if (!res || res->status != 200) {
                throw std::runtime_error("PUT failed");
            }
        }
    }));
}
for (auto& f : futures) { f.get(); }  // wait for all to complete
```

On reads (`fetch_encrypted_shards`), one async task per node collects its shards and returns them; results are merged after all futures resolve. A node that returns a non-200 status or throws is **silently skipped** - the missing shard is acceptable as long as at least `k` shards are recovered (erasure tolerance).

On deletes (`remove_from_nodes`), one async task per node performs best-effort deletions; all errors are swallowed.

### Latency improvement: parallel dispatch

Parallel dispatch using `std::async` achieves the latency profile:

| Operation | Latency | Improvement |
|-----------|---------|-------------|
| `put` | `max(RTT₀, RTT₁, RTT₂)` | 3× vs. sequential |
| `get` | `max(RTT₀, RTT₁, RTT₂)` in the healthy case | up to 3× vs. sequential |
| `delete` | `max(RTT₀, RTT₁, RTT₂)` | 3× vs. sequential |

For 3 nodes with equal latencies, this eliminates ~67% of transport time compared to sequential dispatch. Individual node failures remain transparent to the caller (erasure tolerance on read, best-effort on delete).

### Connection reuse and remaining limits

Hot-path transport calls now reuse clients across operations on the same thread (`apply_placement`, `fetch_encrypted_shards`, `remove_from_nodes`) via thread-local per-node caching. This removes repeated client construction overhead on those paths, especially for small payloads (`kShardSize = 20`). The health-probe and startup paths (`probe_shard`, `is_node_healthy`, and `StorageClient::init()`) share the same `get_node_client` cache, so repair and init no longer rebuild a client on every probe.

Remaining limits:
- The cache has no explicit eviction; entries live until thread exit.

### HTTP timeouts

All `httplib::Client` instances are configured with timeouts to prevent indefinite blocking on unresponsive nodes:

```cpp
client.set_connection_timeout(5, 0);  // 5 seconds
client.set_read_timeout(10, 0);       // 10 seconds
```

These timeouts are applied wherever a client is created. Because shard transport, the health probes (`probe_shard`, `is_node_healthy`), and the `init()` health check loop all obtain clients through `get_node_client`, the timeouts are configured once per thread-local client and reused on every subsequent call.

## Consequences

**Positive:**
- Parallel dispatch via `std::async` uses the language's built-in concurrency primitives - no extra thread pool or queue library required.
- Each node task maintains sequential shard dispatch over a single TCP connection, avoiding per-shard thread overhead.
- Thread-local per-node client reuse reduces repeated client setup on hot paths without introducing locks.
- One client per thread per node avoids cross-thread sharing and matches cpp-httplib safety expectations.
- Versioned shard keys eliminate shard-key overwrite races between concurrent writers of the same object ID.
- Exception propagation through futures preserves error semantics: PUT failures surface immediately; GET/DELETE failures are swallowed (matching erasure tolerance and best-effort semantics).
- Missing-shard tolerance on reads is correctly implemented: individual node failures do not surface as errors as long as `k` shards are available.
- Best-effort delete is the right semantic - a failed delete does not corrupt read availability, it only leaks storage.

**Remaining trade-offs:**
- Thread-local client caches do not evict entries; a long-lived worker thread that touches many unique node addresses retains those clients until thread exit.
- Versioned writes can leave orphaned shard payloads for write attempts that lose the metadata upsert race, and for same-object `put/remove` interleavings allowed by ADR-0006.
- Dynamic placement selects the first `k + m` nodes ordered by registration time. No consistent hashing or rebalancing is performed when nodes are added/removed; existing shard locations are not migrated.
- Repair requires a sufficient number of healthy registered nodes for replacement targets. If too few healthy nodes are available, repair fails before metadata update.
