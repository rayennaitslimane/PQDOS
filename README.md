
# Post Quantum Distributed Object Store (PQDOS)

PQDOS is a C++20 distributed object store designed for a zero-trust model, using ML-KEM-768 for post-quantum key wrapping, AES-256-GCM for data encryption, and ISA-L Reed-Solomon erasure coding for fault tolerance. It stores encrypted shards and metadata in separate backends and exposes a simple HTTP API for reliable object storage and retrieval.

## I. Prerequisites

- Linux
- GCC >= 11 or Clang >= 14 (C++20)
- CMake >= 3.23
- Conan >= 2.x
- Git
- Docker


## II. Building

```bash
# Build for the first time
./scripts/build.sh

# Clean build
./scripts/build.sh clean

# Remove all build files
./scripts/build.sh reset
```

## III. Testing

### Unit Tests

```bash
./build/Release/pqdos_unit_tests
```

### Integration Tests

Integration tests require PostgreSQL and storage nodes running locally.

```bash
# Start all services
./scripts/setup-integration.sh

# Run tests
./build/Release/pqdos_integration_tests

# Stop all services
./scripts/teardown-integration.sh
```

Services exposed on the host:

| Service | Port |
|---------|------|
| Storage Client API | `localhost:8080` |
| Storage Node 1 | `localhost:9001` |
| Storage Node 2 | `localhost:9002` |
| Storage Node 3 | `localhost:9003` |
| PostgreSQL | `localhost:5433` |

## IV. Client API

### Store an object

```bash
DATA_B64=$(printf 'hello world' | base64 -w0)

curl -X PUT http://localhost:8080/objects/00000000-0000-0000-0000-000000000001 \
  -H "Content-Type: application/json" \
  -d "{\"data\":\"${DATA_B64}\",\"k\":2,\"m\":1,\"shard_size\":20}"
# {"status":"ok"}
```

The `data` field is Base64-encoded object bytes. Erasure parameters are supplied per object via `k`, `m`, and `shard_size`.

### Retrieve an object

```bash
curl http://localhost:8080/objects/00000000-0000-0000-0000-000000000001
# {"data":"aGVsbG8gd29ybGQ="}

# Decode returned Base64 payload
echo 'aGVsbG8gd29ybGQ=' | base64 -d
# hello world
```

### Delete an object

```bash
curl -X DELETE http://localhost:8080/objects/00000000-0000-0000-0000-000000000001
# {"status":"ok"}
```

### List all objects

```bash
curl http://localhost:8080/objects
# [{"id":"00000000-0000-0000-0000-000000000001","size":11,"checksum":"b94d27b9..."}]
```

## V. Architecture Overview

Design decisions are documented as ADRs in [docs/adr/](docs/adr/).

| ADR | Decision |
|-----|----------|
| [0001](docs/adr/0001-quantum-encryption.md) | ML-KEM-768 KEM-based KEK ring + AES-256-GCM per-object DEK with shard-index AAD |
| [0002](docs/adr/0002-erasure-codec.md) | ISA-L Reed-Solomon erasure coding with per-object `k`/`m`/`shard_size` |
| [0003](docs/adr/0003-dual-storage.md) | LMDB for shard payloads, PostgreSQL for object metadata |
| [0004](docs/adr/0004-shard-transport.md) | Parallel HTTP shard dispatch using std::async with index-based placement |
| [0005](docs/adr/0005-http-surface.md) | cpp-httplib embedded HTTP server with Base64 object payloads and JSON erasure parameters |
| [0006](docs/adr/0006-concurrency-contract.md) | Concurrency contract: MetadataStore mutex, KEK ring shared_mutex, tolerated per-object races |

Mermaid diagram for runtime wiring and boundaries:

```
[API Consumer]
      |
      v
[StorageClientServer :8080]
      |
      v
[StorageClient]
      │
      ├── ErasureCodec (ISA-L RS)
      ├── Crypto (AES-256-GCM + ML-KEM-768)
      ├── ShardTransport
      │         ├── Node :9001 → StorageNode → LMDB1
      │         ├── Node :9002 → StorageNode → LMDB2
      │         └── Node :9003 → StorageNode → LMDB3
      └── MetadataStore → PostgreSQL
```

## VI. Storage Node API

Used internally by the client. Each node exposes:

| Method | Route | Body | Description |
|--------|-------|------|-------------|
| GET | `/health` | - | Health check |
| PUT | `/shards/:location` | raw bytes | Store a shard |
| GET | `/shards/:location` | - | Retrieve a shard |
| DELETE | `/shards/:location` | - | Delete a shard |

## VII. Rough Backlog

* Improve the placement strategy by selecting nodes randomly from those available.
* Add a repair strategy that operates on a single route.
* Implement metrics collection and include a few benchmark tests.
* Introduce a chunking strategy to manage data or workload more effectively.
* Implement a garbage collection strategy to clean up unused resources.
