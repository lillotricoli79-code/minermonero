import { useEffect, useRef, useState } from "react";
import DataRow from "./components/DataRow";
import MetricCard from "./components/MetricCard";
import { request } from "./services/api";
import {
  Activity,
  ArrowUpRight,
  Check,
  CircleHelp,
  Cpu,
  Gauge,
  LayoutDashboard,
  LockKeyhole,
  Play,
  Radio,
  RefreshCw,
  Save,
  Settings2,
  ShieldCheck,
  Square,
  Wifi,
  Zap,
} from "lucide-react";
import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

type Page = "overview" | "settings";
type Status = {
  state: "ATTIVO" | "AVVIO" | "OFFLINE";
  managed: boolean;
  error: string | null;
  hashrate: {
    "10s": number | null;
    "60s": number | null;
    "15m": number | null;
  };
  shares: {
    accepted: number | null;
    rejected: number | null;
    total: number | null;
  };
  uptime_seconds: number | null;
  pool: string | null;
  connected: boolean | null;
};
type MinerConfig = {
  pool: string;
  wallet: string;
  worker: string;
  password_set: boolean;
  password: string;
  threads: number;
  tls: boolean;
  tls_fingerprint: string;
  msr_mode: "auto" | "off" | "fast" | "custom";
  api_port: number;
};
type ChartPoint = { at: number; hashrate: number };
type LogItem = { id: number; time: number; message: string };

const emptyStatus: Status = {
  state: "OFFLINE",
  managed: false,
  error: null,
  hashrate: { "10s": null, "60s": null, "15m": null },
  shares: { accepted: null, rejected: null, total: null },
  uptime_seconds: null,
  pool: null,
  connected: null,
};

function formatHashrate(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) return "—";
  const units = ["H/s", "KH/s", "MH/s", "GH/s", "TH/s"];
  let amount = value;
  let unit = 0;
  while (amount >= 1000 && unit < units.length - 1) {
    amount /= 1000;
    unit += 1;
  }
  return `${amount.toLocaleString("it-IT", { maximumFractionDigits: 2 })} ${units[unit]}`;
}

