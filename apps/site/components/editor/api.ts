import type { BuildRecord, GameDetail, GameSummary, KeyStatus, Provider } from "@/lib/editor-types";

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

export interface MineGame {
  id: string;
  name: string;
  updated: number;
  latest: number | null;
  published: string | null;
  hasThumb: boolean;
  localId: string | null;
}

export interface Publication {
  slug: string;
  version: number;
  author: string;
  link: string;
  email: string;
  remix: number;
  plays: number;
}

export interface CloudStatus {
  cloudId: string | null;
  versions: number;
  publication: Publication | null;
}

async function call<T>(url: string, init?: RequestInit): Promise<T> {
  const res = await fetch(url, { ...init, headers: { "content-type": "application/json", ...init?.headers } });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((body as { error?: string }).error ?? `Request failed (${res.status})`);
  return body as T;
}

export const api = {
  games: () => call<{ games: GameSummary[]; bannersPending?: boolean }>("/api/games"),
  create: (name: string) => call<{ id: string }>("/api/games", { method: "POST", body: JSON.stringify({ name }) }),
  game: (id: string) => call<{ game: GameDetail; running: boolean }>(`/api/games/${id}`),
  rename: (id: string, name: string) => call(`/api/games/${id}`, { method: "PATCH", body: JSON.stringify({ name }) }),
  build: (id: string, prompt: string, images: { mediaType: string; base64: string }[]) =>
    call<{ build: BuildRecord }>(`/api/games/${id}/build`, { method: "POST", body: JSON.stringify({ prompt, images }) }),
  cancel: (id: string) => call(`/api/games/${id}/build`, { method: "DELETE" }),
  saveFile: (id: string, path: string, content: string) => call(`/api/games/${id}/file`, { method: "PUT", body: JSON.stringify({ path, content }) }),
  restore: (id: string, v: number) => call(`/api/games/${id}/versions/${v}`, { method: "POST" }),
  library: (q: string, before?: number | null) => call<{ games: LibraryGame[]; total: number; next: number | null }>(`/api/library?q=${encodeURIComponent(q)}${before ? `&before=${before}` : ""}`),
  importLibrary: (slug: string) => call<{ id: string }>(`/api/library/${slug}/import`, { method: "POST" }),
  mine: () => call<{ games: MineGame[] }>("/api/cloud/mine"),
  importMine: (cloudId: string) => call<{ id: string }>(`/api/cloud/mine/${cloudId}/import`, { method: "POST" }),
  cloudStatus: (id: string) => call<CloudStatus>(`/api/games/${id}/publish`),
  publish: (id: string, info: { author: string; link: string; email: string; remix: boolean }) =>
    call<{ status: CloudStatus }>(`/api/games/${id}/publish`, { method: "POST", body: JSON.stringify(info) }),
  remix: (id: string) => call<{ id: string }>(`/api/games/${id}/remix`, { method: "POST" }),
  sync: (id: string) => call(`/api/games/${id}/sync`, { method: "POST" }),
  keys: () => call<KeyStatus>("/api/keys"),
  saveKey: (provider: Provider, key: string) => call<KeyStatus>("/api/keys", { method: "POST", body: JSON.stringify({ provider, key }) }),
  removeKey: (provider: Provider) => call<KeyStatus>(`/api/keys?provider=${provider}`, { method: "DELETE" }),
};

export const hasBuildKey = (k: KeyStatus | null) => !!(k?.anthropic || k?.openai);

export function ago(ms: number) {
  const s = Math.round((Date.now() - ms) / 1000);
  if (s < 60) return "just now";
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return new Date(ms).toLocaleDateString();
}

export async function fileToImage(f: File) {
  const buf = new Uint8Array(await f.arrayBuffer());
  let bin = "";
  for (let i = 0; i < buf.length; i += 0x8000) bin += String.fromCharCode(...buf.subarray(i, i + 0x8000));
  return { mediaType: f.type, base64: btoa(bin) };
}
