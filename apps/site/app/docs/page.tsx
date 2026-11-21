import type { Metadata } from "next";
import { Footer } from "@/components/Footer";
import { SiteNav } from "@/components/SiteNav";
import { Window } from "@/components/Window";
import { GITHUB } from "@/lib/content";

export const metadata: Metadata = {
  title: "Docs",
  description: "Build the Dreamsy engine, run a game, and make your own with an AI assistant.",
  alternates: { canonical: "/docs" },
  openGraph: { url: "/docs", title: "Docs · Dreamsy", description: "Build the Dreamsy engine, run a game, and make your own with an AI assistant." },
};

const TOC = [
  ["start", "Get started"],
  ["engine", "Build the engine"],
  ["play", "Run a game"],
  ["mcp", "Use with your AI (MCP)"],
  ["format", "The game format"],
  ["kits", "Kits"],
  ["more", "Read more"],
] as const;

function Code({ children }: { children: string }) {
  return (
    <pre className="code">
      <code>{children}</code>
    </pre>
  );
}

export default function Docs() {
  return (
    <>
      <div className="page-top">
        <SiteNav />
        <header className="page-head">
          <span className="kicker">docs</span>
          <h1>Get started</h1>
          <p>Build the engine, run a game, and make your own with an AI assistant.</p>
        </header>
      </div>
      <main className="page docs">
        <nav className="toc" aria-label="On this page">
          <span className="toc-title">on this page</span>
          {TOC.map(([id, label]) => (
            <a key={id} href={`#${id}`}>{label}</a>
          ))}
        </nav>
        <article className="doc">
          <section id="start">
            <h2>Get started</h2>
            <p>You need Node with pnpm, CMake and a C++20 compiler. Clone the repo and install the JavaScript packages.</p>
            <Code>{`git clone ${GITHUB}
cd dreamsy-mono
pnpm install`}</Code>
          </section>

          <section id="engine">
            <h2>Build the engine</h2>
            <p>The engine lives in <code>engine/</code>. A release build gives you <code>dreamsy-tool</code>, which validates, packs, runs and playtests games.</p>
            <Code>{`cd engine
cmake --preset release && cmake --build --preset release`}</Code>
            <p>For the browser player, install emsdk 4.0.11 and build the web preset:</p>
            <Code>{`cmake --preset web && cmake --build --preset web --target dreamsy-player`}</Code>
          </section>

          <section id="play">
            <h2>Run a game</h2>
            <p>Every game is a folder. Run one headless, or get a playtest report with errors, scenes and sounds.</p>
            <Code>{`./build/release/dreamsy-tool run ../games/examples/coin-dash --ticks 300
./build/release/dreamsy-tool playtest ../games/showcase/cloud-hopper --seconds 8`}</Code>
          </section>

          <section id="mcp">
            <h2>Use with your AI (MCP)</h2>
            <p>
              Dreamsy is an MCP server. Your assistant gets the engine as tools: create a game from a kit, edit it,
              draw sprites, playtest, check and publish. No Dreamsy key needed; your assistant does the thinking.
            </p>
            <h3>Hosted: ChatGPT, Claude.ai, phones</h3>
            <p>
              Add this URL as a remote MCP server (custom connector), no sign-in. Each game gets a private id;
              ask for the game to be published and you get a <code>dreamsy.dev/play</code> link that runs on
              any phone.
            </p>
            <Code>{`https://dreamsy.dev/mcp`}</Code>
            <Code>{`# Claude Code, over HTTP instead of a local process
claude mcp add --transport http dreamsy https://dreamsy.dev/mcp`}</Code>
            <h3>Local: Claude Code, Cursor, VS Code</h3>
            <p>Runs on your computer and keeps the game folders there.</p>
            <Code>{`claude mcp add dreamsy -- npx -y dreamsy-mcp`}</Code>
            <p>
              Games are saved in the folder the server starts in; add <code>-e DREAMSY_GAMES_DIR=$HOME/dreamsy-games</code> to keep
              them in one place. Needs Node 20+; prebuilt for macOS (Apple Silicon and Intel) and Linux (x64 and arm64), and
              Windows through WSL.
            </p>
            <p>Other clients take the same command in their MCP config:</p>
            <Code>{`// Claude Desktop, Cursor, Windsurf, Gemini CLI
{ "mcpServers": { "dreamsy": { "command": "npx", "args": ["-y", "dreamsy-mcp"] } } }

// VS Code (.vscode/mcp.json)
{ "servers": { "dreamsy": { "type": "stdio", "command": "npx", "args": ["-y", "dreamsy-mcp"] } } }`}</Code>
            <Window title="try.txt" tone="ice">
              <p>Then just ask: <strong>“build a cozy frog platformer with dreamsy”</strong>.</p>
            </Window>
          </section>

          <section id="format">
            <h2>The game format</h2>
            <p>A game is a folder you can read and edit by hand, packed into one <code>.dpk</code> file to share.</p>
            <Code>{`my-game/
  dreamsy.toml        name, orientation, view, kits
  scenes/*.scene.json entities and their components
  scripts/            Luau: client/ and shared/
  assets/             sprites (svg, png), sounds, music
  data/               content for kits (levels, items, cards)`}</Code>
            <Code>{`dreamsy-tool validate my-game
dreamsy-tool pack my-game my-game.dpk`}</Code>
          </section>

          <section id="kits">
            <h2>Kits</h2>
            <p>
              Kits are tested Luau libraries for whole genres: platformer, rpg, farming, racing and seventeen more. A game
              opts in with <code>kits = ["platformer"]</code> in <code>dreamsy.toml</code> and{" "}
              <code>require("@kit/platformer")</code> in a script.
            </p>
          </section>

          <section id="more">
            <h2>Read more</h2>
            <ul className="more">
              <li><a href={`${GITHUB}/blob/main/docs/README.md`}>docs/README.md</a> · the overview and the map of the spec</li>
              <li><a href={`${GITHUB}/blob/main/docs/format.md`}>docs/format.md</a> · the package format</li>
              <li><a href={`${GITHUB}/blob/main/docs/engine.md`}>docs/engine.md</a> · how the engine works</li>
              <li><a href={`${GITHUB}/blob/main/docs/harness.md`}>docs/harness.md</a> · how games get built</li>
              <li><a href={`${GITHUB}/blob/main/format/LUAU_API.md`}>format/LUAU_API.md</a> · the scripting API</li>
            </ul>
          </section>
        </article>
      </main>
      <Footer />
    </>
  );
}
