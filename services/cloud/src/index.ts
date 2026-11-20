export interface Env {
  GAMES: R2Bucket;
  DB: D1Database;
  ALLOWED_ORIGINS: string;
  MAX_PACK_BYTES: string;
  MAX_STORAGE_GB: string;
  MAX_DAILY_UPLOAD_GB?: string;
  MAX_MONTHLY_WRITES: string;
  MAX_IP_MB_PER_DAY: string;
  MAX_OWNER_MB: string;
  MAX_DAILY_MUTATIONS?: string;
  MAX_IP_MUTATIONS_PER_DAY?: string;
  MAX_DAILY_PLAYS?: string;
  MAX_DAILY_REPORTS?: string;
}

class HttpError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

const MAGIC = [0x44, 0x52, 0x45, 0x41, 0x4d, 0x53, 0x59, 0x1a];
const MAX_THUMB = 600_000;
const MAX_JSON = 16_384;
const MAX_VERSIONS = 1000;
const LIBRARY_WINDOW = 10_000;
const DAY_MS = 86_400_000;
const ALPHABET = "abcdefghijkmnpqrstuvwxyz23456789";

const now = () => Date.now();

function randomId(length: number) {
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  return [...bytes].map((b) => ALPHABET[b % ALPHABET.length]).join("");
}

