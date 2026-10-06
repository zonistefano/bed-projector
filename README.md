# Bed Projector

Orologio proiettato per ESP32-S3, display ST7735S 128×128 e LED comandato in PWM. Mostra ora, stato della casa e meteo; Home Assistant fornisce i dati e controlla pagine, accensione e luminosità.

## Da dove iniziare

1. **Configura il file locale:** copia [`env.example`](env.example) in `.env` e inserisci `WIFI_SSID`, `WIFI_PASSWORD` e `API_TOKEN`. Il token è una password locale di 32 caratteri esadecimali; puoi crearla con `python3 -c 'import secrets; print(secrets.token_hex(16))'`. Il file `.env` è ignorato da Git.
2. **Compila e installa:** attiva ESP-IDF 5.4.4, poi esegui `idf.py build` e `idf.py -p PORT flash monitor` dalla radice del repository. Il flash include firmware e interfaccia web.
3. **Apri la web UI:** al riavvio il proiettore prova direttamente la rete definita in `.env`. Apri `http://bed-projector.local` o l'IP assegnato dal router e accedi con **lo stesso `API_TOKEN`**. Non serve abbinare o copiare un token generato dal dispositivo.
4. **Collega Home Assistant:** installa l'integrazione **Bed Projector** tramite [HACS](#installazione-con-hacs), aggiungila con lo stesso token e associa sensori, allarme e meteo.

La [guida passo passo](docs/guida.md) copre requisiti, compilazione, primo avvio, uso e recupero. Senza `WIFI_SSID`, il firmware conserva la procedura tramite access point protetto; se `WIFI_SSID` è presente, `API_TOKEN` è obbligatorio. Anche un Wi-Fi configurato ma non raggiungibile attiva l'AP di recupero dopo circa 20 secondi.

## Installazione con HACS

Con [HACS](https://www.hacs.dev/docs/use/) già installato in Home Assistant e questa versione pubblicata nel ramo predefinito del repository GitHub pubblico:

1. Apri **HACS → ⋮ → Repository personalizzati** e aggiungi `https://github.com/zonistefano/bed-projector` come tipo **Integrazione**.
2. Apri **Bed Projector** in HACS e scegli **Scarica**. Riavvia Home Assistant quando il download è terminato.
3. Vai a **Impostazioni → Dispositivi e servizi → Aggiungi integrazione**, cerca **Bed Projector** e inserisci indirizzo del dispositivo, porta `80` e il suo `API_TOKEN`.

HACS installa e aggiorna solo l'integrazione Home Assistant. Il firmware ESP32 e la web UI si installano e aggiornano separatamente come descritto nella [guida](docs/guida.md). Per l'installazione manuale dell'integrazione vedi la [sezione Home Assistant](docs/guida.md#4-home-assistant).

## Cosa puoi fare

- Usare tre pagine iniziali (**Ora**, **Casa**, **Meteo**) e configurarne fino a cinque, con tre widget per pagina. La pagina Ora resta sempre la prima.
- Vedere sulla pagina Ora la previsione di oggi (icona, massima e minima), le porte e finestre aperte (solo se ce ne sono) e la modalità dell'allarme con le stesse icone di Home Assistant (disinserito, in casa, fuori casa, notte, vacanza, personalizzato, inserimento, attesa, scattato).
- Scorrere o scegliere una pagina da Home Assistant o dalla web UI. Dopo 30 secondi su una pagina secondaria ricompare l'ora.
- Regolare la luminosità da 1 a 100%. **Off** spegne il LED di proiezione, mentre ESP32, Wi-Fi e display continuano a funzionare. Livello e stato acceso/spento sono persistenti.
- Centrare il cerchio proiettato, regolarne il diametro e ruotare (a passi di 90°) o specchiare il contenuto dalla web UI locale. Durante la regolazione appaiono una croce, il bordo e la scritta «SU»; geometria e orientamento salvati si applicano a tutte le pagine e non sono esposti a Home Assistant.
- Ricevere dati da Home Assistant all'avvio, ai cambiamenti e ogni 60 secondi. Dopo 120 secondi senza aggiornamenti, i dati diventano sconosciuti; l'ora continua a funzionare.

## Documentazione

| Guida | Contenuto |
| --- | --- |
| [Installazione e uso](docs/guida.md) | Hardware, compilazione, primo avvio, Home Assistant, pagine, aggiornamenti, recupero |
| [API locale v1](docs/api.md) | Autenticazione, endpoint, formati JSON ed esempi `curl` |
| [Verifiche sulla scheda](docs/hardware-check.md) | Controlli da eseguire sul dispositivo fisico |

## Sviluppo e verifica locale

```sh
python3 -m unittest discover -s tests -v
cc -std=c11 -Wall -Wextra -Werror -I main main/projector_logic.c tests/test_firmware_logic.c -o /tmp/bed-projector-logic
/tmp/bed-projector-logic
idf.py build
```

Il progetto usa partizioni da **4 MB**, con due slot OTA e SPIFFS. La build verifica che il firmware entri nello slot; i controlli su proiezione, rete e aggiornamento richiedono la scheda fisica.
