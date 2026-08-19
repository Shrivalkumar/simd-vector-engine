import assert from "node:assert/strict";
import test from "node:test";

import { embedText, encodeFp32, seededVector } from "../services/admin-gateway/embedding.mjs";

function norm(values) {
  return Math.sqrt(values.reduce((total, value) => total + value * value, 0));
}

test("text embeddings are deterministic, normalized, and text-sensitive", () => {
  const first = embedText("cache conscious vector search", 128);
  const repeated = embedText("cache conscious vector search", 128);
  const different = embedText("distributed consensus heartbeat", 128);

  assert.deepEqual(first, repeated);
  assert.equal(first.length, 128);
  assert.ok(Math.abs(norm(first) - 1) < 1e-6);
  assert.notDeepEqual(first, different);
});

test("FP32 vectors are validated and encoded little-endian", () => {
  const encoded = encodeFp32([1.5, -2.25], 2);
  assert.equal(encoded.length, 8);
  assert.equal(encoded.readFloatLE(0), 1.5);
  assert.equal(encoded.readFloatLE(4), -2.25);
  assert.throws(() => encodeFp32([1], 2), /exactly 2/);
  assert.throws(() => encodeFp32([Number.NaN], 1), /not finite/);
});

test("seeded stress vectors are reproducible and normalized", () => {
  const first = seededVector(64, 42, 3);
  const second = seededVector(64, 42, 3);
  assert.deepEqual(first, second);
  assert.ok(Math.abs(norm(first) - 1) < 1e-12);
});
