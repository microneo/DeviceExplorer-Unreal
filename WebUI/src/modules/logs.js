import "./logs.css";
import { $, api, downloadText, formatCount, formatLogTime, svgIcon, textCell, toast } from "../shared.js";

const LEVELS = ["Fatal", "Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose"];
const LEVEL_RANK = new Map(LEVELS.map((level, index) => [level, index]));
const ERROR_LEVELS = new Set(["error", "fatal"]);
const GLOBAL_CATEGORY = "Global";
/** The protocol revision that added list_log_categories / set_log_verbosity. */
const VERBOSITY_PROTOCOL = 7;
// The device buffer is far larger than anything worth keeping in a DOM table, so the view
// holds a rolling window and the footer reports how much of the buffer that covers.
const MAX_BUFFERED_ENTRIES = 20_000;
const MAX_RENDERED_ROWS = 4_000;
const MAX_VERBOSITY_ROWS = 300;
const FILTER_PRESET_KEY = "deviceexplorer.logFilterPreset";
const COPY_ICON = "M5.2 5.2h8v8h-8zM10.8 2.8H3.6a.8.8 0 0 0-.8.8v7.2";

const CATEGORY_SORTS = {
  name: (left, right) => left.name.localeCompare(right.name),
  rows: (left, right) => left.rows - right.rows,
  warnings: (left, right) => left.warnings - right.warnings,
  errors: (left, right) => left.errors - right.errors,
};

const VERBOSITY_PRESETS = [
  { id: "quiet", label: "Quiet", hint: "all → Error", level: "Error", matches: () => true },
  { id: "render", label: "Render debug", level: "Verbose", matches: (name) => /(RHI|Render|Shader|Streaming|Texture)/i.test(name) },
  { id: "net", label: "Net trace", level: "Verbose", matches: (name) => /(Net|Online|Packet|Replication)/i.test(name) },
];

/** Pseudo-preset behind Reset all: every category back to the level the build launched with. */
const BOOT_TARGET = { baseline: true };

/**
 * Owns the Logs tab: the streaming table with its client-side category filter, and the
 * Verbosity sub-tab that writes levels back to the build.
 */
