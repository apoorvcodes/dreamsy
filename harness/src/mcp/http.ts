// Hosted MCP over Streamable HTTP (dreamsy.dev/mcp), stateless: clients like ChatGPT open a new connection for
// every tool call, so nothing lives in a session. A game is named by the unguessable id create_game returns and
// keeps its own anonymous cloud save key; unused games expire. Spec: docs/harness.md §5.4.
import { randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, utimesSync, writeFileSync } from "node:fs";
import { readdir, rm, stat } from "node:fs/promises";
import { createServer as httpServer, type IncomingMessage, type ServerResponse } from "node:http";
import { isIP } from "node:net";
import { basename, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { DEFAULT_CLOUD_URL, type CloudConfig } from "../cloud.js";
import { loadEnv } from "../env.js";
import { repoPath } from "../paths.js";
import { createServer } from "./tools.js";

loadEnv();

const env = process.env;
const PORT = Number(env.DREAMSY_MCP_PORT ?? 8090);
const HOST = env.DREAMSY_MCP_HOST ?? "127.0.0.1";
const DIR = resolve(env.DREAMSY_MCP_DIR ?? repoPath(".data/mcp"));
const GAMES = join(DIR, "games");
const META = join(DIR, "meta");
const CLOUD_URL = env.DREAMSY_CLOUD === "0" ? null : (env.DREAMSY_CLOUD_URL?.trim() || DEFAULT_CLOUD_URL).replace(/\/$/, "");

const MAX_IN_FLIGHT = 8;
const MAX_BODIES_PER_IP = 4;
const MAX_BODY = 8 * 1024 * 1024;
const LARGE_BODY = 512 * 1024;
const BODY_TIMEOUT_MS = 15_000;
const MAX_BATCH = 8;
const GAMES_PER_IP_DAY = positiveInt(env.DREAMSY_MCP_GAMES_PER_IP_DAY, 20);
const GAMES_PER_DAY = positiveInt(env.DREAMSY_MCP_GAMES_PER_DAY, 300);
const MAX_GAMES = positiveInt(env.DREAMSY_MCP_MAX_GAMES, 300);
const GAME_TTL_MS = positiveInt(env.DREAMSY_MCP_GAME_TTL_MS, 24 * 60 * 60 * 1000);
const SWEEP_MS = 10 * 60 * 1000;
const DAY_MS = 24 * 60 * 60 * 1000;

const createdByIp = new Map<string, number[]>();
const bodiesByIp = new Map<string, number>();
let created: number[] = [];
let inFlight = 0;
let largeBodyInFlight = false;
let activeTools = 0;
const leased = new Map<string, { exclusive: boolean; readers: number }>();

mkdirSync(GAMES, { recursive: true });
mkdirSync(META, { recursive: true });
const liveGames = new Set(readdirSync(GAMES));

function positiveInt(value: string | undefined, fallback: number) {
  const n = Number(value);
  return Number.isSafeInteger(n) && n > 0 ? n : fallback;
}

interface Meta { saveKey: string; cloudId?: string }

const metaPath = (dir: string) => join(META, `${basename(dir)}.json`);
function readMeta(dir: string): Meta | null {
  try {
    return JSON.parse(readFileSync(metaPath(dir), "utf8")) as Meta;
  } catch {
    return null;
  }
}
const writeMeta = (dir: string, m: Meta) => writeFileSync(metaPath(dir), JSON.stringify(m), { mode: 0o600 });

function newGameId(ip: string, name: string): string {
  const now = Date.now();
  const recent = (createdByIp.get(ip) ?? []).filter((t) => now - t < DAY_MS);
  if (recent.length >= GAMES_PER_IP_DAY) throw new Error(`${GAMES_PER_IP_DAY} new games a day per network is the limit; keep working on one you made (its id still works)`);
  created = created.filter((t) => now - t < DAY_MS);
  if (created.length >= GAMES_PER_DAY || liveGames.size >= MAX_GAMES) {
    throw new Error("Dreamsy is at its new-game limit; keep working on an existing game or retry later.");
  }
  const slug = name.slice(0, 100).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 24) || "game";
  const id = `${slug}-${BigInt(`0x${randomBytes(6).toString("hex")}`).toString(36).padStart(10, "0")}`;
  mkdirSync(join(GAMES, id));
  liveGames.add(id);
  created.push(now);
  recent.push(now);
  createdByIp.set(ip, recent);
  return id;
}

function touch(dir: string) {
  const now = new Date();
  try { utimesSync(metaPath(dir), now, now); } catch {}
}

