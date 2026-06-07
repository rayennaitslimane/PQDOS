
# Post Quantum Distributed Object Store (PQDOS)

A zero-trust distributed object storage system featuring post-quantum cryptography encryption and ISA-L erasure coding.

## Building

```bash
# Install dependencies
conan install . \
    --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=20

# Configure
cmake --preset conan-release

# Build
cmake --build --preset conan-release
```

## Testing

```bash
# Start PostgreSQL test database
docker compose up -d postgres-test

# Find test executable
find . -name pqdos_tests

# Run tests
./build/Release/pqdos_tests

# Turn off the postgres-test container
docker compose stop postgres-test
```
