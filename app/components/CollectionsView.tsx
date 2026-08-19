"use client";

import { useState } from "react";

import { api, formatBytes, formatNumber, type CollectionInfo } from "../admin-types";

type UploadRecord = { id?: string | number; generation?: number; text?: string; vector?: number[]; payload?: Record<string, unknown> };

function parseCsv(source: string): UploadRecord[] {
  const rows: string[][] = [];
  let row: string[] = [];
  let field = "";
  let quoted = false;
  for (let index = 0; index < source.length; index += 1) {
    const character = source[index];
    if (character === '"' && quoted && source[index + 1] === '"') { field += '"'; index += 1; }
    else if (character === '"') quoted = !quoted;
    else if (character === "," && !quoted) { row.push(field); field = ""; }
    else if ((character === "\n" || character === "\r") && !quoted) {
      if (character === "\r" && source[index + 1] === "\n") index += 1;
      row.push(field); field = "";
      if (row.some((value) => value.length > 0)) rows.push(row);
      row = [];
    } else field += character;
  }
  row.push(field);
  if (row.some((value) => value.length > 0)) rows.push(row);
  if (rows.length < 2) throw new Error("CSV requires a header and at least one data row");
  const headers = rows[0].map((value) => value.trim());
  return rows.slice(1).map((values, rowIndex) => {
    const record: UploadRecord = { payload: {} };
    headers.forEach((header, index) => {
      const value = values[index]?.trim() ?? "";
      if (header === "id") record.id = value;
      else if (header === "generation") record.generation = Number(value || 1);
      else if (header === "text") record.text = value;
      else if (header === "vector") record.vector = value.startsWith("[") ? JSON.parse(value) : value.split(/[;\s]+/).filter(Boolean).map(Number);
      else if (header) record.payload![header] = value;
    });
    if (!record.text && !record.vector) throw new Error(`CSV row ${rowIndex + 2} needs text or vector`);
    return record;
  });
}

function parseRecords(source: string, filename = "data.json"): UploadRecord[] {
  if (filename.toLowerCase().endsWith(".csv")) return parseCsv(source);
  const value = JSON.parse(source);
  const records = Array.isArray(value) ? value : value.records;
  if (!Array.isArray(records)) throw new Error("JSON must be an array or an object containing records");
  return records;
}

export function CollectionsView({ collections, onChanged }: { collections: CollectionInfo[]; onChanged: () => Promise<void> }) {
  const [name, setName] = useState("");
  const [dimensions, setDimensions] = useState(128);
  const [metric, setMetric] = useState("cosine");
  const [selected, setSelected] = useState(collections[0]?.name ?? "documents");
  const [source, setSource] = useState('[\n  {"id": 1, "text": "Vector databases index embeddings for similarity search", "payload": {"category": "docs"}},\n  {"id": 2, "text": "HNSW is a graph index for approximate nearest neighbors", "payload": {"category": "research"}}\n]');
  const [filename, setFilename] = useState("data.json");
  const [message, setMessage] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const activeCollection = collections.some((item) => item.name === selected) ? selected : collections[0]?.name ?? "";

  async function create(event: React.FormEvent) {
    event.preventDefault(); setBusy(true); setError(""); setMessage("");
    try {
      await api("/api/collections", { method: "POST", body: JSON.stringify({ name, dimensions, metric }) });
      setMessage(`Created ${name} on every shard.`); setSelected(name); setName(""); await onChanged();
    } catch (caught) { setError(caught instanceof Error ? caught.message : "create failed"); }
    finally { setBusy(false); }
  }

  async function upload(event: React.FormEvent) {
    event.preventDefault(); setBusy(true); setError(""); setMessage("");
    try {
      const records = parseRecords(source, filename);
      const result = await api<{ applied: number; latencyMillis: number }>(`/api/collections/${encodeURIComponent(activeCollection)}/upsert`, { method: "POST", body: JSON.stringify({ records }) });
      setMessage(`Applied ${formatNumber(result.applied, 0)} records in ${formatNumber(result.latencyMillis)} ms.`); await onChanged();
    } catch (caught) { setError(caught instanceof Error ? caught.message : "upload failed"); }
    finally { setBusy(false); }
  }

  async function readFile(file: File | undefined) {
    if (!file) return;
    setFilename(file.name);
    setSource(await file.text());
  }

  async function remove(collection: CollectionInfo) {
    if (!window.confirm(`Delete ${collection.name} and its local WAL data from every shard? This cannot be undone.`)) return;
    setBusy(true); setError("");
    try { await api(`/api/collections/${encodeURIComponent(collection.name)}`, { method: "DELETE" }); setMessage(`Deleted ${collection.name}.`); await onChanged(); }
    catch (caught) { setError(caught instanceof Error ? caught.message : "delete failed"); }
    finally { setBusy(false); }
  }

  return <div className="collections-layout">
    <section className="panel collection-catalog">
      <div className="panel-heading"><div><div className="eyebrow">Catalog</div><h1>Collections</h1></div><span className="live-label">{collections.length} total</span></div>
      <div className="collection-cards">{collections.map((collection) => <article key={collection.name} className={activeCollection === collection.name ? "collection-card selected" : "collection-card"}><button type="button" className="collection-summary" onClick={() => setSelected(collection.name)}><span><strong>{collection.name}</strong><small>{collection.metric} · {collection.dimensions} dimensions</small></span><dl><span><dt>Vectors</dt><dd>{formatNumber(collection.liveVectors, 0)}</dd></span><span><dt>FP32</dt><dd>{formatBytes(collection.residentBytes)}</dd></span></dl></button><button type="button" className="danger-link" onClick={() => void remove(collection)} disabled={busy}>Delete</button></article>)}</div>
    </section>
    <div className="collections-actions">
      <section className="panel form-panel"><div className="panel-heading"><div><div className="eyebrow">Schema</div><h2>Create collection</h2></div></div><form className="form-stack compact" onSubmit={create}><label>Name<input required pattern="[A-Za-z0-9_-]+" value={name} onChange={(event) => setName(event.target.value)} placeholder="products" /></label><div className="form-row"><label>Dimensions<input type="number" min={1} max={65536} value={dimensions} onChange={(event) => setDimensions(Number(event.target.value))} /></label><label>Metric<select value={metric} onChange={(event) => setMetric(event.target.value)}><option value="cosine">Cosine</option><option value="l2">L2 squared</option><option value="dot">Dot product</option></select></label></div><button className="secondary-button" disabled={busy}>Create on cluster</button></form></section>
      <section className="panel form-panel upload-panel"><div className="panel-heading"><div><div className="eyebrow">Ingestion</div><h2>Upload JSON, CSV, vectors, or text</h2></div><select value={activeCollection} onChange={(event) => setSelected(event.target.value)}>{collections.map((item) => <option key={item.name}>{item.name}</option>)}</select></div><form className="form-stack compact" onSubmit={upload}><label className="file-input">Choose .json or .csv<input type="file" accept=".json,.csv,application/json,text/csv" onChange={(event) => void readFile(event.target.files?.[0])} /></label><label>Records<textarea rows={11} value={source} onChange={(event) => { setSource(event.target.value); setFilename("data.json"); }} /></label><p className="form-note">JSON accepts records with id, generation, text, vector, and payload. CSV headers may include id, generation, text, vector, plus payload columns.</p><button className="primary-button" disabled={busy || !activeCollection}>{busy ? "Applying…" : "Validate and upsert"}</button></form></section>
    </div>
    {(message || error) ? <div className={`toast ${error ? "error" : "success"}`}>{error || message}</div> : null}
  </div>;
}
