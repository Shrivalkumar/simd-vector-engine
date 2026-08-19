import { randomUUID } from "node:crypto";
import { fileURLToPath } from "node:url";
import path from "node:path";

import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";
import express from "express";

import { embedText, encodeFp32, seededVector } from "./embedding.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const coordinatorAddress = process.env.VDB_COORDINATOR_ADDRESS ?? "127.0.0.1:7100";
const shardAddresses = (process.env.VDB_SHARD_ADDRESSES ?? "127.0.0.1:7001,127.0.0.1:7002,127.0.0.1:7003")
  .split(",")
  .map((address) => address.trim())
  .filter(Boolean);
const port = Number.parseInt(process.env.VDB_ADMIN_PORT ?? "8090", 10);
const protoFiles = [
  path.join(root, "proto/vectordb/v1/admin.proto"),
  path.join(root, "proto/vectordb/v1/search.proto"),
  path.join(root, "proto/vectordb/v1/cluster.proto"),
];
const definition = protoLoader.loadSync(protoFiles, {
  includeDirs: [path.join(root, "proto")],
  keepCase: false,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true,
});
const proto = grpc.loadPackageDefinition(definition).vectordb.v1;
const credentials = grpc.credentials.createInsecure();
const dataClient = new proto.VectorData(coordinatorAddress, credentials);
const adminClient = new proto.CollectionAdmin(coordinatorAddress, credentials);
const shardAdminClients = shardAddresses.map((address) => ({
  address,
  client: new proto.CollectionAdmin(address, credentials),
}));

const recentOperations = [];
const stressJobs = new Map();
const startedAt = Date.now();
const uint64Max = (1n << 64n) - 1n;

function boundedInteger(value, fallback, minimum, maximum, label) {
  const number = value === undefined ? fallback : Number(value);
  if (!Number.isInteger(number) || number < minimum || number > maximum) {
    throw new RangeError(`${label} must be an integer between ${minimum} and ${maximum}`);
  }
  return number;
}

function positiveUint64(value, label) {
  if (typeof value === "number" && !Number.isSafeInteger(value)) {
    throw new RangeError(`${label} must be a safe JSON integer or a decimal string`);
  }
  const encoded = String(value);
  if (!/^\d+$/.test(encoded)) throw new TypeError(`${label} must be an unsigned integer`);
  const parsed = BigInt(encoded);
  if (parsed === 0n || parsed > uint64Max) throw new RangeError(`${label} must be between 1 and 2^64-1`);
  return encoded;
}

function httpError(status, message) {
  const error = new Error(message);
  error.status = status;
  return error;
}

function validateStressOptions(options) {
  if (options === null || Array.isArray(options) || typeof options !== "object") {
    throw new TypeError("stress body must be an object");
  }
  if (typeof options.collection !== "string" || options.collection.length === 0) {
    throw new TypeError("collection must be a non-empty string");
  }
  const topK = boundedInteger(options.topK, 10, 1, 100, "topK");
  return {
    collection: options.collection,
    vectorCount: boundedInteger(options.vectorCount, 1_000, 1, 20_000, "vectorCount"),
    queryCount: boundedInteger(options.queryCount, 200, 1, 5_000, "queryCount"),
    concurrency: boundedInteger(options.concurrency, 8, 1, 64, "concurrency"),
    topK,
    efSearch: boundedInteger(options.efSearch, 96, topK, 2_000, "efSearch"),
  };
}

function unary(client, method, request, deadlineMillis = 5_000) {
  return new Promise((resolve, reject) => {
    client[method](request, { deadline: Date.now() + deadlineMillis }, (error, response) => {
      if (error) reject(error);
      else resolve(response);
    });
  });
}

function remember(operation) {
  recentOperations.unshift({ at: new Date().toISOString(), ...operation });
  if (recentOperations.length > 200) recentOperations.length = 200;
}

function elapsedMillis(started) {
  return Number(process.hrtime.bigint() - started) / 1_000_000;
}

