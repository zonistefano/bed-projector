const $ = id => document.getElementById(id);
const TOKEN_KEY = "bed-projector-token";
const STATUS_POLL_MS = 15000;
const MAX_PAGES = 5;
const MAX_WIDGETS = 4;
const TOMORROW_DEFAULT = 21;

const alarmLabels = {
  disarmed: "disinserito", armed_home: "in casa", armed_away: "fuori casa", armed_night: "notte",
  armed_vacation: "vacanza", armed_custom_bypass: "personalizzato", arming: "in inserimento",
  pending: "in attesa", triggered: "SCATTATO", unknown: "?",
};
const weatherLabels = {
  "clear-night": "sereno", cloudy: "nuvoloso", exceptional: "eccezionale", fog: "nebbia",
  hail: "grandine", lightning: "temporale", "lightning-rainy": "temporale",
  partlycloudy: "poco nuvoloso", pouring: "rovesci", rainy: "pioggia", snowy: "neve",
  "snowy-rainy": "nevischio", sunny: "sereno", windy: "ventoso", "windy-variant": "ventoso",
};
const widgetLabels = {
  openings: "Porte/finestre aperte", alarm: "Allarme", date: "Data", weather: "Meteo attuale",
  entity1: "Dato HA 1", entity2: "Dato HA 2", entity3: "Dato HA 3", entity4: "Dato HA 4",
  text: "Testo libero", indoor: "Temperatura camera", outdoor: "Temperatura esterna",
  lights: "Luci accese", sun: "Alba e tramonto", wake: "Prossima sveglia", event: "Prossimo evento",
  moon: "Fase lunare", thermostat: "Termostato", rain: "Probabilità di pioggia", humidity: "Umidità esterna",
  wind: "Vento", countdown: "Conto alla rovescia",
};
/* Widgets that keep the row's text: free text, or the countdown's "YYYY-MM-DD name". */
const textWidgets = { text: "Testo", countdown: "Data e nome (AAAA-MM-GG Nome)" };
const COUNTDOWN_PATTERN = /^\d{4}-\d{2}-\d{2}( .*)?$/;
/* Mirrors the page schema in main/projector_logic.c: [value, label] pairs in the firmware's order,
 * the first being the default. */
const off = ["off", "Nascosto"];
const onOff = [["on", "Visibile"], off];
const layoutElements = {
  clock: [
    ["forecast", "Previsione", [["icon_temps", "Icona e temperature"], ["temps", "Solo temperature"], ["icon", "Solo icona"], off]],
    ["openings", "Porte e finestre aperte", [["count", "Icona e numero"], ["icon", "Solo icona"], off]],
    ["lights", "Luci accese", [["count", "Icona e numero"], ["icon", "Solo icona"], off]],
    ["alarm", "Allarme inserito", [["icon", "Icona"], off]],
    ["wake", "Sveglia di domattina", [["icon", "Solo icona"], ["time", "Icona e ora"], ["countdown", "Icona e tempo mancante"], off]],
    ["date", "Data", [["full", "Completa (Sab 10 Ottobre)"], ["short", "Breve (Sab 10)"], off]],
    ["sun", "Alba e tramonto", onOff],
  ],
  home: [
    ["alarm", "Allarme", [["icon_label", "Icona e stato"], ["label", "Solo stato"], ["icon", "Solo icona"], off]],
    ["temperatures", "Temperature camera ed esterno", [["icons", "Con icone"], ["values", "Solo valori"], off]],
    ["openings", "Porte e finestre", [["names", "Nomi di quelle aperte"], ["count", "Solo quante"], off]],
    ["lights", "Luci", [["names", "Nomi di quelle accese"], ["count", "Solo quante"], off]],
  ],
  weather: [
    ["now", "Adesso", [["icon_temp", "Icona e temperatura"], ["temp", "Solo temperatura"], ["icon", "Solo icona"]]],
    ["details", "Dettagli", [["full", "Condizione, percepita, umidità, vento"], ["condition", "Solo condizione"], off]],
    ["periods", "Giorno e sera", onOff],
    ["rain", "Probabilità di pioggia", onOff],
    ["summary", "Riepilogo della giornata", onOff],
  ],
  agenda: [
    ["wake", "Sveglia", [["time_day", "Ora e giorno"], ["time", "Solo ora"], off]],
    ["events", "Eventi", [["3", "Fino a 3"], ["2", "Fino a 2"], ["1", "Solo il prossimo"], off]],
    ["event_time", "Quando inizia l'evento", onOff],
  ],
  climate: [
    ["dial", "Rotella", onOff],
    ["status", "Stato", [["icon_label", "Icona e stato"], ["label", "Solo stato"], ["icon", "Solo icona"], off]],
    ["preset", "Preset (es. Eco)", onOff],
    ["current", "Temperatura della stanza", onOff],
    ["humidity", "Umidità", onOff],
  ],
  list: [["header", "Intestazione con il nome", onOff]],
  face: [
    ["dial", "Ora", [["analog", "Lancette"], ["digital", "Digitale"]]],
    ["marks", "Bordo", [["ticks", "Tacche"], ["numbers", "Numeri 12, 3, 6, 9 (con le lancette)"], ["off", "Nessuno"]]],
  ],
};
/* Watch face complications, in the firmware's slot order. */
const faceSlots = [["top", "In alto"], ["right", "A destra"], ["bottom", "In basso"], ["left", "A sinistra"]];
const faceStyleLabels = { icon_value: "Icona e valore", value: "Solo valore", label_value: "Etichetta e valore" };
const firstLayoutLabels = { clock: "Ora (classica)", face: "Quadrante (lancette o digitale, con complicazioni)" };
const sizeLabels = { normal: "Normale", small: "Piccolo", large: "Grande" };
const styleLabels = { label_value: "Etichetta e valore", value: "Solo valore", icon_value: "Icona e valore" };
const layoutLabels = {
  home: "Casa (allarme, temperature, aperture, luci)", weather: "Meteo (adesso, giorno e sera)",
  agenda: "Agenda (sveglia ed eventi)", climate: "Termostato (rotella con temperatura impostata e attuale)",
  list: "Elenco widget", face: "Quadrante (lancette o digitale, con complicazioni)",
};

