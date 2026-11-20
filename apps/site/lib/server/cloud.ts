import { randomBytes } from "node:crypto";
import { existsSync } from "node:fs";
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { createGame, setMeta, snapshot } from "./games";
import { dataDir, gameDir, HttpError } from "./paths";
import { tool } from "./tool";

export const DEFAULT_CLOUD_URL = "https://dreamsy-cloud.tofu-develops.workers.dev";

const home = () => process.env.DREAMSY_HOME ?? join(homedir(), ".dreamsy");

export const cloudUrl = () => (process.env.DREAMSY_CLOUD === "0" ? null : (process.env.DREAMSY_CLOUD_URL?.trim() || DEFAULT_CLOUD_URL).replace(/\/$/, ""));

async function readJson<T>(path: string, fallback: T): Promise<T> {
  try {
    return JSON.parse(await readFile(path, "utf8")) as T;
  } catch {
    return fallback;
  }
}

async function writePrivate(path: string, value: unknown) {
  await mkdir(home(), { recursive: true, mode: 0o700 });
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, { mode: 0o600 });
  await chmod(path, 0o600);
}

/** This computer's save key, shared with the MCP server and the harness CLI (~/.dreamsy/config.json). */
export async function saveKey(): Promise<string> {
  const fromEnv = process.env.DREAMSY_SAVE_KEY?.trim();
  if (fromEnv && /^[A-Za-z0-9_-]{32,128}$/.test(fromEnv)) return fromEnv;
  const path = join(home(), "config.json");
  const cfg = await readJson<{ saveKey?: string }>(path, {});
  if (cfg.saveKey && /^[A-Za-z0-9_-]{32,128}$/.test(cfg.saveKey)) return cfg.saveKey;
  const key = randomBytes(32).toString("base64url");
  await writePrivate(path, { ...cfg, saveKey: key });
  return key;
}

const linksPath = () => join(home(), "games.json");
export async function linkedCloudId(id: string) {
  return (await readJson<Record<string, string>>(linksPath(), {}))[gameDir(id)] ?? null;
}
export async function links() {
  return readJson<Record<string, string>>(linksPath(), {});
}
export async function linkCloud(id: string, cloudId: string) {
  const all = await links();
  if (all[gameDir(id)] !== cloudId) await writePrivate(linksPath(), { ...all, [gameDir(id)]: cloudId });
}

async function call<T = Record<string, unknown>>(path: string, init: RequestInit = {}, auth = true): Promise<T> {
  const url = cloudUrl();
  if (!url) throw new HttpError(503, "Cloud is turned off (DREAMSY_CLOUD=0)");
  const res = await fetch(`${url}${path}`, {
    cache: "no-store",
    ...init,
    headers: { ...(auth ? { authorization: `Bearer ${await saveKey()}` } : {}), ...init.headers },
    signal: AbortSignal.timeout(30_000),
  });
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { error?: string };
    throw new HttpError(res.status === 404 ? 404 : 502, body.error ?? `Cloud error ${res.status}`);
  }
  return res.json() as Promise<T>;
}

async function fetchBinary(path: string, auth: boolean, revalidate?: number) {
  const url = cloudUrl();
  if (!url) throw new HttpError(503, "Cloud is turned off");
  const res = await fetch(`${url}${path}`, {
    headers: auth ? { authorization: `Bearer ${await saveKey()}` } : {},
    signal: AbortSignal.timeout(60_000),
    ...(revalidate ? { cache: "force-cache", next: { revalidate } } : { cache: "no-store" }),
  });
  if (!res.ok) throw new HttpError(res.status === 404 ? 404 : 502, `Cloud error ${res.status}`);
  return res;
}

async function fetchBytes(path: string, auth = true) {
  return new Uint8Array(await (await fetchBinary(path, auth)).arrayBuffer());
}

const nameOf = async (id: string) => /^name\s*=\s*"([^"]*)"/m.exec(await readFile(join(gameDir(id), "dreamsy.toml"), "utf8").catch(() => ""))?.[1] ?? id;

