import { readFileSync } from "node:fs";
import { DEVICEEXPLORER_PROTOCOL_VERSION } from "../protocol-version.js";

const fixture = JSON.parse(readFileSync(new URL("./fixtures/default.json", import.meta.url), "utf8"));

function clone(value) {
  return structuredClone(value);
}

function sendJson(response, status, value, scenario) {
  response.statusCode = status;
  response.setHeader("Content-Type", "application/json; charset=utf-8");
  response.setHeader("Cache-Control", "no-store");
  response.setHeader("X-DeviceExplorer-Mock", scenario);
  response.end(`${JSON.stringify(value)}\n`);
}

function sendError(response, status, error, scenario) {
  sendJson(response, status, { error }, scenario);
}

function readJson(request) {
  return new Promise((resolve, reject) => {
    let body = "";
    request.setEncoding("utf8");
    request.on("data", (chunk) => { body += chunk; });
    request.on("end", () => {
      if (!body) return resolve({});
      try {
        resolve(JSON.parse(body));
      } catch (error) {
        reject(error);
      }
    });
    request.on("error", reject);
  });
}

function makeState(scenario) {
  const state = clone(fixture);
  state.scenario = scenario;
  state.transfers = new Map();
  state.nextTransfer = 1;
  state.log_levels = new Map(Object.entries(state.log_categories).map(([name, entry]) => [name, { ...entry, verbosity: entry.boot }]));
  for (const device of state.devices) device.protocol_version = DEVICEEXPLORER_PROTOCOL_VERSION;
  if (scenario === "empty") state.devices = [];
  if (scenario === "offline") {
    for (const device of state.devices) device.connected = false;
  }
  return state;
}

function requireDevice(state, response, deviceId) {
  const device = state.devices.find((item) => item.id === deviceId);
  if (!device) sendError(response, 404, "Unknown device", state.scenario);
  return device;
}

function requirePostHeader(request, response, scenario) {
  if (request.headers["x-deviceexplorer-request"] === "1") return true;
  sendError(response, 400, "Invalid request", scenario);
  return false;
}

function rankConsoleMatch(entry, query, searchHelp) {
  if (!query) return 3;
  const name = entry.name.toLowerCase();
  if (name === query) return 0;
  if (name.startsWith(query)) return 1;
  if (name.includes(query)) return 2;
  if (searchHelp && (entry.help || "").toLowerCase().includes(query)) return 3;
  return -1;
}

function listConsole(state, url) {
  if (url.searchParams.get("index") === "1") {
    // Mirrors the device: the index omits help, arrives name-sorted, and the dashboard filters it locally.
    const entries = state.console_objects
      .map(({ help, ...rest }) => rest)
      .sort((left, right) => left.name.toLowerCase().localeCompare(right.name.toLowerCase()));
    return { index: true, entries, total: entries.length, catalog_total: entries.length, truncated: false };
  }
  const query = (url.searchParams.get("q") || "").toLowerCase();
  const searchHelp = url.searchParams.get("scope") === "all";
  const source = url.searchParams.get("source") || "";
  const kind = url.searchParams.get("kind") || "";
  const limit = Math.max(1, Number(url.searchParams.get("limit") || 400));
  const matches = state.console_objects
    .filter((entry) => (!source || (entry.source || "cvar") === source) && (!kind || entry.type === kind))
    .map((entry) => ({ entry, rank: rankConsoleMatch(entry, query, searchHelp) }))
    .filter((match) => match.rank >= 0)
    .sort((left, right) => left.rank - right.rank || left.entry.name.localeCompare(right.entry.name))
    .map((match) => match.entry);
  return {
    entries: matches.slice(0, limit),
    total: matches.length,
    catalog_total: state.console_objects.length,
    truncated: matches.length > limit
  };
}

function logCategories(state) {
  return [...state.log_levels.entries()].map(([name, entry]) => ({
    name,
    verbosity: entry.verbosity,
    baseline: entry.boot,
    source: entry.verbosity === entry.boot ? "boot" : "runtime",
    ...(entry.discovered ? { max: entry.ceiling } : {})
  }));
}

const LOG_LEVELS = ["Fatal", "Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose"];

function setLogVerbosity(state, body) {
  const results = [];
  for (const { category, verbosity } of body.entries || []) {
    const entry = state.log_levels.get(category);
    if (!entry || !LOG_LEVELS.includes(verbosity)) {
      results.push({ category, requested: verbosity, success: false, error: "This build has no such log category" });
      continue;
    }
    // Mirrors FLogCategoryBase::SetVerbosity(), which silently clamps to the compiled ceiling.
    const ceiling = entry.ceiling || "VeryVerbose";
    const applied = LOG_LEVELS.indexOf(verbosity) > LOG_LEVELS.indexOf(ceiling) ? ceiling : verbosity;
    if (applied !== verbosity) entry.discovered = true;
    entry.verbosity = applied;
    results.push({
      category,
      requested: verbosity,
      applied,
      success: applied === verbosity,
      error: applied === verbosity ? "" : `${verbosity} is compiled out in this build`
    });
  }
  return { success: true, auto_revert: Boolean(body.auto_revert), results, categories: logCategories(state) };
}