function storedToken() {
  try { return localStorage.getItem(TOKEN_KEY) || ""; } catch (_) { return ""; }
}
function storeToken(value) {
  try { value ? localStorage.setItem(TOKEN_KEY, value) : localStorage.removeItem(TOKEN_KEY); } catch (_) { /* Private mode. */ }
}

let token = storedToken();
let savedPages = [];  // As stored on the device: feeds the page selector.
let pages = [];       // Editor draft: survives other actions until "Salva pagine".
let theme = {};       // Draft theme, saved with the pages.
let calibration = null;
let calibrationPending = false;
let pollTimer = null;
const wakePhaseLabels = {
  idle: "Nessuna sveglia in corso", sunrise: "Alba in corso: la luce sta salendo", ringing: "Sveglia in corso",
};

/* ---- Helpers ---- */

function message(value) { $("message").textContent = value; }

async function request(path, method = "GET", body = undefined) {
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const response = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  let data = {};
  try { data = await response.json(); } catch (_) { /* Empty or non-JSON error body. */ }
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

/* Binary upload with progress; fetch() cannot report upload progress. */
function upload(path, file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", path);
    xhr.setRequestHeader("Authorization", `Bearer ${token}`);
    xhr.upload.onprogress = event => { if (event.lengthComputable) onProgress(event.loaded / event.total); };
    xhr.onload = () => {
      let data = {};
      try { data = JSON.parse(xhr.responseText); } catch (_) { /* Reboot may cut the response. */ }
      xhr.status < 400 ? resolve(data) : reject(new Error(data.error || `HTTP ${xhr.status}`));
    };
    xhr.onerror = () => reject(new Error("Connessione interrotta durante il caricamento"));
    xhr.send(file);
  });
}

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

/* After a reboot, reload the page as soon as the device answers again. */
async function reloadWhenBack(seconds = 90) {
  await sleep(4000);
  for (const end = Date.now() + seconds * 1000; Date.now() < end; await sleep(3000)) {
    try { await request("/api/status"); location.reload(); return; } catch (_) { /* Still rebooting. */ }
  }
  message("Il dispositivo non risponde ancora: ricarica la pagina tra poco");
}

function element(tag, properties = {}, children = []) {
  const item = Object.assign(document.createElement(tag), properties);
  item.append(...children);
  return item;
}

