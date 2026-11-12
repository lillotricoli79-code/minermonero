# UNIQUE RX

**Native Monero (XMR) CPU miner · RandomX rx/0 · Windows x64 & Linux x64**

UNIQUE RX is a native C++20 CPU miner for Monero. It combines a RandomX mining engine, Stratum pool connectivity, CPU-aware execution, optional memory/MSR optimizations and a local web dashboard.

> UNIQUE RX is an independent project. It is not an official Monero or HashVault product.

[Source code](https://github.com/lillotricoli79-code/minermonero) · [Releases & builds](https://github.com/lillotricoli79-code/minermonero/actions)

---

## Contents

- [Features](#features)
- [Supported platforms](#supported-platforms)
- [Download](#download)
- [Quick start](#quick-start)
- [Pool configuration](#pool-configuration)
- [Developer fee](#developer-fee)
- [Dashboard and local API](#dashboard-and-local-api)
- [Command-line options](#command-line-options)
- [Benchmark](#benchmark)
- [Build from source](#build-from-source)
- [Logs and troubleshooting](#logs-and-troubleshooting)
- [Security and performance notes](#security-and-performance-notes)
- [Licenses and third-party components](#licenses-and-third-party-components)

## Features

- **Monero RandomX (rx/0):** CPU mining for the Monero network.
- **Native engine:** C++20 application with a JIT-capable RandomX implementation.
- **CPU detection:** detects available processor capabilities and adapts execution to the host.
- **CPU tuning:** configurable worker count, affinity, AES/AVX2 paths where supported, NUMA and memory options.
- **RandomX memory:** huge pages and JIT huge pages are configurable; unavailable privileges or system support may limit their use.
- **Optional MSR support:** read/write controls are configurable. MSR is not required to start mining.
- **Stratum connectivity:** pool endpoint, wallet, worker, password, TLS and certificate fingerprint settings.
- **Share and connection status:** reports pool connection state, accepted/rejected shares, reconnects and mining statistics provided by the engine.
- **Hashrate windows:** 10-second, 60-second and 15-minute measurements.
- **Local dashboard:** React-based interface connected to a Python/FastAPI backend and native-engine telemetry.
- **Logging:** timestamped console output and a rotating Stratum log.

UNIQUE RX is a **CPU miner**. It does not provide GPU mining.

## Supported platforms

| Platform | Architecture | Distribution |
|---|---|---|
| Windows | x86-64 | Native executable and complete package |
| Linux | x86-64 | Native executable and complete package |

The engine detects CPU features at runtime. Available optimizations depend on the processor, operating system, memory configuration and permissions. No single thread count or optimization profile is optimal for every CPU.

## Download

Open the repository's **Actions** page and select a successful run of **Native miner engine validation**:

- `unique-rx-windows-x64` — Windows executable and complete application package.
- `unique-rx-linux-x64` — Linux executable and complete application package.

Each package includes the corresponding source archive, third-party license notices and SHA-256 checksums. On Windows, keep the OpenSSL runtime DLLs in the same directory as `unique-rx.exe`.

[Browse GitHub Actions artifacts](https://github.com/lillotricoli79-code/minermonero/actions)

## Quick start

### Windows

1. Download and extract the complete `unique-rx-windows-x64` package.
2. Copy `config.example.json` to `config.json`.
3. Edit `config.json`: set your Monero wallet and pool address.
4. Keep `unique-rx.exe`, the OpenSSL DLLs, `config.json`, the backend, dashboard and scripts in the extracted package structure.
5. Run `scripts\windows\start.bat`.

The launcher starts the local backend and the miner as a coordinated application. It checks for the required package files and does not silently start a disconnected standalone miner.

### Linux

1. Download and extract the complete `unique-rx-linux-x64` package.
2. Copy `config.example.json` to `config.json`.
3. Edit `config.json`: set your Monero wallet and pool address.
4. From the extracted package directory, run:

```bash
chmod +x start.sh stop.sh diagnose.sh
./start.sh
```

The Linux launcher creates a local Python virtual environment when needed and installs the backend dependencies listed in `requirements.txt`.

### Stop and diagnostics

- Windows: use `scripts\windows\stop.bat` and `scripts\windows\diagnose.bat`.
- Linux: use `./stop.sh` and `./diagnose.sh`.

Stop through the supplied launcher/backend so the engine can shut down cleanly and perform its configured cleanup.

## Pool configuration

The main configuration file is `config.json`, located beside the packaged application. Start from `config.example.json`; do not edit the example as a substitute for your own configuration.

A pool entry has this structure:

```json
{
  "enabled": true,
  "url": "pool.example.org:3333",
  "user": "YOUR_MONERO_WALLET_ADDRESS",
  "pass": "x",
  "rig-id": "unique-rx-worker",
  "tls": false,
  "keepalive": true,
  "nicehash": false,
  "tls-fingerprint": null
}
```

| Setting | Description |
|---|---|
| `url` | Pool hostname and port, in `host:port` form |
| `user` | Your Monero wallet address or pool username |
| `pass` | Pool password, commonly `x` |
| `rig-id` | Optional worker/rig identifier |
| `tls` | Enables TLS for the pool connection |
| `tls-fingerprint` | Optional SHA-256 certificate fingerprint; configure it with TLS enabled |
| `keepalive` | Enables the configured keepalive behavior |
| `enabled` | Enables or disables this pool entry |

Configure at least one enabled pool with your own wallet. The example hostname and wallet are placeholders and cannot be used for mining.

### CPU and RandomX settings

The example configuration includes the main CPU and RandomX options:

- `cpu.rx.threads`: `0` lets the engine choose a worker count; a positive value specifies the thread count.
- `cpu.rx.affinity`: CPU affinity setting.
- `cpu.huge-pages`: enables the RandomX huge-page option.
- `cpu.huge-pages-jit`: optional JIT memory huge pages.
- `cpu.hw-aes` and `cpu.asm`: hardware/assembly selection; automatic detection is recommended unless you know the target CPU.
- `randomx.mode`: RandomX execution mode.
- `randomx.numa`: NUMA-aware behavior where supported.
- `randomx.rdmsr` / `randomx.wrmsr`: optional MSR read/write controls.
- `randomx.scratchpad_prefetch_mode`: JIT scratchpad prefetch mode (0–3).

Change one performance setting at a time and benchmark on the machine that will run the miner.

## Developer fee

The configuration option `donate-level` is a root-level integer percentage from **0 to 100**. The example configuration sets it to **1**.

When the developer-fee phase is active, the engine uses a dedicated HashVault TLS endpoint and the developer wallet configured in the native application. This is separate from the pool and wallet you configure for your own mining. Set the percentage deliberately and review the source code if you need to inspect the implementation.

## Dashboard and local API

The complete package includes:

- the native mining engine;
- the local Python/FastAPI backend;
- the built React dashboard;
- platform-specific start, stop and diagnostic scripts.

The backend binds to **127.0.0.1:8000**. The dashboard communicates with this local backend, which reads telemetry from the native engine. The service is intended for local access; it is not configured as a public internet service.

The optional XMRig-compatible `http` configuration section is separate from the dashboard backend. Do not confuse its configured port with the backend port `8000`.

## Command-line options

Run `unique-rx --help` to display the built-in usage text.

| Option | Description |
|---|---|
| `-c, --config FILE` | Select a configuration file (default: `config.json`) |
| `-o, --url HOST:PORT` | Override the pool URL |
| `-u, --user WALLET` | Override the pool username/wallet |
| `-p, --pass PASSWORD` | Override the pool password |
| `--rig-id WORKER` | Set the worker identifier |
| `--tls` | Enable TLS for the selected pool |
| `--tls-fingerprint SHA256` | Set the TLS certificate fingerprint |
| `--keepalive` | Enable keepalive |
| `-t, --threads N` | Set the worker count (1–256) |
| `--max-hashes N` | Stop after the specified number of hashes |
| `-h, --help` | Show command-line help |

With no arguments, the miner loads `config.json` from the current working directory. The supplied platform launcher is recommended for the complete dashboard/backend package.

## Benchmark

The benchmark runs locally and does not require a pool or submit shares.

Examples:

```bash
# JIT benchmark: 10 seconds, one lane
unique-rx --benchmark 10 1

# Full-memory JIT benchmark
unique-rx --benchmark-full 60 2

# Interpreter benchmark
unique-rx --benchmark-interpreter 60 2

# Full-memory interpreter benchmark
unique-rx --benchmark-full-interpreter 60 2
```

The optional trailing arguments are `seed`, `input`, `prefetch-mode` (0–3) and `jit-huge-pages` (0 or 1). Use the same seed, input, duration and lane count when comparing configurations. Results depend on CPU, memory, cooling, operating-system settings and background load; benchmark results are not a promise of pool earnings.

## Build from source

### Requirements

- CMake 3.24 or newer.
- A C and C++ compiler with C++20 support.
- OpenSSL development headers and libraries.
- Git with submodule support.
- Node.js 22 and npm (dashboard build).
- Python 3.12 (backend tests); runtime dependencies are listed in `requirements.txt`.

### Linux

Install the compiler toolchain, CMake, Ninja and OpenSSL development package for your distribution, then run:

```bash
git clone --recurse-submodules https://github.com/lillotricoli79-code/minermonero.git
cd minermonero

python3 -m pip install -r requirements.txt
python3 -m unittest backend.tests.test_main

npm ci --prefix dashboard
npm run build --prefix dashboard

cmake -S engine -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The native executable is produced as `build/unique-rx`.

### Windows

Use a Windows x64 environment with Visual Studio 2022 C++ tools, CMake, Git, Node.js 22, Python 3.12 and OpenSSL development files. Then run the repository's Windows build script:

```powershell
git clone --recurse-submodules https://github.com/lillotricoli79-code/minermonero.git
cd minermonero
scripts\windows\build.bat
```

The GitHub Actions workflow **Native miner engine validation** builds and tests Windows and Linux packages independently.

## Logs and troubleshooting

- **Missing `config.json`:** copy `config.example.json` to `config.json` and enter a valid wallet and pool.
- **Pool connection fails:** check the hostname, port, firewall, wallet, pool password and whether TLS is required.
- **TLS handshake fails:** confirm that TLS is enabled and that the configured SHA-256 fingerprint matches the pool certificate.
- **No accepted shares yet:** wait for a valid share and check the pool difficulty, connection state and worker address. A connected miner is not proof that a share has been accepted.
- **Huge pages unavailable:** check operating-system permissions and memory availability. The miner can run with reduced memory optimizations.
- **MSR unavailable:** check CPU support, driver and permissions. MSR is optional; do not install an untrusted driver.
- **Dashboard unavailable:** use the complete platform package and its start script; check the files under `logs/` and confirm that the local backend is listening on `127.0.0.1:8000`.

The engine writes its Stratum log to `unique-rx-stratum.log` beside the configuration. The launchers also maintain backend logs under `logs/`.

## Security and performance notes

- Never publish your wallet credentials, access tokens or private configuration files.
- Do not expose the local dashboard/backend port to the public internet without implementing and configuring appropriate authentication and network protection.
- Run with elevated privileges only when required by the selected memory or hardware settings.
- MSR writes are optional and hardware-specific. Incorrect register settings can affect system stability; use only supported settings.
- Mining performance and profitability vary with hardware, pool difficulty, network conditions, electricity costs and XMR price. No earnings are guaranteed.

## Licenses and third-party components

UNIQUE RX contains components and adaptations with different license obligations. It is **not accurate to treat every file in the repository as being under one single permissive license**.

Review the license files distributed with the source and release packages, including:

- RandomX upstream — BSD 3-Clause.
- RxMiner-derived components — MIT; see `LICENSES/RXMINER-MIT.txt`.
- NyxRX-derived components — MIT; see `LICENSES/NYXRX-MIT.txt`.
- XMRig-derived components — GPL-3.0; see `LICENSES/XMRIG-GPL-3.0.txt`.
- XMR-Miner license notice — see `LICENSES/XMR-MINER-GPL-3.0.txt`.
- PawnIO module licensing information — see `LICENSES/PAWNIO-MODULES-COPYING`.

The release package includes the corresponding source archive and license notices. Preserve copyright notices and comply with the applicable license terms when redistributing binaries or modified source.

---

**Project:** [lillotricoli79-code/minermonero](https://github.com/lillotricoli79-code/minermonero)  
**Algorithm:** Monero RandomX `rx/0` · **Engine:** C++20
