# API locale v1

Questa è l'API usata dalla [web UI e dal componente Home Assistant](guida.md). Con `WIFI_SSID` nel `.env`, il dispositivo si collega direttamente alla rete domestica e risponde su `http://bed-projector.local` oppure sul proprio IP, porta 80. Se si avvia l'access point di recupero, usa `http://192.168.4.1` mentre il client è collegato a `bed-projector`.

## Autenticazione

Tutte le chiamate operative richiedono l'header `Authorization: Bearer <token>`, dove il token è **`API_TOKEN` del file `.env` usato per la build**. Se lo cambi, ricompili e installi il nuovo firmware, aggiorna il valore anche nei client API e in Home Assistant. L'unica eccezione è `GET /api/v1/pair`: restituisce il token **solo** a un client collegato all'access point di configurazione e che usa l'indirizzo `192.168.4.1`. Dalla LAN risponde `403`.

Nel percorso normale leggi il token nel tuo `.env`; non serve **Abbina**. Se usi la configurazione AP senza token nel `.env`, ottieni quello casuale con **Abbina** nella web UI o con il primo comando seguente. Per controllare l'API da terminale, sostituisci i valori di esempio:

```sh
# Solo per il percorso AP: esegui mentre sei collegato all'AP del proiettore.
curl http://192.168.4.1/api/v1/pair

# Poi, dalla rete domestica:
PROJECTOR_URL='http://bed-projector.local'
PROJECTOR_TOKEN='incolla-qui-il-token'
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/v1/status"
```

Non pubblicare il token in script condivisi o log. Il firmware compilato contiene anche Wi-Fi e token: proteggi `.env` e gli artefatti di build. Le risposte operative non contengono password Wi-Fi né token. L'API usa HTTP locale, senza cifratura TLS.

## Endpoint

| Metodo | Percorso | Corpo | Risposta |
| --- | --- | --- | --- |
| GET | `/api/v1/pair` | Nessuno | `{"token":"..."}`; solo da AP |
| GET | `/api/v1/status` | Nessuno | Stato del dispositivo e dati HA |
| GET | `/api/v1/pages` | Nessuno | Configurazione di tutte le pagine |
| PUT | `/api/v1/pages` | JSON pagine | Configurazione salvata |
| POST | `/api/v1/display` | JSON comando | Stato aggiornato |
| GET | `/api/v1/calibration` | Nessuno | Centro, diametro e modalità di regolazione |
| POST | `/api/v1/calibration` | JSON regolazione | Stato della regolazione aggiornato |
| POST | `/api/v1/ha/state` | JSON snapshot completo | `{}` |
| GET | `/api/v1/config` | Nessuno | `{"timezone":"..."}` |
| POST | `/api/v1/network` | JSON rete | `{"restarting":true}` |
| POST | `/api/v1/ota` | Binario firmware | `{"restarting":true}` |

I corpi JSON devono essere oggetti lunghi 2–4096 byte. Un errore restituisce JSON `{"error":"..."}`. Le risposte più comuni sono `400` per dati non validi, `401` per token assente o errato, `403` per abbinamento fuori AP e `413` per corpo troppo grande.

## Stato del dispositivo

```sh
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/v1/status"
```

Esempio di risposta:

```json
{
  "api_version": 1,
  "power": true,
  "brightness": 30,
  "page": "clock",
  "page_index": 0,
  "ha_fresh": false,
  "openings": null,
  "alarm": "unknown",
  "weather": "",
  "temperature": "",
  "wifi_connected": true,
  "free_heap": 184000,
  "uptime_seconds": 3600
}
```

`openings:null` significa conteggio sconosciuto; `ha_fresh:false` significa che l'ultimo snapshot ha superato i 120 secondi o non è mai arrivato. `free_heap` e `uptime_seconds` sono valori diagnostici variabili. I quattro dati aggiuntivi sono usati sul display, ma non compaiono nella risposta di stato.

## Comandi del display

`POST /api/v1/display` accetta `power`, `brightness` e al massimo uno tra `page` e `move`. `page` e `move` non possono essere inviati insieme.

```sh
# Spegni solo il LED di proiezione.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"power":false}' "$PROJECTOR_URL/api/v1/display"

# Riaccendi a luminosità 30%.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"power":true,"brightness":30}' "$PROJECTOR_URL/api/v1/display"

# Seleziona Casa, oppure passa alla pagina successiva.
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"page":"home"}' "$PROJECTOR_URL/api/v1/display"
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"move":"next"}' "$PROJECTOR_URL/api/v1/display"
```

`brightness` è un intero da 1 a 100 e resta salvato anche quando `power` è `false`. `move` ammette `next` e `previous` e scorre in circolo. Una pagina secondaria torna automaticamente a `clock` dopo 30 secondi.

## Centraggio circolare locale

`GET /api/v1/calibration` restituisce `{"center_x":64,"center_y":64,"diameter":112,"active":false}` per la geometria iniziale. La web UI usa questo endpoint; l'integrazione Home Assistant non lo usa.

