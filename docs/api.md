# API locale

Questa è l'API usata dalla [web UI e dal componente Home Assistant](guida.md). Con `WIFI_SSID` nel `.env`, il dispositivo si collega direttamente alla rete domestica e risponde su `http://bed-projector.local` oppure sul proprio IP, porta 80. Il router lo elenca con il nome host `bed-projector`. Via mDNS pubblica anche il servizio `_bed-projector._tcp`, con il campo TXT `id` uguale a quello di `/api/status`: Home Assistant lo usa per trovare il proiettore da solo. Se si avvia l'access point di recupero, usa `http://192.168.4.1` mentre il client è collegato a `bed-projector`.

## Autenticazione

Tutte le chiamate operative richiedono l'header `Authorization: Bearer <token>`, dove il token è **`API_TOKEN` del file `.env` usato per la build**. Se lo cambi, ricompili e installi il nuovo firmware, aggiorna il valore anche nei client API e in Home Assistant. L'unica eccezione è `GET /api/pair`: restituisce il token **solo** a un client collegato all'access point di configurazione e che usa l'indirizzo `192.168.4.1`. Dalla LAN risponde `403`.

Nel percorso normale leggi il token nel tuo `.env`; non serve **Abbina**. Se usi la configurazione AP senza token nel `.env`, ottieni quello casuale con **Abbina** nella web UI o con il primo comando seguente. Per controllare l'API da terminale, sostituisci i valori di esempio:

```sh
# Solo per il percorso AP: esegui mentre sei collegato all'AP del proiettore.
curl http://192.168.4.1/api/pair

# Poi, dalla rete domestica:
PROJECTOR_URL='http://bed-projector.local'
PROJECTOR_TOKEN='incolla-qui-il-token'
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/status"
```

Non pubblicare il token in script condivisi o log. Il firmware compilato contiene anche Wi-Fi e token: proteggi `.env` e gli artefatti di build. Le risposte operative non contengono password Wi-Fi né token. L'API usa HTTP locale, senza cifratura TLS.

## Endpoint

| Metodo | Percorso | Corpo | Risposta |
| --- | --- | --- | --- |
| GET | `/api/pair` | Nessuno | `{"token":"..."}`; solo da AP |
| GET | `/api/status` | Nessuno | Stato del dispositivo e dati HA |
| GET | `/api/pages` | Nessuno | Configurazione di tutte le pagine |
| PUT | `/api/pages` | JSON pagine | Configurazione salvata |
| POST | `/api/display` | JSON comando | Stato aggiornato |
| GET | `/api/calibration` | Nessuno | Centro, diametro, orientamento e modalità di regolazione |
| POST | `/api/calibration` | JSON regolazione | Stato della regolazione aggiornato |
| POST | `/api/ha/state` | JSON snapshot completo | `{}` |
| POST | `/api/message` | JSON messaggio | `{}` |
| GET | `/api/config` | Nessuno | `{"timezone":"..."}` |
| POST | `/api/network` | JSON rete | `{"restarting":true}` |
| POST | `/api/ota` | Binario firmware | `{"restarting":true}` |

I corpi JSON devono essere oggetti lunghi 2–4096 byte. Un errore restituisce JSON `{"error":"..."}`. Le risposte più comuni sono `400` per dati non validi, `401` per token assente o errato, `403` per abbinamento fuori AP e `413` per corpo troppo grande.

## Stato del dispositivo

```sh
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/status"
```

Esempio di risposta:

```json
{
  "id": "a1b2c3d4e5f6",
  "power": true,
  "brightness": 30,
  "page": "clock",
  "page_index": 0,
  "page_timeout": 30,
  "wake_display": "icon",
  "ha_fresh": false,
  "openings": null,
  "lights": null,
  "alarm": "unknown",
  "weather": "",
  "temperature": "",
  "wifi_connected": true,
  "free_heap": 184000,
  "uptime_seconds": 3600
}
```

`id` è l'indirizzo MAC Wi-Fi in 12 cifre esadecimali minuscole: non cambia mai e identifica il proiettore anche quando cambia IP. `openings:null` e `lights:null` significano conteggio sconosciuto; `ha_fresh:false` significa che l'ultimo snapshot ha superato i 120 secondi o non è mai arrivato. `free_heap` e `uptime_seconds` sono valori diagnostici variabili. I quattro dati aggiuntivi sono usati sul display, ma non compaiono nella risposta di stato.

