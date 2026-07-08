# ADR-0009: HRW Placement and Manual Rebalancing

**Status:** Accepted  
**Date:** 2026-07-08

## Context

Placement in PQDOS was originally positional: [ADR-0004](0004-shard-transport.md)
assigned shard `i` to `eligible_nodes[i]`, where `eligible_nodes` is the node
registry ordered by `registered_at`. This has two consequences that get worse as
the cluster grows and churns:

1. **Placement depends on registration order, not on the object.** Every object
   with the same `k + m` lands its shards on the *same* prefix of the registry.
   The first `k + m` registered nodes absorb every write; nodes registered later
   receive nothing until an earlier node is unregistered.
2. **Membership changes do not migrate data.** ADR-0004 explicitly stated that
   "no consistent hashing or rebalancing is performed when nodes are added or
   removed; existing shard locations are not migrated." A node added to relieve
   load stayed empty, and a node removed left its objects' positional mapping
   silently shifted for every subsequent write.

The system already had the machinery to move a shard safely - [ADR-0008](0008-self-describing-shards.md)
made every shard a self-describing `StoredShard` (it carries its object's
manifest), and [ADR-0006](0006-concurrency-contract.md) established an optimistic,
version-fenced metadata commit (`conditional_put(metadata, expected_version)`)
that lets `repair()` mutate an object without silently overwriting a concurrent
writer. What was missing was (a) a placement function that maps an object to a
stable, spread set of nodes, and (b) an operator-triggered path that moves an
object's shards toward that placement without risking durability.

## Decision

### 1. HRW (rendezvous) placement

Placement is computed by `hrw_intended_nodes(object_id, eligible_nodes,
total_shards)`, which returns a vector whose element `i` is the intended node for
shard index `i`:

```cpp
std::vector<std::string> hrw_intended_nodes(
    const std::string& object_id,
    const std::vector<std::string>& eligible_nodes,
    std::size_t total_shards
);
```

Each shard independently ranks every eligible node by a rendezvous weight and
greedily takes the highest-weighted node not already assigned to a lower shard
index:

```
weight(object_id, shard_index, node) = FNV-1a-64(object_id | shard_index | node)
```

- **Deterministic and portable.** FNV-1a-64 is a fixed, dependency-free hash, so
  the same `(object, shard, node)` tuple always yields the same weight across
  processes and platforms. Placement depends on the node *set*, not its ordering.
- **Distinct nodes per object.** The greedy exclusion guarantees the `k + m`
  shards of an object occupy `k + m` distinct nodes (fault independence).
- **Membership-stable (minimal churn).** This is the defining rendezvous
  property: adding or removing a node only changes the shards for which that node
  would win or was winning. Dropping a node that held none of an object's shards
  leaves that object's placement completely unchanged.
- **Requires `eligible_nodes.size() >= total_shards`**, and throws otherwise.

`hrw_intended_nodes` computes over **all registered nodes**, so the intended
layout of an object is a stable fact independent of transient node health.

### 2. `put()` and `repair()` adopt HRW

`StorageClient::put()` now places shard `i` on `hrw_intended_nodes(...)[i]`
instead of `eligible_nodes[i]`. New writes therefore land on their intended
nodes.

`StorageClient::repair()` places each reconstructed shard on its HRW-intended
node when that node is healthy, falling back to any healthy node (as before) when
the intended target is down. Repair thus heals *toward* the same layout `put()`
and rebalancing target, rather than fighting them. Health filtering of repair
targets (ADR-0004) is unchanged and orthogonal.

### 3. Manual rebalancing reuses the version-fenced commit

Two methods move objects toward their HRW-intended placement:

```cpp
RebalanceObjectResult rebalance_object(const std::string& object_id,
                                       const RebalancePolicy& policy = {});
RebalanceReport       rebalance(const RebalanceScope& scope = {});
```

`rebalance_object` operates on a single object and is the reusable core;
`rebalance` iterates a bounded set of objects and aggregates a report. The
per-object algorithm mirrors `repair()`'s ordering and fence:

1. **Load metadata, record the version.** `expected_version = metadata.version`.
2. **Safety gate.** Require `>= k + m` registered *and* healthy nodes; otherwise
   the object is skipped (`SkippedUnsafe`). Compute the HRW-intended placement.
3. **Degraded gate.** Probe every current shard. If any shard is missing or
   unreachable, the object is skipped (`SkippedDegraded`) - moving a shard while
   the object is already degraded could drop live copies below `k`. Repair heals
   first; rebalancing only touches fully-replicated objects.
4. **Plan.** A shard is a move candidate only if its intended node differs from
   its current node and the intended node is healthy. The plan is capped by the
   policy's `max_moves`.
