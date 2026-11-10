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

Installa **ESP-IDF 6.1** e i suoi strumenti per ESP32-S3 seguendo la [guida Espressif per Linux e macOS](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/linux-macos-setup.html) o il [percorso di installazione Windows della stessa versione](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html). In ogni nuovo terminale attiva l'ambiente ESP-IDF prima di usare `idf.py`; su Linux/macOS, dalla cartella ESP-IDF:

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

Per creare un token casuale esegui `python3 -c 'import secrets; print(secrets.token_hex(16))'` e incolla il risultato dopo `API_TOKEN=`. Puoi racchiudere SSID e password tra virgolette se contengono spazi. Il file `.env` è ignorato da Git; non pubblicare nemmeno i file di build, perché il firmware contiene le credenziali. Le altre chiavi del `.env` sono ignorate.

Con il file pronto, dalla radice del repository:

```sh
idf.py build
idf.py -p PORT flash monitor
```

Sostituisci `PORT` con la porta seriale della scheda, per esempio `/dev/cu.usbmodem...` su macOS, `/dev/ttyACM0` su Linux o `COM3` su Windows. Il comando `flash` programma bootloader, partizioni e firmware, che contiene anche la web UI. Esci dal monitor con `Ctrl+]`.

La build verifica le tre chiavi senza stampare i valori e produce `build/bed-projector.bin`, il file da usare per l'aggiornamento OTA. Prima di flashare controlla che il modulo abbia almeno 4 MB di flash. Se la capacità è diversa, adatta la tabella delle partizioni prima del flash.

Se cambi `WIFI_SSID` o `WIFI_PASSWORD` nel `.env` e ricompili, il nuovo firmware aggiorna la rete al riavvio. Un cambio della rete fatto dalla web UI resta memorizzato finché i valori Wi-Fi del `.env` non cambiano. Cambiare `API_TOKEN` nel `.env` e ricompilare cambia il token del dispositivo: aggiorna lo stesso valore anche in Home Assistant.

**Ripartenza completa:** `idf.py -p PORT erase-flash` cancella Wi-Fi, token, pagine, luminosità e calibrazione nella NVS; esegui poi di nuovo `idf.py -p PORT flash monitor`. Il firmware riprenderà Wi-Fi e token dal `.env` compilato, ma le altre impostazioni torneranno ai valori predefiniti.

## 3. Primo avvio e accesso

1. Dopo il flash, il proiettore prova direttamente `WIFI_SSID` e `WIFI_PASSWORD` del `.env`. Se la connessione riesce, mostra l'ora. Non devi collegarti a un access point né premere **Abbina**.
2. Apri `http://bed-projector.local` nel browser, oppure trova l'IP del dispositivo nella lista DHCP del router e apri `http://IP`. Alla schermata **Accesso**, incolla **lo stesso `API_TOKEN` del `.env`** e premi **Accedi**.
3. Copia lo stesso token nella configurazione di Home Assistant descritta nella sezione successiva. Non esiste un secondo token da generare o annotare.

Se `bed-projector.local` non si risolve, usa l'IP del router; conviene riservarlo nel DHCP. Il token viene salvato dal browser separatamente per ogni indirizzo (`bed-projector.local`, IP, `192.168.4.1`), quindi potresti doverlo reinserire quando cambi indirizzo. Puoi sempre leggerlo nel tuo `.env`.

### Access point di recupero

Se `WIFI_SSID` è vuoto, il firmware usa la configurazione tramite access point. Se invece la rete configurata non si connette entro circa 20 secondi, avvia l'AP di recupero `bed-projector`. La password casuale dell'AP viene mostrata sul proiettore **e nella console seriale**, nella riga `Configuration Wi-Fi AP password: ...` (`idf.py -p PORT monitor`). Questa password serve soltanto a collegarti all'AP: **non è `API_TOKEN`**.

