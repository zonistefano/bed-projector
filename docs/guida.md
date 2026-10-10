# Installazione e uso di Bed Projector

Questa guida parte da una scheda da programmare e arriva alla configurazione delle pagine in Home Assistant. Per i comandi HTTP e i formati JSON vedi la [specifica API](api.md).

## 1. Preparazione

### Hardware e rete

- ESP32-S3 con flash da **almeno 4 MB**, display ST7735S da 128×128 pixel e LED di proiezione già pilotato dall'hardware attuale. La tabella delle partizioni del progetto è fissa per 4 MB.
- Porta USB/seriale per programmare la scheda. Se usi un adattatore seriale esterno, i segnali logici devono essere a 3,3 V.
- Una rete Wi-Fi domestica raggiungibile da Home Assistant. Il dispositivo usa HTTP locale sulla porta 80; non inoltrare questa porta su Internet.
- Per sincronizzare l'ora, il firmware interroga `pool.ntp.org`: servono DNS e accesso a un server NTP. Senza ora valida il display mostra `--:--` finché la sincronizzazione non riesce.

Il firmware usa questi pin della scheda attuale; verifica il cablaggio prima del flash su hardware diverso:

| Funzione | GPIO |
| --- | ---: |
| LED di proiezione, PWM | 3 |
| LCD MOSI | 10 |
| LCD CS | 11 |
| LCD DC | 12 |
| LCD SCLK | 13 |
| LCD RESET | 9 |

Non è richiesto un sensore di luce. Il meteo arriva da Home Assistant.

### Strumenti

Installa **ESP-IDF 5.4.4** e i suoi strumenti per ESP32-S3 seguendo la [guida Espressif per Linux e macOS](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/linux-macos-setup.html) o il [percorso di installazione Windows della stessa versione](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/get-started/index.html). In ogni nuovo terminale attiva l'ambiente ESP-IDF prima di usare `idf.py`; su Linux/macOS, dalla cartella ESP-IDF:

```sh
. ./export.sh
```

Il repository contiene `sdkconfig`, `partitions.csv` e le dipendenze dichiarate in `main/idf_component.yml`. Esegui i comandi seguenti dalla radice del repository, dove si trova `CMakeLists.txt`.

## 2. Configurazione nel `.env` e installazione USB

Nella radice del progetto copia `env.example` in `.env` **solo se non hai già un `.env`**. Modifica queste tre righe nel file locale:

```dotenv
WIFI_SSID=nome-della-tua-rete
WIFI_PASSWORD=password-della-tua-rete
API_TOKEN=incolla-qui-un-token-di-32-caratteri-esadecimali
```

| Chiave | Significato |
| --- | --- |
| `WIFI_SSID` | Rete Wi-Fi domestica a cui il proiettore si collega al primo avvio; massimo 32 byte UTF-8. |
| `WIFI_PASSWORD` | Password della rete; 8–63 byte, oppure vuota per una rete aperta. |
| `API_TOKEN` | Password **per web UI, API e Home Assistant**. Deve contenere esattamente 32 caratteri esadecimali minuscoli (`0–9`, `a–f`). |

Per creare un token casuale esegui `python3 -c 'import secrets; print(secrets.token_hex(16))'` e incolla il risultato dopo `API_TOKEN=`. Puoi racchiudere SSID e password tra virgolette se contengono spazi. Il file `.env` è ignorato da Git; non pubblicare nemmeno i file di build, perché il firmware contiene le credenziali. Le vecchie chiavi `.env` per meteo o rete del produttore sono ignorate.

Con il file pronto, dalla radice del repository:

```sh
idf.py build
idf.py -p PORT flash monitor
```

Sostituisci `PORT` con la porta seriale della scheda, per esempio `/dev/cu.usbmodem...` su macOS, `/dev/ttyACM0` su Linux o `COM3` su Windows. Il comando `flash` programma bootloader, partizioni, firmware **e immagine SPIFFS** della web UI. Esci dal monitor con `Ctrl+]`.

