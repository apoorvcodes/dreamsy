import { existsSync, readdirSync, readFileSync } from "node:fs";
import { extname, join } from "node:path";
import type { StylePreset } from "@dreamsy/format";
import { repoPath } from "../paths.js";

export interface TastePack {
  id: string;
  name: string;
  preset: StylePreset;
  grid?: 8 | 16 | 32;
  view: string;
  palette: string[];
  direction: string;
  refs: { mediaType: string; base64: string }[];
}

const ROOT = repoPath("taste") + "/";

function parse(id: string, text: string): TastePack | null {
  const m = text.replace(/\r\n/g, "\n").match(/^---\n([\s\S]*?)\n---\n([\s\S]*)$/);
  if (!m) return null;
  const meta: Record<string, string> = {};
  for (const line of m[1]!.split("\n")) {
    const i = line.indexOf(":");
    if (i > 0) meta[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  }
  const list = (v = "") => v.replace(/^\[|\]$/g, "").split(",").map((s) => s.trim().replace(/^"|"$/g, "")).filter(Boolean);
  const refs = list(meta.refs).flatMap((f) => {
    const path = join(ROOT, "refs", id, f);
    if (!existsSync(path)) return [];
    const ext = extname(f).toLowerCase();
    return [{ mediaType: ext === ".jpg" || ext === ".jpeg" ? "image/jpeg" : ext === ".webp" ? "image/webp" : "image/png", base64: readFileSync(path).toString("base64") }];
  });
  return {
    id,
    name: meta.name ?? id,
    preset: (meta.preset ?? "pixel") as StylePreset,
    grid: meta.grid ? (Number(meta.grid) as 8 | 16 | 32) : undefined,
    view: meta.view ?? "side",
    palette: list(meta.palette),
    direction: m[2]!.trim(),
    refs,
  };
}

let cache: Map<string, TastePack> | null = null;
export function tastePacks(): Map<string, TastePack> {
  if (cache) return cache;
  cache = new Map();
  const dir = join(ROOT, "packs");
  if (!existsSync(dir)) return cache;
  for (const f of readdirSync(dir).filter((f) => f.endsWith(".md")).sort()) {
    const id = f.slice(0, -3);
    const pack = parse(id, readFileSync(join(dir, f), "utf8"));
    if (pack) cache.set(id, pack);
  }
  return cache;
}

export function packMenu(): string {
  return [...tastePacks().values()].map((p) => `- ${p.id}: ${p.name} (${p.preset}, ${p.view}) — ${p.direction.split("\n")[0]}`).join("\n");
}
