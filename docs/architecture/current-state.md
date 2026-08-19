# Current implementation state

The repository contains a runnable vertical slice rather than a simulated
directory tree.

- `vectordb_core` provides a 128-byte-aligned buffer primitive, ARM NEON
  L2/dot/cosine kernels, a grouped durable checksummed WAL, records with
  generation control, and an HNSW index. The graph uses `2*M` layer-zero
  connectivity, diversity-aware pruning, one-time cosine normalization, and
  exact FP32 reranking. Updates use inactive HNSW nodes so readers never
  observe an index rebuild.
- `vectordb_shard` exposes idempotent batches to the service layer. The gRPC
  shard daemon compiles from the checked-in proto contract and validates raw
  FP32 wire vectors before storage. Each shard persists a collection manifest,
  reloads its catalogs and WALs after restart, and exposes collection CRUD.
- `vectordb_coordinator` owns an epoch-versioned virtual-node directory and a
  migration state machine. Its gRPC service routes batch upserts, gets, and
  deletes by record ID; fans global searches to logical shard leaders; merges
  by `(distance, id)`; and rejects failed shard calls rather than returning
  silently incomplete results. Collection administration is fanned to every
  unique shard process and aggregated at the coordinator.
- The localhost-only administration gateway translates browser JSON to gRPC,
  performs strict validation, provides an offline deterministic text embedding,
  probes shard health, records recent operation latency, and runs bounded async
  ingestion/query stress jobs in disposable collections against a brute-force
  exact top-K oracle.
- The React/Three.js control plane performs real collection creation/deletion,
  JSON/CSV ingestion, text/vector search, topology inspection, and stress tests.
  Counts, shard health, operation history, and latency are measured. The 3D
  coordinates are explicitly representative until a projection worker exists.

## Deliberate production boundaries

The following APIs fail explicitly rather than pretending to offer guarantees
they do not yet have:

- Payload filtering returns `UNIMPLEMENTED` until the bitmap segment backend
  is landed.
- `bytes` payload values return `INVALID_ARGUMENT` until the segment WAL codec
  carries binary values.
- Collection administration fanout is not transactional: a shard failure can
  leave a partially applied create or delete that requires operator repair.
- Recent telemetry and stress-job state live in the gateway process and reset
  when it restarts.
- Docker Compose uses insecure gRPC only for local development.
- The current coordinator directory is deterministic and tested, but its Raft
  persistence/replication adapter has not yet been wired. Do not use the local
  Compose topology as an HA cluster.

These boundaries are intentional: they keep the public response semantics
honest while the remaining persistence and replication milestones are built.

The diagnosed recall/ingestion bottlenecks, implemented corrections, measured
acceptance results, and exact reproduction commands are recorded in
`docs/architecture/performance-analysis.md`.
