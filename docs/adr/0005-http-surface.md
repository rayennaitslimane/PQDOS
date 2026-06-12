# ADR-0005: HTTP Surface

**Status:** Accepted  
**Date:** 2026-06-11

## Context

Both the client-facing API (`StorageClientServer`) and the internal shard API (`StorageNodeServer`) need an HTTP server layer. Requirements:
- Simple request/response semantics (no streaming, no long-polling)
- Embeddable in a C++ process with minimal build overhead
- Enough structure to be testable with standard HTTP clients
- Consistent error response format

Two additional cross-cutting concerns arise at this layer:
1. **Binary data encoding** — objects are arbitrary byte sequences; the wire format must represent them faithfully.
2. **Thread safety** — cpp-httplib dispatches requests on a thread pool; `StorageClient` and `MetadataStore` must be safe to call from concurrent handler invocations.

## Decision

### Server library: cpp-httplib (header-only)

Both `StorageClientServer` and `StorageNodeServer` embed a `httplib::Server` instance. cpp-httplib is header-only, requires no separate build step, and handles HTTP/1.1 keep-alive and concurrent connections via an internal thread pool. Route registration is done once in `setup_routes()` called from the constructor.

Servers expose the following routes:

**StorageClientServer** (client-facing):
| Method | Path | Description |
|--------|------|-------------|
| `PUT` | `/objects/:id` | Store an object |
| `GET` | `/objects/:id` | Retrieve an object |
| `DELETE` | `/objects/:id` | Remove an object |
| `GET` | `/objects` | List all objects |

**StorageNodeServer** (internal shard storage):
| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Liveness probe |
| `PUT` | `/shards/:location` | Store a shard payload |
| `GET` | `/shards/:location` | Retrieve a shard payload |
| `DELETE` | `/shards/:location` | Remove a shard payload |

Error responses are JSON objects with an `"error"` string field. Success responses are JSON objects with a `"status": "ok"` field or a `"data"` field (and `/objects` returns a JSON array). Current status semantics are mixed: validation errors return 400, missing objects in `DELETE /objects/:id` return 404, but most backend errors (including missing objects in `GET /objects/:id`) currently surface as 500.

### Binary data encoding: Base64

Objects are accepted and returned by `StorageClientServer` as a JSON body with a `"data"` field carrying the object bytes as a Base64-encoded string. The PUT body also includes erasure coding parameters:

```json
{
  "data": "<base64-encoded object bytes>",
  "k": 2,
  "m": 1,
  "shard_size": 20
}
```

```cpp
// PUT handler
auto decoded = Botan::base64_decode(raw);
Bytes bytes(decoded.begin(), decoded.end());

// GET handler
body["data"] = Botan::base64_encode(bytes.data(), bytes.size());
```

This encoding is safe for arbitrary binary payloads including null bytes and invalid UTF-8 sequences.

Shard payloads between `StorageClient` and `StorageNodeServer` are transmitted as raw `application/octet-stream` bodies — this is correct and has no encoding issue.

### Thread safety: resolved by ADR-0006

cpp-httplib's thread pool means handler lambdas for `StorageClientServer` execute concurrently. The two data races that existed at initial design have been addressed:

1. **`StorageClient`** — `kek_ring_` and `active_kek_id_` are now guarded by a `std::shared_mutex`. Concurrent `put()`/`get()` acquire shared locks; `rotate()` acquires an exclusive lock. The lock is scoped narrowly and is never held across HTTP I/O.

2. **`MetadataStore::conn_`** — `pqxx::connection` is not thread-safe. A `std::mutex` serialises all public methods on the single connection instance.

See [ADR-0006](0006-concurrency-contract.md) for the full concurrency contract, including explicitly tolerated races (per-object ordering is not guaranteed).

### Lifecycle management

`StorageClientServer` and `StorageNodeServer` own the `httplib::Server` by value. `start()` blocks the calling thread by calling `server_.listen(...)`. `stop()` signals the server to stop; it is also called from the destructor, making shutdown safe even if `stop()` was never called explicitly.

## Consequences

**Positive:**
- cpp-httplib requires zero daemon setup and compiles cleanly into the binary — minimal operational surface for a POC.
- Consistent JSON error responses are easy to test and consume programmatically.
- `application/octet-stream` for shard payloads is efficient and correct — no encoding overhead on the internal hot path.
- Destructor-called `stop()` ensures the server does not outlive its owning object.

**Negative / Risks:**
- `StorageClient` and `MetadataStore` are now thread-safe under the contract defined in ADR-0006. Per-object operation ordering is not guaranteed; callers must not issue concurrent conflicting mutations on the same object ID.
- The API does not map all domain errors cleanly to HTTP status codes yet (for example, object-not-found in `GET /objects/:id` currently returns 500 rather than 404).
- The blocking `server_.listen()` design means the caller must arrange to run `start()` on a dedicated thread if the process needs to do other work concurrently.