function percentile(values, quantile) {
  if (values.length === 0) return 0;
  const ordered = values.toSorted((left, right) => left - right);
  return ordered[Math.min(ordered.length - 1, Math.ceil(ordered.length * quantile) - 1)];
}

function metricToProto(metric) {
  const values = {
    cosine: "DISTANCE_METRIC_COSINE",
    l2: "DISTANCE_METRIC_L2_SQUARED",
    dot: "DISTANCE_METRIC_DOT_PRODUCT",
  };
  if (!values[metric]) throw new TypeError("metric must be cosine, l2, or dot");
  return values[metric];
}

function metricFromProto(metric) {
  return {
    DISTANCE_METRIC_COSINE: "cosine",
    DISTANCE_METRIC_L2_SQUARED: "l2",
    DISTANCE_METRIC_DOT_PRODUCT: "dot",
  }[metric] ?? "cosine";
}

function mapCollection(collection) {
  return {
    name: collection.name,
    dimensions: collection.dimensions,
    metric: metricFromProto(collection.metric),
    liveVectors: Number(collection.liveVectors),
    residentBytes: Number(collection.residentBytes),
  };
}

async function probeNodes() {
  return Promise.all(shardAdminClients.map(async ({ address, client }, index) => {
    const started = process.hrtime.bigint();
    try {
      const result = await unary(client, "listCollections", {}, 500);
      return {
        id: `node-${String.fromCharCode(97 + index)}`,
        endpoint: address,
        state: "healthy",
        latencyMillis: elapsedMillis(started),
        collectionCount: result.collections.length,
      };
    } catch (error) {
      return {
        id: `node-${String.fromCharCode(97 + index)}`,
        endpoint: address,
        state: "unavailable",
        latencyMillis: elapsedMillis(started),
        collectionCount: 0,
        message: error.details ?? error.message,
      };
    }
  }));
}

function payloadValue(value) {
  if (typeof value === "string") return { stringValue: value };
  if (typeof value === "boolean") return { boolValue: value };
  if (typeof value === "number" && Number.isSafeInteger(value)) return { intValue: String(value) };
  if (typeof value === "number" && Number.isFinite(value)) return { doubleValue: value };
  throw new TypeError("payload values must be strings, booleans, or finite numbers");
}

function encodePayload(payload = {}) {
  if (payload === null || Array.isArray(payload) || typeof payload !== "object") {
    throw new TypeError("payload must be an object");
  }
  return Object.entries(payload).map(([key, value]) => {
    if (!key) throw new TypeError("payload keys cannot be empty");
    return { key, value: payloadValue(value) };
  });
}

function decodePayload(payload = []) {
  return Object.fromEntries(payload.map((field) => {
    const value = field.value ?? {};
    const decoded = value.stringValue ?? value.intValue ?? value.doubleValue ?? value.boolValue ?? null;
    return [field.key, decoded];
  }));
}

async function describeCollection(name) {
  const response = await unary(adminClient, "describeCollection", { name });
  return mapCollection(response.collection);
}

function batchUpsert(request) {
  return new Promise((resolve, reject) => {
    const stream = dataClient.batchUpsert(new grpc.Metadata(), { deadline: Date.now() + 30_000 });
    const acknowledgements = [];
    stream.on("data", (ack) => acknowledgements.push(ack));
    stream.on("error", reject);
    stream.on("end", () => resolve(acknowledgements));
    stream.write(request);
    stream.end();
  });
}

