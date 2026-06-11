# ADR-0005: HTTP Surface

**Status:** Accepted (with known gaps)  
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

Error responses are JSON objects with an `"error"` string field. Success responses are JSON objects with a `"status": "ok"` field or a `"data"` field. HTTP status codes follow REST conventions (200, 400, 404, 500).

### Binary data encoding: raw JSON string (known gap)

Objects are accepted and returned by `StorageClientServer` as a JSON body with a `"data"` field carrying the raw bytes cast to `std::string`:

```cpp
// PUT handler
const std::string& raw = input["data"].get_ref<const std::string&>();
Bytes bytes(raw.begin(), raw.end());

// GET handler
body["data"] = std::string(bytes.begin(), bytes.end());
```

> **Known defect:** this encoding is only safe for text payloads. Byte sequences containing `\0` or invalid UTF-8 will be silently corrupted or rejected by JSON parsers. The correct encoding is **Base64**: encode on `GET`, decode on `PUT`. This must be fixed before the API accepts arbitrary binary objects.

Shard payloads between `StorageClient` and `StorageNodeServer` are transmitted as raw `application/octet-stream` bodies — this is correct and has no encoding issue.

### Thread safety: not implemented (known gap)

cpp-httplib's thread pool means handler lambdas for `StorageClientServer` execute concurrently. Both shared resources are currently unprotected:

1. **`StorageClient`** — `kek_ring_`, `active_kek_id_`, and the call sequence within `put()`/`get()`/`rotate()` are not guarded by any mutex. Concurrent requests reading `kek_ring_` while `rotate()` modifies it is a data race.

2. **`MetadataStore::conn_`** — `pqxx::connection` is not thread-safe. Concurrent calls to `put`, `get`, and `list` through the same connection object will corrupt the connection state.

The fix for `MetadataStore` is a connection pool (one `pqxx::connection` per thread, or a checked-out pool). The fix for `StorageClient` is a `std::shared_mutex` guarding `kek_ring_` and `active_kek_id_` with shared locks for reads and an exclusive lock for `rotate()`.

Both fixes are deferred to the next milestone. For the POC, the integration tests use a single-threaded client and do not exercise concurrent access.

### Lifecycle management

`StorageClientServer` and `StorageNodeServer` own the `httplib::Server` by value. `start()` blocks the calling thread by calling `server_.listen(...)`. `stop()` signals the server to stop; it is also called from the destructor, making shutdown safe even if `stop()` was never called explicitly.

## Consequences

**Positive:**
- cpp-httplib requires zero daemon setup and compiles cleanly into the binary — minimal operational surface for a POC.
- Consistent JSON error responses are easy to test and consume programmatically.
- `application/octet-stream` for shard payloads is efficient and correct — no encoding overhead on the internal hot path.
- Destructor-called `stop()` ensures the server does not outlive its owning object.

**Negative / Risks:**
- The raw-string JSON encoding for object data silently corrupts non-UTF-8 binary payloads. Any test or usage with binary objects must switch to Base64 before this API is considered correct.
- `StorageClient` and `MetadataStore` are not thread-safe, but `httplib::Server` dispatches on a thread pool. Concurrent requests will produce data races in production. This is a latent correctness defect, not just a performance issue.
- The blocking `server_.listen()` design means the caller must arrange to run `start()` on a dedicated thread if the process needs to do other work concurrently.
