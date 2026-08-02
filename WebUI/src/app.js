import "./styles.css";
import { $, api, svgIcon, textCell, toast } from "./shared.js";
import { createLogsModule } from "./modules/logs.js";

"use strict";

const state = {
  devices: [],
  hostReachable: true,
  selectedId: null,
  tab: "logs",
  fileRoot: "saved",
  filePath: "",
  fileEntries: [],
  fileHistory: [""],
  fileHistoryIndex: 0,
  transferPoll: null,
  lastTransferRequest: null,
  consoleEntries: [],
  consoleIndex: [],
  consoleIndexDeviceId: null,
  consoleTotal: 0,
  consoleCatalogTotal: 0,
  consoleType: "",
  consoleSource: "",
  consoleSelected: null,
  consoleQueryTimer: null,
  commandHistory: JSON.parse(localStorage.getItem("deviceexplorer.commandHistory") || "[]"),
  commandHistoryIndex: -1,
  completions: [],
  completionIndex: -1,
  completionTimer: null,
  completionPinned: false,
  commandHint: null,
  moduleTimer: null,
  moduleRequestSerial: 0,
  moduleSchema: null,
  modulePageId: null,
  moduleValues: {},
};

const elements = {
  deviceList: $("#device-list"),
  deviceCount: $("#device-count"),
  deviceSearch: $("#device-search"),
  title: $("#device-title"),
  subtitle: $("#device-subtitle"),
  projectName: $("#project-name"),
  devicePlatform: $("#device-platform"),
  capabilities: $("#device-capabilities"),
  connection: $("#connection-state"),
  empty: $("#empty-state"),
  workspace: $("#workspace"),
  tabs: $("#tabs"),
  moduleTabs: $("#module-tabs"),
  commandForm: $("#command-form"),
  commandShortcutsWrap: $("#command-shortcuts-wrap"),
  commandShortcuts: $("#command-shortcuts"),
  commandInput: $("#command-input"),
  completions: $("#command-completions"),
  commandOutput: $("#command-output"),
  consoleSearch: $("#console-search"),
  consoleScope: $("#console-scope"),
  consoleScopeField: $("#console-scope-field"),
  consoleSourceFilter: $("#console-source"),
  consoleCatalog: $("#console-catalog"),
  consoleCount: $("#console-count"),
  consoleDetail: $("#console-detail"),
  fileRoot: $("#file-root"),
  fileList: $("#file-list"),
  fileSearch: $("#file-search"),
  fileSort: $("#file-sort"),
  fileStatus: $("#file-status"),
  breadcrumbs: $("#file-breadcrumbs"),
  fileBack: $("#file-back"),
  fileForward: $("#file-forward"),
  transfer: $("#transfer-banner"),
  moduleTitle: $("#module-title"),
  moduleDescription: $("#module-description"),
  moduleActions: $("#module-actions"),
  modulePages: $("#module-pages"),
  moduleContent: $("#module-content"),
  confirmOverlay: $("#confirm-overlay"),
  confirmTitle: $("#confirm-title"),
  confirmBody: $("#confirm-body"),
  confirmDevice: $("#confirm-device"),
  confirmCommand: $("#confirm-command"),
  confirmCancel: $("#confirm-cancel"),
  confirmAccept: $("#confirm-accept"),
};

function selectedDevice() {
  return state.devices.find((device) => device.id === state.selectedId) || null;
}

const logs = createLogsModule({ getDevice: selectedDevice });

function requireOnline() {
  const device = selectedDevice();
  if (!device?.connected) {
    toast("The selected device is offline. Launch or reconnect the build first.", true);
    return null;
  }
  return device;
}

async function refreshDevices() {
  try {
    const data = await api("/api/devices");
    state.devices = data.devices || [];
    state.hostReachable = true;
    if (!state.selectedId && state.devices.length) {
      state.selectedId = (state.devices.find((device) => device.connected) || state.devices[0]).id;
    }
    if (state.selectedId && !state.devices.some((device) => device.id === state.selectedId)) {
      state.selectedId = state.devices[0]?.id || null;
    }
    renderDevices();
    renderSelectedDevice();
  } catch (error) {
    // The device list is polled every couple of seconds, so an unreachable host is a state to display once,
    // not an event to announce on every tick.
    if (state.hostReachable) toast(`Host API unavailable: ${error.message}`, true);
    state.hostReachable = false;
    state.devices = [];
    state.consoleIndexDeviceId = null;
    renderDevices();
    renderSelectedDevice();
  }
}

function renderDevices() {
  const query = elements.deviceSearch.value.trim().toLowerCase();
  const devices = state.devices.filter((device) =>
    !query || [device.name, device.project_name, device.platform, device.configuration]
      .some((value) => String(value || "").toLowerCase().includes(query)));
  elements.deviceCount.textContent = String(state.devices.length);
  elements.deviceList.replaceChildren();

  for (const device of devices) {
    const button = document.createElement("button");
    button.className = `device-card ${device.connected ? "online" : ""} ${device.id === state.selectedId ? "active" : ""}`;
    button.type = "button";

    const dot = document.createElement("span");
    dot.className = "dot";
    const copy = document.createElement("span");
    const name = document.createElement("strong");
    name.textContent = device.name || device.id;
    const meta = document.createElement("span");
    meta.className = "device-meta";
    meta.textContent = `${device.project_name || "Project"} · ${device.platform || "Unknown"}`;
    copy.append(name, meta);
    const age = document.createElement("span");
    age.className = "device-age";
    age.textContent = device.connected ? "live" : formatRelative(device.last_seen);
    button.append(dot, copy, age);
    button.addEventListener("click", () => selectDevice(device.id));
    elements.deviceList.append(button);
  }

  if (!devices.length && state.devices.length) {
    elements.deviceList.append(textCell("inline-empty", "No devices match the filter."));
  }
  if (!state.hostReachable) {
    elements.deviceList.append(textCell("inline-empty", "Host is not reachable. Retrying…"));
  }
}

function renderSelectedDevice() {
  const device = selectedDevice();
  elements.empty.classList.toggle("hidden", Boolean(device));
  elements.workspace.classList.toggle("hidden", !device);
  if (!device) {
    elements.title.textContent = state.hostReachable ? "Waiting for a device" : "Host is not reachable";
    elements.subtitle.textContent = state.hostReachable
      ? "Launch a non-Shipping build on the same network."
      : "The DeviceExplorer host is not answering. Start it and this page will reconnect on its own.";
    elements.projectName.textContent = "No project";
    elements.devicePlatform.textContent = "Device";
    elements.connection.innerHTML = "<span></span> Offline";
    elements.connection.className = "status offline";
    elements.capabilities.replaceChildren();
    elements.moduleTabs.replaceChildren();
    return;
  }

  elements.title.textContent = device.name || device.id;
  elements.subtitle.textContent =
    `${device.configuration || "Unknown config"} · ${device.build_version || "unknown build"} · uptime ${formatDuration(device.uptime_seconds)}`;
  elements.projectName.textContent = device.project_name || "Unreal project";
  elements.devicePlatform.textContent = `${device.platform || "Unknown"} · UE ${shortEngineVersion(device.engine_version)}`;
  elements.connection.replaceChildren(document.createElement("span"), document.createTextNode(
    device.connected ? " Online" : ` Offline · ${formatRelative(device.last_seen)}`));
  elements.connection.className = `status ${device.connected ? "online" : "offline"}`;
  logs.setDevice(device.id);
  renderCapabilities(device);
  renderCommandShortcuts(device);
  renderFileRoots(device);
  renderModuleTabs(device);
}

function renderCapabilities(device) {
  const capabilities = device.capabilities?.length ? device.capabilities : ["logs", "console", "files"];
  const available = new Set(capabilities);
  elements.capabilities.replaceChildren(...capabilities.map((item) => textCell("", item)));
  const visibleTabs = [];
  document.querySelectorAll(".tab[data-capability]").forEach((button) => {
    const visible = available.has(button.dataset.capability);
    button.classList.toggle("hidden", !visible);
    if (visible) visibleTabs.push(button.dataset.tab);
  });
  if (!state.tab.startsWith("module:") && !visibleTabs.includes(state.tab) && visibleTabs.length) setTab(visibleTabs[0]);
}

function renderCommandShortcuts(device) {
  elements.commandShortcuts.replaceChildren();
  const commands = device.commands || [];

  const groups = new Map();
  for (const command of commands) {
    const category = command.category || "General";
    if (!groups.has(category)) groups.set(category, []);
    groups.get(category).push(command);
  }

  for (const [category, items] of groups) {
    const group = document.createElement("div");
    group.className = "shortcut-group";
    const label = document.createElement("span");
    label.className = "shortcut-group-label";
    label.textContent = category;
    const row = document.createElement("div");
    row.className = "shortcut-group-row";
    for (const command of items) row.append(commandShortcutButton(device, command));
    group.append(label, row);
    elements.commandShortcuts.append(group);
  }

  elements.commandShortcutsWrap.classList.toggle("hidden", !commands.length);
}

function commandShortcutButton(device, command) {
  const button = document.createElement("button");
  button.className = "button ghost";
  button.title = command.description || command.id;
  button.append(textCell(`action-dot ${command.requires_confirmation ? "confirm" : ""}`, ""), document.createTextNode(command.label || command.id));
  button.addEventListener("click", async () => {
    if (command.requires_confirmation) {
      const commandText = command.command || command.id;
      const accepted = await confirmAction({
        title: `${command.label || command.id}?`,
        body: "This runs an extension action on the connected build. It may briefly stall the game thread.",
        deviceName: device.name || device.id,
        commandLabel: commandText,
        acceptLabel: command.label || command.id,
      });
      if (!accepted) return;
    }
    executeCommand(command.command || command.id, command.id);
  });
  return button;
}

function renderFileRoots(device) {
  const roots = device.file_roots?.length ? device.file_roots : [{ id: "saved", label: "Saved" }];
  if (!roots.some((root) => root.id === state.fileRoot)) resetFileNavigation(roots[0].id);
  const currentOptions = [...elements.fileRoot.options].map((option) => option.value).join("|");
  const nextOptions = roots.map((root) => root.id).join("|");
  if (currentOptions !== nextOptions) {
    elements.fileRoot.replaceChildren(...roots.map((root) => {
      const option = document.createElement("option");
      option.value = root.id;
      option.textContent = root.label || root.id;
      return option;
    }));
  }
  elements.fileRoot.value = state.fileRoot;
  renderBreadcrumbs();
}

function renderModuleTabs(device) {
  const modules = device.data_modules || [];
  const signature = modules.map((module) => `${module.id}:${module.label}`).join("|");
  if (elements.moduleTabs.dataset.signature === signature) return;
  elements.moduleTabs.dataset.signature = signature;
  elements.moduleTabs.replaceChildren(...modules.map((module) => {
    const button = document.createElement("button");
    button.className = `tab ${state.tab === `module:${module.id}` ? "active" : ""}`;
    button.dataset.tab = `module:${module.id}`;
    const iconWrap = document.createElement("span");
    iconWrap.className = "tab-icon";
    iconWrap.append(svgIcon("", module.icon === "pulse" ? "M1.6 8.4h2.9l2-4.2 2.7 7.6 1.9-3.4h3.3" : "M8 2.2 13.8 5.4v5.2L8 13.8 2.2 10.6V5.4z"));
    button.append(iconWrap, document.createTextNode(module.label || module.id));
    return button;
  }));
  if (state.tab.startsWith("module:") && !modules.some((module) => `module:${module.id}` === state.tab)) setTab("logs");
}

function selectDevice(id) {
  if (state.selectedId === id) return;
  state.selectedId = id;
  state.consoleEntries = [];
  state.consoleSelected = null;
  logs.setDevice(id);
  elements.consoleCatalog.innerHTML = '<div class="inline-empty">Refresh to query this build.</div>';
  resetFileNavigation("saved");
  elements.fileList.replaceChildren();
  clearTimeout(state.moduleTimer);
  state.moduleSchema = null;
  state.modulePageId = null;
  state.moduleValues = {};
  renderDevices();
  renderSelectedDevice();
  if (state.tab === "files") refreshFiles();
  if (state.tab === "console") refreshConsoleCatalog();
  if (state.tab.startsWith("module:")) refreshModule();
}

// Builds below this only enumerate IConsoleManager, so the source filter and description search have nothing to act on.
const CONSOLE_CATALOG_PROTOCOL = 8;
// Builds below this cannot serve the name index, so every filter change has to go back to the device.
const CONSOLE_INDEX_PROTOCOL = 9;
const CONSOLE_PAGE_SIZE = 800;

