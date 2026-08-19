import assert from "node:assert/strict";
import test from "node:test";

const gateway = process.env.VDB_ADMIN_URL ?? "http://127.0.0.1:8090";

async function request(path, options = {}) {
  const response = await fetch(`${gateway}${path}`, {
    ...options,
    headers: { "Content-Type": "application/json", ...options.headers },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(`${response.status}: ${body.error ?? "request failed"}`);
  return body;
}

async function waitForStress(id) {
  for (let attempt = 0; attempt < 120; attempt += 1) {
    const { job } = await request(`/api/stress/${id}`);
    if (job.stage === "completed") return job;
    if (job.stage === "failed") throw new Error(job.error);
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("stress test did not complete within 12 seconds");
}

test("local administration console completes collection, ingestion, search, telemetry, and stress flows", async () => {
  const collection = `e2e_${process.pid}_${Date.now()}`;
  try {
    const health = await request("/health");
    assert.equal(health.status, "ok");
    assert.ok(health.nodes.every((node) => node.state === "healthy"));

    const created = await request("/api/collections", {
      method: "POST",
      body: JSON.stringify({ name: collection, dimensions: 32, metric: "cosine" }),
    });
    assert.equal(created.collection.name, collection);

    const upserted = await request(`/api/collections/${collection}/upsert`, {
      method: "POST",
      body: JSON.stringify({ records: [
        { id: "1001", text: "apple silicon SIMD vector kernels", payload: { kind: "hardware" } },
        { id: "1002", text: "hierarchical navigable small world graph", payload: { kind: "index" } },
        { id: "1003", text: "distributed shard coordinator heartbeat", payload: { kind: "cluster" } },
      ] }),
    });
    assert.equal(upserted.applied, 3);

    const searched = await request(`/api/collections/${collection}/search`, {
      method: "POST",
      body: JSON.stringify({ text: "apple silicon vector SIMD", topK: 2, efSearch: 64 }),
    });
    assert.equal(searched.hits[0].id, "1001");
    assert.equal(searched.hits[0].payload.kind, "hardware");
    assert.ok(searched.latencyMillis > 0);

    const accepted = await request("/api/stress", {
      method: "POST",
      body: JSON.stringify({ collection, vectorCount: 64, queryCount: 20, concurrency: 4, topK: 10, efSearch: 64 }),
    });
    const job = await waitForStress(accepted.job.id);
    assert.equal(job.result.errors, 0);
    assert.equal(job.result.vectorCount, 64);
    assert.equal(job.result.queryCount, 20);
    assert.equal(job.result.isolated, true);
    assert.ok(job.result.recallQueries > 0);
    assert.ok(job.result.recallAtK >= 0.95);
    assert.ok(job.result.queriesPerSecond > 0);

    const telemetry = await request("/api/telemetry");
    assert.ok(telemetry.collections.some((item) => item.name === collection));
    assert.ok(telemetry.recentOperations.some((operation) => operation.type === "stress"));
  } finally {
    await request(`/api/collections/${collection}`, { method: "DELETE" }).catch(() => {});
  }
});
