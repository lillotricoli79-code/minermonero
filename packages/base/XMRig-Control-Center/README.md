# XMRig Control Center

Desktop control panel for the **official XMRig executable**. It does not implement, fork, or replace RandomX. It writes an XMRig JSON configuration, starts/stops the miner, reads the local HTTP API, and stores named configuration profiles.

## Included
- Pool, wallet, worker, password and TLS settings
- Local restricted HTTP API configuration
- Start/stop process control and live hashrate/share/uptime display
- Persistent configuration profiles
- Guided CPU-thread starting point (not an automatic benchmark claim)
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

Alternatively, use **Seleziona XMRig** to choose an existing `xmrig.exe`.

## Safety and scope
The control API binds to `127.0.0.1` by default and uses a token. Keep it local; do not expose the API port to the public internet. The application does not fabricate shares, hashes, earnings, or pool acceptance. Share counts and hashrate are read from the actual XMRig API.

This initial integration package is an independently written controller layer. NyxRX, Gupax, XMRigCC and other repositories were examined as feature references; their source code is not copied into this package. XMRig remains a separate upstream component with its own license and notices: https://github.com/xmrig/xmrig

## Current limitations
- Temperature and power sensors are not yet wired into the GUI.
- Thread tuning is a guided starting preset, not a measured automatic optimizer.
- Remote multi-rig administration, notifications and Prometheus export are not included in this initial controller.
