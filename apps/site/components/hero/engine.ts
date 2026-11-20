import { bootScreen, buildScreen, NO_PAD, SCENES, SCREEN_H, SCREEN_W } from "@/lib/pixel/scenes";
import { HEART, SPARKLE, STAR, STRAWBERRY, stamp, type Sprite } from "@/lib/pixel/sprites";
import { CLEAR, dissolve, INK, PAPER, shade, Surface, type RGB } from "@/lib/pixel/surface";
import { buildShell, DEVICE_H, DEVICE_W, drawControls, SCREEN_INK, SCREEN_PAPER, SCREEN_X, SCREEN_Y } from "./handheld";

const BLUE: RGB = [26, 127, 240];
const DEEP: RGB = [15, 92, 204];
const WHITE: RGB = [255, 255, 255];
const SKY: RGB = [159, 209, 246];
const MIST: RGB = [232, 244, 254];
const ICE: RGB = [196, 230, 252];
const LINE: RGB = [204, 229, 250];

const MENU: [RGB, RGB] = [[226, 244, 255], DEEP];
const SCREENS: [RGB, RGB][] = [
  [[205, 236, 255], DEEP],
  [[232, 244, 254], [15, 70, 170]],
  [[248, 252, 255], DEEP],
];

const TYPE_END = 2.8;
const BUILD_END = 5.4;
const FADE = 0.7;
const CYCLE = 15;

const SKY_LINE = 3;
const SKY_ACCENT = 4;
const SKY_LIGHT = 5;

export interface HeroEls {
  hero: HTMLElement;
  sky: HTMLCanvasElement;
  device: HTMLCanvasElement;
  prompt: HTMLElement;
  status: HTMLElement;
  steps: HTMLElement;
}

interface Cloud { x: number; y: number; w: number; v: number; }
interface Floater { sp: Sprite; color: number; x: number; y: number; v: number; phase: number; }
interface Spark { x: number; y: number; vx: number; vy: number; life: number; }

function drawCloud(s: Surface, cx: number, cy: number, w: number) {
  const discs: [number, number, number][] = [
    [cx - w * 0.3, cy + w * 0.06, w * 0.2],
    [cx, cy - w * 0.05, w * 0.28],
    [cx + w * 0.3, cy + w * 0.08, w * 0.17],
  ];
  const bottom = Math.round(cy + w * 0.2);
  const left = Math.round(cx - w * 0.5), right = Math.round(cx + w * 0.47);
  const inside = (x: number, y: number) =>
    y <= bottom && (discs.some(([dx, dy, r]) => (x + 0.5 - dx) ** 2 + (y + 0.5 - dy) ** 2 <= r * r) || (y >= bottom - 2 && x >= left && x <= right));
  for (let y = Math.floor(cy - w * 0.4); y <= bottom; y++)
    for (let x = left - 1; x <= right + 1; x++) {
      if (!inside(x, y)) continue;
      const edge = !inside(x, y - 1) || !inside(x - 1, y) || !inside(x + 1, y) || y === bottom;
      s.set(x, y, edge ? SKY_LINE : PAPER);
    }
}

function recolor(s: Surface, sp: Sprite, x: number, y: number, color: number) {
  stamp(s, sp, x, y);
  if (color === INK) return;
  for (let j = 0; j < sp.h; j++)
    for (let i = 0; i < sp.w; i++) {
      const px = Math.round(x) + i, py = Math.round(y) + j;
      if (s.get(px, py) === INK && sp.rows[j]?.[i] === "#") s.set(px, py, color);
    }
}