async function sha256(data: ArrayBuffer | string) {
  const buf = typeof data === "string" ? new TextEncoder().encode(data) : data;
  const digest = await crypto.subtle.digest("SHA-256", buf);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function json(body: unknown, status = 200, headers: Record<string, string> = {}) {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json", ...headers } });
}

const text = (v: unknown, max: number) => (typeof v === "string" ? v.trim().slice(0, max) : "");

async function owner(req: Request) {
  const m = /^Bearer ([A-Za-z0-9_-]{32,128})$/.exec(req.headers.get("authorization") ?? "");
  if (!m) throw new HttpError(401, "Missing or invalid save key");
  return sha256(`owner:${m[1]}`);
}

async function ipHash(req: Request) {
  return sha256(`ip:${req.headers.get("cf-connecting-ip") ?? "local"}`);
}

type RateRule = [bucket: string, max: number, windowMs: number];
const hourly = (bucket: string, max: number): RateRule => [bucket, max, 3_600_000];

async function limit(env: Env, rules: RateRule[]) {
  const t = now();
  const result = await env.DB.prepare(
    `WITH requested(bucket, maximum, deadline) AS (VALUES ${rules.map(() => "(?, ?, ?)").join(", ")}),
      allowed AS MATERIALIZED (
        SELECT 1 WHERE NOT EXISTS (
          SELECT 1 FROM requested LEFT JOIN rate USING (bucket)
          WHERE maximum < 1 OR (rate.reset > ? AND rate.count >= maximum)
        )
      )
     INSERT INTO rate (bucket, count, reset)
     SELECT bucket, 1, deadline FROM requested, allowed WHERE 1
     ON CONFLICT(bucket) DO UPDATE SET
       count = CASE WHEN rate.reset <= ? THEN 1 ELSE rate.count + 1 END,
       reset = CASE WHEN rate.reset <= ? THEN excluded.reset ELSE rate.reset END`,
  ).bind(...rules.flatMap(([bucket, max, window]) => [bucket, max, t + window]), t, t, t).run();
  if (result.meta.changes !== rules.length) throw new HttpError(429, "Too many requests, slow down a little");
}

const configured = (value: string | undefined, fallback: number) => {
  const n = Number(value);
  return value !== undefined && value.trim() && Number.isFinite(n) && n >= 0 ? n : fallback;
};

function dailyRule(env: Env, type: "mutations" | "plays" | "reports"): RateRule {
  const max = type === "mutations" ? configured(env.MAX_DAILY_MUTATIONS, 20_000)
    : type === "plays" ? configured(env.MAX_DAILY_PLAYS, 50_000) : configured(env.MAX_DAILY_REPORTS, 1000);
  return [`global:${type}`, Math.floor(max), DAY_MS];
}

function mutationRules(env: Env, who: string, ip: string): RateRule[] {
  return [
    hourly(`write-ip:${ip}`, 2400),
    [`day-write-ip:${ip}`, Math.floor(configured(env.MAX_IP_MUTATIONS_PER_DAY, 4000)), DAY_MS],
    hourly(`write:${who}`, 600),
    dailyRule(env, "mutations"),
  ];
}

interface Spender {
  owner: string;
  ip: string;
}

async function budget(env: Env, bytes: number, who: Spender) {
  const date = new Date().toISOString();
  const maxBytes = configured(env.MAX_STORAGE_GB, 500) * 1e9;
  const maxDaily = configured(env.MAX_DAILY_UPLOAD_GB, 10) * 1e9;
  const maxWrites = configured(env.MAX_MONTHLY_WRITES, 3_000_000);
  const maxIp = configured(env.MAX_IP_MB_PER_DAY, 300) * 1e6;
  const maxOwner = configured(env.MAX_OWNER_MB, 2000) * 1e6;
  const keys = ["bytes", `writes:${date.slice(0, 7)}`, `ip:${date.slice(0, 10)}:${who.ip}`, `owner:${who.owner}`, `daily-bytes:${date.slice(0, 10)}`];
  // Reserve all counters in one statement before R2. Keep the reservation on failure
  // because a failed response can still mean R2 committed the upload.
  const result = await env.DB.prepare(
    `WITH quota(key, delta, maximum) AS (VALUES
       (?1, ?6, ?7), (?2, 1, ?8), (?3, ?6, ?9), (?4, ?6, ?10), (?5, ?6, ?11)
     ), allowed AS MATERIALIZED (
       SELECT 1 WHERE NOT EXISTS (
         SELECT 1 FROM quota LEFT JOIN usage USING (key)
         WHERE coalesce(value, 0) + delta > maximum
       )
     )
     INSERT INTO usage (key, value)
     SELECT key, delta FROM quota, allowed WHERE 1
     ON CONFLICT(key) DO UPDATE SET value = usage.value + excluded.value`,
  ).bind(...keys, bytes, maxBytes, maxWrites, maxIp, maxOwner, maxDaily).run();
  if (result.meta.changes === keys.length) return;

  const rows = await env.DB.prepare("SELECT key, value FROM usage WHERE key IN (?1, ?2, ?3, ?4, ?5)").bind(...keys).all<{ key: string; value: number }>();
  const get = (k: string) => rows.results.find((r) => r.key === k)?.value ?? 0;
  if (get("bytes") + bytes > maxBytes || get(keys[1]!) + 1 > maxWrites) {
    throw new HttpError(507, "Dreamsy cloud is full for now; your game is still saved on your computer");
  }
  if (get(keys[2]!) + bytes > maxIp) throw new HttpError(429, "Daily upload limit reached for this network; try again tomorrow");
  if (get(keys[3]!) + bytes > maxOwner) throw new HttpError(507, `Your cloud upload quota (${maxOwner / 1e9} GB) is full`);
  if (get(keys[4]!) + bytes > maxDaily) throw new HttpError(429, "Dreamsy's daily upload limit is full; try again tomorrow");
  throw new HttpError(507, "Cloud upload budget reached; retry later");
}

interface GameRow {
  id: string;
  owner: string;
  name: string;
  created: number;
  updated: number;
  latest: number | null;
  remix_of: string | null;
}

async function ownedGame(env: Env, id: string, who: string) {
  const g = await env.DB.prepare("SELECT * FROM games WHERE id = ?1 AND deleted = 0").bind(id).first<GameRow>();
  if (!g || g.owner !== who) throw new HttpError(404, "No such game");
  return g;
}

async function publication(env: Env, slug: string) {
  const p = await env.DB.prepare(
    `SELECT p.*, g.name, g.remix_of, v.hash, v.size, v.thumb FROM publications p
     JOIN games g ON g.id = p.game AND g.deleted = 0
     JOIN versions v ON v.game = p.game AND v.version = p.version
     WHERE p.slug = ?1 AND p.hidden = 0`,
  )
    .bind(slug)
    .first<{ game: string; slug: string; version: number; author: string; link: string; remix: number; published: number; updated: number; plays: number; name: string; remix_of: string | null; hash: string; size: number; thumb: number }>();
  if (!p) throw new HttpError(404, "No such game");
  return p;
}

const publicView = (p: Awaited<ReturnType<typeof publication>>) => ({
  slug: p.slug,
  name: p.name,
  author: p.author || "anonymous dreamer",
  link: p.link || null,
  remix: !!p.remix,
  remixOf: p.remix_of,
  version: p.version,
  published: p.published,
  updated: p.updated,
  plays: p.plays,
  hasThumb: !!p.thumb,
});

async function readBody(req: Request, max: number, message: string): Promise<ArrayBuffer> {
  const len = Number(req.headers.get("content-length") ?? 0);
  if (len > max) throw new HttpError(413, message);
  if (!req.body) return new ArrayBuffer(0);
  const reader = req.body.getReader();
  let buffer = new Uint8Array(Math.min(max, 16_384));
  let size = 0;
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    void reader.cancel().catch(() => undefined);
  }, 15_000);
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      const end = size + value.byteLength;
      if (end > max) {
        void reader.cancel().catch(() => undefined);
        throw new HttpError(413, message);
      }
      if (end > buffer.length) {
        const larger = new Uint8Array(Math.min(max, Math.max(end, buffer.length * 2)));
        larger.set(buffer);
        buffer = larger;
      }
      buffer.set(value, size);
      size = end;
    }
    if (timedOut) throw new HttpError(408, "Request body timed out");
  } finally {
    clearTimeout(timer);
    reader.releaseLock();
  }
  return buffer.buffer.slice(0, size);
}

