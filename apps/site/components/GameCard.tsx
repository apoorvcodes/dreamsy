import type { Game } from "@/lib/content";

export function GameCard({ game, eager = false }: { game: Game; eager?: boolean }) {
  const shot = `/games/${game.id}`;
  return (
    <article className="game">
      <div className="game-shot">
        <img
          src={`${shot}-480.webp`}
          srcSet={`${shot}-480.webp 480w, ${shot}-720.webp 720w`}
          sizes="(max-width: 586px) 90vw, (max-width: 895px) 45vw, (max-width: 1204px) 30vw, 350px"
          width={480}
          height={360}
          alt={`${game.name} running on the Dreamsy engine`}
          loading={eager ? "eager" : "lazy"}
          fetchPriority={eager ? "high" : undefined}
          decoding="async"
        />
      </div>
      <div className="game-body">
        <h3>{game.name}</h3>
        <p>{game.blurb}</p>
        <div className="game-tags">
          {game.tags.map((t) => (
            <span key={t}>{t}</span>
          ))}
          <span className="view">{game.view}</span>
        </div>
      </div>
    </article>
  );
}