Una volta collegato all'AP, apri `http://192.168.4.1`. Se hai definito `API_TOKEN` nel `.env`, inseriscilo in **Accesso**. Solo se non avevi configurato alcun token, usa **Abbina** per ottenere quello casuale generato dal dispositivo. Puoi correggere la rete dalla web UI (**Salva rete** chiede conferma prima di riavviare il dispositivo); per una configurazione definitiva nel `.env`, correggi il file, ricompila e riflasha. Se la proiezione era stata salvata su **off**, la password AP potrebbe non essere visibile sul proiettore, ma rimane disponibile sulla seriale.

La password dell'AP è mostrata in due righe consecutive da otto caratteri: uniscile senza spazi. La web UI non restituisce la password della rete domestica memorizzata: per modificarla dal browser o cambiare il fuso orario devi reinserire SSID e password. Il fuso predefinito per l'Italia è `CET-1CEST,M3.5.0,M10.5.0/3`.

## 4. Home Assistant

### Installazione tramite HACS

1. Con HACS già configurato e questa versione pubblicata nel ramo predefinito del repository GitHub pubblico, apri **HACS → ⋮ → Repository personalizzati**, inserisci `https://github.com/zonistefano/bed-projector` e scegli il tipo **Integrazione**.
2. Apri **Bed Projector** in HACS, premi **Scarica** e riavvia Home Assistant al termine. HACS copia `custom_components/bed_projector` nella cartella di configurazione di Home Assistant.

Per installare senza HACS, copia **l'intera cartella** `custom_components/bed_projector` del repository in `<cartella di configurazione HA>/custom_components/bed_projector` e riavvia Home Assistant. Alla fine deve esistere il file `<cartella di configurazione HA>/custom_components/bed_projector/manifest.json`. La [documentazione Home Assistant](https://developers.home-assistant.io/docs/creating_integration_file_structure/) descrive questa posizione per le integrazioni personalizzate.

### Configurazione

1. Se Home Assistant è sulla stessa rete, il proiettore acceso compare da solo in **Impostazioni → Dispositivi e servizi** come **Rilevato: Bed Projector**: premi **Aggiungi** e inserisci il token. Altrimenti apri **Aggiungi integrazione**, cerca **Bed Projector** e inserisci host/IP, porta `80` e token. Un errore indica se il dispositivo non risponde o se il token è sbagliato. Il proiettore è riconosciuto dal suo identificativo, non dall'indirizzo: se il router gli assegna un nuovo IP, Home Assistant aggiorna da solo la configurazione appena lo rileva di nuovo, e aggiungerlo una seconda volta non crea un doppione.
2. Incolla il valore `API_TOKEN` del tuo `.env`. È lo stesso usato per accedere alla web UI; non serve abbinarne uno diverso. Se sei già autenticato nella web UI, puoi anche selezionarlo da **Home Assistant → Mostra e seleziona token**. Conservalo come una password.
3. Apri **Configura** nelle opzioni dell'integrazione e scegli le entità da proiettare:

