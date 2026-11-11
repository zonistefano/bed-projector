# AGENTS.md

Istruzioni per chi sviluppa su questo repository, agenti AI compresi. Leggile prima di modificare il codice: descrivono vincoli che dal codice da solo non si vedono.

## Il progetto in breve

Orologio proiettato su ESP32-S3 con display ST7735S 128×128 e LED in PWM. Tre parti che formano un unico prodotto e cambiano insieme:

| Parte | Percorso | Linguaggio |
| --- | --- | --- |
| Firmware | `main/` (+ driver vendorizzato in `components/`) | C, ESP-IDF 6.1.x, LVGL 9.5 |
| Web UI locale | `web/` (incorporata nel firmware) | HTML/CSS/JS vanilla, nessuna build |
| Integrazione Home Assistant | `custom_components/bed_projector/` | Python, distribuita con HACS |

Mappa del firmware:

| File | Ruolo |
| --- | --- |
| `main.c` | Avvio: NVS, stato, display, rete, API, conferma OTA |
| `projector.c/.h` | Stato condiviso (mutex), validazione JSON, persistenza NVS |
| `projector_logic.c/.h` | Logica pura senza ESP-IDF, testata su host; schema di pagine, elementi, modi e widget |
| `bed_api.c` | Server HTTP, autenticazione, OTA, file statici |
| `bed_net.c` | Wi-Fi STA, AP di recupero, SNTP, mDNS |
| `bed_display.c` | LCD, LED, task che renderizza solo quando qualcosa cambia |
| `bed_ui.c` | Pagine LVGL (Ora, Casa, Meteo, Agenda, Elenco, messaggio, schermata AP, calibrazione) |
| `bed_fonts.c`, `bed_icons.c` | **Generati** da `tools/generate_fonts.py` e `tools/generate_icons.py`: non modificarli a mano |

Documentazione per l'utente: `README.md`, `docs/guida.md` (uso), `docs/api.md` (API), `docs/hardware-check.md` (prove sulla scheda).

## Comandi

```sh
# Test Python: protocollo HA e generatore della configurazione di build
python3 -m unittest discover -s tests -v

# Test C della logica pura (host)
cc -std=c11 -Wall -Wextra -Werror -I main main/projector_logic.c tests/test_firmware_logic.c -o /tmp/logic && /tmp/logic

# Sintassi dell'integrazione HA (Home Assistant non è necessario)
python3 -m py_compile custom_components/bed_projector/*.py

# Firmware (ESP-IDF 6.1.x attivo)
idf.py build
idf.py -p PORT flash monitor

# Anteprime PNG delle pagine in build/preview/ (serve una prima idf.py build per LVGL)
python3 tools/render_preview.py
```

La CI (`.github/workflows/ci.yml`) esegue test, sintassi e `idf.py build` con ESP-IDF 6.1. Una modifica è finita quando la CI è verde.

**Ambiente:** il devcontainer può avere una versione di ESP-IDF diversa dalla 6.1.x. Con IDF 6 i driver si richiedono per componente (`esp_driver_ledc`, `esp_driver_spi`, `esp_driver_gpio`, non `driver`) e cJSON arriva dal registro come `espressif/cjson`. Non adattare il progetto alla versione del container: se la build locale fallisce per questo, dillo, compila semmai una copia in una cartella temporanea e affidati alla CI. Non toccare `dependencies.lock` per far funzionare una versione diversa.

## Vincoli da non rompere

### Protocollo e persistenza
- **Un solo protocollo**: esiste un'unica versione di firmware, web UI e integrazione. Se cambi il formato di `/api/*`, aggiorna nello stesso intervento firmware, `protocol.py`, web UI, test e `docs/api.md`. Non aggiungere numeri di versione, campi legacy, migrazioni o note su versioni precedenti.
- **Limiti in byte**: le stringhe che l'integrazione invia sono tagliate da `protocol.short()` ai limiti dei buffer del firmware (`projector.h`: nomi 31, meteo 31, temperatura 15, temperature già formattate 11, riepilogo 63…). Se cambi una dimensione da una parte, cambiala anche dall'altra e nei test.
- **`#` vietato nel testo HA**: `short()` lo rimuove perché `bed_ui.c` usa il markup LVGL `#rrggbb testo#`.
- **NVS**: tutte le impostazioni stanno nel namespace `"bed_projector"` (`NS` in `projector.c`). Per una chiave nuova usa il prefisso `proj_` (massimo 15 caratteri) e scrivi sempre tramite `nvs_save()`.

