# ADR-0004: Shard Transport

**Status:** Accepted  
**Date:** 2026-06-11

## Context

Encrypted shards must be distributed to and retrieved from independent storage nodes. The transport layer must:
- Map each shard to the correct node address deterministically
- Be resilient to individual node failures on reads (erasure coding tolerates missing shards)
- Be simple enough to verify correctness in integration tests without a production-grade scheduler
- Remain easy to reason about for a POC where correctness takes priority over throughput

The primary alternatives considered were synchronous sequential HTTP calls, async/parallel HTTP calls, and a message queue. A message queue introduces operational complexity (a broker) that is out of scope for this milestone.

## Decision

### Placement strategy: shard index → node index

The placement algorithm assigns shard `i` to `kNodeAddresses[i]` — a direct index mapping:

```cpp
// shard 0 → node 0, shard 1 → node 1, shard 2 → node 2
for (std::size_t i = 0; i < total_shards; ++i) {
    placement[kNodeAddresses[i]].push_back({location, serialized_shards[i]});
}
```

Node addresses are read once at static-initialisation time from the `NODE_ADDRESSES` environment variable (comma-separated, exactly `kNumNodes=3` entries), falling back to `localhost:9001,9002,9003`. The parsed array is a `const` static, so parsing happens once per process lifetime.

### Write atomicity model: versioned shard keyspace

Each `put` generates a new random `version` token and writes shards under keys of the form:

`object_id-version-shard_index`

This prevents concurrent writers for the same `object_id` from clobbering each other's shard payloads.

Write ordering is:
1. Encode and encrypt shards.
2. PUT all versioned shard payloads in parallel (`apply_placement`).
3. Upsert metadata to reference only that written version.

If shard placement fails, metadata is not updated. This keeps metadata references aligned with successfully written shard sets.

### Transport: parallel HTTP via cpp-httplib using std::async

All node communication is performed over HTTP/1.1 using **cpp-httplib** (`httplib::Client`). Shards are PUT/GET/DELETE using the path `/shards/:location` with `application/octet-stream` content type for the payload.

**Parallelism granularity:** Each node gets one independent async task (`std::async(std::launch::async, ...)`). Within a single node's task, shards are dispatched sequentially over one `httplib::Client` connection.

On writes (`apply_placement`), one async task is launched per node address:

```cpp
std::vector<std::future<void>> futures;
for (const auto& [node_address, entries] : placement) {
    futures.push_back(std::async(std::launch::async, [&node_address, &entries]() {
        auto [host, port] = parse_address(node_address);
        httplib::Client client(host, port);
        for (const auto& [location, payload] : entries) {
            auto res = client.Put("/shards/" + location, ...);
            if (!res || res->status != 200) {
                throw std::runtime_error("PUT failed");
            }
        }
    }));
}
for (auto& f : futures) { f.get(); }  // wait for all to complete
```

On reads (`fetch_encrypted_shards`), one async task per node collects its shards and returns them; results are merged after all futures resolve. A node that returns a non-200 status or throws is **silently skipped** — the missing shard is acceptable as long as at least `k` shards are recovered (erasure tolerance).

On deletes (`remove_from_nodes`), one async task per node performs best-effort deletions; all errors are swallowed.

### Latency improvement: parallel dispatch

Parallel dispatch using `std::async` achieves the latency profile:

| Operation | Latency | Improvement |
|-----------|---------|-------------|
| `put` | `max(RTT₀, RTT₁, RTT₂)` | 3× vs. sequential |
| `get` | `max` of the `k` fastest nodes | 3× vs. sequential |
| `delete` | `max(RTT₀, RTT₁, RTT₂)` | 3× vs. sequential |

For 3 nodes with equal latencies, this eliminates ~67% of transport time compared to sequential dispatch. Individual node failures remain transparent to the caller (erasure tolerance on read, best-effort on delete).

### Known latency cost: no connection pooling

A new `httplib::Client` is instantiated for every shard operation, performing a full TCP handshake each time. Persistent clients (one per node address, reused across calls) would eliminate connection setup overhead, which dominates at small payload sizes (the current `kShardSize = 20` byte shards are smaller than a TCP packet).

### No HTTP timeouts

No connection or read timeout is configured on `httplib::Client`. A single unresponsive node will block the entire put or get indefinitely. Before any network exposure, timeouts must be set:

```cpp
client.set_connection_timeout(0, 500000);  // 500 ms
client.set_read_timeout(1, 0);             // 1 s
```

## Consequences

**Positive:**
- Parallel dispatch via `std::async` uses the language's built-in concurrency primitives — no extra thread pool or queue library required.
- Each node task maintains sequential shard dispatch over a single TCP connection, avoiding per-shard thread overhead.
- Versioned shard keys eliminate shard-key overwrite races between concurrent writers of the same object ID.
- Exception propagation through futures preserves error semantics: PUT failures surface immediately; GET/DELETE failures are swallowed (matching erasure tolerance and best-effort semantics).
- Missing-shard tolerance on reads is correctly implemented: individual node failures do not surface as errors as long as `k` shards are available.
- Best-effort delete is the right semantic — a failed delete does not corrupt read availability, it only leaks storage.
- Environment-variable-driven node discovery is simple and Docker/container-friendly.

**Remaining trade-offs:**
- No connection pooling: a new `httplib::Client` is instantiated per operation per node, performing a full TCP handshake each time. This overhead dominates at small payload sizes (current `kShardSize = 20` bytes). Persistent clients (one per node, reused across calls) would further reduce overhead.
- No HTTP timeouts: a single unresponsive node will block the entire operation indefinitely. Timeouts must be configured before production network exposure.
- Versioned writes can leave orphaned shard payloads for write attempts that lose the metadata upsert race.
- Static placement strategy (`shard i → node i`) has no rebalancing, no consistent hashing, and cannot handle node addition/removal without changing `kNumNodes` and redeploying.
