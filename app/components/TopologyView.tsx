import dynamic from "next/dynamic";

import { formatBytes, formatNumber, type Telemetry } from "../admin-types";

const PointCloud = dynamic(
  () => import("../PointCloud").then((module) => module.PointCloud),
  { ssr: false, loading: () => <div className="canvas-loading">initializing WebGL projection…</div> },
);

export function TopologyView({ telemetry }: { telemetry: Telemetry | null }) {
  const vectors = telemetry?.collections.reduce((total, collection) => total + collection.liveVectors, 0) ?? 0;
  const resident = telemetry?.collections.reduce((total, collection) => total + collection.residentBytes, 0) ?? 0;
  return (
    <div className="dashboard-grid">
      <section className="panel projection-panel" aria-labelledby="projection-title">
        <div className="panel-heading">
          <div><div className="eyebrow">Vector space</div><h1 id="projection-title">Operational cluster overview</h1></div>
          <span className="live-label">{formatNumber(vectors, 0)} live vectors</span>
        </div>
        <div className="canvas-wrap"><PointCloud activeCluster={0} pointCount={vectors} /></div>
        <p className="canvas-caption">Representative local projection. Counts, latency, collections, and operations are live; vector coordinates await the telemetry projection worker.</p>
      </section>

      <aside className="dashboard-side">
        <section className="panel">
          <div className="panel-heading"><div><div className="eyebrow">Cluster</div><h2>Local shard members</h2></div></div>
          <div className="node-list">
            {(telemetry?.nodes ?? []).map((node) => (
              <div className="node-card" key={node.id}>
                <span className={`node-dot ${node.state}`} />
                <span><strong>{node.id}</strong><small>{node.endpoint}</small></span>
                <small>{node.state}<br />{formatNumber(node.latencyMillis)} ms · {node.collectionCount} collections</small>
              </div>
            ))}
          </div>
        </section>
        <section className="panel metrics-panel">
          <div className="panel-heading"><div><div className="eyebrow">Measured</div><h2>Search latency</h2></div><small>{telemetry?.latency.samples ?? 0} samples</small></div>
          <div className="metrics-row">
            <Metric label="p50" value={telemetry?.latency.p50Millis ?? 0} />
            <Metric label="p95" value={telemetry?.latency.p95Millis ?? 0} />
            <Metric label="p99" value={telemetry?.latency.p99Millis ?? 0} warn />
          </div>
        </section>
        <section className="panel summary-panel">
          <div><span>Collections</span><strong>{telemetry?.collections.length ?? 0}</strong></div>
          <div><span>Resident vector bytes</span><strong>{formatBytes(resident)}</strong></div>
          <div><span>Gateway uptime</span><strong>{formatNumber(telemetry?.uptimeSeconds ?? 0, 0)}s</strong></div>
        </section>
      </aside>

      <section className="panel operations-panel">
        <div className="panel-heading"><div><div className="eyebrow">Trace log</div><h2>Recent real operations</h2></div></div>
        <div className="table-wrap"><table><thead><tr><th>Time</th><th>Operation</th><th>Collection</th><th>Status</th><th>Latency</th></tr></thead>
          <tbody>{(telemetry?.recentOperations ?? []).slice(0, 12).map((operation, index) => (
            <tr key={`${operation.at}-${index}`}><td>{new Date(operation.at).toLocaleTimeString()}</td><td>{operation.type}</td><td>{operation.collection ?? "—"}</td><td><span className={`badge ${operation.status}`}>{operation.status}</span></td><td>{formatNumber(operation.latencyMillis)} ms</td></tr>
          ))}{(telemetry?.recentOperations.length ?? 0) === 0 ? <tr><td colSpan={5} className="empty-cell">Run a query or upload to populate telemetry.</td></tr> : null}</tbody>
        </table></div>
      </section>
    </div>
  );
}

function Metric({ label, value, warn = false }: { label: string; value: number; warn?: boolean }) {
  return <div className="metric"><span>{label}</span><strong className={warn ? "warn" : ""}>{formatNumber(value)}<small> ms</small></strong></div>;
}
