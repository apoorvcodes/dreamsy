import { extname } from "node:path";
import { activeRun } from "@/lib/server/builds";
import { assertEditable, syncGame } from "@/lib/server/cloud";
import { readGameFile, writeGameFile } from "@/lib/server/games";
import { fail, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const TYPES: Record<string, string> = {
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
  ".gif": "image/gif",
  ".ogg": "audio/ogg",
  ".mp3": "audio/mpeg",
  ".wav": "audio/wav",
};

export async function GET(req: Request, { params }: { params: Promise<{ id: string }> }) {
  try {
    const { id } = await params;
    const path = new URL(req.url).searchParams.get("path");
    if (!path) throw new HttpError(400, "Missing path");
    const bytes = await readGameFile(id, path);
    const type = TYPES[extname(path).toLowerCase()] ?? "text/plain; charset=utf-8";
    return new Response(new Uint8Array(bytes), {
      headers: { "content-type": type, "cache-control": "no-store", "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'", "x-content-type-options": "nosniff" },
    });
  } catch (e) {
    return fail(e);
  }
}

export async function PUT(req: Request, { params }: { params: Promise<{ id: string }> }) {
  try {
    const { id } = await params;
    if (activeRun(id)) throw new HttpError(409, "Wait for the current build to finish");
    const { path, content } = (await req.json()) as { path?: string; content?: string };
    if (!path || typeof content !== "string") throw new HttpError(400, "Missing path or content");
    await assertEditable(id);
    await writeGameFile(id, path, content);
    const cloud = await syncGame(id, `Edited ${path}`).catch(() => null);
    return Response.json({ ok: true, cloud });
  } catch (e) {
    return fail(e);
  }
}