| Opzione | Cosa selezionare | Risultato |
| --- | --- | --- |
| Porte/finestre da elencare | Facoltativo: i `binary_sensor` dei contatti | Nomi di quelli aperti nella pagina Casa e loro numero nella pagina Ora. Se vuoto, l'integrazione usa tutti i `binary_sensor` di classe porta, finestra, porta garage o apertura. |
| Template porte/finestre aperte | Facoltativo: un template Jinja | Se compilato sostituisce l'elenco precedente: nomi e numero di aperture sono quelli prodotti dal template (vedi sotto). |
| Luci da elencare | Facoltativo: entità `light` | Nomi delle luci accese nella pagina Casa e loro numero nella pagina Ora. Se vuoto, tutte le luci tranne i gruppi. La luce del proiettore è sempre esclusa. |
| Template luci accese | Facoltativo: un template Jinja | Se compilato sostituisce l'elenco delle luci: nomi e numero di luci accese sono quelli prodotti dal template (vedi sotto). La luce del proiettore va esclusa nel template. |
| Centrale d'allarme | Un `alarm_control_panel` | Distingue disinserito, in casa, fuori casa, notte, vacanza, personalizzato, inserimento, attesa e scattato. |
| Entità meteo | Una `weather`, per esempio `weather.pirateweather` | Condizioni attuali, umidità, vento e temperatura percepita. L'integrazione legge con `weather.get_forecasts` le previsioni giornaliere, orarie e a 12 ore (aggiornate ogni 10 minuti). |
| Temperatura camera da letto | Un `sensor` di temperatura | Pagina Casa, con un decimale. |
| Temperatura esterna | Facoltativo: un `sensor` di temperatura | Pagina Casa; se vuoto usa la temperatura dell'entità meteo. |
| Riepilogo meteo di oggi / di domani | Facoltativo: sensori di testo, per esempio `sensor.pirateweather_summary_0d` e `sensor.pirateweather_summary_1d` | Frase che scorre in fondo alla pagina Meteo. |
| Prossima sveglia | Facoltativo: un `sensor` timestamp, per esempio `sensor.<telefono>_next_alarm` della Companion App, oppure un `input_datetime`. Con sola ora la sveglia vale tutti i giorni (la prossima è la prima volta che l'orologio arriva a quell'ora); con data e ora vale per quel momento. Un `input_datetime` con sola data non è accettato e appare come `?` | Ora e giorno della prossima sveglia nella pagina Agenda e nel widget `wake`; icona della sveglia di domattina nella pagina Ora. Le sveglie già passate o oltre sette giorni non compaiono. |
| Calendari | Facoltativo: una o più entità `calendar` | Fino a tre eventi da adesso alla fine di domani nella pagina Agenda; il primo anche nel widget `event`. Gli eventi sono riletti ogni 10 minuti e quando cambia lo stato di un calendario. |
| Termostato caloriferi (inverno) | Facoltativo: l'entità `climate` dei caloriferi della camera | Pagina Termostato e widget `thermostat` d'inverno. |
| Termostato condizionatore (estate) | Facoltativo: l'entità `climate` del condizionatore della camera | Pagina Termostato e widget `thermostat` d'estate. |
| Dati aggiuntivi 1–4 | Fino a quattro entità HA | Nome, valore e unità vengono mostrati dai widget `entity1`–`entity4` del modello Elenco. |

Per luci e porte/finestre ci sono tre modi, in ordine di priorità: **template**, **elenco di entità**, oppure nessuno dei due per prenderle **tutte in automatico**. Il template deve produrre i nomi da mostrare separati da virgola (oppure uno per riga, o una lista); un risultato vuoto vale "nessuna". Home Assistant lo ricalcola da solo quando cambiano le entità che legge. Se il template dà errore o restituisce `unknown`/`unavailable`, il display mostra `?`. Un template non valido viene rifiutato già nel form delle opzioni. Esempio per le luci, che esclude i gruppi, le luci della sala (sostituite da due nomi riassuntivi) e la proiezione stessa:

```jinja
{{ (states.light
   | rejectattr('attributes.entity_id', 'defined')
   | rejectattr('attributes.group_entities', 'defined')
   | rejectattr('entity_id', 'search', 'sala_')
   | rejectattr('entity_id', 'search', 'light.camera_da_letto_bed_projector_proiezione')
   | selectattr('state', 'eq', 'on')
   | map(attribute='name')
   | list
   + (['Luci sala'] if is_state('light.luci_sala', 'on') else [])
   + (['Luce sala cassa'] if is_state('light.luce_sala_cassa', 'on') else [])
   ) | join(', ') }}
```

I nomi non devono contenere virgole, che separano un nome dall'altro.