function executeCommand(state, body) {
  const command = String(body.command || body.command_id || "").trim();
  if (!command) return { success: false, output: "Command is empty", log_output: "" };
  const [name, ...argumentsList] = command.split(/\s+/);
  const variable = state.console_objects.find((entry) => entry.type === "variable" && entry.name.toLowerCase() === name.toLowerCase());
  if (variable && argumentsList.length) {
    variable.value = argumentsList.join(" ");
    for (const entry of state.console_objects) {
      if (entry.companion === variable.name) entry.value = variable.value;
    }
    return { success: true, output: `${variable.name} = ${variable.value}`, log_output: "" };
  }
  if (variable) return { success: true, output: `${variable.name} = ${variable.value}`, log_output: "" };
  if (command.toLowerCase() === "stat unit") {
    return { success: true, output: "", log_output: "Frame 16.70 ms | Game 8.24 ms | Draw 5.10 ms | GPU 13.82 ms" };
  }
  if (command.toLowerCase().startsWith("memreport")) {
    return {
      success: true,
      output: "",
      log_output: "Memory report saved to Saved/Profiling/MemoryReport/memreport-full.txt"
    };
  }
  return { success: true, output: "", log_output: `Mock executed: ${command}` };
}

function moduleData(state, moduleId) {
  const module = state.module_state[moduleId];
  if (!module) return null;
  if (moduleId === "runtime_monitor") {
    const current = Number(module.values.frame_ms || 16);
    const next = current >= 44 ? 12.4 : current + (Math.random() * 3.4 - 0.9);
    module.values.frame_ms = Number(Math.max(11, next).toFixed(1));
  }
  if (moduleId === "widget_gallery") {
    const series = Array.isArray(module.values.g_series) ? module.values.g_series.slice(1) : [];
    series.push(Number((10 + Math.random() * 18).toFixed(1)));
    module.values.g_series = series;
  }
  return { success: true, error: "", data: clone(module.data), values: clone(module.values) };
}

const SET_VALIDATION = {
  frame_budget_ms: "Must be greater than zero",
  g_number_both: "Must be greater than zero",
};

function invokeModuleAction(state, body) {
  const module = state.module_state[body.module];
  if (!module) return { status: 404, result: { error: "Unknown module" } };
  const parameters = body.parameters || {};
  if (body.action === "__set") {
    for (const [field, message] of Object.entries(SET_VALIDATION)) {
      if (Object.hasOwn(parameters, field) && Number(parameters[field]) <= 0) {
        return { status: 200, result: { success: false, error: `${field}: ${message}`, values: clone(module.values) } };
      }
    }
    Object.assign(module.values, parameters);
    return { status: 200, result: { success: true, error: "", values: clone(module.values) } };
  }
  if (body.action === "__btn_capture") {
    const meter = module.values.snapshots || { value: 0, max: 5 };
    meter.value = Math.min(meter.max, meter.value + 1);
    module.values.snapshots = meter;
    const artifacts = Array.isArray(module.values.artifacts) ? module.values.artifacts : [];
    const name = `snapshot-${String(meter.value).padStart(3, "0")}.json`;
    module.values.artifacts = [{ name, size: "8.7 KB", age: "just now" }, ...artifacts].slice(0, 5);
  }
  if (body.action === "__btn_reset_counters") {
    module.values.snapshots = { value: 0, max: module.values.snapshots?.max || 5 };
  }
  if (body.action === "__btn_restart") {
    module.values.snapshots = { value: 0, max: module.values.snapshots?.max || 5 };
    module.values.active = true;
  }
  return { status: 200, result: { success: true, error: "", data: clone(module.data), values: clone(module.values) } };
}

function createTransfer(state, deviceId, body) {
  const id = `mock-transfer-${state.nextTransfer++}`;
  const source = String(body.path || body.root || "download");
  const leaf = source.split("/").filter(Boolean).pop() || body.root || "download";
  const filename = body.archive ? `${leaf}.zip` : leaf;
  const transfer = {
    id,
    device_id: deviceId,
    root: body.root,
    path: body.path,
    filename,
    state: "requested",
    bytes: 0,
    error: "",
    polls: 0,
    created_at: new Date().toISOString(),
    updated_at: new Date().toISOString()
  };
  state.transfers.set(id, transfer);
  return transfer;
}