const option = (value, label) => element("option", { value, textContent: label });
const utf8Length = value => new TextEncoder().encode(value).length;

function showApp(visible) {
  $("login").hidden = visible;
  $("app").hidden = !visible;
  clearInterval(pollTimer);
  pollTimer = visible ? setInterval(pollStatus, STATUS_POLL_MS) : null;
}

async function run(action) {
  try { message(""); await action(); }
  catch (error) {
    message(error.message === "unauthorized" ? "Token non valido: inserisci API_TOKEN del file .env" : error.message);
    if (error.message === "unauthorized") showApp(false);
  }
}

/* ---- Status ---- */

function applyStatus(status) {
  $("power").checked = status.power;
  if (document.activeElement !== $("brightness")) {
    $("brightness").value = status.brightness;
    $("brightness-value").textContent = `${status.brightness}%`;
  }
  if (document.activeElement !== $("page-timeout")) $("page-timeout").value = status.page_timeout;
  if (document.activeElement !== $("carousel")) $("carousel").value = status.carousel;
  $("preview-state").hidden = $("cancel-preview").hidden = !status.preview;
  const current = savedPages.find(page => page.id === status.page);
  $("state-summary").textContent = `Pagina: ${current ? current.name : status.page} · ` +
    `Wi-Fi: ${status.wifi_connected ? "connesso" : "non connesso"}`;
  $("current-page").value = status.page;
  $("wake-summary").textContent = wakePhaseLabels[status.wake_phase] ?? status.wake_phase;
  $("dismiss-wake").disabled = status.wake_phase === "idle";
  $("ha-summary").textContent = status.ha_fresh ?
    `Aperture: ${status.openings ?? "?"} · Luci: ${status.lights ?? "?"} · ` +
    `Allarme: ${alarmLabels[status.alarm] ?? status.alarm} · ` +
    `Meteo: ${weatherLabels[status.weather] ?? (status.weather || "?")} ${status.temperature}` :
    "Dati Home Assistant sconosciuti o non aggiornati";
}

function pollStatus() {
  if (document.hidden || $("app").hidden) return;
  request("/api/status").then(applyStatus).catch(() => { /* Next poll retries. */ });
}

async function display(command) {
  applyStatus(await request("/api/display", "POST", command));
}

/* ---- Pages ---- */

function renderPageSelect() {
  $("current-page").replaceChildren(...savedPages.map(page => option(page.id, page.name)));
}

function pageField(labelText, value, maxLength, onChange) {
  const input = element("input", { value, maxLength });
  input.addEventListener("input", () => onChange(input.value));
  return element("label", { textContent: labelText }, [input]);
}

function pageSelect(labelText, options, value, onChange) {
  const select = element("select", {}, options.map(([key, label]) => option(key, label)));
  select.value = value;
  select.addEventListener("change", () => onChange(select.value));
  return element("label", { textContent: labelText }, [select]);
}

function pageButton(label, disabled, onClick) {
  const button = element("button", { type: "button", textContent: label, disabled });
  button.addEventListener("click", () => { onClick(); renderPages(); });
  return button;
}

const firstMode = modes => modes[0][0];
const showsTomorrow = page => page.layout === "clock" || page.layout === "weather";

function widgetRow(page, slot) {
  const widget = page.widgets[slot] || { widget: "", style: "label_value", text: "" };
  const row = element("div", { className: "widget-row" });
  const update = () => {
    page.widgets[slot] = widget;
    row.replaceChildren(
      pageSelect(`Riga ${slot + 1}`, [["", "Nessuna"], ...Object.entries(widgetLabels)], widget.widget,
                 value => { widget.widget = value; update(); }),
      pageSelect("Come", Object.entries(styleLabels), widget.style || "label_value", value => { widget.style = value; }),
    );
    if (textWidgets[widget.widget])
      row.append(pageField(textWidgets[widget.widget], widget.text || "", 32, value => { widget.text = value; }));
  };
  update();
  return row;
}