La build verifica le tre chiavi senza stampare i valori e produce `build/bed-projector.bin` per l'aggiornamento OTA e `build/spiffs.bin` per l'interfaccia web. Prima di flashare controlla che il modulo abbia almeno 4 MB di flash. Se la capacità è diversa, adatta la tabella delle partizioni prima del flash.

Su una scheda Frixos esistente, le credenziali Wi-Fi del `.env` sostituiscono quelle precedenti al primo avvio di questa build. Il firmware conserva fuso orario e luminosità notturna valida dalla vecchia NVS. Se cambi `WIFI_SSID` o `WIFI_PASSWORD` nel `.env` e ricompili, il nuovo firmware aggiorna la rete al riavvio. Un cambio della rete fatto dalla web UI resta memorizzato finché i valori Wi-Fi del `.env` non cambiano. Cambiare `API_TOKEN` nel `.env` e ricompilare cambia il token del dispositivo: aggiorna lo stesso valore anche in Home Assistant.

**Ripartenza completa:** `idf.py -p PORT erase-flash` cancella Wi-Fi, token, pagine, luminosità e vecchie impostazioni nella NVS; esegui poi di nuovo `idf.py -p PORT flash monitor`. Il firmware riprenderà Wi-Fi e token dal `.env` compilato, ma le altre impostazioni torneranno ai valori predefiniti.

## 3. Primo avvio e accesso

1. Dopo il flash, il proiettore prova direttamente `WIFI_SSID` e `WIFI_PASSWORD` del `.env`. Se la connessione riesce, mostra l'ora. Non devi collegarti a un access point né premere **Abbina**.
2. Apri `http://bed-projector.local` nel browser, oppure trova l'IP del dispositivo nella lista DHCP del router e apri `http://IP`. Alla schermata **Accesso**, incolla **lo stesso `API_TOKEN` del `.env`** e premi **Accedi**.
3. Copia lo stesso token nella configurazione di Home Assistant descritta nella sezione successiva. Non esiste un secondo token da generare o annotare.

Se `bed-projector.local` non si risolve, usa l'IP del router; conviene riservarlo nel DHCP. Il token viene salvato dal browser separatamente per ogni indirizzo (`bed-projector.local`, IP, `192.168.4.1`), quindi potresti doverlo reinserire quando cambi indirizzo. Puoi sempre leggerlo nel tuo `.env`.

### Access point di recupero

Se `WIFI_SSID` è vuoto, il firmware usa la configurazione tramite access point. Se invece la rete configurata non si connette entro circa 20 secondi, avvia l'AP di recupero `bed-projector`. La password casuale dell'AP viene mostrata sul proiettore **e nella console seriale**, nella riga `Configuration Wi-Fi AP password: ...` (`idf.py -p PORT monitor`). Questa password serve soltanto a collegarti all'AP: **non è `API_TOKEN`**.

Una volta collegato all'AP, apri `http://192.168.4.1`. Se hai definito `API_TOKEN` nel `.env`, inseriscilo in **Accesso**. Solo se non avevi configurato alcun token, usa **Abbina** per ottenere quello casuale generato dal dispositivo. Puoi correggere la rete dalla web UI; per una configurazione definitiva nel `.env`, correggi il file, ricompila e riflasha. Se la proiezione era stata salvata su **off**, la password AP potrebbe non essere visibile sul proiettore, ma rimane disponibile sulla seriale.

La password dell'AP è mostrata in due righe consecutive da otto caratteri: uniscile senza spazi. La web UI non restituisce la password della rete domestica memorizzata: per modificarla dal browser o cambiare il fuso orario devi reinserire SSID e password. Il fuso predefinito per l'Italia è `CET-1CEST,M3.5.0,M10.5.0/3`.

## 4. Home Assistant

Questa versione usa il componente incluso nel repository. Il componente Frixos pubblico usa un'altra API e non è adatto a questo firmware.

### Installazione tramite HACS

