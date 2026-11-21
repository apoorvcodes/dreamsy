export interface Env {
  GAMES: R2Bucket;
  DB: D1Database;
  ALLOWED_ORIGINS: string;
  MAX_PACK_BYTES: string;
  MAX_STORAGE_GB: string;
  MAX_MONTHLY_WRITES: string;
  MAX_IP_MB_PER_DAY: string;
  MAX_OWNER_MB: string;
}

class HttpError extends Error {
  constructor(public status: number, message: string) {
    super(message);
  }
}

const MAGIC = [0x44, 0x52, 0x45, 0x41, 0x4d, 0x53, 0x59, 0x1a];
const MAX_THUMB = 600_000;
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

async function limit(env: Env, bucket: string, max: number, windowMs: number) {
  const t = now();
  const row = await env.DB.prepare(
    `INSERT INTO rate (bucket, count, reset) VALUES (?1, 1, ?2)
     ON CONFLICT(bucket) DO UPDATE SET
       count = CASE WHEN rate.reset < ?3 THEN 1 ELSE rate.count + 1 END,
       reset = CASE WHEN rate.reset < ?3 THEN ?2 ELSE rate.reset END
     RETURNING count`,
  )
    .bind(bucket, t + windowMs, t)
    .first<{ count: number }>();
  if ((row?.count ?? 0) > max) throw new HttpError(429, "Too many requests, slow down a little");
}

const month = () => new Date().toISOString().slice(0, 7);

const day = () => new Date().toISOString().slice(0, 10);

interface Spender {
  owner: string;
  ip: string;
}

async function budget(env: Env, bytes: number, who: Spender) {
  const maxBytes = (Number(env.MAX_STORAGE_GB) || 500) * 1e9;
  const maxWrites = Number(env.MAX_MONTHLY_WRITES) || 3_000_000;
  const maxIp = (Number(env.MAX_IP_MB_PER_DAY) || 300) * 1e6;
  const maxOwner = (Number(env.MAX_OWNER_MB) || 2000) * 1e6;
  const keys = ["bytes", `writes:${month()}`, `ip:${day()}:${who.ip}`, `owner:${who.owner}`];
  const rows = await env.DB.prepare("SELECT key, value FROM usage WHERE key IN (?1, ?2, ?3, ?4)").bind(...keys).all<{ key: string; value: number }>();
  const get = (k: string) => rows.results.find((r) => r.key === k)?.value ?? 0;
  if (get("bytes") + bytes > maxBytes || get(keys[1]!) + 1 > maxWrites) {
    throw new HttpError(507, "Dreamsy cloud is full for now; your game is still saved on your computer");
  }
  if (get(keys[2]!) + bytes > maxIp) throw new HttpError(429, "Daily upload limit reached for this network; try again tomorrow");
  if (get(keys[3]!) + bytes > maxOwner) throw new HttpError(507, "Your cloud space is full (2 GB); delete old games to make room");
}

async function spend(env: Env, bytes: number, who: Spender) {
  const up = "INSERT INTO usage (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = value + ?2";
  await env.DB.batch([
    env.DB.prepare(up).bind(`writes:${month()}`, 1),
    ...(bytes
      ? [env.DB.prepare(up).bind("bytes", bytes), env.DB.prepare(up).bind(`ip:${day()}:${who.ip}`, bytes), env.DB.prepare(up).bind(`owner:${who.owner}`, bytes)]
      : []),
  ]);
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
    `SELECT p.*, g.name, g.remix_of, v.hash, v.thumb FROM publications p
     JOIN games g ON g.id = p.game AND g.deleted = 0
     JOIN versions v ON v.game = p.game AND v.version = p.version
     WHERE p.slug = ?1 AND p.hidden = 0`,
  )
    .bind(slug)
    .first<{ game: string; slug: string; version: number; author: string; link: string; remix: number; published: number; updated: number; plays: number; name: string; remix_of: string | null; hash: string; thumb: number }>();
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

async function readPack(env: Env, req: Request) {
  const max = Number(env.MAX_PACK_BYTES) || 8_000_000;
  const len = Number(req.headers.get("content-length") ?? 0);
  if (len > max) throw new HttpError(413, "Game is too large");
  const body = await req.arrayBuffer();
  if (body.byteLength > max) throw new HttpError(413, "Game is too large");
  const head = new Uint8Array(body, 0, Math.min(8, body.byteLength));
  if (head.length < 8 || MAGIC.some((b, i) => head[i] !== b)) throw new HttpError(400, "Not a Dreamsy package (.dpk)");
  return body;
}

