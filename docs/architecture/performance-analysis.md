# Recall and ingestion diagnostic

## Reproduced baseline

The original workload was reproduced against a new collection before the
optimization:

| Metric | Reproduced value |
|---|---:|
| Source-vector self-hit rate (previously labeled Recall@K) | 70.5% |
| Ingestion | 235.4 vectors/s |
| Query throughput | 1,043.1 QPS |
| p50 / p95 / p99 | 3.40 / 5.33 / 6.56 ms |
| Errors | 0 |

This confirms that the reported 70.4% and 248.6 vectors/s were properties of
the implementation, not a transient UI artifact.

## Root causes

### Recall and graph reachability

The dashboard's old “Recall@K” was not recall. It checked only whether a query
vector found its own source ID anywhere in the top K. It did not build an exact
top-K oracle or measure set overlap. Repeated tests also appended to the
operator-selected collection, so the configured vector count did not describe
the searched dataset after the first run.

The low self-hit value nevertheless exposed real graph defects:

1. Layer zero was capped at `M`, although HNSW requires the denser `2*M` base
   layer for robust routing.
2. Saturated reverse links retained only the closest neighbors. As later nodes
   were inserted, old nodes commonly pruned the new reverse edge. The new node
   could point into the old graph, but an entry-point search could not traverse
   the missing reverse direction to reach it. This made recent regions weakly
   reachable or unreachable despite `ef_search=96`.
3. Construction lacked the HNSW relative-neighborhood diversity heuristic, so
   local clusters consumed the edge budget with redundant short links instead
   of preserving routes across regions.
4. Cosine distance recalculated two norms for every comparison during both
   construction and search. Besides wasting cycles, build and query paths had
   no explicit shared normalization boundary.

### Ingestion

The original path paid essentially one durability barrier per record:

```text
coordinator (serial endpoint loop)
  -> shard global catalog mutex
     -> collection write mutex per record
        -> open + append + fsync + close per record
        -> HNSW exclusive mutex per record
```

At roughly 4 ms per vector, the measured 235–249 vectors/s is consistent with
1,000 serialized `fsync` calls plus repeated lock and file-descriptor overhead.
The shard catalog mutex also remained held during the expensive work, and the
coordinator sent independent physical-shard batches sequentially.

## Implemented design

### Graph quality and metric consistency

- Base-layer capacity is `2*M`; upper layers remain `M`.
- Both new-neighbor selection and saturated-edge pruning use the HNSW
  diversity heuristic, with rejected candidates used only to fill remaining
  capacity.
- Cosine records and queries are validated and normalized once with a
  double-precision norm accumulator. Traversal uses `1 - dot` over normalized
  vectors.
- Search exactly reranks every candidate returned by the configured
  `ef_search` budget using retained FP32 vectors. SQ8 therefore affects
  traversal but does not become the final ranking score.
- Complete batches are validated before durable mutation. Invalid dimensions,
  non-finite values, zero cosine vectors, and stale generations cannot leave a
  partially accepted semantic batch in the WAL.

### Durable batching and concurrency

- A WAL group commit writes independently checksummed frames with consecutive
  LSNs, performs one `fsync`, and acknowledges only afterward.
- A failed append truncates the unacknowledged tail to its original offset.
- Segment and HNSW batch APIs acquire their respective locks once per batch.
- Shard catalog locks now protect only collection lookup. `shared_ptr`
  lifetime keeps the selected collection valid after releasing the catalog
  lock; deletion and mutations coordinate through a request mutex.
- The coordinator partitions records by physical endpoint and forwards those
  independent streams concurrently, then combines acknowledgements only after
  all streams complete.

This retains serialized mutation inside one HNSW graph—the safe model for the
current mutable adjacency representation—while eliminating unrelated catalog,
network, lock, and durability serialization.

### Correct benchmark semantics

Every stress job now:

1. Copies only the selected collection's schema into a unique temporary
   collection.
2. Generates and durably ingests an exact, known FP32 dataset.
3. Builds a brute-force metric-correct top-K oracle. All distinct source
   queries are covered for datasets up to 2,000 vectors.
4. Measures top-K set intersection, source self-hit separately, throughput,
   errors, and latency quantiles.
5. Excludes oracle-construction time from query latency and QPS.
6. Deletes the temporary collection in `finally`, preserving operator data.

## Final acceptance result

Four consecutive runs of the exact requested workload on the optimized native
Apple Silicon Release stack produced:

| Metric | Observed range | Required |
|---|---:|---:|
| Recall@10 | 100.00% | >= 95.00% |
| Source self-hit | 100.00% | diagnostic only |
| Ingestion | 4,797–5,434 vectors/s | >= 2,000 vectors/s |
| Query throughput | 1,095–1,214 QPS | no regression gate |
| p99 | 5.60–7.81 ms | <= 8.00 ms |
| Errors | 0 | 0 |

The worst value in every acceptance column passes simultaneously; no run used
a reduced `ef_search`, relaxed durability, or lower graph-quality setting.

## Verification gates

With the local Release stack running:

```bash
npm run benchmark
```

The command uses 1,000 vectors, 2,000 queries, concurrency 8, top-K 10, and
`ef_search=96`, then exits nonzero if any performance criterion fails.

Native correctness and gRPC integration:

```bash
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug -j4
ctest --preset macos-arm64-debug --output-on-failure

cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release -j4
ctest --test-dir build/macos-arm64-release-make --output-on-failure

cmake --preset macos-arm64-ubsan
cmake --build --preset macos-arm64-ubsan -j4
ctest --preset macos-arm64-ubsan --output-on-failure
```

Local Linux/ARM64 memory and race instrumentation:

```bash
npm run test:sanitizers
```

That command runs ASan+UBSan and TSan in Docker Desktop's local Linux VM. The
suite includes exact-recall, grouped-WAL recovery, invalid-batch durability,
and concurrent HNSW writer/readers tests. Nothing is deployed or published.
