# UNIQUE RX — Monero RandomX CPU Miner

> Documento operativo vincolante del progetto. Le disposizioni qui contenute devono essere lette prima di ogni intervento e rispettate durante sviluppo, integrazione, test e rilascio.

## 1. Obiettivo e identità del progetto

Realizzare un miner CPU autonomo per Monero, algoritmo **RandomX `rx/0`**, con motore nativo, gestione pool/Stratum, worker, shares, ottimizzazioni CPU, MSR sicuro e dashboard collegata ai dati reali del motore.

Il progetto deve essere portabile e utilizzabile su **più computer e configurazioni CPU**, non costruito o limitato al PC di sviluppo (Intel i7-6700K). Il rilevamento hardware e le impostazioni devono adattarsi alla macchina in uso.

Il motore deve restare un progetto autonomo: non trasformarlo in un fork/collage di miner esistenti e non integrare interi motori esterni. I progetti di riferimento servono per studiare e verificare implementazioni mature e per portare singole soluzioni compatibili, non per sostituire l'architettura del progetto.

## 2. Regole inderogabili di lavoro

1. **Leggere questo README prima di ogni modifica.** È la specifica operativa del progetto.
2. Non cancellare, rinominare, svuotare o riscrivere questo README senza autorizzazione esplicita dell'utente.
3. Non eliminare file, cartelle, sorgenti, funzioni o componenti esistenti solo per semplificare il lavoro.
4. Non cambiare arbitrariamente la struttura del repository. Prima di aggiungere o spostare file, verificare la struttura già presente e mantenere organizzazione coerente.
5. Lavorare sul codice effettivo del repository. Non creare demo, placeholder, pseudocodice, file segnaposto o documentazione spacciata per implementazione.
6. Non dichiarare una funzione completata se non è implementata e verificata. Distinguere sempre codice presente, codice testato e codice ancora da completare.
7. Ogni modifica deve essere compatibile con le altre parti del motore: controllare chiamanti, tipi, configurazione, build e test correlati.
8. Non introdurre dipendenze o servizi a pagamento non richiesti. Preferire componenti open source e build riproducibili.
9. Non eseguire operazioni distruttive Git, non usare force push e non sovrascrivere il lavoro dell'utente. Prima di aggiornare un branch verificare HEAD e usare aggiornamenti con controllo della versione attesa.
10. Non effettuare scritture MSR reali durante CI, test automatici o verifiche senza autorizzazione esplicita.

## 3. Sorgenti di riferimento e licenze

Studiare miner e librerie maturi per individuare soluzioni reali e collaudate, verificando sempre sorgente, licenza, compatibilità e test prima di portare una singola implementazione.

Riferimenti già individuati:
- **RandomX upstream**: riferimento/oracolo per la correttezza dell'algoritmo e dei risultati hash; licenza BSD-3-Clause. Mantenere il comportamento compatibile con RandomX usato da Monero.
- **RxMiner**: riferimento per idee e implementazioni CPU/Stratum/MSR; verificare la licenza MIT e conservare attribuzioni e avvisi quando si riutilizza codice.
- **NyxRX**: riferimento per rilevamento hardware, core fisici, cache e NUMA; verificare e rispettare la licenza MIT per ogni porzione eventualmente portata.
- **XMRig**: riferimento tecnico per CPU, RandomX, MSR e gestione miner; è GPL-3.0. Non copiare o incorporare codice senza rispettare tutti gli obblighi GPL e la compatibilità della licenza del progetto.
- **XMR-Miner**: non copiare codice finché non è individuata una licenza esplicita che autorizzi il riutilizzo.

Regole:
- “Open source” non significa assenza di condizioni: verificare la licenza di ogni sorgente.
- Non copiare interi repository, motori, worker o sottosistemi senza una decisione architetturale e una verifica delle licenze.
- Per ogni codice portato, registrare provenienza, autore/copyright, licenza e modifiche; includere i file di licenza e gli avvisi richiesti.
- Non attribuire al progetto funzionalità o prestazioni di un miner di riferimento che non siano state effettivamente integrate e testate.