Alba e tramonto sono calcolati da Home Assistant per la posizione configurata in **Impostazioni → Sistema → Generale**: non servono sensori. La parte **giorno** della previsione riassume le ore 8–18, la **sera** le ore 18–24 della previsione oraria: condizione più frequente (a parità, la più severa), temperatura massima di giorno e media di sera, probabilità di pioggia massima. Se il servizio meteo non fornisce la previsione oraria, il giorno usa quella giornaliera e la sera quella notturna a 12 ore.

#### Termostato: caloriferi d'inverno, condizionatore d'estate

La pagina Termostato mostra un solo termostato alla volta. Con una sola delle due entità `climate` configurata mostra sempre quella. Con entrambe, l'integrazione crea l'entità **Stagione termostato** (tra le impostazioni del dispositivo) con tre scelte:

| Scelta | Termostato mostrato |
| --- | --- |
| **Automatica** (predefinita) | Quello acceso (modalità diversa da `off`). Se sono accesi entrambi, quello che sta lavorando (`hvac_action` diverso da `idle`/`off`); se ancora non si capisce, o sono spenti entrambi, i caloriferi da ottobre ad aprile e il condizionatore da maggio a settembre. |
| **Inverno (caloriferi)** | Sempre i caloriferi |
| **Estate (condizionatore)** | Sempre il condizionatore |

La scelta resta salvata al riavvio di Home Assistant e si può cambiare da un'automazione con `select.select_option`. Se il termostato scelto non è disponibile il display mostra `?`: non passa all'altro, che darebbe valori di un apparecchio diverso. La temperatura della stanza è quella dell'entità `climate`; se l'entità non la fornisce (per esempio alcune valvole termostatiche) si usa il sensore **Temperatura camera da letto**. Con una modalità a intervallo (`heat_cool`) la temperatura impostata è il minimo dell'intervallo per i caloriferi e il massimo per il condizionatore.

Il proiettore mostra il termostato ma non lo comanda: per cambiare temperatura o modalità usa Home Assistant.

Senza centrale d'allarme lo scudo resta grigio scuro; senza entità meteo la riga della previsione non compare. Puoi configurare solo i dati che hai disponibili. I widget aggiuntivi non configurati mostrano **Dato HA ?**. I numeri di aperture e di luci accese sono contati dai nomi, non da sensori di conteggio: un aiutante come `sensor.number_lights_on` includerebbe anche la proiezione stessa ogni volta che è accesa. Accendere o spegnere una luce o aprire un contatto dell'elenco aggiorna subito il display.

L'integrazione espone una **luce** (accensione e luminosità), una **selezione pagina**, la selezione **Stagione termostato** (solo con entrambi i termostati configurati), i pulsanti **Pagina precedente** e **Pagina successiva**, il numero **Ritorno alla pagina Ora** (secondi, 0 = mai) e, tra i dati diagnostici, i sensori di attività e memoria libera. Aperture, luci e allarme non sono riesposti: sono dati che il proiettore riceve da Home Assistant stesso. La selezione pagina mostra i nomi delle pagine. Dalla scheda del dispositivo in Home Assistant, **Visita** apre direttamente la web UI. Le automazioni HA possono usare direttamente queste entità. Per esempio, un'automazione che si attiva quando suona la sveglia può selezionare la pagina Meteo; trascorso il tempo di ritorno il proiettore torna all'ora.

### Messaggi dalle automazioni

L'azione **Bed Projector: Mostra messaggio** (`bed_projector.show_message`) proietta un testo, al massimo 63 byte, con un'icona sopra, al posto della pagina corrente. Il messaggio scompare dopo la durata scelta (5–3600 secondi, predefinita 30) e il proiettore torna alla pagina di prima. **Togli messaggio** (`bed_projector.clear_message`) lo toglie subito. Un nuovo messaggio sostituisce il precedente; un riavvio del proiettore lo cancella. Le icone disponibili sono campanella (predefinita), informazione, avviso, lavatrice, pacco, porta aperta, luce accesa, telefono, sveglia e calendario. Il carattere `#` viene tolto dal testo.

