import { useEffect, useRef, useState } from "react";
import DataRow from "../components/DataRow";
import MetricCard from "../components/MetricCard";
import { request } from "../services/api";
import { formatCount, formatHashrate, formatUptime } from "../utils/format";
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
  Line,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

type Page = "overview" | "settings";
type Status = {
  state: "ATTIVO" | "AVVIO" | "RICONNESSIONE" | "OFFLINE";
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
    stale: number | null;
    pending: number | null;
    expired: number | null;
    dropped: number | null;
  };
  uptime_seconds: number | null;
  algorithm: string | null;
  job_id: string | null;
  height: number | null;
  threads: number | null;
  cpu: {
    vendor: string;
    brand: string;
    family: number;
    model: number;
    stepping: number;
    logical_threads: number;
    physical_cores: number;
    numa_nodes: number;
    l3_cache_bytes: number;
    features: { aes: boolean; ssse3: boolean; avx2: boolean; vaes: boolean; bmi2: boolean };
  } | null;
  msr_active: boolean | null;
  msr_status?: string | null;
  workers?: WorkerStatus[];
  best?: BestStatus | null;
  job_difficulty?: number | null;
  pool_latency_ms: number | null;
  reconnects: number | null;
  large_pages_requested: boolean | null;
  large_pages_active: boolean | null;
  system: { cpu_percent: number | null; process_cpu_percent: number | null; cpu_frequency_mhz: number | null; memory_used_bytes: number | null; process_memory_bytes: number | null; temperature_c: number | null; package_power_watts: number | null; on_battery: boolean | null } | null;
  full_memory: boolean | null;
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
  huge_pages: boolean;
  jit_huge_pages: boolean;
  prefetch_mode: number;
  tls: boolean;
  tls_fingerprint: string;
  msr_mode: "auto" | "off" | "fast" | "custom";
  api_port: number;
};
type ChartPoint = { at: number; hashrate: number | null; hashrate60: number | null; hashrate15m: number | null };
type SystemChartPoint = { at: number; system: number; miner: number | null };
type WorkerStatus = { id: number; hashes: number; hashrate_hps: number; active: boolean };
type BestStatus = { estimated_hashes: number; effort_percent: number; top_difficulties: { difficulty: number; at_unix_seconds: number }[] };
type LogItem = { id: number; time: number; message: string; engine?: string };

const emptyStatus: Status = {
  state: "OFFLINE",
  managed: false,
  error: null,
  hashrate: { "10s": null, "60s": null, "15m": null },
  shares: { accepted: null, rejected: null, total: null, stale: null, pending: null, expired: null, dropped: null },
  uptime_seconds: null,
  algorithm: null,
  job_id: null,
  height: null,
  threads: null,
  cpu: null,
  msr_active: null,
  system: null,
  workers: [],
  pool_latency_ms: null,
  reconnects: null,
  large_pages_requested: null,
  large_pages_active: null,
  full_memory: null,
  pool: null,
  connected: null,
};

