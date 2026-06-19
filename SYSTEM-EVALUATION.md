# PQDOS System Evaluation

## How this was measured

Everything below comes from running the benchmark harness in
[cmd/BenchmarkMain.cpp](cmd/BenchmarkMain.cpp) against a real cluster. The harness doesn't mock
anything: it drives the actual `StorageClient` API through a sweep of erasure settings (`k` from
2 to 8, `m` from 1 to 4) and object sizes from 1 KB to 1 MB, then records timing and outcome for
every `PUT`, `GET` and `REPAIR`. The two notebooks in
[docs/metrics/](docs/metrics/README.md) turn those ~1,600 records into the charts and tables this
report leans on, and the design rationale lives in the [ADRs](docs/adr/README.md). Where I make a
claim, I've tried to point at the chart or the ADR that backs it.

## The short version

PQDOS does what it set out to do. Objects encrypt, split, store, survive node loss, and come back
intact, and the post-quantum machinery costs almost nothing to run. The interesting part is where
the time actually goes, because it isn't where you'd expect.

If you only remember three things:

- **The post-quantum crypto is basically free.** ML-KEM-768 plus AES-256-GCM adds well under a
  millisecond per operation. Security was never the thing slowing this system down.
- **Writes are the expensive path, and the network is why.** A `PUT` takes roughly 9 ms, around
  five to six times a `GET`, and most of that is shard transport over HTTP.
- **The real bottleneck isn't even on the chart.** A large slice of every operation is spent in
  the metadata layer, which the current metrics don't time at all. That blind spot, not the
  cryptography, is what would cap throughput first.

## Where the time goes

The phase breakdown in [01_performance.ipynb](docs/metrics/01_performance.ipynb) splits each
operation into a crypto phase and a transport phase. The crypto phase barely registers, a
fraction of a millisecond for both reads and writes, while transport dominates the write path.
That's exactly the outcome [ADR-0001](docs/adr/0001-quantum-encryption.md) was hoping for: a
per-object DEK wrapped by an ML-KEM-768 KEK is cheap enough that you never have to think about it
again.

The catch is that the two timed phases don't add up to the measured total. There's a consistent
gap, modest on `PUT`, larger on `GET`, and largest of all on `REPAIR`, where it accounts for the
majority of the operation. That missing time is the PostgreSQL metadata path, and it's missing for
a structural reason: [ADR-0006](docs/adr/0006-concurrency-contract.md) funnels every metadata call
through a single connection behind one mutex, and [ADR-0003](docs/adr/0003-dual-storage.md)
documents an N+1 query in `list()` that fires one extra round-trip per object. None of this is
instrumented, so it's invisible in the charts even though it's some of the most expensive work the
system does. The single biggest improvement available here is simply *measuring* it.

## Durability behaves exactly like the theory says

This is the part that's genuinely satisfying. The reliability notebook
([02_reliability.ipynb](docs/metrics/02_reliability.ipynb)) shows recovery tracking
Reed-Solomon's guarantee with no surprises: an object comes back if, and only if, at least `k`
shards survive. Configurations with generous parity (`m=4`) recover every time; configurations
with `m=1` fall off a cliff the moment more than one shard goes missing.

The headline "overall repair success" number is therefore a little misleading on its own, it's an
average across both robust and fragile configurations, and that average hides the fact that the
outcome is completely deterministic once you know how many shards were lost. The ISA-L codec
chosen in [ADR-0002](docs/adr/0002-erasure-codec.md) is doing precisely what it promised. The
practical lesson is about parameter choice, not codec behaviour: `m=1` is a trap under correlated
or multi-node failure.

## Small objects pay a tax

Storage overhead converges nicely on the theoretical `(k+m)/k` rate for large objects, but small
ones are punished. A 1 KB object at `2+1` ends up costing a little over three times its size
against an asymptote closer to 1.5x, because each shard carries fixed framing, a nonce, a GCM
tag, and msgpack envelope, that's negligible for a megabyte and ruinous for a kilobyte. Higher
`k` amortizes this well: at equivalent durability, an `8+2` profile is far leaner than `2+2`. The
takeaway is that PQDOS rewards batching small objects and choosing wider stripes.

## Repair is the wild card

Repair is both the slowest operation and by far the most variable. Reconstruction itself is cheap;
what inflates it is everything around it. Before writing replacements, `repair()` health-probes
every registered node, and per [ADR-0004](docs/adr/0004-shard-transport.md) those probes still
spin up fresh HTTP clients rather than reusing the thread-local ones the hot path already caches.
Combined with the uninstrumented metadata work, that makes repair latency swing widely from run to
run. It's the operation most likely to surprise you in production and the one most worth
tightening.

## What I'd fix first

Roughly in priority order:

1. **Make the metadata cost visible, then relieve it.** Add a `metadata_ms` timer so the hidden
   tax stops being a guess, then break the single-connection serialization with a small pool and
   replace the `list()` N+1 with one JOIN. This is the highest-leverage change in the whole system.
2. **Trim the write path.** Drop the unnecessary defensive copy in `encode()` that ADR-0002 flags
   as removable, and extend the thread-local HTTP client reuse to `init()` and the health probes
   so repair stops rebuilding clients on every call.
3. **Pick erasure defaults on purpose.** Avoid `m=1` in anything that matters. A `4+2` or `6+3`
   profile buys tolerance for two or three simultaneous losses at a sane overhead.
4. **Fix the failure model and add garbage collection.** The benchmark currently leaves the
   `std::min(requested_failures, m)` clamp commented out in
   [cmd/BenchmarkMain.cpp](cmd/BenchmarkMain.cpp); sweeping `failed_nodes` from 0 to `m+1` would
   chart the durability cliff explicitly. Separately, the orphaned-shard collector that ADR-0003
   and ADR-0006 both note as missing needs to exist, races and failed repairs leak inert shards
   today.
5. **Harden key management before this is more than a POC.** The default keystore sits in
   world-accessible `/tmp` and is a single point of failure, and `rotate()` never re-wraps
   existing objects (ADR-0001). Since crypto isn't a performance concern, a re-wrap sweep, a
   keystore backup, and a non-`/tmp` default all come essentially for free.

## Closing thought

PQDOS already delivers the hard parts: post-quantum confidentiality at rest and erasure durability
that matches the math, both at negligible cryptographic cost. Its ceiling is set by an unmeasured
metadata layer and a transport-heavy write path, engineering problems, not cryptographic ones.
Instrument the metadata, relieve the single connection, choose parity to match the real failure
model, and close the key-management and orphan-GC gaps, and this moves from a correct
proof-of-concept toward something you could actually run.