```yaml
alias: Avvisa quando la lavatrice ha finito
triggers:
  - trigger: state
    entity_id: sensor.lavatrice_stato
    to: finito
actions:
  - action: bed_projector.show_message
    data:
      config_entry_id: <scegli il proiettore nell'editor visuale>
      message: Lavatrice finita
      icon: washing-machine
      duration: 120
```

Home Assistant invia lo stato iniziale, i cambiamenti delle entità selezionate e un riepilogo ogni 60 secondi. Se non arrivano aggiornamenti per 120 secondi, il proiettore mostra `?` per i dati domotici ma continua a mostrare l'ora.

## 5. Uso quotidiano

### Le pagine

Le pagine usano Montserrat (testo in SemiBold, temperature in Bold, con le lettere accentate) e le stesse Material Design Icons di Home Assistant. Ogni elemento è centrato nel cerchio di proiezione: i testi più lunghi dello spazio disponibile scorrono lentamente, quelli brevi si rimpiccioliscono invece di scorrere.

**Ora** (sempre la prima pagina), dall'alto:

1. Icona della previsione, massima (arancione) e minima (azzurra) in grassetto. Fino alle 20:59 è la previsione di oggi, dalle 21:00 quella di domani. La riga manca se il meteo non è configurato o i dati sono scaduti.
2. L'ora.
3. Icone di stato, solo quando servono: porta aperta con il numero di aperture, lampadina con il numero di luci accese, scudo dell'allarme se non è disinserito. In fondo, se è configurato il sensore della prossima sveglia, una piccola sveglia arancione indica che domattina suona una sveglia (prima delle 12:00), una sveglia barrata grigia che non ce n'è nessuna. Fino alle 11:59 «domattina» è la mattina in corso, da mezzogiorno quella del giorno dopo. Se il sensore non è configurato o non è disponibile l'icona non compare. Da **Proiezione → Sveglia nella pagina Ora** nella web UI si sceglie cosa mostrare: **Solo icona** (predefinito), **Icona e ora della sveglia** (`06:45`), **Icona e tempo mancante** (`7h 55m`, aggiornato ogni minuto) oppure **Niente**. Ora e tempo mancante compaiono solo accanto alla sveglia arancione; se la riga non entra nel cerchio, la sveglia passa su una riga sua e l'ora diventa un po' più piccola. Il tempo mancante si calcola sull'ora del muro, quindi la notte del cambio dell'ora legale è sbagliato di un'ora. La scelta resta salvata al riavvio.
4. Riga che scorre con la data (es. `Sab 10 Ottobre`), l'ora dell'alba e quella del tramonto.

**Casa**: modalità dell'allarme con icona e nome, temperatura della camera (icona letto) e quella esterna (icona albero), poi una riga con i nomi delle porte/finestre aperte (`Finestra Sala · Finestra Cucina`, oppure **Tutto chiuso**) e una con i nomi delle luci accese (oppure **Luci spente**). Se gli elenchi superano otto nomi compare `+N`.

**Agenda**: icona della sveglia con l'ora della prossima sveglia e, sotto, **Oggi**, **Domani** o il giorno della settimana; poi, dopo una linea, fino a tre eventi del calendario, ciascuno con l'ora (o il giorno, se non è oggi) seguita dal titolo, che scorre se è lungo. Gli eventi di tutto il giorno e quelli iniziati prima di oggi non hanno l'ora. Senza sveglia compare **Nessuna sveglia**, senza eventi **Nessun evento**; se il sensore o i calendari non sono configurati, non sono disponibili o i dati sono scaduti compare `?`. L'app Companion per Android rende il sensore della sveglia `unavailable` quando non c'è nessuna sveglia: in quel caso il proiettore mostra **Sveglia ?**.