const CONSOLE_SOURCE_LABELS = {
  cvar: "CVar registry",
  exec: "Exec function",
  stat: "Stat command",
  show: "Show flag",
  manual: "Declared in config",
};

function consoleSourceOf(entry) {
  return entry.source || "cvar";
}

function supportsConsoleCatalogSources() {
  return Number(selectedDevice()?.protocol_version || 0) >= CONSOLE_CATALOG_PROTOCOL;
}

function supportsConsoleIndex() {
  return Number(selectedDevice()?.protocol_version || 0) >= CONSOLE_INDEX_PROTOCOL;
}

// Mirrors RankConsoleMatch on the device so local and server-side results are ordered the same way.
function rankConsoleEntry(entry, lowerQuery) {
  if (!lowerQuery) return 3;
  const name = entry.lowerName;
  if (name === lowerQuery) return 0;
  if (name.startsWith(lowerQuery)) return 1;
  if (name.includes(lowerQuery)) return 2;
  return -1;
}

function searchConsoleIndex(query, { source = "", kind = "", limit = CONSOLE_PAGE_SIZE } = {}) {
  const lowerQuery = query.trim().toLowerCase();
  const matches = [];
  for (const entry of state.consoleIndex) {
    if (source && consoleSourceOf(entry) !== source) continue;
    if (kind && entry.type !== kind) continue;
    const rank = rankConsoleEntry(entry, lowerQuery);
    if (rank >= 0) matches.push({ entry, rank });
  }
  // The index arrives name-sorted, so a stable sort by rank keeps names ordered inside each rank.
  matches.sort((left, right) => left.rank - right.rank);
  return { entries: matches.slice(0, limit).map((match) => match.entry), total: matches.length };
}

async function ensureConsoleIndex(forceRebuild = false) {
  if (!supportsConsoleIndex()) return false;
  if (state.consoleIndexDeviceId === state.selectedId && !forceRebuild) return true;
  const params = new URLSearchParams({ index: "1" });
  if (forceRebuild) params.set("refresh", "1");
  const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/console-objects?${params}`);
  state.consoleIndex = (result.entries || []).map((entry) => ({ ...entry, lowerName: entry.name.toLowerCase() }));
  state.consoleIndexDeviceId = state.selectedId;
  return true;
}

// Only the CVar registry publishes variables, so pairing Variables with Exec or Stat can never match anything.
function syncConsoleFilterAvailability() {
  const indexed = state.consoleIndexDeviceId === state.selectedId && state.consoleIndex.length > 0;
  const hasAny = (source, kind) => !indexed || state.consoleIndex.some((entry) =>
    (!source || consoleSourceOf(entry) === source) && (!kind || entry.type === kind));
  for (const button of elements.consoleSourceFilter.querySelectorAll("button")) {
    button.disabled = !hasAny(button.dataset.source, state.consoleType);
  }
  for (const button of $("#console-type").querySelectorAll("button")) {
    button.disabled = !hasAny(state.consoleSource, button.dataset.type);
  }
}

async function refreshConsoleCatalog(forceRebuild = false) {
  if (!requireOnline()) return;
  const extended = supportsConsoleCatalogSources();
  elements.consoleSourceFilter.classList.toggle("hidden", !extended);
  elements.consoleScopeField.classList.toggle("hidden", !extended);
  const query = elements.consoleSearch.value.trim();
  // Description search needs help text, which the index deliberately omits, so it always asks the device.
  const searchHelp = extended && elements.consoleScope.checked;
  const indexReady = state.consoleIndexDeviceId === state.selectedId && forceRebuild !== true;
  if (searchHelp || !indexReady) {
    elements.consoleCatalog.innerHTML = '<div class="inline-empty">Querying runtime console…</div>';
    elements.consoleCount.textContent = "Loading catalog";
  }
  const local = !searchHelp && (await ensureConsoleIndex(forceRebuild === true).catch(() => false));
  syncConsoleFilterAvailability();
  try {
    let entries;
    let total;
    let catalogTotal;
    if (local) {
      const found = searchConsoleIndex(query, { source: state.consoleSource, kind: state.consoleType });
      entries = found.entries;
      total = found.total;
      catalogTotal = state.consoleIndex.length;
    } else {
      const params = new URLSearchParams({ q: query, limit: String(CONSOLE_PAGE_SIZE) });
      if (state.consoleType) params.set("kind", state.consoleType);
      if (extended) {
        if (state.consoleSource) params.set("source", state.consoleSource);
        if (searchHelp) params.set("scope", "all");
        if (forceRebuild === true) params.set("refresh", "1");
      }
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/console-objects?${params}`);
      entries = result.entries || [];
      total = Number(result.total || entries.length);
      catalogTotal = Number(result.catalog_total || 0);
    }
    state.consoleEntries = entries;
    state.consoleTotal = total;
    state.consoleCatalogTotal = catalogTotal;
    const matchLabel = query ? `${total.toLocaleString()} matching` : `${total.toLocaleString()} supported`;
    const catalogLabel = query && catalogTotal ? ` of ${catalogTotal.toLocaleString()}` : "";
    elements.consoleCount.textContent =
      `${matchLabel}${catalogLabel}${total > entries.length ? ` · showing ${entries.length}` : ""}`;
    renderConsoleCatalog();
  } catch (error) {
    elements.consoleCount.textContent = "Catalog unavailable";
    elements.consoleCatalog.replaceChildren(errorState("Cannot read console catalog", error.message));
  }
}

function renderConsoleCatalog() {
  const entries = state.consoleEntries;
  elements.consoleCatalog.replaceChildren();
  for (const entry of entries) {
    const button = document.createElement("button");
    button.className = `catalog-row ${state.consoleSelected?.name === entry.name ? "active" : ""}`;
    button.type = "button";
    const copy = document.createElement("span");
    copy.append(textCell("catalog-name", entry.name));
    if (entry.type === "variable") copy.append(textCell("catalog-value", entry.value || "(empty)"));
    else if (entry.arguments) copy.append(textCell("catalog-value", entry.arguments));
    const source = consoleSourceOf(entry);
    const label = source === "cvar" ? (entry.type === "variable" ? "CVar" : "Cmd") : source;
    button.append(copy, textCell(`catalog-type ${source}`, label));
    button.addEventListener("click", () => selectConsoleEntry(entry));
    elements.consoleCatalog.append(button);
  }
  if (!entries.length) elements.consoleCatalog.append(textCell("inline-empty", "No matching console objects were reported by the build."));
}

function selectConsoleEntry(entry) {
  state.consoleSelected = entry;
  elements.commandInput.value = entry.name;
  renderConsoleDetail(entry, entry.help === undefined);
  renderConsoleCatalog();
  elements.commandInput.focus();
  elements.commandInput.setSelectionRange(elements.commandInput.value.length, elements.commandInput.value.length);
  // The index carries no help text, so the selected entry is the one place worth a round trip.
  if (entry.help === undefined) loadConsoleEntryDetail(entry);
}

function refreshSelectedConsoleEntry() {
  const entry = state.consoleSelected;
  if (entry) loadConsoleEntryDetail(entry).then(renderConsoleCatalog);
}

async function loadConsoleEntryDetail(entry) {
  try {
    const params = new URLSearchParams({ q: entry.name, limit: "1" });
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/console-objects?${params}`);
    const detail = (result.entries || []).find((item) => item.name === entry.name);
    if (!detail) {
      entry.help = "";
      return;
    }
    Object.assign(entry, detail);
    if (state.consoleSelected?.name === entry.name) renderConsoleDetail(entry, false);
  } catch {
    entry.help = "";
  }
}

function renderConsoleDetail(entry, pendingHelp) {
  const flags = [entry.read_only && "read-only", entry.cheat && "cheat"].filter(Boolean);
  elements.consoleDetail.replaceChildren();
  const kind = entry.type === "variable" ? "CONSOLE VARIABLE" : "CONSOLE COMMAND";
  const badge = textCell("kind-badge", `${kind} · ${CONSOLE_SOURCE_LABELS[consoleSourceOf(entry)] || "Unknown source"}`);
  const title = document.createElement("h3");
  title.textContent = entry.name;
  if (entry.arguments) title.append(Object.assign(document.createElement("small"), { textContent: ` ${entry.arguments}` }));
  const help = document.createElement("p");
  if (pendingHelp) help.classList.add("pending");
  help.textContent = pendingHelp ? "Reading help text…" : entry.help || "No help text was registered.";
  elements.consoleDetail.append(badge, title, help);
  if (entry.type === "variable" || flags.length) {
    const line = document.createElement("div");
    line.className = "value-line";
    if (entry.type === "variable") {
      line.append(document.createTextNode("Current value"), Object.assign(document.createElement("code"), { textContent: entry.value || "(empty)" }));
    }
    if (flags.length) line.append(document.createTextNode(flags.join(" · ")));
    elements.consoleDetail.append(line);
  }
  if (entry.companion) elements.consoleDetail.append(showFlagControl(entry));
}

// A show flag is reachable two ways: "show X" toggles it in the game viewport, while its ShowFlag.X companion is a
// tri-state override (0 off, 1 on, 2 no override) that also applies in the Editor.
const SHOW_FLAG_STATES = [["0", "Off"], ["1", "On"], ["2", "Default"]];

function showFlagControl(entry) {
  const block = document.createElement("div");
  const wrap = document.createElement("div");
  wrap.className = "value-line";
  wrap.append(document.createTextNode("Override"));
  const group = document.createElement("div");
  group.className = "segmented";
  for (const [value, label] of SHOW_FLAG_STATES) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = label;
    button.classList.toggle("active", (entry.value || "2") === value);
    button.addEventListener("click", async () => {
      await executeCommand(`${entry.companion} ${value}`);
      refreshSelectedConsoleEntry();
    });
    group.append(button);
  }
  wrap.append(group);
  const toggle = document.createElement("button");
  toggle.type = "button";
  toggle.className = "text-button";
  toggle.textContent = "Toggle in viewport";
  toggle.addEventListener("click", () => executeCommand(entry.name));
  wrap.append(toggle);
  const note = textCell("detail-note",
    `${entry.name} only reaches a game viewport, so it reports "not handled" in an Editor build that is not playing. `
    + `${entry.companion} applies everywhere.`);
  block.append(wrap, note);
  return block;
}

async function executeCommand(command, commandId = "") {
  command = command.trim();
  if (!command || !requireOnline()) return;
  pushCommandHistory(command);
  appendCommandOutput(`> ${command}\n`, "command");
  const started = performance.now();
  try {
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/command`, {
      method: "POST",
      body: JSON.stringify({ command, command_id: commandId }),
    });
    const elapsed = Math.round(performance.now() - started);
    // Most commands answer through UE_LOG, not the output device they are handed; the device reports both.
    const direct = result.output || "";
    const logged = result.log_output || "";
    const body = direct || logged ? `${direct}${direct && logged ? "\n" : ""}${logged}` : "(no output)";
    appendCommandOutput(`${result.success ? "" : "[not handled] "}${body}\n[${elapsed} ms]\n\n`);
    // A show flag is also set through its companion variable, so watch both names.
    const touched = [state.consoleSelected?.name, state.consoleSelected?.companion].filter(Boolean);
    if (touched.some((name) => command.toLowerCase().startsWith(name.toLowerCase()))) {
      clearTimeout(state.consoleQueryTimer);
      state.consoleQueryTimer = setTimeout(refreshSelectedConsoleEntry, 250);
    }
  } catch (error) {
    appendCommandOutput(`[error] ${error.message}\n\n`);
  }
}

function appendCommandOutput(text) {
  const wasInitial = elements.commandOutput.querySelector(".terminal-muted");
  if (wasInitial) elements.commandOutput.textContent = "";
  elements.commandOutput.append(document.createTextNode(text));
  elements.commandOutput.scrollTop = elements.commandOutput.scrollHeight;
}

function pushCommandHistory(command) {
  state.commandHistory = state.commandHistory.filter((item) => item !== command);
  state.commandHistory.unshift(command);
  state.commandHistory = state.commandHistory.slice(0, 100);
  state.commandHistoryIndex = -1;
  localStorage.setItem("deviceexplorer.commandHistory", JSON.stringify(state.commandHistory));
}

