import { createServer } from "node:http";
import { WebSocketServer, WebSocket } from "ws";
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { readFile, mkdir } from "node:fs/promises";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export async function startSimulator({ port = 8080, persist = false } = {}) {
  const binary = path.join(
    root,
    process.platform === "win32"
      ? "build/host/Release/robot_sim.exe"
      : "build/host/robot_sim",
  );
  if (!existsSync(binary)) throw new Error("Run npm run build:host first.");
  await mkdir(path.join(root, ".cache"), { recursive: true });
  let nextClient = 1,
    child,
    started,
    ready = false,
    closing = false;
  const clients = new Map();
  const now = () => Math.floor(performance.now() - started);
  function write(data) {
    if (ready && child?.stdin.writable)
      child.stdin.write(JSON.stringify({ now_ms: now(), ...data }) + "\n");
  }
  async function boot() {
    ready = false;
    started = performance.now();
    child = spawn(
      binary,
      persist ? [path.join(root, ".cache/sim-config.json")] : [],
      { windowsHide: true, stdio: ["pipe", "pipe", "inherit"] },
    );
    child.stdin.on("error", () => {});
    await new Promise((resolve, reject) => {
      const lines = createInterface({ input: child.stdout });
      child.once("error", reject);
      child.once("exit", (code) => {
        ready = false;
        for (const ws of clients.values()) ws.close(1011, "Simulator stopped");
        if (!closing && code) console.error(`Native simulator exited: ${code}`);
        reject(new Error(`Simulator exited before ready: ${code}`));
      });
      lines.on("line", (line) => {
        const message = JSON.parse(line);
        if (message.ready) {
          ready = true;
          resolve();
        } else {
          const ws = clients.get(message.to);
          if (ws?.readyState === WebSocket.OPEN)
            ws.send(JSON.stringify(message.message));
        }
      });
    });
  }
  await boot();
  const files = {
    "/": ["index.html", "text/html; charset=utf-8"],
    "/app.js": ["app.js", "text/javascript; charset=utf-8"],
    "/style.css": ["style.css", "text/css; charset=utf-8"],
  };
  const server = createServer(async (req, res) => {
    const file = files[req.url?.split("?")[0]];
    if (!file || req.method !== "GET") {
      res.writeHead(404);
      res.end();
      return;
    }
    try {
      const data = await readFile(path.join(root, "web/dist", file[0]));
      res.writeHead(200, {
        "Content-Type": file[1],
        "Cache-Control": "no-store",
        "X-Content-Type-Options": "nosniff",
      });
      res.end(data);
    } catch {
      res.writeHead(503);
      res.end("Run npm run build first.");
    }
  });
  const wss = new WebSocketServer({ noServer: true, maxPayload: 4096 });
  server.on("upgrade", (req, socket, head) => {
    // Local simulator is intentionally bound to loopback. Browser origins must match it.
    let originOK = !req.headers.origin;
    try {
      originOK ||= new URL(req.headers.origin).host === req.headers.host;
    } catch {}
    if (!ready || clients.size >= 4 || req.url !== "/ws" || !originOK) {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws));
  });
  wss.on("connection", (ws) => {
    const client = nextClient++;
    clients.set(client, ws);
    write({ kind: "connect", client });
    ws.on("message", (data, binaryFrame) => {
      if (binaryFrame) {
        ws.close(1003);
        return;
      }
      write({ kind: "request", client, payload: data.toString() });
    });
    ws.on("error", () => {});
    ws.on("close", () => {
      clients.delete(client);
      write({ kind: "disconnect", client });
    });
  });
  const tick = setInterval(() => write({ kind: "tick" }), 10);
  await new Promise((resolve) => server.listen(port, "127.0.0.1", resolve));
  return {
    port: server.address().port,
    async restart() {
      ready = false;
      for (const ws of clients.values()) ws.terminate();
      clients.clear();
      await new Promise((resolve) => {
        child.once("exit", resolve);
        child.stdin.end();
      });
      await boot();
    },
    async close() {
      closing = true;
      clearInterval(tick);
      ready = false;
      for (const ws of clients.values()) ws.terminate();
      clients.clear();
      wss.close();
      child.stdin.end();
      await new Promise((resolve) => server.close(resolve));
    },
  };
}
if (
  process.argv[1] &&
  path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  const sim = await startSimulator({
    port: Number(process.env.PORT || 8080),
    persist: process.env.SIM_PERSIST !== "0",
  });
  console.log(
    `SIMULATION ONLY · http://127.0.0.1:${sim.port} · shared C++ controller; no CAN hardware access`,
  );
  for (const signal of ["SIGINT", "SIGTERM"])
    process.on(signal, async () => {
      await sim.close();
      process.exit(0);
    });
}
