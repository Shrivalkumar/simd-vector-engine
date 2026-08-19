# VectorDB

**VectorDB is a local-first, distributed vector-search engine and operational administration console.** It combines a C++20/ARM64 search core with a real browser console for creating collections, ingesting embeddings, running similarity searches, inspecting shard health, and measuring recall and tail latency under load.

It is built as a production-oriented systems project: the data path is cache-conscious, the index is durable, search is distributed through a coordinator, and every operator-facing metric is measured from real requests. The project runs locally on Apple Silicon or in a local Docker Compose topology—there is no hosted deployment requirement.

## What it achieves

- Fast approximate nearest-neighbour retrieval with an HNSW graph, exact FP32 reranking, and L2, dot-product, or cosine metrics.
- ARM64 performance through 128-bit NEON distance kernels, aligned vector storage, four-way accumulation, and scalar portability fallbacks.
- Durable ingestion using a checksummed write-ahead log (WAL), generation-controlled records, batch validation, and group commits.
- Distributed query routing with a coordinator, virtual-node consistent hashing, concurrent scatter-gather search, deterministic result merging, and persistent shard collection catalogs.
- A local operations console for collection administration, JSON/CSV ingestion, text or raw-vector search, topology inspection, recent-operation telemetry, and controlled stress tests.
- Reproducible quality gates: the reference Apple Silicon workload reached **100% Recall@10**, **4.8k–5.4k vectors/s ingestion**, and **5.6–7.8 ms p99** at 1,000 vectors / 2,000 queries / concurrency 8 / `ef_search=96`.

## Architecture

```text
React + Three.js console  :3000
            |
            | validated local JSON API
            v
Administration gateway   :8090
            |
            | gRPC + Protobuf
            v
Coordinator              :7100
      |         |         |
      v         v         v
   Shard A   Shard B   Shard C
      |         |         |
  collections: HNSW + FP32 vectors + payloads + checksummed WAL
```

For each search, the coordinator fans out to the relevant logical shards, merges results by `(distance, id)`, and fails the request if a shard fails—rather than returning silently incomplete results. Within a shard, HNSW traversal produces candidates and retained FP32 vectors rerank them exactly. Cosine vectors are normalized once at ingestion/query boundaries.

## Key capabilities

| Area | Included today |
|---|---|
| Vector engine | HNSW, L2/dot/cosine, ARM NEON, SQ8 traversal support, exact FP32 reranking |
| Data safety | Checksummed WAL, recovery after torn trailing writes, generation control, batch atomic validation |
| Distribution | gRPC services, coordinator routing, consistent-hash directory, concurrent endpoint batches, collection fanout |
| Operations | Collection CRUD, JSON/CSV upload, raw vector and deterministic text query tools, topology and latency history |
| Performance testing | Isolated temporary datasets, brute-force Recall@K oracle, QPS and p50/p95/p99 reporting |
| Verification | Unit tests, gRPC integration tests, browser/API tests, UBSan, Docker ASan+UBSan and TSan gates |

## Quick start

### Prerequisites

- Apple Silicon macOS with Xcode Command Line Tools
- CMake 3.24+, Protobuf, gRPC, Node.js 22.13+, and npm
- Docker Desktop only if you want the local Linux/sanitizer topology

On macOS, the native dependencies can be installed with:

```bash
brew install cmake grpc protobuf node@26
```

### Start the complete local system

```bash
npm install
npm run dev:all
```

Open [http://localhost:3000](http://localhost:3000). The supervisor starts three local shard processes, the coordinator, the administration gateway, and the console. Stop all of them with `Ctrl+C`.

In the console, create a collection, upload JSON/CSV vectors or use text embedding, run a search, then use **Stress Tests** for an isolated exact-recall workload. Stress tests create and remove their own temporary collection, so they do not alter operational data.

### Build the C++ engine only

```bash
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug -j4
ctest --preset macos-arm64-debug --output-on-failure
./build/macos-arm64-debug-make/vectordb-cli demo
```

## Verify performance and safety

Start the local Release stack, then run:

```bash
npm run benchmark
```

The benchmark uses 1,000 vectors, 2,000 queries, concurrency 8, TopK 10, and `ef_search=96`. It fails if Recall@10 falls below 95%, ingestion falls below 2,000 vectors/s, p99 exceeds 8 ms, or any errors occur.

Additional verification:

```bash
npm test
npm run test:e2e
npm run test:sanitizers
```

`test:sanitizers` runs ASan+UBSan and TSan in Docker Desktop's local Linux VM.

## Repository layout

```text
core/                  C++20 vector engine: distance kernels, HNSW, SQ8, WAL, segments
services/shard/        Shard implementation and gRPC daemon
services/coordinator/  Consistent-hash routing, placement state, scatter-gather gRPC service
services/admin-gateway/ Local HTTP API, telemetry collector, deterministic text embedding, stress runner
proto/                 Protobuf contracts
app/                   React + Three.js local operations console
tests/                 Unit, integration, browser/API, and sanitizer coverage
scripts/               Local supervisor, benchmark gate, and sanitizer runner
deploy/compose/        Three-shard local Docker Compose development topology
docs/architecture/     Implementation boundaries and performance analysis
```

## Current boundaries

This repository is a runnable vertical slice, not a claim of complete HA database semantics. In particular, replication/quorum durability and Raft-backed metadata persistence are not wired yet; payload filtering and binary payload persistence intentionally return explicit unsupported errors; collection-admin fanout is not transactional across shard failures; and local Compose uses insecure development gRPC. These limits are documented so the console and APIs do not imply guarantees the engine does not provide.

See [the current implementation boundary](docs/architecture/current-state.md) and [the recall/ingestion analysis](docs/architecture/performance-analysis.md) for technical detail and measured results.

## Suggested GitHub description

> Local-first distributed vector database and operations console: C++20 HNSW/NEON search, durable WAL, gRPC shard routing, and real-time recall/latency telemetry for Apple Silicon.
