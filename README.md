# UNIQUE RX

Nuovo miner RandomX con componenti selezionati e verificati singolarmente. Non
è un fork di un miner esistente né un collage di motori: nessuna ottimizzazione
entra nel progetto senza compatibilità, correttezza e un risultato misurabile.

## Stato iniziale

Il repository iniziale conteneva solo archivi di un controller desktop per
XMRig e di un modulo AI. Quegli archivi non sono il motore UNIQUE RX e non
vengono usati come sorgente del miner.

## Architettura obiettivo

- **Rilevamento hardware:** CPU, AVX2, AES-NI, RAM e disponibilità di pagine
  grandi, con un percorso compatibile quando una feature non è presente.
- **Selezione del motore:** JIT standard di riferimento oppure una variante
  multi-lane solo se supportata e verificata sulla macchina.
- **Percorsi di hashing:** standard e AVX2 con 2, 3 o 4 lane. Il numero di lane
  migliore si misura; non si presume che più lane significhino più hash.
- **Pipeline worker e pool:** assegnazione dei job e dei nonce, verifica dei
  risultati, invio delle share e statistiche reali.
- **Verifica e benchmark:** ogni variante deve produrre gli hash attesi e
  superare i test prima di poter essere selezionata o mantenuta.

## Dashboard locale

La dashboard React/TypeScript permette di configurare pool, wallet, thread,
TLS/fingerprint e MSR, e controlla il processo avviato dall'utente. Il backend
ascolta esclusivamente su `127.0.0.1`; preferisce `engine/build/unique-rx` e
legge le sue righe di telemetria `UNIQUE_RX_STATUS`. Il motore nativo applica
il preset Intel RandomX automatico con salvataggio e ripristino dei registri;
preset personalizzati e CPU non Intel vengono instradati a XMRig. Hashrate,
share e uptime restano vuoti finché l'API o la telemetria del miner non li
forniscono.

La configurazione del miner vive esclusivamente nel file radice `config.json`,
nel formato nativo XMRig. Il bridge e `unique-rx` leggono questo stesso file;
non vengono creati profili o configurazioni generate. Il file è ignorato da Git
e viene protetto con permessi locali restrittivi.

MSR Automatico e Fast usano il preset Intel RandomX nativo, con lettura
dei valori originali, verifica dopo la scrittura e ripristino all'uscita.
Personalizzato passa a XMRig; anche le CPU non Intel usano XMRig per il
preset richiesto. Su Windows il motore nativo richiede il driver PawnIO
ufficiale e il modulo IntelMSR verificato tramite
`scripts/windows/setup-msr.ps1`; su Linux richiede accesso a
`/dev/cpu/*/msr`. L'accesso ai registri richiede i privilegi amministrativi
necessari. Se il ripristino fallisce, il miner registra un avviso. Su Windows il backend
richiede un arresto tramite CTRL+BREAK e attende la chiusura normale di XMRig;
se deve forzare la terminazione, segnala che il ripristino MSR non è garantito.

