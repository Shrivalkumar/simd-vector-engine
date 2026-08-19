import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

const templateRoot = new URL("../", import.meta.url);

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", { headers: { accept: "text/html" } }),
    {
      ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) },
    },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the VectorDB control-plane shell", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>VectorDB Control Plane<\/title>/i);
  assert.match(html, /Local operational administration, ingestion, search, and load testing for VectorDB\./);
  assert.match(html, /VectorDB\s*<small[^>]*>\/ operations/);
  assert.match(html, /Operational cluster overview/);
  assert.match(html, /Queries/);
  assert.match(html, /Collections/);
  assert.match(html, /Stress tests/);
  assert.match(html, /measured telemetry only/);
  assert.doesNotMatch(html, /Your site is taking shape|react-loading-skeleton/i);
});

test("keeps operational capabilities and visualization boundaries intentional", async () => {
  const [page, layout, consoleSource, topology, query, collections, stress, gateway, packageJson] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/layout.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/TelemetryConsole.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/components/TopologyView.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/components/QueryView.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/components/CollectionsView.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/components/StressView.tsx", import.meta.url), "utf8"),
    readFile(new URL("../services/admin-gateway/server.mjs", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(page, /import \{ TelemetryConsole \}/);
  assert.match(page, /<TelemetryConsole \/>/);
  assert.match(layout, /title: "VectorDB Control Plane"/);
  assert.match(layout, /summary_large_image/);
  assert.match(layout, /"\/og\.png"/);
  assert.match(consoleSource, /"use client"/);
  assert.match(consoleSource, /"queries"/);
  assert.match(consoleSource, /"collections"/);
  assert.match(consoleSource, /"stress"/);
  assert.match(consoleSource, /view-shell \$\{view\}/);
  assert.match(topology, /dynamic\(\s*\(\) => import\("\.\.\/PointCloud"\)/s);
  assert.match(query, /\/search/);
  assert.match(collections, /\/upsert/);
  assert.match(stress, /\/api\/stress/);
  assert.match(gateway, /runStress/);
  assert.match(gateway, /batchUpsert/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);

  await assert.rejects(access(new URL("app/_sites-preview/SkeletonPreview.tsx", templateRoot)));
});
