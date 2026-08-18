# Guidelines for ESPHome external components

A working reference for building external components that stay small, stay
within their memory budget, and fail loudly instead of silently.

---

## Scope and provenance — read this first

**Audience.** Authors of ESPHome *external components* (`external_components:`),
particularly components that instantiate many entities from a generated catalog.

**Version.** Every API citation below was checked against the ESPHome **2026.7.0**
source tree, and is given as `path:line` so you can re-verify it. Line numbers
drift between releases; the symbol names are the durable part.

**Where these rules come from.** Four sources, kept distinct on purpose:

1. **The ESPHome core source tree.** Container choices, logging macros and
   codegen helpers are all read directly from core. These are facts about the
   framework.
2. **A structural audit and a heap-allocation rework** of a real component
   (~700 entities from a generated catalog, ESP32 + ESP-IDF). Rules tagged
   *"observed"* below caused actual bugs or measurable waste.
3. **Reasoning from the above.** Marked as such where it applies.
4. **`esphome/device-builder`'s `CLAUDE.md`.** A *downstream consumer* of
   ESPHome — the dashboard backend that generates, validates and compiles user
   configs. It is not a component repository and carries no authority on
   component-authoring convention, but its pitfall list records how components
   look from the outside. Rules tagged *"consumer-observed"* come from there and
   were re-verified against the core tree before being written down.

**On the numbers.** Byte and allocation figures are *arithmetic over allocation
sizes* (element sizes × libstdc++ geometric growth), **not** device
measurements, unless explicitly labelled "hardware-observed". They are sound for
comparing designs and unsound as absolute predictions.

---

## The checklist

Tiered by impact. Tier 1 changes whether a component fits on a constrained
target at all; Tier 2 prevents silent wrongness; Tier 3 is hygiene that keeps a
component reviewable.

### Tier 1 — memory and allocation

