"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { useEffect, useState } from "react";

export function NavMenu({ github }: { github: string }) {
  const [open, setOpen] = useState(false);
  const path = usePathname();

  useEffect(() => setOpen(false), [path]);
  useEffect(() => {
    if (!open) return;
    const esc = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    window.addEventListener("keydown", esc);
    return () => window.removeEventListener("keydown", esc);
  }, [open]);

  return (
    <div className="nav-menu">
      <button type="button" className="nav-burger" aria-expanded={open} aria-controls="nav-panel" onClick={() => setOpen((o) => !o)}>
        <span aria-hidden className={open ? "x" : ""}><i /><i /><i /></span>
        <span className="sr-only">{open ? "Close menu" : "Menu"}</span>
      </button>
      {open && (
        <nav id="nav-panel" className="nav-panel">
          <Link href="/#how" onClick={() => setOpen(false)}>How it works</Link>
          <Link href="/library">Library</Link>
          <Link href="/kits">Kits</Link>
          <Link href="/docs">Docs</Link>
          <a href={github} target="_blank" rel="noreferrer">GitHub</a>
          <Link className="btn btn-sm" href="/editor" prefetch={false}>Open editor</Link>
        </nav>
      )}
    </div>
  );
}
