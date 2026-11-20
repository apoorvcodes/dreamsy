import { publicThumbnail } from "@/lib/server/cloud";
import { fail, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";

export async function GET(_req: Request, { params }: { params: Promise<{ slug: string }> }) {
  try {
    const { slug } = await params;
    if (!/^[a-z0-9]{4,32}$/.test(slug)) throw new HttpError(404, "No such game");
    const thumbnail = await publicThumbnail(slug);
    return new Response(thumbnail.body, { headers: { "content-type": "image/jpeg", "cache-control": "public, max-age=300" } });
  } catch (e) {
    return fail(e);
  }
}
