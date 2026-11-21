import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { after, before, test } from "node:test";
import { fileURLToPath } from "node:url";
import { Miniflare } from "miniflare";

const here = new URL(".", import.meta.url);
const pack = readFileSync(new URL("fixture.dpk", here));
const ALICE = "a".repeat(43);
const BOB = "b".repeat(43);
let mf;

const call = async (path, { key, method = "GET", body, headers = {} } = {}) => {
  const res = await mf.dispatchFetch(`http://cloud.test${path}`, {
    method,
    body,
    headers: { ...(key ? { authorization: `Bearer ${key}` } : {}), ...headers },
  });
  const type = res.headers.get("content-type") ?? "";
  return { status: res.status, headers: res.headers, body: type.includes("json") ? await res.json() : new Uint8Array(await res.arrayBuffer()) };
};
const jsonBody = (o) => ({ body: JSON.stringify(o), headers: { "content-type": "application/json" } });

before(async () => {
  mf = new Miniflare({
    modules: true,
    scriptPath: fileURLToPath(new URL("../.build/index.js", here)),
    compatibilityDate: "2025-10-01",
    r2Buckets: ["GAMES"],
    d1Databases: ["DB"],
    bindings: { ALLOWED_ORIGINS: "http://127.0.0.1:3100", MAX_PACK_BYTES: "8000000", MAX_STORAGE_GB: "500", MAX_MONTHLY_WRITES: "3000000", MAX_IP_MB_PER_DAY: "300", MAX_OWNER_MB: "2000" },
  });
  const db = await mf.getD1Database("DB");
  for (const f of ["0001_init.sql", "0002_budget.sql"]) {
    const sql = readFileSync(new URL(`../migrations/${f}`, here), "utf8");
    for (const stmt of sql.split(";").map((s) => s.trim()).filter(Boolean)) await db.prepare(stmt).run();
  }
});

after(() => mf?.dispose());

test("health and auth", async () => {
  assert.equal((await call("/v1/health")).status, 200);
  assert.equal((await call("/v1/games")).status, 401);
  assert.equal((await call("/v1/games", { key: "short" })).status, 401);
});

test("save, version, publish, remix, report, unpublish", async () => {
  const created = await call("/v1/games", { key: ALICE, method: "POST", ...jsonBody({ name: "Cloud Hopper" }) });
  assert.equal(created.status, 201);
  const id = created.body.id;

  assert.equal((await call(`/v1/games/${id}/versions`, { key: ALICE, method: "POST", body: new Uint8Array([1, 2, 3]) })).status, 400);
  const v0 = await call(`/v1/games/${id}/versions`, { key: ALICE, method: "POST", body: pack, headers: { "x-dreamsy-label": encodeURIComponent("first build") } });
  assert.equal(v0.status, 201);
  assert.equal(v0.body.version, 0);
  const v1 = await call(`/v1/games/${id}/versions`, { key: ALICE, method: "POST", body: pack });
  assert.equal(v1.body.version, 1);
  assert.equal(v1.body.hash, v0.body.hash);

  const got = await call(`/v1/games/${id}/versions/1/package`, { key: ALICE });
  assert.equal(got.status, 200);
  assert.deepEqual(Buffer.from(got.body), pack);

  assert.equal((await call(`/v1/games/${id}`, { key: BOB })).status, 404);
  assert.equal((await call(`/v1/games/${id}/versions/0/package`, { key: BOB })).status, 404);

  const thumb = new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 1, 2, 3]);
  assert.equal((await call(`/v1/games/${id}/versions/1/thumb`, { key: ALICE, method: "PUT", body: thumb, headers: { "content-type": "image/jpeg" } })).status, 200);
  assert.deepEqual((await call(`/v1/thumbs/${id}`)).body, thumb);

  const list = await call("/v1/games", { key: ALICE });
  assert.equal(list.body.games.length, 1);
  assert.equal(list.body.games[0].latest, 1);
  assert.equal((await call("/v1/games", { key: BOB })).body.games.length, 0);

  assert.equal((await call(`/v1/games/${id}/publish`, { key: ALICE, method: "POST", ...jsonBody({ email: "nope" }) })).status, 400);
  const pub = await call(`/v1/games/${id}/publish`, { key: ALICE, method: "POST", ...jsonBody({ author: "Apoorv", link: "https://example.com", email: "a@example.com" }) });
  assert.equal(pub.status, 200);
  const slug = pub.body.slug;

  const pubGame = await call(`/v1/public/games/${slug}`);
  assert.equal(pubGame.body.game.author, "Apoorv");
  assert.equal(pubGame.body.game.link, "https://example.com");
  assert.equal(JSON.stringify(pubGame.body).includes("a@example.com"), false);
  assert.equal(pubGame.headers.get("access-control-allow-origin"), "*");
  assert.deepEqual(Buffer.from((await call(`/v1/public/games/${slug}/package`)).body), pack);
  const lib = await call("/v1/public/games");
  assert.equal(lib.body.games.length, 1);
  assert.equal(lib.body.total, 1);
  assert.equal((await call("/v1/public/games?q=cloud")).body.total, 1);
  assert.equal((await call("/v1/public/games?q=apoorv")).body.total, 1);
  assert.equal((await call("/v1/public/games?q=zzz")).body.total, 0);
  assert.equal((await call("/v1/public/games?q=%25")).body.total, 0);

  const owned = await call(`/v1/games/${id}`, { key: ALICE });
  assert.equal(owned.body.publication.email, "a@example.com");

  const remix = await call(`/v1/public/games/${slug}/remix`, { key: BOB, method: "POST" });
  assert.equal(remix.status, 201);
  const bobGame = await call(`/v1/games/${remix.body.id}`, { key: BOB });
  assert.equal(bobGame.body.game.remixOf, slug);
  assert.equal(bobGame.body.versions[0].hash, v0.body.hash);

  assert.equal((await call(`/v1/public/games/${slug}/play`, { method: "POST" })).status, 200);
  assert.equal((await call(`/v1/public/games/${slug}/report`, { method: "POST", ...jsonBody({ reason: "test" }) })).status, 200);

  const again = await call(`/v1/games/${id}/publish`, { key: ALICE, method: "POST", ...jsonBody({ remix: false }) });
  assert.equal(again.body.slug, slug);
  assert.equal((await call(`/v1/public/games/${slug}/remix`, { key: BOB, method: "POST" })).status, 403);

  assert.equal((await call(`/v1/games/${id}/publish`, { key: ALICE, method: "DELETE" })).status, 200);
  assert.equal((await call(`/v1/public/games/${slug}`)).status, 404);

  assert.equal((await call(`/v1/games/${id}`, { key: ALICE, method: "DELETE" })).status, 200);
  assert.equal((await call(`/v1/games/${id}`, { key: ALICE })).status, 404);
});

