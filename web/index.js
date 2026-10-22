// UI logic for the vitohome catalog finder (web/index.html).
"use strict";

const REPO = "https://github.com/SoulSolistice/esphome_vitohome";
const RAW = "https://raw.githubusercontent.com/SoulSolistice/esphome_vitohome/main/example/catalogs/";
// Catalogs are committed per language under example/catalogs/<lang>/ (same
// file names in both); resolve.json is language-independent and sits above them.
const LANGS = ["de", "en"];
let lang = pickLanguage();
const catalogPath = (file) => "example/catalogs/" + lang + "/" + file;

const $ = (id) => document.getElementById(id);
const fields = { id: $("id"), hw: $("hw"), sw: $("sw"), f0: $("f0"), gfa: $("gfa") };
// F0 is not a hex byte like the others: f0Value (lookup.js) reads it.
const limits = { id: 0xffff, hw: 0xff, sw: 0xff, gfa: 0xff };
const widths = { id: 4, hw: 2, sw: 2, gfa: 2 };
const readField = (key, text) => key === "f0" ? f0Value(text) : parseHex(text, limits[key]);
let data = null;

function pickLanguage() {
  const asked = new URLSearchParams(location.search).get("lang");
  if (LANGS.includes(asked)) return asked;
  try {
    const kept = localStorage.getItem("vitohome-lang");
    if (LANGS.includes(kept)) return kept;
  } catch (error) { /* storage unavailable: fall through */ }
  return (navigator.language || "").toLowerCase().startsWith("de") ? "de" : "en";
}

function editorUrl(file, token = "") {
  const params = new URLSearchParams({ lang, file });
  if (token) params.set("token", token);
  const gfaVal = parseHex(fields.gfa.value, limits.gfa);
  if (gfaVal !== null) params.set("gfa", gfaVal.toString(16).toUpperCase().padStart(2, "0"));
  return "editor.html?" + params.toString();
}

function el(tag, attrs, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs || {})) {
    if (key === "class") node.className = value; else node.setAttribute(key, value);
  }
  for (const child of children) node.append(child);
  return node;
}

const hexOf = (value, width) => "0x" + value.toString(16).toUpperCase().padStart(width, "0");
const badge = (kind, text) => el("span", { class: "badge " + kind }, text);
const note = (kind, ...children) => el("div", { class: "note " + kind }, ...children);

function openInEditorBtn(file, small = false, token = "") {
  return el("a", {
    class: small ? "btn btn-sm" : "btn btn-accent",
    href: editorUrl(file, token),
    target: "_blank",
    rel: "noopener"
  }, "Open in Editor");
}

function catalogCell(entry) {
  const c = entry.catalog;
  if (c.status === "ok") {
    const frag = document.createDocumentFragment();
    frag.append(
      el("a", { href: REPO + "/blob/main/" + catalogPath(c.file) }, c.file),
      openInEditorBtn(c.file, true, entry.token)
    );
    return frag;
  }
  if (c.status === "missing") return "not in the repository";
  if (c.status === "skipped") return "none: " + c.detail.replace(/^skipped:\s*/, "");
  return c.detail;
}

function tokenTable(entries, picked, withF0) {
  const head = el("tr", {}, el("th", {}, "Vitosoft token"));
  if (withF0) head.append(el("th", {}, "F0 range"));
  head.append(el("th", {}, "Catalog"));
  const table = el("table", {}, el("thead", {}, head));
  const body = el("tbody");
  for (const entry of entries) {
    const row = el("tr", entry.token === picked ? { class: "picked" } : {}, el("td", { class: "mono" }, entry.token));
    if (withF0) {
      const f0 = entry.f0;
      // Decimal, as Vitosoft lists F0 (the _30 of VScotHO1_200_30).
      row.append(el("td", { class: "mono" }, !f0 ? "any" : f0[0] === f0[1] ? String(f0[0]) : f0[0] + "–" + f0[1]));
    }
    row.append(el("td", {}, catalogCell(entry)));
    body.append(row);
  }
  table.append(body);
  return table;
}

// The GFA companion for a catalog with a burner-control fault history:
// { chip, name, file } when the entered chip has texts, { chip: null } when no
// chip is entered, { chip, unknown: true } when the chip has none.
function gfaChoice(c, gfa) {
  if (!c.gfa) return null;
  if (gfa === null || gfa === undefined) return { chip: null };
  const chip = gfa.toString(16).toUpperCase().padStart(2, "0");
  const chips = (data && data.gfa_chips) || {};
  if (!Object.prototype.hasOwnProperty.call(chips, chip)) return { chip, unknown: true };
  return { chip, name: chips[chip], file: "gfa/chip_" + chip + ".yaml" };
}

