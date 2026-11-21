"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { BuildEvent, BuildRecord, GameDetail, KeyStatus, StreamMessage } from "@/lib/editor-types";
import { timeline } from "@/lib/timeline";
import { Logo } from "../Logo";
import { ago, api, fileToImage, hasBuildKey, type CloudStatus } from "./api";
import { KeysDialog } from "./KeysDialog";
import { Music } from "./Music";
import { PublishDialog } from "./PublishDialog";
import { Sprites } from "./Sprites";

const CHIPS = ["Make it harder", "Add a boss fight", "More juice and particles", "Add a new level", "Polish the art", "Fix the controls"];
const STARTERS = [
  "A frog that hops between clouds collecting strawberries",
  "A tiny space shooter with waves of asteroids and a boss",
  "A cozy farming game where you grow flowers and sell them at a market",
];
const TEXT = /\.(json|luau|lua|toml|md|txt|csv)$/i;
const IMAGE = /\.(svg|png|jpe?g|webp|gif)$/i;
const AUDIO = /\.(ogg|mp3|wav)$/i;

export function Workspace({ id }: { id: string }) {
  const [game, setGame] = useState<GameDetail | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [keys, setKeys] = useState<KeyStatus | null>(null);
  const [keysOpen, setKeysOpen] = useState(false);
  const [publishOpen, setPublishOpen] = useState(false);
  const [cloud, setCloud] = useState<CloudStatus | null>(null);
  const published = cloud?.publication?.slug ?? null;
  const [live, setLive] = useState<{ buildId: string; events: BuildEvent[] } | null>(null);
  const [running, setRunning] = useState(false);
  const [stamp, setStamp] = useState(() => Date.now());
  const [viewing, setViewing] = useState<number | null>(null);
  const [tab, setTab] = useState<"sprites" | "music" | "files" | "versions">("sprites");
  const [draft, setDraft] = useState("");
  const [focusTick, setFocusTick] = useState(0);
  const ask = (text: string) => {
    setDraft(text);
    setFocusTick((n) => n + 1);
  };
  const chat = useRef<HTMLDivElement>(null);

  const refresh = useCallback(async () => {
    try {
      const r = await api.game(id);
      setGame(r.game);
      setRunning(r.running);
      return r;
    } catch (e) {
      setError((e as Error).message);
      return null;
    }
  }, [id]);

  const follow = useCallback(() => {
    const es = new EventSource(`/api/games/${id}/build`);
    es.onmessage = (m) => {
      const msg = JSON.parse(m.data) as StreamMessage;
      if (msg.type === "event") {
        setRunning(true);
        setLive((l) => (l && l.buildId === msg.buildId ? { buildId: l.buildId, events: [...l.events, msg.event] } : { buildId: msg.buildId, events: [msg.event] }));
      } else {
        es.close();
        setRunning(false);
        setLive(null);
        if (msg.type === "done") {
          setViewing(null);
          setStamp(Date.now());
        }
        void refresh();
      }
    };
    es.onerror = () => es.close();
    return es;
  }, [id, refresh]);

  useEffect(() => {
    api.cloudStatus(id).then(setCloud).catch(() => {});
  }, [id, game?.versions.length]);

  useEffect(() => {
    api.keys().then(setKeys).catch(() => {});
    let es: EventSource | null = null;
    refresh().then((r) => {
      if (r?.running) es = follow();
    });
    return () => es?.close();
  }, [refresh, follow]);

  useEffect(() => {
    chat.current?.scrollTo({ top: chat.current.scrollHeight, behavior: "smooth" });
  }, [game?.builds.length, live?.events.length]);

  const startBuild = async (prompt: string, files: File[]) => {
    const images = await Promise.all(files.map(fileToImage));
    const { build } = await api.build(id, prompt, images);
    setGame((g) => (g ? { ...g, builds: [...g.builds, build] } : g));
    setLive({ buildId: build.id, events: [] });
    setRunning(true);
    follow();
  };

  if (error && !game) {
    return (
      <div className="ws-error">
        <p>{error}</p>
        <Link className="btn btn-light btn-sm" href="/editor">Back to your games</Link>
      </div>
    );
  }

  const built = !!game && game.files.length > 0;
  const builds = game?.builds ?? [];

  return (
    <div className="ws">
      <header className="ws-bar">
        <Link href="/editor" className="ws-back" aria-label="Back to your games">←</Link>
        <Link href="/" aria-label="Dreamsy home" className="ws-logo"><Logo /></Link>
        <span className="ws-sep" />
        {game ? <GameName id={id} name={game.name} onSaved={refresh} /> : <span className="muted">Loading…</span>}
        <span className="spacer" />
        {cloud?.cloudId && <span className="cloud-chip" title={`${cloud.versions} versions saved to the Dreamsy cloud`}>☁ saved</span>}
        {published ? (
          <>
            <a className="btn btn-sm" href={`/play/${published}`}>▶ Play</a>
            <RemixButton id={id} />
          </>
        ) : (
          <button type="button" className="btn btn-sm" disabled={!built || running} onClick={() => setPublishOpen(true)}>Publish</button>
        )}
        <button type="button" className={`btn btn-sm ${hasBuildKey(keys) ? "btn-light" : ""}`} onClick={() => setKeysOpen(true)}>
          {hasBuildKey(keys) ? "API keys" : "Add API key"}
        </button>
      </header>

      <div className="ws-grid">
        <section className="ws-preview">
          <Preview id={id} game={game} built={built} stamp={stamp} viewing={viewing} running={running} onLatest={() => setViewing(null)} />
        </section>

        <section className="ws-chat">
          <div className="chat" ref={chat}>
            {builds.length === 0 ? (
              <div className="chat-empty">
                <h2>{built ? "What should change?" : "What are we making?"}</h2>
                <p>{built ? "Describe a change and Dreamsy will build it, then playtest it." : "Describe the game in a sentence or two. You can attach images to set the art style."}</p>
                {!built && (
                  <div className="starters">
                    {STARTERS.map((s) => (
                      <button key={s} type="button" disabled={running || !hasBuildKey(keys)} onClick={() => void startBuild(s, []).catch((e) => alert((e as Error).message))}>{s}</button>
                    ))}
                  </div>
                )}
              </div>
            ) : (
              builds.map((b) => <BuildEntry key={b.id} build={b} events={live?.buildId === b.id ? live.events : b.events} live={live?.buildId === b.id} onView={(v) => { setViewing(v); setStamp(Date.now()); }} />)
            )}
          </div>
          {published ? (
            <div className="locked">
              <strong>Published, so this game is final.</strong>
              <span>It's in the library for everyone to play. To change it, remix it: you get an editable copy.</span>
              <RemixButton id={id} />
            </div>
          ) : (
          <Composer
            text={draft}
            setText={setDraft}
            focusTick={focusTick}
            built={built}
            running={running}
            canBuild={hasBuildKey(keys)}
            onNeedKey={() => setKeysOpen(true)}
            onCancel={() => void api.cancel(id)}
            onSend={startBuild}
          />
          )}
        </section>

        <aside className="ws-side">
          <div className="tabs" role="tablist">
            {(["sprites", "music", "files", "versions"] as const).map((t) => (
              <button key={t} role="tab" aria-selected={tab === t} className={tab === t ? "on" : ""} onClick={() => setTab(t)}>
                {t === "versions" ? `Versions${game?.versions.length ? ` · ${game.versions.length}` : ""}` : t[0]!.toUpperCase() + t.slice(1)}
              </button>
            ))}
          </div>
          {tab === "sprites" ? (
            <Sprites id={id} game={game} stamp={stamp} running={running} onAsk={ask} onSaved={() => { setStamp(Date.now()); void refresh(); }} />
          ) : tab === "music" ? (
            <Music id={id} game={game} stamp={stamp} running={running} onAsk={ask} />
          ) : tab === "files" ? (
            <Files id={id} game={game} stamp={stamp} />
          ) : (
            <Versions
              game={game}
              viewing={viewing}
              running={running}
              onView={(v) => { setViewing(v); setStamp(Date.now()); }}
              onRestore={async (v) => {
                if (!confirm(`Restore version ${v}? Your current files are saved as a new version first.`)) return;
                try {
                  await api.restore(id, v);
                  setViewing(null);
                  setStamp(Date.now());
                  await refresh();
                } catch (e) {
                  alert((e as Error).message);
                }
              }}
            />
          )}
        </aside>
      </div>
      <KeysDialog open={keysOpen} onClose={() => setKeysOpen(false)} onChange={setKeys} />
      {game && <PublishDialog id={id} name={game.name} open={publishOpen} onClose={() => setPublishOpen(false)} onChange={setCloud} />}
    </div>
  );
}

