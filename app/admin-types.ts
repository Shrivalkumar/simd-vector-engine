export type CollectionInfo = {
  name: string;
  dimensions: number;
  metric: "cosine" | "l2" | "dot";
  liveVectors: number;
  residentBytes: number;
};

export type Operation = {
  at: string;
  type: string;
  collection?: string;
  latencyMillis: number;
  status: "ok" | "error";
  count?: number;
  hitCount?: number;
  queryCount?: number;
  p99Millis?: number;
  message?: string;
};

export type StressResult = {
  collection: string;
  isolated: boolean;
  vectorCount: number;
  queryCount: number;
  concurrency: number;
  topK: number;
  efSearch: number;
  dimensions: number;
  ingestMillis: number;
  ingestVectorsPerSecond: number;
  queryMillis: number;
  groundTruthMillis: number;
  recallQueries: number;
  queriesPerSecond: number;
  errors: number;
  recallAtK: number;
  selfHitRate: number;
  p50Millis: number;
  p95Millis: number;
  p99Millis: number;
  maxMillis: number;
};

export type StressJob = {
  id: string;
  stage: "queued" | "ingesting" | "querying" | "completed" | "failed";
  progress: number;
  startedAt: string;
  finishedAt?: string;
  result: StressResult | null;
  error: string | null;
};

export type Telemetry = {
  generatedAt: string;
  uptimeSeconds: number;
  coordinator: string;
  nodes: Array<{
    id: string;
    endpoint: string;
    state: string;
    latencyMillis: number;
    collectionCount: number;
    message?: string;
  }>;
  collections: CollectionInfo[];
  latency: { p50Millis: number; p95Millis: number; p99Millis: number; samples: number };
  recentOperations: Operation[];
  stressJobs: StressJob[];
};

export type SearchResult = {
  latencyMillis: number;
  shardEpoch: string;
  embedding: string;
  hits: Array<{ id: string; generation: string; distance: number; payload: Record<string, unknown> }>;
};

const API_ROOT = "http://127.0.0.1:8090";

export async function api<T>(path: string, options?: RequestInit): Promise<T> {
  const response = await fetch(`${API_ROOT}${path}`, {
    ...options,
    headers: { "Content-Type": "application/json", ...options?.headers },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.error ?? `request failed with status ${response.status}`);
  return body as T;
}

export function formatNumber(value: number, maximumFractionDigits = 1) {
  return new Intl.NumberFormat("en-US", { maximumFractionDigits }).format(value);
}

export function formatBytes(bytes: number) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 ** 2) return `${formatNumber(bytes / 1024)} KiB`;
  if (bytes < 1024 ** 3) return `${formatNumber(bytes / 1024 ** 2)} MiB`;
  return `${formatNumber(bytes / 1024 ** 3)} GiB`;
}
