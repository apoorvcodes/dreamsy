import { activeRun } from "@/lib/server/builds";
import { assertEditable } from "@/lib/server/cloud";
import { detail, rename } from "@/lib/server/games";
import { fail } from "@/lib/server/paths";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

type Ctx = { params: Promise<{ id: string }> };

export async function GET(_req: Request, { params }: Ctx) {
  try {
    const { id } = await params;
    const game = await detail(id);
    const run = activeRun(id);
    if (run) game.builds = [...game.builds.filter((b) => b.id !== run.record.id), run.record];
    return Response.json({ game, running: !!run });
  } catch (e) {
    return fail(e);
  }
}

export async function PATCH(req: Request, { params }: Ctx) {
  try {
    const { id } = await params;
    const { name } = (await req.json()) as { name?: string };
    await assertEditable(id);
    await rename(id, name ?? "");
    return Response.json({ ok: true });
  } catch (e) {
    return fail(e);
  }
}