Per lo sviluppo, avviare il backend e il frontend in due terminali:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn backend.src.main:app --host 127.0.0.1 --port 8000
```

```sh
npm ci --prefix dashboard
npm run dev --prefix dashboard
```

Aprire l'indirizzo stampato da Vite. Per la modalità integrata, eseguire
`npm run build --prefix dashboard` e avviare solo Uvicorn sulla porta `8000`.

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --target unique-rx
ctest --test-dir engine/build --output-on-failure
```

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build --target unique-rx
```

Il backend preferisce `engine/build/unique-rx` solo quando MSR è
esplicitamente disattivato; per MSR automatico, Fast e personalizzato usa
XMRig finché il supporto nativo non è implementato.

**Compatibilità piattaforme:** il trasporto Stratum usa un livello socket
separato per Winsock su Windows e POSIX su Linux. La CI compila e testa il
motore su Windows e Linux; il percorso MSR Windows usa PawnIO, mentre Linux
usa il driver kernel `msr`.

## Candidati da valutare
## Audit fonti dashboard

Verificati repository e commit il 7 novembre 2026. Per “sorgente disponibile”
intendo codice applicativo presente nel tree, non README, screenshots o archivi
binari.

| Fonte | Commit / licenza | Implementazione effettivamente verificata | Esito |
| --- | --- | --- | --- |
| [WildRig-NG `06d9818`](https://github.com/andru-kun/wildrig-ng/tree/06d9818e14f7482876ec69fa06e70c3bc93e1f74) | `LICENSE.txt` è solo un disclaimer “AS IS”; nessuna licenza di riuso | README documenta watchdog GPU, retry, esclusione delle GPU guaste, API JSON e sensori ADL/NVML/IGCL. Il tree contiene README, disclaimer e asset, non il sorgente del miner. | Studiare i comportamenti documentati; niente funzioni da estrarre. |
| [BzMiner `d8828e9`](https://github.com/bzminer/bzminer/tree/d8828e908b8c4950f35e6ed273856fbe724fa208) | Nessuna licenza dichiarata | Tree composto dal README. Documenta modalità hardware-monitor, elenco sensori e API, ma non implementazione o test. | Riferimento funzionale soltanto; non copiare. |
| [MiningMonitor `95c5547`](https://github.com/lennykean/MiningMonitor/tree/95c55476074ffeb71d4917896a2d1580680883f9) | MIT | Sorgente .NET/Angular: `BackgroundScheduler<TWorker>.ExecuteAsync`, `Schedule<TWorker>.Interval`, `DataCollectorWorker.DoWorkAsync`/`GetSnapshotAsync`; scanner `ConnectivityScanner`, `GpuHashrateThresholdScanner`, `GpuTemperatureThresholdScanner`, `GpuFanSpeedThresholdScanner`; azioni `RestartMinerAlertActionExecutor.ExecuteActionAsync`, `DisableGpuAlertActionExecutor.ExecuteActionAsync`, `WebHookAlertActionExecutor.ExecuteActionAsync`. | Unico candidato con codice alert riusabile. È GPU/Ethereum-specifico e legacy (`netcoreapp2.1`, `netstandard2.0`); scheduler di polling, non scheduler start/stop del miner. Test presenti: `DataCollectorWorkerTest.Collect`, `CollectorServiceTest` per sync miner/snapshot. Non eseguiti in questo audit. |
| `mining-mon` | URL non identificato | La ricerca per nome non individua un repository univoco. | Richiedere URL esatto; nessuna attribuzione di funzioni o licenza finché manca. |
| [OverManager `2d2a89c`](https://github.com/overbuildlabs/OverManager/tree/2d2a89c288be7b054a1ef3e6d0f93d1c90226675) | La licenza corrente è proprietaria; il LICENSE dichiara MIT solo per release precedenti specifiche | README documenta controllo remoto, notifiche push, allarmi e stime costo energia; il tree attuale contiene documentazione, screenshots e notice, non sorgenti applicativi. | Funzioni da prendere come requisiti, non codice. Nessun reverse engineering o riuso della versione corrente. |
| [PhoenixMiner mirror `86f6046`](https://github.com/spark-pool/PhoenixMiner/tree/86f604604db39e8ed12ad73c01983a84a59e1ca7) | Mirror non ufficiale, senza licenza dichiarata | Tree con README e ZIP Windows/Linux, non sorgenti. Non è una fonte affidabile per estrarre codice. | Escludere dall'integrazione; usare al massimo documentazione pubblica per compatibilità. |

### Selezione per funzione

- **Watchdog:** implementazione propria come supervisor separato, con grace period, backoff limitato, cause di restart registrate e possibilità di escludere un device; WildRig documenta buoni stati e comportamenti ma non fornisce codice riusabile.
- **Monitor hardware indipendente:** interfaccia provider separata dal miner; MiningMonitor offre pattern scanner/metriche ma copre GPU Ethereum, mentre WildRig/BzMiner documentano sensori vendor-specific senza sorgenti.
- **Notifiche e azioni:** `AlertScanner`/`IAlertActionExecutor` di MiningMonitor sono il solo riferimento MIT con implementazione visibile. Adattare il modello, non copiare i backend GPU; deduplicare gli eventi e richiedere conferma per azioni che riavviano il miner.
- **Programmazione:** `BackgroundScheduler.Interval` di MiningMonitor pianifica raccolta periodica, non finestre di mining. Orari start/stop e fuso orario vanno progettati e testati separatamente.
- **Energia e costi:** OverManager documenta watt, tariffa e netto, ma non pubblica il codice. Implementare stime marcate come tali; usare watt reali solo con un provider verificabile.
- **Controllo remoto:** OverManager lo documenta ma non pubblica sorgenti. Progettare autenticazione, TLS e autorizzazioni per comando; bind loopback come default, LAN solo su abilitazione esplicita.
- **Storico e diagnostica:** MiningMonitor fornisce snapshot, serie temporali e alert GPU; riusare i concetti dopo aver definito schema telemetria CPU/RandomX e migrazione dai dati correnti.

## Roadmap dashboard

Le dodici viste previste sono **Panoramica**, **Hardware**, **Monitoraggio**,
**Watchdog**, **Notifiche**, **Programmazione**, **Energia**, **Connessioni**,
**Controllo remoto**, **Statistiche**, **Diagnostica** e **Impostazioni**.
Priorità di consegna: provider hardware indipendente, watchdog, notifiche,
programmazione, controllo remoto autenticato e stima costi; le viste restanti
riusano gli stessi eventi e campioni, non metriche duplicate o inventate.

## Candidati da valutare

I riferimenti sono piste di ricerca, non istruzioni per copiare interi
repository. Prima di ogni integrazione vanno verificati sorgente, licenza,
dipendenze, compatibilità e vantaggio misurabile.

| Priorità     | Area                  | Parti da esaminare                      |
| ------------ | --------------------- | --------------------------------------- |
| Massima      | JIT multi-lane        | XMR-Miner `jit_multi_x86.cpp`           |
| Massima      | VM multi-lane         | XMR-Miner `vm.c`                        |
| Massima      | Pipeline hash e nonce | XMR-Miner `pool_utils.c`, `worker.c`    |
| Massima      | JIT x86-64            | RxMiner e RandomX upstream              |
| Alta         | AES e Argon2 AVX2     | RxMiner `aes_hash.cpp`, `argon2_avx2.c` |
| Alta         | Dataset e memoria     | RxMiner `dataset.cpp`, `allocator.hpp`  |
| Alta         | Affinità e worker     | RxMiner                                 |
| Alta         | Pool e TLS            | RxMiner                                 |
| Obbligatoria | Benchmark A/B e test  | XMR-Miner e RandomX upstream            |

## Audit iniziale delle fonti

Snapshot verificati il 7 novembre 2026. I commit sono riferimenti di audit, non
dipendenze già integrate.

| Progetto e commit | Licenza verificata | Evidenza e decisione |
| --- | --- | --- |
| [TymekTM/XMR-Miner `0eaf325`](https://github.com/TymekTM/XMR-Miner/tree/0eaf325) | Nessuna licenza dichiarata nel repository | Il JIT multi-lane ha un percorso FP spill compatibile con AVX2, ma `test_multilane` salta senza AVX-512. Il log riporta risultati non misurati sul 6700K. Da studiare, non copiare né integrare senza licenza e test AVX2 completi. |
| [c0mm4nd/rxminer `5fe52dd`](https://github.com/c0mm4nd/rxminer/tree/5fe52dde2650c623fd0472804152a94711889dce) | MIT per RxMiner; BSD-3-Clause per i file RandomX inclusi | Buon riferimento per worker, Stratum, topologia e test locali. I test nativi di consenso sono ignorati per default; quelli di rete richiedono una feature di test. Valutare singoli componenti solo dopo aver eseguito e ampliato i test pertinenti. |
| [tevador/RandomX `v2.0.1` (`aaafe71`)](https://github.com/tevador/RandomX/tree/aaafe71322df6602c21a5c72937ac284724ae561) | BSD-3-Clause | Riferimento canonico per vettori, compatibilità, selezione CPU e confronto hash. Usare come oracle e baseline versionata; non copiare l'intero motore nel sorgente UNIQUE RX. |
| [xmrig/xmrig `b2ca724`](https://github.com/xmrig/xmrig/tree/b2ca72480c58d197e18c885d9fc1a0c8d517e60a) | GPL-3.0 per il repository | Da confrontare per memoria, pagine grandi, MSR, worker e benchmark. Nessun codice GPL viene integrato finché la licenza del nuovo progetto non è decisa; i componenti terzi hanno licenze da verificare separatamente. |

Nessun candidato è promosso dai soli risultati dichiarati nei README o nei log.
Per il target i7-6700K servono build compatibile AVX2, vettori ufficiali su
ogni lane e misure A/B ripetibili sullo stesso hardware. In particolare si
prova prima 2 lane, poi 3; 4 lane non è un default e resta escluso se il guadagno
non supera la variabilità delle misure.

## Criteri di accettazione
## Mappa file e funzioni

Questi sono i punti d'ingresso da studiare. “Studiare” non equivale a copiare:
XMR-Miner non dichiara una licenza, XMRig è GPL-3.0, mentre RxMiner è MIT e i
suoi file RandomX mantengono le notice BSD-3-Clause.

| Componente | File e funzioni candidate | Uso previsto |
| --- | --- | --- |
| JIT multi-lane | XMR-Miner `src/crypto/randomx/jit_multi_x86.cpp`: `rx_mjit_create`, `rx_mjit_generate`, `rx_mjit_code` | Riferimento algoritmico soltanto. Riscrittura indipendente finché manca una licenza; validare prima spill AVX2, poi lane 2 e 3. |
| VM e batch | XMR-Miner `src/crypto/randomx/vm.c`: `rx_create_vm_group`, `rx_group_hash_first`, `rx_group_hash_next`, `rx_group_hash_last`, `rx_calculate_hash_dual`, `rx_vm_run_dual` | Usare i contratti per progettare test differenziali; non condividere VM/scratchpad mutabili tra lane o worker. |
| Pipeline nonce/share | XMR-Miner `src/miner/pool_utils.c`: `pool_pipeline_hash_next`, `pool_lanes_hash_next`; `src/miner/worker.c`: `worker_run`, `worker_submit_lane_share` | Verificare nonce stride, cambio job, ordinamento output e invio share. Nessuna integrazione del codice non licenziato. |
| Miner, Stratum e CPU | RxMiner `src/engine.rs`: `Engine::install`, `build_datasets`, `start_workers`, `report`; `src/stratum.rs`: `Stratum::start`, `session_loop`, `handle_message`, `submit`; `src/topology.rs`: `discover`, `set_affinity`, `pin`, `bind_node`, `available_memory` | Riferimenti MIT per separazione engine/worker/rete/topologia; selezionare singole funzioni solo dopo adattamento e test locali. |
| RandomX baseline | RandomX `src/randomx.h`: `randomx_get_flags`, `randomx_alloc_cache`, `randomx_init_cache`, `randomx_alloc_dataset`, `randomx_init_dataset`, `randomx_create_vm`, `randomx_calculate_hash`, `randomx_calculate_hash_first/next/last` | Oracle e baseline ufficiale; selezione CPU runtime, vettori di consenso e confronto di ogni lane. |
| JIT e opcode | RxMiner `src/crypto/randomx/jit_compiler_x86.cpp`: `JitCompilerX86::generateProgram`, `generateProgramLight`, `h_IMULH_R/M`, `h_ISMULH_R/M`, `h_CFROUND`, `h_CBRANCH`; XMRig ha handler analoghi in `src/crypto/randomx/jit_compiler_x86.cpp` | Implementare un lowering nostro e test per opcode; il file XMRig è GPL, il RandomX incluso in RxMiner ha notice BSD. |
| AES e Argon2 | RxMiner `src/crypto/randomx/aes_hash.cpp`: `hashAes1Rx4`, `fillAes1Rx4`, `hashAndFillAes1Rx4`; `argon2_avx2.c`: `randomx_argon2_fill_segment_avx2` | Kernel distinti da confrontare con il percorso di riferimento; AVX2 solo dopo feature check e vettori equivalenti. |
| Dataset, memoria e benchmark | RxMiner `dataset.cpp`: `initCache`, `initCacheCompile`, `initDatasetItem`, `initDataset`; XMRig `src/crypto/rx/RxDataset.cpp`: `RxDataset::init`, `allocate`; `src/hw/msr/Msr_linux.cpp`: `Msr::write`, `rdmsr`, `wrmsr`; `Benchmark.cpp`: `Benchmark::start`, `finish` | Misurare inizializzazione, pagine grandi, MSR e throughput come varianti separate. MSR opt-in, con ripristino verificato. |

## Compatibilità tra componenti

| Combinazione | Decisione | Vincolo di correttezza/performance |
| --- | --- | --- |
| JIT x86-64 + lowering opcode + allineamento codice | Compatibili, ma da sviluppare e testare insieme | L'allineamento cambia le distanze dei salti: `CBRANCH` va risolto/patchato dopo il layout finale. Testare `IMULH`, `ISMULH`, `CFROUND`, branch presi/non presi e displacement ai limiti. |
| JIT + AES-NI/AVX2 | Compatibili con dispatch runtime | Non compilare un percorso AVX2 come unico percorso obbligatorio; confronto con fallback e vettori ufficiali. Il 6700K non ha AVX-512. |
| JIT multi-lane + batch hashing | Compatibili solo con stato indipendente per lane | Ogni lane deve avere VM, registri, rounding mode e scratchpad isolati. Non usare simultaneamente le API pipeline sullo stesso stato mutabile. |
| Lane + nonce/worker/pool | Compatibili con assegnazione deterministica | Non duplicare nonce; al cambio job invalidare i batch pendenti prima di inviare share. Test locale Stratum separato dal benchmark CPU. |
| AES-NI + Argon2 AVX2 | Generalmente componibili | Operano in fasi diverse, ma contesa CPU/cache durante inizializzazione può annullare il guadagno end-to-end: misurare singolarmente e insieme. |
| Dataset + huge pages + affinità | Condizionali, non sommabili per ipotesi | Verificare allocazione reale, limiti RAM, pagine disponibili e pinning. Sul 6700K non c'è beneficio NUMA; il numero di worker/lane può peggiorare la contesa L2/memoria. |
| MSR + worker/lane | Opzionale e separato | Richiede permessi elevati e può cambiare stato hardware. Disabilitato di default; salvare il valore precedente e ripristinarlo anche su errore/uscita. |
| `-march=native` + fallback CPU | In conflitto per binari distribuibili | Usarlo solo in una build locale di benchmark. Il binario generale deve mantenere dispatch runtime e non contenere istruzioni non supportate dal target. |

Ordine di validazione: baseline ufficiale; JIT scalare per opcode; batch scalare;
2 lane AVX2 con confronto per lane; 3 lane; 4 lane solo se supera rumore e
variabilità. Solo dopo si provano le combinazioni di AES/Argon2, memoria e
affinità.

Stato implementazione: `engine/` contiene un miner CLI C++ che legge config,
esegue login/job/submit Stratum `rx/0` su TCP/TLS, inizializza cache e VM
RandomX, crea worker CPU con nonce disgiunti e valida le share rispetto al
target. Usa il JIT upstream come baseline. L'emitter x86-64 W^X nostro interleave
1–4 lane per opcode interi e scratchpad, ma non è ancora innestato nel loop
RandomX completo: mancano lowering floating point e branch/rounding per-lane;
il dataset completo, affinità e MSR nativi sono inoltre da completare.

Build e test del nucleo nativo:

```sh
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build
ctest --test-dir engine/build --output-on-failure
engine/build/unique-rx --benchmark 60 2
```

## Criteri di accettazione

Una modifica è adottata solo se soddisfa tutti questi punti:

1. È compatibile con l'architettura e le feature hardware supportate, con un
   fallback corretto dove previsto.
2. Produce gli stessi risultati del percorso di riferimento sui test noti e
   sui casi di confronto; le share vengono validate prima dell'invio.
3. Supera i test pertinenti, inclusi quelli di concorrenza per modifiche a VM,
   worker, memoria o pipeline.
4. Migliora il throughput o un'altra metrica dichiarata senza regressioni
   inaccettabili di correttezza, stabilità o consumo di memoria.
5. Rispetta licenze e attribuzioni delle parti eventualmente derivate.

Le ottimizzazioni si provano prima singolarmente e poi in combinazioni: non si
assume che vantaggi misurati isolatamente siano sommabili.

## Piano di benchmark iniziale

Obiettivo prioritario: Intel Core i7-6700K, 4 core fisici, AVX2 e AES-NI.
Confrontare 1, 2, 3 e 4 lane e il percorso JIT standard sullo stesso sistema,
con la stessa configurazione e condizioni di esecuzione ripetibili. Registrare
hash/s, versione e parametri di build, modalità JIT, lane e configurazione
worker. Ripetere le misure e confrontare i risultati con il baseline; nessun
numero proveniente da hardware diverso sostituisce questa prova.

## Fasi di integrazione

1. Stabilire baseline, vettori di riferimento, configurazione di build e
   provenienza/licenza di ogni candidato.
2. Implementare il percorso standard e la pipeline minima con test di
   equivalenza degli hash e delle share.
3. Valutare ogni ottimizzazione isolatamente, una alla volta, contro il
   baseline e sulla macchina obiettivo.
4. Combinare solo le varianti promosse e ripetere test e benchmark end-to-end.
5. Abilitare la selezione automatica esclusivamente tra percorsi validati;
   conservare un percorso standard di riferimento.