function mcpServer(ip: string) {
  return createServer({
    root: GAMES,
    cloud: (dir): CloudConfig | null => {
      const m = readMeta(dir);
      return CLOUD_URL && m ? { url: CLOUD_URL, saveKey: m.saveKey } : null;
    },
    links: {
      get: (dir) => readMeta(dir)?.cloudId ?? null,
      set: (dir, cloudId) => {
        const m = readMeta(dir);
        if (m) writeMeta(dir, { ...m, cloudId });
      },
    },
    remote: {
      async createGame(name, write) {
        const id = newGameId(ip, name);
        const dir = join(GAMES, id);
        leased.set(dir, { exclusive: true, readers: 1 });
        try {
          await write(dir);
          writeMeta(dir, { saveKey: randomBytes(32).toString("base64url") });
          return id;
        } catch (error) {
          await rm(dir, { recursive: true, force: true }).then(() => liveGames.delete(id), () => undefined);
          throw error;
        } finally {
          leased.delete(dir);
        }
      },
      touch,
      async run(dir, exclusive, fn) {
        const current = dir ? leased.get(dir) : undefined;
        if (activeTools >= MAX_IN_FLIGHT || (current && (exclusive || current.exclusive))) {
          throw new Error("Dreamsy MCP is busy; retry in a moment.");
        }
        const lease = current ?? { exclusive, readers: 0 };
        activeTools++;
        if (dir) {
          lease.readers++;
          leased.set(dir, lease);
        }
        try {
          return await fn();
        } finally {
          if (dir && --lease.readers === 0) leased.delete(dir);
          activeTools--;
        }
      },
    },
  });
}

/** Deletes expired games and stale metadata from interrupted creations. */
export async function sweep(now = Date.now()) {
  for (const id of await readdir(GAMES).catch(() => [] as string[])) {
    const dir = join(GAMES, id);
    if (leased.has(dir)) continue;
    leased.set(dir, { exclusive: true, readers: 1 });
    try {
      const st = await stat(metaPath(id)).catch((e: NodeJS.ErrnoException) => {
        if (e.code !== "ENOENT") throw e;
        return null;
      });
      if (st && now - st.mtimeMs <= GAME_TTL_MS) continue;
      await rm(dir, { recursive: true, force: true });
      liveGames.delete(id);
      await rm(metaPath(id), { force: true });
    } catch (e) {
      console.error("dreamsy mcp sweep:", (e as Error).message);
    } finally {
      leased.delete(dir);
    }
  }
  for (const file of await readdir(META).catch(() => [] as string[])) {
    if (!existsSync(join(GAMES, file.replace(/\.json$/, "")))) {
      const st = await stat(join(META, file)).catch(() => null);
      if (st && now - st.mtimeMs > 60_000) await rm(join(META, file), { force: true }).catch(() => undefined);
    }
  }
  for (const [ip, times] of createdByIp) if (!times.some((t) => now - t < DAY_MS)) createdByIp.delete(ip);
}

function clientIp(req: IncomingMessage): string {
  const peer = req.socket.remoteAddress;
  const fly = req.headers["fly-client-ip"];
  if (env.DREAMSY_MCP_TRUST_FLY === "1" && peer === "127.0.0.1" && typeof fly === "string" && isIP(fly)) return fly;
  return peer ?? "?";
}

function send(res: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}) {
  res.writeHead(status, { "content-type": "application/json", ...headers });
  res.end(JSON.stringify(body));
}

const rpcError = (res: ServerResponse, status: number, message: string, headers?: Record<string, string>) =>
  send(res, status, { jsonrpc: "2.0", error: { code: -32000, message }, id: null }, headers);

async function readBody(req: IncomingMessage): Promise<Buffer> {
  if (Number(req.headers["content-length"]) > MAX_BODY) {
    throw Object.assign(new Error("request too large"), { status: 413 });
  }
  let buffer = Buffer.alloc(0);
  let size = 0;
  for await (const chunk of req.iterator({ destroyOnReturn: false }) as AsyncIterable<Buffer>) {
    const end = size + chunk.length;
    if (end > MAX_BODY) throw Object.assign(new Error("request too large"), { status: 413 });
    if (end > buffer.length) {
      const larger = Buffer.allocUnsafe(Math.min(MAX_BODY, Math.max(end, buffer.length * 2)));
      buffer.copy(larger, 0, 0, size);
      buffer = larger;
    }
    chunk.copy(buffer, size);
    size = end;
  }
  return buffer.subarray(0, size);
}

function parseBody(body: Buffer): unknown {
  if (!body.length) return undefined;
  try {
    return JSON.parse(body.toString("utf8"));
  } catch {
    throw Object.assign(new Error("body is not JSON"), { status: 400 });
  }
}

const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, OPTIONS",
  "access-control-allow-headers": "content-type, accept, authorization, mcp-session-id, mcp-protocol-version",
};

/** No bodies or IPs in the log. */
function logRequest(req: IncomingMessage, res: ServerResponse, rpc: string) {
  const start = Date.now();
  res.on("finish", () => {
    console.log(`mcp ${req.method} ${JSON.stringify(rpc.slice(0, 96) || "-")} -> ${res.statusCode} ${Date.now() - start}ms`);
  });
}

