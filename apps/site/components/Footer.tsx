import Link from "next/link";
import { GITHUB } from "@/lib/content";
import { Logo } from "./Logo";

export function Footer() {
  return (
    <footer className="footer">
      <div className="footer-inner">
        <div>
          <Logo />
          <p>An open-source engine and AI builder for 2D and 2.5D games.</p>
        </div>
        <nav>
          <h4>Make</h4>
          <Link href="/docs">Get started</Link>
          <Link href="/docs#mcp">Use with Claude Code</Link>
          <Link href="/editor" prefetch={false}>Editor</Link>
        </nav>
        <nav>
          <h4>Play</h4>
          <Link href="/library">Library</Link>
          <Link href="/kits">Kits</Link>
        </nav>
        <nav>
          <h4>Code</h4>
          <a href={GITHUB} target="_blank" rel="noreferrer">GitHub</a>
          <Link href="/docs#engine">Engine</Link>
          <Link href="/docs#format">Game format</Link>
        </nav>
      </div>
      <div className="footer-ground" aria-hidden />
    </footer>
  );
}