function faceRow(page, slot, label) {
  let widget = page.widgets.find(item => item && item.slot === slot);
  if (!widget) {
    widget = { widget: "", style: "icon_value", text: "", slot };
    page.widgets.push(widget);
  }
  const row = element("div", { className: "widget-row" });
  const update = () => {
    row.replaceChildren(
      pageSelect(label, [["", "Nessuna"], ...Object.entries(widgetLabels)], widget.widget,
                 value => { widget.widget = value; update(); }),
      pageSelect("Come", Object.entries(faceStyleLabels), widget.style || "icon_value", value => { widget.style = value; }),
    );
    if (textWidgets[widget.widget])
      row.append(pageField(textWidgets[widget.widget], widget.text || "", 32, value => { widget.text = value; }));
  };
  update();
  return row;
}

/* Keep the chosen widgets across a change of model: a watch face gives them slots in order. */
function changeLayout(page, layout) {
  const chosen = page.widgets.filter(item => item && item.widget);
  page.widgets = layout === "face" ?
    chosen.slice(0, faceSlots.length).map((item, index) => ({ ...item, slot: faceSlots[index][0] })) :
    chosen.map(({ slot, ...item }) => item);
  page.layout = layout;
  page.show = {};
}

function renderPages() {
  $("pages").replaceChildren(...pages.map((page, index) => {
    page.show ??= {};
    page.widgets ??= [];
    const title = element("h3", { textContent: `${index + 1}. ${page.name}` });
    const grid = element("div", { className: "page-grid" }, [
      pageField("Nome", page.name, 16, value => { page.name = value; title.textContent = `${index + 1}. ${value}`; }),
    ]);
    grid.append(pageSelect("Modello", Object.entries(index > 0 ? layoutLabels : firstLayoutLabels), page.layout,
                           value => { changeLayout(page, value); renderPages(); }));
    grid.append(pageSelect("Dimensione testo", Object.entries(sizeLabels), page.size || "normal",
                           value => { page.size = value; }));
    if (showsTomorrow(page)) {
      const hour = element("input", { type: "number", min: 0, max: 24, step: 1, value: page.tomorrow_from ?? TOMORROW_DEFAULT });
      hour.addEventListener("input", () => { page.tomorrow_from = Number(hour.value); });
      grid.append(element("label", { textContent: "Previsione di domani dalle ore (24 = mai)" }, [hour]));
    }
    const elements = element("div", { className: "page-grid" }, layoutElements[page.layout].map(([key, label, modes]) =>
      pageSelect(label, modes, page.show[key] ?? firstMode(modes), value => { page.show[key] = value; })));
    const card = element("div", { className: "page-editor" }, [title, grid, element("h4", { textContent: "Elementi" }), elements]);
    if (page.layout === "list")
      for (let slot = 0; slot < MAX_WIDGETS; slot++) card.append(widgetRow(page, slot));
    if (page.layout === "face") {
      card.append(element("h4", { textContent: "Complicazioni" }));
      for (const [slot, label] of faceSlots) card.append(faceRow(page, slot, label));
    }
    const actions = element("div", { className: "actions" });
    const trial = element("button", { type: "button", textContent: "Prova sul proiettore" });
    trial.addEventListener("click", () => run(async () => {
      applyStatus(await request("/api/pages/preview", "POST", { ...pagesDocument(), page: page.id }));
      message(`Prova di “${page.name}” sul proiettore: salva per tenerla, altrimenti torna com'era tra 2 minuti`);
    }));
    actions.append(trial);
    if (index > 0) {
      const swap = target => { [pages[index], pages[target]] = [pages[target], pages[index]]; };
      actions.append(
        pageButton("Su", index === 1, () => swap(index - 1)),
        pageButton("Giù", index === pages.length - 1, () => swap(index + 1)),
        pageButton("Rimuovi", false, () => pages.splice(index, 1)),
      );
    }
    card.append(actions);
    return card;
  }));
  $("add-page").disabled = pages.length >= MAX_PAGES;
}

function renderTheme() {
  $("theme-palette").value = theme.palette;
  $("theme-tint").value = theme.tint;
  $("theme-dots").checked = theme.dots;
  $("theme-tint-label").hidden = theme.palette !== "mono";
}

