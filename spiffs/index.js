const $ = id => document.getElementById(id);
const alarmLabels = {
  disarmed: "disinserito", armed_home: "in casa", armed_away: "fuori casa", armed_night: "notte",
  armed_vacation: "vacanza", armed_custom_bypass: "personalizzato", arming: "in inserimento",
  pending: "in attesa", triggered: "SCATTATO", unknown: "?",
};
const widgets = ["openings", "alarm", "date", "weather", "entity1", "entity2", "entity3", "entity4", "text"];
let token = localStorage.getItem("bed-projector-token") || "";
let pages = [];
let calibration = null;
let calibrationPending = false;

function calibrationValid(value) {
  const radius = value.diameter / 2;
  return value.diameter >= 80 && value.diameter <= 128 && value.diameter % 2 === 0 &&
    value.center_x >= radius && value.center_x <= 128 - radius &&
    value.center_y >= radius && value.center_y <= 128 - radius;
}

function renderCalibration() {
  if (!calibration) return;
  $("calibration-summary").textContent =
    `Centro: ${calibration.center_x}, ${calibration.center_y} · Diametro: ${calibration.diameter} px` +
    ` · Rotazione: ${calibration.rotation}°` + (calibration.mirror ? " · Specchiata" : "") +
    (calibration.active ? " · Anteprima in corso" : " · Salvata");
  $("calibration-start").disabled = calibration.active || calibrationPending;
  $("calibration-save").disabled = !calibration.active || calibrationPending;
  $("calibration-cancel").disabled = !calibration.active || calibrationPending;
  const moves = {
    "calibration-up": { center_y: calibration.center_y - 1 },
    "calibration-down": { center_y: calibration.center_y + 1 },
    "calibration-left": { center_x: calibration.center_x - 1 },
    "calibration-right": { center_x: calibration.center_x + 1 },
    "calibration-smaller": { diameter: calibration.diameter - 2 },
    "calibration-larger": { diameter: calibration.diameter + 2 }
  };
  for (const id of ["calibration-rotate", "calibration-mirror"])
    $(id).disabled = !calibration.active || calibrationPending;
  $("calibration-mirror").setAttribute("aria-pressed", String(calibration.mirror));
  for (const [id, change] of Object.entries(moves))
    $(id).disabled = !calibration.active || calibrationPending ||
      !calibrationValid({ ...calibration, ...change });
}

function calibrationBody(value) {
  return { center_x: value.center_x, center_y: value.center_y, diameter: value.diameter,
           rotation: value.rotation, mirror: value.mirror };
}

async function changeCalibration(body) {
  if (calibrationPending) return;
  calibrationPending = true;
  renderCalibration();
  try { calibration = await request("/api/v1/calibration", "POST", body); }
  finally { calibrationPending = false; renderCalibration(); }
}