function navigateCommandHistory(delta) {
  if (!state.commandHistory.length) return;
  state.commandHistoryIndex = Math.max(-1, Math.min(state.commandHistory.length - 1, state.commandHistoryIndex + delta));
  elements.commandInput.value = state.commandHistoryIndex < 0 ? "" : state.commandHistory[state.commandHistoryIndex];
  queueMicrotask(() => elements.commandInput.setSelectionRange(elements.commandInput.value.length, elements.commandInput.value.length));
}

function closeCompletions() {
  state.completions = [];
  state.completionIndex = -1;
  state.completionPinned = false;
  elements.completions.replaceChildren();
  elements.completions.classList.add("hidden");
}

function showPinnedHint(entry) {
  state.completions = [entry];
  state.completionIndex = -1;
  state.completionPinned = true;
  renderCompletions();
}

async function requestCompletions() {
  const prefix = elements.commandInput.value;
  if (!prefix.trim() || !state.selectedId || !selectedDevice()?.connected) {
    state.commandHint = null;
    return closeCompletions();
  }
  try {
    // Names can contain spaces ("stat Unit"), so the whole input is the needle.
    let entries;
    if (await ensureConsoleIndex().catch(() => false)) {
      entries = searchConsoleIndex(prefix, { limit: 8 }).entries;
    } else {
      const params = new URLSearchParams({ q: prefix.trim(), limit: "8" });
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/console-objects?${params}`);
      entries = result.entries || [];
    }
    if (elements.commandInput.value !== prefix) return;
    const exact = entries.find((entry) => entry.name.toLowerCase() === prefix.trim().toLowerCase());
    if (exact) state.commandHint = exact;
    if (entries.length) {
      state.completions = entries;
      state.completionIndex = -1;
      state.completionPinned = false;
      renderCompletions();
      return;
    }
    // Only once the search is dry: names like "stat Unit" are catalog entries, not arguments to "stat".
    const pinned = state.commandHint;
    if (pinned && takesArgument(pinned) && prefix.toLowerCase().startsWith(`${pinned.name.toLowerCase()} `)) {
      showPinnedHint(pinned);
      return;
    }
    closeCompletions();
  } catch {
    closeCompletions();
  }
}

function takesArgument(entry) {
  return Boolean(entry.arguments) || entry.type === "variable";
}

function renderCompletions() {
  elements.completions.replaceChildren();
  elements.completions.classList.toggle("hidden", !state.completions.length);
  state.completions.forEach((entry, index) => {
    const option = document.createElement(state.completionPinned ? "div" : "button");
    if (!state.completionPinned) option.type = "button";
    option.className = `completion ${state.completionPinned ? "pinned" : ""} ${index === state.completionIndex ? "active" : ""}`;
    const label = document.createElement("span");
    label.className = "completion-name";
    label.textContent = entry.name;
    if (entry.arguments) label.append(Object.assign(document.createElement("em"), { textContent: ` ${entry.arguments}` }));
    else if (entry.type === "variable") label.append(Object.assign(document.createElement("em"), { textContent: " <value>" }));
    option.append(label, textCell("completion-hint", entry.type === "variable" ? entry.value || "(empty)" : consoleSourceOf(entry)));
    if (!state.completionPinned) {
      option.addEventListener("mousedown", (event) => {
        event.preventDefault();
        acceptCompletion(index);
      });
    }
    elements.completions.append(option);
  });
}

function moveCompletion(delta) {
  // Slots run -1 (nothing selected) through count-1, so shift by one to make the wrap-around arithmetic simple.
  const slots = state.completions.length + 1;
  state.completionIndex = ((state.completionIndex + 1 + delta) % slots + slots) % slots - 1;
  renderCompletions();
  const active = elements.completions.querySelector(".completion.active");
  if (active) active.scrollIntoView({ block: "nearest" });
}

function acceptCompletion(index = state.completionIndex) {
  const entry = state.completions[index];
  if (!entry) return;
  state.commandHint = entry;
  // A command that takes arguments is left open for typing, with its signature still pinned above the prompt.
  const open = takesArgument(entry);
  elements.commandInput.value = open ? `${entry.name} ` : entry.name;
  if (open) showPinnedHint(entry);
  else closeCompletions();
  elements.commandInput.focus();
  elements.commandInput.setSelectionRange(elements.commandInput.value.length, elements.commandInput.value.length);
}

function resetFileNavigation(root) {
  state.fileRoot = root;
  state.filePath = "";
  state.fileHistory = [""];
  state.fileHistoryIndex = 0;
  renderBreadcrumbs();
}

function navigateFiles(path, addHistory = true) {
  path = normalizePath(path);
  if (addHistory && path !== state.filePath) {
    state.fileHistory = state.fileHistory.slice(0, state.fileHistoryIndex + 1);
    state.fileHistory.push(path);
    state.fileHistoryIndex = state.fileHistory.length - 1;
  }
  state.filePath = path;
  refreshFiles();
}

async function refreshFiles() {
  renderBreadcrumbs();
  updateNavigationButtons();
  if (!requireOnline()) {
    elements.fileList.replaceChildren(errorState("Device is offline", "Reconnect the build to browse its files."));
    elements.fileStatus.textContent = "Offline";
    return;
  }
  elements.fileStatus.textContent = "Loading…";
  elements.fileList.innerHTML = '<div class="inline-empty">Reading directory…</div>';
  const params = new URLSearchParams({ root: state.fileRoot, path: state.filePath });
  try {
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/files?${params}`);
    if (result.error) throw new Error(result.error);
    state.fileEntries = result.entries || [];
    elements.fileStatus.textContent = `${state.fileEntries.length} items`;
    renderFiles();
  } catch (error) {
    elements.fileStatus.textContent = "Unavailable";
    elements.fileList.replaceChildren(errorState(
      error.message.includes("timed out") ? "Build did not answer" : "Cannot read this folder",
      error.message.includes("timed out")
        ? "The connection is listed, but its runtime client is not responding. Relaunch the build and try again."
        : error.message,
      refreshFiles,
    ));
  }
}

function renderFiles() {
  const query = elements.fileSearch.value.trim().toLowerCase();
  const sort = elements.fileSort.value;
  const entries = state.fileEntries
    .filter((entry) => !query || String(entry.name).toLowerCase().includes(query))
    .sort((left, right) => {
      const directoryOrder = Number(right.is_directory) - Number(left.is_directory);
      if (directoryOrder) return directoryOrder;
      if (sort === "modified") return new Date(right.modified) - new Date(left.modified);
      if (sort === "size") return Number(right.size || 0) - Number(left.size || 0);
      return left.name.localeCompare(right.name, undefined, { numeric: true, sensitivity: "base" });
    });

  elements.fileList.replaceChildren();
  for (const entry of entries) {
    const row = document.createElement("div");
    row.className = "file-row";
    const nameWrap = document.createElement("div");
    nameWrap.className = "file-name-wrap";
    const bundle = entry.is_directory && isBundle(entry.name);
    const trace = !entry.is_directory && isTrace(entry.name);
    const kind = entry.is_directory ? (bundle ? "bundle" : "folder") : (trace ? "trace" : "file");
    nameWrap.append(svgIcon(`file-icon ${kind}`, FILE_ICON_PATHS[kind]));
    nameWrap.append(textCell(`file-name ${entry.is_directory ? "directory" : ""}`, entry.name));
    if (bundle || trace) nameWrap.append(textCell("file-tag", bundle ? "BUNDLE" : "TRACE"));
    if (entry.is_directory) {
      nameWrap.tabIndex = 0;
      nameWrap.addEventListener("click", () => navigateFiles(entry.path));
      nameWrap.addEventListener("keydown", (event) => {
        if (event.key === "Enter") navigateFiles(entry.path);
      });
    }

    const action = document.createElement("span");
    action.className = "file-action";
    const button = document.createElement("button");
    button.className = "button ghost";
    button.textContent = entry.is_directory ? "Download .zip" : "Download";
    button.addEventListener("click", () => requestTransfer(entry.path, entry.is_directory));
    action.append(button);
    row.append(
      nameWrap,
      textCell("file-size", entry.is_directory ? "—" : formatBytes(entry.size)),
      textCell("file-modified", formatDate(entry.modified)),
      action,
    );
    elements.fileList.append(row);
  }
  if (!entries.length) {
    elements.fileList.append(textCell("inline-empty", query ? "No files match this filter." : "This directory is empty."));
  }
  elements.fileStatus.textContent = query ? `${entries.length} of ${state.fileEntries.length} items` : `${entries.length} items`;
}

function renderBreadcrumbs() {
  const rootLabel = elements.fileRoot.selectedOptions[0]?.textContent || state.fileRoot;
  const parts = state.filePath.split("/").filter(Boolean);
  elements.breadcrumbs.replaceChildren();
  const rootButton = document.createElement("button");
  rootButton.textContent = rootLabel;
  rootButton.addEventListener("click", () => navigateFiles(""));
  elements.breadcrumbs.append(rootButton);
  let path = "";
  for (const part of parts) {
    elements.breadcrumbs.append(textCell("breadcrumb-separator", "›"));
    path = path ? `${path}/${part}` : part;
    const target = path;
    const button = document.createElement("button");
    button.textContent = part;
    button.addEventListener("click", () => navigateFiles(target));
    elements.breadcrumbs.append(button);
  }
}

function updateNavigationButtons() {
  elements.fileBack.disabled = state.fileHistoryIndex <= 0;
  elements.fileForward.disabled = state.fileHistoryIndex >= state.fileHistory.length - 1;
  $("#file-up").disabled = !state.filePath;
}

function setTransferBanner({ stage, text, showBar = false, indeterminate = false, percent = 0, actions = [] }) {
  clearTimeout(state.transferPoll);
  elements.transfer.className = `transfer-banner ${stage === "failed" ? "failed" : ""} ${stage === "ready" ? "ready" : ""}`.trim();
  elements.transfer.classList.remove("hidden");
  elements.transfer.replaceChildren();
  elements.transfer.append(textCell("transfer-dot", ""));

  const body = document.createElement("span");
  body.className = "transfer-body";
  body.append(textCell("transfer-text", text));
  if (showBar) {
    const bar = document.createElement("span");
    bar.className = `transfer-bar ${indeterminate ? "indeterminate" : ""}`.trim();
    const fill = document.createElement("i");
    if (!indeterminate) fill.style.width = `${Math.min(100, Math.max(0, percent))}%`;
    bar.append(fill);
    body.append(bar);
  }
  elements.transfer.append(body);

  if (actions.length) {
    const actionsWrap = document.createElement("span");
    actionsWrap.className = "transfer-actions";
    actionsWrap.append(...actions);
    elements.transfer.append(actionsWrap);
  }

  const dismiss = document.createElement("button");
  dismiss.className = "transfer-dismiss";
  dismiss.title = "Dismiss";
  dismiss.append(svgIcon("", "m4 4 8 8M12 4l-8 8", 12));
  dismiss.addEventListener("click", () => {
    clearTimeout(state.transferPoll);
    elements.transfer.classList.add("hidden");
  });
  elements.transfer.append(dismiss);
}

