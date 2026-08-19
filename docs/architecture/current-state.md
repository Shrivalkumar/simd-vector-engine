# Current implementation state

The repository contains a runnable vertical slice rather than a simulated
directory tree.

- `vectordb_core` provides cache-aligned storage, ARM NEON L2/dot/cosine
  kernels, a durable checksummed WAL, records with generation control, and an
  HNSW index. Updates use tombstoned HNSW nodes so readers never observe an
  index rebuild.
- `vectordb_shard` exposes idempotent batches to the service layer. The gRPC
  shard daemon compiles from the checked-in proto contract and validates raw
  FP32 wire vectors before storage.
- `vectordb_coordinator` owns an epoch-versioned virtual-node directory and a
  migration state machine. The gRPC coordinator fans a global search to all
  logical shard leaders, merges by `(distance, id)`, and rejects failed shard
  calls rather than returning silently incomplete results.
- The web control plane is a standalone, build-verified Three.js view. It uses
  a deterministic sample until connected to a telemetry stream; its rendering
  component is code-split from the initial UI shell.

## Deliberate production boundaries

The following APIs fail explicitly rather than pretending to offer guarantees
they do not yet have:

- Payload filtering returns `UNIMPLEMENTED` until the bitmap segment backend
  is landed.
- `bytes` payload values return `INVALID_ARGUMENT` until the segment WAL codec
  carries binary values.
- Docker Compose uses insecure gRPC only for local development.
- The current coordinator directory is deterministic and tested, but its Raft
  persistence/replication adapter has not yet been wired. Do not use the local
  Compose topology as an HA cluster.

These boundaries are intentional: they keep the public response semantics
honest while the remaining persistence and replication milestones are built.