async function readJson<T extends object>(req: Request): Promise<Partial<T>> {
  const body = await readBody(req, MAX_JSON, "JSON body is too large");
  try {
    const value = JSON.parse(new TextDecoder().decode(body));
    return value && typeof value === "object" ? value : {};
  } catch {
    return {};
  }
}

async function readPack(env: Env, req: Request) {
  const body = await readBody(req, Math.min(configured(env.MAX_PACK_BYTES, 8_000_000), 8_000_000), "Game is too large");
  const head = new Uint8Array(body, 0, Math.min(8, body.byteLength));
  if (head.length < 8 || MAGIC.some((b, i) => head[i] !== b)) throw new HttpError(400, "Not a Dreamsy package (.dpk)");
  return body;
}

async function addVersion(env: Env, game: GameRow, hash: string, size: number, label: string) {
  const t = now();
  const [insert] = await env.DB.batch<{ version: number }>([
    env.DB.prepare(`INSERT INTO versions (game, version, hash, size, label, created)
      SELECT id, coalesce(latest, -1) + 1, ?2, ?3, ?4, ?5 FROM games
      WHERE id = ?1 AND deleted = 0 AND coalesce(latest, -1) < ?6
      RETURNING version`).bind(game.id, hash, size, label, t, MAX_VERSIONS - 1),
    env.DB.prepare("UPDATE games SET latest = coalesce(latest, -1) + 1, updated = ?2 WHERE id = ?1 AND deleted = 0 AND coalesce(latest, -1) < ?3")
      .bind(game.id, t, MAX_VERSIONS - 1),
  ]);
  const version = insert?.results[0]?.version;
  if (version === undefined) throw new HttpError(409, "Cannot add another version to this game");
  return version;
}

function packResponse(body: BodyInit | null, hash: string, cache: string) {
  return new Response(body, { headers: { "content-type": "application/octet-stream", etag: `"${hash}"`, "cache-control": cache } });
}

async function cached(key: URL, ctx: ExecutionContext, load: () => Promise<Response>) {
  const cache = caches.default;
  const hit = await cache.match(key.href).catch(() => undefined);
  if (hit) return hit;
  const response = await load();
  if (response.ok) ctx.waitUntil(cache.put(key.href, response.clone()).catch(() => undefined));
  return response;
}

