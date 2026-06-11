
# Post Quantum Distributed Object Store (PQDOS)

A zero-trust distributed object storage system featuring post-quantum cryptography (ML-KEM-768) and ISA-L erasure coding.

## Prerequisites

- Linux
- GCC >= 11 or Clang >= 14 (C++20)
- CMake >= 3.23
- Conan >= 2.x
- Git
- Docker


## Building

```bash
# Build for the first time
./scripts/build.sh

# Clean build
./scripts/build.sh clean

# Remove all build files
./scripts/build.sh reset
```

## Testing

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

## Client API

### Store an object

```bash
curl -X PUT http://localhost:8080/objects/00000000-0000-0000-0000-000000000001 \
  -H "Content-Type: application/json" \
  -d '{"data": "hello world"}'
# {"status":"ok"}
```

The `data` field contains the raw binary content as a string.

### Retrieve an object

```bash
curl http://localhost:8080/objects/00000000-0000-0000-0000-000000000001
# {"data":"hello world"}
```

### Delete an object

```bash
curl -X DELETE http://localhost:8080/objects/00000000-0000-0000-0000-000000000001
# {"status":"ok"}
```

### List all objects

```bash
curl http://localhost:8080/objects
# [{"id":"my-file","size":11,"checksum":"b94d27b9..."}]
```

## Architecture Overview

Design decisions are documented as ADRs in [docs/adr/](docs/adr/).

| ADR | Decision |
|-----|----------|
| [0001](docs/adr/0001-quantum-encryption.md) | ML-KEM-768 KEK + AES-256-GCM per-object DEK with shard-index AAD |
| [0002](docs/adr/0002-erasure-codec.md) | ISA-L Reed-Solomon 2+1 erasure coding |
| [0003](docs/adr/0003-dual-storage.md) | LMDB for shard payloads, PostgreSQL for object metadata |
| [0004](docs/adr/0004-shard-transport.md) | Parallel HTTP shard dispatch using std::async with index-based placement |
| [0005](docs/adr/0005-http-surface.md) | cpp-httplib embedded HTTP server for client and node APIs |

## Storage Node API

Used internally by the client. Each node exposes:

| Method | Route | Body | Description |
|--------|-------|------|-------------|
| GET | `/health` | - | Health check |
| PUT | `/shards/:location` | raw bytes | Store a shard |
| GET | `/shards/:location` | - | Retrieve a shard |
| DELETE | `/shards/:location` | - | Delete a shard |

