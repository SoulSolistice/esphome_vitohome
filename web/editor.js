// Tabular catalog editor and YAML exporter for esphome_vitohome (web/editor.html).
"use strict";

const RAW_BASE = "https://raw.githubusercontent.com/SoulSolistice/esphome_vitohome/main/example/catalogs/";

const BASE_COLUMNS = [
  { key: "component",           label: "Component",         mono: true },
  { key: "group",               label: "Group",             mono: false },
  { key: "_tags",               label: "Tags",              mono: true },
  { key: "name",                label: "Name",              mono: false, editable: true },
  { key: "id",                  label: "ID",                mono: true },
  { key: "address",             label: "Address",           mono: true,  numericHex: true },
  { key: "length",              label: "Length",            mono: true,  numeric: true },
  { key: "byte_offset",         label: "Byte Offset",       mono: true,  numeric: true },
  { key: "byte_length",         label: "Byte Length",       mono: true,  numeric: true },
  { key: "bit_mask",            label: "Bit Mask",          mono: true,  numericHex: true },
  { key: "type",                label: "Type",              mono: true },
  { key: "converter",           label: "Converter",         mono: true },
  { key: "signed",              label: "Signed",            mono: true },
  { key: "unit_of_measurement", label: "Unit",              mono: true },
  { key: "accuracy_decimals",   label: "Decimals",          mono: true,  numeric: true },
  { key: "device_class",        label: "Device Class",      mono: true },
  { key: "state_class",         label: "State Class",       mono: true },
  { key: "entity_category",     label: "Category",          mono: true },
  { key: "icon",                label: "Icon",              mono: true },
  { key: "update_interval",     label: "Update Interval",   mono: true,  editable: true, interval: true },
  { key: "min_value",           label: "Min Value",         mono: true,  editable: true, numeric: true },
  { key: "max_value",           label: "Max Value",         mono: true,  editable: true, numeric: true },
  { key: "step",                label: "Step",              mono: true,  editable: true, numeric: true },
  { key: "mode",                label: "Mode",              mono: true },
  { key: "on_value",            label: "On Value",          mono: true },
  { key: "off_value",           label: "Off Value",         mono: true },
  { key: "_map_summary",        label: "Options / Codes",   mono: false, isMap: true },
  { key: "_filters",            label: "Filters",           mono: true,  isMap: true },
  { key: "disabled_by_default", label: "Default Disabled",  mono: true },
  { key: "_notes",              label: "Notes / Comments",  mono: false }
];

