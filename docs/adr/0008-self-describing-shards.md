# ADR-0008: Self-Describing Shards and Metadata Rebuild

**Status:** Accepted  
**Date:** 2026-07-02

## Context

PQDOS aspires to distributed RAIN (Redundant Array of Independent Nodes)
principles. The data plane already honours this: object bytes are erasure coded
with ISA-L Reed-Solomon ([ADR-0002](0002-erasure-codec.md)) and scattered as
`k + m` shards across independent `StorageNode` processes
([ADR-0003](0003-dual-storage.md)). Any `m` nodes can be lost and every object
still reconstructs.

The **metadata plane did not**. Every fact required to reconstruct an object -
the erasure spec (`k`, `m`, `shard_size`), the wrapped per-object DEK, the
`kek_id`, the checksum, the size, and the shard locations - lived in a *single*
PostgreSQL instance. Losing that one database rendered every object permanently
unrecoverable even though 100% of the shards survived. The connection-pool work
in the [ADR-0006](0006-concurrency-contract.md) amendment fixed metadata
*throughput*, not metadata *redundancy*; the durability guarantee was therefore
a system-level fiction.

The goal of this ADR is the **minimal** change that makes the metadata plane
inherit the redundancy the data plane already pays for, plus an efficient path
to rebuild the catalog after its total loss.

## Decision

### 1. Shards carry their object's manifest

Each shard persisted on a node is wrapped in a self-describing envelope:

```cpp
struct ShardManifest {          // replicated object-version metadata
    std::string object_id;
    std::string version;        // per-put random token, also in the shard key
    uint64_t    size;
    std::string checksum;
    ErasureSpec erasure;        // k, m, shard_size
    Bytes       encrypted_dek;  // ML-KEM-wrapped per-object DEK
    std::string kek_id;
    uint64_t    written_at;     // client wall-clock ns, for version tie-breaks
};

struct StoredShard {            // the unit written to LMDB
    ShardManifest manifest;
    EncryptedShard shard;
};
```

The manifest is **fully replicated onto every shard**. Because any surviving `k`
shards suffice to reconstruct the object, they also suffice to recover its
metadata: the manifest inherits the erasure redundancy by construction. The
manifest deliberately does **not** carry `shard_locations`. Physical placement is
distributed knowledge that a rebuild rediscovers by scanning node keyspaces, so
it is never stale or self-referential.

Both `put()` and `repair()` write `StoredShard` envelopes; the read path
(`fetch_encrypted_shards`) decodes them and uses only the `EncryptedShard`.
`StoredShard::decode_shard` also tolerates a legacy bare `EncryptedShard`
payload, so pre-existing data remains readable during migration.

### 2. Nodes can enumerate their keyspace

`StorageNode::list_locations()` performs a read-only LMDB cursor scan returning
every shard key (keys only - no payloads are copied). It is exposed as
`GET /shards/list` on the node server.

### 3. `reindex()` rebuilds the catalog

`StorageClient::reindex()` reconstructs the PostgreSQL catalog from the nodes:

1. **Scan** every node's keyspace in parallel. Keys alone (`object_id/version/
   shard_index`) reconstruct shard placement, so no payloads are read in this
   phase.
2. **Group** discovered shards by `(object_id, version)`.
3. **Fetch one manifest per group** (all shards of a group carry equivalent reconstruction metadata) -
   one payload read per version, not per shard.
4. **Select** a winning version per object: prefer the **newest reconstructable**
   version (`>= k` surviving shards, newest `written_at` wins) so a rebuild
   restores the most recent valid data rather than resurrecting a stale
   overwritten version. Fall back to the version with the most surviving shards
   only when none reaches `k`.
5. **Insert** each object's metadata with `INSERT ... ON CONFLICT (id) DO
   NOTHING` (`MetadataStore::insert_if_absent`) so a reindex never clobbers a
   live or newer catalog entry. Missing shard indices are recorded as an
   unreachable sentinel location (`0.0.0.0:0/...`) so the object stays valid and
   `repair()` can heal it back to full replication.

It is exposed as `POST /admin/reindex` and returns a summary
(`versions_scanned`, `objects_recovered`, `objects_skipped_existing`,
`degraded_objects`, `unreadable_versions`, `errors`).

Manifests whose erasure parameters fall outside the system's own bounds
(`k == 0`, `m == 0`, `k + m > 255`, or `shard_size == 0`) are rejected before
being used to size any allocation, guarding the rebuild against corrupt or
hostile payloads from a compromised node.

## Consequences

**Positive:**
- Metadata durability now equals data durability: the catalog survives the loss
  of PostgreSQL and is fully rebuildable from the surviving shards.
- The rebuild doubles as a disaster-recovery / bootstrap path the system
  previously lacked.
- The scan is efficient: keys-only for placement, one manifest fetch per version.
- `reindex` is idempotent and non-clobbering, so it is safe to run against a
  partially-populated catalog during recovery.

**Negative / Risks:**
- Each shard now carries a replicated manifest (~1.1 KB, dominated by the
  ML-KEM-768 wrapped DEK). This is negligible for large objects but adds to the
  small-object framing tax already noted in the system evaluation.
- The on-disk shard format changed. New writes are `StoredShard`; the read path
  tolerates legacy bare shards, but a manifest cannot be recovered from
  pre-ADR-0008 data (those objects are rebuildable only if re-put).
- Version selection uses a client wall-clock (`written_at`) to break ties.
  Clock skew across writers could, in principle, mis-order two versions written
  within the skew window; the reconstructable-first rule bounds the blast radius
  to "a slightly older but still valid version".
- `POST /admin/reindex` and `GET /shards/list` are unauthenticated like every
  other endpoint (the trust-boundary gap recorded in the system evaluation);
  they should sit behind the same auth control when one is introduced.