/** Upload the game's current files as a new cloud version. */
export async function syncGame(id: string, label: string) {
  if (!cloudUrl()) return null;
  const tmp = await mkdtemp(join(tmpdir(), "dreamsy-sync-"));
  try {
    await tool("pack", gameDir(id), join(tmp, "game.dpk"));
    const pack = await readFile(join(tmp, "game.dpk"));
    const name = await nameOf(id);
    let cloudId = await linkedCloudId(id);
    const upload = (cid: string) =>
      call<{ version: number }>(`/v1/games/${cid}/versions`, {
        method: "POST",
        body: new Uint8Array(pack),
        headers: { "content-type": "application/octet-stream", "x-dreamsy-label": encodeURIComponent(label.slice(0, 200)) },
      });
    let saved: { version: number } | null = null;
    if (cloudId) {
      try {
        saved = await upload(cloudId);
        await call(`/v1/games/${cloudId}`, { method: "PATCH", body: JSON.stringify({ name }), headers: { "content-type": "application/json" } }).catch(() => undefined);
      } catch (e) {
        if ((e as HttpError).status !== 404) throw e;
        cloudId = null;
      }
    }
    if (!cloudId) {
      cloudId = (await call<{ id: string }>("/v1/games", { method: "POST", body: JSON.stringify({ name }), headers: { "content-type": "application/json" } })).id;
      saved = await upload(cloudId);
    }
    await linkCloud(id, cloudId);
    const thumb = join(dataDir(id), "thumb.jpg");
    if (existsSync(thumb)) {
      await call(`/v1/games/${cloudId}/versions/${saved!.version}/thumb`, { method: "PUT", body: new Uint8Array(await readFile(thumb)), headers: { "content-type": "image/jpeg" } }).catch(() => undefined);
    }
    return { cloudId, version: saved!.version };
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
}

export interface PublicationInfo {
  slug: string;
  version: number;
  author: string;
  link: string;
  email: string;
  remix: number;
  plays: number;
}

export async function cloudStatus(id: string) {
  const cloudId = await linkedCloudId(id);
  if (!cloudId || !cloudUrl()) return { cloudId: null, versions: 0, publication: null as PublicationInfo | null };
  try {
    const r = await call<{ versions: unknown[]; publication: PublicationInfo | null }>(`/v1/games/${cloudId}`);
    return { cloudId, versions: r.versions.length, publication: r.publication };
  } catch (e) {
    if ((e as HttpError).status === 404) return { cloudId: null, versions: 0, publication: null };
    throw e;
  }
}

export async function publishGame(id: string, info: { author?: string; link?: string; email?: string; remix?: boolean }) {
  const saved = await syncGame(id, "publish");
  if (!saved) throw new HttpError(503, "Cloud is turned off");
  return call<{ slug: string; version: number }>(`/v1/games/${saved.cloudId}/publish`, { method: "POST", body: JSON.stringify(info), headers: { "content-type": "application/json" } });
}

export async function unpublishGame(id: string) {
  const cloudId = await linkedCloudId(id);
  if (!cloudId) return;
  await call(`/v1/games/${cloudId}/publish`, { method: "DELETE" });
}

export interface LibraryGame {
  slug: string;
  name: string;
  author: string;
  link: string | null;
  remix: boolean;
  plays: number;
  updated: number;
  hasThumb: boolean;
}

export async function library(q: string, before?: number) {
  const params = new URLSearchParams({ limit: "36" });
  if (q) params.set("q", q);
  if (before) params.set("before", String(before));
  const init: RequestInit = !q && !before ? { cache: "force-cache", next: { revalidate: 15 } } : {};
  return call<{ games: LibraryGame[]; total: number; next: number | null }>(`/v1/public/games?${params}`, init, false);
}

export async function publicGame(slug: string) {
  const init: RequestInit = { cache: "force-cache", next: { revalidate: 30 } };
  return (await call<{ game: LibraryGame & { remixOf: string | null; version: number } }>(`/v1/public/games/${encodeURIComponent(slug)}`, init, false)).game;
}

export async function countPlay(slug: string) {
  await call(`/v1/public/games/${encodeURIComponent(slug)}/play`, { method: "POST" }, false).catch(() => undefined);
}

export async function libraryThumb(slug: string) {
  return fetchBytes(`/v1/public/games/${encodeURIComponent(slug)}/thumb`, false);
}

export function publicThumbnail(slug: string) {
  return fetchBinary(`/v1/public/games/${encodeURIComponent(slug)}/thumb`, false, 300);
}

export async function publicDownload(slug: string) {
  const game = await publicGame(slug);
  const download = await fetchBinary(`/v1/public/games/${encodeURIComponent(slug)}/package`, false);
  return { game, download };
}

export async function libraryPackage(slug: string) {
  const meta = await call<{ game: LibraryGame }>(`/v1/public/games/${encodeURIComponent(slug)}`, {}, false);
  return { game: meta.game, bytes: await fetchBytes(`/v1/public/games/${encodeURIComponent(slug)}/package`, false) };
}

async function unpackInto(id: string, bytes: Uint8Array) {
  const tmp = await mkdtemp(join(tmpdir(), "dreamsy-import-"));
  try {
    await writeFile(join(tmp, "game.dpk"), bytes);
    await tool("unpack", join(tmp, "game.dpk"), gameDir(id));
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
}

/** Copy a library game onto this computer as a new editable game (a remix in the cloud when allowed). */
export async function importFromLibrary(slug: string) {
  const { game, bytes } = await libraryPackage(slug);
  const id = await createGame(game.name);
  await unpackInto(id, bytes);
  if (game.hasThumb) {
    const thumb = await libraryThumb(slug).catch(() => null);
    if (thumb) {
      await mkdir(dataDir(id), { recursive: true });
      await writeFile(join(dataDir(id), "thumb.jpg"), thumb);
    }
  }
  if (game.remix) {
    const r = await call<{ id: string }>(`/v1/public/games/${encodeURIComponent(slug)}/remix`, { method: "POST" }).catch(() => null);
    if (r) await linkCloud(id, r.id);
  }
  await setMeta(id, { from: { slug, name: game.name, author: game.author } });
  await snapshot(id, `Copied from the library: ${game.name}`);
  return id;
}

export interface MineGame {
  id: string;
  name: string;
  updated: number;
  latest: number | null;
  published: string | null;
  hasThumb: boolean;
}

export async function mine() {
  return (await call<{ games: MineGame[] }>("/v1/games")).games;
}

/** Bring one of your cloud games onto this computer. */
export async function importMine(cloudId: string) {
  const all = await mine();
  const g = all.find((x) => x.id === cloudId);
  if (!g || g.latest === null) throw new HttpError(404, "No such cloud game");
  const bytes = await fetchBytes(`/v1/games/${cloudId}/versions/${g.latest}/package`);
  const id = await createGame(g.name);
  await unpackInto(id, bytes);
  await linkCloud(id, cloudId);
  await snapshot(id, "Downloaded from the cloud");
  return id;
}

export const thumbUrl = (cloudId: string) => `${cloudUrl()}/v1/thumbs/${cloudId}`;
