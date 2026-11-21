import { mkdir, readdir, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { dirname, isAbsolute, join, relative, resolve } from "node:path";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { Package, STYLE_PRESETS, validatePackage, type StylePreset } from "@dreamsy/format";
import { finalGate } from "../agent/final-gate.js";
import { applyOps } from "../agent/tools/apply-ops.js";
import { getEntities, readFiles } from "../agent/tools/files.js";
import { playtest as playtestTool } from "../agent/tools/playtest.js";
import { drawSprite } from "../agent/tools/sprites.js";
import { COMPOSER_GUIDE } from "../agent/tools/music.js";
import { playUrl, publishToCloud, saveToCloud, type CloudConfig } from "../cloud.js";
import type { Tool, ToolCtx, ToolOutput } from "../agent/tools/tool.js";
import { Workspace } from "../agent/workspace.js";
import { kitMenu, kits } from "../context/kits.js";
import { describeGame, systemBlocks } from "../context/prompt.js";
import { packMenu, tastePacks } from "../context/taste.js";
import { kitTemplate, templateNote } from "../context/templates.js";
import { describeRun, enginePlaytester, type Playtester, type TimelineStep } from "../engine/playtest.js";
import { disabledImages } from "../llm/images.js";
import { CostMeter } from "../llm/models.js";
import { newTrace } from "../memory/lessons.js";
import { PLANS } from "../pipeline/plans.js";
import { expandKits } from "@dreamsy/format";


type Content = { type: "text"; text: string } | { type: "image"; data: string; mimeType: string };
type Result = { content: Content[]; isError?: boolean };

const text = (t: string, isError = false): Result => ({ content: [{ type: "text", text: t }], ...(isError ? { isError: true } : {}) });
const jpeg = (b: Uint8Array): Content => ({ type: "image", data: Buffer.from(b).toString("base64"), mimeType: "image/jpeg" });

function gameDirIn(root: string, path: string, remote: boolean): string {
  const dir = resolve(root, path);
  const rel = relative(root, dir);
  if (!rel || rel.startsWith("..") || isAbsolute(rel) || (remote && rel.includes("/"))) throw new Error(remote ? `${path} is not a game id; use the id create_game returned` : `games live under ${root}; ${path} is outside it`);
  return dir;
}

async function loadIn(root: string, path: string, remote: boolean): Promise<{ dir: string; pkg: Package }> {
  const dir = gameDirIn(root, path, remote);
  if (!existsSync(join(dir, "dreamsy.toml"))) throw new Error(`no game at ${path} (create_game first)`);
  return { dir, pkg: await Package.fromDir(dir) };
}

async function save(dir: string, before: Package, after: Package) {
  for (const p of before.paths()) if (!after.has(p)) await rm(join(dir, p), { force: true });
  for (const p of after.paths()) {
    const b = after.bytes(p)!;
    const old = before.bytes(p);
    if (old && old.byteLength === b.byteLength && old.every((x, i) => x === b[i])) continue;
    await mkdir(dirname(join(dir, p)), { recursive: true });
    await writeFile(join(dir, p), b);
  }
}

let playtester: Promise<Playtester> | null = null;
const engine = () => (playtester ??= enginePlaytester({ browser: process.env.PLAYTEST_BROWSER !== "0" }));

function context(ws: Workspace, pt: Playtester): ToolCtx {
  const manifest = validatePackage(ws.pkg).manifest;
  return {
    ws, playtest: pt, images: disabledImages, emit: async () => {}, signal: new AbortController().signal,
    cost: new CostMeter(), plan: PLANS.tweak, trace: newTrace(), kitIds: manifest?.kits ?? [],
    packId: manifest?.style.pack ?? null, pack: manifest?.style.pack ? tastePacks().get(manifest.style.pack) : undefined,
    state: { reviewed: true },
    llm: { turn: () => Promise.reject(new Error("no model in the MCP server")) },
  };
}

function toResult(out: ToolOutput): Result {
  return { content: [{ type: "text", text: out.text }, ...(out.images ?? []).map(jpeg)], ...(out.isError ? { isError: true } : {}) };
}

export interface ServerOptions {
  root: string;
  cloud: (dir: string) => CloudConfig | null;
  links: { get(dir: string): string | null; set(dir: string, gameId: string): void };
  /** Hosted: games are named by an unguessable id that create_game returns, and expire when unused. */
  remote?: {
    newGameId(name: string): string;
    touch(dir: string): void;
    /** Library slug once published; published games are final (remix_game makes an editable copy). */
    published(dir: string): string | null;
    markPublished(dir: string, slug: string): void;
  };
}

export function createServer(opts: ServerOptions): McpServer {
const ROOT = opts.root;
const remote = !!opts.remote;
const gameDir = (path: string) => gameDirIn(ROOT, path, remote);
const load = async (path: string) => {
  const game = await loadIn(ROOT, path, remote);
  opts.remote?.touch(game.dir);
  return game;
};
const assertEditable = (dir: string) => {
  const slug = opts.remote?.published(dir);
  if (slug) throw new Error(`this game is published (${playUrl(slug)}), so it's final. Call remix_game to get an editable copy.`);
};
const INSTRUCTIONS = remote
  ? `Dreamsy builds 2D and 2.5D games that run in the Dreamsy C++ engine. You are the game designer and programmer; these tools are the engine, hosted at dreamsy.dev.
Workflow: dreamsy_options (kits, styles, packs) → create_game → dreamsy_guide (the full format, entity catalog, Luau API and kit docs: read it before editing) → build with apply_ops and draw_sprite → playtest to see it → check_game until it passes → publish_game to get a dreamsy.dev/play link the user can open on any phone (ask the user first, and what creator name to show).
create_game returns the game's id (like "frog-hop-7fk2m9qx"): pass exactly that id as \`game\` to every other tool. Anyone with the id can open the game, and games unused for 24 hours are deleted, so publish before you finish.`
  : `Dreamsy builds 2D and 2.5D games that run in the Dreamsy C++ engine. You are the game designer and programmer; these tools are the engine.
Workflow: dreamsy_options (kits, styles, packs) → create_game → dreamsy_guide (the full format, entity catalog, Luau API and kit docs: read it before editing) → build with apply_ops and draw_sprite → playtest to see it → check_game until it passes → export_game (also saves it to the Dreamsy cloud) → publish_game if the user wants it in the public library.
Games are folders under ${ROOT}; every tool takes the game's path relative to it.`;
const server = new McpServer({
  name: "dreamsy",
  title: "Dreamsy",
  version: "0.2.0",
  websiteUrl: "https://dreamsy.dev",
  icons: [
    { src: "https://dreamsy.dev/icons/icon-512.png", mimeType: "image/png", sizes: ["512x512"] },
    { src: "https://dreamsy.dev/icons/icon-192.png", mimeType: "image/png", sizes: ["192x192"] },
  ],
}, { instructions: INSTRUCTIONS });

// ChatGPT rejects tools without annotations.
const READ = { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false };
const EDIT = { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: false };
const CLOUD = { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: true };
const ANNOTATIONS: Record<string, typeof READ> = {
  dreamsy_options: READ, dreamsy_guide: READ, game_outline: READ, read_files: READ, get_entities: READ,
  playtest: READ, check_game: READ, create_game: EDIT, apply_ops: { ...EDIT, destructiveHint: true }, draw_sprite: EDIT,
  export_game: CLOUD, publish_game: CLOUD,
};
const register = server.registerTool.bind(server);
server.registerTool = ((name: string, config: { annotations?: object }, cb: unknown) =>
  register(name, { ...config, annotations: { title: (config as { title?: string }).title, ...ANNOTATIONS[name], ...config.annotations } } as never, cb as never)) as typeof server.registerTool;

// ChatGPT calls resources/list during setup.
server.registerResource("kits", "dreamsy://kits", {
  title: "Dreamsy kits, styles and packs",
  description: "The genre kits, art style presets and house style packs a new game can start from.",
  mimeType: "text/markdown",
}, async (uri) => ({ contents: [{ uri: uri.href, mimeType: "text/markdown", text: `# Genre kits\n${kitMenu()}\n\n# Style presets\n${STYLE_PRESETS.join(", ")}\n\n# House style packs\n${packMenu()}` }] }));

function guarded<A>(fn: (args: A) => Promise<Result>) {
  return async (args: A): Promise<Result> => {
    try { return await fn(args); } catch (e) { return text((e as Error).message, true); }
  };
}

function editTool(tool: Tool, title: string, writes = false) {
  const input = (tool.input as unknown as z.ZodObject).extend({ game: z.string().describe("Game folder, relative to the games directory.") });
  server.registerTool(tool.name, { title, description: tool.description, inputSchema: input.shape }, guarded(async (args: Record<string, unknown>) => {
    const { game, ...rest } = args as { game: string };
    const { dir, pkg } = await load(game);
    if (writes) assertEditable(dir);
    const ws = new Workspace(pkg, disabledImages, async () => {}, new AbortController().signal, []);
    const out = await tool.run(tool.input.parse(rest), context(ws, await engine()));
    if (ws.pkg !== pkg) await save(dir, pkg, ws.pkg);
    return toResult(out);
  }));
}

server.registerTool("dreamsy_options", {
  title: "List kits, styles and style packs",
  description: "What a new game can start from: genre kits (tested systems + a template game), art style presets and curated house style packs.",
  inputSchema: {},
}, guarded(async () => text(`# Genre kits\n${kitMenu()}\n\n# Style presets\n${STYLE_PRESETS.join(", ")}\n\n# House style packs\n${packMenu()}`)));

server.registerTool("dreamsy_guide", {
  title: "Builder guide",
  description: "The full builder reference for a game: package format and edit ops, entity catalog, Luau scripting API, art style, house style pack (with reference images) and the docs of the game's kits. Read it before building or editing.",
  inputSchema: { game: z.string().describe("Game folder, relative to the games directory.") },
}, guarded(async ({ game }) => {
  const { pkg } = await load(game);
  const manifest = validatePackage(pkg).manifest;
  const pack = manifest?.style.pack ? tastePacks().get(manifest.style.pack) : undefined;
  const kitDocs = expandKits(manifest?.kits ?? []).map((id) => kits().get(id)).filter((k): k is NonNullable<typeof k> => !!k).map((k) => ({ id: k.id, doc: k.doc }));
  const blocks = systemBlocks(manifest?.style.preset ?? "clean", manifest, pack, kitDocs);
  const refs = pack?.refs.map((r): Content => ({ type: "image", data: r.base64, mimeType: r.mediaType })) ?? [];
  const mcpNote = `# Using this guide through MCP\nEvery tool takes \`game\` (${remote ? "the game id create_game returned" : "the folder"}). build_content, generate_image, find_sprites and compose_music are not available here: draw sprites with draw_sprite, write content yourself, and write music as data/music/<id>.json with apply_ops write_file following \"Composing music\" below (3-5 tracks for a new game). Where the guide says "finish", run check_game until it passes, then ${remote ? "publish_game (ask the user first)" : "export_game"}.`;
  const musicNote = `# Composing music\n${COMPOSER_GUIDE.replace("Return every requested track. Keep each track's id exactly as given.", "")}`;
  return { content: [{ type: "text", text: `${mcpNote}\n\n${trimForMcp(blocks.map((b) => b.text).join("\n\n"))}\n\n${musicNote}` }, ...refs] };
}));

server.registerTool("create_game", {
  title: "Create a game",
  description: "Create a new game folder. With a genre kit it starts as the kit's working template game (reskin and rebuild it); without one it starts empty.",
  inputSchema: {
    game: z.string().describe(remote ? "Short name for the game, e.g. frog-hop; the game id returned is based on it." : "New game folder, relative to the games directory."),
    name: z.string().min(1),
    kit: z.string().optional().describe("Genre kit id from dreamsy_options."),
    style: z.enum(STYLE_PRESETS as [StylePreset, ...StylePreset[]]).optional(),
    pack: z.string().optional().describe("House style pack id from dreamsy_options."),
    orientation: z.enum(["portrait", "landscape"]).optional(),
  },
}, guarded(async ({ game: requested, name, kit, style, pack, orientation }) => {
  const game = opts.remote ? opts.remote.newGameId(requested) : requested;
  const dir = gameDir(game);
  if (existsSync(dir) && (await readdir(dir)).length) throw new Error(`${game} already exists and is not empty`);
  const idNote = opts.remote ? `\nThe game id is "${game}": pass exactly this as \`game\` to every other tool.` : "";
  const packDef = pack ? tastePacks().get(pack) : undefined;
  if (pack && !packDef) throw new Error(`unknown pack ${pack}`);
  const preset = style ?? packDef?.preset ?? "clean";
  const manifestOps = `set_manifest name "${name}", orientation "${orientation ?? "portrait"}", style.preset "${preset}"${packDef ? `, style.pack "${packDef.id}", style.palette ${JSON.stringify(packDef.palette.slice(0, 32))}` : ""}`;
  const template = kit ? await kitTemplate([kit]) : null;
  if (kit && !template) throw new Error(`kit ${kit} has no template game; create it without a kit and add "${kit}" to manifest kits`);
  await mkdir(dir, { recursive: true });
  if (template) {
    const base = template.pkg.with("dreamsy.toml", (template.pkg.text("dreamsy.toml") ?? "").replace(/^id\s*=.*\n/m, ""));
    const ws = new Workspace(base, disabledImages, async () => {}, new AbortController().signal, []);
    const ops: Record<string, unknown>[] = [
      { op: "set_manifest", key: "name", value: name },
      { op: "set_manifest", key: "orientation", value: orientation ?? (/orientation\s*=\s*"landscape"/.test(base.text("dreamsy.toml") ?? "") ? "landscape" : "portrait") },
      { op: "set_manifest", key: "style.preset", value: preset },
      ...(packDef ? [{ op: "set_manifest", key: "style.pack", value: packDef.id }, { op: "set_manifest", key: "style.palette", value: packDef.palette.slice(0, 32) }] : []),
    ];
    const applied = ws.applyOps(ops, "create from kit template");
    await (applied.applied ? ws.pkg : base).toDir(dir);
    const note = applied.applied ? `Set name "${name}", style "${preset}"${packDef ? `, pack "${packDef.id}"` : ""}.` : `Couldn't set the manifest automatically (${applied.text.slice(0, 200)}); do it first: ${manifestOps}.`;
    return text(`Created ${game} from the ${template.kit} kit template. ${note}${idNote}\n${templateNote(template.kit)}\nNext: set the description and palette for the player's game, then call dreamsy_guide.`);
  }
  await writeFile(join(dir, "dreamsy.toml"), `format = "0"\nname = ${JSON.stringify(name)}\ndimension = "2d"\nentry_scene = "scenes/main.scene.json"\norientation = "${orientation ?? "portrait"}"\n\n[style]\npreset = "${preset}"\n${packDef ? `pack = "${packDef.id}"\n` : ""}`);
  return text(`Created an empty game at ${game}.${idNote}\nIt has a manifest but no scenes yet: add scenes/main.scene.json with add_scene, then build. Call dreamsy_guide first.`);
}));

server.registerTool("game_outline", {
  title: "Game outline",
  description: "Manifest, scene trees (ids, types, names), scripts and data files of a game, plus its validation issues.",
  inputSchema: { game: z.string() },
}, guarded(async ({ game }) => text(describeGame((await load(game)).pkg).text)));

editTool(readFiles, "Read game files");
editTool(getEntities, "Read entities");
editTool(applyOps, "Edit the game", true);
editTool(drawSprite, "Draw a sprite", true);

server.registerTool("playtest", {
  title: "Playtest",
  description: `${playtestTool.description} Screenshots need the web player build (engine/README.md); without it you get the simulation only.`,
  inputSchema: (playtestTool.input as unknown as z.ZodObject).extend({ game: z.string() }).shape,
}, guarded(async (args: Record<string, unknown>) => {
  const { game, ...rest } = args as { game: string };
  const d = playtestTool.input.parse(rest);
  const { pkg } = await load(game);
  if (!validatePackage(pkg).ok) return text("Fix the validation errors first (game_outline lists them).", true);
  const pt = await engine();
  const tl = d.timeline as TimelineStep[];
  const start = { scene: d.scene, sceneParams: d.sceneParams, save: d.save };
  const [sim, view] = await Promise.all([
    pt.simulate(pkg, tl, d.seconds, start),
    pt.browser ? pt.browser(pkg, tl, d.shots.filter((t) => t <= d.seconds).slice(0, 4), start) : Promise.resolve(null),
  ]);
  const shots = view ? `\n\nScreenshots at ${view.shots.map((s) => `${s.t}s`).join(", ") || "(none)"}${view.loadError ? `\nRENDERER LOAD FAILED: ${view.loadError}` : ""}` : "\n\n(no screenshots: web player not built or PLAYTEST_BROWSER=0)";
  return { content: [{ type: "text", text: `Simulation (${d.seconds}s):\n${describeRun(sim)}${shots}` }, ...(view?.shots ?? []).map((s) => jpeg(s.jpeg))] };
}));

server.registerTool("check_game", {
  title: "Check the game",
  description: "The release check: the package must validate and survive an 8 second smoke run (move, jump, action) in the engine with no script errors.",
  inputSchema: { game: z.string() },
}, guarded(async ({ game }) => {
  const gate = await finalGate((await load(game)).pkg, await engine());
  return gate.ok ? text("Passed: valid, boots, no script errors in the smoke run.") : text(`Not ready:\n${gate.errors.join("\n")}`, true);
}));

server.registerTool("export_game", {
  title: "Export the game",
  description: "Pack the game into a Dreamsy pack (.dpk): one file anyone can play in the Dreamsy web player (?pkg=<url>) or publish.",
  inputSchema: { game: z.string(), out: z.string().optional().describe("Output .dpk path, relative to the games directory; default <game>.dpk next to the folder.") },
}, guarded(async ({ game, out }) => {
  const { dir, pkg } = await load(game);
  const gate = await finalGate(pkg, await engine());
  if (!gate.ok) return text(`Run check_game and fix these first:\n${gate.errors.join("\n")}`, true);
  const bytes = pkg.toPack();
  if (remote) {
    const cloud = await cloudSave(dir, pkg, "export");
    return text(`Packed ${game} (${(bytes.byteLength / 1024).toFixed(0)} KB). ${cloud}\nTo get a link anyone can play on their phone, ask the user if they want it published, then call publish_game.`);
  }
  const file = out ? gameDir(out) : `${dir}.dpk`;
  await writeFile(file, bytes);
  const cloud = await cloudSave(dir, pkg, "export");
  return text(`Wrote ${file} (${(bytes.byteLength / 1024).toFixed(0)} KB, package id ${pkg.contentHash().slice(0, 16)}).\n${cloud}\nTo get a link anyone can play on their phone, ask the user if they want it published, then call publish_game.`);
}));

const MCP_SKIP = [/^# Multiplayer\b/, /^# Monetization\b/, /^## Networking\b/, /^## Store\b/];

/** Drop guide sections MCP builds can't use (multiplayer rooms, the Stars store, monetization). */
function trimForMcp(guide: string) {
  const out: string[] = [];
  let skipLevel = 0;
  for (const line of guide.split("\n")) {
    const h = /^(#{1,6}) /.exec(line);
    if (h) {
      const level = h[1]!.length;
      if (skipLevel && level <= skipLevel) skipLevel = 0;
      if (!skipLevel && MCP_SKIP.some((re) => re.test(line))) skipLevel = level;
    }
    if (!skipLevel) out.push(line);
  }
  return out.join("\n");
}

const gameName = (pkg: Package) => /^name\s*=\s*"([^"]+)"/m.exec(pkg.text("dreamsy.toml") ?? "")?.[1] ?? "Untitled game";

async function cover(pkg: Package): Promise<Uint8Array | undefined> {
  const pt = await engine();
  if (!pt.browser) return undefined;
  const ready = await import("../engine/browser.js").then((m) => m.chromiumReady()).catch(() => false);
  if (!ready) return undefined;
  try {
    const r = await pt.browser(pkg, [{ t: 0.4, tap: [360, 900] }], [3]);
    return r.shots[0]?.jpeg;
  } catch {
    return undefined;
  }
}

async function cloudSave(dir: string, pkg: Package, label: string): Promise<string> {
  const cfg = opts.cloud(dir);
  if (!cfg) return "Cloud save is off (DREAMSY_CLOUD=0).";
  try {
    const saved = await saveToCloud(cfg, { gameId: opts.links.get(dir), name: gameName(pkg), pack: pkg.toPack(), label, thumbnail: await cover(pkg) });
    opts.links.set(dir, saved.gameId);
    return `Saved to the Dreamsy cloud as version ${saved.version}.`;
  } catch (e) {
    return `Cloud save failed (${(e as Error).message}); the local copy is fine.`;
  }
}

server.registerTool("publish_game", {
  title: "Publish to the Dreamsy library",
  description: "Save the game to the Dreamsy cloud and publish it in the public library so anyone can find, play, download and remix it. Ask the user first, and ask what creator name (and optional link) to show. The email is optional and stays private.",
  inputSchema: {
    game: z.string(),
    author: z.string().max(40).optional().describe("Creator name shown publicly."),
    link: z.string().max(200).optional().describe("Optional public link (http/https), e.g. the creator's site."),
    email: z.string().max(200).optional().describe("Optional, private: never shown publicly."),
    remix: z.boolean().optional().describe("Let others remix it (default true)."),
  },
}, guarded(async ({ game, author, link, email, remix }) => {
  const { dir, pkg } = await load(game);
  assertEditable(dir);
  const cfg = opts.cloud(dir);
  if (!cfg) return text("Cloud is off (DREAMSY_CLOUD=0); nothing published.", true);
  const gate = await finalGate(pkg, await engine());
  if (!gate.ok) return text(`Run check_game and fix these first:\n${gate.errors.join("\n")}`, true);
  const saved = await saveToCloud(cfg, { gameId: opts.links.get(dir), name: gameName(pkg), pack: pkg.toPack(), label: "publish", thumbnail: await cover(pkg) });
  opts.links.set(dir, saved.gameId);
  const pub = await publishToCloud(cfg, saved.gameId, { author, link, email, remix });
  opts.remote?.markPublished(dir, pub.slug);
  return text(`Published "${gameName(pkg)}" in the Dreamsy library${author ? ` by ${author}` : ""}.\nPlay and share it: ${playUrl(pub.slug)}\nGive the user this link. The published game is final: to change it, remix_game makes an editable copy to publish as a new game.`);
}));

if (opts.remote) {
  const hosted = opts.remote;
  server.registerTool("remix_game", {
    title: "Remix a game",
    description: "Make an editable copy of a game (published games are final, so this is how to change one). Returns the new game's id; publish it as a new game when it's done.",
    inputSchema: { game: z.string(), name: z.string().min(1).optional().describe("Name for the remix; default \"<name> remix\".") },
    annotations: EDIT,
  }, guarded(async ({ game, name }) => {
    const { pkg } = await load(game);
    const newName = name ?? `${gameName(pkg)} remix`;
    const id = hosted.newGameId(newName);
    const ws = new Workspace(pkg, disabledImages, async () => {}, new AbortController().signal, []);
    const applied = ws.applyOps([{ op: "set_manifest", key: "name", value: newName }], "remix");
    await (applied.applied ? ws.pkg : pkg).toDir(gameDir(id));
    return text(`Remixed into a new game "${newName}". The game id is "${id}": pass exactly this as \`game\` to every other tool.`);
  }));
}

server.registerPrompt("build_game", {
  title: "Build a game",
  description: "Start building a Dreamsy game from an idea.",
  argsSchema: { idea: z.string().describe("What the game is about."), game: z.string().optional().describe("Folder name.") },
}, ({ idea, game }) => ({
  messages: [{ role: "user", content: { type: "text", text: `Build this game with the Dreamsy tools: ${idea}\nFolder: ${game ?? "pick a short kebab-case name"}.\n${INSTRUCTIONS}\nPlaytest often and look at the screenshots; finish when check_game passes, then export_game.` } }],
}));

return server;
}