async function requestTransfer(path, archive = false) {
  if (!requireOnline()) return;
  state.lastTransferRequest = { path, archive };
  setTransferBanner({
    stage: "archiving",
    text: archive ? `Archiving ${path || state.fileRoot}…` : `Requesting ${path}…`,
    showBar: true,
    indeterminate: true,
  });
  try {
    const transfer = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/transfers`, {
      method: "POST",
      body: JSON.stringify({ root: state.fileRoot, path, archive }),
    });
    pollTransfer(transfer.id);
  } catch (error) {
    setTransferBanner({ stage: "failed", text: `Transfer failed: ${error.message}` });
  }
}

function retryButton() {
  const button = document.createElement("button");
  button.className = "secondary";
  button.type = "button";
  button.textContent = "Try again";
  button.addEventListener("click", () => {
    if (state.lastTransferRequest) requestTransfer(state.lastTransferRequest.path, state.lastTransferRequest.archive);
  });
  return button;
}

function traceActionButton(id) {
  const button = document.createElement("button");
  button.className = "secondary";
  button.type = "button";
  button.textContent = "Open in Insights";
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      await api(`/api/transfers/${encodeURIComponent(id)}/trace`, { method: "POST", body: "{}" });
      button.textContent = "Sent to Insights";
    } catch (error) {
      button.disabled = false;
      button.textContent = "Retry Insights";
      toast(error.message, true);
    }
  });
  return button;
}

function pollTransfer(id) {
  clearTimeout(state.transferPoll);
  const poll = async () => {
    try {
      const transfer = await api(`/api/transfers/${encodeURIComponent(id)}`);
      if (transfer.state === "ready") {
        const downloadLink = document.createElement("a");
        downloadLink.className = "primary";
        downloadLink.href = `/api/transfers/${encodeURIComponent(id)}/download`;
        downloadLink.textContent = "Download";
        const actions = [downloadLink];
        if (transfer.filename.toLowerCase().endsWith(".utrace")) actions.push(traceActionButton(id));
        setTransferBanner({ stage: "ready", text: `${transfer.filename} · ready · ${formatBytes(transfer.bytes)}`, actions });
        return;
      }
      if (transfer.state === "failed") {
        setTransferBanner({ stage: "failed", text: `${transfer.filename}: transfer failed · ${transfer.error}`, actions: [retryButton()] });
        return;
      }
      setTransferBanner({
        stage: "transferring",
        text: `${transfer.filename} · transferring · ${formatBytes(transfer.bytes)}`,
        showBar: true,
        indeterminate: true,
      });
      state.transferPoll = setTimeout(poll, 500);
    } catch (error) {
      setTransferBanner({ stage: "failed", text: `Transfer status failed: ${error.message}`, actions: [retryButton()] });
    }
  };
  poll();
}

const SET_FIELDS_ACTION = "__set";

async function refreshModule() {
  clearTimeout(state.moduleTimer);
  const serial = ++state.moduleRequestSerial;
  const device = requireOnline();
  if (!device || !state.tab.startsWith("module:")) return;
  const moduleId = state.tab.slice("module:".length);
  const module = (device.data_modules || []).find((item) => item.id === moduleId);
  if (!module) return;

  renderModuleIdentity(module);
  elements.moduleDescription.textContent = module.description || "Runtime diagnostics.";
  if (elements.moduleActions.dataset.moduleId !== module.id) {
    // Clear immediately on module switch so a slow/failed fetch can't leave the previous module's controls clickable.
    elements.moduleActions.dataset.moduleId = module.id;
    elements.moduleActions.dataset.signature = "";
    elements.moduleActions.replaceChildren();
  }
  if (!elements.moduleContent.childElementCount) {
    elements.moduleContent.innerHTML = '<div class="module-card"><div class="inline-empty">Loading module data…</div></div>';
  }
  try {
    const params = new URLSearchParams({ module: moduleId });
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-data?${params}`);
    if (serial !== state.moduleRequestSerial || state.tab !== `module:${moduleId}`) return;
    if (!result.success) throw new Error(result.error || "Module provider failed");
    state.moduleData = result.data || {};
    renderModuleActionsIfChanged(module);
    const pages = modulePages(module);
    if (pages.length) {
      if (!pages.some((page) => page.id === state.modulePageId)) state.modulePageId = pages[0].id;
      renderModulePageNavigation(module, pages);
      renderModuleSchemaIfChanged(module, pages.find((page) => page.id === state.modulePageId));
      applyModuleValues(module, result.values || {});
    } else {
      state.moduleSchema = null;
      elements.modulePages.classList.add("hidden");
      renderModuleData(result.data || {});
    }
  } catch (error) {
    state.moduleSchema = null;
    elements.moduleContent.replaceChildren(errorState("Module data unavailable", error.message, refreshModule));
  }
  const interval = Math.max(0, Number(module.refresh_interval_ms || 0));
  if (interval && state.tab === `module:${moduleId}`) state.moduleTimer = setTimeout(refreshModule, Math.max(500, interval));
}

function modulePages(module) {
  if (module.pages?.length) return module.pages;
  if (module.layout?.length) return [{ id: "overview", label: "Overview", sections: module.layout }];
  return [];
}

function renderModulePageNavigation(module, pages) {
  const activePage = pages.find((page) => page.id === state.modulePageId) || pages[0];
  elements.moduleDescription.textContent = activePage?.description || module.description || "Runtime diagnostics.";
  elements.modulePages.classList.toggle("hidden", pages.length < 2);
  const signature = `${module.id}:${pages.map((page) => `${page.id}:${page.label}:${page.icon}:${page.description}`).join("|")}`;
  if (elements.modulePages.dataset.signature !== signature) {
    elements.modulePages.dataset.signature = signature;
    elements.modulePages.replaceChildren(...pages.map((page) => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.page = page.id;
      button.title = page.description || page.label || page.id;
      if (page.icon) {
        const icon = document.createElement("span");
        icon.className = "tab-icon";
        icon.append(svgIcon("", page.icon === "pulse" ? "M1.6 8.4h2.9l2-4.2 2.7 7.6 1.9-3.4h3.3" : "M8 2.2 13.8 5.4v5.2L8 13.8 2.2 10.6V5.4z"));
        button.append(icon);
      }
      button.append(document.createTextNode(page.label || page.id));
      button.addEventListener("click", () => {
        if (state.modulePageId === page.id) return;
        state.modulePageId = page.id;
        state.moduleSchema = null;
        renderModulePageNavigation(module, pages);
        renderModuleSchemaIfChanged(module, page);
        applyModuleValues(module, state.moduleValues || {});
      });
      return button;
    }));
  }
  elements.modulePages.querySelectorAll("button").forEach((button) => button.classList.toggle("active", button.dataset.page === state.modulePageId));
}

function renderModuleIdentity(module) {
  elements.moduleTitle.textContent = module.label || module.id;
  const id = textCell("module-id", module.id);
  elements.moduleTitle.append(id);

  const heading = elements.moduleTitle.closest(".panel-heading");
  if (!heading) return;
  let chip = heading.querySelector(".module-live-chip");
  if (!chip) {
    chip = document.createElement("span");
    chip.className = "module-live-chip";
    heading.insertBefore(chip, elements.moduleActions);
  }
  const interval = Number(module.refresh_interval_ms || 0);
  const period = interval >= 1000 ? `${Number((interval / 1000).toFixed(1))} s` : `${interval} ms`;
  chip.replaceChildren(document.createElement("i"), document.createTextNode(interval ? `Auto-refresh ${period}` : "Manual refresh"));
  chip.title = interval
    ? `Values on this page are re-read from the device every ${interval} ms.`
    : "Values on this page are read once, when the page opens.";
  chip.classList.toggle("static", !interval);
}

function renderModuleActionsIfChanged(module) {
  const signature = `${module.id}::` + (module.actions || []).map((action) => `${action.id}:${(action.inputs || []).map((input) => input.id).join(",")}`).join("|");
  if (elements.moduleActions.dataset.signature === signature) return;
  elements.moduleActions.dataset.signature = signature;
  renderModuleActions(module);
}

function renderModuleActions(module) {
  const actions = (module.actions || []).filter((action) => !action.hidden && !String(action.id || "").startsWith("__"));
  elements.moduleActions.replaceChildren(...actions.map((action) => renderModuleAction(module, action)));
}

function renderModuleAction(module, action) {
  const inputs = action.inputs || [];
  if (inputs.length === 1 && inputs[0].type === "bool") {
    return renderModuleToggle(module, action, inputs[0]);
  }
  if (inputs.length > 0) {
    return renderModuleActionForm(module, action, inputs);
  }
  return renderModuleActionButton(module, action, () => ({}));
}

function renderModuleToggle(module, action, input) {
  const wrap = document.createElement("label");
  wrap.className = "module-toggle";
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.checked = Boolean(state.moduleData?.Settings?.[input.id]);
  const text = document.createElement("span");
  text.textContent = action.label || action.id;
  wrap.append(checkbox, text);
  checkbox.addEventListener("change", async () => {
    checkbox.disabled = true;
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
        method: "POST",
        body: JSON.stringify({ module: module.id, action: action.id, parameters: { [input.id]: checkbox.checked } }),
      });
      if (!result.success) throw new Error(result.error || "Action failed");
      toast(`${action.label || action.id} updated`);
      if (result.data) { state.moduleData = result.data; renderModuleData(result.data); }
    } catch (error) {
      checkbox.checked = !checkbox.checked;
      toast(error.message, true);
    } finally {
      checkbox.disabled = false;
    }
  });
  return wrap;
}

function renderModuleActionForm(module, action, inputs) {
  const form = document.createElement("form");
  form.className = "module-action-form";
  const title = document.createElement("span");
  title.className = "module-action-form-title";
  title.textContent = action.label || action.id;
  form.append(title);

  const fields = inputs.map((input) => {
    const field = document.createElement("label");
    field.className = "module-action-field";
    const text = document.createElement("span");
    text.textContent = input.label || input.id;
    const numberInput = document.createElement("input");
    numberInput.type = "number";
    numberInput.step = "any";
    attachNumberScrub(numberInput, input);
    const currentValue = state.moduleData?.Settings?.[input.id];
    if (currentValue !== undefined) numberInput.value = currentValue;
    field.append(text, numberInput);
    form.append(field);
    return { id: input.id, element: numberInput };
  });

  const submit = document.createElement("button");
  submit.type = "submit";
  submit.className = "button primary";
  submit.textContent = action.label || "Apply";
  form.append(submit);

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    submit.disabled = true;
    try {
      const parameters = Object.fromEntries(fields.map(({ id, element }) => [id, Number(element.value)]));
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
        method: "POST",
        body: JSON.stringify({ module: module.id, action: action.id, parameters }),
      });
      if (!result.success) throw new Error(result.error || "Action failed");
      toast(`${action.label || action.id} applied`);
      if (result.data) { state.moduleData = result.data; renderModuleData(result.data); }
    } catch (error) {
      toast(error.message, true);
    } finally {
      submit.disabled = false;
    }
  });

  return form;
}

function renderModuleActionButton(module, action, buildParameters) {
  const button = document.createElement("button");
  button.className = "button ghost";
  button.textContent = action.label || action.id;
  button.title = action.description || action.id;
  button.addEventListener("click", async () => {
    if (action.requires_confirmation) {
      const accepted = await confirmAction({
        title: `${action.label || action.id}?`,
        body: "This runs an extension action on the connected build. It may briefly stall the game thread.",
        deviceName: selectedDevice()?.name || state.selectedId,
        commandLabel: action.id,
        acceptLabel: action.label || action.id,
      });
      if (!accepted) return;
    }
    button.disabled = true;
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
        method: "POST",
        body: JSON.stringify({ module: module.id, action: action.id, parameters: buildParameters() }),
      });
      if (!result.success) throw new Error(result.error || "Action failed");
      toast(`${action.label || action.id} completed`);
      if (result.data) { state.moduleData = result.data; renderModuleData(result.data); }
      else refreshModule();
    } catch (error) {
      toast(error.message, true);
    } finally {
      button.disabled = false;
    }
  });
  return button;
}

function renderModuleData(data) {
  elements.moduleContent.classList.remove("has-split");
  elements.moduleContent.replaceChildren();
  if (!isPlainObject(data)) {
    const card = document.createElement("section");
    card.className = "module-card raw-card";
    card.append(Object.assign(document.createElement("h3"), { textContent: "Data" }), renderDataValue(data));
    elements.moduleContent.append(card);
    return;
  }
  const entries = Object.entries(data);
  const scalars = entries.filter(([, value]) => !isPlainObject(value) && !Array.isArray(value));
  const groups = entries.filter(([, value]) => isPlainObject(value) || Array.isArray(value));

  const wrap = document.createElement("div");
  wrap.className = "raw-grid";
  if (scalars.length) wrap.append(rawCard("Values", Object.fromEntries(scalars), scalars.length));
  for (const [title, value] of groups) {
    const size = isPlainObject(value) ? Object.keys(value).length : value.length;
    wrap.append(rawCard(title, value, size));
  }
  if (!entries.length) {
    elements.moduleContent.append(textCell("inline-empty", "The module returned no data."));
    return;
  }
  elements.moduleContent.append(wrap);
}

function rawCard(title, value, size) {
  const card = document.createElement("section");
  card.className = "module-card raw-card";
  const heading = document.createElement("h3");
  heading.append(textCell("raw-title", title));
  if (Number.isFinite(size)) heading.append(textCell("raw-count", `${size} ${size === 1 ? "field" : "fields"}`));
  card.append(heading, renderDataValue(value));
  return card;
}

function valueTone(item) {
  if (item === null || item === undefined) return "null";
  if (typeof item === "number") return "number";
  if (typeof item === "boolean") return "bool";
  return "string";
}

