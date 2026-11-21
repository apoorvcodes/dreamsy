#!/usr/bin/env node
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));

process.env.DREAMSY_ROOT ??= join(here, "..", "data");

if (!process.env.DREAMSY_TOOL) {
  const plat = `${process.platform}-${process.arch}`;
  const tool = join(here, "..", "native", plat, process.platform === "win32" ? "dreamsy-tool.exe" : "dreamsy-tool");
  if (!existsSync(tool)) {
    process.stderr.write(
      `dreamsy-mcp: no engine binary for ${plat} yet (macOS and Linux are prebuilt; on Windows use WSL).\n` +
        "Build it from source (https://github.com/apoorvcodes/dreamsy, engine/README.md) and set DREAMSY_TOOL to its path.\n",
    );
    process.exit(1);
  }
  process.env.DREAMSY_TOOL = tool;
}

if (process.env.PLAYTEST_BROWSER === undefined) {
  try {
    await import("playwright-core");
  } catch {
    process.env.PLAYTEST_BROWSER = "0";
  }
}

await import("../dist/server.mjs");
