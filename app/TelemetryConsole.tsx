"use client";

import dynamic from "next/dynamic";
import { useEffect, useMemo, useState } from "react";

const PointCloud = dynamic(
  () => import("./PointCloud").then((module) => module.PointCloud),
  { ssr: false, loading: () => <div className="canvas-loading">initializing WebGL projection…</div> },
);

type NodeState = "healthy" | "migrating";
type ClusterNode = { id: string; role: string; load: string; state: NodeState };

const nodes: ClusterNode[] = [
  { id: "node-sfo-01", role: "shard leader · 4 replicas", load: "71% memory", state: "healthy" },
  { id: "node-sfo-02", role: "shard follower · 4 replicas", load: "58% memory", state: "healthy" },
  { id: "node-sfo-03", role: "handoff learner · shard 08", load: "34% memory", state: "migrating" },
];

function seeded(index: number) {
  const value = Math.sin(index * 12.9898) * 43758.5453;
  return value - Math.floor(value);
}

export function TelemetryConsole() {
  const [selectedNode, setSelectedNode] = useState(0);
  const [running, setRunning] = useState(false);
  const [activeShard, setActiveShard] = useState(8);
  const [now, setNow] = useState("just now");
  const heat = useMemo(() => Array.from({ length: 48 }, (_, index) => 0.18 + seeded(index + 81) * 0.82), []);

  useEffect(() => {
    const timer = window.setInterval(() => setNow(`live · ${new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })}`), 1000);
    return () => window.clearInterval(timer);
  }, []);

  const runQuery = () => {
    setRunning(true);
    window.setTimeout(() => setRunning(false), 1180);
  };

  return (
    <main className="console">
      <header className="topbar">
        <div className="brand"><span className="brand-mark" aria-hidden="true" />VectorDB <span className="subtle">/ control plane</span></div>
        <nav className="topnav" aria-label="Control plane views">
          <button aria-pressed="true">Topology</button><button aria-pressed="false">Queries</button><button aria-pressed="false">Collections</button>
        </nav>
        <div className="status-strip"><span className="status-dot" /> cluster quorum · epoch 4821 <span>{now}</span></div>
      </header>
      <div className="grid">
        <section className="main-column">
          <section className="panel" aria-labelledby="projection-title">
            <div className="panel-heading"><div><div className="eyebrow">3D vector space</div><h1 id="projection-title">Sampled UMAP projection · 50k vectors</h1></div><div className="legend"><span><i />healthy shards</span><span><i className="amber" />migration path</span></div></div>
            <div className="canvas-wrap"><PointCloud activeCluster={selectedNode} /></div>
            <p className="canvas-caption">The projection is a deterministic asynchronous sample. It never runs in a query worker, and its epoch is kept separate from the shard-routing epoch.</p>
          </section>
          <section className="panel query-path" aria-labelledby="query-title">
            <div className="query-header"><div><div className="eyebrow">Scatter gather</div><h2 id="query-title">Query q-7f4a · top-k=10 · cosine</h2></div><button className="action-button" onClick={runQuery}>{running ? "executing…" : "run trace"}</button></div>
            <div className="route-track">
              <div className="route-node active"><small>coordinator</small><strong>route epoch 4821</strong><small>0.18 ms</small></div><div className={`route-line ${running ? "running" : ""}`} /><div className="route-node active"><small>3 logical shards</small><strong>parallel HNSW</strong><small>2.94 ms</small></div><div className={`route-line ${running ? "running" : ""}`} /><div className="route-node"><small>merge + rerank</small><strong>10 stable hits</strong><small>0.61 ms</small></div>
            </div>
          </section>
        </section>
        <aside className="side-column">
          <section className="panel" aria-labelledby="nodes-title"><div className="panel-heading"><div><div className="eyebrow">Cluster members</div><h2 id="nodes-title">3 voting nodes · RF 3</h2></div></div><div className="node-list">{nodes.map((node, index) => <button key={node.id} className={`node-card ${node.state}${selectedNode === index ? " selected" : ""}`} onClick={() => setSelectedNode(index)}><span className="node-dot" /><span><strong>{node.id}</strong><small>{node.role}</small></span><small>{node.load}</small></button>)}</div></section>
          <section className="panel" aria-labelledby="latency-title"><div className="panel-heading"><div><div className="eyebrow">Read path</div><h2 id="latency-title">Tail latency</h2></div><span className="subtle">1k global qps</span></div><div className="metrics"><div className="metric-row"><span className="metric-label">p50</span><span className="metric-value">3.6<span className="subtle"> ms</span></span></div><div className="metric-row"><span className="metric-label">p95</span><span className="metric-value">8.9<span className="subtle"> ms</span></span></div><div className="metric-row"><span className="metric-label">p99</span><span className="metric-value warn">12.4<span className="subtle"> ms</span></span></div></div></section>
          <section className="panel" aria-labelledby="heatmap-title"><div className="panel-heading"><div><div className="eyebrow">Shard pressure</div><h2 id="heatmap-title">Logical shard heatmap</h2></div><span className="subtle">selected: {String(activeShard).padStart(2, "0")}</span></div><div className="heatmap">{heat.map((value, index) => <button key={index} className={activeShard === index ? "active" : ""} style={{ "--heat": value, "--hue": index % 2 } as React.CSSProperties} onClick={() => setActiveShard(index)} aria-label={`Inspect logical shard ${index}`} />)}</div></section>
        </aside>
      </div>
      <footer className="footerbar"><span>engine: NEON SIMD · HNSW / SQ8 · quorum durable</span><span>migration 08: snapshot copy complete · WAL catch-up pending</span></footer>
    </main>
  );
}