async function library(url: URL, env: Env, ctx: ExecutionContext) {
  const rawBefore = Number(url.searchParams.get("before"));
  const before = Number.isSafeInteger(rawBefore) && rawBefore > 0 ? rawBefore : Number.MAX_SAFE_INTEGER;
  const q = text(url.searchParams.get("q"), 80).toLowerCase();
  const limit = Math.min(60, Math.max(1, Math.trunc(Number(url.searchParams.get("limit"))) || 30));
  const like = `%${q.replace(/[\\%_]/g, (m) => `\\${m}`)}%`;
  const key = new URL("/v1/public/games", url);
  key.search = new URLSearchParams({ before: String(before), q, limit: String(limit) }).toString();
  return cached(key, ctx, async () => {
    const recent = `WITH recent AS MATERIALIZED (
      SELECT * FROM publications WHERE hidden = 0 ORDER BY updated DESC LIMIT ${LIBRARY_WINDOW}
    )`;
    const where = `(?2 = '' OR lower(g.name) LIKE ?3 ESCAPE '\\' OR lower(p.author) LIKE ?3 ESCAPE '\\')`;
    const [rows, count] = await Promise.all([
      env.DB.prepare(
        `${recent} SELECT p.*, g.name, g.remix_of, v.hash, v.size, v.thumb FROM recent p
         JOIN games g ON g.id = p.game AND g.deleted = 0
         JOIN versions v ON v.game = p.game AND v.version = p.version
         WHERE ${where} AND p.updated < ?1 ORDER BY p.updated DESC LIMIT ?4`,
      ).bind(before, q, like, limit).all<Awaited<ReturnType<typeof publication>>>(),
      env.DB.prepare(`${recent} SELECT count(*) AS n FROM recent p JOIN games g ON g.id = p.game AND g.deleted = 0 WHERE ${where} AND ?1 = ?1`)
        .bind(0, q, like).first<{ n: number }>(),
    ]);
    const games = rows.results.map(publicView);
    return json({ games, total: count?.n ?? games.length, next: games.length === limit ? games.at(-1)!.updated : null }, 200, { "cache-control": "public, max-age=15" });
  });
}