function gfaNote(choice) {
  const chips = (data && data.gfa_chips) || {};
  const known = Object.keys(chips).sort().map((k) => k + (chips[k] ? " " + chips[k] : "")).join(", ");
  if (choice.chip === null) {
    return note("", el("p", {},
      "This unit has a burner-control (GFA) fault history. Its texts depend on the burner-control chip, not on the device type. The hub logs it at boot as ",
      el("code", {}, "GFA=0x…"), " in the identification line and adds it to the finder link; paste that line, or enter the chip, to add the matching companion. Known chips: ", known, "."));
  }
  if (choice.unknown) {
    return note("warn-note", el("p", {}, "Vitosoft has no fault texts for GFA chip ", el("code", {}, choice.chip),
      "; the GFA history shows raw codes. Known chips: ", known, "."));
  }
  return note("", el("p", {}, "GFA chip ", el("code", {}, choice.chip), choice.name ? " (" + choice.name + ")" : "",
    ": the companion ", el("a", { href: REPO + "/blob/main/" + catalogPath(choice.file) }, el("code", {}, catalogPath(choice.file))),
    " ", openInEditorBtn(choice.file, true),
    " adds its fault texts to the catalog's GFA history. Save it in ", el("code", {}, "catalogs/gfa/"), "."));
}

function catalogBlock(entry) {
  const c = entry.catalog;
  const out = document.createDocumentFragment();
  if (c.status === "ok") {
    const gfa = gfaChoice(c, parseHex(fields.gfa.value, limits.gfa));
    let snippet = "packages:\n  heater: !include catalogs/" + c.file + "\n";
    if (gfa && gfa.file) snippet += "  gfa: !include catalogs/" + gfa.file + "\n";
    const copy = el("button", { type: "button" }, "Copy");
    copy.addEventListener("click", () => {
      navigator.clipboard.writeText(snippet).then(() => { copy.textContent = "Copied"; }, () => { copy.textContent = "Copy failed"; });
    });
    out.append(
      el("div", { class: "catalog-line" },
        el("span", {}, "Catalog file: ",
          el("a", { href: REPO + "/blob/main/" + catalogPath(c.file) }, el("code", {}, catalogPath(c.file))),
          " (", el("a", { href: RAW + lang + "/" + c.file, download: c.file }, "raw"), ")"
        ),
        openInEditorBtn(c.file, false, entry.token)
      ),
      el("p", {}, "Save it next to your device YAML in a ", el("code", {}, "catalogs/"), " folder and include it as a package:"),
      el("pre", {}, snippet), copy,
      ...(gfa ? [gfaNote(gfa)] : []),
      note("warn-note", el("p", {}, "A full catalog registers every datapoint, including the ones left disabled in Home Assistant. On a small ESP, copy only the entities you need into your own file. Not every address answers on every firmware: verify values on your unit.")),
    );
  } else if (c.status === "missing") {
    out.append(note("stop-note",
      el("p", {}, el("strong", {}, "This catalog is not in the repository.")),
      el("p", {}, "The unit is known, but its catalog file is not committed. Generate it from the Vitosoft export with ",
        el("code", {}, "scripts/gen_catalog.py --export-all"), " (see scripts/README.md).")));
  } else if (c.status === "skipped" && /no Optolink-reachable/.test(c.detail)) {
    out.append(note("stop-note",
      el("p", {}, el("strong", {}, "There is no catalog for this unit.")),
      el("p", {}, "Every datapoint the Vitosoft export lists for it needs an access method the Optolink interface cannot drive.")));
  } else if (c.status === "skipped") {
    out.append(note("stop-note",
      el("p", {}, el("strong", {}, "There is no catalog for this unit"), " (", c.detail, ").")));
  } else {
    out.append(note("stop-note", el("p", {}, "No catalog was generated for this token (", c.detail, ").")));
  }
  return out;
}