## Comandi del display

`POST /api/display` accetta `power`, `brightness`, `page_timeout`, `wake_display` e al massimo uno tra `page` e `move`. `page` e `move` non possono essere inviati insieme.

```sh
# Spegni solo il LED di proiezione.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"power":false}' "$PROJECTOR_URL/api/display"

# Riaccendi a luminosità 30%.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"power":true,"brightness":30}' "$PROJECTOR_URL/api/display"

# Seleziona Casa, oppure passa alla pagina successiva.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"page":"home"}' "$PROJECTOR_URL/api/display"
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"move":"next"}' "$PROJECTOR_URL/api/display"
```

`brightness` è un intero da 1 a 100 e resta salvato anche quando `power` è `false`. `move` ammette `next` e `previous` e scorre in circolo. Una pagina secondaria torna automaticamente a `clock` dopo `page_timeout` secondi: `0` la lascia visibile, altrimenti il valore va da 5 a 3600 (predefinito 30). Il valore resta salvato al riavvio ed è riportato da `GET /api/status`.

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"page_timeout":60}' "$PROJECTOR_URL/api/display"
```

`wake_display` sceglie cosa mostra la pagina Ora della sveglia di domattina: `icon` (solo l'icona, predefinito), `time` (icona e ora della sveglia), `countdown` (icona e tempo mancante, per esempio `7h 55m`) o `none` (niente). Resta salvato al riavvio ed è riportato da `GET /api/status`.

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"wake_display":"countdown"}' "$PROJECTOR_URL/api/display"
```

## Centraggio e orientamento locali

`GET /api/calibration` restituisce `{"center_x":64,"center_y":64,"diameter":112,"rotation":0,"mirror":false,"active":false}` per la geometria iniziale. La web UI usa questo endpoint; l'integrazione Home Assistant non lo usa.

`POST /api/calibration` accetta una delle azioni `{"mode":"start"}`, `{"mode":"save"}` o `{"mode":"cancel"}`. Dopo `start`, un corpo `{"center_x":64,"center_y":64,"diameter":110,"rotation":90,"mirror":true}` aggiorna l'anteprima, con croce, bordo e scritta `SU` sul display. `rotation` (0, 90, 180 o 270 gradi) e `mirror` (booleano, specchiatura orizzontale applicata prima della rotazione) sono facoltativi: se mancano restano invariati. Centro e diametro si riferiscono sempre all'orientamento iniziale, quindi rotazione e specchiatura non spostano il cerchio sul display. `save` salva in NVS e ripristina le normali pagine; `cancel` ripristina i valori salvati senza scrivere. Le modifiche non salvate si perdono al riavvio. Il diametro deve essere pari e compreso tra 80 e 128 pixel; il cerchio deve restare interamente sul display 128×128.

## Pagine

`GET /api/pages` restituisce un oggetto con l'array `pages`. `PUT /api/pages` sostituisce e salva l'intero array. Esempio corrispondente alla configurazione iniziale:

```json
{
  "pages": [
    {"id":"clock","name":"Ora","layout":"clock","widgets":[],"text":""},
    {"id":"home","name":"Casa","layout":"home","widgets":[],"text":""},
    {"id":"weather","name":"Meteo","layout":"weather","widgets":[],"text":""}
  ]
}
```

```sh
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/pages"
curl -X PUT -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  --data-binary @pages.json "$PROJECTOR_URL/api/pages"
```

Regole di validazione:

- Da 1 a 5 pagine con ID univoci alfanumerici o `_`, lunghi fino a 16 byte. L'ordine dell'array è l'ordine della navigazione.
- La prima pagina deve avere `id:"clock"` e `layout:"clock"`; nessun'altra pagina può usarli.
- Le altre pagine usano `layout:"home"`, `layout:"weather"`, `layout:"agenda"`, `layout:"climate"` (termostato) o `layout:"list"`. Ogni pagina accetta al massimo tre widget tra `openings`, `alarm`, `date`, `weather`, `entity1`–`entity4`, `text`, `indoor`, `outdoor`, `lights`, `sun`, `wake`, `event`, `moon`, `thermostat`, ma solo `list` li mostra: `clock`, `home`, `weather`, `agenda` e `climate` hanno un design fisso e li ignorano.
- `name` è lungo al massimo 16 byte, `text` al massimo 32 byte. Il widget `text` mostra il testo della propria pagina.

