import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
import sharp from "sharp";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = readFileSync(join(root, "components/Logo.tsx"), "utf8");

const grid = (name) => {
  const block = src.slice(src.indexOf(name));
  return [...block.slice(0, block.indexOf("];") + 2).matchAll(/"([.#]*)"/g)].map((m) => m[1]);
};
const glyphs = {};
for (const m of src.matchAll(/^\s+(\w): \[(.*)\],$/gm)) glyphs[m[1]] = [...m[2].matchAll(/"([.#]*)"/g)].map((x) => x[1]);
const spark = grid("const SPARK");

function wordmark(color) {
  const rects = [];
  const add = (row, x0, y) => [...row].forEach((c, i) => c === "#" && rects.push(`<rect x="${x0 + i}" y="${y}" width="1" height="1"/>`));
  let x = 0;
  for (const ch of "dreamsy") {
    const g = glyphs[ch];
    g.forEach((row, y) => add(row, x, y));
    x += Math.max(...g.map((r) => r.length)) + 1;
  }
  spark.forEach((row, y) => add(row, x + 1, y));
  const w = x + 1 + spark[0].length;
  return { svg: `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${w} 13" shape-rendering="crispEdges" fill="${color}">${rects.join("")}</svg>`, w };
}

function mark(size, pad, bg, fg) {
  const d = glyphs.d;
  const cell = Math.floor((size - pad * 2) / 13);
  const ox = Math.round((size - 6 * cell) / 2), oy = Math.round((size - 11 * cell) / 2);
  const rects = [];
  d.forEach((row, y) => [...row].forEach((c, x) => c === "#" && rects.push(`<rect x="${ox + x * cell}" y="${oy + y * cell}" width="${cell}" height="${cell}"/>`)));
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 ${size} ${size}" shape-rendering="crispEdges"><rect width="${size}" height="${size}" fill="${bg}"/><g fill="${fg}">${rects.join("")}</g></svg>`;
}

const shot = (file) => `data:image/jpeg;base64,${readFileSync(join(root, "scripts/brand-shots", file)).toString("base64")}`;
const logo = wordmark("#1a7ff0");

const SPARK = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 5 5" shape-rendering="crispEdges" fill="#fff"><rect x="2" width="1" height="5"/><rect y="2" width="5" height="1"/></svg>`;
const svg64 = (s) => `data:image/svg+xml;base64,${Buffer.from(s).toString("base64")}`;
const SPARK_BLUE = SPARK.replace('fill="#fff"', 'fill="#1a7ff0"');
const CLOUD_ROWS = ["....####....", "..##oooo##..", ".#oooooooo#.", "#oooooooooo#", "############"];
const CLOUD = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 12 5" shape-rendering="crispEdges">${CLOUD_ROWS.flatMap((r, y) => [...r].map((c, x) => (c === "#" ? `<rect x="${x}" y="${y}" width="1" height="1" fill="#0f5ccc"/>` : c === "o" ? `<rect x="${x}" y="${y}" width="1" height="1" fill="#fff"/>` : ""))).join("")}</svg>`;
const cloud = (style) => `<img class="cloud" style="${style}" src="${svg64(CLOUD)}">`;