function unquoteYaml(val) {
  const t = val.trim();
  if (t.length >= 2 && t.startsWith('"') && t.endsWith('"')) {
    return t.slice(1, -1).replace(/\\(["\\])/g, "$1");
  }
  if (t.length >= 2 && t.startsWith("'") && t.endsWith("'")) {
    return t.slice(1, -1).replace(/''/g, "'");
  }
  return t;
}

function splitValAndComment(rawVal) {
  let inSingle = false;
  let inDouble = false;
  for (let i = 0; i < rawVal.length; i++) {
    const ch = rawVal[i];
    if (ch === "'" && !inDouble) inSingle = !inSingle;
    else if (ch === '"' && !inSingle) inDouble = !inDouble;
    else if (ch === "#" && !inSingle && !inDouble && (i === 0 || /\s/.test(rawVal[i - 1]))) {
      return {
        value: rawVal.slice(0, i).trim(),
        comment: rawVal.slice(i + 1).trim()
      };
    }
  }
  return { value: rawVal.trim(), comment: "" };
}

// The `# vito:` line the generator writes above an entity: Vitosoft's own
// grouping, e.g. "circuit=hk1,dhw level=td overview quickcheck trend". Each
// becomes one tag ("hk1", "dhw", "level=td", "overview", ...), in TAG_ORDER.
const TAG_LINE = /^\s*#\s*vito:\s*(.*)$/;
const TAG_LABELS = {
  hk1: "HK1", hk2: "HK2", hk3: "HK3", hk4: "HK4", hk5: "HK5", hk6: "HK6",
  dhw: "DHW", solar: "Solar", boiler: "Boiler", cascade: "Cascade", mbus: "M-Bus",
  overview: "Overview", quickcheck: "Quick check", trend: "Trend", "level=td": "Service level"
};
const TAG_ORDER = Object.keys(TAG_LABELS);

function parseTagLine(text) {
  const tags = [];
  for (const word of text.trim().split(/\s+/)) {
    if (word.startsWith("circuit=")) tags.push(...word.slice(8).split(",").filter(Boolean));
    else if (word) tags.push(word);
  }
  return tags;
}

function sortTags(tags) {
  const rank = (t) => (TAG_ORDER.includes(t) ? TAG_ORDER.indexOf(t) : TAG_ORDER.length);
  return [...tags].sort((a, b) => rank(a) - rank(b) || a.localeCompare(b));
}

// A `type: action` button RUNS a procedure on the controller when pressed
// (Vitosoft Remote_Procedure_Call). The catalog ships it disabled, and the
// export keeps it that way.
function isActionEntity(ent) {
  return ent.component === "button" && ent.props.type === "action";
}

// Parses an esphome_vitohome catalog YAML into structured entities while
// preserving raw YAML lines and YAML anchors for lossless export.
function parseCatalogYaml(yamlText) {
  const lines = String(yamlText || "").replace(/\r\n/g, "\n").split("\n");
  const parsedEntities = [];
  const anchors = {};

  let currentDomain = null;
  let currentGroup = "";
  let pendingComments = [];
  let currentEntity = null;
  let inNestedMap = null;

  const finalizeEntity = () => {
    if (!currentEntity) return;
    while (currentEntity.rawLines.length && currentEntity.rawLines[currentEntity.rawLines.length - 1].trim() === "") {
      currentEntity.rawLines.pop();
    }
    const mapEntries = currentEntity.mapEntries;
    if (mapEntries.length > 0) {
      currentEntity.props._map_summary = mapEntries.map((m) => m.key + ": " + m.val).join("\n");
    } else if (currentEntity.usesAnchor) {
      const ref = anchors[currentEntity.usesAnchor];
      if (ref && ref.mapEntries) {
        currentEntity.props._map_summary = "*" + currentEntity.usesAnchor + " (" + ref.mapEntries.length + " entries)\n" +
          ref.mapEntries.map((m) => m.key + ": " + m.val).join("\n");
      } else {
        currentEntity.props._map_summary = "*" + currentEntity.usesAnchor;
      }
    }
    if (currentEntity.filterEntries.length > 0) {
      currentEntity.props._filters = currentEntity.filterEntries.join("\n");
    }
    currentEntity.props._notes = currentEntity.notes.join("\n");
    parsedEntities.push(currentEntity);
    currentEntity = null;
    inNestedMap = null;
  };

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const trimmed = line.trim();

    if (!currentDomain) {
      const domainMatch = /^([a-z_]+):\s*$/.exec(line);
      if (domainMatch) {
        currentDomain = domainMatch[1];
        currentGroup = "";
        pendingComments = [];
      }
      continue;
    }

    if (/^#/.test(line)) {
      finalizeEntity();
      continue;
    }

    const domainMatch = /^([a-z_]+):\s*$/.exec(line);
    if (domainMatch) {
      finalizeEntity();
      currentDomain = domainMatch[1];
      currentGroup = "";
      pendingComments = [];
      continue;
    }

    const groupMatch = /^\s*#\s*---\s*(.+?)\s*---\s*$/.exec(line);
    if (groupMatch) {
      finalizeEntity();
      currentGroup = groupMatch[1].trim();
      pendingComments = [];
      continue;
    }

    if (trimmed === "") {
      if (currentEntity) finalizeEntity();
      continue;
    }

    if (trimmed.startsWith("#")) {
      // The tag line stays in the raw lines (an export keeps it) but is no note.
      if (TAG_LINE.test(line) && !currentEntity) {
        pendingComments.push(line);
        continue;
      }
      const cleanComment = trimmed.replace(/^#\s?/, "");
      if (currentEntity) {
        currentEntity.rawLines.push(line);
        currentEntity.notes.push(cleanComment);
      } else {
        pendingComments.push(line);
      }
      continue;
    }

    // Catalog entities start with "- platform:", GFA companion overrides with
    // "- id: !extend ...".
    if (/^\s*-\s+(platform|id):\s*/.test(line)) {
      finalizeEntity();
      const tagLine = pendingComments.map((c) => TAG_LINE.exec(c)).find(Boolean);
      const tags = tagLine ? sortTags(parseTagLine(tagLine[1])) : [];
      const preNotes = pendingComments.filter((c) => !TAG_LINE.test(c)).map((c) => c.trim().replace(/^#\s?/, ""));
      currentEntity = {
        index: parsedEntities.length,
        selected: false,
        component: currentDomain,
        group: currentGroup,
        preComments: [...pendingComments],
        rawLines: [line],
        tags,
        props: {
          component: currentDomain,
          group: currentGroup,
          _tags: tags.join(" ")
        },
        mapName: null,
        mapEntries: [],
        filterEntries: [],
        definesAnchor: null,
        anchorBlockLines: [],
        usesAnchor: null,
        notes: [...preNotes],
        originalProps: {},
        modifiedProps: {}
      };
      const first = /^\s*-\s+([a-zA-Z0-9_]+):\s*(.*)$/.exec(line);
      if (first[1] !== "platform") currentEntity.props[first[1]] = unquoteYaml(splitValAndComment(first[2]).value);
      pendingComments = [];
      inNestedMap = null;
      continue;
    }

    if (!currentEntity) continue;
    currentEntity.rawLines.push(line);

    // A block list (`filters:` and its `- multiply: 0.1` items) is not a map of
    // options or codes; it gets its own column.
    if (inNestedMap && /^\s{6,}-\s+/.test(line)) {
      const { value: item } = splitValAndComment(line.trim().replace(/^-\s+/, ""));
      currentEntity.filterEntries.push(inNestedMap === "filters" ? item : inNestedMap + ": " + item);
      continue;
    }

    if (inNestedMap && /^\s{6,}([^:]+):\s*(.*)$/.test(line)) {
      const m = /^\s{6,}([^:]+):\s*(.*)$/.exec(line);
      const mk = m[1].trim();
      const { value: mv } = splitValAndComment(m[2]);
      currentEntity.mapEntries.push({ key: mk, val: unquoteYaml(mv) });
      if (currentEntity.definesAnchor && inNestedMap === currentEntity.anchorKey) {
        currentEntity.anchorBlockLines.push(line);
      }
      continue;
    } else {
      inNestedMap = null;
    }

    const propMatch = /^\s{4}([a-zA-Z0-9_]+):\s*(.*)$/.exec(line);
    if (propMatch) {
      const key = propMatch[1];
      const rawRest = propMatch[2];
      const { value: valPart, comment: inlineComm } = splitValAndComment(rawRest);
      if (inlineComm) {
        currentEntity.notes.push(key + ": " + inlineComm);
      }

      if (valPart === "" || valPart.startsWith("&") || valPart.startsWith("*")) {
        // mapName names the alias line export expands; a later `filters:` block
        // must not take it over.
        if (key !== "filters") currentEntity.mapName = key;
        if (valPart.startsWith("&")) {
          const anchorName = valPart.slice(1).trim();
          currentEntity.definesAnchor = anchorName;
          currentEntity.anchorKey = key;
          currentEntity.anchorBlockLines = [line];
          anchors[anchorName] = currentEntity;
          inNestedMap = key;
        } else if (valPart.startsWith("*")) {
          currentEntity.usesAnchor = valPart.slice(1).trim();
        } else {
          inNestedMap = key;
        }
      } else {
        currentEntity.props[key] = unquoteYaml(valPart);
      }
    }
  }

  finalizeEntity();
  return { entities: parsedEntities, anchors };
}

// Generates a new ESPHome YAML containing only the selected entities,
// omitting `disabled_by_default: true` (except on action buttons) and
// promoting YAML anchors if needed.
function buildExportYaml(allEntities, anchorRegistry, meta = {}) {
  const selected = allEntities.filter((e) => e.selected).sort((a, b) => a.index - b.index);
  if (selected.length === 0) return "";

  const fileLabel = meta.file || "local YAML";
  const langLabel = meta.lang || "de";
  const actionCount = selected.filter(isActionEntity).length;

  const out = [
    "# ============================================================",
    `# Custom vitohome catalog export from: ${fileLabel} (${langLabel})`,
    `# Selected entities: ${selected.length} / ${allEntities.length}`,
    "# Note: 'disabled_by_default: true' has been omitted so all",
    "# selected entities are enabled by default in Home Assistant."
  ];
  if (actionCount > 0) {
    out.push(
      `# Except: ${actionCount} action button(s) (type: action) keep it. Pressing`,
      "# one RUNS a procedure on the controller -- some clear a history, some",
      "# reset settings or codings. Enable each in Home Assistant only once you",
      "# know what it does."
    );
  }
  out.push("# ============================================================", "");

  const byDomain = new Map();
  for (const ent of selected) {
    if (!byDomain.has(ent.component)) byDomain.set(ent.component, []);
    byDomain.get(ent.component).push(ent);
  }

  const emittedAnchors = new Set();

  for (const [domain, domainEntities] of byDomain.entries()) {
    out.push(`${domain}:`);
    let lastGroup = null;

    for (const ent of domainEntities) {
      if (ent.group && ent.group !== lastGroup) {
        out.push(`  # --- ${ent.group} ---`);
        lastGroup = ent.group;
      }

      for (const pre of ent.preComments) {
        out.push(pre);
      }

      if (ent.definesAnchor) {
        emittedAnchors.add(ent.definesAnchor);
      }

      const updatedKeys = new Set();
      const keepDisabled = isActionEntity(ent);
      for (const rawLine of ent.rawLines) {
        if (!keepDisabled && /^\s*disabled_by_default:\s*true\b/i.test(rawLine)) {
          continue;
        }

        if (ent.usesAnchor && !emittedAnchors.has(ent.usesAnchor)) {
          const aliasLineRegex = new RegExp(`^\\s{4}${ent.mapName}:\\s*\\*${ent.usesAnchor}\\b`);
          if (aliasLineRegex.test(rawLine)) {
            const sourceAnchorEnt = anchorRegistry && anchorRegistry[ent.usesAnchor];
            if (sourceAnchorEnt && sourceAnchorEnt.anchorBlockLines.length > 0) {
              for (const bLine of sourceAnchorEnt.anchorBlockLines) {
                out.push(bLine);
              }
              emittedAnchors.add(ent.usesAnchor);
              continue;
            }
          }
        }

        const propMatch = /^(\s{4})([a-zA-Z0-9_]+):(\s*)(.*)$/.exec(rawLine);
        if (propMatch) {
          const propKey = propMatch[2];
          if (Object.prototype.hasOwnProperty.call(ent.modifiedProps, propKey)) {
            updatedKeys.add(propKey);
            const newVal = ent.modifiedProps[propKey];
            if (newVal === "") continue;
            const formattedVal = propKey === "name"
              ? `"${newVal.replace(/[\\"]/g, "\\$&")}"`
              : newVal;
            out.push(`${propMatch[1]}${propKey}: ${formattedVal}`);
            continue;
          }
        }

        out.push(rawLine);
      }

      for (const [modKey, modVal] of Object.entries(ent.modifiedProps)) {
        if (!updatedKeys.has(modKey) && modVal !== "") {
          out.push(`    ${modKey}: ${modVal}`);
        }
      }

      out.push("");
    }
  }

  return out.join("\n").trimEnd() + "\n";
}

// Only catalog names ("v200kw1.yaml", "gfa/chip_20.yaml") and the two
// languages are ever fetched or linked, wherever the value came from (URL
// parameter, dropdown, stored preference).
function catalogFileOrEmpty(name) {
  const s = String(name || "");
  return /^(gfa\/)?[A-Za-z0-9_.-]+\.ya?ml$/.test(s) && !s.includes("..") ? s : "";
}

function langOrNull(lang) {
  return lang === "de" || lang === "en" ? lang : null;
}

// "<lang>/<file>" below example/catalogs/, each segment URL-encoded.
function catalogPath(lang, file) {
  return [lang, ...file.split("/")].map(encodeURIComponent).join("/");
}

function initEditorUI() {
  const $ = (id) => document.getElementById(id);

  let columns = [...BASE_COLUMNS];
  let activeColumns = [];
  let entities = [];
  let filteredEntities = [];
  let anchorRegistry = {};
  let resolveData = null;
  let currentFile = "";
  let currentLang = "de";
  let currentGfa = "";
  let localFile = false;
  let loadSeq = 0;
  let sortCol = null;
  let sortAsc = true;
  let lastCheckedIndex = null;
  let columnFilters = {};
  let activeTags = new Set(); // tag chips switched on: an entity must carry all of them

  function initParams() {
    const params = new URLSearchParams(location.search);
    const askedLang = langOrNull(params.get("lang"));
    if (askedLang) {
      currentLang = askedLang;
    } else {
      try {
        currentLang = langOrNull(localStorage.getItem("vitohome-lang")) || currentLang;
      } catch (e) { /* ignore */ }
    }
    $("lang-select").value = currentLang;
    currentFile = catalogFileOrEmpty(params.get("file"));
    currentGfa = (params.get("gfa") || "").toUpperCase();
  }

  async function loadResolveJson() {
    for (const url of ["resolve.json", "../example/catalogs/resolve.json", RAW_BASE + "resolve.json"]) {
      try {
        const r = await fetch(url);
        if (r.ok) {
          resolveData = await r.json();
          populateCatalogDropdown();
          updateGfaBanner();
          return;
        }
      } catch (e) { /* try next */ }
    }
  }

  function populateCatalogDropdown() {
    if (!resolveData || !resolveData.tokens) return;
    const sel = $("catalog-select");
    sel.replaceChildren(new Option("— Select catalog —", ""));

    const mainGroup = document.createElement("optgroup");
    mainGroup.label = "Controller Catalogs";
    const seenFiles = new Set();
    const entries = Object.entries(resolveData.tokens)
      .filter(([, info]) => info.status === "ok" && info.file)
      .sort((a, b) => a[0].localeCompare(b[0]));

    for (const [token, info] of entries) {
      if (seenFiles.has(info.file)) continue;
      seenFiles.add(info.file);
      const opt = new Option(`${token}${info.short ? " — " + info.short : ""} (${info.file})`, info.file);
      if (info.product) opt.title = info.product;
      mainGroup.append(opt);
    }
    sel.append(mainGroup);

    if (resolveData.gfa_chips) {
      const gfaGroup = document.createElement("optgroup");
      gfaGroup.label = "GFA Burner-Control Companions";
      for (const [chip, name] of Object.entries(resolveData.gfa_chips).sort((a, b) => a[0].localeCompare(b[0]))) {
        const file = `gfa/chip_${chip}.yaml`;
        const opt = new Option(`GFA 0x${chip} — ${name} (${file})`, file);
        gfaGroup.append(opt);
      }
      sel.append(gfaGroup);
    }

    if (currentFile) sel.value = currentFile;
  }

  function updateGfaBanner() {
    const link = $("gfa-companion-link");
    link.classList.add("hidden");
    if (!resolveData || !resolveData.tokens || !currentFile || localFile) return;

    const matchingToken = Object.values(resolveData.tokens).find((t) => t.file === currentFile);
    if (!matchingToken || !matchingToken.gfa) return;

    if (currentGfa && resolveData.gfa_chips && resolveData.gfa_chips[currentGfa]) {
      const gfaFile = `gfa/chip_${currentGfa}.yaml`;
      link.textContent = `GFA 0x${currentGfa} (${resolveData.gfa_chips[currentGfa]}) companion →`;
      link.href = `editor.html?lang=${encodeURIComponent(currentLang)}&file=${encodeURIComponent(gfaFile)}`;
      link.target = "_blank";
      link.classList.remove("hidden");
    } else {
      link.textContent = "Unit uses GFA fault companion (select GFA chip in dropdown)";
      link.href = "#catalog-select";
      link.target = "_self";
      link.classList.remove("hidden");
    }
  }

  function discoverColumns(parsedEntities) {
    const knownKeys = new Set(BASE_COLUMNS.map((c) => c.key));
    knownKeys.add("platform");
    const dynamicCols = [];
    for (const ent of parsedEntities) {
      for (const k of Object.keys(ent.props)) {
        if (!knownKeys.has(k)) {
          knownKeys.add(k);
          dynamicCols.push({ key: k, label: k, mono: true });
        }
      }
    }
    columns = [...BASE_COLUMNS, ...dynamicCols];
  }

  function computeActiveColumns() {
    const showEmpty = $("show-empty-cols").checked;
    if (showEmpty) {
      activeColumns = [...columns];
    } else {
      activeColumns = columns.filter((col) =>
        entities.some((e) => e.props[col.key] !== undefined && e.props[col.key] !== "")
      );
    }
  }

  function parseSortableValue(val, col) {
    if (val === undefined || val === null || val === "") return null;
    if (col.numericHex) {
      const n = parseInt(String(val), 16);
      return Number.isNaN(n) ? String(val).toLowerCase() : n;
    }
    if (col.interval) {
      const m = /^(\d+(?:\.\d+)?)\s*(ms|s|min|h)?$/i.exec(String(val).trim());
      if (m) {
        const num = parseFloat(m[1]);
        const unit = (m[2] || "s").toLowerCase();
        const mult = unit === "ms" ? 0.001 : unit === "min" ? 60 : unit === "h" ? 3600 : 1;
        return num * mult;
      }
    }
    if (col.numeric) {
      const n = parseFloat(String(val));
      return Number.isNaN(n) ? String(val).toLowerCase() : n;
    }
    return String(val).toLowerCase();
  }

  function matchesColumnFilter(cellVal, filterRaw) {
    const f = filterRaw.trim();
    if (!f) return true;
    const str = String(cellVal ?? "").toLowerCase();
    if (f === "*") return str.length > 0;
    if (f === "!" || f.toLowerCase() === "empty") return str.length === 0;
    if (f.startsWith("!") && f.length > 1) {
      return !str.includes(f.slice(1).trim().toLowerCase());
    }
    if (f.startsWith("=") && f.length > 1) {
      return str === f.slice(1).trim().toLowerCase();
    }
    return str.includes(f.toLowerCase());
  }

  function applyFiltersAndSort() {
    const globalQuery = $("global-search").value.trim().toLowerCase();
    const globalTerms = globalQuery ? globalQuery.split(/\s+/) : [];
    const selFilter = columnFilters._selected || "all";

    filteredEntities = entities.filter((ent) => {
      if (selFilter === "selected" && !ent.selected) return false;
      if (selFilter === "unselected" && ent.selected) return false;
      for (const tag of activeTags) {
        if (!(ent.tags || []).includes(tag)) return false;
      }

      for (const col of activeColumns) {
        const cf = columnFilters[col.key];
        if (cf && !matchesColumnFilter(ent.props[col.key], cf)) {
          return false;
        }
      }

      if (globalTerms.length > 0) {
        const hay = activeColumns.map((c) => ent.props[c.key] || "").join(" ").toLowerCase();
        for (const term of globalTerms) {
          if (!hay.includes(term)) return false;
        }
      }
      return true;
    });

    if (sortCol) {
      const colDef = activeColumns.find((c) => c.key === sortCol);
      if (colDef) {
        filteredEntities.sort((a, b) => {
          const va = parseSortableValue(a.props[sortCol], colDef);
          const vb = parseSortableValue(b.props[sortCol], colDef);
          if (va === null && vb === null) return a.index - b.index;
          if (va === null) return 1;
          if (vb === null) return -1;
          if (va < vb) return sortAsc ? -1 : 1;
          if (va > vb) return sortAsc ? 1 : -1;
          return a.index - b.index;
        });
      }
    } else {
      filteredEntities.sort((a, b) => a.index - b.index);
    }
  }

  function buildTableHeader() {
    const headRow = $("header-row");
    const filterRow = $("filter-row");
    headRow.replaceChildren();
    filterRow.replaceChildren();

    const thCheck = document.createElement("th");
    thCheck.className = "sticky-col";
    const masterCb = document.createElement("input");
    masterCb.type = "checkbox";
    masterCb.id = "master-checkbox";
    masterCb.title = "Select / deselect all visible rows";
    masterCb.addEventListener("change", () => {
      for (const ent of filteredEntities) ent.selected = masterCb.checked;
      renderRows();
      updateCounters();
    });
    thCheck.append(masterCb);
    headRow.append(thCheck);

    const thCheckFilter = document.createElement("th");
    thCheckFilter.className = "sticky-col";
    const selSelect = document.createElement("select");
    selSelect.className = "col-filter";
    selSelect.style.minWidth = "3.4rem";
    selSelect.title = "Filter by selection state";
    for (const [val, text] of [["all", "All"], ["selected", "✓"], ["unselected", "☐"]]) {
      const opt = document.createElement("option");
      opt.value = val;
      opt.textContent = text;
      selSelect.append(opt);
    }
    selSelect.value = columnFilters._selected || "all";
    selSelect.addEventListener("change", () => {
      columnFilters._selected = selSelect.value;
      refreshTableBody();
    });
    thCheckFilter.append(selSelect);
    filterRow.append(thCheckFilter);

    for (const col of activeColumns) {
      const th = document.createElement("th");
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "sort-btn";
      const labelSpan = document.createElement("span");
      labelSpan.textContent = col.label;
      const indSpan = document.createElement("span");
      indSpan.className = "sort-ind";
      indSpan.textContent = sortCol === col.key ? (sortAsc ? "▲" : "▼") : "";
      btn.append(labelSpan, indSpan);
      btn.addEventListener("click", () => {
        if (sortCol === col.key) {
          if (!sortAsc) { sortCol = null; sortAsc = true; }
          else { sortAsc = false; }
        } else {
          sortCol = col.key;
          sortAsc = true;
        }
        buildTableHeader();
        refreshTableBody();
      });
      th.append(btn);
      headRow.append(th);

      const fth = document.createElement("th");
      const input = document.createElement("input");
      input.type = "search";
      input.className = "col-filter";
      input.placeholder = "Filter…";
      input.title = "Substring match, =exact, !exclude, * (non-empty), empty";
      input.value = columnFilters[col.key] || "";

      if (!col.isMap && col.key !== "_notes" && col.key !== "name" && col.key !== "id") {
        const uniqueVals = [...new Set(entities.map((e) => e.props[col.key]).filter((v) => v !== undefined && v !== ""))];
        if (uniqueVals.length > 0 && uniqueVals.length <= 80) {
          const listId = "dl-" + col.key;
          input.setAttribute("list", listId);
          const dl = document.createElement("datalist");
          dl.id = listId;
          for (const u of uniqueVals.sort()) {
            const opt = document.createElement("option");
            opt.value = u;
            dl.append(opt);
          }
          fth.append(dl);
        }
      }

      input.addEventListener("input", () => {
        columnFilters[col.key] = input.value;
        refreshTableBody();
      });
      fth.append(input);
      filterRow.append(fth);
    }
  }

  function createBadge(cls, text) {
    const b = document.createElement("span");
    b.className = "badge " + cls;
    b.textContent = text;
    return b;
  }

  function renderCellContent(ent, col) {
    const val = ent.props[col.key];
    if (col.editable && (val !== undefined || col.key === "update_interval")) {
      const inp = document.createElement("input");
      inp.type = "text";
      inp.className = "cell-edit" + (ent.modifiedProps[col.key] !== undefined ? " modified" : "");
      inp.value = val ?? "";
      inp.placeholder = "—";
      inp.addEventListener("change", () => {
        if (!Object.prototype.hasOwnProperty.call(ent.originalProps, col.key)) {
          ent.originalProps[col.key] = ent.props[col.key];
        }
        const original = ent.originalProps[col.key];
        let nextVal = inp.value.trim();
        // ESPHome needs the name: clearing it restores the catalog's one.
        if (col.key === "name" && nextVal === "") nextVal = original ?? "";
        inp.value = nextVal;
        ent.props[col.key] = nextVal;
        if (nextVal === (original ?? "")) {
          delete ent.modifiedProps[col.key];
        } else {
          ent.modifiedProps[col.key] = nextVal;
        }
        inp.classList.toggle("modified", ent.modifiedProps[col.key] !== undefined);
      });
      return inp;
    }

    if (val === undefined || val === "") return "";

    if (col.key === "component") {
      return createBadge("muted-badge", val);
    }
    if (col.key === "disabled_by_default") {
      if (isActionEntity(ent)) {
        const kept = createBadge("ok", val + " (kept on export)");
        kept.title = "An action button stays disabled by default: pressing it runs a procedure on the controller";
        return kept;
      }
      const span = document.createElement("span");
      span.className = "badge warn";
      span.title = "Will be omitted when exported so the entity is enabled by default";
      span.textContent = val + " (omitted on export)";
      return span;
    }
    if (col.key === "type" && isActionEntity(ent)) {
      const badge = createBadge("stop", "action · runs a procedure");
      badge.title = "Pressing this button RUNS a procedure on the controller (Vitosoft Remote_Procedure_Call), " +
        "at once and without asking. Some clear a history, some reset settings or codings.";
      return badge;
    }
    if (col.isMap) {
      const lines = String(val).split("\n");
      if (lines.length <= 2) {
        const div = document.createElement("div");
        div.className = "note-text";
        div.style.fontFamily = "var(--mono)";
        div.textContent = val;
        return div;
      }
      const details = document.createElement("details");
      details.className = "map-details";
      const summary = document.createElement("summary");
      summary.textContent = lines[0].startsWith("*")
        ? lines[0]
        : `${lines.length} items (${lines[0].slice(0, 28)}…)`;
      const pre = document.createElement("div");
      pre.className = "map-list";
      pre.textContent = val;
      details.append(summary, pre);
      return details;
    }
    if (col.key === "_notes") {
      const div = document.createElement("div");
      div.className = "note-text";
      div.textContent = val;
      return div;
    }
    return document.createTextNode(val);
  }

  function renderRows() {
    const tbody = $("table-body");
    tbody.replaceChildren();
    const frag = document.createDocumentFragment();

    filteredEntities.forEach((ent, visibleIdx) => {
      const tr = document.createElement("tr");
      if (ent.selected) tr.classList.add("selected");

      const tdCheck = document.createElement("td");
      tdCheck.className = "sticky-col";
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = ent.selected;
      cb.addEventListener("click", (ev) => {
        if (ev.shiftKey && lastCheckedIndex !== null) {
          const start = Math.min(lastCheckedIndex, visibleIdx);
          const end = Math.max(lastCheckedIndex, visibleIdx);
          for (let i = start; i <= end; i++) {
            filteredEntities[i].selected = cb.checked;
          }
          lastCheckedIndex = visibleIdx;
          renderRows();
          updateCounters();
          return;
        }
        lastCheckedIndex = visibleIdx;
        ent.selected = cb.checked;
        tr.classList.toggle("selected", ent.selected);
        updateCounters();
      });
      tdCheck.append(cb);
      tr.append(tdCheck);

      for (const col of activeColumns) {
        const td = document.createElement("td");
        if (col.mono) td.className = "mono";
        td.append(renderCellContent(ent, col));
        tr.append(td);
      }
      frag.append(tr);
    });

    tbody.append(frag);
  }

  function updateCounters() {
    const total = entities.length;
    const shown = filteredEntities.length;
    const selCount = entities.filter((e) => e.selected).length;

    $("stats-badge").textContent = `${shown} / ${total} shown`;
    $("selected-badge").textContent = `${selCount} selected`;

    const hasSel = selCount > 0;
    $("preview-export-btn").disabled = !hasSel;
    $("download-export-btn").disabled = !hasSel;

    const masterCb = $("master-checkbox");
    if (masterCb) {
      const visibleSel = filteredEntities.filter((e) => e.selected).length;
      masterCb.checked = shown > 0 && visibleSel === shown;
      masterCb.indeterminate = visibleSel > 0 && visibleSel < shown;
    }
  }

  function refreshTableBody() {
    lastCheckedIndex = null; // shift-click ranges refer to the previous row order
    applyFiltersAndSort();
    renderRows();
    updateCounters();
  }

  function downloadYaml(content) {
    const baseName = (currentFile || "catalog.yaml").replace(/\.ya?ml$/i, "").replace(/\//g, "_");
    const downloadName = `${baseName}_selected.yaml`;
    const blob = new Blob([content], { type: "text/yaml;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = downloadName;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }

  function showStatus(msg, isError = false) {
    const box = $("status-box");
    box.textContent = msg;
    box.classList.remove("hidden");
    box.style.borderColor = isError ? "var(--stop)" : "var(--line)";
  }

  // One toggle chip per tag the loaded catalog carries, with its count.
  function buildTagChips() {
    const box = $("tag-chips");
    const counts = new Map();
    for (const ent of entities) for (const tag of ent.tags || []) counts.set(tag, (counts.get(tag) || 0) + 1);
    activeTags = new Set([...activeTags].filter((t) => counts.has(t)));
    box.replaceChildren();
    box.classList.toggle("hidden", counts.size === 0);
    if (counts.size === 0) return;
    const caption = document.createElement("span");
    caption.className = "tag-caption";
    caption.textContent = "Vitosoft tags:";
    caption.title = "From the catalog's '# vito:' lines: the circuit an entity's menu belongs to, " +
      "Vitosoft's overview screen, quick check and trend recorder, and the service access level. " +
      "Switched-on chips must all match.";
    box.append(caption);
    for (const tag of sortTags([...counts.keys()])) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "tag-chip" + (activeTags.has(tag) ? " active" : "");
      chip.setAttribute("aria-pressed", activeTags.has(tag) ? "true" : "false");
      chip.textContent = `${TAG_LABELS[tag] || tag} (${counts.get(tag)})`;
      chip.addEventListener("click", () => {
        if (activeTags.has(tag)) activeTags.delete(tag); else activeTags.add(tag);
        buildTagChips();
        refreshTableBody();
      });
      box.append(chip);
    }
  }

  function loadYamlContent(yamlText, label) {
    const parsed = parseCatalogYaml(yamlText);
    entities = parsed.entities;
    anchorRegistry = parsed.anchors;
    buildTagChips();

    $("catalog-title").textContent = label;
    document.title = `${label} — vitohome catalog editor`;
    updateGfaBanner();

    discoverColumns(entities);
    computeActiveColumns();
    $("status-box").classList.add("hidden");
    $("table-container").classList.remove("hidden");

    buildTableHeader();
    refreshTableBody();
  }

  async function fetchCatalog() {
    if (!currentFile) {
      showStatus("Select a catalog from the dropdown above or use 'Load local YAML' to inspect and export datapoints.");
      $("catalog-title").textContent = "No catalog selected";
      return;
    }

    localFile = false;
    const seq = ++loadSeq;
    $("catalog-select").value = currentFile;
    const path = catalogPath(currentLang, currentFile);
    const rawUrl = RAW_BASE + path;
    $("raw-link").href = rawUrl;
    showStatus(`Loading example/catalogs/${currentLang}/${currentFile}…`);

    const candidates = [
      `catalogs/${path}`,
      `../example/catalogs/${path}`,
      rawUrl
    ];

    for (const url of candidates) {
      try {
        const resp = await fetch(url);
        if (resp.ok) {
          const text = await resp.text();
          if (seq !== loadSeq) return; // a newer selection superseded this one
          loadYamlContent(text, `example/catalogs/${currentLang}/${currentFile}`);
          return;
        }
      } catch (err) { /* try next candidate */ }
    }

    if (seq !== loadSeq) return;
    showStatus(`Could not load catalog '${currentFile}' (${currentLang}). You can load it manually via 'Load local YAML'.`, true);
  }

  $("global-search").addEventListener("input", refreshTableBody);

  $("show-empty-cols").addEventListener("change", () => {
    computeActiveColumns();
    buildTableHeader();
    refreshTableBody();
  });

  $("clear-filters-btn").addEventListener("click", () => {
    $("global-search").value = "";
    columnFilters = {};
    activeTags = new Set();
    buildTagChips();
    buildTableHeader();
    refreshTableBody();
  });

  $("select-filtered-btn").addEventListener("click", () => {
    for (const ent of filteredEntities) ent.selected = true;
    renderRows();
    updateCounters();
  });

  $("deselect-filtered-btn").addEventListener("click", () => {
    for (const ent of filteredEntities) ent.selected = false;
    renderRows();
    updateCounters();
  });

  $("invert-filtered-btn").addEventListener("click", () => {
    for (const ent of filteredEntities) ent.selected = !ent.selected;
    renderRows();
    updateCounters();
  });

  $("preview-export-btn").addEventListener("click", () => {
    const yaml = buildExportYaml(entities, anchorRegistry, { file: currentFile, lang: currentLang });
    $("yaml-preview").value = yaml;
    $("export-dialog").showModal();
  });

  $("download-export-btn").addEventListener("click", () => {
    const yaml = buildExportYaml(entities, anchorRegistry, { file: currentFile, lang: currentLang });
    if (yaml) downloadYaml(yaml);
  });

  $("modal-download-btn").addEventListener("click", () => {
    const yaml = $("yaml-preview").value;
    if (yaml) downloadYaml(yaml);
  });

  $("copy-yaml-btn").addEventListener("click", () => {
    const btn = $("copy-yaml-btn");
    navigator.clipboard.writeText($("yaml-preview").value).then(
      () => { btn.textContent = "Copied!"; setTimeout(() => { btn.textContent = "Copy to Clipboard"; }, 1500); },
      () => { btn.textContent = "Copy failed"; }
    );
  });

  $("close-modal-btn").addEventListener("click", () => {
    $("export-dialog").close();
  });

  $("catalog-select").addEventListener("change", () => {
    const picked = catalogFileOrEmpty($("catalog-select").value);
    if (!picked) return;
    currentFile = picked;
    const url = new URL(location.href);
    url.searchParams.set("file", currentFile);
    history.replaceState(null, "", url);
    fetchCatalog();
  });

  $("lang-select").addEventListener("change", () => {
    const picked = langOrNull($("lang-select").value);
    if (!picked) return;
    currentLang = picked;
    try { localStorage.setItem("vitohome-lang", currentLang); } catch (e) { /* ignore */ }
    const url = new URL(location.href);
    url.searchParams.set("lang", currentLang);
    history.replaceState(null, "", url);
    if (currentFile && !localFile) fetchCatalog();
  });

  $("file-input").addEventListener("change", (ev) => {
    const file = ev.target.files && ev.target.files[0];
    if (!file) return;
    const seq = ++loadSeq;
    const reader = new FileReader();
    reader.onload = () => {
      if (seq !== loadSeq) return;
      currentFile = file.name;
      localFile = true;
      $("catalog-select").value = "";
      $("raw-link").removeAttribute("href");
      const url = new URL(location.href);
      url.searchParams.delete("file");
      history.replaceState(null, "", url);
      loadYamlContent(String(reader.result || ""), file.name);
    };
    reader.readAsText(file);
    ev.target.value = ""; // allow re-loading the same file after edits on disk
  });

  initParams();
  loadResolveJson();
  fetchCatalog();
}

if (typeof document !== "undefined") {
  initEditorUI();
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = { parseCatalogYaml, buildExportYaml };
}