1. Con HACS già configurato e questa versione pubblicata nel ramo predefinito del repository GitHub pubblico, apri **HACS → ⋮ → Repository personalizzati**, inserisci `https://github.com/zonistefano/bed-projector` e scegli il tipo **Integrazione**.
2. Apri **Bed Projector** in HACS, premi **Scarica** e riavvia Home Assistant al termine. HACS copia `custom_components/bed_projector` nella cartella di configurazione di Home Assistant.

Per installare senza HACS, copia **l'intera cartella** `custom_components/bed_projector` del repository in `<cartella di configurazione HA>/custom_components/bed_projector` e riavvia Home Assistant. Alla fine deve esistere il file `<cartella di configurazione HA>/custom_components/bed_projector/manifest.json`. La [documentazione Home Assistant](https://developers.home-assistant.io/docs/creating_integration_file_structure/) descrive questa posizione per le integrazioni personalizzate.

### Configurazione

1. In Home Assistant apri **Impostazioni → Dispositivi e servizi → Aggiungi integrazione**, cerca **Bed Projector** e inserisci host/IP, porta `80` e token.
2. Incolla il valore `API_TOKEN` del tuo `.env`. È lo stesso usato per accedere alla web UI; non serve abbinarne uno diverso. Se sei già autenticato nella web UI, puoi anche selezionarlo da **Home Assistant → Mostra e seleziona token**. Conservalo come una password.
3. Apri **Configura** nelle opzioni dell'integrazione e scegli le entità da proiettare:

| Opzione | Cosa selezionare | Risultato |
| --- | --- | --- |
| Sensore numero porte/finestre aperte | Un `sensor` numerico, per esempio `sensor.number_open_contacts` | Il valore intero (0–99) è il numero di aperture. Se il sensore è indisponibile o non numerico, il conteggio è sconosciuto. |
| Porte/finestre da elencare per nome | Facoltativo: i `binary_sensor` dei contatti | Nomi mostrati nella pagina Casa per quelli aperti. Se vuoto, l'integrazione usa tutti i `binary_sensor` di classe porta, finestra, porta garage o apertura. |
| Luci da elencare | Facoltativo: entità `light` | Nomi delle luci accese nella pagina Casa e loro numero nella pagina Ora. Se vuoto, tutte le luci tranne i gruppi. La luce del proiettore è sempre esclusa. |
| Centrale d'allarme | Un `alarm_control_panel` | Distingue disinserito, in casa, fuori casa, notte, vacanza, personalizzato, inserimento, attesa e scattato. |
| Entità meteo | Una `weather`, per esempio `weather.pirateweather` | Condizioni attuali, umidità, vento e temperatura percepita. L'integrazione legge con `weather.get_forecasts` le previsioni giornaliere, orarie e a 12 ore (aggiornate ogni 10 minuti). |
| Temperatura camera da letto | Un `sensor` di temperatura | Pagina Casa, con un decimale. |
| Temperatura esterna | Facoltativo: un `sensor` di temperatura | Pagina Casa; se vuoto usa la temperatura dell'entità meteo. |
| Riepilogo meteo di oggi / di domani | Facoltativo: sensori di testo, per esempio `sensor.pirateweather_summary_0d` e `sensor.pirateweather_summary_1d` | Frase che scorre in fondo alla pagina Meteo. |
| Dati aggiuntivi 1–4 | Fino a quattro entità HA | Nome, valore e unità vengono mostrati dai widget `entity1`–`entity4` del modello Elenco. |

Alba e tramonto sono calcolati da Home Assistant per la posizione configurata in **Impostazioni → Sistema → Generale**: non servono sensori. La parte **giorno** della previsione riassume le ore 8–18, la **sera** le ore 18–24 della previsione oraria: condizione più frequente (a parità, la più severa), temperatura massima di giorno e media di sera, probabilità di pioggia massima. Se il servizio meteo non fornisce la previsione oraria, il giorno usa quella giornaliera e la sera quella notturna a 12 ore.

Chi aggiorna da una versione precedente (lista di `binary_sensor`) deve riaprire **Configura** e scegliere il sensore numerico: fino ad allora le aperture restano `?`.