/* The draft as the /api/pages document; throws on a value the projector would refuse. */
function pagesDocument() {
  const list = pages.map((page, index) => {
    const name = page.name.trim();
    if (!name || utf8Length(name) > 16 || /[\u0000-\u001f\u007f]/.test(name))
      throw new Error(`Pagina ${index + 1}: il nome deve avere da 1 a 16 caratteri`);
    const show = Object.fromEntries(layoutElements[page.layout].map(([key, , modes]) =>
      [key, page.show[key] ?? firstMode(modes)]));
    const out = { id: page.id, name, layout: page.layout, size: page.size || "normal", show };
    if (showsTomorrow(page)) {
      const hour = page.tomorrow_from ?? TOMORROW_DEFAULT;
      if (!Number.isInteger(hour) || hour < 0 || hour > 24)
        throw new Error(`Pagina ${index + 1}: l'ora della previsione di domani va da 0 a 24`);
      out.tomorrow_from = hour;
    }
    if (page.layout === "list" || page.layout === "face")
      out.widgets = page.widgets.filter(widget => widget && widget.widget).map(widget => {
        const face = page.layout === "face";
        const base = { widget: widget.widget, style: widget.style || (face ? "icon_value" : "label_value") };
        if (face) base.slot = widget.slot;
        if (!textWidgets[widget.widget]) return base;
        const text = widget.widget === "countdown" ? (widget.text || "").trim() : widget.text || "";
        if (utf8Length(text) > 32 || /[\u0000-\u001f\u007f]/.test(text))
          throw new Error(`Pagina ${index + 1}: il testo può avere al massimo 32 caratteri`);
        if (widget.widget === "countdown" && !COUNTDOWN_PATTERN.test(text))
          throw new Error(`Pagina ${index + 1}: il conto alla rovescia vuole la data come 2026-12-24 seguita dal nome`);
        return { ...base, text };
      });
    return out;
  });
  return { theme: { ...theme }, pages: list };
}

/* ---- Alarm ---- */

function applyWake(wake) {
  $("wake-sunrise").value = wake.sunrise;
  $("wake-screen").value = wake.screen;
  $("wake-blink").checked = wake.blink;
  $("wake-summary").textContent = wakePhaseLabels[wake.phase] ?? wake.phase;
  $("dismiss-wake").disabled = wake.phase === "idle";
}

function minutesField(id, max, label) {
  const value = Number($(id).value);
  if (!Number.isInteger(value) || value < 0 || value > max) throw new Error(`${label}: da 0 a ${max} minuti`);
  return value;
}

/* ---- Calibration ---- */

function calibrationValid(value) {
  const radius = value.diameter / 2;
  return value.diameter >= 80 && value.diameter <= 128 && value.diameter % 2 === 0 &&
    value.center_x >= radius && value.center_x <= 128 - radius &&
    value.center_y >= radius && value.center_y <= 128 - radius;
}

const calibrationMoves = {
  "calibration-up": { center_y: -1 }, "calibration-down": { center_y: 1 },
  "calibration-left": { center_x: -1 }, "calibration-right": { center_x: 1 },
  "calibration-smaller": { diameter: -2 }, "calibration-larger": { diameter: 2 },
};

function moved(value, change) {
  const next = { ...value };
  for (const [key, delta] of Object.entries(change)) next[key] += delta;
  return next;
}

function renderCalibration() {
  if (!calibration) return;
  $("calibration-summary").textContent =
    `Centro: ${calibration.center_x}, ${calibration.center_y} · Diametro: ${calibration.diameter} px` +
    ` · Rotazione: ${calibration.rotation}°` + (calibration.mirror ? " · Specchiata" : "") +
    (calibration.active ? " · Anteprima in corso" : " · Salvata");
  const idle = !calibration.active || calibrationPending;
  $("calibration-start").disabled = calibration.active || calibrationPending;
  for (const id of ["calibration-save", "calibration-cancel", "calibration-rotate", "calibration-mirror"])
    $(id).disabled = idle;
  $("calibration-mirror").setAttribute("aria-pressed", String(calibration.mirror));
  for (const [id, change] of Object.entries(calibrationMoves))
    $(id).disabled = idle || !calibrationValid(moved(calibration, change));
}

async function changeCalibration(body) {
  if (calibrationPending) return;
  calibrationPending = true;
  renderCalibration();
  try { calibration = await request("/api/calibration", "POST", body); }
  finally { calibrationPending = false; renderCalibration(); }
}

function adjustCalibration(change) {
  const { center_x, center_y, diameter, rotation, mirror } = { ...calibration, ...change };
  return changeCalibration({ center_x, center_y, diameter, rotation, mirror });
}

/* ---- Loading ---- */