| # | Rule | §  |
|---|------|----|
| 1 | Emit lookup tables as one `static const` array, never one `add_*()` statement per row | [A1](#a1) |
| 2 | Pick the narrowest container: `StaticVector` → `FixedVector` → `std::vector` | [A2](#a2) |
| 3 | Flatten codegen-fed structs to PODs: `const char *`, not `std::string`; pointer+count, not `std::vector` | [A3](#a3) |
| 4 | Allocate in `setup()`, never in `loop()`; on failure `mark_failed()`, don't limp | [A4](#a4) |
| 5 | Never build a `std::string` per publish — use the `const char *` overloads and stack buffers | [A5](#a5) |
| 6 | Size queues/ring buffers once from a codegen upper bound; never reallocate | [B1](#b1) |
| 7 | Make the sizing invariant *enforced*, not commented | [B2](#b2) |

### Tier 2 — correctness traps

| # | Rule | §  |
|---|------|----|
| 8 | Read the **generated `main.cpp`** — `esphome config` passing does not mean it compiles | [G2](#g2) |
| 9 | `cv.enum` values need `cg.safe_exp()`; interpolating them emits an undeclared identifier | [D2](#d2) |
| 10 | Cross-`to_code` state belongs in `CORE.data`, never a module global | [D1](#d1) |
| 11 | Guard empty tables — a zero-length C++ array does not compile | [D4](#d4) |
| 12 | Use `CoroPriority.FINAL` when codegen must see every registered entity | [D3](#d3) |
| 13 | Decode integers in 64-bit and scale in `double`; narrow to `float` only at publish | [F1](#f1) |
| 14 | Distrust third-party converters that return a tagless union | [F2](#f2) |
| 15 | Validate at config time anything that would corrupt the wire | [E2](#e2) |
| 16 | `to_code` mutates the config — validated YAML is not codegen's input | [D6](#d6) |

### Tier 3 — logging, structure, verification

| # | Rule | §  |
|---|------|----|
| 17 | `ESP_LOGCONFIG` only in `dump_config()`; use the `LOG_<PLATFORM>` macros | [C1](#c1) |
| 18 | `LOG_STR` for strings handed to core APIs | [C2](#c2) |
| 19 | Match format specifiers to the actual type — `%u`, and `PRI*32` for `uint32_t` | [C3](#c3) |
| 20 | Don't "merge" `dump_config()` lines by duplicating a format string into both branches | [C4](#c4) |
| 21 | Don't `AUTO_LOAD` platform bases; guard with `#ifdef USE_<PLATFORM>` | [E1](#e1) |
| 22 | Don't redefine `CONF_` keys that already exist in `esphome.const` | [D5](#d5) |
| 23 | Rename config keys with `cv.rename_key`, never by hand in `to_code` | [D7](#d7) |
| 24 | Keep the module-level import surface light — it is paid on every config load | [D8](#d8) |
| 25 | Prefer composed `cv.*` validators over opaque functions (cheap default, not a hard rule) | [E3](#e3) |
| 26 | Keep pure logic framework-free so it is host-testable | [G1](#g1) |
| 27 | Work the gate ladder cheapest-first; know what each rung can and cannot catch | [G3](#g3) |

---

## A. Allocation and memory hygiene

The single highest-leverage area. An ESP32 usually absorbs sloppiness here; an
**ESP8266 (~40 KiB usable heap, `umm_malloc`, no MMU)** does not.

<a id="a1"></a>
### A1. Emit lookup tables as `static const` arrays

**Rule.** A mapping known at config time (enum labels, fault codes, option
values) becomes **one** `static const` array plus a setter taking a pointer and
a count. It must never become a run of one `add_option()` / `add_code()`
statement per row.

**Use.** `cg.static_const_array()` — `esphome/cpp_generator.py:466`. Already used
by `uart`, `sx127x`, `ble_client` and `remote_base`, so this is an established
core idiom, not an invention. `cg.progmem_array()` (`:457`) is its PROGMEM
sibling for AVR-style targets.

```python
# Python side
table_id = ID(f"{config[CONF_ID]}_options", is_declaration=True, type=MyOption)
arr = cg.static_const_array(table_id, cg.ArrayInitializer(*rows, multiline=True))
cg.add(var.set_options(arr, len(rows)))
```

```cpp
// C++ side — pointer + count, no container
struct MyOption { uint32_t value; const char *label; };

void set_options(const MyOption *options, uint16_t count) {
  this->options_ = options;
  this->option_count_ = count;
}
```

**Why.** Each per-row statement is an `emplace_back` on a geometrically growing
`std::vector`. A 94-entry map reallocates **eight** times (capacity 1→128),
freeing each predecessor, *interleaved with every other allocation ESPHome makes
during `setup()`*. That is a fragmentation generator, not merely wasted bytes.

**Observed**, over a real generated catalog:

| Catalog | Rows | Boot allocations | Freed immediately | Held after boot |
|---|---|---|---|---|
| curated | 116 | 27 | 19 | 1.2 KiB → **0** |
| complete | 1883 | 662 | 463 | 18.9 KiB → **0** |

18.9 KiB is roughly half an ESP8266's entire heap. The table also removes ~1900
statements from the generated `main.cpp` — the per-row calls are emitted
*instructions*, so this is a flash win too.

**Failure it prevents.** A component that boots fine on ESP32 and cannot boot at
all on a small target, for reasons invisible in the YAML.

<a id="a2"></a>
### A2. Pick the narrowest container

**Rule.** In order of preference:

| Container | Heap | When | Path |
|---|---|---|---|
| C array / `static const` | none | contents known at codegen time | [A1](#a1) |
| `StaticVector<T, N>` | none | `N` known at **compile** time | `core/helpers.h:222` |
| `FixedVector<T>` | one allocation | size known at **setup** time | `core/helpers.h:534` |
| `std::vector<T>` | grows | genuinely dynamic at runtime | — |

Core's own doc-comments state the intent:

> `StaticVector` — *"Minimal static vector - saves memory by avoiding
> `std::vector` overhead"*
>
> `FixedVector` — *"Fixed-capacity vector - allocates once at runtime, never
> reallocates. This avoids `std::vector` template overhead
> (`_M_realloc_insert`, `_M_default_append`) when size is known at
> initialization but not at compile time"*

**Why.** Beyond the allocations, `std::vector`'s growth machinery
(`_M_realloc_insert`) is instantiated per element type and is not small.

<a id="a3"></a>
### A3. Flatten codegen-fed structs to PODs

**Rule.** A struct populated from codegen should hold `const char *` and
pointer+count, not `std::string` and `std::vector`. Codegen string literals have
static storage; nothing needs to own them.

```cpp
// Before — two heap blocks per element, plus the outer vector's growth
struct Preset { std::string name; std::vector<uint8_t> read_values; /* ... */ };
std::vector<Preset> presets_;

// After — a POD; the whole table is static
struct Preset { const char *name; const uint8_t *read_values; uint8_t read_count; /* ... */ };
const Preset *presets_{nullptr};
uint16_t preset_count_{0};
```

**Why.** libstdc++'s small-string optimisation covers only ~15 characters — any
longer name is a heap block. Every `std::vector` member is another. Comparisons
become `std::strcmp` instead of `operator==`; that is the whole migration cost.

**Observed:** ~10 allocations → 0 for a 5-preset climate entity. Small in
absolute terms, but it is the same fix as A1 and costs nothing extra once the
table is static.

<a id="a4"></a>
### A4. Allocate at `setup()`, fail loudly

**Rule.** Every allocation happens once in `setup()`. If one fails, call
`mark_failed()` and stop — never continue in a half-configured state.

```cpp
if (!this->read_queue_.reserve(entity_count)) {
  ESP_LOGE(TAG, "failed to allocate read queue for %zu entities", entity_count);
  this->mark_failed(LOG_STR("read queue allocation failed"));
  return;
}
```

`mark_failed(const LogString *)` — `core/component.h:225`. Use separate checks
per allocation so the log names *which* one failed.

**Why.** An allocation in `loop()` is a fragmentation source that runs forever.
A component that silently half-works is worse than one that reports failure:
ESPHome surfaces `mark_failed` in `dump_config` and the API.

<a id="a5"></a>
### A5. No per-publish string building

**Rule.** Don't construct a `std::string` on every state publish.

**Use.** The `const char *` overloads —
`components/text_sensor/text_sensor.h:40-42` provides all three:

```cpp
void publish_state(const std::string &state);
void publish_state(const char *state);        // no allocation
void publish_state(const char *state, size_t len);
```

For formatting, write into a stack buffer:

```cpp
char buf[format_hex_pretty_size(48)];              // core/helpers.h:1400
format_hex_pretty_to(buf, sizeof(buf), data, len, ' ');  // core/helpers.h:1413
this->publish_state(buf);
```

`format_hex_pretty_to` NUL-terminates and hard-clamps to the buffer, so this is
overflow-safe. For non-owning string members, `StringRef`
(`core/string_ref.h:26`) — *"a reference to a string owned by something else …
never free its pointer"*.

**Why.** `publish_state(const char *)` assigns into the entity's reused state
string. Building a `std::string` first allocates on every publish — at poll
rates, forever.

---

## B. Queues, lanes and ring buffers

<a id="b1"></a>
### B1. Size once, from a codegen upper bound

**Rule.** Ring buffers and work queues get exactly one element-storage
allocation, sized at `setup()` from a count codegen computed. They never
reallocate and never grow.

Emit a `reserve_*(n)` / `init(n)` call from codegen ahead of any fill, then use
`FixedVector` (A2). Deliberately compute an **upper bound**, not an exact count —
under-sizing is a runtime failure, over-sizing costs a few pointers.

**Why.** A queue that reallocates does so under load, which is exactly when the
heap is most fragmented and least able to satisfy a large contiguous request.

<a id="b2"></a>
### B2. Enforce the sizing invariant, don't just comment it

**Rule.** If sizing must happen after all registration, make late registration a
hard error rather than a comment asking future maintainers to be careful.

```cpp
if (this->lanes_sized_) {
  ESP_LOGE(TAG, "register_entity() after the lanes were sized; entity ignored");
  return;
}
// ... later, once:
const std::size_t entity_count = this->entities_.size();
this->lanes_sized_ = true;
```

Likewise, a full fixed-capacity container must **log and fail**, not silently
drop:

```cpp
if (this->entities_.full()) {
  ESP_LOGE(TAG, "entity registry full (capacity %zu); '%s' dropped", ...);
  this->mark_failed(LOG_STR("entity registry full"));
  return;
}
```

**Why — hardware-observed.** Sampling the entity count *one line* above a block
that registers one more participant reserved `size() - 1` and cost that
participant's slot. The symptom was one dropped entity per poll cycle, every
boot:

```
[C][vitohome:563]: Entities: 56
[E][vitohome:537]: Poll cycle: read queue rejected 1 due entities (size=55, capacity=55)
```

A `FixedVector` silently drops a `push_back` past capacity. Without the explicit
`full()` check the entity would simply never poll, with no diagnostic at all.

---

## C. Logging

<a id="c1"></a>
### C1. `ESP_LOGCONFIG` belongs in `dump_config()`

**Rule.** `ESP_LOGCONFIG` (`core/log.h:164`) is for `dump_config()` only. Start
each entity with the platform macro, then indent details by two spaces per
level.

```cpp
void MySensor::dump_config() {
  LOG_SENSOR("  ", "My Sensor", this);            // components/sensor/sensor.h:20
  ESP_LOGCONFIG(TAG, "    Address: 0x%04X", this->address_);
}
```

`LOG_SENSOR` / `LOG_BINARY_SENSOR` (`components/binary_sensor/binary_sensor.h:16`)
and friends already apply `LOG_STR_LITERAL` to the type string.

**Why.** `dump_config` runs once at boot; `ESP_LOGCONFIG` output is what users
paste into bug reports. Anything logged per-poll belongs at `ESP_LOGD` or lower.

<a id="c2"></a>
### C2. `LOG_STR` keeps strings in flash

**Rule.** Use `LOG_STR(...)` for strings passed to core APIs that take a
`LogString *`, e.g. `mark_failed(LOG_STR("engine begin() failed"))`.

`LOG_STR`, `LOG_STR_ARG`, `LOG_STR_LITERAL` — `core/log.h:200-207`. On targets
with a separate program-memory space these expand to `PSTR(...)` and the
matching accessor; elsewhere they are a transparent cast.

<a id="c3"></a>
### C3. Match format specifiers to the actual type

**Rule.**

- `%u` for `uint8_t` / `uint16_t` (they promote to `int`/`unsigned`).
- `PRIu32` / `PRIx32` / `PRIX32` from `<cinttypes>` for `uint32_t`.
- `%zu` only for a genuine `size_t`.

```cpp
ESP_LOGD(TAG, "%s = option %u (raw 0x%02" PRIX32 ")", name, index, raw);
```

**Why.** `uint32_t` is not the same underlying type across ESPHome's targets —
`unsigned int` on some, `unsigned long` on others — so a hardcoded `%u` or `%lu`
is warning-clean on one platform and wrong on another. The `PRI*32` macros are
the portable form.

**Corollary (observed).** Don't reach for `%zu` and a `static_cast<std::size_t>`
to print a `uint16_t` count. It works, but it makes the file depend on
`<cstddef>` arriving transitively. `%u` with the value as-is is simpler and has
no include dependency.

<a id="c4"></a>
### C4. Don't merge `dump_config()` lines by duplicating format strings

**Rule.** Merging adjacent `ESP_LOGCONFIG` calls into one is only a win when the
lines are *unconditional*. Never collapse a conditional pair by repeating the
unconditional text in both branches.

```cpp
// WRONG — "Address: 0x%04X" is now stored TWICE in flash
if (extract) {
  ESP_LOGCONFIG(TAG, "  Address: 0x%04X\n  Extract: %u", addr, len);
} else {
  ESP_LOGCONFIG(TAG, "  Address: 0x%04X", addr);
}

// RIGHT — one copy of each string
ESP_LOGCONFIG(TAG, "  Address: 0x%04X", addr);
if (extract) {
  ESP_LOGCONFIG(TAG, "  Extract: %u", len);
}
```

**Why — observed.** This was a genuine wrong turn during an audit whose stated
goal was *reducing* flash. Fewer call sites looked like an improvement; the
duplicated format string made it a regression. Caught by reasoning about what
actually lands in `.rodata`, and reverted.

---

## D. Codegen (Python) discipline

<a id="d1"></a>
### D1. Cross-`to_code` state goes in `CORE.data`

**Rule.** When several platform files must contribute to one structure, collect
into `CORE.data`, never a module-level list.

```python
def register_hub_sensor(kind, var):
    store = CORE.data.setdefault("mycomponent", {}).setdefault("sensors", {})
    store.setdefault(kind, []).append(var)
```

**Why.** `CORE.reset()` clears `CORE.data`. A module global survives it, so
entities leak between configs compiled in one process — which is exactly what a
test suite does. The bug is invisible when compiling one config by hand and
appears only under test, as entities from a previous config bleeding into the
next.

<a id="d2"></a>
### D2. `cv.enum` values need `cg.safe_exp()`

**Rule.** Never interpolate a validated enum straight into generated code.

```python
# WRONG — emits a bare `heat`
f"{{{name}, {preset[CONF_MODE]}}}"

# RIGHT — emits climate::CLIMATE_MODE_HEAT
f"{{{name}, {cg.safe_exp(preset[CONF_MODE])}}}"
```

**Why — observed.** `cv.enum` returns an `EnumValue`, a `str` subclass whose
`str()` is the **YAML key**; the C++ expression lives in `.enum_value`.
`safe_exp()` unwraps it. Passing the value directly to a generated method call
also works, because ESPHome runs arguments through `safe_exp` for you — it is
only manual f-string interpolation that breaks.

The failure mode is nasty: valid Python, config validation passes, and the
generated C++ contains an **undeclared identifier** that only surfaces at
compile time. See [G2](#g2).

<a id="d3"></a>
### D3. Use `CoroPriority.FINAL` when you need every entity

**Rule.** Codegen that must see all registered entities runs as a final job:

```python
@coroutine_with_priority(CoroPriority.FINAL)     # coroutine.py:59, FINAL = -1000
async def _emit_tables():
    ...

# from the hub's to_code:
CORE.add_job(_emit_tables)
```

This is the pattern `uart`'s `final_step()` uses. Because it runs last, every
entity variable is already declared, so generated table initialisers can
reference them.

<a id="d4"></a>
### D4. Guard empty tables

**Rule.** An emitter must return "no table" for empty input, and the caller must
skip the setter.

```python
if not mapping:
    return None, 0
```

**Why.** `static const T x[] = {};` is not valid C++. Empty input is reachable in
practice — an entity type that takes no options, or a filter that empties a
one-entry map.

<a id="d5"></a>
### D5. Don't redefine core `CONF_` keys

**Rule.** Import shared option names from `esphome.const` rather than declaring
a second literal — e.g. `CONF_LENGTH` (`const.py:573`), `CONF_PROTOCOL`
(`const.py:853`). Define only genuinely component-specific keys, and centralise
those in the package `__init__.py` so one change propagates to every platform.

<a id="d6"></a>
### D6. `to_code` mutates the config — validated YAML is not codegen's input

**Rule.** Never assume the config dict your `to_code` receives is the one
`esphome config` printed. Validation runs first and produces a config; then every
component's `to_code` runs *against that same mutable structure* and is free to
change it — pinning auto-generated IDs, back-filling defaults, normalising
shorthand. A `CoroPriority.FINAL` job ([D3](#d3)) therefore sees the
**post-mutation** config, not the validated one.

Two practical consequences:

- **Read what you need at the priority you need it.** If a FINAL emitter depends
  on a value another component might normalise, read it in the FINAL job, not in
  a snapshot taken during your own `to_code`.
- **Don't read `CORE.config_hash` from component code.** The property
  (`core/__init__.py:730`) computes the FNV-1a hash of `CORE.config` **lazily and
  then caches it** — the first read for the whole run wins. `writer.py`'s
  `get_build_info` (`:406`) reads it after codegen, and that post-codegen value
  is what gets baked into the firmware as `ESPHOME_CONFIG_HASH`
  (`writer.py:456`). A component that touches `CORE.config_hash` during
  `to_code` freezes a pre-mutation hash and silently changes what the firmware
  advertises.

**Why — consumer-observed.** The dashboard hit exactly this from the other side:
a subprocess that loaded YAML, called `read_config` and read `CORE.config_hash`
disagreed with the hash the running firmware broadcast (`f3e21d5a` pre-codegen
vs `5a94a12d` post-codegen for the same config). The asymmetry is a fact about
`to_code`, and it is a component's `to_code` that creates it.

This is also the mechanism behind [G2](#g2): `esphome config` shows you the
validated config, and codegen runs on something else. Read the generated
`main.cpp`.

<a id="d7"></a>
### D7. Rename config keys with `cv.rename_key`

**Rule.** When an option is renamed, express it as `cv.rename_key(old, new)`
(`config_validation.py:2701`) on the schema's own mapping. Do not accept the old
spelling by rewriting keys by hand in `to_code`, and do not silently accept both.

```python
CONFIG_SCHEMA = cv.All(
    cv.Schema({ ... }),
    cv.rename_key(CONF_OLD_NAME, CONF_NEW_NAME),
)
```

`rename_key` raises if the user sets **both** keys — without that check one of
the two would be dropped silently. On `dev` it also takes
`removed_in=` and `component=` keyword arguments, which log a deprecation
warning naming your component while the old spelling still works; those are not
in 2026.7.0, so guard on the ESPHome version if you rely on them.

**Why — consumer-observed.** A rename expressed as a `rename_key` pair sitting
directly on a component or platform schema is *machine-discoverable*: tooling
introspects it and can offer users a one-click migration. The dashboard
generates its whole migration ruleset this way, and everything it cannot express
as a direct pair — wrapper-nested pairs, registry-node renames — needs
hand-written support in that consumer for your component specifically. A rename
you implement by rewriting keys inside `to_code` is invisible to all of it, so
users get no migration path and no deprecation warning.


<a id="d8"></a>
### D8. Keep the module-level import surface light

**Rule.** Everything at module scope in your component's Python runs on **every**
config load — every `esphome config`, every dashboard validation, every compile,
for every user who has your component in their YAML. Import heavy things inside
the function that needs them, not at the top of the module.

```python
# WRONG — paid by every config load, including ones that only validate
import requests
from esphome.components.esp32 import add_idf_component

CATALOG = _parse_catalog(Path(__file__).parent / "catalog.json")   # also module scope

# RIGHT — deferred to the call that actually needs it
async def to_code(config):
    from esphome.components.esp32 import add_idf_component
    ...

def _catalog():                      # memoise if it is genuinely needed twice
    global _CATALOG
    if _CATALOG is None:
        _CATALOG = _parse_catalog(...)
    return _CATALOG
```

Three specific things to keep out of module scope:

- **`esphome.components.*` imports.** Reach for a constant from another
  component and you drag in its whole dependency tree.
- **Third-party packages** (`requests`, `yaml`, anything numeric). They are also
  a dependency your users must now have installed to *validate* a config.
- **Eager parsing of a generated catalog.** Components that instantiate many
  entities from a catalog file (this document's audience) are the likeliest to
  do this. Parse lazily behind a memoised accessor, or precompute the parsed
  form into a plain Python module the loader can import cheaply.

**Why — consumer-observed.** The dashboard measured `esphome.components.esp32`
transitively pulling espidf → `requests` → `esphome.config`, "roughly 9s of cold
start before the first log line on an HA Green", and treats never importing
`esphome.components.*` in the long-lived process as an invariant guarded by CI
(an import-time budget plus a `sys.modules` probe). That is core's own import
graph; a component's module scope sits on the same path and is paid the same
way. The cost is invisible in your own testing — you notice one compile, not the
hundred validations a dashboard user's session performs.


---

## E. Component structure

<a id="e1"></a>
### E1. Don't `AUTO_LOAD` platform bases

**Rule.** A component shipping many platforms should **not** `AUTO_LOAD` their
base components. Guard each platform's implementation with
`#ifdef USE_<PLATFORM>`, including any platform-typed members the hub itself
owns.

```cpp
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *const *link_sensors_{nullptr};
#endif
```

**Why.** The base is then pulled in — and `USE_<PLATFORM>` defined — only when
the user actually configures that platform in their YAML. A config that uses two
platforms compiles two, not ten.

*Provenance note:* the component this came from records that forcing all bases
in via `AUTO_LOAD` is disallowed by ESPHome's component guidelines. That
attribution could not be re-verified here (docs unreachable), but the
compile-cost argument stands on its own.

**The other module-level markers.** `AUTO_LOAD` is one of a small set of
attributes the loader reads off your component's `__init__.py` by name
(`loader.py`); all default to absent, so an omission is silent rather than an
error:

| Marker | Loader | Meaning |
|---|---|---|
| `AUTO_LOAD` | `loader.py:98` | components pulled in implicitly — see the rule above |
| `MULTI_CONF` | `loader.py:76` | the component may be configured more than once |
| `MULTI_CONF_NO_DEFAULT` | `loader.py:80` | repeatable, but no implicit first instance |
| `IS_PLATFORM_COMPONENT` | `loader.py:64` | this component *is* a platform domain others register under |
| `CODEOWNERS` | `loader.py:102` | maintainer handles |

A hub that supports two of the same bus or two devices on one bus needs
`MULTI_CONF = True`; without it a user's second block is a validation error with
no hint that the component simply never declared itself repeatable. Set
`IS_PLATFORM_COMPONENT` only if you are defining your own platform domain —
a hub with `sensor` / `binary_sensor` platform files is a *consumer* of core's
domains, not a platform component itself.

<a id="e2"></a>
### E2. Config-time validation is load-bearing

**Rule.** Anything that would corrupt the wire, wrap, or be silently truncated
must be rejected at config time, with the offending option path named in the
error.

```python
raise cv.Invalid(
    f"visual.{key} must be a whole number of degrees: the setpoint is written "
    "as an integer degC byte, so a fractional bound would be silently truncated",
    [CONF_VISUAL, key],
)
```

Use `final_validate` for constraints that span components (e.g. "this feature is
incompatible with protocol X").

**Why.** A validator runs on the user's machine with a readable message. The
alternative is a wrong value on a bus, discovered as misbehaving hardware.

<a id="e3"></a>
### E3. Prefer composed `cv.*` validators over opaque functions

**Rule.** Build option schemas out of composable `cv.*` validators
(`cv.int_range`, `cv.enum`, `cv.one_of`, `cv.All`, `cv.Schema`). Reach for a
bespoke validation *function* only for a constraint that genuinely cannot be
expressed declaratively — and when you do, keep it as narrow as possible and
leave the surrounding structure declarative.

```python
# Discoverable — the range and the enum survive introspection
cv.Optional(CONF_SETPOINT, default=20): cv.int_range(min=10, max=80),
cv.Required(CONF_MODE): cv.enum(MODES, upper=True),

# Opaque — a consumer sees an untyped field with no bounds and no options
cv.Required(CONF_MODE): _validate_mode,
```

**Why — consumer-observed.** A custom function is a black box to anything that
introspects validators. ESPHome's published schema bundle is derived that way,
and the dashboard records that `api.encryption` "emerges as
`{key: Optional, docs: ...}` because ESPHome validates it with a custom
function" — it had to hard-code a `_FIELD_OVERRIDES` entry to render that field
as anything better than an untyped box.

**Scope — this is weaker for external components than for core ones.** The
published bundle is built by `script/build_language_schema.py`, whose
`get_component_names()` iterates **`esphome/components/` only**
(`build_language_schema.py:80`). An `external_components:` package is never in
it, so no amount of declarative schema gets your component rendered by a tool
that reads the bundle alone. What still holds:

- Tools that introspect the **loaded** `esphome` package see your component once
  the config imports it — that live path is exactly how the dashboard recovers
  what the bundle doesn't carry (`multi_conf`, `supported_platforms`,
  `cv.typed_schema` `default_type` read out of the validator's closure).
- Composed validators produce better **error messages** for free — `cv.one_of`
  names the valid values, a bespoke `raise cv.Invalid` says whatever you
  remembered to write.
- If the component is ever upstreamed, an opaque function is the thing that
  makes it render badly on day one.

So treat this as a cheap default rather than a hard rule, and don't contort a
genuinely non-declarative constraint to satisfy it. It yields outright to
[E2](#e2): a constraint that protects the wire gets validated at config time
whatever it takes.


---

## F. Numeric correctness

<a id="f1"></a>
### F1. Don't do sensor math in `float`

**Rule.** Extract integers into `int64_t`/`uint64_t`, scale in `double`, and
narrow to `float` only for the final published state.

**Why.** `float32` has a 24-bit significand: it silently drops bits above
2²⁴ ≈ 16.7 M. A 4-byte counter — burner hours in seconds, energy in Wh — exceeds
that in normal operation, and the error is a plausible-looking wrong number, not
an obvious one. After scaling, published values are small (hours, °C, %), so the
final narrowing is harmless.

<a id="f2"></a>
### F2. Distrust tagless-union converters

**Rule.** Be wary of vendored/third-party converter layers that return a union
with no discriminant. Reading the wrong member returns garbage with no error.

If a library's converter layer is unsound, decoding the raw payload yourself in
your own well-tested header is a legitimate and often better choice — see
[G1](#g1).

---

## G. Verification

<a id="g1"></a>
### G1. Keep pure logic framework-free

**Rule.** Put decode/encode and other pure logic in a header with **no ESPHome
includes**, so it compiles and runs on the host.

```
components/mycomp/decode.h        # <cstdint>, <cmath> only
tests/native/test_decode.cpp      # g++ -std=c++17 -Wall -Wextra -Werror
```

**Why.** This is the only layer you can test exhaustively and instantly. Bit
extraction, sign extension, BCD, scaling and range rejection are exactly where
subtle bugs live, and exactly what needs no framework.

<a id="g2"></a>
### G2. Read the generated `main.cpp`

**Rule.** After any codegen change, open
`.esphome/build/<name>/src/main.cpp` and confirm the emitted C++ is what you
intended. Grep for the new construct **and** for the absence of the old one.

```bash
grep -nE "static const (MyOption|uint32_t)" main.cpp   # new form present
grep -c "add_option(\|add_code(" main.cpp              # old form gone: expect 0
```

**Why — observed.** `esphome config` validates *YAML*, not generated C++. The
`EnumValue` bug in [D2](#d2) passed config validation on all six test configs
and would have failed at compile time with an undeclared identifier. Reading the
generated table caught it in seconds.

This matters most when you cannot compile firmware locally: generated-code
inspection is the strongest gate still available to you.

<a id="g3"></a>
### G3. A gate ladder

Run cheapest-first; each rung catches what the one below cannot.

| Rung | Gate | Catches |
|---|---|---|
| 1 | `ruff check` / `ruff format --check`, `clang-format` | style, dead imports |
| 2 | `esphome config` over representative YAMLs (one per protocol/toolchain) | schema, validators, final_validate |
| 3 | Host unit tests (`pytest`) for emitters and validators | empty-input guards, escaping, counts |
| 4 | Host native tests for pure logic | decode/encode correctness |
| 5 | **Generated `main.cpp` inspection** | codegen that validates but won't compile |
| 6 | Firmware compile in CI | everything C++ |
| 7 | Device: `debug` component `free` / `block` / `min_free` | actual memory behaviour |

**On rung 7:** ESPHome's ESP32 fragmentation percentage is
`100 − 100 × largest_free_block / free_heap` over `MALLOC_CAP_INTERNAL`. That
capability spans **several non-contiguous DRAM regions** — the numerator is
bounded by the largest *single* region while the denominator sums *all* of them,
so a heap that has never fragmented still reports a substantial figure. A real
example: 52.3% fragmentation with a **124 KiB contiguous** largest free block —
a healthy heap. **Judge by `block` and `min_free`, not the ratio.**

---

## Quick reference — core APIs cited

| Symbol | Path (ESPHome 2026.7.0) | Use |
|---|---|---|
| `StaticVector<T, N>` | `core/helpers.h:222` | no-heap container, compile-time N |
| `FixedVector<T>` | `core/helpers.h:534` | one allocation, setup-time size |
| `format_hex_pretty_size` | `core/helpers.h:1400` | stack buffer sizing |
| `format_hex_pretty_to` | `core/helpers.h:1413` | no-alloc hex formatting |
| `StringRef` | `core/string_ref.h:26` | non-owning string |
| `LOG_STR` / `LOG_STR_ARG` | `core/log.h:200-207` | flash-resident log strings |
| `ESP_LOGCONFIG` | `core/log.h:164` | `dump_config()` output |
| `LOG_SENSOR` | `components/sensor/sensor.h:20` | entity header line |
| `LOG_BINARY_SENSOR` | `components/binary_sensor/binary_sensor.h:16` | entity header line |
| `mark_failed(LogString *)` | `core/component.h:225` | fail loudly at setup |
| `publish_state(const char *)` | `components/text_sensor/text_sensor.h:41` | no-alloc publish |
| `cg.static_const_array` | `cpp_generator.py:466` | `static const T x[] = {...}` |
| `cg.progmem_array` | `cpp_generator.py:457` | PROGMEM variant |
| `CoroPriority.FINAL` | `coroutine.py:59` (`= -1000`) | run codegen last |
| `MULTI_CONF` / `IS_PLATFORM_COMPONENT` | `loader.py:76` / `:64` | component module markers |
| `cv.rename_key` | `config_validation.py:2701` | machine-discoverable key rename |
| `CORE.config_hash` | `core/__init__.py:730` | lazily computed **and cached** — don't read it in `to_code` |
| `writer.get_build_info` | `writer.py:400` | reads `config_hash` post-codegen |