async function upsertRecords(collection, sourceRecords, requestPrefix = "admin") {
  const info = await describeCollection(collection);
  const records = sourceRecords.map((record, index) => {
    if (record === null || Array.isArray(record) || typeof record !== "object") {
      throw new TypeError(`record ${index} must be an object`);
    }
    const id = positiveUint64(record.id === undefined ? Date.now() * 1000 + index : record.id, `record ${index} id`);
    const generation = positiveUint64(record.generation ?? 1, `record ${index} generation`);
    const vectorValues = record.vector ?? embedText(record.text, info.dimensions);
    const payload = { ...(record.payload ?? {}) };
    if (record.text) payload.text = record.text;
    return {
      id,
      generation,
      vector: { dimension: info.dimensions, fp32Le: encodeFp32(vectorValues, info.dimensions) },
      payload: encodePayload(payload),
    };
  });

  let applied = 0;
  let committedIndex = "0";
  const errors = [];
  for (let offset = 0; offset < records.length; offset += 256) {
    const chunk = records.slice(offset, offset + 256);
    const acknowledgements = await batchUpsert({
      collection,
      idempotencyKey: Buffer.from(`${requestPrefix}-${randomUUID()}-${offset}`),
      sequence: String(offset / 256 + 1),
      records: chunk,
      consistency: "CONSISTENCY_QUORUM",
    });
    for (const ack of acknowledgements) {
      applied += ack.appliedCount;
      if (BigInt(ack.committedIndex) > BigInt(committedIndex)) committedIndex = ack.committedIndex;
      errors.push(...ack.errors);
    }
  }
  if (errors.length > 0) throw new Error(errors.map((error) => error.message).join("; "));
  return { applied, committedIndex, records };
}

async function searchCollection(collection, body, track = true) {
  if (body === null || Array.isArray(body) || typeof body !== "object") {
    throw new TypeError("search body must be an object");
  }
  const info = await describeCollection(collection);
  const values = body.vector ?? embedText(body.text, info.dimensions);
  const topK = boundedInteger(body.topK, 10, 1, 100, "topK");
  const efSearch = boundedInteger(body.efSearch, 96, topK, 2_000, "efSearch");
  const rerankCandidates = boundedInteger(body.rerankCandidates, 100, topK, 5_000, "rerankCandidates");
  const timeoutMillis = boundedInteger(body.timeoutMillis, 2_000, 1, 30_000, "timeoutMillis");
  const started = process.hrtime.bigint();
  try {
    const response = await unary(dataClient, "vectorSearch", {
      collection,
      vector: { dimension: info.dimensions, fp32Le: encodeFp32(values, info.dimensions) },
      topK,
      efSearch,
      rerankCandidates,
      consistency: "CONSISTENCY_EVENTUAL",
      timeoutMillis: String(timeoutMillis),
    }, timeoutMillis + 1_000);
    const latencyMillis = elapsedMillis(started);
    const result = {
      latencyMillis,
      shardEpoch: response.shardIndex,
      embedding: body.text ? "local-feature-hash" : "provided-vector",
      hits: response.hits.map((hit) => ({
        id: hit.id,
        generation: hit.generation,
        distance: hit.distance,
        payload: decodePayload(hit.payload),
      })),
    };
    if (track) remember({ type: "search", collection, latencyMillis, status: "ok", hitCount: result.hits.length });
    return result;
  } catch (error) {
    if (track) remember({ type: "search", collection, latencyMillis: elapsedMillis(started), status: "error", message: error.message });
    throw error;
  }
}

function exactDistance(left, right, metric) {
  let dot = 0;
  let leftNorm = 0;
  let rightNorm = 0;
  let l2 = 0;
  for (let index = 0; index < left.length; index += 1) {
    const delta = left[index] - right[index];
    l2 += delta * delta;
    dot += left[index] * right[index];
    if (metric === "cosine") {
      leftNorm += left[index] * left[index];
      rightNorm += right[index] * right[index];
    }
  }
  if (metric === "l2") return l2;
  if (metric === "dot") return -dot;
  return 1 - dot / Math.sqrt(leftNorm * rightNorm);
}

async function buildExactGroundTruth(records, metric, topK, querySamples) {
  const result = [];
  const exactCount = Math.min(topK, records.length);
  for (let queryIndex = 0; queryIndex < querySamples; queryIndex += 1) {
    const query = records[queryIndex].vector;
    const candidates = records.map((record) => ({
      id: record.id,
      distance: exactDistance(query, record.vector, metric),
    }));
    candidates.sort((left, right) => {
      const distanceOrder = left.distance - right.distance;
      if (distanceOrder !== 0) return distanceOrder;
      const leftId = BigInt(left.id);
      const rightId = BigInt(right.id);
      return leftId < rightId ? -1 : leftId > rightId ? 1 : 0;
    });
    result.push(new Set(candidates.slice(0, exactCount).map((candidate) => candidate.id)));
    if (queryIndex % 4 === 3) await new Promise((resolve) => setImmediate(resolve));
  }
  return { neighbors: result, exactCount };
}

