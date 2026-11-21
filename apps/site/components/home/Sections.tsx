import Link from "next/link";
import { GAMES, GITHUB, KITS, MCP_COMMAND } from "@/lib/content";
import { FROG, HEART, SHIP, STAR, STRAWBERRY, CAT, COIN } from "@/lib/pixel/sprites";
import { CopyCommand } from "../CopyCommand";
import { PixelArt } from "../PixelArt";
import { Window } from "../Window";
import { GameCard } from "../GameCard";

const TICKER = ["C++20 engine", "Native on iOS & Android", "Luau scripting", "Box2D physics", "Plays in the browser", "MCP server", "Built-in editor", "Headless playtests", "One-file games", "2D + 2.5D", "21 game kits"];

export function Ticker() {
  return (
    <div className="ticker" aria-label={TICKER.join(", ")}>
      <div className="ticker-track">
        {[0, 1].map((k) => (
          <span key={k} aria-hidden={k === 1}>
            {TICKER.map((t) => (
              <span key={t} className="tick">
                {t}
                <i>✦</i>
              </span>
            ))}
          </span>
        ))}
      </div>
    </div>
  );
}

function Heading({ kicker, title, children }: { kicker: string; title: string; children?: React.ReactNode }) {
  return (
    <div className="heading">
      <span className="kicker">{kicker}</span>
      <h2>{title}</h2>
      {children && <p>{children}</p>}
    </div>
  );
}

export function HowItWorks() {
  return (
    <section className="section" id="how">
      <Heading kicker="how it works" title="From a sentence to a playable game.">
        Describe the game. Dreamsy picks a kit, builds the scenes and scripts, and playtests the result before handing it
        to you.
      </Heading>
      <div className="steps-grid">
        <Window title="1 · describe.txt" tone="sky">
          <PixelArt sprite={FROG[0]!} className="step-art" />
          <h3>Describe it</h3>
          <p>Write what you want in plain language.</p>
          <blockquote>“a frog that hops between clouds collecting strawberries”</blockquote>
        </Window>
        <Window title="2 · build.log" tone="mist">
          <PixelArt sprite={STAR} className="step-art" />
          <h3>Watch it build</h3>
          <p>It picks a kit, writes scenes and scripts, draws sprites, then runs a playtest.</p>
          <ul className="log">
            <li><span>route</span><span>new game</span></li>
            <li><span>kit</span><span>platformer</span></li>
            <li><span>apply_ops</span><span>18 ops</span></li>
            <li><span>playtest</span><span>0 errors</span></li>
          </ul>
        </Window>
        <Window title="3 · play.dpk" tone="ice">
          <PixelArt sprite={HEART} className="step-art" ink="var(--ink)" />
          <h3>Play and share</h3>
          <p>The game is one small file that runs natively on iOS and Android, and in any browser.</p>
          <p className="aside">Ask for changes the same way: “make the clouds bouncier”.</p>
        </Window>
      </div>
    </section>
  );
}

const FEATURES = [
  { title: "Native on mobile", body: "Runs natively on iOS and Android, and in the browser from the same game file.", art: COIN[0]!, tone: "ice" },
  { title: "Luau scripting", body: "Game logic is written in sandboxed Luau: readable and safe to run.", art: STRAWBERRY, tone: "sky" },
  { title: "Physics", body: "Box2D handles collisions, gravity and movement.", art: SHIP, tone: "mist" },
  { title: "2D and 2.5D", body: "Side, top-down, oblique and isometric views in one format.", art: CAT, tone: "ice" },
  { title: "Automatic playtests", body: "Every build runs headless first, so errors are caught before you see them.", art: STAR, tone: "sky" },
  { title: "One-file games", body: "Each game is a single .dpk package you can download, share or host anywhere.", art: HEART, tone: "mist" },
] as const;

export function Engine() {
  return (
    <section className="section" id="engine">
      <Heading kicker="the engine" title="A small engine built for the web.">
        Native C++ on iOS and Android, WebAssembly in the browser. Games load quickly and feel at home on phones.
      </Heading>
      <div className="feature-grid">
        {FEATURES.map((f) => (
          <article key={f.title} className={`feature feature-${f.tone}`}>
            <PixelArt sprite={f.art} className="feature-art" />
            <h3>{f.title}</h3>
            <p>{f.body}</p>
          </article>
        ))}
      </div>
    </section>
  );
}

export function Kits() {
  const tones = ["sky", "mist", "ice"];
  return (
    <section className="section" id="kits">
      <Heading kicker="21 kits" title="Start from a kit">
        Kits are tested libraries for common genres. Every game builds on one, so the basics already work.
      </Heading>
      <ul className="kit-grid">
        {KITS.map((k, i) => (
          <li key={k.id}>
            <Link className={`kit kit-${k.group === "foundation" ? "base" : tones[i % 3]}`} href={`/kits/${k.id}`}>
              <strong>{k.id}</strong>
              <span>{k.blurb}</span>
            </Link>
          </li>
        ))}
      </ul>
    </section>
  );
}

export function LibraryTeaser() {
  return (
    <section className="section" id="library">
      <Heading kicker="library" title="Made with Dreamsy">
        Example games running on the engine.
      </Heading>
      <div className="game-grid">
        {GAMES.filter((g) => g.featured).map((g) => (
          <GameCard key={g.id} game={g} />
        ))}
      </div>
      <div className="center">
        <Link className="btn btn-light" href="/library">See all {GAMES.length} games</Link>
      </div>
    </section>
  );
}

export function EditorTeaser() {
  return (
    <section className="section" id="editor">
      <Heading kicker="editor" title="Build and tweak in one place">
        Describe the game, watch it build, play it, then ask for changes. Files and settings sit right next to the preview.
      </Heading>
      <div className="byok-grid">
        <Window title="editor" tone="ice">
          <h3>The Dreamsy editor</h3>
          <p>A prompt on the left, your game in the middle, files and settings on the right.</p>
          <Link className="btn btn-sm" href="/editor" prefetch={false}>Open the editor</Link>
        </Window>
        <Window title="mcp.json" tone="mist">
          <h3>Or use Claude Code</h3>
          <p>Add Dreamsy as an MCP server and your assistant builds games with the engine.</p>
          <CopyCommand command={MCP_COMMAND} />
        </Window>
      </div>
    </section>
  );
}

export function FinalCta() {
  return (
    <section className="cta-band">
      <PixelArt sprite={STRAWBERRY} className="cta-art cta-art-l" ink="var(--paper)" paper="var(--ink)" />
      <h2>Build your first game</h2>
      <div className="ctas center-row">
        <Link className="btn" href="/docs">Get started</Link>
        <a className="btn btn-light" href={GITHUB} target="_blank" rel="noreferrer">View on GitHub</a>
      </div>
      <PixelArt sprite={STAR} className="cta-art cta-art-r" ink="var(--paper)" />
    </section>
  );
}
