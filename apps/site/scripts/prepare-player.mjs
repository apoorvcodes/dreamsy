import { createHash } from "node:crypto";
import { readFile, readdir, writeFile } from "node:fs/promises";
import { join } from "node:path";

const dir = process.argv[2];
if (!dir) throw new Error("Usage: node prepare-player.mjs <web player directory>");

const files = ["dreamsy-player.html", "dreamsy-player.js", "dreamsy-player.wasm"];
if ((await readdir(dir)).includes("dreamsy-player.data")) files.push("dreamsy-player.data");
for (const kit of (await readdir(join(dir, "kits"))).sort()) {
  if (/^[a-z][a-z0-9-]*\.dpk$/.test(kit)) files.push(`kits/${kit}`);
}

const version = createHash("sha256");
for (const file of files) {
  let bytes = await readFile(join(dir, file));
  if (file.endsWith(".html")) {
    const html = bytes.toString("utf8");
    if (!/<\/title>/i.test(html)) throw new Error("The player shell is missing its title");
    // Preload in the iframe's document so Emscripten can reuse the fetch.
    if (!/<link\b[^>]*\brel="preload"[^>]*\bhref="(?:\.\/)?dreamsy-player\.wasm"/i.test(html)) {
      bytes = Buffer.from(html.replace(/<\/title>/i,
        '</title><link rel="preload" as="fetch" href="./dreamsy-player.wasm" type="application/wasm" crossorigin>'));
    }
    await writeFile(join(dir, "dreamsy-player.preload.html"), bytes);
  }
  version.update(file).update("\0").update(createHash("sha256").update(bytes).digest());
}

const hash = version.digest("hex").slice(0, 24);
await writeFile(join(dir, ".version"), hash);
console.log(`Prepared player ${hash} (${files.length} assets)`);