Un JSON non valido viene rifiutato senza cambiare le pagine. Se il salvataggio NVS fallisce, il firmware ripristina in RAM la configurazione precedente e risponde con un errore.

## Snapshot da Home Assistant

Il componente incluso invia uno snapshot completo all'avvio, ai cambiamenti delle entità selezionate e ogni 60 secondi. Per una prova manuale:

```json
{
  "openings": 2,
  "openings_known": true,
  "alarm": "armed_night",
  "weather": "partlycloudy",
  "temperature": "17.3°C",
  "extras": ["Camera: 21°C", "", "", ""],
  "lights": 1,
  "lights_known": true,
  "opening_names": ["Finestra Sala", "Finestra Cucina"],
  "light_names": ["Luce Soggiorno"],
  "indoor": "21.5°",
  "outdoor": "17.3°",
  "temp_now": 17,
  "feels": "18°",
  "humidity": 71,
  "wind": "2 km/h",
  "days": [
    {"date": "2026-10-10", "condition": "partlycloudy", "high": 22, "low": 14, "precipitation": 50,
     "sunrise": "07:33", "sunset": "18:47", "summary": "Nubi sparse fino a sera.",
     "day": {"condition": "partlycloudy", "temperature": 22, "precipitation": 20},
     "evening": {"condition": "clear-night", "temperature": 16, "precipitation": 0}},
    {"date": "2026-10-11", "condition": "rainy", "high": 19, "low": 13, "precipitation": 70,
     "sunrise": "07:35", "sunset": "18:45", "summary": "", "day": null, "evening": null}
  ],
  "wake_known": true,
  "wake": {"date": "2026-10-11", "time": "06:45"},
  "events_known": true,
  "events": [
    {"date": "2026-10-10", "time": "18:30", "title": "Cena da Marco"},
    {"date": "2026-10-11", "time": "", "title": "Compleanno Giulia"}
  ],
  "thermostat": {"source": "heat", "mode": "heat", "action": "heating", "current": 193, "target": 215,
                 "min": 100, "max": 300, "humidity": 48, "preset": "Eco"}
}
```

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  --data-binary @snapshot.json "$PROJECTOR_URL/api/ha/state"
```

Campi obbligatori:

- `openings_known` e `lights_known` (booleani). Quando sono `true`, `openings` e `lights` sono richiesti e sono interi da 0 a 99. Se il conteggio non è disponibile, usa `false`: il display mostra `?` e il numero viene ignorato. Le luci contate escludono la proiezione.
- `alarm` ammette `disarmed`, `armed_home`, `armed_away`, `armed_night`, `armed_vacation`, `armed_custom_bypass`, `arming`, `pending`, `triggered`, `unknown`.
- `weather` (condizione attuale di HA, massimo 31 byte UTF-8), `temperature` (15 byte) ed `extras` (fino a quattro valori da 31 byte).
- `wake_known` ed `events_known` (booleani): `false` quando l'entità della sveglia (sensore timestamp o `input_datetime`) o i calendari non sono configurati o non sono disponibili. Il display mostra allora `?`, mai "nessuna sveglia" o "nessun evento".

Campi facoltativi: se mancano o sono `null` il dato è sconosciuto; se presenti devono essere validi, altrimenti l'intero snapshot è rifiutato.

| Campo | Formato |
| --- | --- |
| `opening_names`, `light_names` | Fino a 8 nomi non vuoti da 31 byte ciascuno |
| `indoor`, `outdoor`, `feels` | Temperature già formattate, massimo 11 byte |
| `temp_now` | Temperatura attuale intera, da -99 a 199 |
| `humidity` | Intero da 0 a 100 |
| `wind` | Vento già formattato, massimo 15 byte |
| `days` | Fino a 2 giorni, oggi e domani |
| `wake` | Prossima sveglia `{"date":"AAAA-MM-GG","time":"HH:MM"}` locale di HA; `null` se non ce n'è una nei prossimi 7 giorni |
| `events` | Fino a 3 eventi `{"date","time","title"}` da adesso alla fine di domani: `time` vuoto per gli eventi di tutto il giorno o iniziati prima di oggi, `title` non vuoto e al massimo 31 byte |
| `thermostat` | Termostato della camera mostrato dalla pagina `climate`; `null` se non configurato o non disponibile (vedi sotto) |

L'oggetto `thermostat` descrive un'entità `climate` di HA:

| Campo | Formato |
| --- | --- |
| `source` | Obbligatorio: `heat` (caloriferi) o `cool` (condizionatore) |
| `mode` | Obbligatorio: `off`, `heat`, `cool`, `heat_cool`, `auto`, `dry`, `fan_only` |
| `min`, `max` | Obbligatori: scala della rotella in decimi di grado, da -500 a 1000, con `min` < `max` |
| `action` | `off`, `heating`, `cooling`, `drying`, `idle`, `fan`, `preheating`, `defrosting`; `null` se l'entità non la indica |
| `current`, `target` | Temperatura della stanza e temperatura impostata in decimi di grado (215 = 21,5 °C), da -500 a 1000 |
| `humidity` | Umidità della stanza, intero da 0 a 100 |
| `preset` | Preset attivo già tradotto, massimo 15 byte; vuoto se non c'è |

Ogni elemento di `days` contiene `date` (data locale di HA, `AAAA-MM-GG`), `condition`, `high`, `low`, `precipitation` (0–100), `sunrise` e `sunset` (`HH:MM` locali), `summary` (massimo 63 byte), `day` ed `evening`. Questi ultimi sono oggetti `{"condition", "temperature", "precipitation"}` oppure `null`. Il dispositivo sceglie il giorno confrontando `date` con il proprio orologio: fino alle 20:59 usa oggi, dalle 21:00 domani. Il dispositivo misura 120 secondi dall'ultimo snapshot ricevuto, senza affidarsi all'orologio di HA.

## Messaggi

`POST /api/message` mostra un messaggio temporaneo al posto della pagina corrente, finché non scade o non viene tolto. Non viene salvato: un riavvio lo cancella. La calibrazione e la schermata dell'access point hanno la precedenza.

```json
{"text": "Lavatrice finita", "icon": "washing-machine", "duration": 60}
```

- `text`: obbligatorio, da 1 a 63 byte UTF-8, senza caratteri di controllo.
- `icon`: facoltativa, una tra `bell`, `information`, `alert`, `washing-machine`, `package-variant`, `door-open`, `lightbulb-on`, `phone`, `alarm`, `calendar`. Se manca compare `bell`.
- `duration`: facoltativa, da 5 a 3600 secondi, predefinita 30.

Un nuovo messaggio sostituisce quello mostrato. `{"clear": true}`, senza altri campi, lo toglie subito.

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"text":"Lavatrice finita","icon":"washing-machine","duration":60}' "$PROJECTOR_URL/api/message"
```

