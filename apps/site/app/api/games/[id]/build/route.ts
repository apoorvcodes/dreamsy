import { activeRun, cancelBuild, startBuild, subscribe } from "@/lib/server/builds";
import { assertEditable } from "@/lib/server/cloud";
import { readKeys } from "@/lib/server/keys";
import { fail, gameDir, HttpError } from "@/lib/server/paths";
import type { StreamMessage } from "@/lib/editor-types";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

type Ctx = { params: Promise<{ id: string }> };
type Image = { mediaType: string; base64: string };

const IMAGE_TYPES = new Set(["image/png", "image/jpeg", "image/webp"]);

export async function POST(req: Request, { params }: Ctx) {
  try {
    const { id } = await params;
    gameDir(id);
    await assertEditable(id);
    const body = (await req.json()) as { prompt?: string; images?: Image[] };
    const images = (body.images ?? []).slice(0, 6);
    if (images.some((i) => !IMAGE_TYPES.has(i.mediaType) || typeof i.base64 !== "string" || i.base64.length > 8_000_000)) throw new HttpError(400, "Images must be PNG, JPEG or WebP under 6 MB");
    const record = await startBuild(id, body.prompt ?? "", images, await readKeys());
    return Response.json({ build: record });
  } catch (e) {
    return fail(e);
  }
}

export async function DELETE(_req: Request, { params }: Ctx) {
  const { id } = await params;
  return Response.json({ canceled: cancelBuild(id) });
}

export async function GET(req: Request, { params }: Ctx) {
  const { id } = await params;
  const enc = new TextEncoder();
  let off: (() => void) | undefined;
  const stream = new ReadableStream({
    start(controller) {
      const write = (m: StreamMessage) => {
        try {
          controller.enqueue(enc.encode(`data: ${JSON.stringify(m)}\n\n`));
        } catch {}
      };
      const close = () => {
        off?.();
        try {
          controller.close();
        } catch {}
      };
      if (!activeRun(id)) {
        write({ type: "idle" });
        return close();
      }
      const sub = subscribe(id, (m) => {
        write(m);
        if (m.type === "done") close();
      });
      if (!sub) {
        write({ type: "idle" });
        return close();
      }
      off = sub.off;
      for (const event of sub.record.events) write({ type: "event", buildId: sub.record.id, event });
      req.signal.addEventListener("abort", close);
    },
    cancel() {
      off?.();
    },
  });
  return new Response(stream, { headers: { "content-type": "text/event-stream", "cache-control": "no-cache, no-transform", connection: "keep-alive" } });
}