test("cors for the editor origin", async () => {
  const res = await mf.dispatchFetch("http://cloud.test/v1/games", { method: "OPTIONS", headers: { origin: "http://127.0.0.1:3100" } });
  assert.equal(res.status, 204);
  assert.equal(res.headers.get("access-control-allow-origin"), "http://127.0.0.1:3100");
  const other = await mf.dispatchFetch("http://cloud.test/v1/games", { method: "OPTIONS", headers: { origin: "https://evil.example" } });
  assert.equal(other.headers.get("access-control-allow-origin"), null);
});

test("budget guard refuses uploads past the storage cap", async () => {
  const db = await mf.getD1Database("DB");
  const created = await call("/v1/games", { key: ALICE, method: "POST", ...jsonBody({ name: "Budget" }) });
  await db.prepare("INSERT INTO usage (key, value) VALUES ('bytes', ?1) ON CONFLICT(key) DO UPDATE SET value = ?1").bind(500e9).run();
  const tweaked = Buffer.concat([pack, Buffer.from("x")]);
  const res = await call(`/v1/games/${created.body.id}/versions`, { key: ALICE, method: "POST", body: tweaked });
  assert.equal(res.status, 507);
  const dup = await call(`/v1/games/${created.body.id}/versions`, { key: ALICE, method: "POST", body: pack });
  assert.equal(dup.status, 201);
  await db.prepare("UPDATE usage SET value = 0 WHERE key = 'bytes'").run();
});

test("per-owner storage limit", async () => {
  const db = await mf.getD1Database("DB");
  const CAROL = "c".repeat(43);
  const created = await call("/v1/games", { key: CAROL, method: "POST", ...jsonBody({ name: "Full" }) });
  const ownerKey = (await db.prepare("SELECT owner FROM games WHERE id = ?1").bind(created.body.id).first()).owner;
  await db.prepare("INSERT INTO usage (key, value) VALUES (?1, ?2)").bind(`owner:${ownerKey}`, 2000e6).run();
  const res = await call(`/v1/games/${created.body.id}/versions`, { key: CAROL, method: "POST", body: Buffer.concat([pack, Buffer.from("yy")]) });
  assert.equal(res.status, 507);
  assert.match(res.body.error, /2 GB/);
});
