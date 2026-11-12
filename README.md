# UNIQUE RX

**Native Monero CPU miner powered by RandomX**

UNIQUE RX is a native C++20 miner for Monero (XMR), using the RandomX `rx/0` proof-of-work algorithm. It provides Stratum pool connectivity, CPU-aware execution, configurable RandomX memory options and an optional local monitoring dashboard.

[Source code](https://github.com/lillotricoli79-code/minermonero) · [Builds](https://github.com/lillotricoli79-code/minermonero/actions)

## Features

- Native C++20 mining engine.
- Monero RandomX (`rx/0`) CPU mining.
- Stratum pool connection with optional TLS and certificate fingerprint verification.
- CPU capability detection and supported execution optimizations.
- Configurable worker count and CPU affinity.
- RandomX JIT and interpreter benchmark modes.
- Optional huge pages, JIT huge pages and MSR controls.
- Mining statistics, pool connection state and share reporting.
- Optional local web dashboard with a Python backend.

**UNIQUE RX is a CPU miner. GPU mining is not supported.**

## Compatibility

| Operating system | Architecture |
|---|---|
| Windows | x86-64 |
| Linux | x86-64 |

Performance and available optimizations depend on the processor, operating system, memory and system permissions.

## Download

Prebuilt packages are published as artifacts by the **Native miner engine validation** workflow.

1. Open [GitHub Actions](https://github.com/lillotricoli79-code/minermonero/actions).
2. Select a successful workflow run.
3. Download the package for your operating system:
   - `unique-rx-windows-x64`
   - `unique-rx-linux-x64`
4. Extract the complete archive. Keep the files and folders together.

The Windows package includes the runtime DLL files required by the executable.

## Quick start

### Windows

1. Extract the Windows package.
2. Copy `config.example.json` to `config.json`.
3. Open `config.json` and enter your pool address and Monero wallet.
4. Run `scripts\windows\start.bat`.

To stop the application, run `scripts\windows\stop.bat`.

### Linux

1. Extract the Linux package.
2. Copy `config.example.json` to `config.json`.
3. Edit the pool address and wallet in `config.json`.
4. From the package directory, run:

```bash
chmod +x start.sh stop.sh diagnose.sh
./start.sh
```

Stop the application with:

```bash
./stop.sh
```

## Configuration

The miner reads its settings from `config.json`. A sample configuration is provided as `config.example.json`.

At minimum, configure an enabled pool entry with:

- **url** — pool hostname and port.
- **user** — your Monero wallet address or pool username.
- **pass** — pool password required by the pool.
- **rig-id** — optional worker name.
- **tls** — enable TLS when required by the pool.
- **tls-fingerprint** — certificate fingerprint when fingerprint verification is used.

The example file contains the available engine settings. Change CPU and RandomX options individually and compare results using the built-in benchmark.

## Benchmark

Benchmark mode runs locally and does not connect to a pool or submit shares.

Examples:

```bash
# RandomX JIT
unique-rx --benchmark 10 1

# Full-memory JIT
unique-rx --benchmark-full 60 2

# RandomX interpreter
unique-rx --benchmark-interpreter 60 2

# Full-memory interpreter
unique-rx --benchmark-full-interpreter 60 2
```

Use consistent benchmark parameters when comparing configurations. Results vary with hardware, memory, cooling, operating-system settings and background activity. Benchmark hashrate is not a guarantee of mining income.

## Dashboard

The complete application package includes the native engine, local backend, dashboard files and platform launch scripts. The dashboard displays information reported by the miner and backend.

The backend is configured for local access on `127.0.0.1:8000`. It is not intended to be exposed directly to the public internet.

## Build from source

The repository includes the native engine, dashboard, backend and platform build scripts.

### Windows

Requirements: Windows x64, a C++20 build toolchain, CMake, Git, Node.js and Python.

```powershell
git clone --recurse-submodules https://github.com/lillotricoli79-code/minermonero.git
cd minermonero
scripts\windows\build.bat
```

### Linux

Requirements: a C++20 compiler, CMake, Git, OpenSSL development files, Node.js and Python.

```bash
git clone --recurse-submodules https://github.com/lillotricoli79-code/minermonero.git
cd minermonero
./scripts/linux/build.sh
```

The GitHub Actions workflow also builds and validates the Windows and Linux targets.

## Troubleshooting

- **The pool does not connect:** verify the pool hostname, port, wallet, password and TLS settings.
- **TLS connection errors:** check that TLS is enabled when required and that the configured certificate fingerprint is correct.
- **No accepted shares:** check the pool connection, wallet/worker identity and pool difficulty. A successful connection does not necessarily mean a share has already been found.
- **Huge pages are unavailable:** check operating-system support, permissions and available memory. Some optimizations may be unavailable on some systems.
- **Dashboard does not open:** use the complete package and its platform launcher; check the application logs.

## Third-party components and licenses

UNIQUE RX incorporates third-party software. Components retain their respective licenses and copyright notices. Relevant license texts are included in the `LICENSES/` directory, including notices for RandomX-related components, RxMiner, NyxRX, XMRig, XMR-Miner and PawnIO.

Review the license applicable to each component before modifying or redistributing the software.

---

**Algorithm:** Monero RandomX (`rx/0`)  
**Engine:** C++20  
**Platforms:** Windows x64 · Linux x64
