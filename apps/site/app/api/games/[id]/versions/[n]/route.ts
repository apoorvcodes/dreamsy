import { activeRun } from "@/lib/server/builds";
import { assertEditable, syncGame } from "@/lib/server/cloud";
import { restore, snapshot } from "@/lib/server/games";
import { fail, HttpError } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function POST(_req: Request, { params }: { params: Promise<{ id: string; n: string }> }) {
  try {
    const { id, n } = await params;
    if (activeRun(id)) throw new HttpError(409, "Wait for the current build to finish");
    const version = Number(n);
    if (!Number.isInteger(version) || version < 0) throw new HttpError(400, "Bad version");
    await assertEditable(id);
    await restore(id, version);
    await snapshot(id, `Restored version ${version}`);
    await syncGame(id, `Restored version ${version}`).catch(() => null);
    return Response.json({ ok: true });
  } catch (e) {
    return fail(e);
  }
}