## 4. Algoritmo e motore RandomX

- Supportare Monero RandomX `rx/0` come obiettivo del motore CPU.
- Non aggiungere altri algoritmi di mining e non sviluppare un motore GPU.
- Mantenere la compatibilità con i dati e i risultati dell'implementazione RandomX di riferimento.
- Completare correttamente tutte le fasi necessarie: blob/job, nonce, inizializzazione RandomX, cache, dataset, esecuzione hash e risultato.
- Verificare l'hash con vettori/oracoli noti di RandomX. Non basarsi soltanto sul fatto che il programma compili.
- Le ottimizzazioni devono mantenere la correttezza bit-per-bit.
- Supportare percorsi CPU con AES-NI e AVX2 quando disponibili, con fallback sicuro per CPU che non dispongono delle istruzioni richieste.
- Gestire in modo esplicito le diverse modalità/lane di esecuzione (1, 2, 3 e 4 dove supportate): misurarle sullo stesso hardware e non impostare automaticamente una modalità più ampia se non dimostra un vantaggio ripetibile. Sul sistema i7-6700K la modalità a 4 lane non deve essere assunta come predefinita senza misurazioni.
- Non presumere che tutte le CPU abbiano le stesse cache, istruzioni, numero di core o topologia.

## 5. Rilevamento CPU, thread e affinità

Il miner deve adattarsi alla macchina, non a un singolo processore.

Rilevare, quando il sistema operativo lo consente:
- vendor, famiglia, modello e feature set CPU;
- core fisici e processori logici;
- gruppi di processori Windows;
- topologia NUMA e cache rilevanti per RandomX;
- CPU effettivamente disponibili al processo, inclusi affinity mask e limiti container/cgroup Linux.

Comportamento richiesto:
- scegliere un numero iniziale di worker sensato in base ai core fisici e alle risorse realmente disponibili;
- consentire all'utente di modificare il numero di thread;
- applicare affinity senza bloccare il miner se il sistema rifiuta una specifica assegnazione;
- evitare di assegnare più worker di quanti il processo possa usare;
- gestire sistemi mono-socket, multi-socket, NUMA, container e CPU con SMT/Hyper-Threading;
- fornire fallback se il rilevamento avanzato non è disponibile;
- non codificare il numero di core del PC di sviluppo come costante.

## 6. MSR e ottimizzazioni hardware

- Le modalità dashboard `auto` e `fast` selezionano lo stesso preset CPU nativo verificato; `fast` è mantenuta solo per compatibilità con configurazioni precedenti e non rappresenta un secondo profilo.
- `off` disabilita le scritture MSR. `custom` richiede il backend XMRig esterno e non viene eseguita dal motore nativo. Il preset nativo è facoltativo: CPU non supportate, driver o permessi assenti non devono impedire l'avvio del mining.

MSR è una funzione di ottimizzazione **facoltativa**, mai un requisito per avviare il miner.
- `cpu.prefetch-mode` controlla il prefetch del scratchpad nel JIT x86: `0` disattivato, `1` T0 (predefinito), `2` NTA, `3` caricamento demand. La scelta è configurabile e non implica un vantaggio prestazionale senza benchmark sul processore effettivo.

- `cpu.jit-huge-pages` abilita in modo facoltativo le pagine esplicite per il buffer JIT x86; se la prenotazione o i privilegi non sono disponibili, `allocJitMemory` ripiega automaticamente sulle pagine standard/THP. È disattivato per impostazione predefinita.


Requisiti:
- riconoscere CPU e modelli supportati prima di proporre un preset;
- usare soltanto registri, maschere e valori documentati e compatibili con la CPU rilevata;
- richiedere privilegi e driver/interfaccia appropriati;
- salvare il valore originale di ogni registro prima di modificarlo;
- applicare le modifiche in modo controllato e verificare gli esiti;
- in caso di errore parziale, ripristinare i registri già modificati;
- ripristinare i valori originali alla chiusura normale, allo stop e quando possibile dopo un errore;
- non sovrascrivere valori non modificati dal miner;
- esporre chiaramente stato, errori e motivo del mancato supporto;
- se MSR non è disponibile o fallisce, continuare il mining senza MSR.