async function addVersion(env: Env, game: GameRow, hash: string, size: number, label: string) {
  const version = (game.latest ?? -1) + 1;
  const t = now();
  await env.DB.batch([
    env.DB.prepare("INSERT INTO versions (game, version, hash, size, label, created) VALUES (?1, ?2, ?3, ?4, ?5, ?6)").bind(game.id, version, hash, size, label, t),
    env.DB.prepare("UPDATE games SET latest = ?2, updated = ?3 WHERE id = ?1").bind(game.id, version, t),
  ]);
  return version;
}

function packResponse(obj: R2ObjectBody, hash: string, cache: string) {
  return new Response(obj.body, { headers: { "content-type": "application/octet-stream", etag: `"${hash}"`, "cache-control": cache } });
}

async function route(req: Request, env: Env): Promise<Response> {
  const url = new URL(req.url);
  const parts = url.pathname.split("/").filter(Boolean);
  const method = req.method;
  if (parts[0] !== "v1") throw new HttpError(404, "Not found");
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
    if (!c && method === "GET") {
      const before = Number(url.searchParams.get("before")) || Number.MAX_SAFE_INTEGER;
      const q = text(url.searchParams.get("q"), 80).toLowerCase();
      const like = `%${q.replace(/[\\%_]/g, (m) => `\\${m}`)}%`;
      const limit = Math.min(60, Math.max(1, Number(url.searchParams.get("limit")) || 30));
      const where = `p.hidden = 0 AND (?2 = '' OR lower(g.name) LIKE ?3 ESCAPE '\\' OR lower(p.author) LIKE ?3 ESCAPE '\\')`;
      const [rows, count] = await Promise.all([
        env.DB.prepare(
          `SELECT p.*, g.name, g.remix_of, v.hash, v.thumb FROM publications p
           JOIN games g ON g.id = p.game AND g.deleted = 0
           JOIN versions v ON v.game = p.game AND v.version = p.version
           WHERE ${where} AND p.updated < ?1 ORDER BY p.updated DESC LIMIT ?4`,
        )
          .bind(before, q, like, limit)
          .all<Awaited<ReturnType<typeof publication>>>(),
        env.DB.prepare(`SELECT count(*) AS n FROM publications p JOIN games g ON g.id = p.game AND g.deleted = 0 WHERE ${where} AND ?1 = ?1`)
          .bind(0, q, like)
          .first<{ n: number }>(),
      ]);
      const games = rows.results.map(publicView);
      return json({ games, total: count?.n ?? games.length, next: games.length === limit ? games.at(-1)!.updated : null }, 200, { "cache-control": "public, max-age=15" });
    }
    if (!c) throw new HttpError(405, "Method not allowed");
    const p = await publication(env, c);
    if (!d && method === "GET") return json({ game: publicView(p) }, 200, { "cache-control": "public, max-age=30" });
    if (d === "package" && method === "GET") {
      const obj = await env.GAMES.get(`packs/${p.hash}.dpk`);
      if (!obj) throw new HttpError(404, "Package missing");
      return packResponse(obj, p.hash, "public, max-age=60");
    }
    if (d === "thumb" && method === "GET") {
      const obj = p.thumb ? await env.GAMES.get(`thumbs/${p.game}/${p.version}`) : null;
      if (!obj) throw new HttpError(404, "No thumbnail");
      return new Response(obj.body, { headers: { "content-type": obj.httpMetadata?.contentType ?? "image/jpeg", "cache-control": "public, max-age=300" } });
    }
    if (d === "play" && method === "POST") {
      await limit(env, `play:${await ipHash(req)}:${p.slug}`, 3, 60 * 60_000);
      await env.DB.prepare("UPDATE publications SET plays = plays + 1 WHERE slug = ?1").bind(p.slug).run();
      return json({ ok: true });
    }
    if (d === "report" && method === "POST") {
      const ip = await ipHash(req);
      await limit(env, `report:${ip}`, 10, 60 * 60_000);
      const body = (await req.json().catch(() => ({}))) as { reason?: string };
      await env.DB.prepare("INSERT INTO reports (slug, reason, ip_hash, created) VALUES (?1, ?2, ?3, ?4)").bind(p.slug, text(body.reason, 500) || "unspecified", ip, now()).run();
      return json({ ok: true });
    }
    if (d === "remix" && method === "POST") {
      if (!p.remix) throw new HttpError(403, "The author turned off remixing for this game");
      const who = await owner(req);
      await limit(env, `create:${who}`, 60, 60 * 60_000);
      const id = randomId(16);
      const t = now();
      const name = `${p.name} (remix)`.slice(0, 80);
      await env.DB.prepare("INSERT INTO games (id, owner, name, created, updated, latest, remix_of) VALUES (?1, ?2, ?3, ?4, ?4, NULL, ?5)").bind(id, who, name, t, p.slug).run();
      const game = await ownedGame(env, id, who);
      const size = (await env.GAMES.head(`packs/${p.hash}.dpk`))?.size ?? 0;
      await addVersion(env, game, p.hash, size, `Remix of ${p.name}`);
      return json({ id, name }, 201);
    }
    throw new HttpError(404, "Not found");
  }

  if (a !== "games") throw new HttpError(404, "Not found");
  const who = await owner(req);
  const spender: Spender = { owner: who, ip: (await ipHash(req)).slice(0, 24) };
  if (method !== "GET") {
    await limit(env, `write:${who}`, 600, 60 * 60_000);
    await limit(env, `write-ip:${await ipHash(req)}`, 20_000, 60 * 60_000);
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
      await limit(env, `create:${who}`, 60, 60 * 60_000);
      const body = (await req.json().catch(() => ({}))) as { name?: string };
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
      const versions = await env.DB.prepare("SELECT version, hash, size, label, thumb, created FROM versions WHERE game = ?1 ORDER BY version").bind(game.id).all();
      const pub = await env.DB.prepare("SELECT slug, version, author, link, email, remix, published, updated, plays FROM publications WHERE game = ?1").bind(game.id).first();
      return json({ game: { id: game.id, name: game.name, created: game.created, updated: game.updated, latest: game.latest, remixOf: game.remix_of }, versions: versions.results, publication: pub });
    }
    if (method === "PATCH") {
      const body = (await req.json().catch(() => ({}))) as { name?: string };
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
      await limit(env, `version:${who}`, 240, 60 * 60_000);
      const body = await readPack(env, req);
      const hash = await sha256(body);
      const key = `packs/${hash}.dpk`;
      if (!(await env.GAMES.head(key))) {
        await budget(env, body.byteLength, spender);
        await env.GAMES.put(key, body, { httpMetadata: { contentType: "application/octet-stream" } });
        await spend(env, body.byteLength, spender);
      }
      const label = text(decodeURIComponent(req.headers.get("x-dreamsy-label") ?? ""), 200);
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
      return packResponse(obj, row.hash, "private, max-age=31536000, immutable");
    }
    if (e === "thumb" && method === "PUT") {
      const type = req.headers.get("content-type") ?? "";
      if (!/^image\/(jpeg|png|webp)$/.test(type)) throw new HttpError(400, "Thumbnail must be JPEG, PNG or WebP");
      const body = await req.arrayBuffer();
      if (body.byteLength > MAX_THUMB) throw new HttpError(413, "Thumbnail is too large");
      await budget(env, body.byteLength, spender);
      await env.GAMES.put(`thumbs/${game.id}/${v}`, body, { httpMetadata: { contentType: type } });
      await spend(env, body.byteLength, spender);
      await env.DB.prepare("UPDATE versions SET thumb = 1 WHERE game = ?1 AND version = ?2").bind(game.id, v).run();
      return json({ ok: true });
    }
    throw new HttpError(404, "Not found");
  }

  if (c === "publish") {
    if (method === "POST") {
      await limit(env, `publish:${who}`, 30, 60 * 60_000);
      if (game.latest === null) throw new HttpError(400, "Build the game before publishing it");
      const body = (await req.json().catch(() => ({}))) as { version?: number; author?: string; link?: string; email?: string; remix?: boolean };
      const version = body.version ?? game.latest;
      const exists = await env.DB.prepare("SELECT 1 FROM versions WHERE game = ?1 AND version = ?2").bind(game.id, version).first();
      if (!exists) throw new HttpError(400, "No such version");
      const author = text(body.author, 40);
      const link = text(body.link, 200);
      const email = text(body.email, 200);
      if (link && !/^https?:\/\/[^\s]+\.[^\s]+$/.test(link)) throw new HttpError(400, "Link must start with http:// or https://");
      if (email && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) throw new HttpError(400, "That email doesn't look right");
      const t = now();
      // Published games are final: changing one means remixing it into a new game.
      const prev = await env.DB.prepare("SELECT slug FROM publications WHERE game = ?1").bind(game.id).first<{ slug: string }>();
      if (prev) throw new HttpError(409, "Already published. Published games are final: remix it to publish changes as a new game.");
      const slug = randomId(10);
      await env.DB.prepare(
        `INSERT INTO publications (game, slug, version, author, link, email, remix, published, updated)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8)`,
      )
        .bind(game.id, slug, version, author, link, email, body.remix === false ? 0 : 1, t)
        .run();
      return json({ slug, version });
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
  async fetch(req: Request, env: Env): Promise<Response> {
    if (req.method === "OPTIONS") return cors(req, env, new Response(null, { status: 204 }));
    try {
      return cors(req, env, await route(req, env));
    } catch (e) {
      const status = e instanceof HttpError ? e.status : 500;
      if (status === 500) console.error(e);
      return cors(req, env, json({ error: status === 500 ? "Something went wrong" : (e as Error).message }, status));
    }
  },
};