## Rete, configurazione e OTA

`GET /api/config` restituisce il solo fuso orario. Per cambiare rete o fuso orario, invia **tutti e tre** i campi; SSID e password non possono essere letti dall'API:

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"ssid":"ReteCasa","password":"password-wifi","timezone":"CET-1CEST,M3.5.0,M10.5.0/3"}' \
  "$PROJECTOR_URL/api/network"
```

Il dispositivo salva i valori e si riavvia. La password deve essere vuota per una rete aperta o avere 8–63 caratteri. Una rete cambiata via API/web UI resta attiva ai riavvii successivi; se cambi le credenziali Wi-Fi nel `.env`, ricompili e installi il nuovo firmware, quelle nuove credenziali sostituiscono la modifica fatta via API.

Per l'OTA manuale carica il firmware, che contiene anche la web UI:

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @build/bed-projector.bin "$PROJECTOR_URL/api/ota"
```

Il firmware controlla il nome del progetto già sul primo blocco ricevuto (un file sbagliato è rifiutato con `Not a Bed Projector firmware` prima di scrivere nulla), cancella solo lo spazio necessario all'immagine e la verifica prima di selezionarla per l'avvio. La web UI è incorporata nel firmware e si aggiorna con esso. Il token e le impostazioni NVS sopravvivono a un OTA valido.