Non applicare preset Intel o AMD in modo indiscriminato. Il preset deve dipendere dalla famiglia/modello effettivo. Il backup/restore deve essere testabile con backend simulato; i test automatici non devono scrivere registri hardware reali.
Il percorso comune `engine/src/msr/transaction.hpp` è usato dai backend Linux e Windows. Il test simulato verifica maschere, snapshot, read-back, rollback dopo errore parziale e nuovo tentativo di restore; non esegue scritture MSR hardware.

## 7. Pool, Stratum e shares

La pipeline deve essere completa e funzionare con pool reali compatibili con il protocollo supportato.

Flusso obbligatorio:
1. configurazione endpoint, porta, wallet/username, password, TLS e opzioni pool;
2. connessione e gestione timeout;
3. handshake/login e ricezione risposta;
4. ricezione, parsing e validazione dei job;
5. generazione blob di lavoro e gestione nonce;
6. distribuzione del job ai worker;
7. hashing RandomX;
8. confronto del risultato con il target;
9. costruzione e invio della share al server;
10. parsing della risposta e registrazione dell'esito.

Gestire esplicitamente:
- share accepted e rejected;
- stale/obsolete share e job scaduti;
- errori di protocollo e risposte inattese;
- disconnessione, timeout, riconnessione e backoff;
- cambio job e aggiornamento del lavoro dei worker;
- nonce e identificatori senza duplicazioni improprie;
- code limitate e backpressure;
- stop ordinato: arrestare i worker, gestire le share in coda e registrare quelle non inviate o scadute.

La dashboard deve mostrare contatori reali di accepted/rejected/stale, errori, connessione e hashrate. Non inventare share accettate o simulare risultati come se provenissero da una pool reale.

I test loopback con server simulato sono utili e necessari, ma **non equivalgono a una prova con una pool reale**. Il test di riconnessione deve simulare una connessione TCP autenticata che cade dopo il login e verificare una seconda sessione e il drenaggio ordinato delle share. Dichiarare chiaramente quale tipo di test è stato eseguito.

## 8. Benchmark e prestazioni

- Fornire benchmark ripetibili del motore e delle configurazioni CPU.
- Misurare hashrate, durata, thread/lane, consumo di memoria e condizioni del test quando disponibili.
- Confrontare le modifiche con una baseline A/B sullo stesso sistema e con le stesse condizioni.
- Ripetere le misure per distinguere miglioramenti reali dalla normale variabilità.
- Non dichiarare aumenti percentuali o vantaggi senza risultati misurati.
- Il benchmark deve poter funzionare senza collegarsi a una pool e senza inviare shares.

Comando di riferimento già previsto:
`engine/build/unique-rx --benchmark 60 2`
Verificare il nome effettivo dell'eseguibile e mantenere coerenti documentazione e build.

## 9. Dashboard e backend

La dashboard esistente va mantenuta e completata, non duplicata e non confusa con l'interfaccia di un miner esterno.

Deve leggere lo stato effettivo del motore/backend e mostrare almeno:
- stato miner e pool;
- wallet/worker configurati, senza esporre segreti;
- algoritmo e job attivo;
- CPU rilevata (vendor, modello, famiglia, feature, core fisici/logici, NUMA e cache L3), thread e impostazioni;
- Controlli dashboard per huge pages RandomX, huge pages JIT e modalità prefetch x86;
- hashrate corrente e storico;
- shares accepted/rejected/stale;
- uptime, connessioni, errori e riconnessioni;
- stato MSR e ripristino;
- log ed eventi importanti;
- controlli start/stop e modifica configurazione con validazione.

Non mostrare dati fittizi come dati reali. Se una funzione non è ancora collegata al motore, indicarla come non disponibile invece di simularla.

La configurazione principale prevista è `config.json` nella radice, salvo che il codice già presente stabilisca in modo coerente un percorso diverso: non creare configurazioni concorrenti.

## 10. Struttura del repository e sorgenti