async function runStress(job, options) {
  const info = await describeCollection(options.collection);
  const vectorCount = boundedInteger(options.vectorCount, 1_000, 1, 20_000, "vectorCount");
  const queryCount = boundedInteger(options.queryCount, 200, 1, 5_000, "queryCount");
  const concurrency = boundedInteger(options.concurrency, 8, 1, 64, "concurrency");
  const topK = boundedInteger(options.topK, 10, 1, 100, "topK");
  const efSearch = boundedInteger(options.efSearch, 96, topK, 2_000, "efSearch");
  const baseId = BigInt(Date.now()) * 1_000_000n;
  const runSeed = [...job.id].reduce((value, character) => (
    Math.imul(value ^ character.charCodeAt(0), 16_777_619) >>> 0
  ), 2_166_136_261);
  const benchmarkCollection = `stress_${job.id.replaceAll("-", "")}`;
  const records = Array.from({ length: vectorCount }, (_, index) => ({
    id: String(baseId + BigInt(index + 1)),
    generation: 1,
    vector: seededVector(info.dimensions, (runSeed + index + 1) >>> 0, index % 16).map(Math.fround),
    payload: { source: "stress", run: job.id, sequence: index, cluster: index % 16 },
  }));
  let benchmarkResult;
  let created = false;
  try {
    await unary(adminClient, "createCollection", {
      name: benchmarkCollection,
      dimensions: info.dimensions,
      metric: metricToProto(info.metric),
      hnswM: 32,
      efConstruction: 200,
    });
    created = true;
    job.stage = "ingesting";
    const ingestStarted = process.hrtime.bigint();
    const upsert = await upsertRecords(benchmarkCollection, records, `stress-${job.id}`);
    const ingestMillis = elapsedMillis(ingestStarted);
    job.progress = 0.45;

    const recallQueries = Math.min(queryCount, vectorCount, vectorCount <= 2_000 ? vectorCount : 100);
    const groundTruthStarted = process.hrtime.bigint();
    const groundTruth = await buildExactGroundTruth(records, info.metric, topK, recallQueries);
    const groundTruthMillis = elapsedMillis(groundTruthStarted);
    job.stage = "querying";

    const latencies = [];
    let nextQuery = 0;
    let errors = 0;
    let exactMatches = 0;
    let selfHits = 0;
    const workers = Array.from({ length: concurrency }, async () => {
      while (true) {
        const queryIndex = nextQuery;
        nextQuery += 1;
        if (queryIndex >= queryCount) return;
        const recordIndex = queryIndex % records.length;
        try {
          const result = await searchCollection(benchmarkCollection, {
            vector: records[recordIndex].vector,
            topK,
            efSearch,
            timeoutMillis: 5_000,
          }, false);
          latencies.push(result.latencyMillis);
          if (result.hits.some((hit) => hit.id === records[recordIndex].id)) selfHits += 1;
          if (queryIndex < recallQueries) {
            const expected = groundTruth.neighbors[queryIndex];
            for (const hit of result.hits) exactMatches += Number(expected.has(hit.id));
          }
        } catch {
          errors += 1;
        }
        job.progress = 0.45 + 0.55 * (nextQuery / queryCount);
      }
    });
    const queryStarted = process.hrtime.bigint();
    await Promise.all(workers);
    const queryMillis = elapsedMillis(queryStarted);
    benchmarkResult = {
      collection: options.collection,
      isolated: true,
      vectorCount: upsert.applied,
      queryCount,
      concurrency,
      topK,
      efSearch,
      dimensions: info.dimensions,
      ingestMillis,
      ingestVectorsPerSecond: upsert.applied / (ingestMillis / 1000),
      groundTruthMillis,
      recallQueries,
      queryMillis,
      queriesPerSecond: (queryCount - errors) / (queryMillis / 1000),
      errors,
      recallAtK: exactMatches / (recallQueries * groundTruth.exactCount),
      selfHitRate: selfHits / queryCount,
      p50Millis: percentile(latencies, 0.5),
      p95Millis: percentile(latencies, 0.95),
      p99Millis: percentile(latencies, 0.99),
      maxMillis: latencies.length === 0 ? 0 : Math.max(...latencies),
    };
  } finally {
    if (created) await unary(adminClient, "deleteCollection", { name: benchmarkCollection }, 10_000);
  }
  job.result = benchmarkResult;
  job.progress = 1;
  job.stage = "completed";
  job.finishedAt = new Date().toISOString();
  remember({ type: "stress", collection: options.collection, latencyMillis: job.result.queryMillis, status: "ok",
             queryCount, p99Millis: job.result.p99Millis });
}

