# ADR-0002: Erasure Codec

**Status:** Accepted  
**Date:** 2026-06-11

## Context

Storing each encrypted shard on a separate node creates a single-node fault tolerance problem: if any one node is unavailable, the object is unreadable. A replication strategy (storing the full object on every node) would 3× the storage footprint and eliminate the privacy benefit of splitting data across nodes (any single node would hold enough to reconstruct the object).

The system needs a codec that:
- Reconstructs the original object from a strict subset of shards (fault tolerance)
- Does not allow any single node's shard to reveal the plaintext alone
- Runs efficiently in a CPU-bound path that also includes AES-GCM per shard
- Integrates with a fixed-width shard model required by the chosen erasure library

## Decision

### Reed-Solomon via Intel ISA-L (`isa-l::isa-l`)

**Intel ISA-L** (`gf_gen_rs_matrix` / `ec_init_tables` / `ec_encode_data`) is used to implement Reed-Solomon erasure coding over GF(2⁸). ISA-L uses SIMD-accelerated Galois Field arithmetic (SSE/AVX on x86) making it substantially faster than portable RS implementations for the parity-generation inner loop.

### Fixed parameters: k=2, m=1, shard\_size=20

```cpp
constexpr uint32_t kDataShards   = 2;
constexpr uint32_t kParityShards = 1;
constexpr std::size_t kShardSize = 20;
```

- **k=2, m=1** means the object is split into 2 data shards and 1 parity shard (total 3 shards). The object can be recovered from any 2 of the 3 shards, tolerating the loss of any single node.
- **shard\_size=20** is a POC constant sized for unit-test payloads. It caps the maximum storable object at `k × shard_size = 40 bytes`. This must be replaced with a dynamic value computed from the actual object size before any production use.

### Padding model

If the final data shard is not full, the remainder is zero-padded to `shard_size`. The original unpadded length is stored in `ObjectMetadata.size` and used as the `original_size` argument to `decode()`, which trims the reconstructed buffer back to the correct length.

### Encode signature and ownership

`encode(Bytes& data, ErasureSpec& erasure_spec)` takes both arguments by non-const reference. In practice `encode` only reads `data` — it does not mutate it. The non-const signature forces callers to make a defensive copy:

```cpp
Bytes mutable_input = bytes;  // StorageClient::put
std::vector<PlainShard> plain_shards = encode(mutable_input, erasure_spec);
```

> **Known inefficiency:** the copy is unnecessary. Changing `encode`'s signature to `const Bytes&` eliminates one full object-sized allocation per write.

### Shard ordering

Shards are stored and retrieved as a `std::vector<PlainShard>` where `PlainShard.index` records the original shard position. The ISA-L decode path (`ec_encode_data` with a recovery matrix) requires knowing which shard positions are present; the index field provides this mapping without imposing ordering constraints on the vector.

## Consequences

**Positive:**
- 1-of-3 node fault tolerance: any single storage node can be offline and all objects remain readable.
- No single shard is sufficient to reconstruct the plaintext (combined with AES-GCM encryption, each node holds only an opaque encrypted fragment).
- ISA-L SIMD acceleration keeps parity generation off the critical path for typical payload sizes.
- Zero-padding with stored original length is a simple, correct approach to variable-length inputs.

**Negative / Risks:**
- `kShardSize = 20` must be made dynamic before storing real data. The current value causes a hard exception for any object larger than 40 bytes.
- The `encode` / `decode` functions reference global types (`Bytes`, `PlainShard`, `ErasureSpec`) declared in `Models.hpp` without including it — they rely on the caller's translation unit having already included `Models.hpp`. This implicit coupling should be made explicit via a direct `#include` in `ErasureCodec.hpp`.
- The `<iostream>` include in `ErasureCodec.hpp` is unused and should be removed.
- Parameters are compile-time constants in an anonymous namespace inside `StorageClient.cpp`, not in `ErasureCodec`. A caller wanting a different erasure configuration has no clean extension point.