export function createLogsModule({ getDevice }) {
  const elements = {
    deviceLabel: $("#log-device-label"),
    streamState: $("#log-stream-state"),
    pages: $("#log-pages"),
    streamPage: $("#log-stream-page"),
    verbosityPage: $("#log-verbosity-page"),
    overrideCount: $("#log-override-count"),

    rows: $("#logs"),
    search: $("#log-search"),
    levelFilter: $("#log-level-filter"),
    follow: $("#follow-logs"),
    filterBar: $("#log-filter-bar"),
    filterChips: $("#log-filter-chips"),
    filterCount: $("#log-filter-count"),
    errorCount: $("#log-error-count"),
    warningCount: $("#log-warning-count"),
    hiddenCount: $("#log-hidden-count"),
    bufferCount: $("#log-buffer-count"),

    categorySearch: $("#log-category-search"),
    categoryCount: $("#log-category-count"),
    categoryBulk: $("#log-category-bulk"),
    selectedCount: $("#log-selected-count"),
    categories: $("#log-categories"),
    categoryHead: $("#log-category-head"),
    toggleAll: $("#toggle-all-log-categories"),

    verbositySearch: $("#verbosity-search"),
    verbosityScope: $("#verbosity-scope"),
    verbosityGlobal: $("#verbosity-global"),
    verbosityRows: $("#verbosity-rows"),
    verbosityStatus: $("#verbosity-status"),
    verbosityShowAll: $("#show-all-verbosity"),
    verbosityResetAll: $("#reset-all-verbosity"),
    verbosityPresets: $("#verbosity-presets"),
    verbosityPending: $("#verbosity-pending"),
    verbosityValidation: $("#verbosity-validation"),
    verbositySummary: $("#verbosity-summary"),
    persist: $("#persist-log-verbosity"),
    autoRevert: $("#auto-revert-log-verbosity"),
    discard: $("#discard-verbosity"),
    apply: $("#apply-verbosity"),
  };

  const state = {
    active: false,
    deviceId: null,
    page: "stream",
    sequence: 0,
    entries: [],
    stats: new Map(),
    errors: 0,
    warnings: 0,
    dropped: 0,
    buffered: 0,
    capacity: 0,
    level: "all",
    hidden: new Set(),
    categorySort: { key: "rows", direction: "desc" },
    categoryRows: new Map(),
    categoryRenderedAt: 0,
    // Verbosity sub-tab.
    levels: new Map(),
    pending: new Map(),
    rejected: new Map(),
    ceiling: new Map(),
    scope: "active",
    showAll: false,
    loading: false,
    applying: false,
  };

  /* ---------------------------------------------------------------- stream */

  function levelOf(entry) {
    return String(entry.verbosity || "").toLowerCase();
  }

  function countEntry(entry, delta) {
    const name = String(entry.category || "Uncategorized");
    const stats = state.stats.get(name) || { name, rows: 0, warnings: 0, errors: 0 };
    stats.rows += delta;
    const level = levelOf(entry);
    if (level === "warning") {
      stats.warnings += delta;
      state.warnings += delta;
    }
    if (ERROR_LEVELS.has(level)) {
      stats.errors += delta;
      state.errors += delta;
    }
    if (stats.rows > 0) state.stats.set(name, stats);
    else state.stats.delete(name);
  }

  function isVisible(entry) {
    if (state.hidden.has(String(entry.category || "Uncategorized"))) return false;
    const level = levelOf(entry);
    if (state.level === "warnings" && level !== "warning" && !ERROR_LEVELS.has(level)) return false;
    if (state.level === "errors" && !ERROR_LEVELS.has(level)) return false;
    const query = elements.search.value.trim().toLowerCase();
    return !query || `${entry.category} ${entry.message}`.toLowerCase().includes(query);
  }

  function buildRow(entry) {
    const row = document.createElement("div");
    row.className = `log-row ${levelOf(entry)}`;
    const copy = document.createElement("button");
    copy.type = "button";
    copy.className = "log-copy";
    copy.title = "Copy line";
    copy.setAttribute("aria-label", "Copy log line");
    copy.append(svgIcon("", COPY_ICON, 12));
    row.append(
      textCell("time", formatLogTime(entry.timestamp)),
      textCell("category", entry.category),
      textCell("level", entry.verbosity),
      textCell("message", entry.message),
      copy,
    );
    return row;
  }

  function trimRows() {
    while (elements.rows.childElementCount > MAX_RENDERED_ROWS) elements.rows.firstElementChild.remove();
  }

  function appendRows(entries) {
    const fragment = document.createDocumentFragment();
    let appended = 0;
    for (const entry of entries) {
      if (!isVisible(entry)) continue;
      fragment.append(buildRow(entry));
      appended += 1;
    }
    if (!appended) return 0;
    elements.rows.querySelector(".log-empty")?.remove();
    elements.rows.append(fragment);
    trimRows();
    return appended;
  }

  function atBottom() {
    return elements.rows.scrollHeight - elements.rows.scrollTop - elements.rows.clientHeight < 24;
  }

  function rebuildRows() {
    elements.rows.replaceChildren();
    appendRows(state.entries);
    showEmptyStateIfNeeded();
    scrollToEnd();
  }

  function showEmptyStateIfNeeded() {
    if (elements.rows.childElementCount) return;
    if (state.entries.length) elements.rows.append(textCell("log-empty", "No messages match the current filter."));
    else if (getDevice()?.connected) elements.rows.append(textCell("log-empty", "Waiting for log messages…"));
    else elements.rows.append(textCell("log-empty", "This build is offline and its host buffer is empty."));
  }

  function scrollToEnd() {
    if (elements.follow.checked) elements.rows.scrollTop = elements.rows.scrollHeight;
  }

  function visibleRowCount() {
    return elements.rows.querySelectorAll(".log-row").length;
  }

  function renderFooter() {
    const shown = visibleRowCount();
    const hidden = Math.max(0, state.entries.length - shown);
    elements.errorCount.lastChild.textContent = `${formatCount(state.errors)} ${state.errors === 1 ? "error" : "errors"}`;
    elements.warningCount.lastChild.textContent = `${formatCount(state.warnings)} ${state.warnings === 1 ? "warning" : "warnings"}`;
    elements.hiddenCount.textContent = hidden ? `${formatCount(hidden)} hidden by filter` : "";
    elements.hiddenCount.classList.toggle("hidden", hidden === 0);

    const buffered = Math.max(state.buffered, state.entries.length);
    const parts = [`buffer ${formatCount(buffered)}${state.capacity ? ` / ${formatCount(state.capacity)}` : ""}`];
    if (state.dropped) parts.push(`${formatCount(state.dropped)} dropped`);
    elements.bufferCount.textContent = parts.join(" · ");

    const device = getDevice();
    elements.deviceLabel.textContent = device ? `${device.name || device.id} · ${device.platform || "Unknown"}` : "";
    elements.streamState.classList.toggle("static", !device?.connected);
    elements.streamState.lastChild.textContent = device?.connected ? "streaming" : "offline";
    renderFilterBar(shown);
  }

  function renderFilterBar(shown) {
    const chips = [];
    const query = elements.search.value.trim();
    if (query) {
      chips.push(filterChip(`search: ${query}`, () => {
        elements.search.value = "";
        applyFilters();
      }));
    }
    if (state.level !== "all") {
      chips.push(filterChip(state.level === "warnings" ? "Warnings+" : "Errors", () => setLevel("all")));
    }
    if (state.hidden.size) {
      const visible = [...state.stats.keys()].filter((name) => !state.hidden.has(name)).sort();
      if (visible.length && visible.length <= 6) {
        for (const name of visible) {
          chips.push(filterChip(name, () => {
            state.hidden.add(name);
            applyFilters();
          }));
        }
      } else {
        chips.push(filterChip(`${state.hidden.size} categories hidden`, () => {
          state.hidden.clear();
          applyFilters();
        }));
      }
    }
    elements.filterChips.replaceChildren(...chips);
    elements.filterCount.textContent = `${formatCount(shown)} / ${formatCount(state.entries.length)} rows`;
    elements.filterBar.classList.toggle("hidden", chips.length === 0);
  }

  function filterChip(label, onRemove) {
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "log-filter-chip";
    chip.setAttribute("aria-label", `Remove filter ${label}`);
    chip.append(textCell("", label), textCell("log-filter-chip-x", "✕"));
    chip.addEventListener("click", onRemove);
    return chip;
  }

  function applyFilters() {
    rebuildRows();
    renderFooter();
    renderCategories(true);
  }

  function setLevel(level) {
    state.level = level;
    for (const button of elements.levelFilter.querySelectorAll("button")) {
      button.classList.toggle("active", button.dataset.level === level);
    }
    applyFilters();
  }

  /* ------------------------------------------------------------ categories */

  function categoryRow(name) {
    let cells = state.categoryRows.get(name);
    if (cells) return cells;

    const row = document.createElement("div");
    row.className = "log-category-row";
    const check = document.createElement("button");
    check.type = "button";
    check.className = "log-check";
    check.setAttribute("role", "checkbox");
    const label = textCell("log-category-name", name);
    label.title = name;
    const only = document.createElement("button");
    only.type = "button";
    only.className = "log-category-only";
    only.textContent = "only";
    only.title = `Show only ${name}`;
    const rows = textCell("log-category-number", "0");
    const warnings = textCell("log-category-number warning", "—");
    const errors = textCell("log-category-number error", "—");
    row.append(check, label, only, rows, warnings, errors);

    const toggle = () => {
      if (state.hidden.has(name)) state.hidden.delete(name);
      else state.hidden.add(name);
      applyFilters();
    };
    check.addEventListener("click", toggle);
    label.addEventListener("click", toggle);
    only.addEventListener("click", (event) => {
      event.stopPropagation();
      state.hidden = new Set([...state.stats.keys()].filter((item) => item !== name));
      applyFilters();
    });

    cells = { row, check, rows, warnings, errors };
    state.categoryRows.set(name, cells);
    return cells;
  }

  function renderCategories(force = false) {
    const now = performance.now();
    if (!force && now - state.categoryRenderedAt < 900) return;
    state.categoryRenderedAt = now;

    const query = elements.categorySearch.value.trim().toLowerCase();
    const { key, direction } = state.categorySort;
    const compare = CATEGORY_SORTS[key] || CATEGORY_SORTS.rows;
    const all = [...state.stats.values()].sort((left, right) =>
      (direction === "asc" ? compare(left, right) : compare(right, left)) || left.name.localeCompare(right.name));
    for (const header of elements.categoryHead.querySelectorAll(".log-sort")) {
      header.classList.toggle("active", header.dataset.sort === key);
      header.dataset.direction = header.dataset.sort === key ? direction : "";
    }
    const shown = all.filter((entry) => !query || entry.name.toLowerCase().includes(query));

    const ordered = [];
    for (const entry of shown) {
      const cells = categoryRow(entry.name);
      const enabled = !state.hidden.has(entry.name);
      cells.row.classList.toggle("enabled", enabled);
      cells.check.setAttribute("aria-checked", String(enabled));
      cells.check.setAttribute("aria-label", `${enabled ? "Hide" : "Show"} ${entry.name}`);
      cells.rows.textContent = formatCount(entry.rows);
      cells.warnings.textContent = entry.warnings ? formatCount(entry.warnings) : "—";
      cells.errors.textContent = entry.errors ? formatCount(entry.errors) : "—";
      cells.warnings.classList.toggle("muted", !entry.warnings);
      cells.errors.classList.toggle("muted", !entry.errors);
      ordered.push(cells.row);
    }
    for (const [name, cells] of state.categoryRows) {
      if (!state.stats.has(name)) {
        cells.row.remove();
        state.categoryRows.delete(name);
      }
    }
    // Re-appending live nodes reorders without rebuilding them, so hover and focus survive.
    const keep = new Set(ordered);
    for (const child of [...elements.categories.children]) {
      if (!keep.has(child)) child.remove();
    }
    elements.categories.append(...ordered);
    if (!ordered.length) {
      elements.categories.append(textCell("log-empty", all.length ? "No categories match this filter." : "No categories yet."));
    }

    const hiddenHere = all.filter((entry) => state.hidden.has(entry.name)).length;
    elements.categoryCount.textContent = `${formatCount(all.length)} in buffer`;
    elements.selectedCount.textContent = hiddenHere ? `${formatCount(all.length - hiddenHere)} of ${formatCount(all.length)}` : "all";
    const toggleState = hiddenHere === 0 ? "all" : hiddenHere === all.length ? "none" : "some";
    elements.toggleAll.dataset.state = toggleState;
    elements.toggleAll.setAttribute("aria-checked", toggleState === "all" ? "true" : toggleState === "none" ? "false" : "mixed");
  }

  /* -------------------------------------------------------------- polling */

  function resetStream() {
    state.sequence = 0;
    state.entries = [];
    state.stats.clear();
    state.categoryRows.clear();
    state.errors = 0;
    state.warnings = 0;
    state.dropped = 0;
    state.buffered = 0;
    elements.rows.replaceChildren();
    elements.categories.replaceChildren();
  }

  async function poll() {
    if (!state.active || !state.deviceId) return;
    try {
      const data = await api(`/api/devices/${encodeURIComponent(state.deviceId)}/logs?after=${state.sequence}`);
      const incoming = data.entries || [];
      state.dropped = Number(data.dropped || 0);
      state.buffered = Number(data.buffered || 0);
      state.capacity = Number(data.capacity || state.capacity || 0);
      for (const entry of incoming) {
        state.sequence = Math.max(state.sequence, entry.sequence || 0);
        state.entries.push(entry);
        countEntry(entry, 1);
      }
      while (state.entries.length > MAX_BUFFERED_ENTRIES) countEntry(state.entries.shift(), -1);
      if (incoming.length) {
        const stick = elements.follow.checked || atBottom();
        appendRows(incoming);
        showEmptyStateIfNeeded();
        if (stick) elements.rows.scrollTop = elements.rows.scrollHeight;
      }
      renderFooter();
      renderCategories();
    } catch (error) {
      if (!String(error).toLowerCase().includes("unknown device")) console.error(error);
    }
  }

  /* ------------------------------------------------------------- verbosity */

  function effectiveLevel(name) {
    return state.pending.get(name) || state.levels.get(name)?.verbosity || "Log";
  }

  function isBlocked(name, level) {
    const ceiling = state.ceiling.get(name);
    return Boolean(ceiling) && LEVEL_RANK.get(level) > LEVEL_RANK.get(ceiling);
  }

  function matchesScope(entry) {
    // A row with an unapplied edit always stays visible: scoping on the pending level would
    // make the row vanish from under the click that set it.
    if (state.pending.has(entry.name)) return true;
    if (state.scope === "changed") return entry.source === "runtime";
    if (state.scope === "active") return entry.verbosity !== "Fatal";
    return true;
  }

  function verbosityEntries() {
    const query = elements.verbositySearch.value.trim().toLowerCase();
    return [...state.levels.values()]
      .filter((entry) => entry.name !== GLOBAL_CATEGORY)
      .filter((entry) => !query || entry.name.toLowerCase().includes(query))
      .filter(matchesScope)
      // Deliberately stable: reordering on edit would slide the row out from under the click.
      .sort((left, right) => left.name.localeCompare(right.name));
  }

  function verbosityDetail(entry) {
    const rejection = state.rejected.get(entry.name);
    if (rejection) return rejection;
    if (state.pending.has(entry.name)) return `was ${entry.verbosity}`;
    const cap = state.ceiling.get(entry.name);
    if (cap) return `compiled out above ${cap}`;
    const stats = state.stats.get(entry.name);
    if (stats) return `${formatCount(stats.rows)} ${stats.rows === 1 ? "row" : "rows"} in buffer`;
    if (entry.source === "runtime") return `boot level ${entry.baseline}`;
    return "at boot level";
  }

  function buildVerbosityRow(entry, { global = false } = {}) {
    const pending = state.pending.has(entry.name);
    const rejected = state.rejected.has(entry.name);
    const row = document.createElement("div");
    row.className = `verbosity-row${pending ? " pending" : ""}${rejected ? " rejected" : ""}${global ? " global" : ""}`;

    const identity = document.createElement("span");
    identity.className = "verbosity-identity";
    const name = textCell("verbosity-name", global ? "Global default" : entry.name);
    name.title = entry.name;
    identity.append(name, textCell("verbosity-detail", global ? "applies to every category" : verbosityDetail(entry)));

    const source = rejected ? "rejected" : pending ? "pending" : entry.source === "runtime" ? "runtime" : "boot";
    const control = document.createElement("span");
    control.className = "verbosity-levels";
    const segments = document.createElement("span");
    segments.className = "verbosity-segments";
    const current = effectiveLevel(entry.name);
    for (const level of LEVELS) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = level;
      button.dataset.level = level.toLowerCase();
      button.className = level === current ? `on${rejected ? " rejected" : ""}` : "";
      button.disabled = state.applying || isBlocked(entry.name, level);
      if (button.disabled && level !== current) button.title = `${level} is compiled out of ${entry.name} in this build`;
      button.setAttribute("aria-pressed", String(level === current));
      button.addEventListener("click", () => queueLevel(entry.name, level));
      segments.append(button);
    }
    control.append(segments);
    if (pending || entry.verbosity !== entry.baseline) {
      const reset = document.createElement("button");
      reset.type = "button";
      reset.className = "text-button verbosity-reset";
      reset.textContent = "Reset";
      reset.title = pending ? `Drop this edit and keep ${entry.verbosity}` : `Queue a change back to the boot level ${entry.baseline}`;
      reset.addEventListener("click", () => queueLevel(entry.name, pending ? entry.verbosity : entry.baseline));
      control.append(reset);
    }

    row.append(identity, textCell(`verbosity-source ${source}`, source), control);
    return row;
  }

  function queueLevel(name, level) {
    const entry = state.levels.get(name);
    if (!entry || state.applying) return;
    if (level === entry.verbosity) state.pending.delete(name);
    else state.pending.set(name, level);
    state.rejected.delete(name);
    renderVerbosity();
  }

  function renderVerbosity() {
    const global = state.levels.get(GLOBAL_CATEGORY);
    elements.verbosityGlobal.replaceChildren(...(global ? [buildVerbosityRow(global, { global: true })] : []));

    const entries = verbosityEntries();
    const capped = state.showAll ? entries : entries.slice(0, MAX_VERBOSITY_ROWS);
    elements.verbosityRows.replaceChildren(...capped.map((entry) => buildVerbosityRow(entry)));
    if (!capped.length) {
      elements.verbosityRows.append(textCell("log-empty",
        state.levels.size ? "No categories match this view." : "This build reported no log categories."));
    }

    const total = state.levels.size ? state.levels.size - (global ? 1 : 0) : 0;
    elements.verbosityStatus.textContent = state.loading
      ? "Reading log categories from the build…"
      : `${formatCount(capped.length)} of ${formatCount(total)} categories shown`;
    elements.verbosityShowAll.classList.toggle("hidden", capped.length >= entries.length && state.scope === "all" && !elements.verbositySearch.value);

    const resettable = presetTargets(BOOT_TARGET).length;
    elements.verbosityResetAll.disabled = state.applying || resettable === 0;
    elements.verbosityResetAll.title = resettable
      ? `Queue ${formatCount(resettable)} ${resettable === 1 ? "category" : "categories"} back to the levels the build launched with`
      : "Every category is already at its boot level";

    renderPending();
    renderPresets();
  }

  function renderPending() {
    const pending = [...state.pending.entries()];
    if (pending.length) {
      elements.verbosityPending.replaceChildren(...pending.map(([name, level]) => {
        const line = document.createElement("code");
        line.append(textCell("pending-verb", "Log"), textCell("", ` ${name} `), textCell(`pending-level ${state.rejected.has(name) ? "rejected" : ""}`, level));
        return line;
      }));
    } else {
      elements.verbosityPending.replaceChildren(Object.assign(document.createElement("p"), { textContent: "No pending changes." }));
    }

    const rejected = state.rejected.size;
    elements.verbosityValidation.textContent = rejected
      ? `${rejected} ${rejected === 1 ? "level was" : "levels were"} clamped by the build; pick a level it can emit.`
      : "";
    elements.verbosityValidation.classList.toggle("hidden", rejected === 0);
    elements.verbositySummary.textContent = pending.length
      ? `${pending.length} ${pending.length === 1 ? "change" : "changes"}${rejected ? ` · ${rejected} rejected` : ""}`
      : "No changes";
    elements.verbositySummary.classList.toggle("has-rejected", rejected > 0);
    elements.discard.disabled = pending.length === 0 || state.applying;
    elements.apply.disabled = pending.length === 0 || state.applying;
    const overrides = [...state.levels.values()].filter((entry) => entry.source === "runtime").length;
    elements.overrideCount.textContent = String(overrides + pending.length);
    elements.overrideCount.classList.toggle("hidden", overrides + pending.length === 0);
  }

  function renderPresets() {
    elements.verbosityPresets.replaceChildren(...VERBOSITY_PRESETS.map((preset) => {
      const targets = presetTargets(preset);
      const button = document.createElement("button");
      button.type = "button";
      button.disabled = state.applying || targets.length === 0;
      button.append(
        Object.assign(document.createElement("strong"), { textContent: preset.label }),
        textCell("", preset.hint || `${formatCount(targets.length)} ${targets.length === 1 ? "category" : "categories"}`),
      );
      button.addEventListener("click", () => {
        queueTargets(targets);
      });
      return button;
    }));
  }

  function queueTargets(targets) {
    for (const [name, level] of targets) {
      if (level === state.levels.get(name).verbosity) state.pending.delete(name);
      else state.pending.set(name, level);
      state.rejected.delete(name);
    }
    renderVerbosity();
  }

  function presetTargets(preset) {
    const targets = [];
    for (const entry of state.levels.values()) {
      if (entry.name === GLOBAL_CATEGORY) continue;
      const level = preset.baseline ? entry.baseline : preset.matches(entry.name) ? preset.level : null;
      if (!level || isBlocked(entry.name, level) || level === effectiveLevel(entry.name)) continue;
      targets.push([entry.name, level]);
    }
    return targets;
  }

  function ingestCategories(categories) {
    state.levels = new Map((categories || []).map((entry) => [entry.name, {
      name: entry.name,
      verbosity: entry.verbosity || "Log",
      baseline: entry.baseline || entry.verbosity || "Log",
      source: entry.source || "boot",
    }]));
    for (const entry of categories || []) {
      if (entry.max) state.ceiling.set(entry.name, entry.max);
    }
    for (const name of [...state.pending.keys()]) {
      if (!state.levels.has(name) || state.pending.get(name) === state.levels.get(name).verbosity) state.pending.delete(name);
    }
  }

  async function loadVerbosity() {
    if (!state.deviceId || state.loading) return;
    const device = getDevice();
    if (Number(device?.protocol_version || 0) < VERBOSITY_PROTOCOL) {
      state.levels.clear();
      renderVerbosity();
      elements.verbosityStatus.textContent =
        `This build speaks protocol ${device?.protocol_version || "?"}; log levels need ${VERBOSITY_PROTOCOL}. Rebuild it against this plugin version.`;
      return;
    }
    state.loading = true;
    renderVerbosity();
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.deviceId)}/log-categories`);
      ingestCategories(result.categories);
      if (typeof result.auto_revert === "boolean") elements.autoRevert.checked = result.auto_revert;
    } catch (error) {
      state.levels.clear();
      toast(`Cannot read log categories: ${error.message}`, true);
    } finally {
      state.loading = false;
      renderVerbosity();
    }
  }

  async function applyVerbosity() {
    const pending = [...state.pending.entries()];
    if (!pending.length || !state.deviceId) return;
    state.applying = true;
    state.rejected.clear();
    renderVerbosity();
    try {
      const result = await api(`/api/devices/${encodeURIComponent(state.deviceId)}/log-verbosity`, {
        method: "POST",
        body: JSON.stringify({
          entries: pending.map(([category, verbosity]) => ({ category, verbosity })),
          persist: elements.persist.checked,
          auto_revert: elements.autoRevert.checked,
        }),
      });
      ingestCategories(result.categories);
      let applied = 0;
      for (const outcome of result.results || []) {
        if (outcome.success) {
          applied += 1;
          state.pending.delete(outcome.category);
          continue;
        }
        state.pending.set(outcome.category, outcome.requested);
        state.rejected.set(outcome.category, outcome.error || "The build rejected this level");
      }
      if (applied) toast(`${applied} log ${applied === 1 ? "level" : "levels"} applied`);
      if (state.rejected.size) toast(`${state.rejected.size} log ${state.rejected.size === 1 ? "level" : "levels"} rejected by the build`, true);
    } catch (error) {
      toast(`Cannot set log levels: ${error.message}`, true);
    } finally {
      state.applying = false;
      renderVerbosity();
    }
  }

  /* ---------------------------------------------------------------- pages */

  function setPage(page) {
    state.page = page;
    for (const button of elements.pages.querySelectorAll("button[data-log-page]")) {
      button.classList.toggle("active", button.dataset.logPage === page);
    }
    elements.streamPage.classList.toggle("hidden", page !== "stream");
    elements.verbosityPage.classList.toggle("hidden", page !== "verbosity");
    if (page === "verbosity" && !state.levels.size) loadVerbosity();
    if (page === "stream") poll();
  }

  /* --------------------------------------------------------------- events */

  elements.pages.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-log-page]");
    if (button) setPage(button.dataset.logPage);
  });
  elements.rows.addEventListener("click", (event) => {
    const copy = event.target.closest(".log-copy");
    if (!copy) return;
    const line = [...copy.parentElement.children].filter((cell) => cell !== copy).map((cell) => cell.textContent).join("\t");
    navigator.clipboard?.writeText(line).then(() => toast("Log line copied")).catch(() => {});
  });
  elements.rows.addEventListener("scroll", () => {
    if (elements.follow.checked && !atBottom()) elements.follow.checked = false;
  });
  elements.search.addEventListener("input", applyFilters);
  elements.categorySearch.addEventListener("input", () => renderCategories(true));
  elements.levelFilter.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-level]");
    if (button) setLevel(button.dataset.level);
  });
  elements.categoryBulk.addEventListener("click", (event) => {
    const action = event.target.closest("button[data-category-action]")?.dataset.categoryAction;
    if (!action) return;
    const names = [...state.stats.keys()];
    if (action === "all") state.hidden.clear();
    if (action === "none") state.hidden = new Set(names);
    if (action === "invert") state.hidden = new Set(names.filter((name) => !state.hidden.has(name)));
    applyFilters();
  });
  elements.categoryHead.addEventListener("click", (event) => {
    const key = event.target.closest(".log-sort")?.dataset.sort;
    if (!key) return;
    // Names read best A-Z, counts read best worst-first, so each key picks its own default.
    state.categorySort = state.categorySort.key === key
      ? { key, direction: state.categorySort.direction === "asc" ? "desc" : "asc" }
      : { key, direction: key === "name" ? "asc" : "desc" };
    renderCategories(true);
  });
  elements.toggleAll.addEventListener("click", () => {
    state.hidden = state.hidden.size ? new Set() : new Set(state.stats.keys());
    applyFilters();
  });
  $("#reset-log-filters").addEventListener("click", () => {
    elements.search.value = "";
    state.hidden.clear();
    setLevel("all");
  });
  $("#open-log-verbosity").addEventListener("click", () => setPage("verbosity"));
  $("#clear-logs").addEventListener("click", () => {
    state.entries = [];
    state.stats.clear();
    state.categoryRows.clear();
    state.errors = 0;
    state.warnings = 0;
    elements.categories.replaceChildren();
    applyFilters();
  });
  $("#save-logs").addEventListener("click", () => {
    const lines = state.entries.filter(isVisible)
      .map((entry) => `${formatLogTime(entry.timestamp)}\t${entry.category}\t${entry.verbosity}\t${entry.message}`);
    if (!lines.length) {
      toast("There are no log messages to save yet.", true);
      return;
    }
    const device = getDevice();
    const stamp = new Date().toISOString().slice(0, 19).replaceAll(":", "-");
    downloadText(`${String(device?.name || "device").replace(/[^\w.-]+/g, "_")}-${stamp}.log`, `${lines.join("\n")}\n`);
  });
  $("#save-log-preset").addEventListener("click", () => {
    localStorage.setItem(FILTER_PRESET_KEY, JSON.stringify({ level: state.level, hidden: [...state.hidden] }));
    toast("Log filter preset saved");
  });
  $("#load-log-preset").addEventListener("click", () => {
    const saved = localStorage.getItem(FILTER_PRESET_KEY);
    if (!saved) {
      toast("No log filter preset has been saved yet.", true);
      return;
    }
    try {
      const preset = JSON.parse(saved);
      state.hidden = new Set(Array.isArray(preset.hidden) ? preset.hidden : []);
      setLevel(["all", "warnings", "errors"].includes(preset.level) ? preset.level : "all");
      toast("Log filter preset loaded");
    } catch {
      toast("The saved log filter preset is unreadable.", true);
    }
  });

  elements.verbositySearch.addEventListener("input", renderVerbosity);
  elements.verbosityScope.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-scope]");
    if (!button) return;
    state.scope = button.dataset.scope;
    for (const item of elements.verbosityScope.querySelectorAll("button")) item.classList.toggle("active", item === button);
    renderVerbosity();
  });
  elements.verbosityShowAll.addEventListener("click", () => {
    elements.verbositySearch.value = "";
    state.scope = "all";
    state.showAll = true;
    for (const item of elements.verbosityScope.querySelectorAll("button")) item.classList.toggle("active", item.dataset.scope === "all");
    renderVerbosity();
  });
  elements.verbosityResetAll.addEventListener("click", () => queueTargets(presetTargets(BOOT_TARGET)));
  $("#refresh-verbosity").addEventListener("click", loadVerbosity);
  elements.discard.addEventListener("click", () => {
    state.pending.clear();
    state.rejected.clear();
    renderVerbosity();
  });
  elements.apply.addEventListener("click", applyVerbosity);

  return {
    setActive(active) {
      state.active = active;
      if (!active) return;
      renderFooter();
      if (state.page === "stream") poll();
      else if (!state.levels.size) loadVerbosity();
    },
    setDevice(deviceId) {
      if (state.deviceId === deviceId) return;
      state.deviceId = deviceId;
      resetStream();
      state.levels.clear();
      state.pending.clear();
      state.rejected.clear();
      state.ceiling.clear();
      state.showAll = false;
      renderVerbosity();
      applyFilters();
    },
    poll,
  };
}
