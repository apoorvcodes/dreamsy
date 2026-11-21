import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import { extname } from "node:path";
import { Readable } from "node:stream";
import { fail, HttpError, inside, PLAYER_DIR } from "@/lib/server/paths";
import { playerVersion } from "@/lib/server/player";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript",
  ".wasm": "application/wasm",
  ".data": "application/octet-stream",
  ".dpk": "application/octet-stream",
};

export async function GET(req: Request, { params }: { params: Promise<{ path: string[] }> }) {
  try {
    const { path } = await params;
    const versioned = /^[a-f0-9]{24}$/.test(path[0] ?? "");
    const rel = (versioned ? path.slice(1) : path).join("/");
    if (!/^(dreamsy-player\.(html|js|wasm|data)|kits\/[a-z][a-z0-9-]*\.dpk)$/.test(rel)) throw new HttpError(404, "Not found");
    if (versioned && path[0] !== await playerVersion()) throw new HttpError(404, "Not found");

    const file = versioned && rel === "dreamsy-player.html" ? "dreamsy-player.preload.html" : rel;
    const filename = inside(PLAYER_DIR, file);
    await stat(filename).catch((error: NodeJS.ErrnoException) => {
      if (error.code !== "ENOENT") throw error;
      if (versioned) throw new HttpError(404, "Not found");
      throw new HttpError(503, "The web player isn't built. Run: cd engine && cmake --preset web && cmake --build --preset web --target dreamsy-player");
    });
    const headers = new Headers({
      "content-type": TYPES[extname(rel)] ?? "application/octet-stream",
      "cache-control": versioned ? "public, max-age=31536000, immutable" : "no-cache",
    });
    const body = req.method === "HEAD" ? null : Readable.toWeb(createReadStream(filename)) as ReadableStream<Uint8Array>;
    return new Response(body, { headers });
  } catch (e) {
    return fail(e);
  }
}

export const HEAD = GET;
