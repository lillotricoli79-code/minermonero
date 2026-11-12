# UNIQUE RX

**Advanced native Monero RandomX CPU miner**

UNIQUE RX is a native C++20 mining engine for Monero (XMR), built around the RandomX `rx/0` proof-of-work algorithm. It combines hardware-aware CPU scheduling, configurable RandomX memory modes, Stratum connectivity and an optional local monitoring dashboard.

## Features

- **Advanced CPU thread management:** detects available logical processors and physical cores, reads the available L3 cache and respects the CPU resources visible to the process.
- **Automatic worker selection:** chooses a conservative worker count using physical-core topology and an L3-cache budget; an explicit worker count can be configured when manual tuning is preferred.
- **Live thread control:** adjust the number of active mining workers while the engine is running, or restore the automatically selected count.
- **CPU affinity and worker telemetry:** supports CPU affinity settings and reports per-worker hashrate and activity.
- **RandomX execution modes:** JIT and interpreter benchmark modes, with configurable full-memory and light-memory operation.
- **Memory optimizations:** optional huge pages and JIT huge pages, with standard-page fallback when large pages are unavailable.
- **CPU feature detection:** uses the RandomX runtime's available CPU optimizations and reports detected hardware information.
- **Stratum connectivity:** pool connection, optional TLS and certificate-fingerprint verification, reconnect handling and share submission.
- **Mining statistics:** hashrate, worker activity, accepted/rejected shares, uptime and pool connection status.
- **Local dashboard:** optional web interface served by a Python backend bound to `127.0.0.1`.

**UNIQUE RX is a CPU miner. GPU mining is not supported.**

## Efficient CPU operation

UNIQUE RX is designed to use CPU resources deliberately rather than automatically occupying every logical processor. Its automatic profile considers physical-core availability, shared L3 cache and the CPU resources visible to the process. This helps avoid unnecessarily oversized worker configurations and can reduce wasted CPU activity and energy use compared with poorly tuned settings.

Actual power consumption and efficiency depend on the processor, memory, cooling, operating system, power limits and selected worker count. No fixed wattage reduction is guaranteed. Use the built-in benchmark and your system's power measurements to compare settings on the same machine.

## Compatibility

| Operating system | Architecture |
|---|---|
| Windows | x86-64 |
| Linux | x86-64 |

Available optimizations depend on the processor, operating system, memory and system permissions.

## Download

Prebuilt packages are published by the **Native miner engine validation** workflow.

