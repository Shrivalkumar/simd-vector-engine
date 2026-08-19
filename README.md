# VectorDB

VectorDB is a C++20 vector search engine with a cache-conscious single-node core,
HNSW approximate nearest-neighbour search, durable write-ahead logging, and a
distributed control-plane roadmap. The first executable slice is deliberately
small enough to run on an 8 GB Apple Silicon development machine while keeping
the storage and API boundaries needed for sharding, replication, and telemetry.

## Build and verify

```bash
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug
ctest --preset macos-arm64-debug
./build/macos-arm64-debug-make/vectordb-cli demo
```

The core currently provides persistent collection records, L2/dot/cosine
distance kernels with ARM NEON acceleration, HNSW search, typed payloads,
generation-checked deletion, and crash-safe WAL recovery. It also includes
gRPC shard/coordinator services and a build-verified telemetry UI. See
`docs/architecture/current-state.md` for the deliberately explicit HA and
filtering boundaries.
