"use client";

import { useState } from "react";

import { api, formatNumber, type CollectionInfo, type SearchResult } from "../admin-types";

export function QueryView({ collections, onOperation }: { collections: CollectionInfo[]; onOperation: () => void }) {
  const [collection, setCollection] = useState(collections[0]?.name ?? "");
  const [mode, setMode] = useState<"text" | "vector">("text");
  const [query, setQuery] = useState("distributed vector search");
  const [topK, setTopK] = useState(10);
  const [efSearch, setEfSearch] = useState(96);
  const [result, setResult] = useState<SearchResult | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);

  const activeCollection = collections.some((item) => item.name === collection) ? collection : collections[0]?.name ?? "";

  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (!activeCollection) return;
    setLoading(true);
    setError("");
    try {
      const body = mode === "text"
        ? { text: query, topK, efSearch }
        : { vector: JSON.parse(query), topK, efSearch };
      const response = await api<SearchResult>(`/api/collections/${encodeURIComponent(activeCollection)}/search`, {
        method: "POST", body: JSON.stringify(body),
      });
      setResult(response);
      onOperation();
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : "search failed");
    } finally {
      setLoading(false);
    }
  }

  return (
    <div className="workspace-grid">
      <section className="panel form-panel">
        <div className="panel-heading"><div><div className="eyebrow">Query workbench</div><h1>Search stored vectors</h1></div></div>
        <form className="form-stack" onSubmit={submit}>
          <label>Collection<select value={activeCollection} onChange={(event) => setCollection(event.target.value)} disabled={collections.length === 0}>{collections.map((item) => <option key={item.name}>{item.name}</option>)}</select></label>
          <div className="segmented" role="group" aria-label="Query input mode">
            <button type="button" aria-pressed={mode === "text"} onClick={() => { setMode("text"); setQuery("distributed vector search"); }}>Text</button>
            <button type="button" aria-pressed={mode === "vector"} onClick={() => { setMode("vector"); setQuery("[]"); }}>Vector JSON</button>
          </div>
          <label>{mode === "text" ? "Search text" : "Vector array"}<textarea rows={8} value={query} onChange={(event) => setQuery(event.target.value)} placeholder={mode === "text" ? "Enter natural-language search text" : "[0.1, 0.2, ...]"} /></label>
          <div className="form-row"><label>Top K<input type="number" min={1} max={100} value={topK} onChange={(event) => setTopK(Number(event.target.value))} /></label><label>ef_search<input type="number" min={topK} max={2000} value={efSearch} onChange={(event) => setEfSearch(Number(event.target.value))} /></label></div>
          {mode === "text" ? <p className="form-note">Text is embedded locally with deterministic feature hashing. It is offline and useful for lexical similarity, but it is not a transformer semantic model.</p> : null}
          {error ? <div className="alert error">{error}</div> : null}
          <button className="primary-button" disabled={loading || !activeCollection}>{loading ? "Searching…" : "Run distributed search"}</button>
        </form>
      </section>

      <section className="panel results-panel">
        <div className="panel-heading"><div><div className="eyebrow">Results</div><h2>{result ? `${result.hits.length} nearest records` : "Awaiting query"}</h2></div>{result ? <span className="live-label">{formatNumber(result.latencyMillis)} ms</span> : null}</div>
        {result ? <>
          <div className="query-trace"><div><small>Browser gateway</small><strong>validated</strong></div><span /><div><small>Coordinator</small><strong>epoch {result.shardEpoch}</strong></div><span /><div><small>HNSW shards</small><strong>{result.embedding}</strong></div></div>
          <div className="result-list">{result.hits.map((hit, index) => <article className="result-card" key={hit.id}><div className="rank">{index + 1}</div><div><strong>Record {hit.id}</strong><small>generation {hit.generation}</small><pre>{JSON.stringify(hit.payload, null, 2)}</pre></div><div className="distance"><span>distance</span><strong>{formatNumber(hit.distance, 6)}</strong></div></article>)}</div>
        </> : <div className="empty-state"><strong>No query executed</strong><span>Select a collection and search by text or exact vector.</span></div>}
      </section>
    </div>
  );
}