function App() {
  const [page, setPage] = useState<Page>("overview");
  const [status, setStatus] = useState<Status>(emptyStatus);
  const [config, setConfig] = useState<MinerConfig | null>(null);
  const [history, setHistory] = useState<ChartPoint[]>([]);
  const [systemHistory, setSystemHistory] = useState<SystemChartPoint[]>([]);
  const [logs, setLogs] = useState<LogItem[]>([]);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");
  const logCursor = useRef(0);

  useEffect(() => {
    let active = true;
    void request<{ lines: string[] }>("/logs/history?limit=400")
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
          return [...unseen, ...current].slice(-400);
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
      const statusRequest = request<Status>("/status");
      const logsRequest = request<{ items: LogItem[] }>(
        `/logs?after=${logCursor.current}`,
      );
      const [statusResult, logsResult] = await Promise.allSettled([
        statusRequest,
        logsRequest,
      ]);
      if (!active) return;

      // A transient log endpoint failure must not erase a valid miner status.
      if (statusResult.status === "fulfilled") {
        const nextStatus = statusResult.value;
        setStatus(nextStatus);
        const rate = nextStatus.hashrate["10s"];
        const rate60 = nextStatus.hashrate["60s"];
        const rate15m = nextStatus.hashrate["15m"];
        if ([rate, rate60, rate15m].some((value) => value != null && Number.isFinite(value))) {
          setHistory((current) =>
            [...current, {
              at: Date.now(),
              hashrate: rate != null && Number.isFinite(rate) ? rate : null,
              hashrate60: rate60 != null && Number.isFinite(rate60) ? rate60 : null,
              hashrate15m: rate15m != null && Number.isFinite(rate15m) ? rate15m : null,
            }].slice(-180),
          );
        }
        const systemCpu = nextStatus.system?.cpu_percent;
        if (systemCpu != null && Number.isFinite(systemCpu)) {
          const minerCpu = nextStatus.system?.process_cpu_percent ?? null;
          setSystemHistory((current) =>
            [...current, { at: Date.now(), system: systemCpu, miner: minerCpu }].slice(-180),
          );
        }
      }

      // Keep telemetry and log polling independent: either endpoint can recover
      // on its next poll without making the other one appear offline.
      if (logsResult.status === "fulfilled" && logsResult.value.items.length) {
        const nextLogs = logsResult.value.items;
        logCursor.current = Math.max(
          logCursor.current,
          ...nextLogs.map((item) => item.id),
        );
        setLogs((current) => {
          // Persistent history and the live in-memory stream overlap during
          // startup. Match their second-resolution timestamp and message so
          // the same line is not rendered twice with different IDs.
          const seen = new Set(
            current.map((item) => `${Math.floor(item.time)}:${item.message}`),
          );
          const unseen: LogItem[] = [];
          for (const item of nextLogs) {
            const key = `${Math.floor(item.time)}:${item.message}`;
            if (seen.has(key)) continue;
            seen.add(key);
            unseen.push(item);
          }
          return [...current, ...unseen].slice(-400);
        });
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
    : status.state === "RICONNESSIONE"
      ? "RICONNESSIONE POOL"
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
          {status.error && (
            <div className="notice" role="status">
              <span>{status.error}</span>
            </div>
          )}
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
                    disabled={busy || active || status.managed}
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
                  label="SHARE STALE"
                  value={formatCount(status.shares.stale)}
                  icon={<RefreshCw size={16} />}
                  tone="orange"
                  detail="job non più valido"
                />
                <MetricCard
                  label="SHARE IN ATTESA"
                  value={formatCount(status.shares.pending)}
                  icon={<Radio size={16} />}
                  tone="blue"
                  detail="risposta pool pendente"
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
                    <i /> Medie 10s · 60s · 15m <span>ULTIMI CAMPIONI</span>
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
                          formatter={(value, name) => [
                            formatHashrate(Number(value)),
                            name === "Media 60s" ? "Media 60s" : name === "Media 15m" ? "Media 15m" : "Media 10s",
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
                        <Line type="monotone" dataKey="hashrate60" name="Media 60s" stroke="#4385b5" strokeWidth={1.8} dot={false} isAnimationActive={false} connectNulls />
                        <Line type="monotone" dataKey="hashrate15m" name="Media 15m" stroke="#d28b42" strokeWidth={1.8} dot={false} isAnimationActive={false} connectNulls />
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
              <section className="chart-section">
                <div className="section-heading">
                  <div>
                    <div className="eyebrow">TELEMETRIA NYXRX · CAMPIONI REALI</div>
                    <h2>Carico CPU sistema e miner</h2>
                  </div>
                  <div className="chart-legend">
                    <i /> Sistema <span>PROCESSO MINER</span>
                  </div>
                </div>
                <div className="chart-frame">
                  {systemHistory.length > 1 ? (
                    <ResponsiveContainer width="100%" height="100%">
                      <AreaChart data={systemHistory} margin={{ top: 12, right: 12, bottom: 0, left: 0 }}>
                        <CartesianGrid stroke="#e1e9e5" vertical={false} />
                        <XAxis dataKey="at" tickFormatter={(value: number) => new Date(value).toLocaleTimeString("it-IT", { hour: "2-digit", minute: "2-digit", second: "2-digit" })} tickLine={false} axisLine={false} tick={{ fill: "#87938e", fontSize: 11 }} minTickGap={45} />
                        <YAxis domain={[0, 100]} unit="%" tickLine={false} axisLine={false} width={52} tick={{ fill: "#87938e", fontSize: 11 }} />
                        <Tooltip labelFormatter={(value) => new Date(Number(value)).toLocaleTimeString("it-IT")} formatter={(value, name) => [`${Number(value).toFixed(1)}%`, name === "system" ? "CPU sistema" : "CPU miner"]} />
                        <Area type="monotone" dataKey="system" name="system" stroke="#6e9d2b" strokeWidth={2} fill="#b8e86c" fillOpacity={0.16} dot={false} isAnimationActive={false} />
                        <Area type="monotone" dataKey="miner" name="miner" stroke="#4385b5" strokeWidth={2} fill="#4385b5" fillOpacity={0.10} dot={false} isAnimationActive={false} connectNulls />
                      </AreaChart>
                    </ResponsiveContainer>
                  ) : (
                    <div className="chart-empty">
                      <div className="empty-glyph"><Activity size={20} /></div>
                      <strong>Telemetria in attesa</strong>
                      <span>Il grafico compare quando il sistema operativo fornisce campioni CPU reali.</span>
                    </div>
                  )}
                </div>
                <div className="chart-foot">
                  <span>Campioni CPU forniti dal sistema operativo · nessun valore simulato</span>
                  <span>{systemHistory.length} PUNTI</span>
                </div>
              </section>
              <section className="chart-section">
                <div className="section-heading">
                  <div>
                    <div className="eyebrow">NYXRX · METRICHE PER THREAD</div>
                    <h2>Worker RandomX</h2>
                  </div>
                  <div className="chart-legend">
                    {status.workers?.filter((worker) => worker.active).length ?? 0} ATTIVI
                  </div>
                </div>
                <div className="worker-list">
                  {status.workers?.length ? status.workers.map((worker) => (
                    <DataRow
                      key={worker.id}
                      icon={<Cpu size={15} />}
                      label={\`Worker #\${worker.id + 1} · \${worker.active ? "attivo" : "fermo"}\`}
                      value={\`\${formatHashrate(worker.hashrate_hps)} · \${formatCount(worker.hashes)} hash\`}
                    />
                  )) : (
                    <div className="chart-empty">
                      <strong>Worker in attesa</strong>
                      <span>Le metriche compaiono quando il motore avvia i thread di hashing.</span>
                    </div>
                  )}
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
                      label="Thread motore"
                      value={
                        status.threads != null
                          ? String(status.threads)
                          : config?.threads
                            ? String(config.threads)
                            : config
                              ? "Automatico"
                              : "—"
                      }
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="Processore"
                      value={status.cpu?.brand || status.cpu?.vendor || "Non rilevato"}
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="Famiglia / modello"
                      value={status.cpu ? `${status.cpu.family} / ${status.cpu.model} · step ${status.cpu.stepping}` : "—"}
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="Core fisici / logici"
                      value={status.cpu ? `${status.cpu.physical_cores} / ${status.cpu.logical_threads}` : "—"}
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="Feature CPU"
                      value={status.cpu ? Object.entries(status.cpu.features).filter(([, enabled]) => enabled).map(([name]) => name.toUpperCase()).join(" · ") || "Base" : "—"}
                    />
                    <DataRow
                      icon={<Cpu size={15} />}
                      label="NUMA / cache L3"
                      value={status.cpu ? `${status.cpu.numa_nodes > 0 ? `${status.cpu.numa_nodes} nodi` : "NUMA n/d"} · ${status.cpu.l3_cache_bytes > 0 ? `${Math.round(status.cpu.l3_cache_bytes / 1048576)} MB L3` : "L3 n/d"}` : "—"}
                    />
                    <DataRow icon={<Activity size={15} />} label="CPU sistema / miner" value={status.system?.cpu_percent != null ? `${status.system.cpu_percent.toFixed(1)}% / ${status.system.process_cpu_percent != null ? status.system.process_cpu_percent.toFixed(1) : "n/d"}%` : "Non disponibile"} />
                    <DataRow icon={<Gauge size={15} />} label="Frequenza CPU" value={status.system?.cpu_frequency_mhz != null ? `${formatCount(status.system.cpu_frequency_mhz)} MHz` : "Non disponibile"} />
                    <DataRow icon={<Cpu size={15} />} label="RAM sistema / processo" value={status.system?.memory_used_bytes != null ? `${(status.system.memory_used_bytes / 1073741824).toFixed(2)} / ${status.system.process_memory_bytes != null ? (status.system.process_memory_bytes / 1073741824).toFixed(2) : "n/d"} GB` : "Non disponibile"} />
                    <DataRow icon={<Activity size={15} />} label="Temperatura sensore" value={status.system?.temperature_c != null ? `${status.system.temperature_c.toFixed(1)} °C` : "Non disponibile"} />
                    <DataRow icon={<Gauge size={15} />} label="Difficoltà job" value={status.job_difficulty == null ? "Non disponibile" : formatCount(status.job_difficulty)} />
                    <DataRow icon={<ShieldCheck size={15} />} label="Miglior share" value={status.best?.top_difficulties?.[0] ? status.best.top_difficulties[0].difficulty.toLocaleString("it-IT", { maximumFractionDigits: 2 }) : "In attesa"} />
                    {status.best?.top_difficulties?.map((entry, index) => (
                      <DataRow
                        key={`best-share-${index}`}
                        icon={<ShieldCheck size={15} />}
                        label={`Share #${index + 1}`}
                        value={`${entry.difficulty.toLocaleString("it-IT", { maximumFractionDigits: 2 })} · ${new Date(entry.at_unix_seconds * 1000).toLocaleString("it-IT")}`}
                      />
                    ))}
                    <DataRow icon={<Gauge size={15} />} label="Effort stimato" value={status.best ? status.best.effort_percent.toFixed(2) + "%" : "In attesa"} />
                    <DataRow icon={<Activity size={15} />} label="Lavoro accettato stimato" value={status.best ? formatCount(status.best.estimated_hashes) + " hash" : "In attesa"} />
                    <DataRow icon={<Cpu size={15} />} label="Huge Pages" value={status.large_pages_active == null ? "Non disponibile" : status.large_pages_active ? "Attive" : status.large_pages_requested ? "Non disponibili · fallback" : "Disattivate"} />
                    <DataRow icon={<Gauge size={15} />} label="Latenza share" value={status.pool_latency_ms == null ? "In attesa" : `${formatCount(status.pool_latency_ms)} ms`} />
                    <DataRow icon={<RefreshCw size={15} />} label="Riconnessioni pool" value={status.reconnects == null ? "Non disponibile" : formatCount(status.reconnects)} />
                    <DataRow icon={<Activity size={15} />} label="Alimentazione" value={status.system?.on_battery == null ? "Non disponibile" : status.system.on_battery ? "Batteria" : "Rete elettrica"} />
                    <DataRow icon={<Gauge size={15} />} label="Potenza package" value={status.system?.package_power_watts != null ? `${status.system.package_power_watts.toFixed(1)} W` : "Non disponibile"} />
                    <DataRow
                      icon={<Zap size={15} />}
                      label="Algoritmo"
                      value={status.algorithm || "—"}
                    />
                    <DataRow
                      icon={<Activity size={15} />}
                      label="Job attivo"
                      value={status.job_id || "—"}
                    />
                    <DataRow
                      icon={<Gauge size={15} />}
                      label="Altezza blocco"
                      value={status.height != null ? formatCount(status.height) : "—"}
                    />
                    <DataRow
                      icon={<LockKeyhole size={15} />}
                      label="Ottimizzazione MSR"
                      value={status.msr_status || (status.msr_active === true ? "Attiva" : status.msr_active === false ? "Non attiva" : "Non disponibile")}
                    />
                    <DataRow
                      icon={<LockKeyhole size={15} />}
                      label="Dataset RandomX"
                      value={
                        status.full_memory === true
                          ? "Completo"
                          : status.full_memory === false
                            ? "Light"
                            : "—"
                      }
                    />
                    <DataRow
                      icon={<RefreshCw size={15} />}
                      label="Share scadute"
                      value={formatCount(status.shares.expired)}
                    />
                    <DataRow
                      icon={<CircleHelp size={15} />}
                      label="Share perse"
                      value={formatCount(status.shares.dropped)}
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
                        .slice(-400)
                        .reverse()
                        .map((entry) => (
                          <div className="log-line" key={entry.id}>
                            <time>
                              {new Date(entry.time * 1000).toLocaleTimeString(
                                "it-IT",
                              )}
                            </time>
                            <span>{entry.engine ? `[${entry.engine}] ` : ""}{entry.message}</span>
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
                        <strong>Huge pages RandomX</strong>
                        <small>Abilita le pagine grandi per cache, dataset e scratchpad; fallback automatico se non disponibili.</small>
                      </span>
                      <input
                        type="checkbox"
                        checked={config.huge_pages}
                        onChange={(event) => updateConfig("huge_pages", event.target.checked)}
                      />
                      <i className="toggle-ui" />
                    </label>
                    <label className="toggle-field">
                      <span>
                        <strong>Huge pages JIT</strong>
                        <small>Richiede privilegi del sistema; il motore ripiega sulle pagine standard se necessario.</small>
                      </span>
                      <input
                        type="checkbox"
                        checked={config.jit_huge_pages}
                        onChange={(event) => updateConfig("jit_huge_pages", event.target.checked)}
                      />
                      <i className="toggle-ui" />
                    </label>
                    <label className="field">
                      <span>Prefetch JIT x86</span>
                      <select
                        value={config.prefetch_mode}
                        onChange={(event) => updateConfig("prefetch_mode", Number(event.target.value))}
                      >
                        <option value={0}>Disattivato</option>
                        <option value={1}>T0 · predefinito</option>
                        <option value={2}>NTA</option>
                        <option value={3}>Demand load</option>
                      </select>
                      <small>La modalità più veloce dipende dalla CPU: confronta con benchmark.</small>
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
                        <option value="auto">Automatico · preset CPU supportato</option>
                        <option value="off">Disattivato</option>
                        <option value="fast">Fast · preset CPU nativo</option>
                        {config.msr_mode === "custom" && (
                          <option value="custom">Personalizzato · preservato</option>
                        )}
                      </select>
                      <small>
                        Automatico e Fast usano il preset CPU nativo
                        supportato dal rilevamento hardware; Fast è un alias
                        compatibile e non applica un profilo diverso.
                        Personalizzato usa il backend XMRig. CPU non supportate
                        o permessi MSR mancanti non bloccano il mining nativo.
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