function renderDataValue(value) {
  if (Array.isArray(value) && value.length && value.every(isPlainObject)) {
    const columns = [...new Set(value.flatMap((row) => Object.keys(row)))];
    const table = document.createElement("div");
    table.className = "data-table raw-table";
    table.style.setProperty("--table-columns", `1fr repeat(${Math.max(0, columns.length - 1)}, minmax(60px, 110px))`);
    const head = document.createElement("div");
    head.className = "data-row head";
    head.append(...columns.map((column) => textCell("data-cell", column)));
    table.append(head);
    for (const row of value) {
      const line = document.createElement("div");
      line.className = "data-row";
      line.append(...columns.map((column) => textCell(`data-cell tone-${valueTone(row[column])}`, formatModuleValue(row[column]))));
      table.append(line);
    }
    return table;
  }
  if (Array.isArray(value)) {
    const list = document.createElement("div");
    list.className = "raw-chips";
    list.append(...value.map((item) => textCell(`raw-chip tone-${valueTone(item)}`, formatModuleValue(item))));
    if (!value.length) list.append(textCell("raw-empty", "empty"));
    return list;
  }
  if (isPlainObject(value)) {
    const list = document.createElement("dl");
    list.className = "key-value";
    for (const [key, item] of Object.entries(value)) {
      const term = document.createElement("dt");
      term.textContent = key;
      const description = document.createElement("dd");
      if (isPlainObject(item) || Array.isArray(item)) {
        description.className = "nested";
        description.append(renderDataValue(item));
      } else {
        description.className = `tone-${valueTone(item)}`;
        description.textContent = formatModuleValue(item);
      }
      list.append(term, description);
    }
    if (!Object.keys(value).length) list.append(textCell("raw-empty", "empty"));
    return list;
  }
  return buildJsonView(value);
}

function moduleSchemaSignature(module, page) {
  return `${module.id}:${page?.id || "overview"}:${JSON.stringify(page?.sections || [])}`;
}

function renderModuleSchemaIfChanged(module, page) {
  const signature = moduleSchemaSignature(module, page);
  if (state.moduleSchema?.moduleId === module.id && state.moduleSchema.signature === signature) return;
  const updaters = new Map();
  const built = (page?.sections || []).map((section) => ({ section, node: buildModuleSection(module, section, updaters) }));
  const inspector = built.filter(({ section }) => ["toolbar", "notes", "settings"].includes(section.style));
  const main = built.filter((item) => !inspector.includes(item));
  if (inspector.length && main.length) {
    const split = document.createElement("div");
    split.className = "module-split";
    const mainColumn = document.createElement("div");
    mainColumn.className = "module-main";
    mainColumn.append(...main.map((item) => item.node));
    const aside = document.createElement("aside");
    aside.className = "module-inspector";
    aside.append(...inspector.map((item) => item.node));
    split.append(mainColumn, aside);
    elements.moduleContent.classList.add("has-split");
    elements.moduleContent.replaceChildren(split);
  } else {
    elements.moduleContent.classList.remove("has-split");
    elements.moduleContent.replaceChildren(...built.map((item) => item.node));
  }
  state.moduleSchema = { moduleId: module.id, pageId: page?.id, signature, updaters, values: {} };
}

function applyModuleValues(module, values) {
  const schema = state.moduleSchema;
  if (!schema || schema.moduleId !== module.id) return;
  state.moduleValues = { ...(state.moduleValues || {}), ...values };
  Object.assign(schema.values, values);
  for (const [fieldId, updater] of schema.updaters) {
    if (Object.prototype.hasOwnProperty.call(values, fieldId)) updater(values[fieldId]);
  }
}

function revertModuleField(fieldId) {
  const schema = state.moduleSchema;
  if (!schema || !Object.prototype.hasOwnProperty.call(schema.values, fieldId)) return;
  schema.updaters.get(fieldId)?.(schema.values[fieldId]);
}

function buildModuleSection(module, section, updaters) {
  const card = document.createElement("section");
  card.className = `module-card module-section-${section.style || "default"}`;
  const heading = document.createElement("h3");
  const title = document.createElement("span");
  title.textContent = section.label || section.id;
  heading.append(title);
  if (section.description) {
    const description = document.createElement("small");
    description.textContent = section.description;
    heading.append(description);
  }
  card.append(heading);

  if (section.style === "stats") card.classList.add("stats-bare");
  const body = document.createElement("div");
  body.className = "module-card-body";
  if (section.style === "hero") {
    card.classList.add("module-section-hero");
    heading.remove();
    for (const field of section.fields || []) body.append(buildHeroPanel(field, updaters));
    card.append(body);
    return card;
  }
  const grid = document.createElement("div");
  grid.className = "field-grid";
  if (section.columns > 0) {
    grid.style.setProperty("--field-columns", String(section.columns));
    grid.style.setProperty("--field-min", "0");
  }

  const dirty = section.apply === "manual" ? new Map() : null;
  let applyButton = null;
  let discardButton = null;
  let applyStatus = null;
  let submitting = false;
  const updateApplyButton = () => {
    const count = dirty ? dirty.size : 0;
    const invalid = grid.querySelectorAll(".field-row.invalid").length;
    for (const row of grid.querySelectorAll(".field-row[data-field-id]")) {
      const isDirty = Boolean(dirty?.has(row.dataset.fieldId));
      row.classList.toggle("dirty", isDirty);
      const was = row.querySelector(".field-was");
      if (was) {
        const previous = state.moduleSchema?.values?.[row.dataset.fieldId];
        was.textContent = isDirty && previous !== undefined && !isPlainObject(previous) ? `was ${formatModuleValue(previous)}` : "";
      }
    }
    if (applyStatus) {
      const parts = [];
      if (count) parts.push(`${count} unsaved ${count === 1 ? "change" : "changes"}`);
      if (invalid) parts.push(`${invalid} invalid`);
      applyStatus.textContent = parts.join(" \u00b7 ");
      applyStatus.classList.toggle("has-invalid", invalid > 0);
    }
    if (applyButton) applyButton.disabled = count === 0;
    if (discardButton) discardButton.disabled = count === 0;
  };

  for (const field of section.fields || []) {
    grid.append(buildModuleField(module, field, updaters, dirty, updateApplyButton, section));
  }
  body.append(grid);

  if (dirty) {
    const footer = document.createElement("div");
    footer.className = "field-apply-row";
    applyStatus = textCell("field-apply-status", "");
    footer.append(applyStatus);
    discardButton = document.createElement("button");
    discardButton.type = "button";
    discardButton.className = "button ghost";
    discardButton.textContent = "Discard";
    discardButton.disabled = true;
    applyButton = document.createElement("button");
    applyButton.type = "button";
    applyButton.className = "button primary";
    applyButton.textContent = "Apply";
    applyButton.disabled = true;
    applyButton.addEventListener("click", async () => {
      if (submitting) return;
      submitting = true;
      applyButton.disabled = true;
      await submitDirtyFields(module, dirty, updateApplyButton);
      submitting = false;
    });
    discardButton.addEventListener("click", () => {
      const fields = [...dirty.keys()];
      dirty.clear();
      for (const fieldId of fields) revertModuleField(fieldId);
      updateApplyButton();
    });
    footer.append(discardButton, applyButton);
    body.append(footer);
  }
  card.append(body);

  if (section.collapsible) {
    card.classList.add("collapsible");
    card.classList.toggle("collapsed", Boolean(section.collapsed));
    heading.tabIndex = 0;
    heading.addEventListener("click", () => card.classList.toggle("collapsed"));
    heading.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") card.classList.toggle("collapsed");
    });
  }
  return card;
}

function buildSegmentedControl(module, field, updaters, dirty, onDirtyChange) {
  const wrap = document.createElement("div");
  wrap.className = "field-segmented";
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  let current = "";
  const buttons = (field.options || []).map((option) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "segment";
    button.textContent = option;
    button.disabled = Boolean(field.readonly);
    button.addEventListener("click", () => {
      current = option;
      sync();
      queueOrCommit(module, field, option, dirty, onDirtyChange, guard);
    });
    return button;
  });
  const sync = () => buttons.forEach((button, index) => button.classList.toggle("on", (field.options || [])[index] === current));
  wrap.append(...buttons);
  updaters.set(field.id, (value) => {
    if (dirty?.has(field.id) || guard.inFlight > 0) return;
    current = String(value ?? "");
    sync();
  });
  return wrap;
}

function buildTextareaControl(module, field, updaters, dirty, onDirtyChange) {
  const area = document.createElement("textarea");
  area.className = "field-textarea";
  area.rows = field.rows || 3;
  area.disabled = Boolean(field.readonly);
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  updaters.set(field.id, (value) => { if (isFieldBusy(area, dirty, field.id, guard)) return; area.value = String(value ?? ""); });
  if (field.readonly) return area;
  area.addEventListener("change", () => queueOrCommit(module, field, area.value, dirty, onDirtyChange, guard));
  return area;
}

function buildActionGroupControl(module, field) {
  const wrap = document.createElement("div");
  wrap.className = "field-action-group";
  wrap.append(...(field.items || []).map((item) => buildModuleFieldButton(module, { ...item, widget: "button" })));
  return wrap;
}

function buildActionFormControl(module, field) {
  const wrap = document.createElement("div");
  wrap.className = "field-action-form";
  const values = {};
  for (const input of field.inputs || []) {
    values[input.id] = input.default ?? "";
    const cell = document.createElement("label");
    cell.className = "action-input";
    cell.append(textCell("action-input-label", input.label || input.id));
    const control = document.createElement("input");
    control.type = input.type === "number" ? "number" : "text";
    control.value = String(input.default ?? "");
    if (input.type === "number") attachNumberScrub(control, input);
    control.addEventListener("input", () => { values[input.id] = input.type === "number" ? Number(control.value) : control.value; });
    cell.append(control);
    wrap.append(cell);
  }
  const run = document.createElement("button");
  run.type = "button";
  run.className = "button primary";
  run.textContent = field.action_label || "Run";
  run.addEventListener("click", async () => {
    run.disabled = true;
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
        method: "POST",
        body: JSON.stringify({ module: module.id, action: field.action, parameters: { ...values } }),
      });
      if (!result.success) throw new Error(result.error || "Action failed");
      toast(`${field.action_label || field.label} started`);
      if (result.values) applyModuleValues(module, result.values);
    } catch (error) {
      toast(error.message, true);
    } finally {
      run.disabled = false;
    }
  });
  wrap.append(run);
  return wrap;
}

function buildHeroPanel(field, updaters) {
  const card = document.createElement("div");
  card.className = "hero-metric";
  const top = document.createElement("div");
  top.className = "hero-top";
  const left = document.createElement("div");
  left.className = "hero-left";
  left.append(textCell("hero-label", field.label || field.id));
  const readout = document.createElement("div");
  readout.className = "hero-readout";
  const value = textCell("hero-value", "");
  readout.append(value);
  if (field.unit) readout.append(textCell("hero-unit", field.unit));
  const caption = textCell("hero-caption", "");
  readout.append(caption);
  left.append(readout);
  const aggregates = document.createElement("div");
  aggregates.className = "hero-aggregates";
  const stat = (name) => {
    const cell = document.createElement("div");
    cell.className = "hero-stat";
    const node = textCell("hero-stat-value", "\u2014");
    cell.append(textCell("hero-stat-label", name), node);
    aggregates.append(cell);
    return node;
  };
  const avgNode = stat("avg 60s");
  const peakNode = stat("peak");
  top.append(left, aggregates);
  card.append(top);

  const history = [];
  const spark = buildSparkline(field, history);
  spark.svg.classList.add("hero-spark");
  card.append(spark.svg);

  const legend = document.createElement("div");
  legend.className = "hero-legend";
  const legendItem = (className, text) => {
    const item = document.createElement("span");
    item.className = "legend-item";
    const swatch = document.createElement("i");
    swatch.className = className;
    item.append(swatch, document.createTextNode(text));
    return item;
  };
  if (Number.isFinite(field.warn_above)) legend.append(legendItem("warn", `warn ${field.warn_above} ${field.unit || ""}`.trim()));
  if (Number.isFinite(field.error_above)) legend.append(legendItem("error", `error ${field.error_above} ${field.unit || ""}`.trim()));
  legend.append(textCell("legend-note", "live sampling"));
  card.append(legend);

  updaters.set(field.id, (raw) => {
    const numeric = Number(raw);
    if (!Number.isFinite(numeric)) return;
    const level = levelFor(field, numeric);
    card.className = `hero-metric ${level}`;
    value.textContent = formatModuleValue(numeric);
    caption.textContent = level === "error" ? `above error ${field.error_above}`
      : level === "warn" ? `above warn ${field.warn_above}` : "within budget";
    history.push(numeric);
    if (history.length > 60) history.shift();
    spark.redraw();
    avgNode.textContent = formatModuleValue(history.reduce((sum, item) => sum + item, 0) / history.length);
    peakNode.textContent = formatModuleValue(Math.max(...history));
  });
  return card;
}