async function loadAll() {
  const [status, configuration, pageData, calibrationData, wake] = await Promise.all([
    request("/api/status"), request("/api/config"), request("/api/pages"),
    request("/api/calibration"), request("/api/wake"),
  ]);
  showApp(true);
  savedPages = pageData.pages;
  pages = structuredClone(savedPages);
  theme = { ...pageData.theme };
  calibration = calibrationData;
  $("ha-token").value = token;
  $("timezone").value = configuration.timezone;
  $("discovery").checked = configuration.discovery;
  renderPageSelect();
  renderPages();
  renderTheme();
  renderCalibration();
  applyStatus(status);
  applyWake(wake);
}

/* ---- Events ---- */

$("token").value = token;
$("pair-button").hidden = location.hostname !== "192.168.4.1";
$("login-button").addEventListener("click", () => run(async () => {
  token = $("token").value.trim();
  await loadAll();
  storeToken(token);
}));
$("token").addEventListener("keydown", event => { if (event.key === "Enter") $("login-button").click(); });
$("pair-button").addEventListener("click", () => run(async () => {
  token = (await request("/api/pair")).token;
  $("token").value = token;
  storeToken(token);
  await loadAll();
}));
$("show-ha-token").addEventListener("click", () => {
  const input = $("ha-token");
  input.type = input.type === "password" ? "text" : "password";
  $("show-ha-token").textContent = input.type === "text" ? "Nascondi token" : "Mostra e seleziona token";
  if (input.type === "text") { input.focus(); input.select(); }
});

$("power").addEventListener("change", () => run(() => display({ power: $("power").checked })));
$("brightness").addEventListener("input", () => { $("brightness-value").textContent = `${$("brightness").value}%`; });
$("brightness").addEventListener("change", () => run(() => display({ brightness: Number($("brightness").value) })));
$("page-timeout").addEventListener("change", () => run(async () => {
  const seconds = Number($("page-timeout").value);
  if (!Number.isInteger(seconds) || (seconds !== 0 && (seconds < 5 || seconds > 3600)))
    throw new Error("Usa 0 (mai) oppure un valore da 5 a 3600 secondi");
  await display({ page_timeout: seconds });
  message(seconds ? `La pagina Ora torna dopo ${seconds} secondi` : "La pagina scelta resta visibile");
}));
$("carousel").addEventListener("change", () => run(async () => {
  const seconds = Number($("carousel").value);
  if (!Number.isInteger(seconds) || (seconds !== 0 && (seconds < 5 || seconds > 600)))
    throw new Error("Usa 0 (spenta) oppure un valore da 5 a 600 secondi");
  await display({ carousel: seconds });
  message(seconds ? `Le pagine ruotano ogni ${seconds} secondi` : "Rotazione delle pagine spenta");
}));
$("current-page").addEventListener("change", () => run(() => display({ page: $("current-page").value })));
for (const [button, move] of [["previous-page", "previous"], ["next-page", "next"]])
  $(button).addEventListener("click", () => run(() => display({ move })));

$("save-wake").addEventListener("click", () => run(async () => {
  const sunrise = minutesField("wake-sunrise", 60, "Luce dell'alba");
  const screen = minutesField("wake-screen", 30, "Schermata Buongiorno");
  applyWake(await request("/api/wake", "POST", { sunrise, screen, blink: $("wake-blink").checked }));
  message(sunrise || screen || $("wake-blink").checked ? "Sveglia salvata" : "Sveglia salvata: il proiettore non fa nulla alla sveglia");
}));
$("test-wake").addEventListener("click", () => run(async () => {
  const wake = await request("/api/wake", "POST", { test: true });
  applyWake(wake);
  message(wake.sunrise ? "Prova in corso: un minuto di alba, poi la sveglia" : "Prova in corso: la sveglia parte subito");
}));
$("dismiss-wake").addEventListener("click", () => run(async () => {
  applyWake(await request("/api/wake", "POST", { dismiss: true }));
  message("Sveglia fermata");
}));

for (const [id, mode, done] of [["calibration-start", "start", ""],
                                ["calibration-save", "save", "Regolazione salvata"],
                                ["calibration-cancel", "cancel", "Regolazione annullata"]])
  $(id).addEventListener("click", () => run(async () => { await changeCalibration({ mode }); message(done); }));
