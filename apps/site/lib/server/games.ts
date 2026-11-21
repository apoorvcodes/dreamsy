import { existsSync } from "node:fs";
import { mkdir, mkdtemp, readdir, readFile, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import type { BuildRecord, GameDetail, GameFile, GameSummary, VersionInfo } from "../editor-types";
import { dataDir, gameDir, GAMES_DIR, HttpError, inside, isGameId } from "./paths";
import { requestBanner } from "./banners";
import { tool } from "./tool";

const nameOf = (toml: string) => /^name\s*=\s*"([^"]*)"/m.exec(toml)?.[1];

async function readJson<T>(path: string, fallback: T): Promise<T> {
  try {
    return JSON.parse(await readFile(path, "utf8")) as T;
  } catch {
    return fallback;
  }
}

async function visible(dir: string) {
  return (await readdir(dir).catch(() => [] as string[])).filter((f) => !f.startsWith("."));
}

export async function history(id: string): Promise<BuildRecord[]> {
  return readJson(join(dataDir(id), "builds.json"), [] as BuildRecord[]);
}

export async function saveHistory(id: string, builds: BuildRecord[]) {
  await mkdir(dataDir(id), { recursive: true });
  await writeFile(join(dataDir(id), "builds.json"), JSON.stringify(builds));
}

export async function meta(id: string) {
  return readJson(join(dataDir(id), "meta.json"), {} as GameMeta);
}

interface GameMeta {
  name?: string;
  created?: number;
  from?: { slug: string; name: string; author: string };
  /** Library slug once published (published games are read-only), null when known to be unpublished. */
  published?: string | null;
}

export async function setMeta(id: string, patch: Pick<GameMeta, "from" | "published">) {
  await mkdir(dataDir(id), { recursive: true });
  await writeFile(join(dataDir(id), "meta.json"), JSON.stringify({ ...(await meta(id)), ...patch }));
}

async function displayName(id: string) {
  const toml = await readFile(join(gameDir(id), "dreamsy.toml"), "utf8").catch(() => "");
  return nameOf(toml) ?? (await meta(id)).name ?? id;
}

async function updatedAt(id: string) {
  const dir = gameDir(id);
  const s = await stat(join(dir, "dreamsy.toml")).catch(() => stat(dir));
  return s.mtimeMs;
}

export async function listGames(): Promise<GameSummary[]> {
  await mkdir(GAMES_DIR, { recursive: true });
  const ids = (await visible(GAMES_DIR)).filter(isGameId);
  const games = await Promise.all(
    ids.map(async (id) => {
      const s = await stat(gameDir(id));
      if (!s.isDirectory()) return null;
      const builds = await history(id);
      const empty = (await visible(gameDir(id))).length === 0;
      if (!empty) requestBanner(id);
      const assets = await readdir(join(gameDir(id), "assets")).catch(() => [] as string[]);
      const svgs = assets.filter((f) => f.endsWith(".svg"));
      const pick = svgs.find((f) => /logo|title|banner|cover/.test(f)) ?? svgs.find((f) => /hero|player|idle/.test(f)) ?? svgs[0];
      return {
        id,
        name: await displayName(id),
        updated: await updatedAt(id),
        builds: builds.length,
        hasThumb: existsSync(join(dataDir(id), "thumb.jpg")),
        empty,
        ...(pick ? { banner: `assets/${pick}` } : {}),
      } as GameSummary;
    }),
  );
  return games.filter((g): g is GameSummary => !!g).sort((a, b) => b.updated - a.updated);
}

const slug = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40) || "game";

export async function createGame(name: string) {
  const clean = name.trim().slice(0, 80) || "Untitled game";
  let id = slug(clean);
  for (let n = 2; existsSync(join(GAMES_DIR, id)); n++) id = `${slug(clean)}-${n}`;
  await mkdir(gameDir(id), { recursive: true });
  await mkdir(dataDir(id), { recursive: true });
  await writeFile(join(dataDir(id), "meta.json"), JSON.stringify({ name: clean, created: Date.now() }));
  return id;
}

async function walk(root: string, rel = ""): Promise<GameFile[]> {
  const out: GameFile[] = [];
  for (const f of (await readdir(join(root, rel), { withFileTypes: true })).sort((a, b) => a.name.localeCompare(b.name))) {
    if (f.name.startsWith(".")) continue;
    const p = rel ? `${rel}/${f.name}` : f.name;
    if (f.isDirectory()) out.push(...(await walk(root, p)));
    else out.push({ path: p, size: (await stat(join(root, p))).size });
  }
  return out;
}

export async function versions(id: string): Promise<VersionInfo[]> {
  const dir = join(dataDir(id), "versions");
  const list = await readJson(join(dir, "index.json"), [] as VersionInfo[]);
  return list.filter((v) => existsSync(join(dir, `v${v.version}.dpk`)));
}