function isFieldBusy(control, dirty, fieldId, guard) {
  return document.activeElement === control || guard.pendingDebounce || guard.inFlight > 0 || Boolean(dirty?.has(fieldId));
}

async function submitDirtyFields(module, dirty, updateApplyButton) {
  if (!dirty.size) return;
  const parameters = Object.fromEntries(dirty);
  try {
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
      method: "POST",
      body: JSON.stringify({ module: module.id, action: SET_FIELDS_ACTION, parameters }),
    });
    if (!result.success) throw new Error(result.error || "Update failed");
    dirty.clear();
    applyModuleValues(module, result.values || {});
    toast("Changes applied");
  } catch (error) {
    const fieldIds = [...dirty.keys()];
    const match = /^([\w.]+):\s*(.+)$/.exec(error.message || "");
    if (match && fieldIds.includes(match[1])) {
      setFieldError(match[1], match[2]);
    } else {
      dirty.clear();
      for (const fieldId of fieldIds) revertModuleField(fieldId);
      toast(error.message, true);
    }
  } finally {
    updateApplyButton();
  }
}

function setFieldError(fieldId, message) {
  const row = elements.moduleContent.querySelector(`.field-row[data-field-id="${CSS.escape(fieldId)}"]`);
  if (!row) return;
  row.classList.toggle("invalid", Boolean(message));
  let node = row.querySelector(".field-error");
  if (!message) { node?.remove(); return; }
  if (!node) { node = textCell("field-error", ""); row.append(node); }
  node.textContent = message;
}

async function commitModuleField(module, field, value, guard) {
  const serial = ++guard.serial;
  guard.inFlight += 1;
  try {
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
      method: "POST",
      body: JSON.stringify({ module: module.id, action: SET_FIELDS_ACTION, parameters: { [field.id]: value } }),
    });
    if (serial !== guard.serial) return;
    if (!result.success) throw new Error(result.error || "Update failed");
    applyModuleValues(module, result.values || {});
  } catch (error) {
    if (serial === guard.serial) toast(error.message, true);
  } finally {
    guard.inFlight -= 1;
    if (guard.inFlight === 0) revertModuleField(field.id);
  }
}

async function queueOrCommit(module, field, value, dirty, onDirtyChange, guard) {
  if (dirty) {
    setFieldError(field.id, "");
    dirty.set(field.id, value);
    onDirtyChange();
    return;
  }
  await commitModuleField(module, field, value, guard);
}

function buildModuleField(module, field, updaters, dirty, onDirtyChange, section) {
  const reference = section?.style === "reference";
  const row = document.createElement("div");
  row.className = reference ? "field-row field-row-reference" : "field-row";
  row.dataset.fieldId = field.id;
  row.style.setProperty("--field-span", String(field.span || 1));
  if (field.description && !reference) row.title = field.description;

  if (field.widget === "button" && !reference) {
    row.classList.add("field-row-button");
    row.append(buildModuleFieldButton(module, field));
    return row;
  }

  const label = textCell("field-label", "");
  label.append(textCell("field-label-text", field.label || field.id));
  if (field.description && !reference) {
    const help = textCell("field-help", "?");
    help.title = field.description;
    label.append(help);
  }
  const was = dirty && !field.readonly ? textCell("field-was", "") : null;
  if (was && !reference) label.append(was);

  if (reference) {
    const head = document.createElement("div");
    head.className = "reference-head";
    head.append(label);
    if (field.description) head.append(textCell("reference-hint", field.description));
    const control = document.createElement("div");
    control.className = "reference-control";
    control.append(field.widget === "button" ? buildModuleFieldButton(module, field) : buildFieldControl(module, field, updaters, dirty, onDirtyChange));
    // The label sits in a narrow fixed column here, so the previous value belongs with the control.
    if (was) control.append(was);
    row.append(head, control, textCell("reference-api", field.api || ""));
    return row;
  }

  row.append(label, buildFieldControl(module, field, updaters, dirty, onDirtyChange));
  return row;
}

function buildFieldControl(module, field, updaters, dirty, onDirtyChange) {
  switch (field.widget) {
    case "bool": return buildBoolControl(module, field, updaters, dirty, onDirtyChange);
    case "number": return buildNumberControl(module, field, updaters, dirty, onDirtyChange);
    case "enum": return buildEnumControl(module, field, updaters, dirty, onDirtyChange);
    case "string": return buildStringControl(module, field, updaters, dirty, onDirtyChange);
    case "textarea": return buildTextareaControl(module, field, updaters, dirty, onDirtyChange);
    case "actions": return buildActionGroupControl(module, field);
    case "action_form": return buildActionFormControl(module, field);
    case "badge": return buildBadgeControl(field, updaters);
    case "meter": return buildMeterControl(field, updaters);
    case "json": return buildJsonControl(field, updaters);
    case "series": return buildSeriesControl(field, updaters);
    case "status": return buildStatusControl(field, updaters);
    case "table": return buildTableControl(field, updaters);
    case "vector": return buildVectorControl(module, field, updaters, dirty, onDirtyChange);
    case "color": return buildColorControl(module, field, updaters, dirty, onDirtyChange);
    case "path": return buildPathControl(field, updaters);
    case "artifact": return buildArtifactControl(field, updaters);
    case "flags": return buildFlagsControl(module, field, updaters, dirty, onDirtyChange);
    default: return buildTextControl(field, updaters);
  }
}

function buildTextControl(field, updaters) {
  const cell = textCell("field-value", "");
  const updater = (value) => { cell.textContent = field.unit ? `${formatModuleValue(value)} ${field.unit}` : formatModuleValue(value); };
  updaters.set(field.id, updater);
  return cell;
}

function buildBoolControl(module, field, updaters, dirty, onDirtyChange) {
  if (field.readonly) {
    const wrap = document.createElement("span");
    wrap.className = "field-bool-readonly";
    const dot = document.createElement("i");
    const text = textCell("bool-text", "");
    wrap.append(dot, text);
    updaters.set(field.id, (value) => {
      wrap.classList.toggle("on", Boolean(value));
      text.textContent = String(Boolean(value));
    });
    return wrap;
  }
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.className = "field-switch";
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const applyValue = (value) => { checkbox.checked = Boolean(value); };
  updaters.set(field.id, (value) => { if (isFieldBusy(checkbox, dirty, field.id, guard)) return; applyValue(value); });
  checkbox.addEventListener("change", async () => {
    // Read the new state before disabling: disabling a focused checkbox blurs it, and the blur revert would flip it back to the old value first.
    const value = checkbox.checked;
    guard.pendingDebounce = true;
    checkbox.disabled = true;
    try {
      guard.pendingDebounce = false;
      await queueOrCommit(module, field, value, dirty, onDirtyChange, guard);
    } finally {
      checkbox.disabled = false;
    }
  });
  checkbox.addEventListener("blur", () => revertModuleField(field.id));
  return checkbox;
}

function buildNumberControl(module, field, updaters, dirty, onDirtyChange) {
  if (field.readonly) return buildTextControl(field, updaters);

  const hasBounds = Number.isFinite(field.min) && Number.isFinite(field.max);
  const display = field.number_display === "auto" ? (hasBounds ? "slider_input" : "input") : field.number_display;
  const wrap = document.createElement("div");
  wrap.className = "field-number";
  const inputs = [];
  const addInput = (type) => {
    const input = document.createElement("input");
    input.type = type;
    if (Number.isFinite(field.min)) input.min = String(field.min);
    if (Number.isFinite(field.max)) input.max = String(field.max);
    input.step = field.step ? String(field.step) : "any";
    inputs.push(input);
    wrap.append(input);
    return input;
  };
  const slider = hasBounds && (display === "slider" || display === "slider_input") ? addInput("range") : null;
  const number = display === "input" || display === "slider_input" ? addInput("number") : null;
  if (number) attachNumberScrub(number, field);
  const decimals = String(field.step ?? "").split(".")[1]?.length ?? 0;
  const formatReadout = (value) => Number(value).toLocaleString(undefined, {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals || 2,
  });

  const readout = document.createElement("span");
  readout.className = "field-number-readout";
  const readoutValue = textCell("readout-value", "");
  // A number input already shows the value; the readout then only carries the unit.
  if (!number) readout.append(readoutValue);
  if (field.unit) readout.append(textCell("readout-unit", field.unit));
  if (!number || field.unit) wrap.append(readout);
  if (hasBounds) {
    const digits = String(Math.trunc(Math.max(Math.abs(field.min), Math.abs(field.max)))).length;
    readoutValue.style.minWidth = `${digits + (decimals ? decimals + 1 : 0)}ch`;
  }

  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const applyValue = (value) => {
    const numeric = Number(value);
    for (const input of inputs) input.value = String(numeric);
    readoutValue.textContent = formatReadout(numeric);
  };
  const busy = () => inputs.includes(document.activeElement) || guard.pendingDebounce || guard.inFlight > 0 || Boolean(dirty?.has(field.id));
  updaters.set(field.id, (value) => { if (!busy()) applyValue(value); });

  let debounce;
  for (const input of inputs) {
    input.addEventListener("input", () => {
      const value = Number(input.value);
      if (Number.isNaN(value)) return;
      for (const peer of inputs) if (peer !== input) peer.value = String(value);
      readoutValue.textContent = formatReadout(value);
      guard.pendingDebounce = true;
      clearTimeout(debounce);
      debounce = setTimeout(() => {
        guard.pendingDebounce = false;
        queueOrCommit(module, field, value, dirty, onDirtyChange, guard);
      }, 250);
    });
    input.addEventListener("blur", () => {
      if (!inputs.includes(document.activeElement)) revertModuleField(field.id);
    });
  }
  return wrap;
}

// Editor-style numeric entry: dragging the field horizontally spins the value, a click that never
// travelled drops into text entry. Scrubbing writes through the input event so the field's own
// debounce, peer sync and commit path stay the single owner of the value.
function attachNumberScrub(input, bounds = {}) {
  const min = Number.isFinite(bounds.min) ? bounds.min : null;
  const max = Number.isFinite(bounds.max) ? bounds.max : null;
  const step = Number.isFinite(bounds.step) && bounds.step > 0 ? bounds.step : null;
  const origin = min ?? 0;
  input.classList.add("scrub-number");

  const unitFor = (value) => {
    if (step) return step;
    if (min !== null && max !== null) return (max - min) / 250;
    // Unbounded and unstepped: scale the per-pixel increment with the value so 0.5 and 5000 both stay controllable.
    return Math.max(0.001, Math.abs(value) / 100);
  };
  const quantize = (value, unit) => {
    const snapped = step ? origin + Math.round((value - origin) / step) * step : value;
    const decimals = Math.min(6, Math.max(0, Math.ceil(-Math.log10(step ?? unit)) + 1));
    return Number(snapped.toFixed(decimals));
  };
  const clamp = (value) => Math.min(max ?? Infinity, Math.max(min ?? -Infinity, value));

  let scrub = null;
  input.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || event.pointerType === "touch" || input.disabled || input.readOnly) return;
    if (document.activeElement === input) return;
    const start = Number(input.value);
    const value = Number.isFinite(start) ? start : 0;
    scrub = { pointerId: event.pointerId, lastX: event.clientX, travel: 0, dragging: false, value, unit: unitFor(value) };
    input.setPointerCapture(event.pointerId);
    // Preventing pointerdown suppresses the compatibility mousedown, so nothing else blurs either: do it here.
    if (document.activeElement instanceof HTMLElement) document.activeElement.blur();
    event.preventDefault();
  });

  input.addEventListener("pointermove", (event) => {
    if (!scrub || event.pointerId !== scrub.pointerId) return;
    const delta = event.clientX - scrub.lastX;
    scrub.lastX = event.clientX;
    if (!scrub.dragging) {
      scrub.travel += Math.abs(delta);
      if (scrub.travel < 3) return;
      scrub.dragging = true;
      document.body.classList.add("scrubbing");
      input.classList.add("scrubbing");
    }
    const speed = event.shiftKey ? 0.1 : event.ctrlKey || event.metaKey ? 10 : 1;
    scrub.value = clamp(scrub.value + delta * scrub.unit * speed);
    const next = String(quantize(scrub.value, scrub.unit));
    if (next === input.value) return;
    input.value = next;
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });

  const release = (event) => {
    if (!scrub || event.pointerId !== scrub.pointerId) return;
    const dragged = scrub.dragging;
    scrub = null;
    document.body.classList.remove("scrubbing");
    input.classList.remove("scrubbing");
    if (dragged) return;
    input.focus();
    input.select();
  };
  input.addEventListener("pointerup", release);
  input.addEventListener("pointercancel", release);
  // Capture can be lost without a pointerup (window focus stolen mid-drag); do not strand the scrub cursor.
  input.addEventListener("lostpointercapture", release);
}