`POST /api/v1/calibration` accetta una delle azioni `{"mode":"start"}`, `{"mode":"save"}` o `{"mode":"cancel"}`. Dopo `start`, un corpo `{"center_x":64,"center_y":64,"diameter":110}` aggiorna l'anteprima, con croce e bordo sul display. `save` salva in NVS e ripristina le normali pagine; `cancel` ripristina i valori salvati senza scrivere. Le modifiche non salvate si perdono al riavvio. Il diametro deve essere pari e compreso tra 80 e 128 pixel; il cerchio deve restare interamente sul display 128×128.

## Pagine

`GET /api/v1/pages` restituisce un oggetto con l'array `pages`. `PUT /api/v1/pages` sostituisce e salva l'intero array. Esempio corrispondente alla configurazione iniziale:

```json
{
  "pages": [
    {"id":"clock","name":"Ora","layout":"clock","widgets":["openings","alarm","date"],"text":""},
    {"id":"home","name":"Casa","layout":"home","widgets":["openings","alarm","entity1"],"text":""},
    {"id":"weather","name":"Meteo","layout":"weather","widgets":["weather","date","entity2"],"text":""}
  ]
}
```

```sh
curl -H "Authorization: Bearer $PROJECTOR_TOKEN" "$PROJECTOR_URL/api/v1/pages"
curl -X PUT -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  --data-binary @pages.json "$PROJECTOR_URL/api/v1/pages"
```

Regole di validazione:

- Da 1 a 5 pagine con ID univoci alfanumerici o `_`, lunghi fino a 16 byte. L'ordine dell'array è l'ordine della navigazione.
- La prima pagina deve avere `id:"clock"`, `layout:"clock"` e i widget `openings`, `alarm` nei primi due slot. Il terzo slot è configurabile.
- Le altre pagine usano `layout:"home"` o `layout:"weather"`; massimo tre widget tra `openings`, `alarm`, `date`, `weather`, `entity1`–`entity4`, `text`.
- `name` è lungo al massimo 16 byte, `text` al massimo 32 byte. Il widget `text` mostra il testo della propria pagina.

Un JSON non valido viene rifiutato senza cambiare le pagine. Se il salvataggio NVS fallisce, il firmware ripristina in RAM la configurazione precedente e risponde con un errore.

## Snapshot da Home Assistant

Il componente incluso invia uno snapshot completo all'avvio, ai cambiamenti delle entità selezionate e ogni 60 secondi. Per una prova manuale:

```json
{
  "openings": 1,
  "openings_known": true,
  "alarm": "armed_home",
  "weather": "sunny",
  "temperature": "19°C",
  "extras": ["Camera: 21°C", "", "", ""]
}
```

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  --data-binary @snapshot.json "$PROJECTOR_URL/api/v1/ha/state"
```

`openings` è un intero da 0 a 99. Se il sensore del conteggio non ha un valore numerico valido, usa `openings_known:false`; il display mostra `?` anche se il numero inviato è zero. `alarm` ammette `disarmed`, `armed_home`, `armed_away`, `armed_night`, `armed_vacation`, `armed_custom_bypass`, `arming`, `pending`, `triggered`, `unknown`. `weather` è lungo al massimo 31 byte UTF-8, `temperature` 15 byte, ogni valore di `extras` 31 byte; sono accettati fino a quattro valori. Il dispositivo misura 120 secondi dall'ultimo snapshot ricevuto, senza affidarsi all'orologio di HA.

## Rete, configurazione e OTA

`GET /api/v1/config` restituisce il solo fuso orario. Per cambiare rete o fuso orario, invia **tutti e tre** i campi; SSID e password non possono essere letti dall'API:

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" -H 'Content-Type: application/json' \
  -d '{"ssid":"ReteCasa","password":"password-wifi","timezone":"CET-1CEST,M3.5.0,M10.5.0/3"}' \
  "$PROJECTOR_URL/api/v1/network"
```

Il dispositivo salva i valori e si riavvia. La password deve essere vuota per una rete aperta o avere 8–63 caratteri. Una rete cambiata via API/web UI resta attiva ai riavvii successivi; se cambi le credenziali Wi-Fi nel `.env`, ricompili e installi il nuovo firmware, quelle nuove credenziali sostituiscono la modifica fatta via API.

Per l'OTA manuale carica **solo** il firmware applicativo:

```sh
curl -X POST -H "Authorization: Bearer $PROJECTOR_TOKEN" \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @build/bed-projector.bin "$PROJECTOR_URL/api/v1/ota"
```

Il firmware verifica l'immagine, il nome del progetto e la dimensione dello slot prima di selezionarla per l'avvio. L'OTA non aggiorna SPIFFS; per modifiche alla web UI usa `idf.py -p PORT flash` via USB. Il token e le impostazioni NVS sopravvivono a un OTA valido.
