
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
conan install . --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset conan-release
cmake --build --preset conan-release
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

## Storage Node API

Used internally by the client. Each node exposes:

| Method | Route | Body | Description |
|--------|-------|------|-------------|
| GET | `/health` | - | Health check |
| PUT | `/shards/:location` | raw bytes | Store a shard |
| GET | `/shards/:location` | - | Retrieve a shard |
| DELETE | `/shards/:location` | - | Delete a shard |
