"use client";

import { useMemo, useState } from "react";
import { GAMES } from "@/lib/content";
import { GameCard } from "./GameCard";

export function LibraryGrid() {
  const tags = useMemo(() => ["all", ...new Set(GAMES.flatMap((g) => g.tags))], []);
  const [tag, setTag] = useState("all");
  const games = tag === "all" ? GAMES : GAMES.filter((g) => g.tags.includes(tag));
  return (
    <>
      <div className="filters" role="toolbar" aria-label="Filter by genre">
        {tags.map((t) => (
          <button key={t} type="button" className={t === tag ? "on" : ""} aria-pressed={t === tag} onClick={() => setTag(t)}>
            {t}
          </button>
        ))}
      </div>
      <div className="game-grid">
        {games.map((g, i) => (
          <GameCard key={g.id} game={g} eager={i === 0} />
        ))}
      </div>
    </>
  );
}
