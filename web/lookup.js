// Catalog finder lookup for esphome_vitohome.
//
// All matching rules live in scripts/gen_catalog.py (Catalog.resolve_pool).
// example/catalogs/resolve.json is that function's answer for every device id,
// hardware index and software index, so this file only reads the table. The
// pytest in tests/unit/test_resolve_index.py runs it under Node and compares
// each answer with Catalog.resolve().
"use strict";

const hex = (value, width) => value.toString(16).toUpperCase().padStart(width, "0");

function parseHex(text, max) {
  if (text === null || text === undefined) return null;
  const match = /^\s*(?:0x)?([0-9a-f]+)\s*$/i.exec(String(text));
  if (!match) return null;
  const value = parseInt(match[1], 16);
  return value <= max ? value : null;
}

// The F0 value the export's F0 ranges are compared against -- the same rule as
// scripts/vitocatalog/identify.py f0_value. Vitosoft reads F0 at 0x00F0 as one
// signed 16-bit little-endian integer and keeps its ranges in decimal (30..39
// is 30..39, not 0x30..0x39). A number is the value itself; four hex digits
// (with or without 0x) are the two raw bytes the hub logs in register order,
// read little-endian (F0=0x1E00, &f0=1E00 -> 30; FF FF -> -1, in no range);
// one to three decimal digits are the value as Vitosoft writes it ("30").
function f0Value(value) {
  if (value === null || value === undefined) return null;
  if (typeof value === "number") {
    return Number.isInteger(value) && value >= -0x8000 && value <= 0x7fff ? value : null;
  }
  const text = String(value).trim();
  const raw = /^(?:0x)?([0-9a-f]{4})$/i.exec(text);
  if (raw) {
    const number = parseInt(raw[1].slice(0, 2), 16) | (parseInt(raw[1].slice(2), 16) << 8);
    return number >= 0x8000 ? number - 0x10000 : number;
  }
  return /^[0-9]{1,3}$/.test(text) ? parseInt(text, 10) : null;
}

// Reads the identification out of anything the hub prints, e.g.
//   "Device: 0x2053 (GWG_VBEM) HW=0x01 SW=0x7A"
// plus an optional "GFA=0x24": the burner-control chip, which selects the GFA
// fault-text companion, and "F0=0x1E00": the raw bytes at 0x00F0. Missing
// fields come back as null; f0 comes back as text -- four hex digits are the
// hub's raw bytes, up to three decimal digits the value itself (f0Value).
function parseIdentification(text) {
  // Prefer the hub's own "Device:" / "Device identification:" line when a whole
  // log was pasted, so an unrelated 0xNNNN address earlier in it is not taken.
  // Without such a line, only a single-line input (a typed "0x20CB HW=03 ...")
  // is read: in a multi-line paste the first 0xNNNN is a register address far
  // more often than a device id.
  const pasted = String(text || "");
  const line = /^.*\bDevice(?: identification)?:.*\b0x[0-9a-f]{4}\b.*$/im.exec(pasted);
  const trimmed = pasted.trim();
  if (!line && /[\r\n]/.test(trimmed)) return { id: null, hw: null, sw: null, f0: null, gfa: null };
  const source = line ? line[0].slice(line[0].search(/\bDevice(?: identification)?:/i)) : trimmed;
  const pick = (pattern, max) => {
    const match = pattern.exec(source);
    return match ? parseHex(match[1], max) : null;
  };
  const f0 = /\bF0\s*[=:]\s*(?:(?:0x)?([0-9a-f]{4})|([0-9]{1,3}))\b/i.exec(source);
  return {
    id: pick(/\b0x([0-9a-f]{4})\b/i, 0xffff),
    hw: pick(/\bHW\s*[=:]\s*(?:0x)?([0-9a-f]{1,2})\b/i, 0xff),
    sw: pick(/\bSW\s*[=:]\s*(?:0x)?([0-9a-f]{1,2})\b/i, 0xff),
    f0: f0 ? (f0[1] || f0[2]).toUpperCase() : null,
    gfa: pick(/\bGFA\s*[=:]\s*(?:0x)?([0-9a-f]{1,2})\b/i, 0xff),
  };
}

function catalogOf(data, token) {
  return data.tokens[token] || { file: "", status: "unknown", detail: "not part of this export" };
}

// Returns one of:
//   { kind: "unknown", ident }
//   { kind: "family", ident, tokens: [{token, catalog}] }
//   { kind: "need-sw", ident, tokens: [{token, catalog}] }
//   { kind: "match", ident, token, catalog, how, hwIgnored, sw: [lo, hi],
//     f0: null | "matched" | "missing" | "no-match", f0Value, candidates, unranged }
// *f0* is the F0 value, its decimal text, or the hub's four raw hex digits
// (f0Value); f0Value is the value compared, null when F0 was not given.
function lookup(data, id, hw, sw, f0) {
  if (id === null || id === undefined) return { kind: "unknown", ident: null };
  const ident = hex(id, 4);
  const entry = data.idents[ident];
  const describe = (token) => ({ token, catalog: catalogOf(data, token) });
  if (!entry) return { kind: "unknown", ident };
  if (entry.family) return { kind: "family", ident, tokens: entry.family.map(describe) };

  const hwKey = hw === null || hw === undefined ? null : hex(hw, 2);
  const hwKnown = hwKey !== null && Object.prototype.hasOwnProperty.call(entry.hw, hwKey);
  const runs = hwKnown ? entry.hw[hwKey] : entry.any;

  if (sw === null || sw === undefined) {
    const seen = new Set();
    for (const run of runs) for (const c of run.candidates || [{ token: run.token }]) seen.add(c.token);
    return { kind: "need-sw", ident, tokens: [...seen].map(describe) };
  }

  const run = runs.find((r) => r.sw[0] <= sw && sw <= r.sw[1]);
  let token = run.token;
  let f0State = null;
  let value = null;
  const candidates = (run.candidates || []).map((c) => ({ ...describe(c.token), f0: c.f0 }));
  if (candidates.some((c) => c.f0 !== null)) {
    f0State = "missing";
    value = f0Value(f0);
    if (value !== null) {
      f0State = "no-match";
      const hit = candidates.find((c) => c.f0 !== null && c.f0[0] <= value && value <= c.f0[1]);
      if (hit) {
        f0State = "matched";
        token = hit.token;
      }
    }
  }
  return {
    kind: "match",
    ident,
    token,
    catalog: catalogOf(data, token),
    how: run.how,
    hwIgnored: hwKey !== null && !hwKnown,
    hwCatalogued: Object.keys(entry.hw),
    sw: run.sw,
    f0: f0State,
    f0Value: value,
    candidates,
    unranged: (entry.unranged || []).map(describe),
  };
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = { lookup, parseIdentification, parseHex, f0Value };
}