function buildStringControl(module, field, updaters, dirty, onDirtyChange) {
  if (field.readonly) return buildTextControl(field, updaters);
  const input = document.createElement("input");
  input.type = "text";
  input.className = "field-text-input";
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const applyValue = (value) => { input.value = value ?? ""; };
  updaters.set(field.id, (value) => { if (isFieldBusy(input, dirty, field.id, guard)) return; applyValue(value); });
  let debounce;
  input.addEventListener("input", () => {
    guard.pendingDebounce = true;
    clearTimeout(debounce);
    debounce = setTimeout(() => {
      guard.pendingDebounce = false;
      queueOrCommit(module, field, input.value, dirty, onDirtyChange, guard);
    }, 350);
  });
  input.addEventListener("blur", () => revertModuleField(field.id));
  return input;
}

function buildEnumControl(module, field, updaters, dirty, onDirtyChange) {
  if (field.enum_display === "segmented") return buildSegmentedControl(module, field, updaters, dirty, onDirtyChange);
  const select = document.createElement("select");
  select.className = "field-select";
  select.disabled = Boolean(field.readonly);
  select.replaceChildren(...(field.options || []).map((option) => Object.assign(document.createElement("option"), { value: option, textContent: option })));
  if (field.readonly) {
    updaters.set(field.id, (value) => { select.value = value; });
    return select;
  }
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const applyValue = (value) => { select.value = value; };
  updaters.set(field.id, (value) => { if (isFieldBusy(select, dirty, field.id, guard)) return; applyValue(value); });
  select.addEventListener("change", () => queueOrCommit(module, field, select.value, dirty, onDirtyChange, guard));
  select.addEventListener("blur", () => revertModuleField(field.id));
  return select;
}

function levelFor(field, numeric) {
  if (Number.isFinite(field.error_above) && numeric >= field.error_above) return "error";
  if (Number.isFinite(field.warn_above) && numeric >= field.warn_above) return "warn";
  return "ok";
}

function buildSparkline(field, history) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", "field-spark");
  svg.setAttribute("viewBox", "0 0 220 44");
  svg.setAttribute("preserveAspectRatio", "none");
  const mk = (tag, attrs) => {
    const node = document.createElementNS("http://www.w3.org/2000/svg", tag);
    for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, String(value));
    return node;
  };
  const warnLine = mk("line", { class: "spark-warn", x1: 0, x2: 220, y1: 0, y2: 0 });
  const errorLine = mk("line", { class: "spark-error", x1: 0, x2: 220, y1: 0, y2: 0 });
  const path = mk("polyline", { class: "spark-line", points: "" });
  svg.append(errorLine, warnLine, path);
  const redraw = () => {
    if (history.length < 2) return;
    const ceiling = Math.max(...history, Number(field.error_above) || 0, Number(field.warn_above) || 0) * 1.15 || 1;
    const y = (v) => 40 - Math.min(1, Math.max(0, v / ceiling)) * 36;
    path.setAttribute("points", history.map((v, i) => `${(i / (history.length - 1)) * 220},${y(v)}`).join(" "));
    for (const [line, bound] of [[warnLine, field.warn_above], [errorLine, field.error_above]]) {
      const visible = Number.isFinite(bound) && bound < ceiling;
      line.setAttribute("y1", String(visible ? y(bound) : -10));
      line.setAttribute("y2", String(visible ? y(bound) : -10));
    }
  };
  return { svg, redraw };
}

function buildBadgeControl(field, updaters) {
  const wrap = document.createElement("div");
  wrap.className = "field-badge";
  const readout = document.createElement("div");
  readout.className = "badge-readout";
  const value = textCell("badge-value", "");
  readout.append(value);
  if (field.unit) readout.append(textCell("badge-unit", field.unit));
  const caption = textCell("badge-caption", "");
  readout.append(caption);
  wrap.append(readout);

  const history = [];
  const spark = field.series ? buildSparkline(field, history) : null;
  if (spark) wrap.append(spark.svg);

  updaters.set(field.id, (raw) => {
    const numeric = Number(raw);
    const level = levelFor(field, numeric);
    wrap.className = `field-badge ${level}`;
    value.textContent = formatModuleValue(numeric);
    caption.textContent = level === "error" ? `above error ${field.error_above}`
      : level === "warn" ? `above warn ${field.warn_above}`
      : Number.isFinite(field.warn_above) ? "within budget" : "";
    if (spark && Number.isFinite(numeric)) {
      history.push(numeric);
      if (history.length > 60) history.shift();
      spark.redraw();
    }
  });
  return wrap;
}

function buildSeriesControl(field, updaters) {
  const history = [];
  const spark = buildSparkline(field, history);
  updaters.set(field.id, (raw) => {
    const points = Array.isArray(raw) ? raw.map(Number) : [Number(raw)];
    for (const point of points) if (Number.isFinite(point)) history.push(point);
    while (history.length > 60) history.shift();
    if (Array.isArray(raw)) { history.length = 0; history.push(...points.filter(Number.isFinite)); }
    spark.redraw();
  });
  return spark.svg;
}

function buildStatusControl(field, updaters) {
  const pill = textCell("status-pill", "");
  updaters.set(field.id, (value) => {
    const label = isPlainObject(value) ? value.label : value;
    const tone = isPlainObject(value) ? value.tone : "idle";
    pill.textContent = String(label ?? "");
    pill.className = `status-pill ${tone || "idle"}`;
  });
  return pill;
}

function buildTableControl(field, updaters) {
  const host = document.createElement("div");
  host.className = "field-table";
  updaters.set(field.id, (value) => {
    const rows = Array.isArray(value) ? value : [];
    const columns = field.columns_spec || (rows[0] ? Object.keys(rows[0]) : []);
    const table = document.createElement("div");
    table.className = "data-table";
    table.style.setProperty("--table-columns", `1fr repeat(${Math.max(0, columns.length - 1)}, minmax(60px, 90px))`);
    const head = document.createElement("div");
    head.className = "data-row head";
    head.append(...columns.map((column) => textCell("data-cell", column)));
    table.append(head);
    for (const row of rows) {
      const line = document.createElement("div");
      line.className = "data-row";
      line.append(...columns.map((column) => textCell("data-cell", formatModuleValue(row[column]))));
      table.append(line);
    }
    if (!rows.length) table.append(textCell("data-empty", "No rows"));
    host.replaceChildren(table);
  });
  return host;
}

function buildVectorControl(module, field, updaters, dirty, onDirtyChange) {
  const wrap = document.createElement("div");
  wrap.className = "field-vector";
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const cells = new Map();
  let signature = "";
  let debounce;

  const commit = () => {
    const value = {};
    for (const [axis, node] of cells) value[axis] = Number(node.value);
    guard.pendingDebounce = true;
    clearTimeout(debounce);
    debounce = setTimeout(() => {
      guard.pendingDebounce = false;
      queueOrCommit(module, field, value, dirty, onDirtyChange, guard);
    }, 300);
  };
  const busy = () => [...cells.values()].includes(document.activeElement) || guard.pendingDebounce || guard.inFlight > 0 || Boolean(dirty?.has(field.id));

  const build = (axes) => {
    cells.clear();
    wrap.replaceChildren(...axes.map((axis) => {
      const cell = document.createElement("span");
      cell.className = `vector-axis axis-${axis.toLowerCase()}`;
      cell.append(textCell("vector-label", axis.toUpperCase()));
      let node;
      if (field.readonly) {
        node = textCell("vector-value", "");
      } else {
        node = document.createElement("input");
        node.type = "number";
        node.className = "vector-input";
        node.step = field.step ? String(field.step) : "any";
        attachNumberScrub(node, field);
        node.addEventListener("input", commit);
        node.addEventListener("blur", () => { if (!busy()) revertModuleField(field.id); });
      }
      cells.set(axis, node);
      cell.append(node);
      return cell;
    }));
  };

  updaters.set(field.id, (value) => {
    const entries = isPlainObject(value) ? Object.entries(value) : [];
    const next = entries.map(([axis]) => axis).join(",");
    if (next !== signature) {
      signature = next;
      build(entries.map(([axis]) => axis));
    }
    if (busy()) return;
    for (const [axis, component] of entries) {
      const node = cells.get(axis);
      if (!node) continue;
      if (node.tagName === "INPUT") node.value = String(Number(component));
      else node.textContent = formatModuleValue(component);
    }
  });
  return wrap;
}

function buildColorControl(module, field, updaters, dirty, onDirtyChange) {
  const wrap = document.createElement("div");
  wrap.className = "field-color";
  const hex = textCell("color-hex", "");

  if (field.readonly) {
    const swatch = document.createElement("i");
    wrap.append(swatch, hex);
    updaters.set(field.id, (value) => {
      const text = String(value || "");
      swatch.style.background = text;
      hex.textContent = text.toUpperCase();
    });
    return wrap;
  }

  const picker = document.createElement("input");
  picker.type = "color";
  picker.className = "color-input";
  wrap.append(picker, hex);
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  let debounce;
  picker.addEventListener("input", () => {
    const value = picker.value.toUpperCase();
    hex.textContent = value;
    guard.pendingDebounce = true;
    clearTimeout(debounce);
    debounce = setTimeout(() => {
      guard.pendingDebounce = false;
      queueOrCommit(module, field, value, dirty, onDirtyChange, guard);
    }, 250);
  });
  updaters.set(field.id, (value) => {
    if (isFieldBusy(picker, dirty, field.id, guard)) return;
    const text = String(value || "#000000");
    picker.value = text.toLowerCase();
    hex.textContent = text.toUpperCase();
  });
  return wrap;
}

function buildPathControl(field, updaters) {
  const wrap = document.createElement("div");
  wrap.className = "field-path";
  const text = textCell("path-value", "");
  const browse = document.createElement("button");
  browse.type = "button";
  browse.className = "button ghost";
  browse.textContent = "Browse";
  browse.addEventListener("click", () => setTab("files"));
  wrap.append(text, browse);
  updaters.set(field.id, (value) => { text.textContent = String(value ?? ""); });  return wrap;
}

function buildArtifactControl(field, updaters) {
  const wrap = document.createElement("div");
  wrap.className = "field-artifact";
  updaters.set(field.id, (value) => {
    const items = Array.isArray(value) ? value : value ? [value] : [];
    wrap.replaceChildren(...items.map((item) => {
      const line = document.createElement("div");
      line.className = "artifact-row";
      line.append(textCell("artifact-name", item.name || String(item)));
      if (item.size) line.append(textCell("artifact-meta", item.size));
      if (item.age) line.append(textCell("artifact-meta", item.age));
      const download = document.createElement("button");
      download.type = "button";
      download.className = "button ghost";
      download.textContent = "Download";
      download.addEventListener("click", () => setTab("files"));
      line.append(download);
      return line;
    }));
    if (!items.length) wrap.replaceChildren(textCell("artifact-empty", "No artifacts yet"));
  });
  return wrap;
}

function buildFlagsControl(module, field, updaters, dirty, onDirtyChange) {
  const wrap = document.createElement("div");
  wrap.className = "field-flags";
  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  let current = [];
  const render = () => {
    wrap.replaceChildren(...(field.options || []).map((option) => {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = `flag-chip${current.includes(option) ? " on" : ""}`;
      chip.textContent = current.includes(option) ? `${option} \u2713` : option;
      chip.disabled = Boolean(field.readonly);
      chip.addEventListener("click", () => {
        current = current.includes(option) ? current.filter((item) => item !== option) : [...current, option];
        render();
        queueOrCommit(module, field, current, dirty, onDirtyChange, guard);
      });
      return chip;
    }));
  };
  updaters.set(field.id, (value) => {
    if (dirty?.has(field.id) || guard.inFlight > 0) return;
    current = Array.isArray(value) ? value : String(value || "").split(",").map((item) => item.trim()).filter(Boolean);
    render();
  });
  render();
  return wrap;
}

