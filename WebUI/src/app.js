import "./styles.css";

"use strict";

const state = {
  devices: [],
  selectedId: null,
  tab: "logs",
  lastSequence: 0,
  visibleLogs: 0,
  fileRoot: "saved",
  filePath: "",
  fileEntries: [],
  fileHistory: [""],
  fileHistoryIndex: 0,
  transferPoll: null,
  lastTransferRequest: null,
  consoleEntries: [],
  consoleTotal: 0,
  consoleType: "",
  consoleSelected: null,
  consoleQueryTimer: null,
  commandHistory: JSON.parse(localStorage.getItem("deviceexplorer.commandHistory") || "[]"),
  commandHistoryIndex: -1,
  moduleTimer: null,
  moduleRequestSerial: 0,
  moduleSchema: null,
  modulePageId: null,
  moduleValues: {},
};

const $ = (selector) => document.querySelector(selector);
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
  logs: $("#logs"),
  logStats: $("#log-stats"),
  category: $("#log-category"),
  verbosity: $("#log-verbosity"),
  follow: $("#follow-logs"),
  commandForm: $("#command-form"),
  commandShortcutsWrap: $("#command-shortcuts-wrap"),
  commandShortcuts: $("#command-shortcuts"),
  commandInput: $("#command-input"),
  commandOutput: $("#command-output"),
  consoleSearch: $("#console-search"),
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
  toasts: $("#toast-region"),
  confirmOverlay: $("#confirm-overlay"),
  confirmTitle: $("#confirm-title"),
  confirmBody: $("#confirm-body"),
  confirmDevice: $("#confirm-device"),
  confirmCommand: $("#confirm-command"),
  confirmCancel: $("#confirm-cancel"),
  confirmAccept: $("#confirm-accept"),
};

async function api(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.method === "POST") {
    headers["Content-Type"] = "application/json";
    headers["X-DeviceExplorer-Request"] = "1";
  }
  const response = await fetch(path, { ...options, headers });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || `${response.status} ${response.statusText}`);
  return data;
}

function selectedDevice() {
  return state.devices.find((device) => device.id === state.selectedId) || null;
}

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
    if (!state.selectedId && state.devices.length) {
      state.selectedId = (state.devices.find((device) => device.connected) || state.devices[0]).id;
    }
    if (state.selectedId && !state.devices.some((device) => device.id === state.selectedId)) {
      state.selectedId = state.devices[0]?.id || null;
    }
    renderDevices();
    renderSelectedDevice();
  } catch (error) {
    toast(`Host API unavailable: ${error.message}`, true);
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
}

