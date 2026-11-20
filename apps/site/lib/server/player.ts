import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { PLAYER_DIR } from "./paths";

let version: Promise<string | null> | undefined;

export function playerVersion() {
  return (version ??= readFile(join(PLAYER_DIR, ".version"), "utf8").then(
    (hash) => /^[a-f0-9]{24}$/.test(hash) ? hash : null,
    (error: NodeJS.ErrnoException) => {
      if (error.code === "ENOENT") return null;
      throw error;
    },
  ));
}
