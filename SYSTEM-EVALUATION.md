# PQDOS System Evaluation

## I. Introduction

This report evaluates the post-quantum distributed object store (PQDOS) using the
benchmark harness in [cmd/BenchmarkMain.cpp](cmd/BenchmarkMain.cpp), which drives the real
`StorageClient` API across a sweep of erasure parameters (`k ∈ {2,4,6,8}`, `m ∈ {1,2,3,4}`)
and object sizes (1 KB-1 MB). Findings are drawn from the performance and reliability
notebooks ([docs/metrics/01_performance.ipynb](docs/metrics/01_performance.ipynb),
[docs/metrics/02_reliability.ipynb](docs/metrics/02_reliability.ipynb)) over 1,596 records,
interpreted against the architecture defined in [docs/adr/](docs/adr/README.md).

## II. Executive Summary

The system is functionally correct and its post-quantum security is effectively free: 

ML-KEM-768 + AES-256-GCM adds under 1 ms per operation. The dominant cost is the write path (`PUT` ≈ 9.3 ms, 5.7x a `GET`), driven by shard transport. Durability behaves exactly as Reed-Solomon theory predicts-objects recover if and only if failures <= `m`. The two biggest issues are not in the instrumented hot path at all: a large, *unmeasured* metadata layer (PostgreSQL behind a single serialized connection) and operational gaps in key management and shard garbage collection.

## III. Key Findings

1. **Crypto is negligible; transport dominates.** Mean phase split for `PUT` is 0.75 ms crypto
   vs 6.0 ms transport; for `GET`, 0.36 ms vs 0.64 ms. The post-quantum KEM is not a
   performance concern, validating [ADR-0001](docs/adr/0001-quantum-encryption.md).

2. **A hidden metadata tax.** Instrumented phases (crypto + transport) do not sum to the
   measured total: the gap is ~27% for `PUT`, ~39% for `GET`, and ~65% for `REPAIR`
   (2.55 ms of 7.24 ms is accounted for; the rest is elsewhere). This unmeasured time is the
   PostgreSQL path-single `pqxx::connection` serialized by the [ADR-0006](docs/adr/0006-concurrency-contract.md)
   mutex, plus the N+1 query in `list()` ([ADR-0003](docs/adr/0003-dual-storage.md)). It is
   the system's real bottleneck and is invisible to the current metrics.

3. **Durability tracks parity exactly.** Overall `REPAIR` success is 0.73, but this average is
   misleading: `m=4` configs recover 100% of the time, while `m=1` configs collapse to
   0.33-0.50 once the failure sweep drops more than one shard. Recovery is governed solely by
   `survivors ≥ k`, confirming the MDS property of the ISA-L codec ([ADR-0002](docs/adr/0002-erasure-codec.md)).

4. **Small objects pay a fixed-overhead penalty.** Measured storage overhead converges on the
   theoretical `(k+m)/k` rate for large objects, but a 1 KB object at `2+1` costs 3.2x (vs a
   1.5x asymptote) due to per-shard nonce + GCM tag + msgpack framing. Overhead is amortized by
   larger objects and higher `k` (e.g. `8+2` = 1.32x vs `2+2` = 2.03x for equal durability).

5. **Repair is the most expensive and most variable operation** (σ = 4.14 ms). Beyond
   reconstruction, every repair health-probes all registered nodes over fresh HTTP clients
   ([ADR-0004](docs/adr/0004-shard-transport.md)), inflating its uninstrumented portion.

## IV. Recommendations

1. **Instrument and parallelize the metadata layer.** Add a `metadata_ms` phase timer to make
   the hidden tax visible, then break the single-connection serialization with a small
   connection pool and replace the `list()` N+1 with a single JOIN. This targets the largest
   real cost, especially for `REPAIR` and `GET`.

2. **Trim the write hot path.** Eliminate the unnecessary defensive copy in `encode()`
   (ADR-0002 documents this as removable) to save one object-sized allocation per `PUT`, and
   extend thread-local HTTP client reuse to `init()`/health probes (ADR-0004) so `REPAIR` stops
   reconstructing clients on every call.

3. **Choose erasure defaults deliberately.** Avoid `m=1` in production-durability falls off a
   cliff under correlated, multi-node failure. Prefer a higher-`k` profile such as `4+2` or
   `6+3`, which balances the ~1.3-1.5x overhead against tolerating 2-3 simultaneous losses.

4. **Fix the benchmark's failure model and add a GC sweep.** In
   [cmd/BenchmarkMain.cpp](cmd/BenchmarkMain.cpp) the clamp `std::min(requested_failures, m)`
   is commented out, so the harness can drop more shards than parity allows. Keep it unclamped
   *intentionally* and sweep `failed_nodes` from `0…m+1` to chart the durability cliff
   explicitly. Separately, implement the orphaned-shard garbage collector that ADR-0003/0006
   note is missing-races and failed repairs leak inert shards indefinitely.

5. **Harden key management before non-POC use.** The default keystore lives in world-accessible
   `/tmp` and is a single point of failure, and `rotate()` never re-wraps existing objects
   (ADR-0001). Add a background re-wrap sweep, a keystore backup, and a non-`/tmp` default.
   Because crypto is not a performance bottleneck, these carry negligible runtime cost.

## V. Conclusion

PQDOS delivers post-quantum confidentiality and theory-perfect erasure durability at near-zero
cryptographic cost-a strong foundation. Its performance ceiling, however, is set by an
uninstrumented metadata layer and a transport-heavy write path, not by the security primitives.
Making the metadata cost visible, relieving the single-connection bottleneck, choosing parity
to match the real failure model, and closing the key-management and orphan-GC gaps would move
this system from a correct proof-of-concept toward a production-credible system.
