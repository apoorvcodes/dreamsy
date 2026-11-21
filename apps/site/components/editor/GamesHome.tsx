"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useMemo, useState } from "react";
import type { GameSummary, KeyStatus } from "@/lib/editor-types";
import { ago, api, hasBuildKey, type LibraryGame, type MineGame } from "./api";
import { KeysDialog } from "./KeysDialog";

type Tab = "mine" | "library";

export function GamesHome() {
  const router = useRouter();
  const [tab, setTab] = useState<Tab>("library");
  const [games, setGames] = useState<GameSummary[] | null>(null);
  const [cloud, setCloud] = useState<MineGame[] | null>(null);
  const [keys, setKeys] = useState<KeyStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [name, setName] = useState("");
  const [creating, setCreating] = useState(false);
  const [keysOpen, setKeysOpen] = useState(false);
  const [query, setQuery] = useState("");
  const [libTotal, setLibTotal] = useState<number | null>(null);

  useEffect(() => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    let tries = 0;
    const load = () =>
      api
        .games()
        .then((r) => {
          setGames(r.games);
          if (r.bannersPending && tries++ < 60) timer = setTimeout(load, 3000);
        })
        .catch((e) => setError((e as Error).message));
    void load();
    api.keys().then(setKeys).catch(() => {});
    api.mine().then((r) => setCloud(r.games)).catch(() => setCloud([]));
    api.library("").then((r) => setLibTotal(r.total)).catch(() => {});
    return () => clearTimeout(timer);
  }, []);

  const byLocal = useMemo(() => new Map((cloud ?? []).filter((c) => c.localId).map((c) => [c.localId!, c])), [cloud]);
  const cloudOnly = (cloud ?? []).filter((c) => !c.localId);

  const create = async (e: React.FormEvent) => {
    e.preventDefault();
    setCreating(true);
    setError(null);
    try {
      const { id } = await api.create(name || "Untitled game");
      router.push(`/editor/${id}`);
    } catch (err) {
      setError((err as Error).message);
      setCreating(false);
    }
  };

  const q = query.trim().toLowerCase();
  const shown = (games ?? []).filter((g) => !q || g.name.toLowerCase().includes(q) || g.id.includes(q));

  return (
    <main className="games-home">
      <div className="games-head">
        <div>
          <h1>Games</h1>
          <p>Your games live on this computer and save to the Dreamsy cloud. The library has everything people have published.</p>
        </div>
        <button type="button" className="btn btn-light btn-sm" onClick={() => setKeysOpen(true)}>API keys</button>
      </div>

      <div className="home-tabs" role="tablist">
        <button role="tab" aria-selected={tab === "library"} className={tab === "library" ? "on" : ""} onClick={() => setTab("library")}>
          Library <span>{libTotal ?? "…"}</span>
        </button>
        <button role="tab" aria-selected={tab === "mine"} className={tab === "mine" ? "on" : ""} onClick={() => setTab("mine")}>
          Your games <span>{games?.length ?? "…"}</span>
        </button>
      </div>

      {tab === "library" ? (
        <LibraryTab onOpened={(id) => router.push(`/editor/${id}`)} onTotal={setLibTotal} />
      ) : (
        <>
          {keys && !hasBuildKey(keys) && (
            <div className="notice">
              <div>
                <strong>Add an API key to start building.</strong>
                <span>Dreamsy runs on your own Anthropic or OpenAI key.</span>
              </div>
              <button type="button" className="btn btn-sm" onClick={() => setKeysOpen(true)}>Add key</button>
            </div>
          )}

          <form className="new-game" onSubmit={create}>
            <input value={name} onChange={(e) => setName(e.target.value)} placeholder="Name your new game" aria-label="New game name" maxLength={80} />
            <button type="submit" className="btn" disabled={creating}>{creating ? "Creating…" : "New game"}</button>
          </form>
          {error && <p className="form-error">{error}</p>}

          {games && games.length > 0 && (
            <div className="games-tools">
              <input type="search" value={query} onChange={(e) => setQuery(e.target.value)} placeholder={`Search your ${games.length} games`} aria-label="Search your games" />
              {q && <span className="muted">{shown.length} found</span>}
            </div>
          )}

          {games === null ? (
            <p className="muted">Loading…</p>
          ) : games.length === 0 ? (
            <p className="muted">No games yet. Name one above, or open one from the library.</p>
          ) : shown.length === 0 ? (
            <p className="muted">No games match “{query}”.</p>
          ) : (
            <ul className="games-list">
              {shown.map((g) => {
                const c = byLocal.get(g.id);
                return (
                  <li key={g.id} className="tile-wrap">
                    {c?.published && <a className="tile-play" href={`/play/${c.published}`}>▶ Play</a>}
                    <Link href={`/editor/${g.id}`} className="game-tile">
                      <Cover src={g.hasThumb ? `/api/games/${g.id}/thumb?t=${g.updated}` : null} sprite={g.banner ? `/api/games/${g.id}/file?path=${encodeURIComponent(g.banner)}` : null} name={g.name} empty={g.empty} />
                      <div className="tile-body">
                        <strong>{g.name}</strong>
                        <span>
                          {ago(g.updated)}
                          {g.builds ? ` · ${g.builds} ${g.builds === 1 ? "build" : "builds"}` : ""}
                        </span>
                        <div className="tile-badges">
                          {c?.published ? <span className="pill on">published</span> : c ? <span className="pill">in cloud</span> : <span className="pill muted-pill">local only</span>}
                        </div>
                      </div>
                    </Link>
                  </li>
                );
              })}
            </ul>
          )}

          {cloudOnly.length > 0 && (
            <>
              <h2 className="subhead cloud-head">In your cloud, not on this computer</h2>
              <ul className="games-list">
                {cloudOnly.map((c) => (
                  <li key={c.id}>
                    <div className="game-tile">
                      <Cover src={null} sprite={null} name={c.name} empty={c.latest === null} />
                      <div className="tile-body">
                        <strong>{c.name}</strong>
                        <span>{ago(c.updated)}{c.published ? " · published" : ""}</span>
                        <button
                          type="button"
                          className="tool"
                          disabled={c.latest === null}
                          onClick={async () => {
                            try {
                              const { id } = await api.importMine(c.id);
                              router.push(`/editor/${id}`);
                            } catch (e) {
                              alert((e as Error).message);
                            }
                          }}
                        >
                          Download to this computer
                        </button>
                      </div>
                    </div>
                  </li>
                ))}
              </ul>
            </>
          )}
        </>
      )}
      <KeysDialog open={keysOpen} onClose={() => setKeysOpen(false)} onChange={setKeys} />
    </main>
  );
}

