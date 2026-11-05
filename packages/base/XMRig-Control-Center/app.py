#!/usr/bin/env python3
"""XMRig Control Center: standalone GUI/controller for the official XMRig engine.
No mining/hash implementation is included or replaced here.
"""
from __future__ import annotations
import json, os, queue, subprocess, threading, time, urllib.request, urllib.error
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

APP = "XMRig Control Center"
ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
PROFILES = DATA / "profiles"
CONFIG = DATA / "config.json"
DEFAULTS = {
    "pool": "pool.supportxmr.com:3333", "wallet": "", "worker": "rig01",
    "password": "x", "threads": 0, "tls": False, "api_host": "127.0.0.1",
    "api_port": 18088, "api_token": "local-control-token", "xmrig_path": ""
}

class Controller:
    def __init__(self):
        self.proc = None
        self.lock = threading.RLock()
        self.events = queue.Queue()
        self.last_summary = {}
    def running(self):
        return self.proc is not None and self.proc.poll() is None
    def api(self, path="/1/summary", timeout=2):
        cfg = load_config()
        url = f"http://{cfg['api_host']}:{int(cfg['api_port'])}{path}"
        req = urllib.request.Request(url, headers={"Authorization": "Bearer " + cfg["api_token"]})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    def start(self):
        cfg = load_config()
        exe = Path(cfg["xmrig_path"]) if cfg["xmrig_path"] else ROOT / "xmrig.exe"
        if not exe.is_file():
            raise FileNotFoundError("xmrig.exe non trovato. Usa 'Seleziona XMRig' oppure lo script download_xmrig.ps1.")
        if not cfg["wallet"].strip(): raise ValueError("Inserisci il wallet prima di avviare il miner.")
        config_path = DATA / "xmrig.generated.json"
        config = {
            "autosave": True, "background": False, "colors": True,
            "http": {"enabled": True, "host": cfg["api_host"], "port": int(cfg["api_port"]), "access-token": cfg["api_token"], "restricted": True},
            "opencl": False, "cuda": False,
            "pools": [{"url": cfg["pool"], "user": cfg["wallet"], "pass": cfg["password"], "rig-id": cfg["worker"], "keepalive": True, "tls": bool(cfg["tls"]), "enabled": True}]
        }
        if int(cfg["threads"]) > 0:
            import os
            logical = max(1, os.cpu_count() or 1)
            hint = max(1, min(100, round(100 * int(cfg["threads"]) / logical)))
            config["cpu"] = {"enabled": True, "huge-pages": True, "priority": 2, "max-threads-hint": hint}
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        with self.lock:
            if self.running(): return
            self.proc = subprocess.Popen([str(exe), "-c", str(config_path)], cwd=str(exe.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            threading.Thread(target=self._read_log, daemon=True).start()
    def _read_log(self):
        p = self.proc
        if p and p.stdout:
            for line in p.stdout:
                self.events.put(("log", line.rstrip()))
        self.events.put(("exit", None))
    def stop(self):
        with self.lock:
            if self.running():
                self.proc.terminate()
                try: self.proc.wait(timeout=8)
                except subprocess.TimeoutExpired: self.proc.kill()
    def status(self):
        if not self.running(): return {"state": "FERMO"}
        try:
            s = self.api()
            h = s.get("hashrate", {}).get("total", [])
            results = s.get("results", {})
            return {"state":"ATTIVO", "h10":num(h,0), "h60":num(h,1), "h15m":num(h,2),
                    "accepted":results.get("shares_good", 0), "rejected":results.get("shares_total", 0)-results.get("shares_good",0),
                    "uptime":s.get("connection",{}).get("uptime",0), "raw":s}
        except Exception as e: return {"state":"AVVIO / API IN ATTESA", "error":str(e)}

def num(a, i):
    try: return float(a[i])
    except Exception: return 0.0

def load_config():
    DATA.mkdir(exist_ok=True); PROFILES.mkdir(exist_ok=True)
    if CONFIG.exists():
        try: return {**DEFAULTS, **json.loads(CONFIG.read_text(encoding="utf-8"))}
        except Exception: pass
    save_config(DEFAULTS); return DEFAULTS.copy()

def save_config(c):
    DATA.mkdir(exist_ok=True); CONFIG.write_text(json.dumps(c, indent=2), encoding="utf-8")

def fmt_hash(v):
    units=["H/s","KH/s","MH/s","GH/s"]
    for u in units:
        if abs(v)<1000: return f"{v:.2f} {u}"
        v/=1000
    return f"{v:.2f} TH/s"

class App(tk.Tk):
    def __init__(self):
        super().__init__(); self.title(APP); self.geometry("1050x720"); self.minsize(850,600)
        self.configure(bg="#111827"); self.ctrl=Controller(); self.vars={}
        self._style(); self._header(); self._layout(); self._load_fields(); self.after(500,self._tick)
        self.protocol("WM_DELETE_WINDOW", self._close)
    def _style(self):
        st=ttk.Style(self); st.theme_use("clam")
        st.configure("TFrame",background="#111827"); st.configure("TLabel",background="#111827",foreground="#e5e7eb",font=("Segoe UI",10))
        st.configure("Title.TLabel",font=("Segoe UI",20,"bold"),foreground="#f9fafb")
        st.configure("Card.TLabelframe",background="#1f2937",foreground="#93c5fd")
        st.configure("Card.TLabelframe.Label",background="#1f2937",foreground="#93c5fd",font=("Segoe UI",11,"bold"))
        st.configure("TButton",padding=8); st.configure("TEntry",fieldbackground="#f9fafb")
    def _header(self):
        f=ttk.Frame(self,padding=16); f.pack(fill="x")
        ttk.Label(f,text="XMRig Control Center",style="Title.TLabel").pack(side="left")
        ttk.Label(f,text="XMRig engine · profili · telemetria · controllo",foreground="#9ca3af").pack(side="right")
    def _layout(self):
        body=ttk.Frame(self,padding=(16,0,16,16)); body.pack(fill="both",expand=True)
        left=ttk.Frame(body); left.pack(side="left",fill="y",padx=(0,12))
        right=ttk.Frame(body); right.pack(side="left",fill="both",expand=True)
        conn=ttk.LabelFrame(left,text="Configurazione pool",style="Card.TLabelframe",padding=12); conn.pack(fill="x",pady=(0,10))
        fields=[("pool","Pool (host:porta)"),("wallet","Wallet"),("worker","Worker"),("password","Password"),("threads","Thread CPU (0 = automatico)"),("api_port","Porta API locale"),("api_token","Token API")]
        for key,label in fields:
            ttk.Label(conn,text=label).pack(anchor="w",pady=(5,1)); v=tk.StringVar(); self.vars[key]=v
            e=ttk.Entry(conn,textvariable=v,width=36,show="•" if key in ("wallet","api_token") else "")
            e.pack(fill="x")
        self.tls=tk.BooleanVar(); ttk.Checkbutton(conn,text="TLS pool",variable=self.tls).pack(anchor="w",pady=7)
        ttk.Button(conn,text="Seleziona XMRig…",command=self._select_exe).pack(fill="x",pady=(4,2))
        ttk.Button(conn,text="Salva configurazione",command=self._save).pack(fill="x",pady=2)
        prof=ttk.LabelFrame(left,text="Profili",style="Card.TLabelframe",padding=12); prof.pack(fill="x")
        self.profile=tk.StringVar(); self.profile_box=ttk.Combobox(prof,textvariable=self.profile,state="readonly"); self.profile_box.pack(fill="x",pady=4)
        ttk.Button(prof,text="Salva profilo",command=self._save_profile).pack(fill="x",pady=2)
        ttk.Button(prof,text="Carica profilo",command=self._load_profile).pack(fill="x",pady=2)
        stats=ttk.LabelFrame(right,text="Monitoraggio in tempo reale",style="Card.TLabelframe",padding=14); stats.pack(fill="x")
        self.state=ttk.Label(stats,text="FERMO",font=("Segoe UI",16,"bold"),foreground="#fbbf24"); self.state.pack(anchor="w")
        grid=ttk.Frame(stats); grid.pack(fill="x",pady=12)
        self.metrics={}
        for i,(k,label) in enumerate([("h10","Hashrate 10s"),("h60","Hashrate 60s"),("h15m","Hashrate 15m"),("accepted","Share accettate"),("rejected","Share rifiutate"),("uptime","Uptime (s)")]):
            card=ttk.Frame(grid,padding=10); card.grid(row=i//3,column=i%3,sticky="nsew",padx=4,pady=4); grid.columnconfigure(i%3,weight=1)
            ttk.Label(card,text=label,foreground="#9ca3af").pack(anchor="w")
            val=ttk.Label(card,text="—",font=("Segoe UI",15,"bold")); val.pack(anchor="w"); self.metrics[k]=val
        controls=ttk.Frame(right); controls.pack(fill="x",pady=10)
        ttk.Button(controls,text="▶  AVVIA MINING",command=self._start).pack(side="left",padx=(0,8))
        ttk.Button(controls,text="■  STOP",command=self._stop).pack(side="left",padx=8)
        ttk.Button(controls,text="⟳  Benchmark / autotuning guidato",command=self._tune).pack(side="left",padx=8)
        logs=ttk.LabelFrame(right,text="Log miner",style="Card.TLabelframe",padding=8); logs.pack(fill="both",expand=True)
        self.log=tk.Text(logs,bg="#030712",fg="#d1d5db",insertbackground="white",font=("Consolas",9),wrap="word",height=18)
        self.log.pack(fill="both",expand=True)
        self._refresh_profiles()
    def _load_fields(self):
        c=load_config()
        for k,v in self.vars.items(): v.set(str(c.get(k,DEFAULTS.get(k,""))))
        self.tls.set(bool(c.get("tls",False)))
    def _values(self):
        c=load_config()
        for k,v in self.vars.items(): c[k]=v.get().strip()
        for k in ("threads","api_port"):
            try:c[k]=int(c[k])
            except ValueError: raise ValueError(f"{k} deve essere numerico")
        c["tls"]=self.tls.get(); return c
    def _save(self):
        try: save_config(self._values()); self._log("Configurazione salvata.")
        except Exception as e: messagebox.showerror("Configurazione",str(e))
    def _select_exe(self):
        p=filedialog.askopenfilename(title="Seleziona xmrig.exe",filetypes=[("XMRig","xmrig.exe"),("Eseguibili","*.exe")])
        if p:
            c=self._values();c["xmrig_path"]=p;save_config(c);self._log("Motore selezionato: "+p)
    def _start(self):
        try: save_config(self._values()); self.ctrl.start(); self._log("Avvio richiesto a XMRig.")
        except Exception as e: messagebox.showerror("Avvio miner",str(e))
    def _stop(self): self.ctrl.stop(); self._log("Arresto richiesto.")
    def _save_profile(self):
        try:
            c=self._values(); name=self.profile.get().strip() or f"profilo-{int(time.time())}"
            (PROFILES/(name.replace("/","_")+".json")).write_text(json.dumps(c,indent=2),encoding="utf-8")
            self.profile.set(name);self._refresh_profiles();self._log("Profilo salvato: "+name)
        except Exception as e: messagebox.showerror("Profilo",str(e))
    def _load_profile(self):
        name=self.profile.get()
        if not name:return
        try: save_config(json.loads((PROFILES/(name+".json")).read_text(encoding="utf-8")));self._load_fields();self._log("Profilo caricato: "+name)
        except Exception as e:messagebox.showerror("Profilo",str(e))
    def _refresh_profiles(self):
        PROFILES.mkdir(exist_ok=True);self.profile_box["values"]=[p.stem for p in PROFILES.glob("*.json")]
    def _tune(self):
        # Conservative guided presets only: no blind changes to XMRig internals.
        import os
        n=os.cpu_count() or 1
        messagebox.showinfo("Autotuning guidato",f"CPU logiche rilevate: {n}\n\nProva iniziale consigliata: {max(1,n-1)} thread.\nConfronta i risultati a 10 e 60 secondi, controllando temperature e stabilità.\nIl numero viene inserito nel campo Thread: salva il profilo dopo il test.")
        self.vars["threads"].set(str(max(1,n-1)))
    def _log(self,s): self.log.insert("end",f"[{time.strftime('%H:%M:%S')}] {s}\n");self.log.see("end")
    def _tick(self):
        st=self.ctrl.status();self.state.config(text=st["state"])
        for k in ("h10","h60","h15m"):
            self.metrics[k].config(text=fmt_hash(st.get(k,0)))
        for k in ("accepted","rejected","uptime"):self.metrics[k].config(text=str(st.get(k,"—")))
        while not self.ctrl.events.empty():
            typ,msg=self.ctrl.events.get()
            if typ=="log":self._log(msg)
            elif typ=="exit":self._log("Processo XMRig terminato.")
        self.after(1200,self._tick)
    def _close(self):
        if self.ctrl.running() and messagebox.askyesno("Uscita","XMRig è attivo. Arrestarlo prima di uscire?"):self.ctrl.stop()
        self.destroy()

if __name__=="__main__":
    DATA.mkdir(exist_ok=True); PROFILES.mkdir(exist_ok=True); App().mainloop()