**Termostato**: una rotella di tacche, come la ghiera di un termostato, arancione per i caloriferi e azzurra per il condizionatore. Le tacche si colorano dall'inizio della scala fino alla temperatura impostata, segnata da una tacca più lunga; una tacca bianca indica la temperatura della stanza. La scala va normalmente da 10 a 30 °C, ristretta ai limiti del termostato e allargata se la temperatura impostata ne esce. Al centro, dall'alto: lo stato con la sua icona (**Riscalda**, **Raffresca**, **In pausa**, **Deumidifica**, **Ventilazione**…, seguito dal preset, per esempio `Eco`), la temperatura impostata in grande e, con l'icona del termometro, quella della stanza; in basso, nello spazio libero della rotella, l'umidità se il termostato la fornisce. Quando il termostato lavora la rotella è accesa, in pausa è più tenue. Da spento compaiono **Spento**, la temperatura della stanza in grigio e il nome dell'apparecchio (**Caloriferi** o **Condizionatore**). Se il termostato non è configurato, non è disponibile o i dati sono scaduti compaiono **Clima ?** e `--°`.

Sulle pagine diverse da Ora, una fila di quadratini in basso indica quante pagine ci sono e quale è mostrata (quella chiara).

**Meteo**: icona e temperatura attuali, una riga che scorre con condizione, temperatura percepita, umidità e vento; sotto il titolo **OGGI** (o **DOMANI** dalle 21:00) le colonne **Giorno** e **Sera** con icona e temperatura; in fondo, a scorrimento, probabilità di pioggia (quando è almeno 10%) e il riepilogo della giornata.

| Indicazione | Significato |
| --- | --- |
| Icona porta aperta con `1`, `2`, ... | Numero di porte/finestre aperte; con zero aperture l'icona non compare |
| Icona porta grigia con `?` | Nessun sensore configurato, sensore indisponibile o dati scaduti |
| Lampadina gialla con numero | Luci accese; con zero luci l'icona non compare |
| `mdi:alarm` arancione | Pagina Ora: c'è una sveglia domattina |
| `mdi:alarm-off` grigia | Pagina Ora: nessuna sveglia domattina |
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

Nella web UI la sezione **Proiezione** consente di accendere o spegnere la luce, scegliere la luminosità 1–100% (si applica appena rilasci il cursore), selezionare una pagina o scorrere. Lo stato si aggiorna da solo ogni 15 secondi. In Home Assistant usa la luce e la selezione pagina equivalenti. **Off** spegne la proiezione impostando il PWM a zero, ma la rete, il display e le API restano attivi; **On** ripristina l'ultimo livello. Stato e livello sopravvivono al riavvio.

### Centrare e orientare l'area circolare

Nella web UI apri **Centraggio e orientamento** e premi **Avvia regolazione**. Il display mostra una croce, la circonferenza che delimita l'area usata da tutte le pagine e la scritta **SU**. Usa le frecce per spostare il centro di un pixel alla volta; **Riduci** e **Aumenta** cambiano il diametro di due pixel. Il cerchio resta interamente nei 128×128 pixel del display: se una direzione non è disponibile, riduci prima il diametro. **Ruota 90°** gira il contenuto di un quarto di giro alla volta; **Specchia** lo ribalta, utile quando la lente o uno specchio invertono l'immagine. Regola finché la scritta **SU** è leggibile e rivolta verso l'alto. Rotazione e specchiatura non spostano il cerchio, quindi il centraggio resta valido. Alla fine premi **Salva regolazione**; **Annulla** ripristina geometria e orientamento precedenti. Solo il salvataggio scrive nella memoria persistente.

Il diametro iniziale è 112 pixel, centrato su (64, 64); puoi impostarlo tra 80 e 128 pixel. Fuori dal cerchio i pixel restano neri. Le pagine, inclusa la schermata di configurazione Wi-Fi, seguono centro, diametro e orientamento salvati. Durante la regolazione il LED si accende almeno al 30% anche se la proiezione era spenta; alla chiusura torna allo stato precedente. Questa funzione è disponibile dalla web UI locale e non crea entità o comandi Home Assistant.