function Cover({ src, sprite, name, empty }: { src: string | null; sprite: string | null; name: string; empty: boolean }) {
  return (
    <div className="tile-shot">
      {src ? (
        <div className="tile-cover">
          <img className="tile-blur" src={src} alt="" aria-hidden />
          <img className="tile-shot-img" src={src} alt={`${name} screenshot`} />
        </div>
      ) : sprite ? (
        <div className="tile-banner">
          <img src={sprite} alt="" />
          <span>{name}</span>
        </div>
      ) : (
        <span className="tile-empty">{empty ? "not built yet" : name}</span>
      )}
    </div>
  );
}

function LibraryTab({ onOpened, onTotal }: { onOpened: (id: string) => void; onTotal: (n: number) => void }) {
  const [query, setQuery] = useState("");
  const [games, setGames] = useState<LibraryGame[] | null>(null);
  const [total, setTotal] = useState(0);
  const [next, setNext] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  useEffect(() => {
    const t = setTimeout(() => {
      api
        .library(query.trim())
        .then((r) => {
          setGames(r.games);
          setTotal(r.total);
          setNext(r.next);
          if (!query.trim()) onTotal(r.total);
          setError(null);
        })
        .catch((e) => setError((e as Error).message));
    }, 250);
    return () => clearTimeout(t);
  }, [query]); // eslint-disable-line react-hooks/exhaustive-deps

  const more = async () => {
    if (!next) return;
    const r = await api.library(query.trim(), next);
    setGames((g) => [...(g ?? []), ...r.games]);
    setNext(r.next);
  };

  return (
    <>
      <div className="library-head">
        <input type="search" value={query} onChange={(e) => setQuery(e.target.value)} placeholder="Search games or creators" aria-label="Search the library" />
        <span className="lib-total">
          <strong>{total}</strong> {query.trim() ? `match${total === 1 ? "" : "es"}` : `game${total === 1 ? "" : "s"} in the library`}
        </span>
      </div>
      {error && <p className="form-error">{error}</p>}
      {games === null ? (
        <p className="muted">Loading the library…</p>
      ) : games.length === 0 ? (
        <p className="muted">{query.trim() ? `Nothing matches “${query}”.` : "Nothing published yet. Open one of your games and press Publish."}</p>
      ) : (
        <ul className="games-list">
          {games.map((g) => (
            <li key={g.slug}>
              <div className="game-tile lib-tile">
                <a href={`/play/${g.slug}`} className="lib-cover" aria-label={`Play ${g.name}`}>
                  <Cover src={g.hasThumb ? `/api/library/${g.slug}/thumb` : null} sprite={null} name={g.name} empty={false} />
                  <span className="lib-play" aria-hidden>▶</span>
                </a>
                <div className="tile-body">
                  <strong>{g.name}</strong>
                  <span>
                    by {g.link ? <a href={g.link} target="_blank" rel="noreferrer nofollow">{g.author}</a> : g.author} · {ago(g.updated)}
                    {g.plays ? ` · ${g.plays} ${g.plays === 1 ? "play" : "plays"}` : ""}
                  </span>
                  <div className="lib-actions">
                    <a className="btn btn-sm lib-play-btn" href={`/play/${g.slug}`}>▶ Play</a>
                    <a className="tool" href={`/api/library/${g.slug}/download`}>Download .dpk</a>
                    <button
                      type="button"
                      className="tool"
                      disabled={busy === g.slug}
                      onClick={async () => {
                        setBusy(g.slug);
                        try {
                          const { id } = await api.importLibrary(g.slug);
                          onOpened(id);
                        } catch (e) {
                          alert((e as Error).message);
                          setBusy(null);
                        }
                      }}
                    >
                      {busy === g.slug ? "Copying…" : g.remix ? "Remix in editor" : "Open a copy"}
                    </button>
                  </div>
                </div>
              </div>
            </li>
          ))}
        </ul>
      )}
      {next && (
        <div className="center">
          <button type="button" className="btn btn-light btn-sm" onClick={() => void more()}>Load more</button>
        </div>
      )}
    </>
  );
}
