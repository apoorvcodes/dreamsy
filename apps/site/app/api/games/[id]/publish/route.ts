import { activeRun } from "@/lib/server/builds";
import { cloudStatus, publishGame } from "@/lib/server/cloud";
import { fail, gameDir, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

type Ctx = { params: Promise<{ id: string }> };

export async function GET(_req: Request, { params }: Ctx) {
  try {
    const { id } = await params;
    gameDir(id);
    return Response.json(await cloudStatus(id));
  } catch (e) {
    return fail(e);
  }
}

export async function POST(req: Request, { params }: Ctx) {
  try {
    const { id } = await params;
    gameDir(id);
    if (activeRun(id)) throw new HttpError(409, "Wait for the current build to finish");
    const b = (await req.json()) as { author?: string; link?: string; email?: string; remix?: boolean };
    const pub = await publishGame(id, { author: b.author?.trim(), link: b.link?.trim(), email: b.email?.trim(), remix: b.remix !== false });
    return Response.json({ publication: pub, status: await cloudStatus(id) });
  } catch (e) {
    return fail(e);
  }
}

export async function DELETE() {
  return fail(new HttpError(403, "Published games stay in the library. Remix it to make a new version."));
}