### Firmware
- **Concorrenza**: `state` in `projector.c` si legge e si scrive solo sotto `lock`. Gli altri moduli lo leggono tramite `projector_snapshot()`, mai direttamente.
- **Stack**: le strutture grandi (`projector_snapshot_t` ~2,5 KB, `projector_ha_t`, i buffer OTA) vanno dichiarate `static`. Il server HTTP gestisce una richiesta alla volta, quindi i buffer statici negli handler sono sicuri. Il task del display ha 8 KB di stack: un overflow non dà errori visibili.
- **Rendering solo su cambiamento**: `display_task` ridisegna quando cambia lo snapshot, il minuto o lo stato dell'AP. Tutto ciò che compare sullo schermo deve quindi stare nello snapshot, oppure dipendere da quei due segnali. Un dato letto da altrove non si aggiornerebbe.
- **Setter in `bed_ui.c`**: usa `set_text`, `set_font`, `set_color`, `set_area` e `show`, che non fanno nulla se il valore è uguale. Chiamare direttamente `lv_label_set_text` farebbe ripartire le scritte che scorrono.
- **Dati HA scaduti**: dopo 120 s senza snapshot, `projector_snapshot()` azzera `ha` con `ha_unknown()`. Un dato sconosciuto deve apparire come `?`, mai come un valore plausibile (per esempio "tutto chiuso").
- **Validazione**: ogni input JSON passa dagli helper di `projector.c` (`is_integer`, `parse_text`, `in_list`…) prima di toccare lo stato. Uno snapshot non valido viene rifiutato per intero.
- **Logica pura**: calcoli senza ESP-IDF (date, orientamento, timeout, backoff) vanno in `projector_logic.c`, accompagnati da un test in `tests/test_firmware_logic.c`.
- **Sicurezza**: tutte le API, tranne `/api/pair` (che risponde solo dall'AP), richiedono il token. Non registrare né restituire token o password Wi-Fi in log o risposte. Unica eccezione voluta: la password casuale dell'AP stampata sulla seriale.

### Integrazione Home Assistant
- La costruzione del payload sta in `protocol.py`, che non importa nulla di Home Assistant: i test la caricano senza HA. Tienila così e metti lì la logica da testare.
- Le piattaforme leggono il coordinator da `entry.runtime_data`. Gli errori delle azioni utente vanno sollevati come `HomeAssistantError`.
- `listed()` gira a ogni cambio di stato di HA: niente I/O e niente accessi al registro (usa `own_entities`, che è già in cache).

### Web UI
- Niente framework e niente passaggi di build: i file in `web/` finiscono come sono nel firmware (`EMBED_FILES` in `main/CMakeLists.txt`). Un file nuovo va aggiunto lì e in `bed_api.c`.
- Ogni `$("id")` in `index.js` deve esistere in `index.html`.
- `alert`, `confirm` e `prompt` non si usano: le conferme sono riquadri nella pagina.
- La web UI viaggia nel `.bin`: un OTA aggiorna firmware e interfaccia insieme. Il flash via USB serve solo se cambia `partitions.csv`; in quel caso segnalalo nelle note di rilascio.

## Stile

- Testi visibili all'utente (display, web UI, entità HA, documentazione) in **italiano**. Commenti, identificatori e messaggi d'errore dell'API in **inglese**.
- C: C11/gnu, 4 spazi, righe fino a ~110 colonne. Le funzioni `static` non esposte non hanno prefisso; quelle pubbliche hanno il prefisso del modulo (`projector_`, `bed_ui_`…). Commenti brevi che spiegano il *perché*.
- Python: annotazioni di tipo, `from __future__ import annotations`, chiavi delle opzioni da `const.py` e non come stringhe letterali.
- Niente codice morto, di compatibilità o di migrazione: il codice descrive solo come funziona il progetto oggi.

## Ricette frequenti

**Aggiungere un dato inviato da Home Assistant**
1. Campo in `projector_ha_t` (`main/projector.h`) e parsing in `parse_ha()` (`main/projector.c`).
2. Valore in `make_snapshot()` (`protocol.py`), con lo stesso limite in byte, ed eventuale opzione in `const.py`, `config_flow.py` e `translations/*.json`.
3. Visualizzazione in `bed_ui.c` e scenario in `tools/preview/preview.c`.
4. Test in `tests/test_protocol.py`, documentazione in `docs/api.md` e `docs/guida.md`.

**Aggiungere un endpoint**: un `json_route_t` con `set` (applica il JSON) e `respond` (costruisce la risposta) e una riga in `routes[]` di `bed_api_start()`. `max_uri_handlers` si adegua da solo.

**Aggiungere un widget del modello Elenco**: valore in fondo all'enum `WIDGET_*` e nome in `projector_widgets[]` (`projector_logic.c/.h`), etichetta, valore e icona in `text_widget()` (`bed_ui.c`), etichetta in `widgetLabels` (`index.js`) e tabella in `docs/guida.md`.

**Aggiungere un elemento o una modalità a una pagina**: lo schema sta in `projector_logic.c/.h` (enum degli elementi e dei modi, tabelle `*_elements[]`); la modalità di indice 0 è sempre la predefinita, così una pagina azzerata resta com'era. Al massimo `PROJECTOR_MAX_ELEMENTS` elementi per modello. Poi il rendering in `bed_ui.c` (letto da `page->show[...]`), lo stesso schema con le etichette in `layoutElements` (`index.js`, stesso ordine), uno scenario in `tools/preview/preview.c` e le tabelle in `docs/api.md` e `docs/guida.md`.

**Cambiare l'aspetto delle pagine**: esegui `tools/render_preview.py` prima e dopo, e confronta le PNG (o i `.rgb565` con `cmp`) per verificare che cambi solo ciò che volevi.

## Segreti e file generati

- `.env` (Wi-Fi e `API_TOKEN`) è ignorato da Git. Non leggerlo, stamparlo o copiarlo in output, log o commit. Per le prove usa valori finti, come fa la CI.
- `build/bed_build_config.h` è generato da `tools/generate_build_config.py` e contiene i segreti: non va committato.
- `build/` e `managed_components/` non sono versionati. `sdkconfig` è versionato: modificalo con `idf.py menuconfig` e non a mano, e spiega il motivo nel commit.

## Prima di consegnare

1. Test Python, test C della logica e `py_compile` passano.
2. Se hai toccato `main/`, il firmware compila (in locale con IDF 6.1.x oppure in CI). Se hai toccato `bed_ui.c`, le anteprime sono state controllate.
3. La documentazione per l'utente è aggiornata quando cambia un comportamento visibile.
4. Le prove che richiedono la scheda (Wi-Fi, LED, OTA, proiezione) o un Home Assistant reale vanno **dichiarate come non eseguite**, indicando il punto corrispondente di `docs/hardware-check.md`. Non presentarle come verificate.
5. Commit piccoli, uno per area (firmware, integrazione, web UI, documentazione), solo quando richiesti.
