import { importFromLibrary, publishedSlug } from "@/lib/server/cloud";
import { fail, gameDir, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

/** A new, editable copy of a published game (a remix in the library's remix chain). */
export async function POST(_req: Request, { params }: { params: Promise<{ id: string }> }) {
  try {
    const { id } = await params;
    gameDir(id);
    const slug = await publishedSlug(id);
    if (!slug) throw new HttpError(400, "Only published games are remixed; this one is still editable.");
    return Response.json({ id: await importFromLibrary(slug) });
  } catch (e) {
    return fail(e);
  }
}