async function route(req: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
  const url = new URL(req.url);
  const parts = url.pathname.split("/").filter(Boolean);
  const method = req.method;
  if (parts[0] !== "v1" || parts.length > 6 || parts.some((part) => part.length > 64)) throw new HttpError(404, "Not found");
  const [, a, b, c, d, e] = parts;

  if (a === "health") return json({ ok: true });

  if (a === "thumbs" && b && method === "GET") {
    const g = await env.DB.prepare("SELECT g.id, g.latest, v.thumb FROM games g JOIN versions v ON v.game = g.id AND v.version = g.latest WHERE g.id = ?1 AND g.deleted = 0")
      .bind(b)
      .first<{ id: string; latest: number; thumb: number }>();
    if (!g?.thumb) throw new HttpError(404, "No thumbnail");
    const obj = await env.GAMES.get(`thumbs/${g.id}/${g.latest}`);
    if (!obj) throw new HttpError(404, "No thumbnail");
    return new Response(obj.body, { headers: { "content-type": obj.httpMetadata?.contentType ?? "image/jpeg", "cache-control": "private, max-age=60" } });
  }

  if (a === "public" && b === "games") {
    if (!c && method === "GET") return library(url, env, ctx);
    if (!c) throw new HttpError(405, "Method not allowed");
    if (e || !((method === "GET" && (!d || d === "package" || d === "thumb")) ||
      (method === "POST" && (d === "play" || d === "report" || d === "remix")))) throw new HttpError(404, "Not found");
    const p = await publication(env, c);
    if (!d && method === "GET") return json({ game: publicView(p) }, 200, { "cache-control": "public, max-age=30" });
    if (d === "package" && method === "GET") {
      const pack = await cached(new URL(`/_cache/packs/${p.hash}`, url), ctx, async () => {
        const obj = await env.GAMES.get(`packs/${p.hash}.dpk`);
        if (!obj) throw new HttpError(404, "Package missing");
        return packResponse(obj.body, p.hash, "public, max-age=86400, immutable");
      });
      return packResponse(pack.body, p.hash, "public, max-age=60");
    }
    if (d === "thumb" && method === "GET") {
      const obj = p.thumb ? await env.GAMES.get(`thumbs/${p.game}/${p.version}`) : null;
      if (!obj) throw new HttpError(404, "No thumbnail");
      return new Response(obj.body, { headers: { "content-type": obj.httpMetadata?.contentType ?? "image/jpeg", "cache-control": "public, max-age=300" } });
    }
    if (d === "play" && method === "POST") {
      const ip = await ipHash(req);
      await limit(env, [hourly(`play-ip:${ip}`, 240), hourly(`play:${ip}:${p.slug}`, 3), dailyRule(env, "plays")]);
      await env.DB.prepare("UPDATE publications SET plays = plays + 1 WHERE slug = ?1").bind(p.slug).run();
      return json({ ok: true });
    }
    if (d === "report" && method === "POST") {
      const ip = await ipHash(req);
      await limit(env, [hourly(`report:${ip}`, 10), dailyRule(env, "reports")]);
      const body = await readJson<{ reason: string }>(req);
      await env.DB.prepare("INSERT INTO reports (slug, reason, ip_hash, created) VALUES (?1, ?2, ?3, ?4)").bind(p.slug, text(body.reason, 500) || "unspecified", ip, now()).run();
      return json({ ok: true });
    }
    if (d === "remix" && method === "POST") {
      if (!p.remix) throw new HttpError(403, "The author turned off remixing for this game");
      const who = await owner(req);
      await limit(env, [...mutationRules(env, who, await ipHash(req)), hourly(`create:${who}`, 60)]);
      const id = randomId(16);
      const t = now();
      const name = `${p.name} (remix)`.slice(0, 80);
      await env.DB.batch([
        env.DB.prepare("INSERT INTO games (id, owner, name, created, updated, latest, remix_of) VALUES (?1, ?2, ?3, ?4, ?4, 0, ?5)").bind(id, who, name, t, p.slug),
        env.DB.prepare("INSERT INTO versions (game, version, hash, size, label, created) VALUES (?1, 0, ?2, ?3, ?4, ?5)").bind(id, p.hash, p.size, `Remix of ${p.name}`, t),
      ]);
      return json({ id, name }, 201);
    }
    throw new HttpError(404, "Not found");
  }

  if (a !== "games") throw new HttpError(404, "Not found");
  const who = await owner(req);
  const ip = method === "GET" ? "" : await ipHash(req);
  const spender: Spender = { owner: who, ip: ip.slice(0, 24) };
  if (method !== "GET") {
    const valid = (!b && method === "POST") || (b && !c && (method === "PATCH" || method === "DELETE")) ||
      (b && c === "versions" && ((!d && method === "POST") || (d && e === "thumb" && method === "PUT"))) ||
      (b && c === "publish" && !d && (method === "POST" || method === "DELETE"));
    if (!valid) throw new HttpError(405, "Method not allowed");
    const rules = mutationRules(env, who, ip);
    if (!b) rules.push(hourly(`create:${who}`, 60));
    if (c === "versions" && !d) rules.push(hourly(`version:${who}`, 240));
    if (c === "publish" && method === "POST") rules.push(hourly(`publish:${who}`, 30));
    await limit(env, rules);
  }

  if (!b) {
    if (method === "GET") {
      const rows = await env.DB.prepare(
        `SELECT g.id, g.name, g.created, g.updated, g.latest, g.remix_of, p.slug, v.thumb FROM games g
         LEFT JOIN publications p ON p.game = g.id
         LEFT JOIN versions v ON v.game = g.id AND v.version = g.latest
         WHERE g.owner = ?1 AND g.deleted = 0 ORDER BY g.updated DESC LIMIT 500`,
      )
        .bind(who)
        .all<{ id: string; name: string; created: number; updated: number; latest: number | null; remix_of: string | null; slug: string | null; thumb: number | null }>();
      return json({
        games: rows.results.map((r) => ({ id: r.id, name: r.name, created: r.created, updated: r.updated, latest: r.latest, remixOf: r.remix_of, published: r.slug, hasThumb: !!r.thumb })),
      });
    }
    if (method === "POST") {
      const body = await readJson<{ name: string }>(req);
      const id = randomId(16);
      const t = now();
      const name = text(body.name, 80) || "Untitled game";
      await env.DB.prepare("INSERT INTO games (id, owner, name, created, updated) VALUES (?1, ?2, ?3, ?4, ?4)").bind(id, who, name, t).run();
      return json({ id, name, created: t }, 201);
    }
    throw new HttpError(405, "Method not allowed");
  }

  const game = await ownedGame(env, b, who);

  if (!c) {
    if (method === "GET") {
      const versions = await env.DB.prepare(`SELECT * FROM (SELECT version, hash, size, label, thumb, created FROM versions
        WHERE game = ?1 ORDER BY version DESC LIMIT ?2) ORDER BY version`).bind(game.id, MAX_VERSIONS).all();
      const pub = await env.DB.prepare("SELECT slug, version, author, link, email, remix, published, updated, plays FROM publications WHERE game = ?1").bind(game.id).first();
      return json({ game: { id: game.id, name: game.name, created: game.created, updated: game.updated, latest: game.latest, remixOf: game.remix_of }, versions: versions.results, publication: pub });
    }
    if (method === "PATCH") {
      const body = await readJson<{ name: string }>(req);
      const name = text(body.name, 80);
      if (!name) throw new HttpError(400, "Name can't be empty");
      await env.DB.prepare("UPDATE games SET name = ?2, updated = ?3 WHERE id = ?1").bind(game.id, name, now()).run();
      return json({ ok: true });
    }
    if (method === "DELETE") {
      await env.DB.batch([
        env.DB.prepare("UPDATE games SET deleted = 1, updated = ?2 WHERE id = ?1").bind(game.id, now()),
        env.DB.prepare("DELETE FROM publications WHERE game = ?1").bind(game.id),
      ]);
      return json({ ok: true });
    }
    throw new HttpError(405, "Method not allowed");
  }

  if (c === "versions") {
    if (!d && method === "POST") {
      if ((game.latest ?? -1) >= MAX_VERSIONS - 1) throw new HttpError(409, "Cannot add another version to this game");
      let label: string;
      try {
        label = text(decodeURIComponent(req.headers.get("x-dreamsy-label") ?? ""), 200);
      } catch {
        throw new HttpError(400, "Invalid version label");
      }
      const body = await readPack(env, req);
      const hash = await sha256(body);
      const key = `packs/${hash}.dpk`;
      if (!(await env.GAMES.head(key))) {
        await budget(env, body.byteLength, spender);
        await env.GAMES.put(key, body, { httpMetadata: { contentType: "application/octet-stream" } });
      }
      const version = await addVersion(env, game, hash, body.byteLength, label);
      return json({ version, hash, size: body.byteLength }, 201);
    }
    const v = Number(d);
    if (!Number.isInteger(v) || v < 0) throw new HttpError(400, "Bad version");
    const row = await env.DB.prepare("SELECT hash FROM versions WHERE game = ?1 AND version = ?2").bind(game.id, v).first<{ hash: string }>();
    if (!row) throw new HttpError(404, "No such version");
    if (e === "package" && method === "GET") {
      const obj = await env.GAMES.get(`packs/${row.hash}.dpk`);
      if (!obj) throw new HttpError(404, "Package missing");
      return packResponse(obj.body, row.hash, "private, max-age=31536000, immutable");
    }
    if (e === "thumb" && method === "PUT") {
      const type = req.headers.get("content-type") ?? "";
      if (!/^image\/(jpeg|png|webp)$/.test(type)) throw new HttpError(400, "Thumbnail must be JPEG, PNG or WebP");
      const body = await readBody(req, MAX_THUMB, "Thumbnail is too large");
      if (!body.byteLength) throw new HttpError(400, "Thumbnail is empty");
      await budget(env, body.byteLength, spender);
      await env.GAMES.put(`thumbs/${game.id}/${v}`, body, { httpMetadata: { contentType: type } });
      await env.DB.prepare("UPDATE versions SET thumb = 1 WHERE game = ?1 AND version = ?2").bind(game.id, v).run();
      return json({ ok: true });
    }
    throw new HttpError(404, "Not found");
  }

  if (c === "publish") {
    if (method === "POST") {
      if (game.latest === null) throw new HttpError(400, "Build the game before publishing it");
      const body = await readJson<{ version: number; author: string; link: string; email: string; remix: boolean }>(req);
      const version = body.version ?? game.latest;
      if (!Number.isSafeInteger(version) || version < 0) throw new HttpError(400, "No such version");
      const exists = await env.DB.prepare("SELECT 1 FROM versions WHERE game = ?1 AND version = ?2").bind(game.id, version).first();
      if (!exists) throw new HttpError(400, "No such version");
      const author = text(body.author, 40);
      const link = text(body.link, 200);
      const email = text(body.email, 200);
      if (link && !/^https?:\/\/[^\s]+\.[^\s]+$/.test(link)) throw new HttpError(400, "Link must start with http:// or https://");
      if (email && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) throw new HttpError(400, "That email doesn't look right");
      const t = now();
      const publication = await env.DB.prepare(
        `INSERT INTO publications (game, slug, version, author, link, email, remix, published, updated)
         SELECT id, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8 FROM games WHERE id = ?1 AND deleted = 0
         ON CONFLICT(game) DO UPDATE SET version = ?3, author = ?4, link = ?5, email = ?6, remix = ?7, updated = ?8
         RETURNING slug`,
      )
        .bind(game.id, randomId(10), version, author, link, email, body.remix === false ? 0 : 1, t)
        .first<{ slug: string }>();
      if (!publication) throw new HttpError(404, "No such game");
      return json({ slug: publication.slug, version });
    }
    if (method === "DELETE") {
      await env.DB.prepare("DELETE FROM publications WHERE game = ?1").bind(game.id).run();
      return json({ ok: true });
    }
  }

  throw new HttpError(404, "Not found");
}