function render() {
  const out = $("result");
  out.replaceChildren();
  if (!data) return;
  const v = {};
  for (const key of Object.keys(fields)) v[key] = readField(key, fields[key].value);
  for (const key of Object.keys(fields)) {
    fields[key].setAttribute("aria-invalid", fields[key].value.trim() !== "" && v[key] === null ? "true" : "false");
  }
  if (v.id === null) return;

  const r = lookup(data, v.id, v.hw, v.sw, v.f0);
  const panel = el("div", { class: "panel" });
  out.append(panel);
  const idText = hexOf(v.id, 4);

  if (r.kind === "unknown") {
    panel.append(el("h2", {}, "Unknown device id"),
      el("p", {}, "The Vitosoft export has no device type with identification ", el("code", {}, idText), ". Check the value, or use the raw scanner example to explore the unit."));
    return;
  }
  if (r.kind === "family") {
    if (r.tokens.length === 1) {
      panel.append(el("h2", {}, "Match"), el("div", {}, badge("ok", "Only device type with this id")),
        el("div", { class: "token" }, r.tokens[0].token), catalogBlock(r.tokens[0]));
      return;
    }
    panel.append(el("h2", {}, "Cannot be narrowed down"),
      el("p", {}, r.tokens.length + " device types share identification ", el("code", {}, idText),
        ". They are told apart by a longer identification key, not by the hardware and software index the hub reads."),
      tokenTable(r.tokens, null, false));
    return;
  }
  if (r.kind === "need-sw") {
    panel.append(el("h2", {}, "Software index needed"),
      el("p", {}, "Identification ", el("code", {}, idText), " covers several device types. Enter the SW index to pick one."),
      tokenTable(r.tokens, null, false));
    return;
  }

  panel.append(el("h2", {}, "Match"));
  const badges = el("div");
  const swRange = r.sw[0] === r.sw[1] ? hexOf(r.sw[0], 2) : hexOf(r.sw[0], 2) + "–" + hexOf(r.sw[1], 2);
  if (r.how === "range") badges.append(badge("ok", "SW index inside catalogued range"));
  if (r.how === "floor") badges.append(badge("warn", "SW index not listed: nearest lower revision"));
  if (r.how === "lowest") badges.append(badge("warn", "SW index below all listed: lowest revision"));
  if (r.hwIgnored) badges.append(badge("warn", "HW index not listed: ignored"));
  if (r.f0 === "missing" || r.f0 === "no-match") badges.append(badge("warn", "F0 decides: generic variant shown"));
  if (r.f0 === "matched") badges.append(badge("ok", "F0 matched"));
  panel.append(badges, el("div", { class: "token" }, r.token));

  if (r.how !== "range") {
    panel.append(note("warn-note", el("p", {},
      "Software index ", el("code", {}, hexOf(v.sw, 2)), " (" + v.sw + " decimal) is not listed for this device in the Vitosoft export. ",
      r.how === "floor"
        ? "The nearest lower listed revision is shown, on the assumption that a revision applies from its software index onwards."
        : "It is below every listed revision, so the lowest one is shown.",
      " Treat this as a starting point and verify values on the unit.")));
  }
  if (r.hwIgnored) {
    panel.append(note("warn-note", el("p", {},
      "Hardware index ", el("code", {}, hexOf(v.hw, 2)), " does not occur for this device in the export (listed: ",
      r.hwCatalogued.map((h) => "0x" + h).join(", "), "). The match uses the software index only.")));
  }
  if (r.f0 !== null) {
    panel.append(note(r.f0 === "matched" ? "" : "warn-note",
      el("p", {}, "For software index " + swRange + " this device has several variants, told apart by F0 (Vitosoft: Erweiterte Gerätekennung): the two bytes at ",
        el("code", {}, "0x00F0"), " read as one little-endian number, compared in decimal. The hub logs the raw bytes, e.g. ",
        el("code", {}, "F0=0x1E00"), " for F0 30, the ", el("code", {}, "_30"), " of ", el("code", {}, "VScotHO1_200_30"), ". ",
        r.f0 === "matched"
          ? "F0 " + r.f0Value + " selects the highlighted variant."
          : r.f0 === "no-match" ? "F0 " + r.f0Value + " matches none of the listed ranges; the generic variant is highlighted."
          : "Without it, the generic variant is highlighted."),
      tokenTable([...r.candidates].sort((a, b) => (a.f0 ? a.f0[0] : -1) - (b.f0 ? b.f0[0] : -1)), r.token, true)));
  }
  panel.append(catalogBlock(r));
  if (r.unranged.length && r.how !== "range") {
    panel.append(note("", el("p", {}, "Same device id, but listed without a hardware/software range, so never selected automatically:"),
      tokenTable(r.unranged, null, false)));
  }
}

function fromLine() {
  const parsed = parseIdentification($("line").value);
  for (const key of Object.keys(fields)) {
    if (parsed[key] === null) continue;
    fields[key].value = key === "f0" ? parsed.f0 : parsed[key].toString(16).toUpperCase().padStart(widths[key], "0");
  }
  render();
}

function fromQuery() {
  const params = new URLSearchParams(location.search);
  for (const key of Object.keys(fields)) {
    const value = readField(key, params.get(key));
    if (value === null) continue;
    // F0 keeps its digits: four are the hub's two raw bytes (f0Value).
    fields[key].value = key === "f0" ? params.get(key).trim().replace(/^0x/i, "").toUpperCase()
      : value.toString(16).toUpperCase().padStart(widths[key], "0");
  }
}

async function loadData() {
  for (const url of ["resolve.json", RAW + "resolve.json"]) {
    try {
      const response = await fetch(url);
      if (response.ok) return await response.json();
    } catch (error) { /* try the next source */ }
  }
  return null;
}

for (const radio of document.querySelectorAll('input[name="lang"]')) {
  radio.checked = radio.value === lang;
  radio.addEventListener("change", () => {
    lang = radio.value;
    try { localStorage.setItem("vitohome-lang", lang); } catch (error) { /* not persisted */ }
    render();
  });
}
$("line").addEventListener("input", fromLine);
for (const input of Object.values(fields)) input.addEventListener("input", render);
fromQuery();
loadData().then((loaded) => {
  if (!loaded || loaded.schema !== 2) {
    $("result").replaceChildren(el("div", { class: "panel" }, el("h2", {}, "Lookup data unavailable"),
      el("p", {}, "resolve.json could not be loaded or has an unexpected format.")));
    return;
  }
  data = loaded;
  render();
});