- Conservare una struttura chiara e professionale, con sorgenti effettivi nelle cartelle dedicate (in particolare `engine/src` e relativi moduli).
- Ogni modulo deve contenere codice compilabile e appartenere a una responsabilità precisa.
- Evitare file duplicati, cartelle parallele inutili, documenti temporanei e testi inseriti dentro le cartelle sorgenti.
- Non ridurre artificialmente il motore a pochi file: RandomX, worker, CPU, MSR, Stratum, trasporto, configurazione e test devono essere organizzati in moduli coerenti.
- Tenere separati motore, backend, dashboard, script, test e licenze.
- Non spostare o rinominare moduli senza aggiornare tutti i riferimenti CMake, include, test, script e workflow.
- Mantenere gli avvisi di licenza accanto ai componenti derivati.

## 11. Piattaforme e compilazione

Obiettivo: supporto Windows e Linux, con rilevamento delle funzionalità disponibili e fallback per le differenze di sistema operativo.

La compilazione deve essere verificata su entrambe le piattaforme. Non considerare una build Linux come prova di compatibilità Windows, o viceversa.

Comandi CMake/CTest di riferimento:
```sh
cmake -S engine -B engine/build
cmake --build engine/build --config Release
ctest --test-dir engine/build --output-on-failure
```

Adattare i comandi al generatore e alla piattaforma (ad esempio configurazione Release su generatori multi-config). Gli script BAT/PowerShell per Windows e gli script shell per Linux devono essere inclusi e mantenuti funzionanti, senza avviare automaticamente una compilazione sul PC dell'utente.

Script operativi aggiunti (da verificare nella fase finale):
- Windows: `scripts/windows/start.ps1`, `scripts/windows/stop.ps1`, `scripts/windows/diagnose.ps1` e wrapper `.bat` omonimi (più `build.bat`).
- Linux: `scripts/linux/start.sh`, `scripts/linux/stop.sh`, `scripts/linux/diagnose.sh`.
- Gli script start/stop usano il backend locale su `127.0.0.1:8000`, così l'arresto passa dal gestore che invia il segnale di shutdown al motore e consente il ripristino MSR.

## 12. Test obbligatori

A ogni modifica eseguire i test pertinenti e aggiungerne quando manca copertura.

Copertura minima:
- vettori hash RandomX e confronto con implementazione di riferimento;
- parsing/validazione job e nonce;
- worker, code e cancellazione job;
- protocollo Stratum: login, job, submit, accepted, rejected, stale, timeout, disconnessione e riconnessione;
- shutdown e drenaggio delle shares;
- rilevamento CPU e fallback;
- thread limitati da affinity/cgroup;
- MSR: CPU non supportata, permessi negati, errore parziale, rollback e restore con backend simulato;
  - Stato della copertura automatica: preset CPU non supportati e transazioni simulate (maschere, snapshot, errore di scrittura, rollback e retry restore) sono coperti; permessi/driver e hardware reali non sono verificati dalla CI.
- configurazione e API backend;
- dashboard e collegamento ai dati backend;
- build nativa Linux e Windows.

Un test simulato deve essere identificato come tale. Non dichiarare verificata una pool reale, un hardware reale o un preset MSR reale se non è stato effettivamente provato.

## 13. Stato e criteri di completamento

Il progetto è completato soltanto quando:
- motore RandomX CPU corretto e verificato;
- rilevamento CPU universale con fallback;
- MSR facoltativo, sicuro e con backup/restore;
- pipeline worker/job/nonce/share completa;
- Stratum robusto con test dei diversi esiti;
- dashboard collegata ai dati reali;
- build e test Linux e Windows riusciti;
- licenze e attribuzioni rispettate;
- script di avvio, arresto, diagnostica e compilazione presenti e verificati;
- benchmark ripetibili disponibili.

Ogni rapporto sullo stato deve separare chiaramente:
- implementato;
- compilato;
- testato automaticamente;
- verificato su hardware reale;
- verificato con pool reale;
- ancora da completare.

Non definire il progetto “finito” se manca uno dei criteri sopra.

---

