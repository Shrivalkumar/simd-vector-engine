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
  assert.match(html, /Live topology and search telemetry for VectorDB\./);
  assert.match(html, /VectorDB\s*<span[^>]*>\/ control plane/);
  assert.match(html, /Sampled UMAP projection · 50k vectors/);
  assert.match(html, /3 voting nodes · RF 3/);
  assert.match(html, /Query q-7f4a · top-k=10 · cosine/);
  assert.doesNotMatch(html, /Your site is taking shape|react-loading-skeleton/i);
});

test("keeps dashboard metadata and visualization boundaries intentional", async () => {
  const [page, layout, consoleSource, packageJson] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/layout.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/TelemetryConsole.tsx", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(page, /import \{ TelemetryConsole \}/);
  assert.match(page, /<TelemetryConsole \/>/);
  assert.match(layout, /title: "VectorDB Control Plane"/);
  assert.match(layout, /summary_large_image/);
  assert.match(layout, /"\/og\.png"/);
  assert.match(consoleSource, /"use client"/);
  assert.match(consoleSource, /dynamic\(\s*\(\) => import\("\.\/PointCloud"\)/s);
  assert.match(consoleSource, /deterministic asynchronous sample/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);

  await assert.rejects(access(new URL("app/_sites-preview/SkeletonPreview.tsx", templateRoot)));
});