Senza sensore configurato il conteggio aperture resta `?`; senza centrale d'allarme lo scudo resta grigio scuro; senza entità meteo la riga della previsione non compare. Puoi configurare solo i dati che hai disponibili. I widget aggiuntivi non configurati mostrano **Dato HA ?**. Il numero di luci accese è contato dall'elenco, non da un sensore di conteggio: un aiutante come `sensor.number_lights_on` includerebbe anche la proiezione stessa ogni volta che è accesa. Accendere o spegnere una luce o aprire un contatto dell'elenco aggiorna subito il display.

L'integrazione espone una **luce** (accensione e luminosità), una **selezione pagina**, i pulsanti **Pagina precedente** e **Pagina successiva**, il numero **Ritorno alla pagina Ora** (secondi, 0 = mai) e sensori per aperture, allarme, attività e memoria libera. Le automazioni HA possono usare direttamente queste entità. Per esempio, un'automazione che si attiva quando suona la sveglia può selezionare la pagina Meteo; trascorso il tempo di ritorno il proiettore torna all'ora.

Home Assistant invia lo stato iniziale, i cambiamenti delle entità selezionate e un riepilogo ogni 60 secondi. Se non arrivano aggiornamenti per 120 secondi, il proiettore mostra `?` per i dati domotici ma continua a mostrare l'ora.

## 5. Uso quotidiano

### Le pagine

Le pagine usano Montserrat (testo in SemiBold, temperature in Bold, con le lettere accentate) e le stesse Material Design Icons di Home Assistant. Ogni elemento è centrato nel cerchio di proiezione: i testi più lunghi dello spazio disponibile scorrono lentamente, quelli brevi si rimpiccioliscono invece di scorrere.

**Ora** (sempre la prima pagina), dall'alto:

1. Icona della previsione, massima (arancione) e minima (azzurra) in grassetto. Fino alle 20:59 è la previsione di oggi, dalle 21:00 quella di domani. La riga manca se il meteo non è configurato o i dati sono scaduti.
2. L'ora.
3. Icone di stato, solo quando servono: porta aperta con il numero di aperture, lampadina con il numero di luci accese, scudo dell'allarme se non è disinserito.
4. Riga che scorre con la data (es. `Sab 10 Ottobre`), l'ora dell'alba e quella del tramonto.

**Casa**: modalità dell'allarme con icona e nome, temperatura della camera (icona letto) e quella esterna (icona albero), poi una riga con i nomi delle porte/finestre aperte (`Finestra Sala · Finestra Cucina`, oppure **Tutto chiuso**) e una con i nomi delle luci accese (oppure **Luci spente**). Se gli elenchi superano otto nomi compare `+N`.

**Meteo**: icona e temperatura attuali, una riga che scorre con condizione, temperatura percepita, umidità e vento; sotto il titolo **OGGI** (o **DOMANI** dalle 21:00) le colonne **Giorno** e **Sera** con icona e temperatura; in fondo, a scorrimento, probabilità di pioggia (quando è almeno 10%) e il riepilogo della giornata.

| Indicazione | Significato |
| --- | --- |
| Icona porta aperta con `1`, `2`, ... | Numero di porte/finestre aperte; con zero aperture l'icona non compare |
| Icona porta grigia con `?` | Nessun sensore configurato, sensore indisponibile o dati scaduti |
| Lampadina gialla con numero | Luci accese; con zero luci l'icona non compare |
| `mdi:shield-off` | Centrale disinserita; nella pagina Ora non compare |
| `mdi:shield-home` | Inserito in casa (`armed_home`); giallo |
| `mdi:shield-lock` | Inserito fuori casa (`armed_away`); arancione |
| `mdi:shield-moon` | Inserito notte (`armed_night`); blu |
| `mdi:shield-airplane` | Inserito vacanza (`armed_vacation`); arancione |
| `mdi:security` | Inserito personalizzato (`armed_custom_bypass`); arancione |
| `mdi:shield` giallo | In fase di inserimento (`arming`) |
| `mdi:shield-outline` | Ritardo d'ingresso prima dello scatto (`pending`); rosso-arancio |
| `mdi:bell-ring` | Allarme scattato; rosso |
| `mdi:shield` grigio scuro | Stato assente o scaduto |

