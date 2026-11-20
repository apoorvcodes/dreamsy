import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { cache } from "react";
import { PlayActions } from "@/components/PlayActions";
import { PlayFrame } from "@/components/PlayFrame";
import { Logo } from "@/components/Logo";
import { cloudUrl, publicGame } from "@/lib/server/cloud";
import { HttpError } from "@/lib/server/paths";
import { playerVersion } from "@/lib/server/player";

export const revalidate = 30;

export function generateStaticParams() {
  return []; // Generate published games on demand, including games published after a deploy.
}

type Props = { params: Promise<{ slug: string }> };

const load = cache(async (slug: string) => {
  if (!/^[a-z0-9]{4,32}$/.test(slug)) return null;
  try {
    return await publicGame(slug);
  } catch (error) {
    if (error instanceof HttpError && error.status === 404) return null;
    throw error;
  }
});

export async function generateMetadata({ params }: Props): Promise<Metadata> {
  const { slug } = await params;
  const game = await load(slug);
  if (!game) return { title: "Game not found" };
  const description = `${game.name}, a game by ${game.author}, made with Dreamsy. Play it in your browser.`;
  const images = game.hasThumb ? [{ url: `/api/library/${slug}/thumb`, width: 720, height: 1280, alt: game.name }] : undefined;
  return {
    title: game.name,
    description,
    alternates: { canonical: `/play/${slug}` },
    openGraph: { type: "website", url: `/play/${slug}`, title: `${game.name} · Dreamsy`, description, ...(images ? { images } : {}) },
    twitter: { card: images ? "summary_large_image" : "summary", title: `${game.name} · Dreamsy`, description },
  };
}

export default async function Play({ params }: Props) {
  const { slug } = await params;
  const game = await load(slug);
  if (!game) notFound();
  const version = await playerVersion();
  const base = version ? `/player/${version}` : "/player";
  const src = `${base}/dreamsy-player.html?api=${encodeURIComponent(cloudUrl() ?? "")}&public=${slug}`;
  return (
    <div className="play-page">
      <header className="play-bar">
        <Link href="/" aria-label="Dreamsy home"><Logo /></Link>
        <span className="spacer" />
        <Link className="btn btn-light btn-sm" href="/editor" prefetch={false}>Make your own</Link>
      </header>
      <main className="play-main">
        <PlayFrame src={src} title={game.name} />
        <aside className="play-info">
          <span className="kicker">made with dreamsy</span>
          <h1>{game.name}</h1>
          <p className="play-by">
            by {game.link ? <a href={game.link} target="_blank" rel="noreferrer nofollow">{game.author}</a> : game.author}
            {game.plays ? ` · ${game.plays} ${game.plays === 1 ? "play" : "plays"}` : ""}
          </p>
          <PlayActions slug={slug} remix={game.remix} />
          <p className="play-hint">Touch, keyboard and gamepads all work. Sound starts on your first tap or key press. On a phone, press Full screen and turn it sideways for landscape games.</p>
        </aside>
      </main>
    </div>
  );
}