const app = express();
app.disable("x-powered-by");
app.use(express.json({ limit: "32mb" }));
app.use((request, response, next) => {
  const origin = request.headers.origin;
  if (origin && /^http:\/\/(localhost|127\.0\.0\.1):3000$/.test(origin)) {
    response.setHeader("Access-Control-Allow-Origin", origin);
    response.setHeader("Vary", "Origin");
  }
  response.setHeader("Access-Control-Allow-Headers", "Content-Type");
  response.setHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  if (request.method === "OPTIONS") response.sendStatus(204);
  else next();
});

app.get("/health", async (_request, response, next) => {
  try {
    const [collections, nodes] = await Promise.all([
      unary(adminClient, "listCollections", {}),
      probeNodes(),
    ]);
    response.json({ status: "ok", coordinator: coordinatorAddress, collections: collections.collections.length,
                    nodes, uptimeSeconds: Math.floor((Date.now() - startedAt) / 1000) });
  } catch (error) { next(error); }
});

app.get("/api/collections", async (_request, response, next) => {
  try {
    const result = await unary(adminClient, "listCollections", {});
    response.json({ collections: result.collections.map(mapCollection) });
  } catch (error) { next(error); }
});

app.post("/api/collections", async (request, response, next) => {
  try {
    const started = process.hrtime.bigint();
    const result = await unary(adminClient, "createCollection", {
      name: request.body.name,
      dimensions: boundedInteger(request.body.dimensions, 128, 1, 65_536, "dimensions"),
      metric: metricToProto(request.body.metric ?? "cosine"),
      hnswM: boundedInteger(request.body.hnswM, 32, 2, 128, "hnswM"),
      efConstruction: boundedInteger(request.body.efConstruction, 200, 4, 2_000, "efConstruction"),
    });
    const collection = mapCollection(result.collection);
    remember({ type: "create_collection", collection: collection.name, latencyMillis: elapsedMillis(started), status: "ok" });
    response.status(201).json({ collection });
  } catch (error) { next(error); }
});

app.delete("/api/collections/:name", async (request, response, next) => {
  try {
    const result = await unary(adminClient, "deleteCollection", { name: request.params.name });
    remember({ type: "delete_collection", collection: request.params.name, latencyMillis: 0, status: "ok" });
    response.json({ deleted: result.deleted });
  } catch (error) { next(error); }
});

app.post("/api/collections/:name/upsert", async (request, response, next) => {
  const started = process.hrtime.bigint();
  try {
    if (!Array.isArray(request.body.records) || request.body.records.length === 0) {
      throw new TypeError("records must be a non-empty array");
    }
    if (request.body.records.length > 20_000) throw new RangeError("one upload is limited to 20000 records");
    const result = await upsertRecords(request.params.name, request.body.records);
    const latencyMillis = elapsedMillis(started);
    remember({ type: "upsert", collection: request.params.name, latencyMillis, status: "ok", count: result.applied });
    response.json({ applied: result.applied, committedIndex: result.committedIndex, latencyMillis });
  } catch (error) { next(error); }
});

