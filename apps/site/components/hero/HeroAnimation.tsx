"use client";

import { useEffect, useRef, type ReactNode } from "react";

export function HeroAnimation({ children }: { children: ReactNode }) {
  const section = useRef<HTMLElement>(null);

  useEffect(() => {
    const hero = section.current!;
    let mounted = true;
    let stop: (() => void) | undefined;
    const observer = new IntersectionObserver(([entry]) => {
      if (!entry?.isIntersecting) return;
      observer.disconnect();
      void import("./engine").then(({ startHero }) => {
        if (!mounted) return;
        stop = startHero({
          hero,
          sky: hero.querySelector<HTMLCanvasElement>(".sky")!,
          device: hero.querySelector<HTMLCanvasElement>(".device")!,
          prompt: hero.querySelector<HTMLElement>("[data-hero-prompt]")!,
          status: hero.querySelector<HTMLElement>(".status")!,
          steps: hero.querySelector<HTMLElement>(".steps")!,
        });
      });
    });
    observer.observe(hero);
    return () => {
      mounted = false;
      observer.disconnect();
      stop?.();
    };
  }, []);

  return <section className="hero" ref={section}>{children}</section>;
}
