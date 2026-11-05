# XMRig Control Center

Desktop control panel for the **official XMRig executable**. It does not implement, fork, or replace RandomX. It writes an XMRig JSON configuration, starts/stops the miner, reads the local HTTP API, and stores named configuration profiles.

## Included
- Pool, wallet, worker, password and TLS settings
- Local restricted HTTP API configuration
- Start/stop process control and live hashrate/share/uptime display
- Persistent configuration profiles
- CPU threads, CPU priority, CPU affinity mask, RandomX mode and huge-page setting
- Guided CPU-thread starting point (not an automatic benchmark claim)
- PowerShell shell control for Configure / Start / Stop / Status
- Optional OpenCL and CUDA backend toggles; actual use depends on the XMRig build, GPU, drivers and runtime
- Log viewer
- Optional AI Qween advisor button when the sibling AI Qween package is present

## Requirements
- Windows 10/11 x64
- Python 3.10+ (Tkinter included in standard Windows Python installer)
- Official XMRig release

## Setup
1. Run `download_xmrig.ps1` in PowerShell. It fetches the latest Windows x64 release from the official XMRig GitHub releases API.
2. Keep the repository `packages/base` and `packages/aiqween` folders together if you want the optional AI Qween advisor.
3. Run `python app.py`.
4. Enter your pool and wallet, save configuration, then start mining.
5. The AI Qween button reads the local XMRig summary and provides manual recommendations only. It does not apply thread changes or control the miner automatically.

Alternatively, use **Seleziona XMRig** to choose an existing `xmrig.exe`.\n\n## Shell controls (PowerShell)\nRun from this folder:\n\n```powershell\n.\\xmrig-shell.ps1 -Action Configure -Pool pool.example:3333 -Wallet YOUR_WALLET -Worker rig01 -Threads 7 -CpuPriority 2 -CpuAffinity 0x7F -RandomXMode auto -Tls\n.\\xmrig-shell.ps1 -Action Start\n.\\xmrig-shell.ps1 -Action Status\n.\\xmrig-shell.ps1 -Action Stop\n```\n\n`-CpuAffinity` is a hexadecimal CPU bit mask; omit it for automatic affinity. GPU toggles only request OpenCL/CUDA from the selected XMRig build; they do not install drivers or guarantee device support.

## Safety and scope
The control API binds to `127.0.0.1` by default and uses a token. Keep it local; do not expose the API port to the public internet. The application does not fabricate shares, hashes, earnings, or pool acceptance. Share counts and hashrate are read from the actual XMRig API.

This integration package is an independently written controller layer. NyxRX, Gupax, XMRigCC and other repositories were examined as feature references; their source code is not copied into this package. XMRig remains a separate upstream component with its own license and notices: https://github.com/xmrig/xmrig

## Current limitations
- Temperature and power sensors are not yet wired into the GUI.
- Thread tuning is a guided starting preset, not a measured automatic optimizer.
- Remote multi-rig administration, notifications and Prometheus export are not included in this initial controller.