const og = `<!doctype html><html><head>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Pixelify+Sans:wght@600;700&family=Nunito:wght@600;800&display=block">
<style>
*{box-sizing:border-box;margin:0}
body{width:1200px;height:630px;overflow:hidden;font-family:Nunito;color:#0f5ccc;position:relative;
background:#f8fcff;background-image:linear-gradient(#dcedfb 1px,transparent 1px),linear-gradient(90deg,#dcedfb 1px,transparent 1px);background-size:24px 24px}
.block{position:absolute;left:640px;top:0;width:560px;height:630px;background:#1a7ff0}
.block::before{content:"";position:absolute;left:-24px;top:0;width:24px;height:630px;
background:repeating-conic-gradient(#1a7ff0 0 25%,transparent 0 50%) 0 0/12px 12px;opacity:.9}

.dots{position:absolute;inset:0;background-image:radial-gradient(rgba(255,255,255,.18) 2px,transparent 2.5px);background-size:24px 24px}
.ground{position:absolute;left:640px;right:0;bottom:0;height:86px;background:
repeating-conic-gradient(rgba(255,255,255,.22) 0 25%,transparent 0 50%) 0 0/12px 12px,#0f5ccc}
.copy{position:absolute;left:72px;top:84px;width:540px}
.logo{height:44px;display:block;margin-bottom:36px}
h1{font:700 64px/1.08 "Pixelify Sans";color:#0f5ccc;letter-spacing:-1px}
.hl{display:inline-block;background:#1a7ff0;color:#fff;padding:0 12px 2px;box-shadow:6px 6px 0 #9fd1f6;margin-top:6px}
.sub{margin-top:28px;font:600 23px/1.4 Nunito;color:#4a6488;white-space:nowrap}
.chips{display:flex;gap:10px;margin-top:26px;flex-wrap:wrap}
.chip{font:600 17px "Pixelify Sans";color:#0f5ccc;background:#fff;border:2px solid #1a7ff0;padding:5px 12px;box-shadow:3px 3px 0 #9fd1f6}
.cmd{display:inline-flex;margin-top:28px;align-items:stretch;border:2px solid #1a7ff0;background:#fff;box-shadow:5px 5px 0 #9fd1f6}
.cmd b{font:600 16px "Pixelify Sans";background:#1a7ff0;color:#fff;padding:9px 12px}
.cmd code{font:600 18px ui-monospace,Menlo,monospace;color:#0f5ccc;padding:9px 14px}
.frame{position:absolute;background:#fff;border:4px solid #fff;outline:3px solid #0f5ccc;box-shadow:12px 12px 0 rgba(10,40,120,.35)}
.frame img{display:block;width:100%;height:100%;object-fit:cover;object-position:center}
.phone{border-radius:26px;padding:12px;background:#fff}
.phone img{border-radius:12px;border:3px solid #0f5ccc}
.f1{left:700px;top:150px;width:330px;height:186px;transform:rotate(-7deg);padding:6px}
.f2{left:930px;top:60px;width:210px;height:376px;transform:rotate(5deg)}
.f3{left:780px;top:300px;width:300px;height:169px;transform:rotate(3deg);padding:6px}
.tag{position:absolute;font:700 15px "Pixelify Sans";background:#fff;color:#0f5ccc;border:2px solid #0f5ccc;padding:2px 10px;box-shadow:3px 3px 0 rgba(10,40,120,.35)}
.t1{left:706px;top:122px;transform:rotate(-7deg)}
.t2{left:1010px;top:448px;transform:rotate(5deg)}
.t3{left:790px;top:484px;transform:rotate(3deg)}
.sp{position:absolute;width:20px;height:20px;image-rendering:pixelated}
.cloud{position:absolute;image-rendering:pixelated}
</style></head><body>
<div class="block"><div class="dots"></div></div><div class="ground"></div>
${cloud("left:672px;top:34px;width:108px")}${cloud("left:1068px;top:536px;width:96px")}${cloud("left:1110px;top:470px;width:60px")}
<img class="sp" style="left:676px;top:470px" src="${svg64(SPARK)}"><img class="sp" style="left:1150px;top:30px" src="${svg64(SPARK)}">
<img class="sp" style="left:880px;top:30px;width:14px;height:14px" src="${svg64(SPARK)}">
<img class="sp" style="left:600px;top:96px;width:16px;height:16px" src="${svg64(SPARK_BLUE)}"><img class="sp" style="left:560px;top:420px;width:12px;height:12px" src="${svg64(SPARK_BLUE)}">
<div class="copy">
  <img class="logo" src="${svg64(logo.svg)}">
  <h1>Describe a game.<br><span class="hl">Then play it.</span></h1>
  <p class="sub">A tiny engine + AI builder for 2D &amp; 2.5D games.</p>
  <div class="chips"><span class="chip">iOS</span><span class="chip">Android</span><span class="chip">Web</span><span class="chip">21 genre kits</span></div>
  <div class="cmd"><b>MCP</b><code>npx dreamsy-mcp</code></div>
</div>
<div class="frame f1"><img src="${shot("summer-ride.jpg")}"></div>
<div class="frame f3"><img src="${shot("street-racer.jpg")}"></div>
<div class="frame phone f2"><img src="${shot("kit-farming.jpg")}"></div>
<span class="tag t1">summer ride</span><span class="tag t2">mossbell farm</span><span class="tag t3">city rush</span>
</body></html>`;

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1200, height: 630 } });
await page.setContent(og, { waitUntil: "networkidle" });
await page.evaluate(() => document.fonts.ready);
const ogPng = await sharp(await page.screenshot({ type: "png" }))
  .png({ palette: true, colours: 256, effort: 10, dither: 0.35 })
  .toBuffer();
writeFileSync(join(root, "app/opengraph-image.png"), ogPng);
writeFileSync(join(root, "app/twitter-image.png"), ogPng);
writeFileSync(join(root, "app/opengraph-image.alt.txt"), "Dreamsy: describe a game, then play it. A tiny engine and AI builder for 2D and 2.5D games, with real game screenshots.");
writeFileSync(join(root, "app/twitter-image.alt.txt"), "Dreamsy: describe a game, then play it. A tiny engine and AI builder for 2D and 2.5D games, with real game screenshots.");

const icons = [
  ["public/icons/icon-192.png", mark(192, 30, "#1a7ff0", "#ffffff")],
  ["public/icons/icon-512.png", mark(512, 80, "#1a7ff0", "#ffffff")],
  ["public/icons/maskable-512.png", mark(512, 140, "#1a7ff0", "#ffffff")],
  ["app/apple-icon.png", mark(180, 34, "#1a7ff0", "#ffffff")],
];
for (const [file, svg] of icons) {
  const size = Number(/width="(\d+)"/.exec(svg)[1]);
  await page.setViewportSize({ width: size, height: size });
  await page.setContent(`<body style="margin:0">${svg}</body>`);
  writeFileSync(join(root, file), await page.screenshot({ type: "png", clip: { x: 0, y: 0, width: size, height: size } }));
}
writeFileSync(join(root, "app/icon.svg"), mark(64, 10, "#1a7ff0", "#ffffff"));
await browser.close();
console.log("wrote og image, twitter image, icons");
