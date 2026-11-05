# AI Qween — modulo sorgente per XMRig Control Center

Questa è una prima integrazione modulare lato controller. Non sostituisce XMRig,
non implementa RandomX e non produce hash/share/guadagni fittizi.

## Contenuto
- `ai_qween/engine.py`: analisi dei campioni reali, guardia termica con isteresi,
  raccomandazioni conservative sui thread.
- `ai_qween/xmrig_bridge.py`: lettura in sola lettura di `/1/summary` su loopback.
- `ai_qween/__init__.py`: esportazioni del modulo.

## Collegamento previsto
```python
from ai_qween import AIQween, QweenConfig, XMRigQweenBridge

qween = AIQween(QweenConfig(
    min_threads=1,
    max_threads=16,  # impostare in base alla CPU reale
    max_cpu_temperature_c=82.0,
    resume_temperature_c=75.0,
    temperature_required_for_auto=True,
))
bridge = XMRigQweenBridge(
    host="127.0.0.1",
    port=18088,
    token="TOKEN_API_XMRIG",
    qween=qween,
)
sample = bridge.poll()  # sensori None finché non viene collegato un provider reale
decision = qween.decide(current_threads=8, logical_cpus=16)
print(sample, decision.to_dict())
```

## Limiti intenzionali
- `auto_apply` è disabilitato: questo modulo non riscrive la configurazione
  né modifica il processo miner in esecuzione.
- Senza sensore termico reale non dà via libera all'ottimizzazione automatica.
- Non inventa temperatura o potenza se Windows non fornisce i dati.
- Per applicare una modifica thread occorre fermare XMRig, aggiornare la
  configurazione con il controller e riavviare in modo controllato.
- Il modulo non integra direttamente il codice C++ del repository esterno:
  è un adattatore Python separato, più adatto all'attuale app Tkinter.

## Stato
Sorgenti soltanto. Nessuna compilazione, nessun miner incluso.
