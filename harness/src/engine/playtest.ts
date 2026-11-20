import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import type { Package } from "@dreamsy/format";

export type Control = "left" | "right" | "up" | "down" | "jump" | "action" | "action2";

export interface TimelineStep {
  t: number;
  hold?: Control[];
  release?: Control[];
  press?: Control[];
  tap?: [number, number];
  swipe?: { from: [number, number]; to: [number, number]; seconds?: number };
}

export interface AudioSummary { sfx: Record<string, number>; music: string[] }

export interface PlaytestReport {
  loadError?: string;
  errors: { script: string; line?: number; message: string; count: number }[];
  logs: string[];
  toasts: string[];
  entities: number;
  audio: AudioSummary;
  avatarTrace: { t: number; x: number; y: number }[];
  dialog?: { text: string; speaker?: string; options?: string[] };
  dialogLog?: string[];
  save?: Record<string, unknown>;
  scene?: string;
  scenes?: { t: number; scene: string }[];
  controls?: { read: string[]; scripts: { move: boolean; look: boolean; buttons: string[] } };
  time?: number;
}

export interface BrowserPlaytest {
  loadError?: string;
  shots: { t: number; jpeg: Uint8Array }[];
  consoleErrors: string[];
  scriptErrors: number;
}

export interface StartAt { scene?: string; sceneParams?: Record<string, unknown>; save?: Record<string, unknown> }

export interface Playtester {
  simulate(pkg: Package, timeline: TimelineStep[], seconds: number, start?: StartAt): Promise<PlaytestReport>;
  browser?(pkg: Package, timeline: TimelineStep[], shotTimes: number[], start?: StartAt): Promise<BrowserPlaytest>;
  close?(): Promise<void>;
}

export const SMOKE_TIMELINE: TimelineStep[] = [
  { t: 0.5, hold: ["right"] },
  { t: 1, press: ["jump"] },
  { t: 2, press: ["action"] },
  { t: 2.5, press: ["jump"] },
  { t: 3.5, release: ["right"], hold: ["left"] },
  { t: 4.5, press: ["jump"] },
  { t: 5.5, release: ["left"], hold: ["up"] },
  { t: 6.5, release: ["up"], hold: ["down"] },
  { t: 7.5, release: ["down"] },
];

export const PLAYTEST_EPOCH = 1767225600;

import { REPO_ROOT } from "../paths.js";
export { REPO_ROOT };

export function dreamsyTool(): string {
  const env = process.env.DREAMSY_TOOL;
  if (env) return env;
  for (const preset of ["release", "headless", "debug"]) {
    const p = join(REPO_ROOT, "engine/build", preset, "dreamsy-tool");
    if (existsSync(p)) return p;
  }
  throw new Error("dreamsy-tool not found: build it (cd engine && cmake --preset release && cmake --build --preset release --target dreamsy-tool) or set DREAMSY_TOOL");
}

export function runTool(args: string[], opts: { stdin?: string; timeoutMs?: number; maxOutputBytes?: number } = {}): Promise<{ stdout: string; stderr: string; code: number | null }> {
  return new Promise((resolve, reject) => {
    const child = spawn(dreamsyTool(), args, { stdio: ["pipe", "pipe", "pipe"] });
    const out: Buffer[] = [], err: Buffer[] = [];
    const ms = opts.timeoutMs ?? 120_000;
    let failure: Error | undefined;
    let outputBytes = 0;
    const stop = (message: string) => {
      failure ??= new Error(message);
      child.kill("SIGKILL");
    };
    const timer = setTimeout(() => stop(`dreamsy-tool ${args[0]} timed out after ${ms} ms`), ms);
    const collect = (chunks: Buffer[], b: Buffer) => {
      if (failure) return;
      outputBytes += b.length;
      if (outputBytes > (opts.maxOutputBytes ?? Infinity)) {
        stop(`dreamsy-tool ${args[0]} exceeded its output limit`);
      } else {
        chunks.push(b);
      }
    };
    child.stdout.on("data", (b: Buffer) => collect(out, b));
    child.stderr.on("data", (b: Buffer) => collect(err, b));
    child.on("error", (e) => { clearTimeout(timer); reject(e); });
    child.on("close", (code, signal) => {
      clearTimeout(timer);
      if (failure) { reject(failure); return; }
      const stdout = Buffer.concat(out).toString("utf8"), stderr = Buffer.concat(err).toString("utf8");
      if (signal) reject(new Error(`dreamsy-tool ${args[0]} crashed (${signal}): ${stderr.slice(-500)}`));
      else resolve({ stdout, stderr, code });
    });
    child.stdin.on("error", () => {});
    child.stdin.end(opts.stdin ?? "");
  });
}

export function tempPack(pkg: Package): { path: string; cleanup: () => void } {
  const dir = mkdtempSync(join(tmpdir(), "dreamsy-pt-"));
  const path = join(dir, "game.dpk");
  const cleanup = () => rmSync(dir, { recursive: true, force: true });
  try {
    writeFileSync(path, pkg.toPack());
    return { path, cleanup };
  } catch (e) {
    cleanup();
    throw e;
  }
}

