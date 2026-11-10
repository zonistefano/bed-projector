const $ = id => document.getElementById(id);
const TOKEN_KEY = "bed-projector-token";
const STATUS_POLL_MS = 15000;
const MAX_PAGES = 5;
const MAX_WIDGETS = 3;

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
  moon: "Fase lunare", thermostat: "Termostato",
};
const layoutLabels = {
  home: "Casa (allarme, temperature, aperture, luci)", weather: "Meteo (adesso, giorno e sera)",
  agenda: "Agenda (sveglia ed eventi)", climate: "Termostato (rotella con temperatura impostata e attuale)",
  list: "Elenco widget",
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
let calibration = null;
let calibrationPending = false;
let pollTimer = null;

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
  $("wake-display").value = status.wake_display;
  const current = savedPages.find(page => page.id === status.page);
  $("state-summary").textContent = `Pagina: ${current ? current.name : status.page} · ` +
    `Wi-Fi: ${status.wifi_connected ? "connesso" : "non connesso"}`;
  $("current-page").value = status.page;
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

function renderPages() {
  $("pages").replaceChildren(...pages.map((page, index) => {
    const title = element("h3", { textContent: `${index + 1}. ${page.name}` });
    const grid = element("div", { className: "page-grid" }, [
      pageField("Nome", page.name, 16, value => { page.name = value; title.textContent = `${index + 1}. ${value}`; }),
    ]);
    if (index > 0)
      grid.append(pageSelect("Modello", Object.entries(layoutLabels), page.layout,
                             value => { page.layout = value; renderPages(); }));
    // Ora, Casa, Meteo, Agenda e Termostato hanno un design fisso: i widget servono solo al modello Elenco.
    if (page.layout === "list") {
      for (let slot = 0; slot < MAX_WIDGETS; slot++)
        grid.append(pageSelect(`Widget ${slot + 1}`, [["", "Nessuno"], ...Object.entries(widgetLabels)],
                               page.widgets[slot] || "", value => { page.widgets[slot] = value; }));
      grid.append(pageField("Testo (widget Testo libero)", page.text || "", 32, value => { page.text = value; }));
    }
    const card = element("div", { className: "page-editor" }, [title, grid]);
    if (index > 0) {
      const swap = target => { [pages[index], pages[target]] = [pages[target], pages[index]]; };
      card.append(element("div", { className: "actions" }, [
        pageButton("Su", index === 1, () => swap(index - 1)),
        pageButton("Giù", index === pages.length - 1, () => swap(index + 1)),
        pageButton("Rimuovi", false, () => pages.splice(index, 1)),
      ]));
    }
    return card;
  }));
  $("add-page").disabled = pages.length >= MAX_PAGES;
}

function validatePages() {
  for (const [index, page] of pages.entries()) {
    const name = page.name.trim();
    if (!name || utf8Length(name) > 16 || /[\u0000-\u001f\u007f]/.test(name))
      throw new Error(`Pagina ${index + 1}: il nome deve avere da 1 a 16 caratteri`);
    if (page.layout === "list" && utf8Length(page.text || "") > 32)
      throw new Error(`Pagina ${index + 1}: il testo può avere al massimo 32 caratteri`);
  }
  return pages.map(page => ({ ...page, name: page.name.trim(), widgets: page.widgets.filter(Boolean) }));
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
  const [status, configuration, pageData, calibrationData] = await Promise.all([
    request("/api/status"), request("/api/config"), request("/api/pages"),
    request("/api/calibration"),
  ]);
  showApp(true);
  savedPages = pageData.pages;
  pages = structuredClone(savedPages);
  calibration = calibrationData;
  $("ha-token").value = token;
  $("timezone").value = configuration.timezone;
  renderPageSelect();
  renderPages();
  renderCalibration();
  applyStatus(status);
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
$("wake-display").addEventListener("change", () => run(() => display({ wake_display: $("wake-display").value })));
$("current-page").addEventListener("change", () => run(() => display({ page: $("current-page").value })));
for (const [button, move] of [["previous-page", "previous"], ["next-page", "next"]])
  $(button).addEventListener("click", () => run(() => display({ move })));

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
  pages.push({ id: `page${n}`, name: `Pagina ${n}`, layout: "list", widgets: ["entity1"], text: "" });
  renderPages();
});
$("save-pages").addEventListener("click", () => run(async () => {
  savedPages = (await request("/api/pages", "PUT", { pages: validatePages() })).pages;
  pages = structuredClone(savedPages);
  renderPageSelect();
  renderPages();
  applyStatus(await request("/api/status"));
  message("Pagine salvate");
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
