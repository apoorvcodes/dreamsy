import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { dataDir, fail, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(_req: Request, { params }: { params: Promise<{ id: string }> }) {
  try {
    const { id } = await params;
    const bytes = await readFile(join(dataDir(id), "thumb.jpg")).catch(() => {
      throw new HttpError(404, "No thumbnail");
    });
    return new Response(new Uint8Array(bytes), { headers: { "content-type": "image/jpeg", "cache-control": "private, max-age=30" } });
  } catch (e) {
    return fail(e);
  }
}