### Modificare le pagine

Apri **Pagine** nella web UI. Puoi rinominare le pagine, scegliere il modello delle pagine secondarie, spostarle con **Su/Giù**, aggiungerne o rimuoverne fino a un totale di cinque. Premi **Salva pagine** alla fine: fino ad allora le modifiche restano una bozza, che non va persa se nel frattempo usi gli altri comandi della pagina. Un nome vuoto o troppo lungo viene segnalato prima dell'invio.

La pagina Ora resta prima. Le altre usano il modello **Casa**, **Meteo**, **Agenda**, **Termostato** (design fissi descritti sopra) o **Elenco**, che mostra il nome della pagina e fino a tre widget:

| Widget (nome nella web UI) | Contenuto |
| --- | --- |
| **Porte/finestre aperte** (`openings`) | Conteggio porte/finestre aperte |
| **Allarme** (`alarm`) | Stato della centrale |
| **Data** (`date`) | Data locale |
| **Meteo attuale** (`weather`) | Condizione in italiano e temperatura da HA |
| **Dato HA 1–4** (`entity1`–`entity4`) | Dati aggiuntivi scelti nelle opzioni HA |
| **Testo libero** (`text`) | Testo statico inserito nel campo **Testo** della pagina |
| **Temperatura camera** (`indoor`) | `Camera 21.5°` |
| **Temperatura esterna** (`outdoor`) | `Esterno 17.3°` |
| **Luci accese** (`lights`) | Conteggio luci accese |
| **Alba e tramonto** (`sun`) | Orari di oggi calcolati da HA |
| **Prossima sveglia** (`wake`) | `Sveglia domani 06:45`, oppure **Nessuna sveglia** |
| **Prossimo evento** (`event`) | Primo evento del calendario, oppure **Nessun evento** |
| **Fase lunare** (`moon`) | Calcolata dal proiettore, non serve HA |
| **Termostato** (`thermostat`) | `Termostato 21.5°, camera 19.3°`, oppure **Termostato spento** |

Le pagine preconfigurate sono **Ora**, **Casa** e **Meteo**. Le pagine con modello `home`, `weather`, `agenda` e `climate` hanno un design fisso e ignorano i widget. Il nome della pagina è lungo al massimo 16 byte e il testo statico 32 byte: con caratteri accentati il limite in caratteri può essere inferiore.

L'ordine delle pagine definisce **precedente/successiva**. Selezionando una pagina secondaria da web UI o HA, la pagina Ora torna automaticamente dopo il tempo impostato in **Proiezione → Ritorno alla pagina Ora** (web UI) o con l'entità **Ritorno alla pagina Ora** (HA): da 5 a 3600 secondi, predefinito 30; con `0` la pagina scelta resta visibile. Il tempo resta salvato al riavvio, la pagina scelta no.

## 6. Aggiornamenti

Per aggiornare il firmware via rete, compila la nuova versione con `idf.py build`, apri la web UI, scegli `build/bed-projector.bin` nella sezione **Aggiornamento firmware** e premi **Carica firmware**: una barra mostra l'avanzamento e, al riavvio, la pagina si ricarica da sola. Il file deve essere un'immagine ESP32-S3 valida di questo progetto e rientrare nello slot OTA. Al termine il dispositivo si riavvia. L'OTA è autenticato con il token; un'immagine rifiutata non diventa la partizione di avvio. Il bootloader ha il rollback abilitato per le immagini che non completano correttamente l'avvio.

**L'OTA aggiorna insieme firmware e web UI**, perché l'interfaccia è incorporata in `bed-projector.bin`. Se il nuovo firmware non completa l'avvio, il rollback ripristina anche la web UI precedente. Serve il flash via USB solo quando cambia la tabella delle partizioni (`partitions.csv`), che l'OTA non riscrive.