**Regola finale:** queste disposizioni sono permanenti per il progetto. Prima di ogni intervento rileggere il README e preservare il lavoro già presente. Le modifiche devono migliorare il miner reale, non soltanto aggiungere file o dichiarazioni.

### Attribuzioni di codice integrato

- Il percorso AES VAES in `engine/src/randomx/upstream/src/aes_hash_vaes.cpp` deriva da `c0mm4nd/rxminer`, commit `5fe52dde2650c623fd0472804152a94711889dce`; le porzioni RxMiner sono MIT, con testo conservato in `engine/src/randomx/upstream/LICENSES/RXMINER-MIT.txt`.
- Il profilo MSR AMD Zen 3 in `engine/src/msr/presets.hpp` è portato da `c0mm4nd/rxminer/src/msr.rs`, commit `5fe52dde2650c623fd0472804152a94711889dce`; licenza MIT. Sono mantenuti allow-list e valori dei registri della sorgente di riferimento.
- Il lock inter-processo Linux MSR in `engine/src/msr/msr.cpp` adatta il blocco `flock` di `c0mm4nd/rxminer/src/msr.rs`, commit `5fe52dde2650c623fd0472804152a94711889dce`; licenza MIT.
- Il modulo opzionale Windows `IntelMSR.bin` è il binario ufficiale `namazso/PawnIO.Modules` release `0.2.11`, file `IntelMSR.bin`, SHA-256 `d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f`; il sorgente del modulo è LGPL-2.1-or-later. Il binario non è incluso nel repository: il motore verifica il digest prima di caricarlo e PawnIO verifica la firma del modulo.
- Il codice RandomX upstream resta BSD-3-Clause secondo la licenza conservata in `engine/src/randomx/upstream/LICENSE`.
- Le allocazioni RandomX page-backed, l'allineamento Linux a 2 MiB per Transparent Huge Pages, il fault-in delle huge pages esplicite, il riuso NUMA-aware delle pagine scratchpad da 2 MiB, il prefetch JIT configurabile e il dispatch BMI2/RORX in `cpu.cpp`, `cpu.hpp` e `jit_compiler_x86.cpp`, oltre alle API THP/JIT/prefetch in `virtual_memory.c` e `.h` e all'allocatore in `allocator.cpp`, derivano da `c0mm4nd/rxminer`, commit `5fe52dde2650c623fd0472804152a94711889dce`. Le modifiche RxMiner sono MIT; il copyright e la licenza BSD-3-Clause upstream RandomX sono conservati nei file.

## 15. Documento operativo vincolante — istruzioni aggiornate dell'utente

Il presente documento operativo è vincolante per tutte le attività del progetto. Le disposizioni devono essere lette prima di ogni intervento e rispettate durante sviluppo, integrazione, test e rilascio.

1. **Priorità assoluta: completare il codice effettivo dei miner di riferimento.** Procedere prima di tutto a recuperare, integrare e completare nel progetto principale il codice reale prelevato dai miner indicati dall'utente. Non sostituire questo lavoro con analisi, audit, elenchi, spiegazioni o documentazione non richiesta.
2. **Nessuna finzione.** Non dichiarare mai di aver copiato, integrato, completato, compilato o verificato codice se l'operazione non è stata realmente eseguita. Distinguere sempre il codice effettivamente presente dalle attività ancora da svolgere e dalle verifiche non eseguite.
3. **Niente codice inventato come sostituto.** Non creare implementazioni fittizie, segnaposto, moduli dimostrativi o codice improvvisato al posto delle implementazioni reali disponibili nei miner di riferimento. Integrare soltanto codice compatibile, adattandolo tecnicamente quando necessario e rispettando sempre licenze e attribuzioni.
4. **Lavoro continuo e senza interruzioni superflue.** Non interrompere continuamente lo sviluppo per inviare aggiornamenti, commenti o resoconti di singoli passaggi. Procedere in modo continuativo e comunicare soltanto quando è necessario un intervento dell'utente o quando si raggiunge un risultato concreto.
5. **Stima del tempo ogni dieci minuti.** Durante il lavoro, fornire ogni dieci minuti un'indicazione aggiornata e realistica del tempo residuo stimato per completare il progetto. Non inventare una scadenza: se non è possibile stimarla con affidabilità, dichiararlo chiaramente e indicare le attività che determinano il tempo residuo.
6. **Rilettura ogni tre minuti.** Rileggere il README ogni tre minuti durante il lavoro continuativo, oltre che prima di ogni intervento e prima di ogni fase significativa. La cadenza di tre minuti sostituisce la precedente indicazione di due minuti.
7. **Applicazione in tutte le fasi.** Le regole valgono senza eccezioni durante sviluppo, integrazione dei moduli, gestione delle share, MSR, compilazione, test, correzioni, preparazione dei pacchetti e rilascio.
8. **Ordine di assemblaggio richiesto.** Prima completare l'integrazione di tutti i moduli e del codice compatibile dei miner di riferimento. Non eseguire test, compilazioni o verifiche intermedie; solo dopo l'assemblaggio completo eseguire un'unica fase finale di build, test, verifica e correzione. Durante questa fase i workflow CI restano manuali e vanno ripristinati prima della verifica finale.