function formatUptime(value: number | null): string {
  if (value == null) return "—";
  const days = Math.floor(value / 86400);
  const hours = Math.floor((value % 86400) / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  return days > 0 ? `${days}g ${hours}h` : `${hours}h ${minutes}m`;
}

function formatCount(value: number | null): string {
  return value == null ? "—" : value.toLocaleString("it-IT");
}

function App() {
  const [page, setPage] = useState<Page>("overview");
  const [status, setStatus] = useState<Status>(emptyStatus);
  const [config, setConfig] = useState<MinerConfig | null>(null);
  const [history, setHistory] = useState<ChartPoint[]>([]);
  const [logs, setLogs] = useState<LogItem[]>([]);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");
  const logCursor = useRef(0);

  useEffect(() => {
    let active = true;
    void request<{ lines: string[] }>("/logs/history?limit=12")
      .then(({ lines }) => {
        if (!active || !lines.length) return;
        const entries = lines.map((line, index): LogItem => {
          const parsed = line.match(/^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) \w+ (.*)$/);
          const time = parsed
            ? Date.parse(parsed[1].replace(" ", "T")) / 1000
            : Date.now() / 1000;
          return {
            id: index - lines.length,
            time: Number.isFinite(time) ? time : Date.now() / 1000,
            message: parsed ? parsed[2] : line,
          };
        });
        setLogs((current) => {
          const existing = new Set(current.map((item) => item.message));
          const unseen = entries.filter((item) => !existing.has(item.message));
          return [...unseen, ...current].slice(-12);
        });
      })
      .catch(() => {
        // The live log stream remains available if the history file cannot be read.
      });
    void request<MinerConfig>("/config")
      .then((value) => {
        if (active) setConfig({ ...value, password: "" });
      })
      .catch((error: unknown) => {
        if (active)
          setNotice(
            error instanceof Error
              ? error.message
              : "Bridge locale non raggiungibile",
          );
      });
    return () => {
      active = false;
    };
  }, []);

  useEffect(() => {
    let active = true;
    const poll = async () => {
      try {
        const [nextStatus, nextLogs] = await Promise.all([
          request<Status>("/status"),
          request<{ items: LogItem[] }>(`/logs?after=${logCursor.current}`),
        ]);
        if (!active) return;
        setStatus(nextStatus);
        const rate = nextStatus.hashrate["10s"];
        if (rate != null && Number.isFinite(rate)) {
          setHistory((current) =>
            [...current, { at: Date.now(), hashrate: rate }].slice(-48),
          );
        }
        if (nextLogs.items.length) {
          logCursor.current = Math.max(
            logCursor.current,
            ...nextLogs.items.map((item) => item.id),
          );
          setLogs((current) => [...current, ...nextLogs.items].slice(-12));
        }
      } catch {
        if (active) setStatus(emptyStatus);
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 2500);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, []);

  const updateConfig = (
    key: keyof MinerConfig,
    value: string | number | boolean,
  ) => {
    setConfig((current) => (current ? { ...current, [key]: value } : current));
  };

  const saveConfig = async () => {
    if (!config) return;
    setBusy(true);
    setNotice("");
    try {
      const { password_set: _passwordSet, ...values } = config;
      const saved = await request<Omit<MinerConfig, "password">>("/config", {
        method: "PUT",
        body: JSON.stringify({ ...values, password: config.password || null }),
      });
      setConfig({ ...saved, password: "" });
      setNotice("Configurazione salvata in config.json.");
    } catch (error) {
      setNotice(
        error instanceof Error
          ? error.message
          : "Impossibile salvare config.json.",
      );
    } finally {
      setBusy(false);
    }
  };

  const runMiner = async (action: "start" | "stop") => {
    setBusy(true);
    setNotice("");
    try {
      await request(`/miner/${action}`, { method: "POST" });
      setNotice(
        action === "start"
          ? "Avvio del miner richiesto."
          : "Arresto del miner richiesto.",
      );
      setStatus(await request<Status>("/status"));
    } catch (error) {
      setNotice(
        error instanceof Error ? error.message : "Comando non completato.",
      );
    } finally {
      setBusy(false);
    }
  };

  const active = status.state === "ATTIVO";
  const stateLabel = active
    ? status.managed
      ? "MINER ATTIVO"
      : "MINER RILEVATO"
    : status.state === "AVVIO"
      ? "AVVIO IN CORSO"
      : "NON CONNESSO";

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand-lockup">
          <div className="brand-mark">
            <Cpu size={21} />
          </div>
          <div>
            <div className="brand-name">
              UNIQUE<span>RX</span>
            </div>
            <div className="brand-caption">LOCAL MINER CONTROL</div>
          </div>
        </div>
        <div className="side-label">WORKSPACE</div>
        <nav className="side-nav" aria-label="Navigazione">
          <button
            className={`nav-item ${page === "overview" ? "selected" : ""}`}
            onClick={() => setPage("overview")}
          >
            <LayoutDashboard size={17} />
            <span>Panoramica</span>
          </button>
          <button
            className={`nav-item ${page === "settings" ? "selected" : ""}`}
            onClick={() => setPage("settings")}
          >
            <Settings2 size={17} />
            <span>Configurazione</span>
          </button>
        </nav>
        <div className="sidebar-spacer" />
        <div className="local-panel">
          <div className="local-panel-head">
            <ShieldCheck size={16} />
            CONTROLLO LOCALE
          </div>
          <p>
            Un solo config.json, API limitata a loopback. Credenziali custodite
            su questo dispositivo.
          </p>
          <div className="local-status">
            <i className="status-dot good" /> API bridge <span>127.0.0.1</span>
          </div>
        </div>
        <div className="side-footer">
          <span>UNIQUE RX</span>
          <span>v0.1.0</span>
        </div>
      </aside>

      <main className="main-area">
        <header className="topbar">
          <div className="breadcrumb">
            <span>MINER</span>
            <ArrowUpRight size={13} />
            <strong>
              {page === "overview" ? "PANORAMICA" : "CONFIGURAZIONE"}
            </strong>
          </div>
          <div className="topbar-right">
            <span
              className={`connection-pill ${active ? "online" : "offline"}`}
            >
              <i className="status-dot" />
              {stateLabel}
            </span>
            <span className="loopback-label">
              <LockKeyhole size={13} /> SOLO LOCALE
            </span>
          </div>
        </header>
        <div className="page-content">
          {notice && (
            <div className="notice" role="status">
              <span>{notice}</span>
              <button
                aria-label="Chiudi messaggio"
                onClick={() => setNotice("")}
              >
                ×
              </button>
            </div>
          )}
          {page === "overview" ? (
            <>
              <section className="page-heading">
                <div>
                  <div className="eyebrow">
                    <i /> CONTROL ROOM / 01
                  </div>
                  <h1>
                    Panoramica <span>miner</span>
                  </h1>
                  <p>
                    Telemetria del miner in tempo reale, senza metriche simulate.
                  </p>
                </div>
                <div className="primary-actions">
                  <button
                    className="button button-start"
                    disabled={busy || active}
                    onClick={() => void runMiner("start")}
                  >
                    <Play size={15} fill="currentColor" />
                    Avvia miner
                  </button>
                  <button
                    className="button button-stop"
                    disabled={busy || !status.managed}
                    onClick={() => void runMiner("stop")}
                  >
                    <Square size={13} fill="currentColor" />
                    Arresta
                  </button>
                </div>
              </section>
              <section className="metric-grid" aria-label="Metriche miner">
                <MetricCard
                  label="HASHRATE · 10S"
                  value={formatHashrate(status.hashrate["10s"])}
                  icon={<Zap size={16} />}
                  tone="lime"
                  detail="istantaneo"
                />
                <MetricCard
                  label="HASHRATE · 60S"
                  value={formatHashrate(status.hashrate["60s"])}
                  icon={<Activity size={16} />}
                  tone="blue"
                  detail="media mobile"
                />
                <MetricCard
                  label="HASHRATE · 15M"
                  value={formatHashrate(status.hashrate["15m"])}
                  icon={<Gauge size={16} />}
                  tone="orange"
                  detail="media mobile"
                />
                <MetricCard
                  label="SHARE ACCETTATE"
                  value={formatCount(status.shares.accepted)}
                  icon={<Check size={16} />}
                  tone="lime"
                  detail="confermate dal miner"
                />
                <MetricCard
                  label="SHARE RIFIUTATE"
                  value={formatCount(status.shares.rejected)}
                  icon={<CircleHelp size={16} />}
                  tone="orange"
                  detail="conteggio reale"
                />
                <MetricCard
                  label="UPTIME"
                  value={formatUptime(status.uptime_seconds)}
                  icon={<RefreshCw size={16} />}
                  tone="blue"
                  detail="sessione miner"
                />
              </section>
              <section className="chart-section">
                <div className="section-heading">
                  <div>
                    <div className="eyebrow">PRESTAZIONI</div>
                    <h2>Andamento hashrate</h2>
                  </div>
                  <div className="chart-legend">
                    <i /> Hashrate 10s <span>ULTIMI CAMPIONI</span>
                  </div>
                </div>
                <div className="chart-frame">
                  {history.length > 1 ? (
                    <ResponsiveContainer width="100%" height="100%">
                      <AreaChart
                        data={history}
                        margin={{ top: 12, right: 12, bottom: 0, left: 0 }}
                      >
                        <defs>
                          <linearGradient
                            id="rateFill"
                            x1="0"
                            y1="0"
                            x2="0"
                            y2="1"
                          >
                            <stop
                              offset="0%"
                              stopColor="#b8e86c"
                              stopOpacity={0.28}
                            />
                            <stop
                              offset="95%"
                              stopColor="#b8e86c"
                              stopOpacity={0}
                            />
                          </linearGradient>
                        </defs>
                        <CartesianGrid stroke="#e1e9e5" vertical={false} />
                        <XAxis
                          dataKey="at"
                          tickFormatter={(value: number) =>
                            new Date(value).toLocaleTimeString("it-IT", {
                              hour: "2-digit",
                              minute: "2-digit",
                              second: "2-digit",
                            })
                          }
                          tickLine={false}
                          axisLine={false}
                          tick={{ fill: "#87938e", fontSize: 11 }}
                          minTickGap={45}
                        />
                        <YAxis
                          tickFormatter={(value: number) =>
                            formatHashrate(value)
                          }
                          tickLine={false}
                          axisLine={false}
                          width={90}
                          tick={{ fill: "#87938e", fontSize: 11 }}
                        />
                        <Tooltip
                          labelFormatter={(value) =>
                            new Date(Number(value)).toLocaleTimeString("it-IT")
                          }
                          formatter={(value) => [
                            formatHashrate(Number(value)),
                            "Hashrate",
                          ]}
                          contentStyle={{
                            border: "1px solid #dfe8e3",
                            borderRadius: 8,
                            fontFamily: "IBM Plex Mono, monospace",
                            fontSize: 12,
                          }}
                        />
                        <Area
                          type="monotone"
                          dataKey="hashrate"
                          stroke="#6e9d2b"
                          strokeWidth={2.5}
                          fill="url(#rateFill)"
                          dot={false}
                          isAnimationActive={false}
                        />
                      </AreaChart>
                    </ResponsiveContainer>
                  ) : (
                    <div className="chart-empty">
                      <div className="empty-glyph">
                        <Activity size={20} />
                      </div>
                      <strong>In attesa dei campioni</strong>
                      <span>
                        Il grafico si popola quando il miner restituisce un
                        hashrate reale.
                      </span>
                    </div>
                  )}
                </div>
                <div className="chart-foot">
                  <span>Campioni raccolti da questa sessione dashboard</span>
                  <span>{history.length} PUNTI</span>
                </div>
              </section>
              <section className="lower-grid">
                <div className="data-section">
                  <div className="section-heading compact">
                    <div>
                      <div className="eyebrow">CONNESSIONE</div>
                      <h2>Pool attivo</h2>
                    </div>
                    <Radio size={17} className="section-icon" />
                  </div>
                  <div className="pool-address">
                    {status.pool || config?.pool || "Pool non configurato"}
                  </div>
                  <div className="data-rows">
                    <DataRow
                      icon={<Wifi size={15} />}
                      label="Stato connessione"
                      value={
                        status.connected === true
                          ? "Connesso"
                          : status.connected === false
                            ? "Disconnesso"
                            : "Non disponibile"
                      }
                      positive={status.connected === true}
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="Thread richiesti"
                      value={
                        config?.threads
                          ? String(config.threads)
                          : config
                            ? "Automatico"
                            : "—"
                      }
                    />
                    <DataRow
                      icon={<LockKeyhole size={15} />}
                      label="TLS pool"
                      value={
                        config?.tls ? "Attivo" : config ? "Disattivo" : "—"
                      }
                    />
                  </div>
                  <button
                    className="text-action"
                    onClick={() => setPage("settings")}
                  >
                    Apri configurazione <ArrowUpRight size={14} />
                  </button>
                </div>
                <div className="data-section">
                  <div className="section-heading compact">
                    <div>
                      <div className="eyebrow">PROCESSO</div>
                      <h2>Log miner</h2>
                    </div>
                    <span className="log-live">
                      <i className="status-dot" /> LIVE
                    </span>
                  </div>
                  <div className="log-list">
                    {logs.length ? (
                      logs
                        .slice(-6)
                        .reverse()
                        .map((entry) => (
                          <div className="log-line" key={entry.id}>
                            <time>
                              {new Date(entry.time * 1000).toLocaleTimeString(
                                "it-IT",
                              )}
                            </time>
                            <span>{entry.message}</span>
                          </div>
                        ))
                    ) : (
                      <div className="log-empty">
                        I log appariranno dopo l'avvio controllato del miner.
                      </div>
                    )}
                  </div>
                </div>
              </section>
              <footer className="page-footer">
                <span>UNIQUE RX · PANNELLO LOCALE</span>
                <span>
                  TELEMETRIA FORNITA DAL MINER <ShieldCheck size={13} />
                </span>
              </footer>
            </>
          ) : (
            <>
              <section className="page-heading">
                <div>
                  <div className="eyebrow">
                    <i /> SETUP / 02
                  </div>
                  <h1>
                    Configurazione <span>miner</span>
                  </h1>
                  <p>Un solo file di configurazione: config.json.</p>
                </div>
                <div className="config-note">
                  <LockKeyhole size={15} /> Credenziali custodite localmente
                </div>
              </section>
              <section className="settings-form-area">
                <div className="form-section-title">
                  <div className="form-icon">
                    <Settings2 size={17} />
                  </div>
                  <div>
                    <h2>Pool e risorse</h2>
                    <p>Le modifiche aggiornano direttamente config.json.</p>
                  </div>
                </div>
                {config ? (
                  <form
                    className="config-form"
                    onSubmit={(event) => {
                      event.preventDefault();
                      void saveConfig();
                    }}
                  >
                    <label className="field field-wide">
                      <span>Indirizzo pool</span>
                      <input
                        value={config.pool}
                        onChange={(event) =>
                          updateConfig("pool", event.target.value)
                        }
                        placeholder="host:porta"
                        autoComplete="off"
                        required
                      />
                      <small>
                        Formato host:porta, senza schema o credenziali.
                      </small>
                    </label>
                    <label className="field">
                      <span>Wallet</span>
                      <input
                        value={config.wallet}
                        onChange={(event) =>
                          updateConfig("wallet", event.target.value)
                        }
                        placeholder="Indirizzo wallet"
                        autoComplete="off"
                        required
                      />
                    </label>
                    <label className="field">
                      <span>Worker ID</span>
                      <input
                        value={config.worker}
                        onChange={(event) =>
                          updateConfig("worker", event.target.value)
                        }
                        placeholder="rig01"
                        autoComplete="off"
                      />
                    </label>
                    <label className="field field-wide">
                      <span>Password pool</span>
                      <input
                        type="password"
                        value={config.password}
                        onChange={(event) =>
                          updateConfig("password", event.target.value)
                        }
                        placeholder={
                          config.password_set
                            ? "Impostata · lascia vuoto per mantenere"
                            : "x"
                        }
                        autoComplete="new-password"
                      />
                      <small>
                        La password salvata non viene riletta né mostrata.
                      </small>
                    </label>
                    <div className="form-divider">
                      <span>RISORSE E SICUREZZA</span>
                    </div>
                    <label className="field">
                      <span>Thread CPU</span>
                      <input
                        type="number"
                        min="0"
                        max="256"
                        value={config.threads}
                        onChange={(event) =>
                          updateConfig("threads", Number(event.target.value))
                        }
                      />
                      <small>0 usa la configurazione automatica del miner.</small>
                    </label>
                    <label className="toggle-field">
                      <span>
                        <strong>TLS pool</strong>
                        <small>Cifra la connessione verso il pool.</small>
                      </span>
                      <input
                        type="checkbox"
                        checked={config.tls}
                        onChange={(event) =>
                          updateConfig("tls", event.target.checked)
                        }
                      />
                      <i className="toggle-ui" />
                    </label>
                    <label className="field field-wide">
                      <span>Fingerprint certificato TLS del pool</span>
                      <input
                        value={config.tls_fingerprint}
                        onChange={(event) => {
                          const fingerprint = event.target.value;
                          setConfig((current) =>
                            current
                              ? {
                                  ...current,
                                  tls_fingerprint: fingerprint,
                                  tls: fingerprint.trim() ? true : current.tls,
                                }
                              : current,
                          );
                        }}
                        placeholder="SHA-256 · 64 caratteri HEX"
                        maxLength={95}
                        autoComplete="off"
                        spellCheck={false}
                      />
                      <small>
                        Pin opzionale del certificato pool. I due punti sono
                        accettati; inserirlo abilita TLS.
                      </small>
                    </label>
                    <label className="field field-wide">
                      <span>RandomX MSR</span>
                      <select
                        value={config.msr_mode}
                        onChange={(event) =>
                          updateConfig(
                            "msr_mode",
                            event.target.value as MinerConfig["msr_mode"],
                          )
                        }
                      >
                        <option value="auto">Automatico · preset XMRig</option>
                        <option value="off">Disattivato</option>
                        <option value="fast">Fast · preset XMRig</option>
                        {config.msr_mode === "custom" && (
                          <option value="custom">Personalizzato · preservato</option>
                        )}
                      </select>
                      <small>
                        Automatico, Fast e Personalizzato usano XMRig finché
                        il motore nativo non integra MSR. Richiedono accesso
                        MSR; `rdmsr` resta attivo per ripristinare i valori
                        originali quando il miner termina.
                      </small>
                    </label>
                    <label className="field">
                      <span>Porta API</span>
                      <input
                        type="number"
                        min="1024"
                        max="65535"
                        value={config.api_port}
                        onChange={(event) =>
                          updateConfig("api_port", Number(event.target.value))
                        }
                      />
                      <small>Binding limitato a 127.0.0.1.</small>
                    </label>
                    <div className="token-status">
                      <ShieldCheck size={16} />
                      <span>
                        Token API generato e conservato in config.json.
                      </span>
                      <b>NON ESPOSTO</b>
                    </div>
                    <div className="form-actions">
                      <button
                        className="button button-start"
                        type="submit"
                        disabled={busy}
                      >
                        <Save size={15} />
                        Salva in config.json
                      </button>
                      <span>
                        Ferma il miner prima di modificare la configurazione.
                      </span>
                    </div>
                  </form>
                ) : (
                  <div className="loading-state">
                    <RefreshCw size={17} /> Connessione al bridge locale…
                  </div>
                )}
              </section>
              <div className="guard-panel">
                <ShieldCheck size={18} />
                <div>
                  <div className="eyebrow">PROTEZIONE LOCALE</div>
                  <h3>Nessuna configurazione duplicata</h3>
                  <p>
                    Il backend e XMRig leggono lo stesso config.json. La
                    dashboard non crea profili, copie o file di configurazione
                    temporanei.
                  </p>
                  <span>
                    API LOCALE · TOKEN NON ESPOSTO · CONFIGURAZIONE UNICA
                  </span>
                </div>
              </div>
              <footer className="page-footer">
                <span>UNIQUE RX · CONFIGURAZIONE</span>
                <span>
                  NESSUNA OTTIMIZZAZIONE AUTOMATICA <ShieldCheck size={13} />
                </span>
              </footer>
            </>
          )}
        </div>
      </main>
    </div>
  );
}

export default App;