function GameName({ id, name, onSaved }: { id: string; name: string; onSaved: () => void }) {
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(name);
  useEffect(() => setValue(name), [name]);
  const save = async () => {
    setEditing(false);
    if (!value.trim() || value.trim() === name) return setValue(name);
    try {
      await api.rename(id, value.trim());
      onSaved();
    } catch (e) {
      alert((e as Error).message);
      setValue(name);
    }
  };
  return editing ? (
    <input
      className="ws-name-input"
      value={value}
      autoFocus
      maxLength={80}
      onChange={(e) => setValue(e.target.value)}
      onBlur={() => void save()}
      onKeyDown={(e) => {
        if (e.key === "Enter") void save();
        if (e.key === "Escape") {
          setValue(name);
          setEditing(false);
        }
      }}
    />
  ) : (
    <button type="button" className="ws-name" onClick={() => setEditing(true)} title="Rename">{name}</button>
  );
}

function Preview({ id, game, built, stamp, viewing, running, onLatest }: { id: string; game: GameDetail | null; built: boolean; stamp: number; viewing: number | null; running: boolean; onLatest: () => void }) {
  const pkg = `/api/games/${id}/pack${viewing !== null ? `?v=${viewing}` : ""}`;
  const src = `/player/dreamsy-player.html?pkg=${encodeURIComponent(pkg)}&t=${stamp}`;
  const landscape = game?.orientation === "landscape";
  return (
    <>
      <div className={`player ${landscape ? "landscape" : "portrait"}`}>
        {built || viewing !== null ? (
          <iframe key={src} src={src} title="Game preview" allow="autoplay; fullscreen; gamepad" />
        ) : (
          <div className="player-empty">
            <span>{running ? "Building your game…" : "Your game shows up here after the first build."}</span>
          </div>
        )}
        {running && built && <span className="player-badge">building…</span>}
      </div>
      {viewing !== null && (
        <div className="viewing">
          Viewing version {viewing}
          <button type="button" onClick={onLatest}>back to latest</button>
        </div>
      )}
      {(built || viewing !== null) && (
        <div className="player-actions">
          <a className="tool" href={src} target="_blank" rel="noreferrer">Open in new tab</a>
          <a className="tool" href={`${pkg}${viewing !== null ? "&" : "?"}download=1`}>Download .dpk</a>
        </div>
      )}
    </>
  );
}