Un orario `--:--` e la scritta **In attesa dell'ora** indicano che la sincronizzazione NTP non è ancora riuscita.

Nella web UI la sezione **Proiezione** consente di accendere o spegnere la luce, scegliere la luminosità 1–100%, selezionare una pagina o scorrere. In Home Assistant usa la luce e la selezione pagina equivalenti. **Off** spegne la proiezione impostando il PWM a zero, ma la rete, il display e le API restano attivi; **On** ripristina l'ultimo livello. Stato e livello sopravvivono al riavvio.

### Centrare e orientare l'area circolare

Nella web UI apri **Centraggio e orientamento** e premi **Avvia regolazione**. Il display mostra una croce, la circonferenza che delimita l'area usata da tutte le pagine e la scritta **SU**. Usa le frecce per spostare il centro di un pixel alla volta; **Riduci** e **Aumenta** cambiano il diametro di due pixel. Il cerchio resta interamente nei 128×128 pixel del display: se una direzione non è disponibile, riduci prima il diametro. **Ruota 90°** gira il contenuto di un quarto di giro alla volta; **Specchia** lo ribalta, utile quando la lente o uno specchio invertono l'immagine. Regola finché la scritta **SU** è leggibile e rivolta verso l'alto. Rotazione e specchiatura non spostano il cerchio, quindi il centraggio resta valido. Alla fine premi **Salva regolazione**; **Annulla** ripristina geometria e orientamento precedenti. Solo il salvataggio scrive nella memoria persistente.

Il diametro iniziale è 112 pixel, centrato su (64, 64); puoi impostarlo tra 80 e 128 pixel. Fuori dal cerchio i pixel restano neri. Le pagine, inclusa la schermata di configurazione Wi-Fi, seguono centro, diametro e orientamento salvati. Durante la regolazione il LED si accende almeno al 30% anche se la proiezione era spenta; alla chiusura torna allo stato precedente. Questa funzione è disponibile dalla web UI locale e non crea entità o comandi Home Assistant.

### Modificare le pagine

Apri **Pagine** nella web UI. Puoi rinominare le pagine, scegliere il modello delle pagine secondarie, spostarle con **Su/Giù**, aggiungerne o rimuoverne fino a un totale di cinque. Premi **Salva pagine** alla fine.

La pagina Ora resta prima. Le altre usano il modello **Casa**, **Meteo** (design fissi descritti sopra) o **Elenco**, che mostra il nome della pagina e fino a tre widget:

| Widget | Contenuto |
| --- | --- |
| `openings` | Conteggio porte/finestre aperte |
| `alarm` | Stato della centrale |
| `date` | Data locale |
| `weather` | Condizione e temperatura da HA |
| `entity1`–`entity4` | Dati aggiuntivi scelti nelle opzioni HA |
| `text` | Testo statico inserito nel campo **Testo** della pagina |

Le pagine preconfigurate sono **Ora**, **Casa** e **Meteo**. Le configurazioni salvate da versioni precedenti restano valide: le pagine con modello `home` e `weather` mostrano i nuovi design e i loro widget vengono ignorati. Il nome della pagina è lungo al massimo 16 byte e il testo statico 32 byte: con caratteri accentati il limite in caratteri può essere inferiore.

L'ordine delle pagine definisce **precedente/successiva**. Selezionando una pagina secondaria da web UI o HA, la pagina Ora torna automaticamente dopo il tempo impostato in **Proiezione → Ritorno alla pagina Ora** (web UI) o con l'entità **Ritorno alla pagina Ora** (HA): da 5 a 3600 secondi, predefinito 30; con `0` la pagina scelta resta visibile. Il tempo resta salvato al riavvio, la pagina scelta no.

## 6. Aggiornamenti

Per aggiornare il firmware via rete, compila la nuova versione con `idf.py build`, apri la web UI, scegli `build/bed-projector.bin` nella sezione **Aggiornamento firmware** e premi **Carica firmware**. Il file deve essere un'immagine ESP32-S3 valida di questo progetto e rientrare nello slot OTA. Al termine il dispositivo si riavvia. L'OTA è autenticato con il token; un'immagine rifiutata non diventa la partizione di avvio. Il bootloader ha il rollback abilitato per le immagini che non completano correttamente l'avvio.

