"use client";

import { useCallback, useEffect, useState } from "react";

import { api, type CollectionInfo, type Telemetry } from "./admin-types";
import { CollectionsView } from "./components/CollectionsView";
import { QueryView } from "./components/QueryView";
import { StressView } from "./components/StressView";
import { TopologyView } from "./components/TopologyView";

type View = "topology" | "queries" | "collections" | "stress";

const labels: Array<{ id: View; label: string }> = [
  { id: "topology", label: "Topology" },
  { id: "queries", label: "Queries" },
  { id: "collections", label: "Collections" },
  { id: "stress", label: "Stress tests" },
];

export function TelemetryConsole() {
  const [view, setView] = useState<View>("topology");
  const [telemetry, setTelemetry] = useState<Telemetry | null>(null);
  const [collections, setCollections] = useState<CollectionInfo[]>([]);
  const [connectionError, setConnectionError] = useState("");

  const refresh = useCallback(async () => {
    try {
      const snapshot = await api<Telemetry>("/api/telemetry");
      setTelemetry(snapshot);
      setCollections(snapshot.collections);
      setConnectionError("");
    } catch (error) {
      setConnectionError(error instanceof Error ? error.message : "gateway unavailable");
    }
  }, []);

  useEffect(() => {
    const initial = window.setTimeout(() => void refresh(), 0);
    const timer = window.setInterval(() => void refresh(), 2000);
    return () => { window.clearTimeout(initial); window.clearInterval(timer); };
  }, [refresh]);

  return (
    <main className="console">
      <header className="topbar">
        <button className="brand" onClick={() => setView("topology")}><span className="brand-mark" aria-hidden="true" /><span>VectorDB <small>/ operations</small></span></button>
        <nav className="topnav" aria-label="Administration views">{labels.map((item) => <button key={item.id} aria-pressed={view === item.id} onClick={() => setView(item.id)}>{item.label}</button>)}</nav>
        <div className={`status-strip ${connectionError ? "offline" : ""}`}><span className="status-dot" />{connectionError ? "gateway offline" : `live · ${telemetry?.coordinator ?? "connecting"}`}</div>
      </header>
      {connectionError ? <div className="connection-banner"><strong>Local data plane unavailable.</strong><span>{connectionError}. Start the full stack with <code>npm run dev:all</code>.</span></div> : null}
      <section className={`view-shell ${view}`} aria-live="polite">
        {view === "topology" ? <TopologyView telemetry={telemetry} /> : null}
        {view === "queries" ? <QueryView collections={collections} onOperation={refresh} /> : null}
        {view === "collections" ? <CollectionsView collections={collections} onChanged={refresh} /> : null}
        {view === "stress" ? <StressView collections={collections} onOperation={refresh} /> : null}
      </section>
      <footer className="footerbar"><span>local data plane · NEON SIMD · HNSW · durable WAL</span><span>{collections.reduce((total, collection) => total + collection.liveVectors, 0).toLocaleString()} live vectors · measured telemetry only</span></footer>
    </main>
  );
}
