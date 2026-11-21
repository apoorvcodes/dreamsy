import { publicDownload } from "@/lib/server/cloud";
import { fail, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";

export async function GET(_req: Request, { params }: { params: Promise<{ slug: string }> }) {
  try {
    const { slug } = await params;
    if (!/^[a-z0-9]{4,32}$/.test(slug)) throw new HttpError(404, "No such game");
    const { game, download } = await publicDownload(slug);
    const file = `${game.name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || slug}.dpk`;
    return new Response(download.body, { headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="${file}"`,
      "cache-control": "public, max-age=60",
    } });
  } catch (e) {
    return fail(e);
  }
}