5. **Copy before commit.** For each planned move, the raw `StoredShard` bytes are
   fetched from the current node and PUT to the intended node under the *same*
   shard key. Because the shard is self-describing (ADR-0008), no decryption,
   decoding, or re-encryption occurs - the manifest and version are preserved
   verbatim. The new copy is written **before** the metadata commit, so
   durability is never reduced.
6. **Version-fenced commit.** `conditional_put(updated_metadata,
   expected_version)` atomically repoints the moved shard locations *iff* the
   version is unchanged. If a concurrent `put`/`remove`/`repair`/`rebalance`
   advanced the version, the commit affects zero rows and the object is reported
   `SkippedConflict`; the freshly written copies are inert orphans and no
   metadata changes.
7. **Old copies are not deleted inline.** After a successful commit the previous
   copies on their old nodes are left as inert, encrypted orphans to be reclaimed
   by a future garbage-collection sweep (out of scope, consistent with the orphan
   model already established in ADR-0006 and ADR-0008). Rebalancing never issues
   deletes, so a mid-move failure can only leak storage, never lose data.

### 4. Scope and safety policies

Rebalancing is bounded explicitly so an operator controls its blast radius:

| Control | Meaning |
|---------|---------|
| `dry_run` | Plan only - report misplaced shards and would-be moves, write nothing. |
| `max_moves` | Cap on the total number of shards relocated in the invocation. |
| `max_objects` | Cap on the number of objects processed (cluster scope). |
| `object_ids` | Restrict a cluster pass to an explicit set of objects (empty = whole catalog). |

Per-object outcomes are reported as a `RebalanceStatus`: `Balanced`, `Moved`,
`DryRun`, `SkippedDegraded`, `SkippedUnsafe`, `SkippedConflict`,
`SkippedNotFound`, or `Errored`. A cluster pass aggregates these into a
`RebalanceReport` with per-object detail.

### 5. REST surface

Two endpoints expose the capability (see [ADR-0005](0005-http-surface.md)):

| Method | Path | Body (all fields optional) | Description |
|--------|------|----------------------------|-------------|
| `POST` | `/objects/:id/rebalance` | `{ dry_run, max_moves }` | Rebalance one object toward HRW placement. |
| `POST` | `/admin/rebalance` | `{ dry_run, max_objects, max_moves, object_ids }` | Rebalance a bounded set of objects. |

Both return `"status": "ok"` with the per-object result / aggregate report;
malformed bodies return 400 and backend errors 500, matching the existing surface.
The per-object outcome (the `RebalanceStatus` above) is carried in an `"outcome"`
field so it does not collide with the transport-level `"status": "ok"`.

## Consequences

**Positive:**
- Placement is now a stable function of the object, spread across the cluster by
  rendezvous hashing, and nodes added to the registry become write targets
  immediately for the objects that hash to them.
- Rebalancing is durability-safe by construction: copy-before-commit plus the
  version fence mean a move either completes atomically or leaves only inert
  orphans; it can never reduce replication or overwrite a concurrent mutation.
- The feature reuses existing primitives - `conditional_put` version fencing
  (ADR-0006) and self-describing shards (ADR-0008) - rather than introducing
  distributed locks or a re-encryption path.
- Operators get a dry-run preview and hard `max_moves`/`max_objects` bounds, so a
  rebalance is auditable and its I/O cost is capped.
- Rebalancing composes safely with `put`/`get`/`remove`/`repair` under the
  existing concurrency contract; no new global coordination is required.

**Negative / Risks:**
- Rebalancing leaves orphaned shard copies on the old nodes. These are inert
  encrypted data, but there is still no GC sweep to reclaim them, so repeated
  rebalancing grows reclaimable storage until GC lands.
- `hrw_intended_nodes` uses FNV-1a for weighting. It is a good, fast,
  non-cryptographic hash with well-spread output for this use, but it is not a
  keyed/cryptographic function; an adversary who controls node addresses could in
  principle bias placement. Node membership is a trusted, operator-controlled
  input, so this is acceptable for the current model.
- Changing `put()` to HRW changes the placement of *new* writes relative to the
  old positional scheme. Objects written before this change remain on their old
  positional nodes until a rebalance moves them; the two schemes coexist until
  the catalog is swept.
- A cluster-wide rebalance without `object_ids` scans the whole catalog via
  `MetadataStore::list()`; on a large catalog this should be bounded with
  `max_objects`/`max_moves` and run as an administrative operation, not on the
  hot path.
- Rebalancing only acts on fully-replicated objects. A persistently degraded
  object is never rebalanced until `repair()` restores it, by design.
