import { access, mkdir } from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const buildDirectory = process.env.VDB_BUILD_DIR ?? path.join(root, "build/macos-arm64-release-make");
const shardBinary = path.join(buildDirectory, "vectordb-shardd");
const coordinatorBinary = path.join(buildDirectory, "vectordb-coordinatord");
const localData = path.join(root, ".local-data");
const children = [];
let stopping = false;

const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

async function requireBinaries() {
  try {
    await Promise.all([access(shardBinary), access(coordinatorBinary)]);
  } catch {
    throw new Error("local C++ binaries are missing; run cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release -j4 first");
  }
}

function start(label, command, arguments_, environment = {}) {
  const child = spawn(command, arguments_, {
    cwd: root,
    env: { ...process.env, ...environment },
    stdio: "inherit",
  });
  children.push(child);
  child.once("exit", (code, signal) => {
    if (!stopping && code !== 0) {
      process.stderr.write(`${label} exited unexpectedly (${signal ?? code})\n`);
      void shutdown(1);
    }
  });
  return child;
}

async function shutdown(exitCode = 0) {
  if (stopping) return;
  stopping = true;
  for (const child of children.toReversed()) {
    if (child.exitCode === null && child.signalCode === null) child.kill("SIGTERM");
  }
  await Promise.all(children.map((child) => child.exitCode === null
    ? new Promise((resolve) => child.once("exit", resolve))
    : Promise.resolve()));
  process.exit(exitCode);
}

async function main() {
  await requireBinaries();
  await Promise.all(["node-a", "node-b", "node-c"].map((name) => mkdir(path.join(localData, name), { recursive: true })));
  start("node-a", shardBinary, [path.join(localData, "node-a"), "127.0.0.1:7001", "documents", "128"]);
  start("node-b", shardBinary, [path.join(localData, "node-b"), "127.0.0.1:7002", "documents", "128"]);
  start("node-c", shardBinary, [path.join(localData, "node-c"), "127.0.0.1:7003", "documents", "128"]);
  await wait(400);
  start("coordinator", coordinatorBinary, [
    "127.0.0.1:7100",
    "3",
    "node-a=127.0.0.1:7001,node-b=127.0.0.1:7002,node-c=127.0.0.1:7003",
  ]);
  await wait(400);
  start("admin-gateway", process.execPath, ["services/admin-gateway/server.mjs"], {
    VDB_COORDINATOR_ADDRESS: "127.0.0.1:7100",
    VDB_ADMIN_PORT: "8090",
  });
  await wait(400);
  start("telemetry-ui", process.platform === "win32" ? "npm.cmd" : "npm", ["run", "dev", "--", "--port", "3000"]);
  process.stdout.write("\nVectorDB local control plane starting:\n  UI      http://localhost:3000\n  API     http://127.0.0.1:8090\n  gRPC    127.0.0.1:7100\n\nPress Ctrl+C to stop the complete stack.\n");
}

process.on("SIGINT", () => void shutdown(0));
process.on("SIGTERM", () => void shutdown(0));

main().catch((error) => {
  process.stderr.write(`${error.message}\n`);
  void shutdown(1);
});