**L'OTA web aggiorna solo il firmware applicativo.** L'interfaccia web sta nella partizione SPIFFS: se una release cambia anche `spiffs/`, usa `idf.py -p PORT flash` via USB per aggiornare insieme firmware e web UI. Non caricare `spiffs.bin` nel campo OTA, che accetta soltanto `bed-projector.bin`.

Un flash USB ordinario conserva NVS. `erase-flash` la cancella, come spiegato nella sezione 2.

## 7. API e sicurezza

Le chiamate operative e la web UI usano `API_TOKEN` del `.env` nell'header `Authorization: Bearer`. Se nel `.env` non sono configurati né Wi-Fi né token, il dispositivo usa un token NVS già presente o ne genera uno casuale; `GET /api/v1/pair` lo restituisce dalla rete protetta di configurazione. Le risposte di stato e configurazione non restituiscono password Wi-Fi o token. Vedi [endpoint ed esempi](api.md).

Il browser salva il token nel proprio archivio locale. **Rimuovi token da questo browser** lo elimina da quel browser; non cambia il token memorizzato nel proiettore e non disconnette Home Assistant. La comunicazione HTTP non cifra il traffico: usa il dispositivo sulla rete locale fidata e non pubblicare la porta 80 su Internet.

## 8. Problemi e recupero

| Sintomo | Controllo e azione |
| --- | --- |
| `bed-projector.local` non si apre | Trova l'IP nel router e prova `http://IP`. Verifica che il client sia sulla stessa rete e che non sia isolato dal Wi-Fi. |
| Il dispositivo mostra `--:--` | Controlla Wi-Fi, DNS e accesso NTP a `pool.ntp.org`; attendi la sincronizzazione. |
| Aperture, luci, allarme o meteo mostrano `?` | Configura le entità nelle opzioni HA, verifica che siano disponibili e che HA possa raggiungere l'IP del proiettore. Dopo 120 secondi senza snapshot i dati scadono. |
| La pagina torna da sola a Ora | È il comportamento previsto: regola o disattiva (`0`) il **Ritorno alla pagina Ora** dalla web UI o da HA. |
| L'access point non compare | Con Wi-Fi valido nel `.env` il dispositivo si collega direttamente: è il comportamento previsto. Per forzare l'AP, rendi temporaneamente indisponibile quella rete e riavvia; attendi circa 20 secondi. |
| `/api/v1/pair` restituisce `403` | Accedi da un client collegato all'AP `bed-projector` usando `http://192.168.4.1`; dalla LAN l'abbinamento è disabilitato. |
| Dopo il cambio Wi-Fi il browser richiede di nuovo il token | È normale: l'indirizzo del sito è cambiato. Reinserisci `API_TOKEN` dal `.env`. |
| Una chiamata API restituisce `401` | Il token manca o non coincide con `API_TOKEN` compilato. Correggi il browser o l'integrazione HA; se hai modificato `.env`, ricompila e riflasha il dispositivo. |
| Ho perso il token e la proiezione è spenta | Se il token era nel `.env`, recuperalo lì. Se usavi il token casuale senza `.env`, riaccendi da HA se possibile, oppure usa la seriale per la password AP e **Abbina**; come ultima possibilità cancella NVS e riflasha. |
| La web UI non mostra le novità dopo OTA | OTA aggiorna solo l'applicazione; aggiorna SPIFFS con `idf.py -p PORT flash` via USB. |
| `idf.py` segnala un percorso Python diverso da quello della build precedente | Attiva la stessa installazione ESP-IDF/Python usata per configurare il progetto. Se stai cambiando ambiente di sviluppo, esegui `idf.py fullclean` e poi `idf.py build`; questo rigenera `build/` senza cancellare NVS sul dispositivo. |

Per una verifica completa dopo l'installazione usa la [checklist hardware](hardware-check.md).
