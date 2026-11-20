// The hosted MCP endpoint (src/mcp/http.ts), driven the way ChatGPT drives it: a new connection for every call.
import { mkdtempSync, readdirSync, rmSync, utimesSync } from "node:fs";
import { createServer, request, type IncomingMessage, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

type ToolResult = { content: { type: string; text?: string }[]; isError?: boolean };
const { simulate } = vi.hoisted(() => ({
  simulate: vi.fn(async () => ({
    errors: [], logs: [], toasts: [], entities: 1, audio: { sfx: {}, music: [] }, avatarTrace: [],
  })),
}));
vi.mock("../src/engine/playtest.js", async (importOriginal) => ({
  ...await importOriginal<typeof import("../src/engine/playtest.js")>(),
  enginePlaytester: async () => ({ simulate }),
}));

const dir = mkdtempSync(join(tmpdir(), "dreamsy-mcp-http-"));

// A stand-in for the Dreamsy cloud worker: records who saved what.
const cloudLog: { method: string; path: string; auth: string }[] = [];
let versions = 0;
const cloud = createServer(async (req: IncomingMessage, res) => {
  for await (const _ of req) void _;
  const path = req.url ?? "";
  cloudLog.push({ method: req.method ?? "", path, auth: req.headers.authorization ?? "" });
  const json = (v: unknown) => { res.writeHead(200, { "content-type": "application/json" }); res.end(JSON.stringify(v)); };
  if (req.method === "POST" && path === "/v1/games") return json({ id: "cloud1" });
  if (req.method === "POST" && /\/versions$/.test(path)) return json({ version: ++versions });
  if (req.method === "POST" && /\/publish$/.test(path)) return json({ slug: "lantern01", version: versions });
  return json({});
});

let http: Server;
let url: URL;
let sweep: (now?: number) => Promise<void>;

/** One tool call on a brand-new connection, like ChatGPT. */
async function call(name: string, args: Record<string, unknown> = {}) {
  const c = new Client({ name: "chatgpt-like", version: "0" });
  await c.connect(new StreamableHTTPClientTransport(url));
  try {
    const r = (await c.callTool({ name, arguments: args })) as ToolResult;
    return { ok: !r.isError, text: r.content.filter((x) => x.type === "text").map((x) => x.text).join("\n") };
  } finally {
    await c.close();
  }
}

const post = (body: unknown, headers: Record<string, string> = {}) =>
  fetch(url, { method: "POST", headers: { "content-type": "application/json", accept: "application/json, text/event-stream", ...headers }, body: typeof body === "string" ? body : JSON.stringify(body) });
const rpcTool = async (name: string, args: Record<string, unknown>, headers?: Record<string, string>) => {
  const r = await post({ jsonrpc: "2.0", id: 7, method: "tools/call", params: { name, arguments: args } }, headers);
  expect(r.status).toBe(200);
  return (await r.json()).result as ToolResult;
};

describe("hosted MCP (stateless Streamable HTTP)", () => {
  let id = "";

  beforeAll(async () => {
    await new Promise<void>((r) => cloud.listen(0, "127.0.0.1", r));
    Object.assign(process.env, {
      DREAMSY_MCP_DIR: dir,
      DREAMSY_CLOUD_URL: `http://127.0.0.1:${(cloud.address() as AddressInfo).port}`,
      DREAMSY_MCP_GAMES_PER_IP_DAY: "4",
      DREAMSY_MCP_GAMES_PER_DAY: "10",
      DREAMSY_MCP_MAX_GAMES: "5",
      DREAMSY_MCP_TRUST_FLY: "0",
      PLAYTEST_BROWSER: "0",
    });
    delete process.env.DREAMSY_CLOUD;
    const mod = await import("../src/mcp/http.js");
    sweep = mod.sweep;
    http = createServer((req, res) => void mod.handle(req, res));
    await new Promise<void>((r) => http.listen(0, "127.0.0.1", r));
    url = new URL(`http://127.0.0.1:${(http.address() as AddressInfo).port}/mcp`);
  });
  afterAll(async () => {
    http.closeAllConnections();
    await new Promise<void>((r) => http.close(() => r()));
    await new Promise<void>((r) => cloud.close(() => r()));
    rmSync(dir, { recursive: true, force: true });
  });

  it("answers plain HTTP: info, health, OAuth discovery, no GET stream", async () => {
    expect((await (await fetch(url)).json()).name).toBe("dreamsy");
    expect((await (await fetch(new URL("/mcp/health", url))).json()).ok).toBe(true);
    const wk = await fetch(new URL("/.well-known/oauth-protected-resource", url));
    expect(wk.status).toBe(404);
    expect(wk.headers.get("content-type")).toContain("json");
    expect((await fetch(url, { headers: { accept: "text/event-stream" } })).status).toBe(405);
  });

  it("lists tools ChatGPT accepts: annotated, no tuple or combinator schemas, plus a resource and icons", async () => {
    const c = new Client({ name: "t", version: "0" });
    await c.connect(new StreamableHTTPClientTransport(url));
    const tools = (await c.listTools()).tools;
    expect(tools.map((t) => t.name)).toEqual(expect.arrayContaining(["create_game", "dreamsy_guide", "apply_ops", "playtest", "check_game", "export_game", "publish_game"]));
    for (const t of tools) {
      expect(typeof t.annotations?.readOnlyHint, t.name).toBe("boolean");
      expect(JSON.stringify(t.inputSchema), t.name).not.toMatch(/"items":\[|"anyOf"|"oneOf"|"allOf"/);
    }
    expect((await c.listResources()).resources.map((r) => r.uri)).toContain("dreamsy://kits");
    expect(c.getServerVersion()?.icons?.length).toBeGreaterThan(0);
    await c.close();
  });

  it("makes server/discover fail the legacy way so clients fall back to initialize", async () => {
    const r = await post({ jsonrpc: "2.0", id: 1, method: "server/discover", params: {} }, { "mcp-protocol-version": "2026-07-28" });
    expect(r.status).toBe(400);
    expect((await r.json()).error.code).toBe(-32000);
  });

  it("keeps a game across separate connections (the ChatGPT flow)", async () => {
    const created = await call("create_game", { game: "lantern", name: "Little Lantern", kit: "platformer", style: "pixel", orientation: "landscape" });
    expect(created.ok, created.text).toBe(true);
    id = /game id is "([^"]+)"/.exec(created.text)?.[1] ?? "";
    expect(id).toMatch(/^lantern-[a-z0-9]{10}$/);

    expect((await call("dreamsy_guide", { game: id })).ok).toBe(true);
    expect((await call("game_outline", { game: id })).text).toContain("Little Lantern");
    const edit = await call("apply_ops", { game: id, reason: "describe", ops: [{ op: "set_manifest", key: "description", value: "A frog lights lanterns." }] });
    expect(edit.ok, edit.text).toBe(true);
    expect((await call("read_files", { game: id, paths: ["dreamsy.toml"] })).text).toContain("A frog lights lanterns.");
    expect((await call("playtest", { game: id, seconds: 2, shots: [], timeline: [{ t: 0.5, tap: [640, 360] }] })).ok).toBe(true);
    expect((await call("check_game", { game: id })).ok).toBe(true);
  }, 120_000);

  it("saves to the cloud under the game's own key, and publishing returns the play link", async () => {
    expect((await call("export_game", { game: id })).ok).toBe(true);
    expect((await call("export_game", { game: id })).ok).toBe(true);
    expect(cloudLog.filter((r) => r.method === "POST" && r.path === "/v1/games")).toHaveLength(1);
    expect(cloudLog.filter((r) => /\/versions$/.test(r.path))).toHaveLength(2);
    expect(new Set(cloudLog.map((r) => r.auth)).size).toBe(1);

    const pub = await call("publish_game", { game: id, author: "Apoorv", remix: true });
    expect(pub.ok, pub.text).toBe(true);
    expect(pub.text).toContain("/play/lantern01");
  }, 120_000);

  it("returns busy across runtime tools and keeps the game until active work finishes", async () => {
    const report = await simulate();
    let release!: () => void;
    let started!: () => void;
    const running = new Promise<void>((resolve) => { started = resolve; });
    simulate.mockImplementationOnce(async () => {
      started();
      await new Promise<void>((resolve) => { release = resolve; });
      return report;
    });
    const pending = call("check_game", { game: id });
    await running;
    try {
      expect((await call("export_game", { game: id })).text).toMatch(/runner is busy; retry/);
      expect((await call("apply_ops", { game: id, reason: "edit", ops: [] })).text).toMatch(/MCP is busy; retry/);
      const expired = new Date(Date.now() - 2 * 24 * 60 * 60 * 1000);
      utimesSync(join(dir, "meta", `${id}.json`), expired, expired);
      await sweep();
      expect(readdirSync(join(dir, "games"))).toContain(id);
    } finally {
      release();
      expect((await pending).ok).toBe(true);
    }
    expect((await call("check_game", { game: id })).ok).toBe(true);
  }, 30_000);

  it("rejects edits that would grow a game past its storage quota", async () => {
    const result = await call("apply_ops", {
      game: id, reason: "large file",
      ops: [{ op: "write_file", path: "data/huge.txt", content: "x".repeat(8_010_000) }],
    });
    expect(result.ok).toBe(false);
    expect(result.text).toMatch(/storage limit/);
    expect((await call("game_outline", { game: id })).ok).toBe(true);
  }, 30_000);

  it("answers a tool call that skips initialize", async () => {
    const r = await post({ jsonrpc: "2.0", id: 7, method: "tools/call", params: { name: "game_outline", arguments: { game: id } } }, { "mcp-protocol-version": "2025-06-18" });
    expect(r.status).toBe(200);
    expect(JSON.stringify(await r.json())).toContain("Little Lantern");
  });

  it("refuses ids that aren't a game", async () => {
    for (const game of ["lantern", "../meta", "lantern/x", id.replace(/.$/, "_")]) expect((await call("game_outline", { game })).ok, game).toBe(false);
  });

  it("limits new games per network when a standalone client supplies forwarding headers", async () => {
    let refused = "";
    for (let i = 0; i < 5 && !refused; i++) {
      const r = await rpcTool("create_game", { game: `g${i}`, name: `G${i}` }, { "x-forwarded-for": `192.0.2.${i + 1}`, "fly-client-ip": `198.51.100.${i + 1}` });
      if (r.isError) refused = r.content[0]?.text ?? "";
    }
    expect(refused).toMatch(/new games a day/);
  }, 60_000);

  it("refuses oversized requests", async () => {
    // The server answers 413 and drops the connection; the client may see either while still uploading.
    const status = await post("x".repeat(9 * 1024 * 1024)).then((r) => r.status, () => "closed");
    expect([413, "closed"]).toContain(status);
  });

  it("recovers after disconnects and refuses new POSTs when all eight slots are occupied", async () => {
    const report = await simulate();
    let finish!: () => void;
    let start!: () => void;
    const running = new Promise<void>((resolve) => { start = resolve; });
    simulate.mockImplementationOnce(async () => {
      start();
      await new Promise<void>((resolve) => { finish = resolve; });
      return report;
    });
    const closed = new Promise<void>((resolve) => {
      http.once("request", (_req, res) => res.once("close", resolve));
    });
    const abandoned = post({ jsonrpc: "2.0", id: 1, method: "tools/call", params: { name: "check_game", arguments: { game: id } } }).catch(() => undefined);
    await running;
    http.closeAllConnections();
    await closed;
    finish();
    await abandoned;

    process.env.DREAMSY_MCP_TRUST_FLY = "1";
    let seen = 0;
    let four!: () => void;
    let eight!: () => void;
    const firstGroup = new Promise<void>((resolve) => { four = resolve; });
    const secondGroup = new Promise<void>((resolve) => { eight = resolve; });
    const listen = (req: IncomingMessage) => {
      if (!req.headers["x-test-partial"]) return;
      if (++seen === 4) four();
      if (seen === 8) eight();
    };
    const partial = (ip: string) => {
      const req = request(url, { method: "POST", headers: {
        "content-length": 2, "content-type": "application/json", "x-test-partial": "1", "fly-client-ip": ip,
      } });
      const done = new Promise<number | undefined>((resolve, reject) => {
        req.on("error", reject);
        req.on("response", (res) => res.resume().on("end", () => resolve(res.statusCode)));
      });
      req.write("{");
      return { req, done };
    };
    http.on("request", listen);
    const requests = Array.from({ length: 4 }, () => partial("192.0.2.1"));
    try {
      await firstGroup;
      expect((await post({}, { "fly-client-ip": "192.0.2.1" })).status).toBe(429);
      requests.push(...Array.from({ length: 4 }, () => partial("192.0.2.2")));
      await secondGroup;
      const busy = await post({}, { "fly-client-ip": "192.0.2.3" });
      expect(busy.status).toBe(503);
      expect(busy.headers.get("retry-after")).toBe("5");
      expect((await fetch(new URL("/mcp/health", url))).status).toBe(200);
    } finally {
      http.off("request", listen);
      process.env.DREAMSY_MCP_TRUST_FLY = "0";
      for (const { req } of requests) req.end("}");
      expect(await Promise.all(requests.map(({ done }) => done))).not.toContain(503);
    }
  }, 30_000);

  it("deletes games nobody used for a day", async () => {
    expect(readdirSync(join(dir, "games")).length).toBeGreaterThan(0);
    await sweep(Date.now() + 2 * 24 * 60 * 60 * 1000);
    expect(readdirSync(join(dir, "games"))).toEqual([]);
    expect((await call("game_outline", { game: id })).ok).toBe(false);
  });

  it("uses trusted Fly IPs and enforces creation and live-game caps across networks", async () => {
    process.env.DREAMSY_MCP_TRUST_FLY = "1";
    try {
      for (let i = 1; i <= 5; i++) {
        expect((await rpcTool("create_game", { game: "test", name: "Test" }, { "fly-client-ip": `192.0.2.${i}` })).isError).toBeUndefined();
      }
      const overflow = await rpcTool("create_game", { game: "test", name: "Test" }, { "fly-client-ip": "192.0.2.10" });
      expect(overflow.content[0]?.text).toMatch(/new-game limit/);
      await sweep(Date.now() + 2 * 24 * 60 * 60 * 1000);
      expect((await rpcTool("create_game", { game: "last", name: "Last" }, { "fly-client-ip": "192.0.2.10" })).isError).toBeUndefined();
      expect((await rpcTool("create_game", { game: "extra", name: "Extra" }, { "fly-client-ip": "192.0.2.11" })).isError).toBe(true);
    } finally {
      process.env.DREAMSY_MCP_TRUST_FLY = "0";
    }
  });
});