export async function snapshot(id: string, label: string, buildId?: string): Promise<number | null> {
  if (!(await visible(gameDir(id))).length) return null;
  const dir = join(dataDir(id), "versions");
  await mkdir(dir, { recursive: true });
  const list = await readJson(join(dir, "index.json"), [] as VersionInfo[]);
  const version = list.length ? Math.max(...list.map((v) => v.version)) + 1 : 0;
  await tool("pack", gameDir(id), join(dir, `v${version}.dpk`));
  list.push({ version, label, at: Date.now(), buildId });
  await writeFile(join(dir, "index.json"), JSON.stringify(list));
  return version;
}

export async function restore(id: string, version: number) {
  const file = join(dataDir(id), "versions", `v${version}.dpk`);
  if (!existsSync(file)) throw new HttpError(404, "No such version");
  for (const f of await visible(gameDir(id))) await rm(join(gameDir(id), f), { recursive: true, force: true });
  await tool("unpack", file, gameDir(id));
}

export async function detail(id: string): Promise<GameDetail> {
  const dir = gameDir(id);
  if (!existsSync(dir)) throw new HttpError(404, "No such game");
  const toml = await readFile(join(dir, "dreamsy.toml"), "utf8").catch(() => "");
  return {
    id,
    name: await displayName(id),
    orientation: /^orientation\s*=\s*"(\w+)"/m.exec(toml)?.[1] === "landscape" ? "landscape" : "portrait",
    files: existsSync(dir) ? await walk(dir) : [],
    builds: await history(id),
    versions: await versions(id),
    hasThumb: existsSync(join(dataDir(id), "thumb.jpg")),
  };
}

export async function readGameFile(id: string, path: string) {
  return readFile(inside(gameDir(id), path));
}

export async function rename(id: string, name: string) {
  const clean = name.trim().slice(0, 80);
  if (!clean) throw new HttpError(400, "Name can't be empty");
  const tomlPath = join(gameDir(id), "dreamsy.toml");
  const toml = await readFile(tomlPath, "utf8").catch(() => null);
  if (toml !== null && /^name\s*=/m.test(toml)) await writeFile(tomlPath, toml.replace(/^name\s*=\s*"[^"]*"/m, `name = ${JSON.stringify(clean)}`));
  await mkdir(dataDir(id), { recursive: true });
  await writeFile(join(dataDir(id), "meta.json"), JSON.stringify({ ...(await meta(id)), name: clean }));
}

export async function pack(id: string, version?: number) {
  if (version !== undefined) {
    const file = join(dataDir(id), "versions", `v${version}.dpk`);
    if (!existsSync(file)) throw new HttpError(404, "No such version");
    return readFile(file);
  }
  if (!(await visible(gameDir(id))).length) throw new HttpError(404, "Nothing built yet");
  const tmp = await mkdtemp(join(tmpdir(), "dreamsy-pack-"));
  try {
    await tool("pack", gameDir(id), join(tmp, "game.dpk"));
    return await readFile(join(tmp, "game.dpk"));
  } catch (e) {
    throw new HttpError(422, `The game doesn't validate: ${(e as { stderr?: string }).stderr?.trim().split("\n").slice(-3).join(" ") ?? (e as Error).message}`);
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
}

const EDITABLE = [/^assets\/[a-z0-9_-]+\.svg$/, /^data\/music\/[a-z][a-z0-9_]*\.json$/, /^data\/sfx\/[a-z][a-z0-9_]*\.json$/];
const UNSAFE_SVG = /<script|<foreignObject|\son[a-z]+\s*=|javascript:|(?:href|src)\s*=\s*["']\s*(?:https?:|data:text)/i;

export async function writeGameFile(id: string, path: string, content: string) {
  if (!EDITABLE.some((re) => re.test(path))) throw new HttpError(400, "Only sprites (assets/*.svg), music and sound effects can be edited here");
  if (content.length > 400_000) throw new HttpError(413, "File is too large");
  if (path.endsWith(".svg")) {
    if (!/^\s*(<\?xml[^>]*>\s*)?<svg[\s>]/i.test(content) || !/<\/svg>\s*$/i.test(content)) throw new HttpError(400, "Not an SVG document");
    if (UNSAFE_SVG.test(content)) throw new HttpError(400, "SVG can't contain scripts, event handlers or external links");
  } else {
    try {
      JSON.parse(content);
    } catch (e) {
      throw new HttpError(400, `Invalid JSON: ${(e as Error).message}`);
    }
  }
  const abs = inside(gameDir(id), path);
  await mkdir(join(abs, ".."), { recursive: true });
  await writeFile(abs, content);
  try {
    await tool("validate", gameDir(id));
  } catch (e) {
    throw new HttpError(422, `Saved, but the game no longer validates: ${(e as { stdout?: string; stderr?: string }).stdout?.trim().split("\n").slice(-3).join(" ") ?? (e as Error).message}`);
  }
  await snapshot(id, `Edited ${path}`);
}
