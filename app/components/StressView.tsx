"use client";

import { useEffect, useState } from "react";

import { api, formatNumber, type CollectionInfo, type StressJob } from "../admin-types";

export function StressView({ collections, onOperation }: { collections: CollectionInfo[]; onOperation: () => void }) {
  const [collection, setCollection] = useState(collections[0]?.name ?? "");
  const [vectorCount, setVectorCount] = useState(1000);
  const [queryCount, setQueryCount] = useState(200);
  const [concurrency, setConcurrency] = useState(8);
  const [topK, setTopK] = useState(10);
  const [efSearch, setEfSearch] = useState(96);
  const [job, setJob] = useState<StressJob | null>(null);
  const [error, setError] = useState("");
  const activeCollection = collections.some((item) => item.name === collection) ? collection : collections[0]?.name ?? "";

  useEffect(() => {
    if (!job || job.stage === "completed" || job.stage === "failed") return;
    const timer = window.setInterval(async () => {
      try {
        const response = await api<{ job: StressJob }>(`/api/stress/${job.id}`);
        setJob(response.job);
        if (response.job.stage === "completed" || response.job.stage === "failed") onOperation();
      } catch (caught) {
        setError(caught instanceof Error ? caught.message : "stress status failed");
      }
    }, 750);
    return () => window.clearInterval(timer);
  }, [job, onOperation]);

  async function start(event: React.FormEvent) {
    event.preventDefault(); setError("");
    try {
      const response = await api<{ job: StressJob }>("/api/stress", { method: "POST", body: JSON.stringify({ collection: activeCollection, vectorCount, queryCount, concurrency, topK, efSearch }) });
      setJob(response.job);
    } catch (caught) { setError(caught instanceof Error ? caught.message : "stress test failed"); }
  }

  const running = job && job.stage !== "completed" && job.stage !== "failed";
  return <div className="workspace-grid stress-workspace">
    <section className="panel form-panel"><div className="panel-heading"><div><div className="eyebrow">Load generator</div><h1>Stress test the real cluster</h1></div></div>
      <form className="form-stack" onSubmit={start}>
        <label>Collection<select value={activeCollection} onChange={(event) => setCollection(event.target.value)}>{collections.map((item) => <option key={item.name}>{item.name}</option>)}</select></label>
        <div className="form-row"><label>Vectors to ingest<input type="number" min={1} max={20000} value={vectorCount} onChange={(event) => setVectorCount(Number(event.target.value))} /></label><label>Search queries<input type="number" min={1} max={5000} value={queryCount} onChange={(event) => setQueryCount(Number(event.target.value))} /></label></div>
        <div className="form-row"><label>Concurrency<input type="number" min={1} max={64} value={concurrency} onChange={(event) => setConcurrency(Number(event.target.value))} /></label><label>Top K<input type="number" min={1} max={100} value={topK} onChange={(event) => setTopK(Number(event.target.value))} /></label></div>
        <label>ef_search<input type="number" min={topK} max={2000} value={efSearch} onChange={(event) => setEfSearch(Number(event.target.value))} /></label>
        <p className="form-note">Runs in an isolated temporary collection, performs durable routed upserts, then issues concurrent coordinator searches. Recall@K is exact Top-K overlap against a full brute-force oracle through 2,000 vectors and a bounded sample above that; test data is removed afterward.</p>
        {error ? <div className="alert error">{error}</div> : null}
        <button className="primary-button" disabled={Boolean(running) || !activeCollection}>{running ? "Test running…" : "Start measured load test"}</button>
      </form>
    </section>
    <section className="panel stress-results"><div className="panel-heading"><div><div className="eyebrow">Benchmark result</div><h2>{job ? job.stage : "No active run"}</h2></div>{job ? <span className="live-label">{formatNumber(job.progress * 100, 0)}%</span> : null}</div>
      {job ? <div className="job-body"><div className="progress-track"><span style={{ width: `${job.progress * 100}%` }} /></div>{job.error ? <div className="alert error">{job.error}</div> : null}{job.result ? <ResultGrid result={job.result} /> : <div className="empty-state"><strong>{job.stage}</strong><span>{job.stage === "ingesting" ? "Durably writing generated vectors…" : job.stage === "querying" ? "Issuing concurrent searches and collecting latency…" : "Waiting for workers…"}</span></div>}</div> : <div className="empty-state"><strong>No benchmark yet</strong><span>Configure a bounded local test to measure ingestion, throughput, recall, and tail latency.</span></div>}
    </section>
  </div>;
}

function ResultGrid({ result }: { result: NonNullable<StressJob["result"]> }) {
  const metrics = [
    ["Ingest rate", `${formatNumber(result.ingestVectorsPerSecond)} vec/s`],
    ["Query rate", `${formatNumber(result.queriesPerSecond)} qps`],
    ["Recall@K", `${formatNumber(result.recallAtK * 100, 2)}%`],
    ["Self-hit rate", `${formatNumber(result.selfHitRate * 100, 2)}%`],
    ["Errors", formatNumber(result.errors, 0)],
    ["p50", `${formatNumber(result.p50Millis)} ms`],
    ["p95", `${formatNumber(result.p95Millis)} ms`],
    ["p99", `${formatNumber(result.p99Millis)} ms`],
    ["Max", `${formatNumber(result.maxMillis)} ms`],
  ];
  return <div className="benchmark-grid">{metrics.map(([label, value]) => <div key={label}><span>{label}</span><strong>{value}</strong></div>)}</div>;
}