function cors(req: Request, env: Env, res: Response) {
  const origin = req.headers.get("origin");
  const allowed = env.ALLOWED_ORIGINS.split(",").map((s) => s.trim());
  const url = new URL(req.url);
  const h = new Headers(res.headers);
  if (origin && allowed.includes(origin)) {
    h.set("access-control-allow-origin", origin);
    h.set("vary", "origin");
  } else if (url.pathname.startsWith("/v1/public/") && req.method === "GET") {
    h.set("access-control-allow-origin", "*");
  }
  h.set("access-control-allow-methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
  h.set("access-control-allow-headers", "authorization, content-type, x-dreamsy-label");
  h.set("access-control-max-age", "86400");
  return new Response(res.body, { status: res.status, headers: h });
}

export default {
  async fetch(req: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    if (req.method === "OPTIONS") return cors(req, env, new Response(null, { status: 204 }));
    try {
      return cors(req, env, await route(req, env, ctx));
    } catch (e) {
      const status = e instanceof HttpError ? e.status : 500;
      if (status === 500) console.error(e);
      return cors(req, env, json({ error: status === 500 ? "Something went wrong" : (e as Error).message }, status));
    }
  },
  async scheduled(event: ScheduledController, env: Env) {
    const cutoff = new Date(event.scheduledTime - 7 * DAY_MS).toISOString().slice(0, 10);
    await env.DB.batch([
      env.DB.prepare("DELETE FROM rate WHERE bucket IN (SELECT bucket FROM rate WHERE reset <= ?1 ORDER BY reset LIMIT 10000)").bind(event.scheduledTime),
      env.DB.prepare("DELETE FROM usage WHERE key IN (SELECT key FROM usage WHERE key >= 'ip:' AND key < ?1 ORDER BY key LIMIT 10000)").bind(`ip:${cutoff}`),
      env.DB.prepare("DELETE FROM usage WHERE key >= 'daily-bytes:' AND key < ?1").bind(`daily-bytes:${cutoff}`),
    ]);
  },
};
