import Link from "next/link";
import { GITHUB, MCP_COMMAND } from "@/lib/content";
import { CopyCommand } from "../CopyCommand";
import { SiteNav } from "../SiteNav";
import { HeroAnimation } from "./HeroAnimation";

export function Hero() {
  return (
    <HeroAnimation>
      <canvas className="sky" aria-hidden />
      <SiteNav />

      <div className="hero-grid">
        <div className="copy">
          <p className="eyebrow">
            <span className="led" aria-hidden /> Open source · 2D &amp; 2.5D · iOS, Android &amp; web
          </p>
          <h1 className="display">
            <span className="line">Describe a game.</span>
            <span className="line">
              Then <span className="hl">play it.</span>
            </span>
          </h1>
          <div className="ctas">
            <Link className="btn" href="/docs">Get started</Link>
            <a className="btn btn-light" href={GITHUB} target="_blank" rel="noreferrer">View on GitHub</a>
          </div>
          <CopyCommand command={MCP_COMMAND} />
        </div>

        <div className="stage">
          <canvas
            className="device"
            width={200}
            height={320}
            role="img"
            aria-label="A little handheld that turns ideas into games. Click it for another idea."
            title="Click for another idea"
          />
          <div className="window prompt-window">
            <div className="titlebar">
              <span className="close" aria-hidden />
              <span className="title">idea.txt</span>
            </div>
            <div className="window-body">
              <span className="status" data-state="listening">listening</span>
              <p className="prompt-text">
                <span className="gt">&gt;</span> <span data-hero-prompt />
                <span className="caret" aria-hidden />
              </p>
              <ol className="steps" />
            </div>
          </div>
        </div>
      </div>
    </HeroAnimation>
  );
}