function BuildEntry({ build, events, live, onView }: { build: BuildRecord; events: BuildEvent[]; live: boolean; onView: (v: number) => void }) {
  const { steps, working, usd } = useMemo(() => timeline(events), [events]);
  const [open, setOpen] = useState(live);
  useEffect(() => setOpen(live || build.status === "failed"), [live, build.status]);
  const status = live ? "running" : build.status;
  const cost = build.usd ?? usd;
  return (
    <div className="entry">
      <div className="bubble-user">
        <p>{build.prompt}</p>
        {build.images > 0 && <span className="muted">{build.images} reference {build.images === 1 ? "image" : "images"}</span>}
      </div>
      <div className={`bubble-bot status-${status}`}>
        <div className="bot-head">
          <span className="dot" aria-hidden />
          <strong>
            {status === "running" ? working ?? steps.at(-1)?.label ?? "Starting…" : status === "succeeded" ? "Done" : status === "canceled" ? "Canceled" : "Build failed"}
          </strong>
          {steps.length > 0 && (
            <button type="button" className="link" onClick={() => setOpen((o) => !o)}>{open ? "hide steps" : `${steps.length} steps`}</button>
          )}
        </div>
        {open && steps.length > 0 && (
          <ol className="steps-list">
            {steps.map((s) => (
              <li key={s.key} className={s.bad ? "bad" : ""}>
                <span>{s.label}</span>
                {s.text && (s.kind === "plan" || s.kind === "note" || s.kind === "route" || s.kind === "cloud") && <p>{s.text}</p>}
              </li>
            ))}
          </ol>
        )}
        {build.summary && <p className="summary">{build.summary}</p>}
        {build.error && <p className="form-error">{build.error}</p>}
        <div className="bot-foot">
          <span>{ago(build.at)}</span>
          {build.seconds ? <span>{build.seconds < 60 ? `${build.seconds}s` : `${Math.round(build.seconds / 60)} min`}</span> : null}
          {cost ? <span>${cost.toFixed(2)}</span> : null}
          {build.cloud && "gameId" in build.cloud && <span>saved to cloud</span>}
          {build.version !== undefined && (
            <button type="button" className="link" onClick={() => onView(build.version!)}>version {build.version}</button>
          )}
        </div>
      </div>
    </div>
  );
}