function message(value) { $("message").textContent = value; }
async function request(path, method = "GET", body = undefined) {
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  if (body && !(body instanceof Blob)) headers["Content-Type"] = "application/json";
  const response = await fetch(path, { method, headers, body: body instanceof Blob ? body : body ? JSON.stringify(body) : undefined });
  let data = {};
  try { data = await response.json(); } catch (_) { /* Binary OTA response can be interrupted by reboot. */ }
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

function option(value, label) {
  const item = document.createElement("option");
  item.value = value;
  item.textContent = label;
  return item;
}

function field(labelText, value, maxLength, onChange) {
  const wrap = document.createElement("label");
  wrap.textContent = labelText;
  const input = document.createElement("input");
  input.value = value;
  input.maxLength = maxLength;
  input.addEventListener("input", () => onChange(input.value));
  wrap.append(input);
  return wrap;
}

function renderPages() {
  const container = $("pages");
  container.replaceChildren();
  pages.forEach((page, index) => {
    const card = document.createElement("div");
    card.className = "page-editor";
    const title = document.createElement("h3");
    title.textContent = `${index + 1}. ${page.name}`;
    card.append(title);
    const grid = document.createElement("div");
    grid.className = "page-grid";
    grid.append(field("Nome", page.name, 16, value => { page.name = value; title.textContent = `${index + 1}. ${value}`; }));
    if (index > 0) {
      const layoutWrap = document.createElement("label");
      layoutWrap.textContent = "Modello";
      const layout = document.createElement("select");
      [["home", "Casa (allarme, temperature, aperture, luci)"], ["weather", "Meteo (adesso, giorno e sera)"],
       ["list", "Elenco widget"]].forEach(([value, label]) => layout.append(option(value, label)));
      layout.value = page.layout;
      layout.addEventListener("change", () => { page.layout = layout.value; renderPages(); });
      layoutWrap.append(layout);
      grid.append(layoutWrap);
    }
    // Ora, Casa e Meteo hanno un design fisso: i widget servono solo al modello Elenco.
    for (let slot = 0; page.layout === "list" && slot < 3; slot++) {
      const wrap = document.createElement("label");
      wrap.textContent = `Widget ${slot + 1}`;
      const select = document.createElement("select");
      select.append(option("", "Nessuno"));
      widgets.forEach(value => select.append(option(value, value)));
      select.value = page.widgets[slot] || "";
      select.addEventListener("change", () => { page.widgets[slot] = select.value; });
      wrap.append(select);
      grid.append(wrap);
    }
    if (page.layout === "list")
      grid.append(field("Testo (widget text)", page.text || "", 32, value => { page.text = value; }));
    card.append(grid);
    if (index > 0) {
      const actions = document.createElement("div");
      actions.className = "actions";
      for (const [label, target] of [["Su", index - 1], ["Giù", index + 1]]) {
        const button = document.createElement("button");
        button.textContent = label;
        button.disabled = target === 0 || target >= pages.length;
        button.addEventListener("click", () => { [pages[index], pages[target]] = [pages[target], pages[index]]; renderPages(); });
        actions.append(button);
      }
      const remove = document.createElement("button");
      remove.textContent = "Rimuovi";
      remove.addEventListener("click", () => { pages.splice(index, 1); renderPages(); });
      actions.append(remove);
      card.append(actions);
    }
    container.append(card);
  });
  $("add-page").disabled = pages.length >= 5;
}

async function refresh() {
  const [status, configuration, pageData, calibrationData] = await Promise.all([
    request("/api/v1/status"), request("/api/v1/config"), request("/api/v1/pages"),
    request("/api/v1/calibration")
  ]);
  $("login").hidden = true;
  $("app").hidden = false;
  $("power").checked = status.power;
  $("brightness").value = status.brightness;
  $("brightness-value").textContent = `${status.brightness}%`;
  $("page-timeout").value = status.page_timeout ?? 30;
  $("state-summary").textContent = `Pagina: ${status.page} · Wi-Fi: ${status.wifi_connected ? "connesso" : "non connesso"}`;
  $("ha-summary").textContent = status.ha_fresh ?
    `Aperture: ${status.openings ?? "?"} · Luci: ${status.lights ?? "?"} · Allarme: ${alarmLabels[status.alarm] ?? status.alarm} · Meteo: ${status.weather || "?"}` :
    "Dati Home Assistant sconosciuti o non aggiornati";
  $("ha-token").value = token;
  $("timezone").value = configuration.timezone;
  pages = pageData.pages;
  calibration = calibrationData;
  renderCalibration();
  renderPages();
  const select = $("current-page");
  select.replaceChildren();
  pages.forEach(page => select.append(option(page.id, page.name)));
  select.value = status.page;
}

async function run(action) {
  try { message(""); await action(); }
  catch (error) { message(error.message); if (error.message === "unauthorized") { $("login").hidden = false; $("app").hidden = true; } }
}

$("token").value = token;
$("pair-button").hidden = location.hostname !== "192.168.4.1";
$("login-button").addEventListener("click", () => run(async () => {
  token = $("token").value.trim();
  await refresh();
  localStorage.setItem("bed-projector-token", token);
}));
$("pair-button").addEventListener("click", () => run(async () => {
  const response = await request("/api/v1/pair");
  token = response.token;
  $("token").value = token;
  localStorage.setItem("bed-projector-token", token);
  await refresh();
}));
$("show-ha-token").addEventListener("click", () => {
  const input = $("ha-token");
  input.type = input.type === "password" ? "text" : "password";
  $("show-ha-token").textContent = input.type === "text" ?
    "Nascondi token" : "Mostra e seleziona token";
  if (input.type === "text") { input.focus(); input.select(); }
});
$("power").addEventListener("change", () => run(async () => {
  await request("/api/v1/display", "POST", { power: $("power").checked });
  await refresh();
}));
$("brightness").addEventListener("input", () => { $("brightness-value").textContent = `${$("brightness").value}%`; });
$("page-timeout-save").addEventListener("click", () => run(async () => {
  const seconds = Number($("page-timeout").value);
  if (!Number.isInteger(seconds) || (seconds !== 0 && (seconds < 5 || seconds > 3600)))
    throw new Error("Usa 0 (mai) oppure un valore da 5 a 3600 secondi");
  await request("/api/v1/display", "POST", { page_timeout: seconds });
  message(seconds ? `La pagina Ora torna dopo ${seconds} secondi` : "La pagina scelta resta visibile");
  await refresh();
}));
$("brightness-save").addEventListener("click", () => run(async () => {
  await request("/api/v1/display", "POST", { brightness: Number($("brightness").value) });
  await refresh();
}));
for (const [id, mode] of [["calibration-start", "start"], ["calibration-save", "save"],
                          ["calibration-cancel", "cancel"]])
  $(id).addEventListener("click", () => run(async () => {
    await changeCalibration({ mode });
    if (mode === "save") message("Regolazione salvata");
    if (mode === "cancel") message("Regolazione annullata");
  }));
for (const [id, change] of [
  ["calibration-up", { center_y: -1 }], ["calibration-down", { center_y: 1 }],
  ["calibration-left", { center_x: -1 }], ["calibration-right", { center_x: 1 }],
  ["calibration-smaller", { diameter: -2 }], ["calibration-larger", { diameter: 2 }]
]) $(id).addEventListener("click", () => run(async () => {
  const next = { ...calibration };
  for (const [key, delta] of Object.entries(change)) next[key] += delta;
  if (calibrationValid(next)) await changeCalibration(calibrationBody(next));
}));
for (const [id, change] of [
  ["calibration-rotate", value => ({ rotation: (value.rotation + 90) % 360 })],
  ["calibration-mirror", value => ({ mirror: !value.mirror })]
]) $(id).addEventListener("click", () => run(async () => {
  await changeCalibration(calibrationBody({ ...calibration, ...change(calibration) }));
}));
$("current-page").addEventListener("change", () => run(async () => {
  await request("/api/v1/display", "POST", { page: $("current-page").value });
  await refresh();
}));
for (const [button, move] of [["previous-page", "previous"], ["next-page", "next"]])
  $(button).addEventListener("click", () => run(async () => {
    await request("/api/v1/display", "POST", { move });
    await refresh();
  }));
$("add-page").addEventListener("click", () => {
  let n = 1;
  while (pages.some(page => page.id === `page${n}`)) n++;
  pages.push({ id: `page${n}`, name: `Pagina ${n}`, layout: "list", widgets: ["entity1"], text: "" });
  renderPages();
});
$("save-pages").addEventListener("click", () => run(async () => {
  pages.forEach(page => { page.widgets = page.widgets.filter(Boolean); });
  await request("/api/v1/pages", "PUT", { pages });
  message("Pagine salvate");
  await refresh();
}));
$("save-network").addEventListener("click", () => run(async () => {
  await request("/api/v1/network", "POST", {
    ssid: $("ssid").value, password: $("wifi-password").value, timezone: $("timezone").value
  });
  message("Rete salvata: il dispositivo si riavvia");
}));
$("upload-firmware").addEventListener("click", () => run(async () => {
  const file = $("firmware").files[0];
  if (!file || !file.name.endsWith(".bin")) throw new Error("Seleziona un file .bin");
  await request("/api/v1/ota", "POST", file);
  message("Firmware caricato: il dispositivo si riavvia");
}));
$("logout").addEventListener("click", () => {
  localStorage.removeItem("bed-projector-token");
  token = "";
  $("token").value = "";
  $("ha-token").value = "";
  $("ha-token").type = "password";
  $("show-ha-token").textContent = "Mostra e seleziona token";
  $("app").hidden = true;
  $("login").hidden = false;
});
if (token) run(refresh);