## 14. Disposizioni operative esplicite dell'utente — priorità e ritmo

Questa sezione integra le regole precedenti e deve essere rispettata in ogni sessione di lavoro.

1. **Eseguire le disposizioni esplicite dell'utente.** Non sostituirle con iniziative autonome, audit non richiesti, riorganizzazioni arbitrarie, spiegazioni ripetute o attività estranee alla consegna. In caso di conflitto tra una scelta autonoma e un'istruzione esplicita dell'utente, prevale l'istruzione dell'utente, nel rispetto di sicurezza, correttezza tecnica e licenze.
2. **Priorità alla consegna concreta.** L'obiettivo è completare il miner richiesto nel repository principale `main`, non limitarsi a descrivere cosa andrebbe fatto, creare elenchi di attività o aggiungere documentazione al posto del codice.
3. **Usare implementazioni sorgente reali dei miner di riferimento.** Recuperare i file e i moduli effettivi, selezionare le parti compatibili, conservarne provenienza e licenza e integrarle nel progetto. Non inventare implementazioni sostitutive quando esiste codice riutilizzabile compatibile. Non copiare codice senza licenza compatibile o senza rispettarne gli obblighi.
4. **Nessun lavoro parallelo.** Integrare il lavoro nel progetto principale e nella sua struttura esistente. Non creare rami, repository, progetti o copie parallele salvo richiesta esplicita.
5. **Rilettura periodica obbligatoria.** Rileggere il README all'inizio di ogni intervento e poi ogni tre minuti durante il lavoro continuativo. La rilettura serve a mantenere tutte le disposizioni presenti e a impedire deviazioni, dimenticanze o ripetizione degli errori già corretti.
6. **Rapidità senza finzioni.** Procedere con la massima rapidità ragionevole, evitando interruzioni, aggiornamenti superflui e richieste di conferma non necessarie. Non fermarsi per riferire ogni singolo passaggio: comunicare quando serve una decisione indispensabile oppure quando la consegna è pronta. Non sacrificare correttezza, compilabilità, sicurezza MSR o licenze per dichiarare un completamento anticipato.
7. **Tempo indicativo di consegna.** L'utente richiede il massimo impegno per consegnare il progetto entro un'ora. Questo è un obiettivo di urgenza, non un'autorizzazione a dichiarare completate funzioni non implementate o non verificate. Se una verifica o una parte essenziale non è materialmente completabile nel tempo disponibile, indicare con precisione cosa è stato fatto e cosa resta.
8. **Pubblicazione.** L'utente ha autorizzato il lavoro direttamente su `main`, inclusi i commit necessari. Non usare rami paralleli. Non eseguire force push o operazioni distruttive.
9. **Consegna finale.** Consegnare il progetto effettivamente aggiornato, con riepilogo conciso dei moduli integrati, commit pubblicati, verifiche realmente eseguite e limitazioni residue. Non dichiarare finito ciò che non è stato effettivamente realizzato.


