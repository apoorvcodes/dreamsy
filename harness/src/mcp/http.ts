// Hosted MCP over Streamable HTTP (dreamsy.dev/mcp), stateless: clients like ChatGPT open a new connection for
// every tool call, so nothing lives in a session. A game is named by the unguessable id create_game returns and
// keeps its own anonymous cloud save key; unused games expire. Spec: docs/harness.md §5.4.
import { randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, utimesSync, writeFileSync } from "node:fs";
import { readdir, rm, stat } from "node:fs/promises";
import { createServer as httpServer, type IncomingMessage, type ServerResponse } from "node:http";
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
const MAX_BODY = 8 * 1024 * 1024;
const GAMES_PER_IP_DAY = Number(env.DREAMSY_MCP_GAMES_PER_IP_DAY ?? 20);
const GAME_TTL_MS = Number(env.DREAMSY_MCP_GAME_TTL_MS ?? 24 * 60 * 60 * 1000);
const SWEEP_MS = 10 * 60 * 1000;
const DAY_MS = 24 * 60 * 60 * 1000;

const createdByIp = new Map<string, number[]>();
let inFlight = 0;

mkdirSync(GAMES, { recursive: true });
mkdirSync(META, { recursive: true });

interface Meta { saveKey: string; cloudId?: string; published?: string }

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
  recent.push(now);
  createdByIp.set(ip, recent);
  const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 24) || "game";
  const id = `${slug}-${BigInt(`0x${randomBytes(6).toString("hex")}`).toString(36).padStart(10, "0")}`;
  writeMeta(id, { saveKey: randomBytes(32).toString("base64url") });
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
      newGameId: (name) => newGameId(ip, name),
      touch,
      published: (dir) => readMeta(dir)?.published ?? null,
      markPublished: (dir, slug) => {
        const m = readMeta(dir);
        if (m) writeMeta(dir, { ...m, published: slug });
      },
    },
  });
}

/** Deletes games unused for GAME_TTL_MS, and anything that isn't a game (older layouts, half-made games). */
export async function sweep(now = Date.now()) {
  for (const name of await readdir(DIR).catch(() => [] as string[])) {
    if (name !== "games" && name !== "meta") await rm(join(DIR, name), { recursive: true, force: true }).catch(() => undefined);
  }
  for (const id of await readdir(GAMES).catch(() => [] as string[])) {
    const st = await stat(metaPath(id)).catch(() => null);
    if (st && now - st.mtimeMs <= GAME_TTL_MS) continue;
    // Published games live on in the library; the working copy here can go.
    await rm(join(GAMES, id), { recursive: true, force: true }).catch(() => undefined);
    await rm(metaPath(id), { force: true }).catch(() => undefined);
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
  const h = (n: string) => (req.headers[n] as string | undefined)?.split(",")[0]?.trim();
  return h("fly-client-ip") || h("x-forwarded-for") || req.socket.remoteAddress || "?";
}

function send(res: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}) {
  res.writeHead(status, { "content-type": "application/json", ...headers });
  res.end(JSON.stringify(body));
}

const rpcError = (res: ServerResponse, status: number, message: string, headers?: Record<string, string>) =>
  send(res, status, { jsonrpc: "2.0", error: { code: -32000, message }, id: null }, headers);

async function readBody(req: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const c of req) {
    size += (c as Buffer).length;
    if (size > MAX_BODY) throw Object.assign(new Error("request too large"), { status: 413 });
    chunks.push(c as Buffer);
  }
  const text = Buffer.concat(chunks).toString("utf8");
  if (!text) return undefined;
  try {
    return JSON.parse(text);
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
    const ua = (req.headers["user-agent"] ?? "-").slice(0, 60);
    console.log(`mcp ${req.method} ${req.url} ${rpc || "-"} -> ${res.statusCode} ${Date.now() - start}ms ua="${ua}"`);
  });
}

const rpcMethods = (body: unknown) =>
  (Array.isArray(body) ? body : [body]).map((m) => (m && typeof m === "object" && "method" in m ? String((m as { method: unknown }).method) : "")).filter(Boolean).join(",");

export async function handle(req: IncomingMessage, res: ServerResponse) {
  for (const [k, v] of Object.entries(CORS)) res.setHeader(k, v);
  const path = new URL(req.url ?? "/", "http://x").pathname.replace(/\/$/, "");
  if (req.method !== "POST") logRequest(req, res, "");
  if (path.startsWith("/.well-known/")) return send(res, 404, { error: "not_found", error_description: "Dreamsy MCP needs no authentication" });
  if (req.method === "OPTIONS") { res.writeHead(204); res.end(); return; }
  if (path === "/mcp/health") return send(res, 200, { ok: true });
  if (path !== "/mcp") return send(res, 404, { error: "not found" });
  if (req.method === "GET" && !(req.headers.accept ?? "").includes("text/event-stream")) {
    return send(res, 200, { name: "dreamsy", endpoint: "Streamable HTTP MCP. Add this URL as a remote MCP server / custom connector.", docs: "https://dreamsy.dev/docs#mcp" });
  }
  if (req.method !== "POST") return send(res, 405, { error: "use POST" }, { allow: "POST" });

  if (inFlight >= MAX_IN_FLIGHT) return rpcError(res, 503, "Dreamsy MCP is busy; retry in a moment.", { "retry-after": "5" });
  inFlight++;
  try {
    const body = await readBody(req);
    logRequest(req, res, rpcMethods(body));
    // A legacy-style error, so modern clients (ChatGPT) fall back to initialize.
    if (rpcMethods(body) === "server/discover") return rpcError(res, 400, "server/discover is not supported; use initialize");
    const server = mcpServer(clientIp(req));
    const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined, enableJsonResponse: true });
    res.on("close", () => {
      void transport.close();
      void server.close();
    });
    await server.connect(transport);
    await transport.handleRequest(req, res, body);
  } catch (e) {
    const status = (e as { status?: number }).status ?? 500;
    if (status === 413) {
      // The client is still uploading: answer, then drop the connection so it can't be reused half-read.
      res.setHeader("connection", "close");
      res.on("finish", () => req.socket?.destroy());
    }
    if (!res.headersSent) rpcError(res, status, status === 500 ? "internal error" : (e as Error).message);
    if (status === 500) console.error("dreamsy mcp:", e);
  } finally {
    inFlight--;
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  setInterval(() => void sweep(), SWEEP_MS).unref();
  void sweep();
  httpServer((req, res) => void handle(req, res)).listen(PORT, HOST, () => {
    console.error(`dreamsy mcp (http): http://${HOST}:${PORT}/mcp, games in ${GAMES}`);
  });
}