function buildMeterControl(field, updaters) {
  const wrap = document.createElement("div");
  wrap.className = "field-meter";
  const bar = document.createElement("div");
  bar.className = "field-meter-bar";
  const fill = document.createElement("i");
  bar.append(fill);
  const readout = textCell("field-meter-readout", "");
  wrap.append(bar, readout);
  updaters.set(field.id, (value) => {
    const current = Number(value?.value ?? 0);
    const max = Number(value?.max ?? 0);
    fill.style.width = `${max > 0 ? Math.min(100, Math.max(0, (current / max) * 100)) : 0}%`;
    readout.textContent = max > 0 ? `${formatModuleValue(current)} / ${formatModuleValue(max)}` : formatModuleValue(current);
  });
  return wrap;
}

function buildJsonView(value) {
  let parsed = value;
  if (typeof value === "string") { try { parsed = JSON.parse(value); } catch { parsed = null; } }
  if (!isPlainObject(parsed)) {
    const pre = document.createElement("pre");
    pre.className = "module-json";
    pre.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2);
    return pre;  }
  const table = document.createElement("div");
  table.className = "json-table";
  for (const [key, item] of Object.entries(parsed)) {
    const row = document.createElement("div");
    row.className = "json-row";
    const keyCell = textCell("json-key", key);
    let kind = "other";
    let text;
    if (item === null) { kind = "null"; text = "null"; }
    else if (typeof item === "number") { kind = "number"; text = String(item); }
    else if (typeof item === "boolean") { kind = "bool"; text = String(item); }
    else if (typeof item === "string") { kind = "string"; text = item; }
    else { text = JSON.stringify(item); }
    row.append(keyCell, textCell(`json-val ${kind}`, text));
    table.append(row);
  }
  if (!table.childElementCount) table.append(textCell("json-empty", "{ }"));
  return table;
}

function buildJsonControl(field, updaters) {
  const host = document.createElement("div");
  host.className = "field-json";
  const bar = document.createElement("div");
  bar.className = "json-bar";
  const copy = document.createElement("button");
  copy.type = "button";
  copy.className = "button ghost";
  copy.textContent = "Copy JSON";
  bar.append(copy);
  const view = document.createElement("div");
  host.append(bar, view);
  let raw = "";
  copy.addEventListener("click", () => {
    if (navigator.clipboard) navigator.clipboard.writeText(raw).catch(() => {});
    toast("Session data copied");
  });
  updaters.set(field.id, (value) => {
    raw = typeof value === "string" ? value : JSON.stringify(value, null, 2);
    view.replaceChildren(buildJsonView(value));
  });
  return host;
}

function buildModuleFieldButton(module, field) {
  const button = document.createElement("button");
  if (field.command) {
    button.className = "button command-chip";
    button.append(textCell("command-caret", "\u203a"), textCell("command-text", field.command));
  } else if (field.busy_ms) {
    button.className = "button ghost busy";
    button.append(document.createElement("i"), textCell("command-text", field.label || field.id));
  } else {
    button.className = `button ${field.action_style === "primary" ? "primary" : field.action_style === "danger" ? "danger" : "ghost"}`;
    button.textContent = field.label || field.id;
    if (field.action_style === "primary" && field.description) button.append(textCell("button-sub", field.description));
  }
  button.title = field.description || field.command || "";
  button.addEventListener("click", async () => {
    if (field.requires_confirmation) {
      const accepted = await confirmAction({
        title: `${field.label || field.id}?`,
        body: "This runs an extension action on the connected build. It may briefly stall the game thread.",
        deviceName: selectedDevice()?.name || state.selectedId,
        commandLabel: field.action || field.id,
        acceptLabel: field.label || field.id,
      });
      if (!accepted) return;
    }
    button.disabled = true;
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/module-action`, {
        method: "POST",
        body: JSON.stringify({ module: module.id, action: field.action, parameters: {} }),
      });
      if (!result.success) throw new Error(result.error || "Action failed");
      toast(field.command ? `${field.command} executed` : `${field.label || field.id} completed`);
      if (result.values) applyModuleValues(module, result.values);
      else refreshModule();
    } catch (error) {
      toast(error.message, true);
    } finally {
      button.disabled = false;
    }
  });
  return button;
}

function setTab(tab) {
  state.tab = tab;
  clearTimeout(state.moduleTimer);
  document.querySelectorAll(".tab").forEach((button) => button.classList.toggle("active", button.dataset.tab === tab));
  document.querySelectorAll(".tab-panel").forEach((panel) => {
    const expected = tab.startsWith("module:") ? "tab-module" : `tab-${tab}`;
    panel.classList.toggle("hidden", panel.id !== expected);
  });
  logs.setActive(tab === "logs");
  if (tab === "files") refreshFiles();
  if (tab === "console" && !state.consoleEntries.length) refreshConsoleCatalog();
  if (tab.startsWith("module:")) {
    state.moduleSchema = null;
    state.modulePageId = null;
    state.moduleValues = {};
    elements.moduleContent.replaceChildren();
    refreshModule();
  }
}

function errorState(title, message, retry) {
  const wrapper = document.createElement("div");
  wrapper.className = "file-error";
  const copy = document.createElement("div");
  const strong = document.createElement("strong");
  strong.textContent = title;
  const detail = document.createElement("span");
  detail.textContent = message;
  copy.append(strong, detail);
  if (retry) {
    const button = document.createElement("button");
    button.className = "button ghost";
    button.style.marginTop = "12px";
    button.textContent = "Try again";
    button.addEventListener("click", retry);
    copy.append(document.createElement("br"), button);
  }
  wrapper.append(copy);
  return wrapper;
}

function confirmAction({ title, body, deviceName, commandLabel, acceptLabel }) {
  return new Promise((resolve) => {
    elements.confirmTitle.textContent = title;
    elements.confirmBody.textContent = body;
    elements.confirmDevice.textContent = deviceName;
    elements.confirmCommand.textContent = commandLabel;
    elements.confirmAccept.textContent = acceptLabel || title;
    elements.confirmOverlay.classList.remove("hidden");

    const settle = (result) => {
      elements.confirmOverlay.classList.add("hidden");
      elements.confirmCancel.removeEventListener("click", onCancel);
      elements.confirmAccept.removeEventListener("click", onAccept);
      elements.confirmOverlay.removeEventListener("click", onOverlay);
      resolve(result);
    };
    const onCancel = () => settle(false);
    const onAccept = () => settle(true);
    const onOverlay = (event) => { if (event.target === elements.confirmOverlay) settle(false); };

    elements.confirmCancel.addEventListener("click", onCancel);
    elements.confirmAccept.addEventListener("click", onAccept);
    elements.confirmOverlay.addEventListener("click", onOverlay);
  });
}

function normalizePath(path) {
  return String(path || "").replaceAll("\\", "/").split("/").filter((part) => part && part !== ".").join("/");
}
function isPlainObject(value) { return Boolean(value) && typeof value === "object" && !Array.isArray(value); }
function formatModuleValue(value) {
  if (typeof value === "number") return Number.isInteger(value) ? value.toLocaleString() : value.toLocaleString(undefined, { maximumFractionDigits: 2 });
  if (value === null) return "null";
  if (typeof value === "boolean") return value ? "Yes" : "No";
  return String(value);
}
const FILE_ICON_PATHS = {
  folder: "M2 4.4h4.1l1.3 1.7H14v7.5H2z",
  bundle: "M8 2.2 13.8 5.4v5.2L8 13.8 2.2 10.6V5.4zM2.2 5.4 8 8.6l5.8-3.2M8 8.6v5.2",
  file: "M4 2h5l3 3v9H4zM9 2v3.2h3",
  trace: "M1.6 8.4h2.9l2-4.2 2.7 7.6 1.9-3.4h3.3",
};
function isBundle(name) {
  return [".gputrace", ".app", ".dsym", ".framework"].some((suffix) => String(name).toLowerCase().endsWith(suffix));
}
function isTrace(name) {
  return [".utrace", ".ucache"].some((suffix) => String(name).toLowerCase().endsWith(suffix));
}
function formatBytes(bytes) {
  let value = Number(bytes || 0);
  const units = ["B", "KB", "MB", "GB", "TB"];
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit += 1; }
  return `${value.toFixed(unit ? 1 : 0)} ${units[unit]}`;
}
function formatDate(value) {
  if (!value) return "—";
  return new Date(value).toLocaleString([], { dateStyle: "medium", timeStyle: "short" });
}
function formatRelative(value) {
  if (!value) return "never";
  const seconds = Math.max(0, Math.round((Date.now() - new Date(value).getTime()) / 1000));
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  return `${Math.floor(seconds / 3600)}h`;
}
function formatDuration(seconds) {
  seconds = Number(seconds || 0);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  return hours ? `${hours}h ${minutes}m` : `${minutes}m`;
}
function shortEngineVersion(value) {
  const match = String(value || "").match(/\d+\.\d+(?:\.\d+)?/);
  return match?.[0] || "unknown";
}

elements.tabs.addEventListener("click", (event) => {
  const button = event.target.closest(".tab");
  if (button) setTab(button.dataset.tab);
});
elements.deviceSearch.addEventListener("input", renderDevices);
elements.commandForm.addEventListener("submit", (event) => {
  event.preventDefault();
  executeCommand(elements.commandInput.value);
});
elements.commandInput.addEventListener("keydown", (event) => {
  // A pinned hint is a signature reminder, not a menu, so it must not swallow keys.
  const open = state.completions.length > 0 && !state.completionPinned;
  if (event.key === "Escape" && state.completions.length) { event.preventDefault(); closeCompletions(); return; }
  if (event.key === "Tab" && open) { event.preventDefault(); acceptCompletion(state.completionIndex < 0 ? 0 : state.completionIndex); return; }
  if (event.key === "Enter" && open && state.completionIndex >= 0) { event.preventDefault(); acceptCompletion(); return; }
  if (event.key === "ArrowUp") { event.preventDefault(); open ? moveCompletion(-1) : navigateCommandHistory(1); }
  if (event.key === "ArrowDown") { event.preventDefault(); open ? moveCompletion(1) : navigateCommandHistory(-1); }
});
elements.commandInput.addEventListener("input", () => {
  clearTimeout(state.completionTimer);
  state.completionTimer = setTimeout(requestCompletions, 140);
});
elements.commandInput.addEventListener("blur", () => {
  clearTimeout(state.completionTimer);
  closeCompletions();
});
$("#clear-command-output").addEventListener("click", () => {
  elements.commandOutput.innerHTML = '<span class="terminal-muted">Output cleared.</span>';
});
$("#refresh-console").addEventListener("click", () => refreshConsoleCatalog(true));
elements.consoleSearch.addEventListener("input", () => {
  clearTimeout(state.consoleQueryTimer);
  state.consoleQueryTimer = setTimeout(refreshConsoleCatalog, 160);
});
elements.consoleScope.addEventListener("change", () => refreshConsoleCatalog());
$("#console-type").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-type]");
  if (!button) return;
  state.consoleType = button.dataset.type;
  $("#console-type").querySelectorAll("button").forEach((item) => item.classList.toggle("active", item === button));
  refreshConsoleCatalog();
});
elements.consoleSourceFilter.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-source]");
  if (!button) return;
  state.consoleSource = button.dataset.source;
  elements.consoleSourceFilter.querySelectorAll("button").forEach((item) => item.classList.toggle("active", item === button));
  refreshConsoleCatalog();
});
$("#download-folder").addEventListener("click", () => requestTransfer(state.filePath, true));
$("#refresh-files").addEventListener("click", refreshFiles);
elements.fileRoot.addEventListener("change", () => {
  resetFileNavigation(elements.fileRoot.value);
  refreshFiles();
});
elements.fileSearch.addEventListener("input", renderFiles);
elements.fileSort.addEventListener("change", renderFiles);
$("#file-up").addEventListener("click", () => {
  const parts = state.filePath.split("/").filter(Boolean);
  parts.pop();
  navigateFiles(parts.join("/"));
});
elements.fileBack.addEventListener("click", () => {
  if (state.fileHistoryIndex <= 0) return;
  state.fileHistoryIndex -= 1;
  state.filePath = state.fileHistory[state.fileHistoryIndex];
  refreshFiles();
});
elements.fileForward.addEventListener("click", () => {
  if (state.fileHistoryIndex >= state.fileHistory.length - 1) return;
  state.fileHistoryIndex += 1;
  state.filePath = state.fileHistory[state.fileHistoryIndex];
  refreshFiles();
});
refreshDevices();
logs.setActive(state.tab === "logs");
setInterval(refreshDevices, 2000);
setInterval(() => logs.poll(), 500);