async function route(request, response, state) {
  const url = new URL(request.url, "http://127.0.0.1");
  const method = request.method || "GET";
  if (url.pathname === "/health" && method === "GET") {
    return sendJson(response, 200, { status: "ok", mock: true, scenario: state.scenario }, state.scenario);
  }
  if (url.pathname === "/api/config" && method === "GET") {
    return sendJson(response, 200, {
      protocol_version: DEVICEEXPLORER_PROTOCOL_VERSION,
      device_port: 18081,
      service_type: "_deviceexplorer._tcp.local.",
      mock: true,
      scenario: state.scenario
    }, state.scenario);
  }
  if (url.pathname === "/api/devices" && method === "GET") {
    return sendJson(response, 200, { devices: state.devices }, state.scenario);
  }

  const deviceMatch = url.pathname.match(/^\/api\/devices\/([^/]+)\/([^/]+)$/);
  if (deviceMatch) {
    const deviceId = decodeURIComponent(deviceMatch[1]);
    const action = deviceMatch[2];
    const device = requireDevice(state, response, deviceId);
    if (!device) return;
    if (state.scenario === "errors" && ["console-objects", "files", "module-data"].includes(action)) {
      return sendError(response, 504, `Mock ${action} timeout`, state.scenario);
    }
    if (action === "logs" && method === "GET") {
      const after = Number(url.searchParams.get("after") || 0);
      const category = (url.searchParams.get("category") || "").toLowerCase();
      const verbosity = (url.searchParams.get("verbosity") || "").toLowerCase();
      const entries = (state.logs[deviceId] || []).filter((entry) =>
        entry.sequence > after &&
        (!category || entry.category.toLowerCase().includes(category)) &&
        (!verbosity || entry.verbosity.toLowerCase() === verbosity));
      return sendJson(response, 200, {
        entries,
        dropped: device.dropped_logs || 0,
        buffered: (state.logs[deviceId] || []).length,
        capacity: 100000
      }, state.scenario);
    }
    if (action === "log-categories" && method === "GET") {
      return sendJson(response, 200, { success: true, auto_revert: true, categories: logCategories(state) }, state.scenario);
    }
    if (action === "log-verbosity" && method === "POST") {
      if (!requirePostHeader(request, response, state.scenario)) return;
      return sendJson(response, 200, setLogVerbosity(state, await readJson(request)), state.scenario);
    }
    if (action === "console-objects" && method === "GET") {
      return sendJson(response, 200, listConsole(state, url), state.scenario);
    }
    if (action === "files" && method === "GET") {
      const root = url.searchParams.get("root") || "saved";
      const path = (url.searchParams.get("path") || "").replaceAll("\\", "/").replace(/^\/+|\/+$/g, "");
      const tree = state.file_trees[root];
      if (!tree) return sendError(response, 404, "Unknown file root", state.scenario);
      if (!Object.hasOwn(tree, path)) return sendError(response, 404, "Directory not found", state.scenario);
      return sendJson(response, 200, { entries: clone(tree[path]) }, state.scenario);
    }
    if (action === "module-data" && method === "GET") {
      const result = moduleData(state, url.searchParams.get("module"));
      return result
        ? sendJson(response, 200, result, state.scenario)
        : sendError(response, 404, "Unknown module", state.scenario);
    }
    if (action === "command" && method === "POST") {
      if (!requirePostHeader(request, response, state.scenario)) return;
      return sendJson(response, 200, executeCommand(state, await readJson(request)), state.scenario);
    }
    if (action === "module-action" && method === "POST") {
      if (!requirePostHeader(request, response, state.scenario)) return;
      const result = invokeModuleAction(state, await readJson(request));
      return sendJson(response, result.status, result.result, state.scenario);
    }
    if (action === "transfers" && method === "POST") {
      if (!requirePostHeader(request, response, state.scenario)) return;
      return sendJson(response, 202, createTransfer(state, deviceId, await readJson(request)), state.scenario);
    }
  }

  const transferMatch = url.pathname.match(/^\/api\/transfers\/([^/]+)(?:\/([^/]+))?$/);
  if (transferMatch) {
    const id = decodeURIComponent(transferMatch[1]);
    const action = transferMatch[2] || "";
    const transfer = state.transfers.get(id);
    if (!transfer) return sendError(response, 404, "Unknown transfer", state.scenario);
    if (!action && method === "GET") {
      transfer.polls += 1;
      transfer.state = transfer.polls >= 2 ? "ready" : "transferring";
      transfer.bytes = transfer.state === "ready" ? 131072 : 65536;
      transfer.updated_at = new Date().toISOString();
      return sendJson(response, 200, transfer, state.scenario);
    }
    if (action === "trace" && method === "POST") {
      if (!requirePostHeader(request, response, state.scenario)) return;
      return sendJson(response, 200, { status: "sent" }, state.scenario);
    }
    if (action === "download" && method === "GET") {
      const content = `DeviceExplorer mock download: ${transfer.filename}\n`;
      response.statusCode = 200;
      response.setHeader("Content-Type", "application/octet-stream");
      response.setHeader("Content-Disposition", `attachment; filename="${transfer.filename.replaceAll('"', "")}"`);
      response.setHeader("X-DeviceExplorer-Mock", state.scenario);
      return response.end(content);
    }
  }

  sendError(response, 404, "Mock route not found", state.scenario);
}

export function deviceExplorerMock({ scenario = "default" } = {}) {
  const state = makeState(scenario);
  return {
    name: "deviceexplorer-mock-api",
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const pathname = new URL(request.url, "http://127.0.0.1").pathname;
        if (pathname !== "/health" && !pathname.startsWith("/api/")) return next();
        route(request, response, state).catch((error) => {
          if (!response.headersSent) sendError(response, 500, error.message, state.scenario);
          else response.end();
        });
      });
    }
  };
}
