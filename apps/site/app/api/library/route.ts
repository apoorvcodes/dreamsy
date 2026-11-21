import { library } from "@/lib/server/cloud";
import { fail } from "@/lib/server/paths";

export const runtime = "nodejs";

export async function GET(req: Request) {
  try {
    const u = new URL(req.url);
    const query = (u.searchParams.get("q") ?? "").trim().slice(0, 80);
    const before = Number(u.searchParams.get("before")) || undefined;
    const games = await library(query, before);
    return Response.json(games, { headers: { "cache-control": "public, max-age=15" } });
  } catch (e) {
    return fail(e);
  }
}