1. Open [GitHub Actions](https://github.com/lillotricoli79-code/unique-rx-miner-randomx-/actions).
2. Select a successful workflow run.
3. Download the package for your operating system:
   - `unique-rx-windows-x64`
   - `unique-rx-linux-x64`
4. Extract the complete package and keep its files and folders together.

Do not run the executable alone from inside an incomplete archive. The package includes supporting runtime files, the backend, dashboard assets, launch scripts and license notices.

## Quick start

### Windows

1. Extract the complete Windows package to a folder where you have write permission.
2. Copy `config.example.json` to `config.json` in the package's main folder.
3. Open `config.json` in a text editor.
4. Set the pool address in `pools[0].url`, your Monero wallet in `pools[0].user`, and the pool password in `pools[0].pass`.
5. Set `pools[0].tls` according to the pool's requirements. If fingerprint verification is required, enter the correct value in `pools[0].tls-fingerprint`.
6. Save the file and run `scripts\windows\start.bat`.

The launcher starts the local backend, makes the dashboard available at `http://127.0.0.1:8000`, opens it in the default browser and requests miner startup. To stop the application, run `scripts\windows\stop.bat`.

### Linux

1. Extract the complete Linux package.
2. Copy `config.example.json` to `config.json`.
3. Edit the pool address, wallet and password in `config.json`.
4. From the package directory, make the launch scripts executable and start the application:

```bash
chmod +x start.sh stop.sh diagnose.sh
./start.sh
```

Stop the application with:

```bash
./stop.sh
```

## Configuration

The miner reads settings from `config.json`. Start from `config.example.json` and keep the JSON syntax valid.

### Pool

Configure an enabled pool entry:

- `enabled`: enables or disables the pool entry.
- `url`: pool hostname and port, in `host:port` format.
- `user`: your Monero wallet address or pool username.
- `pass`: pool password required by the pool.
- `rig-id`: optional worker identifier.
- `tls`: enables encrypted TLS transport when required.
- `tls-fingerprint`: certificate fingerprint when fingerprint verification is used.

Use the exact connection details supplied by your pool. A successful TCP/TLS connection alone does not confirm that valid shares are being accepted.

### CPU workers and memory

- `cpu.enabled`: enables CPU mining.
- `cpu.rx.threads`: worker count; `0` selects the automatic profile.
- `cpu.rx.affinity`: optional CPU affinity selection.
- `cpu.max-threads-hint`: utilization hint used by automatic worker selection.
- `cpu.huge-pages`: requests large memory pages where supported.
- `cpu.huge-pages-jit`: requests large pages for JIT memory where supported.
- `cpu.priority`: process/worker priority setting.
- `randomx.mode`: selects automatic, full-memory or light-memory operation where supported.
- `randomx.rdmsr` and `randomx.wrmsr`: configure the supported MSR read/write options.

The automatic worker profile starts from the available physical-core count and applies a conservative limit based on detected shared L3 cache. If topology information is unavailable, the engine uses the available logical processor count as a fallback. An explicit worker count takes precedence over automatic selection.

Large-page allocation can require operating-system privileges and sufficient contiguous memory. If large pages cannot be allocated, the engine can fall back to standard pages.

## Runtime controls

When the native engine is running interactively, its command interface includes:

- `threads N`: set the active worker count.
- `threads auto`: restore the automatically selected worker count.
- `workers`: show worker hashrates and counters.
- `cpu`: show detected CPU, core, thread and hardware information.
- `set prefetch-mode N`: change the supported JIT prefetch mode.
- `set cpu.priority N`: change worker priority.
- `set huge-pages on|off`: change the huge-page setting; restart may be required.

The dashboard also exposes the runtime controls supported by the backend. A manual worker-count change is a performance choice: test different values instead of assuming that using every logical processor is always optimal.

## Benchmark

Benchmark mode runs locally. It does not connect to a pool or submit shares.

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

Use the same benchmark duration and settings when comparing configurations. Record hashrate, CPU temperature and wall-power consumption under comparable conditions. Results vary with hardware, memory, cooling, operating-system settings and background activity. Benchmark hashrate is not a guarantee of mining income or pool shares.

## Dashboard and local security

The complete application package includes the native engine, local backend, dashboard files and platform launch scripts. The dashboard displays information reported by the engine and backend.

The backend binds to `127.0.0.1:8000` by default and is intended for local access. Do not expose this service directly to the public internet.

## Build from source

The repository includes the native engine, dashboard, backend and platform build scripts.

### Windows

Requirements: Windows x64, a C++20 build toolchain, CMake, Git, Node.js and Python.

```powershell
git clone --recurse-submodules https://github.com/lillotricoli79-code/unique-rx-miner-randomx-.git
cd unique-rx-miner-randomx-
scripts\windows\build.bat
```

### Linux

Requirements: a C++20 compiler, CMake, Git, OpenSSL development files, Node.js and Python.

```bash
git clone --recurse-submodules https://github.com/lillotricoli79-code/unique-rx-miner-randomx-.git
cd unique-rx-miner-randomx-
./scripts/linux/build.sh
```

The automated workflow builds and validates the Windows and Linux targets.

## Troubleshooting

- **The pool does not connect:** verify the hostname, port, wallet, password and TLS settings.
- **TLS connection errors:** check that TLS is enabled when required and that the configured certificate fingerprint is correct.
- **No accepted shares:** confirm that the miner is connected, the wallet and worker identity are correct, and the pool difficulty is appropriate. Shares are submitted only when the calculated result meets the pool target; a share may take time to find.
- **Huge pages are unavailable:** check operating-system support, permissions and available memory. Mining can continue with standard pages when the fallback succeeds.
- **The dashboard does not open:** use the complete package and its platform launcher; check the application logs.
- **The miner uses too much CPU:** reduce the worker count, try `threads auto`, and compare benchmark results and power measurements.
- **The miner has a lower hashrate than expected:** compare full-memory and light-memory modes, worker counts and memory settings under the same system conditions.

## Third-party components and licenses

UNIQUE RX includes third-party components. Their original copyright notices and applicable license texts are retained in the repository's `LICENSES/` directory and relevant upstream source directories. Review the applicable license terms before modifying or redistributing the software.

---

**Algorithm:** Monero RandomX (`rx/0`)  
**Engine:** Native C++20  
**Platforms:** Windows x64 · Linux x64
