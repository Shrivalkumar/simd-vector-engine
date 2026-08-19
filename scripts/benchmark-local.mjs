const gateway = process.env.VDB_ADMIN_URL ?? "http://127.0.0.1:8090";

function numericEnvironment(name, fallback) {
  if (process.env[name] === undefined) return fallback;
  const value = Number(process.env[name]);
  if (!Number.isFinite(value)) throw new TypeError(`${name} must be numeric`);
  return value;
}

async function request(path, options = {}) {
  const response = await fetch(`${gateway}${path}`, {
    ...options,
    headers: { "Content-Type": "application/json", ...options.headers },
    signal: AbortSignal.timeout(30_000),
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(`${response.status}: ${body.error ?? "request failed"}`);
  return body;
}

async function waitForJob(id) {
  const deadline = Date.now() + numericEnvironment("VDB_BENCHMARK_TIMEOUT_MS", 180_000);
  while (Date.now() < deadline) {
    const { job } = await request(`/api/stress/${id}`);
    if (job.stage === "completed") return job.result;
    if (job.stage === "failed") throw new Error(job.error ?? "stress job failed");
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error("benchmark did not complete before VDB_BENCHMARK_TIMEOUT_MS");
}

async function main() {
  const health = await request("/health");
  if (health.status !== "ok") throw new Error("local cluster is not healthy");
  const { collections } = await request("/api/collections");
  const requestedCollection = process.env.VDB_BENCHMARK_COLLECTION;
  const collection = requestedCollection
    ? collections.find((candidate) => candidate.name === requestedCollection)
    : collections[0];
  if (!collection) {
    throw new Error(requestedCollection
      ? `collection ${requestedCollection} does not exist`
      : "create at least one collection before running the benchmark");
  }

  const workload = {
    collection: collection.name,
    vectorCount: numericEnvironment("VDB_BENCHMARK_VECTORS", 1_000),
    queryCount: numericEnvironment("VDB_BENCHMARK_QUERIES", 2_000),
    concurrency: numericEnvironment("VDB_BENCHMARK_CONCURRENCY", 8),
    topK: numericEnvironment("VDB_BENCHMARK_TOP_K", 10),
    efSearch: numericEnvironment("VDB_BENCHMARK_EF_SEARCH", 96),
  };
  const accepted = await request("/api/stress", {
    method: "POST",
    body: JSON.stringify(workload),
  });
  const result = await waitForJob(accepted.job.id);
  const thresholds = {
    recallAtK: numericEnvironment("VDB_MIN_RECALL", 0.95),
    ingestVectorsPerSecond: numericEnvironment("VDB_MIN_INGEST_VPS", 2_000),
    p99Millis: numericEnvironment("VDB_MAX_P99_MS", 8),
    errors: 0,
  };
  const failures = [];
  if (result.recallAtK < thresholds.recallAtK) {
    failures.push(`Recall@K ${result.recallAtK.toFixed(4)} < ${thresholds.recallAtK.toFixed(4)}`);
  }
  if (result.ingestVectorsPerSecond < thresholds.ingestVectorsPerSecond) {
    failures.push(`ingest ${result.ingestVectorsPerSecond.toFixed(1)} < ${thresholds.ingestVectorsPerSecond.toFixed(1)} vec/s`);
  }
  if (result.p99Millis > thresholds.p99Millis) {
    failures.push(`p99 ${result.p99Millis.toFixed(3)} > ${thresholds.p99Millis.toFixed(3)} ms`);
  }
  if (result.errors !== 0) failures.push(`errors ${result.errors} != 0`);

  process.stdout.write(`${JSON.stringify({ workload, thresholds, result }, null, 2)}\n`);
  if (failures.length > 0) throw new Error(`performance gate failed: ${failures.join("; ")}`);
  process.stdout.write("Performance gate passed.\n");
}

main().catch((error) => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});