function Composer({ text, setText, focusTick, built, running, canBuild, onNeedKey, onCancel, onSend }: { text: string; setText: (t: string | ((p: string) => string)) => void; focusTick: number; built: boolean; running: boolean; canBuild: boolean; onNeedKey: () => void; onCancel: () => void; onSend: (prompt: string, files: File[]) => Promise<void> }) {
  const [files, setFiles] = useState<File[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const ta = useRef<HTMLTextAreaElement>(null);
  const picker = useRef<HTMLInputElement>(null);
  const previews = useMemo(() => files.map((f) => URL.createObjectURL(f)), [files]);
  useEffect(() => () => previews.forEach((u) => URL.revokeObjectURL(u)), [previews]);

  useEffect(() => {
    if (!focusTick || !ta.current) return;
    const el = ta.current;
    el.focus();
    el.setSelectionRange(el.value.length, el.value.length);
  }, [focusTick]);

  useEffect(() => {
    const el = ta.current;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = `${Math.min(220, el.scrollHeight)}px`;
  }, [text]);

  const add = (list: FileList | null) => {
    if (!list) return;
    const ok = [...list].filter((f) => ["image/png", "image/jpeg", "image/webp"].includes(f.type) && f.size < 6_000_000);
    setFiles((p) => [...p, ...ok].slice(0, 6));
  };

  const send = async () => {
    if (!text.trim() || busy || running) return;
    if (!canBuild) return onNeedKey();
    setBusy(true);
    setError(null);
    try {
      await onSend(text.trim(), files);
      setText("");
      setFiles([]);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="composer" onDragOver={(e) => e.preventDefault()} onDrop={(e) => { e.preventDefault(); add(e.dataTransfer.files); }}>
      {files.length > 0 && (
        <div className="attach">
          {previews.map((u, i) => (
            <div key={u} className="attach-thumb">
              <img src={u} alt="" />
              <button type="button" aria-label="Remove image" onClick={() => setFiles((p) => p.filter((_, j) => j !== i))}>×</button>
            </div>
          ))}
        </div>
      )}
      <textarea
        ref={ta}
        value={text}
        rows={2}
        disabled={running}
        placeholder={running ? "Building… you can write the next change when it finishes" : built ? "What should change? e.g. “add a dash move and make the slimes bounce”" : "Describe your game…"}
        onChange={(e) => setText(e.target.value)}
        onPaste={(e) => e.clipboardData.files.length && add(e.clipboardData.files)}
        onKeyDown={(e) => {
          if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) {
            e.preventDefault();
            void send();
          }
        }}
      />
      {built && !running && (
        <div className="chips-row">
          {CHIPS.map((c) => (
            <button key={c} type="button" onClick={() => { setText((t) => (t.trim() ? `${t.trim()}. ${c}` : c)); ta.current?.focus(); }}>{c}</button>
          ))}
        </div>
      )}
      <div className="composer-bar">
        <input ref={picker} type="file" accept="image/png,image/jpeg,image/webp" multiple hidden onChange={(e) => { add(e.target.files); e.target.value = ""; }} />
        <button type="button" className="tool" onClick={() => picker.current?.click()} disabled={running}>+ image</button>
        <span className="hint-key">⌘ ↵ to build</span>
        <span className="spacer" />
        {running ? (
          <button type="button" className="btn btn-light btn-sm" onClick={onCancel}>Stop build</button>
        ) : canBuild ? (
          <button type="button" className="btn btn-sm" disabled={!text.trim() || busy} onClick={() => void send()}>{busy ? "Starting…" : "Build"}</button>
        ) : (
          <button type="button" className="btn btn-sm" onClick={onNeedKey}>Add API key to build</button>
        )}
      </div>
      {error && <p className="form-error">{error}</p>}
    </div>
  );
}

function Files({ id, game, stamp }: { id: string; game: GameDetail | null; stamp: number }) {
  const [selected, setSelected] = useState<string | null>(null);
  const [text, setText] = useState<string | null>(null);
  const files = game?.files ?? [];

  useEffect(() => {
    setText(null);
    if (!selected || !TEXT.test(selected)) return;
    fetch(`/api/games/${id}/file?path=${encodeURIComponent(selected)}`)
      .then((r) => (r.ok ? r.text() : Promise.reject()))
      .then((t) => setText(t.length > 60_000 ? `${t.slice(0, 60_000)}\n…` : t))
      .catch(() => setText("Couldn't open this file."));
  }, [id, selected, stamp]);

  if (!files.length) return <p className="muted pad">Files show up here after the first build.</p>;

  const url = (p: string) => `/api/games/${id}/file?path=${encodeURIComponent(p)}&t=${stamp}`;
  let lastDir = "";
  return (
    <div className="files">
      <ul className="tree">
        {files.map((f) => {
          const dir = f.path.includes("/") ? f.path.slice(0, f.path.lastIndexOf("/")) : "";
          const head = dir !== lastDir ? dir : null;
          lastDir = dir;
          return (
            <li key={f.path}>
              {head && <span className="tree-dir">{head}/</span>}
              <button type="button" className={selected === f.path ? "on" : ""} style={{ paddingLeft: dir ? 18 : 8 }} onClick={() => setSelected(f.path)}>
                {f.path.slice(dir ? dir.length + 1 : 0)}
              </button>
            </li>
          );
        })}
      </ul>
      {selected && (
        <div className="viewer">
          <div className="viewer-head">
            <code>{selected}</code>
            <button type="button" className="icon-close" aria-label="Close file" onClick={() => setSelected(null)}>×</button>
          </div>
          {IMAGE.test(selected) ? (
            <div className="viewer-img"><img src={url(selected)} alt={selected} /></div>
          ) : AUDIO.test(selected) ? (
            <audio controls src={url(selected)} />
          ) : TEXT.test(selected) ? (
            <pre className="code small"><code>{text ?? "Loading…"}</code></pre>
          ) : (
            <p className="muted pad">No preview for this file type.</p>
          )}
        </div>
      )}
    </div>
  );
}

function Versions({ game, viewing, running, onView, onRestore }: { game: GameDetail | null; viewing: number | null; running: boolean; onView: (v: number) => void; onRestore: (v: number) => void }) {
  const list = [...(game?.versions ?? [])].reverse();
  if (!list.length) return <p className="muted pad">Every successful build is saved here as a version you can go back to.</p>;
  const latest = list[0]!.version;
  return (
    <ul className="versions">
      {list.map((v) => (
        <li key={v.version} className={viewing === v.version ? "on" : ""}>
          <div className="v-top">
            <strong>v{v.version}</strong>
            {v.version === latest && <span className="pill on">latest</span>}
            <span className="muted">{ago(v.at)}</span>
          </div>
          <p>{v.label}</p>
          <div className="v-actions">
            <button type="button" className="tool" onClick={() => onView(v.version)}>Play</button>
            {v.version !== latest && (
              <button type="button" className="tool" disabled={running} onClick={() => onRestore(v.version)}>Restore</button>
            )}
          </div>
        </li>
      ))}
    </ul>
  );
}

function RemixButton({ id }: { id: string }) {
  const router = useRouter();
  const [busy, setBusy] = useState(false);
  return (
    <button
      type="button"
      className="btn btn-light btn-sm"
      disabled={busy}
      onClick={async () => {
        setBusy(true);
        try {
          router.push(`/editor/${(await api.remix(id)).id}`);
        } catch (e) {
          alert((e as Error).message);
          setBusy(false);
        }
      }}
    >
      {busy ? "Copying…" : "Remix"}
    </button>
  );
}