const rpcMethods = (body: unknown) =>
  (Array.isArray(body) ? body : [body]).map((m) => (typeof m?.method === "string" ? m.method.slice(0, 64) : "")).filter(Boolean).join(",");

export async function handle(req: IncomingMessage, res: ServerResponse) {
  for (const [k, v] of Object.entries(CORS)) res.setHeader(k, v);
  const path = (req.url ?? "/").split("?", 1)[0]!.replace(/\/$/, "");
  if (req.method !== "POST") logRequest(req, res, "");
  if (path.startsWith("/.well-known/")) return send(res, 404, { error: "not_found", error_description: "Dreamsy MCP needs no authentication" });
  if (req.method === "OPTIONS") { res.writeHead(204); res.end(); return; }
  if (path === "/mcp/health") return send(res, 200, { ok: true });
  if (path !== "/mcp") return send(res, 404, { error: "not found" });
  if (req.method === "GET" && !(req.headers.accept ?? "").includes("text/event-stream")) {
    return send(res, 200, { name: "dreamsy", endpoint: "Streamable HTTP MCP. Add this URL as a remote MCP server / custom connector.", docs: "https://dreamsy.dev/docs#mcp" });
  }
  if (req.method !== "POST") return send(res, 405, { error: "use POST" }, { allow: "POST" });

  if (inFlight >= MAX_IN_FLIGHT) return rpcError(res, 503, "Dreamsy MCP is busy; retry in a moment.", { "retry-after": "5", connection: "close" });
  const ip = clientIp(req);
  const bodies = bodiesByIp.get(ip) ?? 0;
  if (bodies >= MAX_BODIES_PER_IP) return rpcError(res, 429, "Too many uploads from this network; retry in a moment.", { "retry-after": "5", connection: "close" });
  bodiesByIp.set(ip, bodies + 1);
  inFlight++;
  let bodyTimer: ReturnType<typeof setTimeout> | undefined;
  let reading = true;
  let holdsLargeBody = false;
  const releaseBody = () => {
    if (!reading) return;
    reading = false;
    const count = bodiesByIp.get(ip)!;
    if (count === 1) bodiesByIp.delete(ip);
    else bodiesByIp.set(ip, count - 1);
  };
  try {
    const deadline = new Promise<never>((_, reject) => {
      bodyTimer = setTimeout(() => reject(Object.assign(new Error("request body timed out"), { status: 408 })), BODY_TIMEOUT_MS);
    });
    const raw = await Promise.race([readBody(req), deadline]);
    releaseBody();
    clearTimeout(bodyTimer);
    if (raw.length > LARGE_BODY) {
      if (largeBodyInFlight) return rpcError(res, 503, "Dreamsy MCP is busy handling a large request; retry in a moment.", { "retry-after": "5" });
      largeBodyInFlight = holdsLargeBody = true;
    }
    const body = parseBody(raw);
    if (Array.isArray(body) && body.length > MAX_BATCH) return rpcError(res, 400, "Too many requests in one MCP batch.");
    const methods = rpcMethods(body);
    logRequest(req, res, methods);
    // A legacy-style error, so modern clients (ChatGPT) fall back to initialize.
    if (methods === "server/discover") return rpcError(res, 400, "server/discover is not supported; use initialize");
    const server = mcpServer(ip);
    const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined, enableJsonResponse: true });
    const closed = new Promise<void>((resolve) => {
      res.once("close", () => {
        resolve();
        void server.close();
      });
    });
    await server.connect(transport);
    // The transport's JSON response can stay pending after it discards the stream.
    // Tool leases still count work that was already admitted until that work stops.
    await Promise.race([transport.handleRequest(req, res, body), closed]);
  } catch (e) {
    if (res.destroyed) return;
    const status = (e as { status?: number }).status ?? 500;
    if (status === 413 || status === 408) {
      // The client is still uploading: answer, then drop the connection so it can't be reused half-read.
      res.setHeader("connection", "close");
      res.on("finish", () => req.socket?.destroy());
    }
    if (!res.headersSent) rpcError(res, status, status === 500 ? "internal error" : (e as Error).message);
    if (status === 500) console.error("dreamsy mcp:", e);
  } finally {
    clearTimeout(bodyTimer);
    releaseBody();
    if (holdsLargeBody) largeBodyInFlight = false;
    inFlight--;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  setInterval(() => void sweep(), SWEEP_MS).unref();
  void sweep();
  httpServer({ headersTimeout: 10_000, requestTimeout: 20_000, connectionsCheckingInterval: 1000 }, (req, res) => void handle(req, res)).listen(PORT, HOST, () => {
    console.error(`dreamsy mcp (http): http://${HOST}:${PORT}/mcp, games in ${GAMES}`);
  });
}