export interface SimulateOptions extends StartAt {
  clock?: number;
  autoDialog?: "first" | "last" | number;
  budgetMs?: number;
  seed?: number;
  hosted?: boolean;
}

const emptyReport = (loadError: string): PlaytestReport => ({ loadError, errors: [], logs: [], toasts: [], entities: 0, avatarTrace: [], audio: { sfx: {}, music: [] } });

export async function simulate(pkg: Package, timeline: TimelineStep[], seconds: number, opts: SimulateOptions = {}): Promise<PlaytestReport> {
  let pack: { path: string; cleanup: () => void };
  try {
    pack = tempPack(pkg);
  } catch (e) {
    return emptyReport(`cannot pack the game: ${(e as Error).message}`);
  }
  try {
    const secs = Math.max(0, Math.min(3600, seconds));
    const args = ["playtest", pack.path, "--timeline", "-", "--seconds", String(secs)];
    if (opts.scene) args.push("--scene", opts.scene);
    if (opts.sceneParams) args.push("--params", JSON.stringify(opts.sceneParams));
    if (opts.save) args.push("--save", JSON.stringify(opts.save));
    if (opts.clock !== undefined) args.push("--clock", String(opts.clock));
    if (opts.autoDialog !== undefined) args.push("--auto-dialog", String(opts.autoDialog));
    if (opts.budgetMs !== undefined) args.push("--budget", String(opts.budgetMs));
    if (opts.seed !== undefined) args.push("--seed", String(opts.seed));
    const { stdout, stderr } = await runTool(args, {
      stdin: JSON.stringify(timeline),
      timeoutMs: opts.hosted ? 30_000 : 60_000 + secs * 5_000,
      maxOutputBytes: opts.hosted ? 1024 * 1024 : undefined,
    });
    try {
      return JSON.parse(stdout) as PlaytestReport;
    } catch {
      throw new Error(`dreamsy-tool playtest: ${(stderr || stdout).trim().slice(0, 500)}`);
    }
  } finally {
    pack.cleanup();
  }
}

export async function enginePlaytester(opts: { browser: boolean; hosted?: boolean }): Promise<Playtester> {
  dreamsyTool();
  const renderer = opts.browser ? await import("./browser.js").catch(() => null) : null;
  const browser = opts.hosted && renderer && !renderer.chromiumReady() ? null : renderer;
  return {
    simulate: (p, tl, secs, start) => simulate(p, tl, secs, { autoDialog: "first", ...start, hosted: opts.hosted }),
    ...(browser ? { browser: (p: Package, tl: TimelineStep[], shots: number[], start?: StartAt) => browser.playtestBrowser(p, tl, shots, { ...start, hosted: opts.hosted }) } : {}),
    close: async () => { await browser?.closeBrowser(); },
  };
}

export function describeRun(r: PlaytestReport): string {
  const lines: string[] = [];
  if (r.loadError) lines.push(`LOAD FAILED: ${r.loadError}`);
  if (r.errors.length) {
    lines.push(`${r.errors.length} script error(s):`);
    for (const e of r.errors.slice(0, 15)) lines.push(`- ${e.script}${e.line ? `:${e.line}` : ""} ${e.message}${e.count > 1 ? ` (×${e.count})` : ""}`);
  } else if (!r.loadError) lines.push("No script errors.");
  if (r.scenes && r.scenes.length > 1) lines.push(`Scenes: ${r.scenes.map((s) => `${s.scene} (${Math.round(s.t * 10) / 10}s)`).join(" → ")}`);
  lines.push(`Entities alive at end: ${r.entities}`);
  if (r.avatarTrace.length) lines.push(`Player position per second: ${r.avatarTrace.map((p) => `${p.t}s(${p.x},${p.y})`).join(" ")}`);
  if (r.controls) lines.push(`Controls the game read: ${r.controls.read.join(", ") || "none"}`);
  const sfx = Object.entries(r.audio.sfx).map(([id, n]) => `${id}×${n}`).join(", ");
  lines.push(`Sound: ${sfx || "no sfx played"}; music: ${r.audio.music.join(" → ") || "none"}`);
  if (r.dialogLog?.length) lines.push(`Dialog shown:\n${r.dialogLog.slice(-10).join("\n")}`);
  if (r.dialog) lines.push(`Dialog open at the end: ${r.dialog.speaker ? `${r.dialog.speaker}: ` : ""}${r.dialog.text}${r.dialog.options ? ` [${r.dialog.options.join(" | ")}]` : ""}`);
  if (r.toasts.length) lines.push(`Toasts on screen: ${r.toasts.join(" | ")}`);
  if (r.save && Object.keys(r.save).length) lines.push(`Save data at the end: ${JSON.stringify(r.save).slice(0, 600)}`);
  if (r.logs.length) lines.push(`Last logs:\n${r.logs.slice(-15).join("\n")}`);
  return lines.join("\n");
}