app.post("/api/collections/:name/search", async (request, response, next) => {
  try { response.json(await searchCollection(request.params.name, request.body)); }
  catch (error) { next(error); }
});

app.post("/api/stress", async (request, response, next) => {
  try {
    const options = validateStressOptions(request.body);
    const activeJobs = [...stressJobs.values()].filter((job) => !["completed", "failed"].includes(job.stage));
    if (activeJobs.length >= 2) throw httpError(429, "at most two stress jobs may run concurrently");
    await describeCollection(options.collection);
    const id = randomUUID();
    const job = { id, stage: "queued", progress: 0, startedAt: new Date().toISOString(), result: null, error: null };
    stressJobs.set(id, job);
    if (stressJobs.size > 100) {
      const oldestFinished = [...stressJobs.entries()].find(([, candidate]) => ["completed", "failed"].includes(candidate.stage));
      if (oldestFinished) stressJobs.delete(oldestFinished[0]);
    }
    runStress(job, options).catch((error) => {
      job.stage = "failed";
      job.error = error.message;
      job.finishedAt = new Date().toISOString();
      remember({ type: "stress", collection: options.collection, latencyMillis: 0, status: "error", message: error.message });
    });
    response.status(202).json({ job });
  } catch (error) { next(error); }
});

app.get("/api/stress/:id", (request, response) => {
  const job = stressJobs.get(request.params.id);
  if (!job) response.status(404).json({ error: "stress job not found" });
  else response.json({ job });
});

app.get("/api/telemetry", async (_request, response, next) => {
  try {
    const [result, nodes] = await Promise.all([
      unary(adminClient, "listCollections", {}),
      probeNodes(),
    ]);
    const searches = recentOperations.filter((operation) => operation.type === "search" && operation.status === "ok");
    const latencies = searches.map((operation) => operation.latencyMillis);
    response.json({
      generatedAt: new Date().toISOString(),
      uptimeSeconds: Math.floor((Date.now() - startedAt) / 1000),
      coordinator: coordinatorAddress,
      nodes,
      collections: result.collections.map(mapCollection),
      latency: { p50Millis: percentile(latencies, 0.5), p95Millis: percentile(latencies, 0.95),
                 p99Millis: percentile(latencies, 0.99), samples: latencies.length },
      recentOperations: recentOperations.slice(0, 50),
      stressJobs: [...stressJobs.values()].slice(-10).toReversed(),
    });
  } catch (error) { next(error); }
});

app.use((error, _request, response, next) => {
  void next;
  const status = error.status
    ?? (error instanceof TypeError || error instanceof RangeError || error instanceof SyntaxError ? 400 : undefined)
    ?? (error.code === grpc.status.NOT_FOUND ? 404 : undefined)
    ?? (error.code === grpc.status.ALREADY_EXISTS ? 409 : undefined)
    ?? (error.code === grpc.status.INVALID_ARGUMENT ? 400 : undefined)
    ?? (error.code === grpc.status.RESOURCE_EXHAUSTED ? 429 : undefined)
    ?? (error.code === grpc.status.UNAVAILABLE ? 503 : undefined)
    ?? (error.code === grpc.status.DEADLINE_EXCEEDED ? 504 : 500);
  response.status(status).json({ error: error.details ?? error.message ?? "request failed" });
});

async function bootstrap() {
  for (let attempt = 0; attempt < 40; attempt += 1) {
    try {
      const result = await unary(adminClient, "listCollections", {}, 500);
      if (result.collections.length === 0) {
        await unary(adminClient, "createCollection", {
          name: "documents", dimensions: 128, metric: "DISTANCE_METRIC_COSINE", hnswM: 32, efConstruction: 200,
        });
      }
      return;
    } catch (error) {
      if (attempt === 39) throw error;
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
}

await bootstrap();
app.listen(port, "127.0.0.1", () => {
  process.stdout.write(`VectorDB admin gateway listening on http://127.0.0.1:${port}\n`);
});