export function startHero(els: HeroEls) {
  const motion = window.matchMedia("(prefers-reduced-motion: reduce)");
  const shell = buildShell();
  const frame = new Surface(DEVICE_W, DEVICE_H);
  const screen = new Surface(SCREEN_W, SCREEN_H);
  const a = new Surface(SCREEN_W, SCREEN_H);
  const b = new Surface(SCREEN_W, SCREEN_H);
  const lastPlay = new Surface(SCREEN_W, SCREEN_H);
  let lastColors = MENU;
  let hasLast = false;
  const dctx = els.device.getContext("2d")!;
  els.device.width = DEVICE_W;
  els.device.height = DEVICE_H;

  let sky = new Surface(1, 1, CLEAR);
  const sctx = els.sky.getContext("2d")!;
  let scale = 5;
  let clouds: Cloud[] = [];
  let floaters: Floater[] = [];
  const sparks: Spark[] = [];

  const resizeSky = () => {
    const r = els.hero.getBoundingClientRect();
    scale = r.width < 720 ? 4 : 5;
    const w = Math.ceil(r.width / scale), h = Math.ceil(r.height / scale);
    if (w === sky.w && h === sky.h) return;
    sky = new Surface(w, h, CLEAR);
    els.sky.width = w;
    els.sky.height = h;
    clouds = [0.12, 0.62].map((f, i) => ({ x: w * (0.15 + i * 0.4), y: h * f, w: 24 + i * 6, v: 0.7 + i * 0.4 }));
    const kinds: [Sprite, number][] = [[STRAWBERRY, SKY_ACCENT], [STAR, SKY_LIGHT], [HEART, SKY_ACCENT], [SPARKLE[0]!, INK], [STAR, INK]];
    floaters = Array.from({ length: Math.max(5, Math.round((w * h) / 9000)) }, (_, i) => ({
      sp: kinds[i % kinds.length]![0],
      color: kinds[i % kinds.length]![1],
      x: w * (0.45 + ((i * 0.37) % 0.55)),
      y: h * ((i * 0.61) % 1),
      v: 2.5 + (i % 4) * 1.2,
      phase: i * 1.3,
    }));
  };
  const onDown = (e: PointerEvent) => {
    if (motion.matches || (e.target as HTMLElement).closest("a,button,canvas.device")) return;
    const r = els.hero.getBoundingClientRect();
    const x = (e.clientX - r.left) / scale, y = (e.clientY - r.top) / scale;
    for (let k = 0; k < 8; k++) {
      const ang = (k / 8) * Math.PI * 2;
      sparks.push({ x, y, vx: Math.cos(ang) * 16, vy: Math.sin(ang) * 16, life: 0.6 });
    }
  };
  els.hero.addEventListener("pointerdown", onDown);

  let idx = 0;
  let elapsed = 0;
  let cycleStart = 0.8;
  let built = false;
  let label = "";
  let stepsFor = -1;
  let stepsShown = -1;

  const setStatus = (s: string) => {
    if (s === label) return;
    label = s;
    els.status.textContent = s;
    els.status.dataset.state = s;
  };

  const setupSteps = () => {
    const sc = SCENES[idx % SCENES.length]!;
    els.steps.replaceChildren(
      ...sc.log.map(([k, v]) => {
        const li = document.createElement("li");
        const kk = document.createElement("span");
        kk.textContent = k;
        const vv = document.createElement("span");
        vv.textContent = v;
        li.append(kk, vv);
        return li;
      }),
    );
    stepsFor = idx;
    stepsShown = -1;
  };

  const markSteps = (shown: number) => {
    if (shown === stepsShown) return;
    stepsShown = shown;
    [...els.steps.children].forEach((li, i) => li.setAttribute("data-s", i < shown ? "done" : i === shown ? "run" : ""));
  };

  const next = () => {
    if (!motion.matches && elapsed - cycleStart > BUILD_END) {
      lastPlay.px.set(screen.px);
      lastColors = SCREENS[idx % SCREENS.length]!;
      hasLast = true;
    }
    idx++;
    built = false;
    if (motion.matches) showStatic();
    else cycleStart = elapsed;
  };
  els.device.addEventListener("click", next);

  const drawDevice = (t: number, dt: number) => {
    const sc = SCENES[idx % SCENES.length]!;
    if (stepsFor !== idx) setupSteps();
    const typed = sc.prompt.slice(0, Math.min(sc.prompt.length, Math.floor((sc.prompt.length * t) / (TYPE_END - 0.4))));
    if (els.prompt.textContent !== typed) els.prompt.textContent = typed;

    let pad = NO_PAD;
    let colors = MENU;
    if (t < TYPE_END) {
      setStatus("listening");
      markSteps(-1);
      bootScreen(a, t);
      if (hasLast && t < FADE) {
        dissolve(screen, lastPlay, a, t / FADE);
        colors = t / FADE > 0.5 ? MENU : lastColors;
      } else screen.px.set(a.px);
    } else if (t < BUILD_END) {
      setStatus("building");
      if (!built) { sc.reset(); built = true; }
      const p = (t - TYPE_END) / (BUILD_END - TYPE_END - 0.3);
      buildScreen(screen, sc, p, t);
      markSteps(Math.min(sc.log.length, Math.floor(Math.min(1, p) * (sc.log.length + 0.5))));
    } else {
      setStatus("playing");
      markSteps(sc.log.length);
      if (!built) { sc.reset(); built = true; }
      sc.update(dt);
      pad = sc.pad();
      if (t < BUILD_END + FADE) {
        buildScreen(a, sc, 1, t);
        sc.draw(b);
        const p = (t - BUILD_END) / FADE;
        dissolve(screen, a, b, p);
        if (p > 0.5) colors = SCREENS[idx % SCREENS.length]!;
      } else {
        sc.draw(screen);
        colors = SCREENS[idx % SCREENS.length]!;
      }
    }

    frame.px.set(shell.px);
    frame.draw(screen, SCREEN_X, SCREEN_Y, { [PAPER]: SCREEN_PAPER, [INK]: SCREEN_INK });
    drawControls(frame, pad);
    frame.blit(dctx, [WHITE, BLUE, MIST, colors[0], colors[1], SKY]);
  };

  const measureBackdrop = () => {
    const hr = els.hero.getBoundingClientRect();
    const dr = els.device.getBoundingClientRect();
    return {
      x0: Math.round((dr.left - hr.left - dr.width * 0.3) / scale),
      x1: Math.round((dr.right - hr.left + dr.width * 0.18) / scale),
      y0: Math.round((dr.top - hr.top + dr.height * 0.12) / scale),
      y1: Math.round(Math.min(hr.height - 2, dr.bottom - hr.top + dr.height * 0.08) / scale),
    };
  };

  resizeSky();
  let backdrop = measureBackdrop();

  const drawSky = (now: number, dt: number) => {
    const s = sky;
    s.clear(CLEAR);
    for (const c of clouds) {
      c.x += c.v * dt;
      if (c.x - c.w > s.w) c.x = -c.w;
      drawCloud(s, Math.round(c.x), Math.round(c.y), c.w);
    }
    const { x0, x1, y0, y1 } = backdrop;
    for (let y = y0 + 5; y < y1 + 5; y++) for (let x = x0 + 5; x < x1 + 5; x++) if (shade(x, y, 6)) s.set(x, y, INK);
    s.rect(x0, y0, x1 - x0, y1 - y0, INK);
    for (let x = x0; x < x1; x++) {
      const wy = y1 - 14 + Math.sin(x * 0.09 + now * 1.3) * 2 + Math.sin(x * 0.03 - now * 0.6) * 2;
      for (let y = Math.max(y0, Math.floor(wy)); y < y1; y++)
        if (y - wy < 1 || shade(x, y, 2 + Math.floor((y - wy) / 5))) s.set(x, y, PAPER);
    }
    for (let y = y0 + 6; y < y1 - 22; y += 3) for (let x = x1 - 10; x < x1 - 4; x += 3) s.set(x, y, PAPER);
    const mx = x1 - 6, my = y0 + 2;
    for (let y = my - 12; y <= my + 12; y++)
      for (let x = mx - 12; x <= mx + 12; x++) {
        const d = (x + 0.5 - mx) ** 2 + (y + 0.5 - my) ** 2;
        if (d <= 144 && (x + 0.5 - mx - 5) ** 2 + (y + 0.5 - my + 4) ** 2 > 110) s.set(x, y, d > 120 ? INK : SKY_LIGHT);
      }
    for (const f of floaters) {
      f.y -= f.v * dt;
      if (f.y < -12) f.y = s.h + 8;
      recolor(s, f.sp, f.x + Math.sin(now * 0.8 + f.phase) * 3, f.y, f.color);
    }
    for (let i = sparks.length - 1; i >= 0; i--) {
      const p = sparks[i]!;
      p.life -= dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      p.vx *= 0.92;
      p.vy *= 0.92;
      if (p.life <= 0) { sparks.splice(i, 1); continue; }
      const sp = SPARKLE[p.life > 0.35 ? 0 : p.life > 0.15 ? 1 : 2]!;
      stamp(s, sp, p.x - (sp.w >> 1), p.y - (sp.h >> 1));
    }
    s.blit(sctx, [WHITE, BLUE, MIST, LINE, SKY, ICE]);
  };

  let visible = false;
  let last = 0;
  let lastSky = -Infinity;
  let raf = 0;

  const tick = (now: number) => {
    raf = requestAnimationFrame(tick);
    const step = (now - last) / 1000;
    if (step < 1 / 30 - 0.001) return;
    last = now;
    const dt = Math.min(0.1, step);
    elapsed += dt;
    let t = elapsed - cycleStart;
    if (t >= CYCLE) {
      lastPlay.px.set(screen.px);
      lastColors = SCREENS[idx % SCREENS.length]!;
      hasLast = true;
      idx++;
      built = false;
      cycleStart = elapsed;
      t = 0;
    }
    drawDevice(Math.max(0, t), dt);
    if (elapsed - lastSky >= 1 / 15 - 0.002) {
      drawSky(elapsed, Math.min(0.15, elapsed - lastSky));
      lastSky = elapsed;
    }
  };

  function showStatic() {
    const sc = SCENES[idx % SCENES.length]!;
    sc.reset();
    built = true;
    for (let i = 0; i < 90; i++) sc.update(1 / 30);
    drawDevice(BUILD_END + FADE + 1, 0);
    drawSky(elapsed, 0);
  }

  const sync = () => {
    const running = visible && !document.hidden && !motion.matches;
    els.hero.toggleAttribute("data-paused", !running);
    if (running && !raf) {
      last = performance.now();
      raf = requestAnimationFrame(tick);
    } else if (!running && raf) {
      cancelAnimationFrame(raf);
      raf = 0;
    }
  };
  const onMotionChange = () => {
    hasLast = false;
    built = false;
    cycleStart = elapsed;
    sparks.length = 0;
    if (motion.matches && visible && !document.hidden) showStatic();
    sync();
  };
  const visibility = new IntersectionObserver(([entry]) => {
    visible = Boolean(entry?.isIntersecting);
    if (visible && motion.matches && !document.hidden) showStatic();
    sync();
  });
  const resize = new ResizeObserver(() => {
    resizeSky();
    backdrop = measureBackdrop();
    if (visible && motion.matches && !document.hidden) drawSky(elapsed, 0);
    lastSky = -Infinity;
  });
  visibility.observe(els.hero);
  resize.observe(els.hero);
  resize.observe(els.device);
  motion.addEventListener("change", onMotionChange);
  const onVisibilityChange = () => {
    if (visible && motion.matches && !document.hidden) showStatic();
    sync();
  };
  document.addEventListener("visibilitychange", onVisibilityChange);

  return () => {
    cancelAnimationFrame(raf);
    resize.disconnect();
    visibility.disconnect();
    motion.removeEventListener("change", onMotionChange);
    document.removeEventListener("visibilitychange", onVisibilityChange);
    els.hero.removeEventListener("pointerdown", onDown);
    els.device.removeEventListener("click", next);
  };
}