Un flash USB ordinario conserva NVS. `erase-flash` la cancella, come spiegato nella sezione 2.

## 7. API e sicurezza

Le chiamate operative e la web UI usano `API_TOKEN` del `.env` nell'header `Authorization: Bearer`. Se nel `.env` non sono configurati né Wi-Fi né token, il dispositivo usa un token NVS già presente o ne genera uno casuale; `GET /api/pair` lo restituisce dalla rete protetta di configurazione. Le risposte di stato e configurazione non restituiscono password Wi-Fi o token. Vedi [endpoint ed esempi](api.md).

Il browser salva il token nel proprio archivio locale. **Rimuovi token da questo browser** lo elimina da quel browser; non cambia il token memorizzato nel proiettore e non disconnette Home Assistant. La comunicazione HTTP non cifra il traffico: usa il dispositivo sulla rete locale fidata e non pubblicare la porta 80 su Internet.

## 8. Problemi e recupero

| Sintomo | Controllo e azione |
| --- | --- |
| Home Assistant non rileva il proiettore | Il rilevamento usa mDNS: Home Assistant deve stare sulla stessa rete (in Docker con `network_mode: host`) e il router non deve bloccare il multicast. Aggiungi l'integrazione a mano con l'IP. |
| `bed-projector.local` non si apre | Trova l'IP nel router e prova `http://IP`. Verifica che il client sia sulla stessa rete e che non sia isolato dal Wi-Fi. |
| Il dispositivo mostra `--:--` | Controlla Wi-Fi, DNS e accesso NTP a `pool.ntp.org`; attendi la sincronizzazione. |
| Aperture, luci, allarme o meteo mostrano `?` | Configura le entità nelle opzioni HA, verifica che siano disponibili e che HA possa raggiungere l'IP del proiettore. Dopo 120 secondi senza snapshot i dati scadono. |
| La pagina torna da sola a Ora | È il comportamento previsto: regola o disattiva (`0`) il **Ritorno alla pagina Ora** dalla web UI o da HA. |
| L'access point non compare | Con Wi-Fi valido nel `.env` il dispositivo si collega direttamente: è il comportamento previsto. Per forzare l'AP, rendi temporaneamente indisponibile quella rete e riavvia; attendi circa 20 secondi. |
| `/api/pair` restituisce `403` | Accedi da un client collegato all'AP `bed-projector` usando `http://192.168.4.1`; dalla LAN l'abbinamento è disabilitato. |
| Dopo il cambio Wi-Fi il browser richiede di nuovo il token | È normale: l'indirizzo del sito è cambiato. Reinserisci `API_TOKEN` dal `.env`. |
| Una chiamata API restituisce `401` | Il token manca o non coincide con `API_TOKEN` compilato. Correggi il browser o l'integrazione HA; se hai modificato `.env`, ricompila e riflasha il dispositivo. |
| Ho perso il token e la proiezione è spenta | Se il token era nel `.env`, recuperalo lì. Se usavi il token casuale senza `.env`, riaccendi da HA se possibile, oppure usa la seriale per la password AP e **Abbina**; come ultima possibilità cancella NVS e riflasha. |
| La web UI non mostra le novità dopo OTA | Ricarica la pagina. Se è ancora quella vecchia, il nuovo firmware non ha completato l'avvio e il bootloader è tornato al precedente: controlla il monitor seriale. |
| `idf.py` segnala un percorso Python diverso da quello della build precedente | Attiva la stessa installazione ESP-IDF/Python usata per configurare il progetto. Se stai cambiando ambiente di sviluppo, esegui `idf.py fullclean` e poi `idf.py build`; questo rigenera `build/` senza cancellare NVS sul dispositivo. |

Per una verifica completa dopo l'installazione usa la [checklist hardware](hardware-check.md).