for (const [id, change] of Object.entries(calibrationMoves))
  $(id).addEventListener("click", () => run(async () => {
    const next = moved(calibration, change);
    if (calibrationValid(next)) await adjustCalibration(next);
  }));
$("calibration-rotate").addEventListener("click", () => run(() =>
  adjustCalibration({ rotation: (calibration.rotation + 90) % 360 })));
$("calibration-mirror").addEventListener("click", () => run(() =>
  adjustCalibration({ mirror: !calibration.mirror })));

$("add-page").addEventListener("click", () => {
  let n = 1;
  while (pages.some(page => page.id === `page${n}`)) n++;
  pages.push({ id: `page${n}`, name: `Pagina ${n}`, layout: "list", size: "normal", show: {},
               widgets: [{ widget: "entity1", style: "label_value" }] });
  renderPages();
});
$("save-pages").addEventListener("click", () => run(async () => {
  const saved = await request("/api/pages", "PUT", pagesDocument());
  savedPages = saved.pages;
  pages = structuredClone(savedPages);
  theme = { ...saved.theme };
  renderPageSelect();
  renderPages();
  renderTheme();
  applyStatus(await request("/api/status"));
  message("Pagine salvate");
}));

$("theme-palette").addEventListener("change", () => { theme.palette = $("theme-palette").value; renderTheme(); });
$("theme-tint").addEventListener("input", () => { theme.tint = $("theme-tint").value; });
$("theme-dots").addEventListener("change", () => { theme.dots = $("theme-dots").checked; });
$("cancel-preview").addEventListener("click", () => run(async () => {
  applyStatus(await request("/api/pages/preview", "POST", { cancel: true }));
  message("Prova annullata: il proiettore mostra di nuovo le pagine salvate");
}));

$("discovery").addEventListener("change", () => run(async () => {
  try {
    const configuration = await request("/api/config", "POST", { discovery: $("discovery").checked });
    $("discovery").checked = configuration.discovery;
  } catch (error) {
    $("discovery").checked = !$("discovery").checked;
    throw error;
  }
  message($("discovery").checked ? "Rilevamento automatico attivo" : "Rilevamento automatico disattivato");
}));

$("save-network").addEventListener("click", () => {
  const ssid = $("ssid").value.trim();
  const password = $("wifi-password").value;
  if (!ssid || utf8Length(ssid) > 32) return message("Il nome della rete deve avere da 1 a 32 caratteri");
  if (password && (password.length < 8 || utf8Length(password) > 63))
    return message("La password Wi-Fi deve avere da 8 a 63 caratteri, oppure essere vuota per una rete aperta");
  if (!$("timezone").value.trim()) return message("Indica il fuso orario");
  message("");
  $("network-confirm-ssid").textContent = ssid;
  $("network-confirm").hidden = false;
});
$("network-confirm-no").addEventListener("click", () => { $("network-confirm").hidden = true; });
$("network-confirm-yes").addEventListener("click", () => run(async () => {
  $("network-confirm").hidden = true;
  await request("/api/network", "POST", {
    ssid: $("ssid").value.trim(), password: $("wifi-password").value, timezone: $("timezone").value.trim(),
  });
  message("Rete salvata: il dispositivo si riavvia. Se cambia indirizzo, riapri la pagina dal nuovo IP.");
  await reloadWhenBack();
}));

$("upload-firmware").addEventListener("click", () => run(async () => {
  const file = $("firmware").files[0];
  if (!file || !file.name.endsWith(".bin")) throw new Error("Seleziona un file .bin");
  const progress = $("firmware-progress");
  progress.hidden = false;
  $("upload-firmware").disabled = true;
  try {
    await upload("/api/ota", file, fraction => {
      progress.value = Math.round(fraction * 100);
      message(fraction < 1 ? `Caricamento ${progress.value}%` : "Verifica del firmware…");
    });
  } finally {
    $("upload-firmware").disabled = false;
  }
  message("Firmware installato: il dispositivo si riavvia, la pagina si ricarica da sola");
  await reloadWhenBack();
}));

$("logout").addEventListener("click", () => {
  storeToken("");
  token = "";
  $("token").value = $("ha-token").value = "";
  $("ha-token").type = "password";
  $("show-ha-token").textContent = "Mostra e seleziona token";
  showApp(false);
});
document.addEventListener("visibilitychange", pollStatus);

if (token) run(loadAll);
