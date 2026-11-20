import { existsSync } from "node:fs";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { Package } from "@dreamsy/format";
import { REPO_ROOT, runTool, simulate, SMOKE_TIMELINE, type TimelineStep } from "../src/engine/playtest.js";

const game = (dir: string) => Package.fromDir(join(REPO_ROOT, "games", dir));
const START: TimelineStep[] = [{ t: 0.3, tap: [360, 900] }, ...SMOKE_TIMELINE];

it("stops and reaps a child that exceeds its time or output limit", async () => {
  const original = process.env.DREAMSY_TOOL;
  process.env.DREAMSY_TOOL = process.execPath;
  try {
    await expect(runTool(["-e", "setInterval(() => {}, 1000)"], { timeoutMs: 200 })).rejects.toThrow(/timed out/);
    await expect(runTool(["-e", "process.stdout.write('x'.repeat(8192))"], { maxOutputBytes: 1024 })).rejects.toThrow(/output limit/);
  } finally {
    if (original === undefined) delete process.env.DREAMSY_TOOL;
    else process.env.DREAMSY_TOOL = original;
  }
});

describe("simulate (dreamsy-tool playtest)", () => {
  it("plays cloud-hopper: title tap, dialogs, avatar trace, sounds, scenes, controls", async () => {
    const r = await simulate(await game("showcase/cloud-hopper"), START, 8, { autoDialog: "first" });
    expect(r.loadError).toBeUndefined();
    expect(r.errors).toEqual([]);
    expect(r.scenes?.map((s) => s.scene)).toEqual(["scenes/main.scene.json", "scenes/level.scene.json"]);
    expect(r.avatarTrace).toHaveLength(8);
    expect(r.avatarTrace[1]!.x).toBeGreaterThan(r.avatarTrace[0]!.x);
    expect(r.dialogLog?.[0]).toMatch(/^Pip: /);
    expect(r.audio.music).toEqual(["sky_tune"]);
    expect(r.audio.sfx.jump).toBeGreaterThan(0);
    expect(r.controls?.read).toContain("move_x");
  });

  it("starts mid-game (scene + save) and reports script errors with lines", async () => {
    const base = await game("showcase/cloud-hopper");
    const r = await simulate(base, [], 1, { scene: "scenes/level.scene.json", save: { best: 3 } });
    expect(r.scenes?.[0]?.scene).toBe("scenes/level.scene.json");
    expect(r.save).toMatchObject({ best: 3 });

    const files: Record<string, Uint8Array | string> = {};
    for (const p of base.paths()) files[p] = base.bytes(p)!;
    files["scripts/client/boom.luau"] = "print('before')\nlocal t = nil\nprint(t.x)\n";
    const bad = await simulate(Package.fromFiles(files), [], 0.5);
    expect(bad.errors).toEqual([expect.objectContaining({ script: "scripts/client/boom.luau", line: 3, count: 1 })]);
    expect(bad.logs).toContain("[info] before");

    expect((await simulate(base, [], 1, { scene: "scenes/nope.scene.json" })).loadError).toMatch(/unknown scene/);
  });
});

const webBuilt = existsSync(join(REPO_ROOT, "engine/build/web/dreamsy-player.html"));
describe.skipIf(!webBuilt || process.env.PLAYTEST_BROWSER_TEST !== "1")("playtestBrowser (C++ web player)", () => {
  it("screenshots cloud-hopper at game times", async () => {
    const { playtestBrowser, closeBrowser } = await import("../src/engine/browser.js");
    try {
      const r = await playtestBrowser(await game("showcase/cloud-hopper"), START, [1, 3]);
      expect(r.loadError).toBeUndefined();
      expect(r.scriptErrors).toBe(0);
      expect(r.shots.map((s) => s.t)).toEqual([1, 3]);
      for (const s of r.shots) expect([s.jpeg[0], s.jpeg[1]]).toEqual([0xff, 0xd8]);
    } finally {
      await closeBrowser();
    }
  }, 120_000);
});