function renderSelectedDevice() {
  const device = selectedDevice();
  elements.empty.classList.toggle("hidden", Boolean(device));
  elements.workspace.classList.toggle("hidden", !device);
  if (!device) {
    elements.title.textContent = "Waiting for a device";
    elements.subtitle.textContent = "Launch a non-Shipping build on the same network.";
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
  state.lastSequence = 0;
  state.visibleLogs = 0;
  state.consoleEntries = [];
  state.consoleSelected = null;
  elements.logs.replaceChildren();
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

async function refreshLogs() {
  if (!state.selectedId || state.tab !== "logs") return;
  const params = new URLSearchParams({ after: String(state.lastSequence) });
  if (elements.category.value) params.set("category", elements.category.value);
  if (elements.verbosity.value) params.set("verbosity", elements.verbosity.value);
  try {
    const data = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/logs?${params}`);
    for (const entry of data.entries || []) {
      state.lastSequence = Math.max(state.lastSequence, entry.sequence || 0);
      appendLog(entry);
    }
    elements.logStats.textContent =
      `${state.visibleLogs.toLocaleString()} messages · ${Number(data.dropped || 0).toLocaleString()} dropped`;
    if (elements.follow.checked && data.entries?.length) elements.logs.scrollTop = elements.logs.scrollHeight;
  } catch (error) {
    if (!String(error).toLowerCase().includes("unknown device")) console.error(error);
  }
}

function appendLog(entry) {
  const row = document.createElement("div");
  row.className = `log-row ${String(entry.verbosity || "").toLowerCase()}`;
  row.append(
    textCell("time", formatLogTime(entry.timestamp)),
    textCell("category", entry.category),
    textCell("verbosity", entry.verbosity),
    textCell("message", entry.message),
  );
  elements.logs.append(row);
  state.visibleLogs += 1;
  while (elements.logs.childElementCount > 10_000) elements.logs.firstElementChild.remove();
}

async function refreshConsoleCatalog() {
  if (!requireOnline()) return;
  const query = elements.consoleSearch.value.trim();
  elements.consoleCatalog.innerHTML = '<div class="inline-empty">Querying runtime console…</div>';
  elements.consoleCount.textContent = "Loading catalog";
  try {
    const params = new URLSearchParams({ q: query, limit: "800" });
    const result = await api(`/api/devices/${encodeURIComponent(state.selectedId)}/console-objects?${params}`);
    state.consoleEntries = result.entries || [];
    state.consoleTotal = Number(result.total || state.consoleEntries.length);
    elements.consoleCount.textContent =
      `${state.consoleTotal.toLocaleString()} supported${result.truncated ? ` · showing ${state.consoleEntries.length}` : ""}`;
    renderConsoleCatalog();
  } catch (error) {
    elements.consoleCount.textContent = "Catalog unavailable";
    elements.consoleCatalog.replaceChildren(errorState("Cannot read console catalog", error.message));
  }
}

function renderConsoleCatalog() {
  const entries = state.consoleEntries.filter((entry) => !state.consoleType || entry.type === state.consoleType);
  elements.consoleCatalog.replaceChildren();
  for (const entry of entries) {
    const button = document.createElement("button");
    button.className = `catalog-row ${state.consoleSelected?.name === entry.name ? "active" : ""}`;
    button.type = "button";
    const copy = document.createElement("span");
    copy.append(textCell("catalog-name", entry.name));
    if (entry.type === "variable") copy.append(textCell("catalog-value", entry.value || "(empty)"));
    button.append(copy, textCell(`catalog-type ${entry.type === "variable" ? "variable" : "command"}`, entry.type === "variable" ? "CVar" : "Cmd"));
    button.addEventListener("click", () => selectConsoleEntry(entry));
    elements.consoleCatalog.append(button);
  }
  if (!entries.length) elements.consoleCatalog.append(textCell("inline-empty", "No matching console objects were reported by the build."));
}

function selectConsoleEntry(entry) {
  state.consoleSelected = entry;
  elements.commandInput.value = entry.name;
  const flags = [entry.read_only && "read-only", entry.cheat && "cheat"].filter(Boolean);
  elements.consoleDetail.replaceChildren();
  const badge = textCell("kind-badge", entry.type === "variable" ? "CONSOLE VARIABLE" : "CONSOLE COMMAND");
  const title = document.createElement("h3");
  title.textContent = entry.name;
  const help = document.createElement("p");
  help.textContent = entry.help || "No help text was registered.";
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
  renderConsoleCatalog();
  elements.commandInput.focus();
  elements.commandInput.setSelectionRange(elements.commandInput.value.length, elements.commandInput.value.length);
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
    appendCommandOutput(`${result.success ? "" : "[not handled] "}${result.output || "(no direct output)"}\n[${elapsed} ms]\n\n`);
    if (state.consoleSelected?.type === "variable" && command.startsWith(state.consoleSelected.name)) {
      clearTimeout(state.consoleQueryTimer);
      state.consoleQueryTimer = setTimeout(refreshConsoleCatalog, 250);
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

  elements.moduleTitle.textContent = module.label || module.id;
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
  elements.moduleContent.replaceChildren();
  const entries = isPlainObject(data) ? Object.entries(data) : [["Data", data]];
  for (const [title, value] of entries) {
    const card = document.createElement("section");
    card.className = "module-card";
    const heading = document.createElement("h3");
    heading.textContent = title;
    card.append(heading, renderDataValue(value));
    elements.moduleContent.append(card);
  }
  if (!entries.length) elements.moduleContent.append(textCell("inline-empty", "The module returned no data."));
}

function renderDataValue(value) {
  if (isPlainObject(value) && Object.values(value).every((item) => !isPlainObject(item) && !Array.isArray(item))) {
    const list = document.createElement("dl");
    list.className = "key-value";
    for (const [key, item] of Object.entries(value)) {
      const term = document.createElement("dt");
      term.textContent = key;
      const description = document.createElement("dd");
      description.textContent = formatModuleValue(item);
      list.append(term, description);
    }
    return list;
  }
  const pre = document.createElement("pre");
  pre.className = "module-json";
  pre.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2);
  return pre;
}

function moduleSchemaSignature(module, page) {
  return `${module.id}:${page?.id || "overview"}:${JSON.stringify(page?.sections || [])}`;
}

function renderModuleSchemaIfChanged(module, page) {
  const signature = moduleSchemaSignature(module, page);
  if (state.moduleSchema?.moduleId === module.id && state.moduleSchema.signature === signature) return;
  const updaters = new Map();
  const sections = (page?.sections || []).map((section) => buildModuleSection(module, section, updaters));
  elements.moduleContent.replaceChildren(...sections);
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

  const body = document.createElement("div");
  body.className = "module-card-body";
  const grid = document.createElement("div");
  grid.className = "field-grid";
  if (section.columns > 0) grid.style.setProperty("--field-columns", String(section.columns));

  const dirty = section.apply === "manual" ? new Map() : null;
  let applyButton = null;
  let discardButton = null;
  let submitting = false;
  const updateApplyButton = () => {
    const disabled = !dirty || dirty.size === 0;
    if (applyButton) applyButton.disabled = disabled;
    if (discardButton) discardButton.disabled = disabled;
  };

  for (const field of section.fields || []) {
    grid.append(buildModuleField(module, field, updaters, dirty, updateApplyButton));
  }
  body.append(grid);

  if (dirty) {
    const footer = document.createElement("div");
    footer.className = "field-apply-row";
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
    dirty.clear();
    for (const fieldId of fieldIds) revertModuleField(fieldId);
    toast(error.message, true);
  } finally {
    updateApplyButton();
  }
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
    dirty.set(field.id, value);
    onDirtyChange();
    return;
  }
  await commitModuleField(module, field, value, guard);
}

function buildModuleField(module, field, updaters, dirty, onDirtyChange) {
  const row = document.createElement("div");
  row.className = "field-row";
  row.style.setProperty("--field-span", String(field.span || 1));
  if (field.description) row.title = field.description;

  if (field.widget === "button") {
    row.classList.add("field-row-button");
    row.append(buildModuleFieldButton(module, field));
    return row;
  }

  const label = textCell("field-label", field.label || field.id);
  if (field.description) {
    const help = textCell("field-help", "?");
    help.title = field.description;
    label.append(help);
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
    case "badge": return buildBadgeControl(field, updaters);
    case "meter": return buildMeterControl(field, updaters);
    case "json": return buildJsonControl(field, updaters);
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
    const chip = textCell("bool-chip", "");
    const updater = (value) => {
      chip.textContent = value ? "Yes" : "No";
      chip.classList.toggle("on", Boolean(value));
    };
    updaters.set(field.id, updater);
    return chip;
  }
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.className = "field-checkbox";
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
  const readout = textCell("field-number-readout", "");
  if (!number || field.unit) wrap.append(readout);

  const guard = { pendingDebounce: false, inFlight: 0, serial: 0 };
  const applyValue = (value) => {
    const numeric = Number(value);
    for (const input of inputs) input.value = String(numeric);
    readout.textContent = field.unit ? `${formatModuleValue(numeric)} ${field.unit}` : formatModuleValue(numeric);
  };
  const busy = () => inputs.includes(document.activeElement) || guard.pendingDebounce || guard.inFlight > 0 || Boolean(dirty?.has(field.id));
  updaters.set(field.id, (value) => { if (!busy()) applyValue(value); });

  let debounce;
  for (const input of inputs) {
    input.addEventListener("input", () => {
      const value = Number(input.value);
      if (Number.isNaN(value)) return;
      for (const peer of inputs) if (peer !== input) peer.value = String(value);
      readout.textContent = field.unit ? `${formatModuleValue(value)} ${field.unit}` : formatModuleValue(value);
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

function buildBadgeControl(field, updaters) {
  const chip = textCell("badge-chip", "");
  updaters.set(field.id, (value) => {
    const numeric = Number(value);
    chip.textContent = field.unit ? `${formatModuleValue(numeric)} ${field.unit}` : formatModuleValue(numeric);
    let level = "ok";
    if (Number.isFinite(field.error_above) && numeric >= field.error_above) level = "error";
    else if (Number.isFinite(field.warn_above) && numeric >= field.warn_above) level = "warn";
    chip.className = `badge-chip ${level}`;
  });
  return chip;
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

function buildJsonControl(field, updaters) {
  const pre = document.createElement("pre");
  pre.className = "module-json field-json";
  updaters.set(field.id, (value) => { pre.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2); });
  return pre;
}

function buildModuleFieldButton(module, field) {
  const button = document.createElement("button");
  button.className = `button ${field.action_style === "primary" ? "primary" : field.action_style === "danger" ? "danger" : "ghost"}`;
  button.textContent = field.label || field.id;
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
      toast(`${field.label || field.id} completed`);
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
  if (tab === "files") refreshFiles();
  if (tab === "logs") refreshLogs();
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

function toast(message, isError = false) {
  const item = document.createElement("div");
  item.className = `toast ${isError ? "error" : ""}`;
  item.textContent = message;
  elements.toasts.append(item);
  setTimeout(() => item.remove(), 4500);
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

function textCell(className, value) {
  const span = document.createElement("span");
  span.className = className;
  span.textContent = value ?? "";
  return span;
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
function downloadText(filename, text) {
  const url = URL.createObjectURL(new Blob([text], { type: "text/plain;charset=utf-8" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = filename;
  link.click();
  URL.revokeObjectURL(url);
}
const FILE_ICON_PATHS = {
  folder: "M2 4.4h4.1l1.3 1.7H14v7.5H2z",
  bundle: "M8 2.2 13.8 5.4v5.2L8 13.8 2.2 10.6V5.4zM2.2 5.4 8 8.6l5.8-3.2M8 8.6v5.2",
  file: "M4 2h5l3 3v9H4zM9 2v3.2h3",
  trace: "M1.6 8.4h2.9l2-4.2 2.7 7.6 1.9-3.4h3.3",
};
function svgIcon(className, path, size = 15) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("width", String(size));
  svg.setAttribute("height", String(size));
  svg.setAttribute("viewBox", "0 0 16 16");
  svg.setAttribute("class", `icon ${className}`.trim());
  const path_ = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path_.setAttribute("d", path);
  svg.append(path_);
  return svg;
}
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
function formatLogTime(value) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "--:--:--";
  return date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit", fractionalSecondDigits: 3, hour12: false });
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
  if (event.key === "ArrowUp") { event.preventDefault(); navigateCommandHistory(1); }
  if (event.key === "ArrowDown") { event.preventDefault(); navigateCommandHistory(-1); }
});
$("#clear-command-output").addEventListener("click", () => {
  elements.commandOutput.innerHTML = '<span class="terminal-muted">Output cleared.</span>';
});
$("#refresh-console").addEventListener("click", refreshConsoleCatalog);
elements.consoleSearch.addEventListener("input", () => {
  clearTimeout(state.consoleQueryTimer);
  state.consoleQueryTimer = setTimeout(refreshConsoleCatalog, 280);
});
$("#console-type").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-type]");
  if (!button) return;
  state.consoleType = button.dataset.type;
  $("#console-type").querySelectorAll("button").forEach((item) => item.classList.toggle("active", item === button));
  renderConsoleCatalog();
});
$("#clear-logs").addEventListener("click", () => {
  elements.logs.replaceChildren();
  state.visibleLogs = 0;
  elements.logStats.textContent = "0 messages";
});
$("#save-logs").addEventListener("click", () => {
  const lines = [...elements.logs.children].map((row) => [...row.children].map((cell) => cell.textContent).join("\t"));
  if (!lines.length) {
    toast("There are no log messages to save yet.", true);
    return;
  }
  const device = selectedDevice();
  const stamp = new Date().toISOString().slice(0, 19).replaceAll(":", "-");
  downloadText(`${String(device?.name || "device").replace(/[^\w.-]+/g, "_")}-${stamp}.log`, `${lines.join("\n")}\n`);
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
for (const control of [elements.category, elements.verbosity]) {
  control.addEventListener("input", () => {
    clearTimeout(control._filterTimer);
    control._filterTimer = setTimeout(() => {
      elements.logs.replaceChildren();
      state.visibleLogs = 0;
      state.lastSequence = 0;
      refreshLogs();
    }, 200);
  });
}

refreshDevices();
setInterval(refreshDevices, 2000);
setInterval(refreshLogs, 500);
