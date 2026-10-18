"""Protocol, conversion, and component-mirror constants for vitocatalog."""

from __future__ import annotations

# --- conversion -> vitohome converter --------------------------------------
# Maps the Vitosoft ``Conversion`` name to a vitohome ``converter:`` plus a
# "kind" used for platform/poll decisions. A None converter means "numeric but
# no scaling preset": emitted as noconv (raw) with a NOTE.
DIV = "div"
COUNTER = "counter"
PLAIN = "plain"

CONVERSION_MAP = {
    "": ("noconv", PLAIN),
    "NoConversion": ("noconv", PLAIN),
    "Div2": ("div2", DIV),
    "Div10": ("div10", DIV),
    "Div100": ("div100", DIV),
    "Div1000": ("div1000", DIV),
    "Mult2": ("mult2", PLAIN),
    "Mult5": ("mult5", PLAIN),
    "Mult10": ("mult10", PLAIN),
    "Mult100": ("mult100", PLAIN),
    "Sec2Hour": ("sec2hour", COUNTER),
    "RotateBytes": ("rotatebytes", PLAIN),  # big-endian 2-byte values -> decode_scaled_be
    "Sec2Minute": (None, COUNTER),  # no preset; emit noconv + note
}

# Conversions that are not a plain scaled integer. No numeric entity is emitted
# for them; they appear as commented hints in the output.
NON_NUMERIC_CONVERSIONS = {
    "DateTimeBCD": "8-byte date/time -> error_history or custom decode",
    "DateBCD": "date -> custom decode",
    "Time53": "switching times -> custom decode",
    "IPAddress": "string -> text_sensor + custom decode",
    "Phone2BCD": "string -> custom decode",
    "Convert4BytesToFloat": "IEEE-754 float -> not yet supported by vitohome",
    "HexToFloat": "IEEE-754 float -> not yet supported by vitohome",
    "MultOffsetFloat": "float -> custom decode",
    "MultOffsetBCD": "BCD-coded value -> custom decode",
}

# ConversionFactor values that map 1:1 onto a component converter when
# ConversionOffset is 0. Used for ``MultOffset`` (value = raw * factor +
# offset) and for the ``NoConversion`` rows that carry a real factor (0.1 and
# 10 occur on Virtual_READ rows). Other factors fall back to noconv plus an
# ESPHome ``multiply:`` filter on read-only sensors.
_FACTOR_PRESETS = (
    (2.0, "mult2"),
    (5.0, "mult5"),
    (10.0, "mult10"),
    (100.0, "mult100"),
    (0.5, "div2"),
    (0.1, "div10"),
    (0.01, "div100"),
    (0.001, "div1000"),
)

# Vitosoft's placeholder pair: rows without a real conversion factor carry the
# literal strings -0.1067 / 6992.58 (Phone2BCD, HexByte2AsciiByte, VitocomNV
# and other non-numeric rows). The pair means "no factor/offset present".
_FACTOR_PLACEHOLDERS = ("-0.1067", "6992.58")
# Tolerance for "a border / stepping is a whole number of raw steps" in
# _scale_from_bounds: steppings such as 0.125 and 0.1 are not exact in binary
# floating point.
_STEP_TOL = 1e-6

# Mode commands whose live state is read at a DIFFERENT address than the
# command register: writable command address -> read-only effective-state
# address, per heating circuit. Emitted as `state_address`, so the read-back
# shows the actual state rather than the command register. Betriebsart
# (0x2323) is not listed: its read (0x2501, 4 values) and write (0x2323, 5
# values) domains differ, so it stays a command-only select plus a separate
# effective-mode sensor.
COMMAND_STATE_ADDR = {
    0x2330: 0x2303,  # A1: party  (NRx Partybetrieb  -> BedienPartybetrieb)
    0x2331: 0x2302,  # A1: economy (NRx Sparbetrieb  -> BedienSparbetrieb)
    0x3330: 0x3303,  # M2: party
    0x3331: 0x3302,  # M2: economy
    0x4330: 0x4303,  # M3: party
    0x4331: 0x4302,  # M3: economy
}

# Poll tiers (seconds).
POLL_FAST = 60  # live temperatures / measurements
POLL_SLOW = 600  # monotonic counters (hours, starts, consumption)
POLL_CODING = 3600  # writable coding values / setpoints (rarely change)
POLL_ERROR = 300  # error history

PROFILES = ("minimal", "standard", "full")

# Culture name -> CultureId in Textresource.xml: the full ecnCulture table that
# every Textresource*.xml header declares (18 cultures). The slim
# Textresource.xml carries ~350 UI strings per culture; the full
# Textresource_<culture>.xml files (one culture each) carry every name, option
# label and fault text -- see _load_text_layers.
_CULTURES = {
    "de": "1",
    "en": "2",
    "fr": "3",
    "it": "4",
    "ru": "5",
    "nl": "6",
    "pl": "7",
    "da": "8",
    "hu": "9",
    "es": "10",
    "tr": "11",
    "lt": "12",
    "cs": "13",
    "sv": "14",
    "no": "15",
    "sk": "16",
    "hr": "17",
    "ro": "18",
}

# Identification reads (0xF8 group, 0xF9 ident, 0xFA hardware, 0xFB software).
# The hub reads these at boot and feeds the device_id text_sensor, so the raw
# datapoints at these addresses are suppressed when --device-id is on.
_IDENT_ADDRESSES = (0x00F8, 0x00F9, 0x00FA, 0x00FB)
# Vitotronic SYSTEM error-history block: 10 slots x 9 bytes at 0x7507, slot
# index at 0x7561. Used as an address fallback for exports whose system archive
# carries no recognizable token. Independent of the FehlerHis* tokens, which are
# the GFA burner-control archive; a unit can carry both (see _is_error_history).
_ERROR_HISTORY_ADDRESS = 0x7507

# Element local-name prefixes that denote a table row or dataset container.
# Clearing these while streaming bounds memory on the ~212 MB DPDefinitions.xml.
# Scalar fields (Id, Name, DataPointTypeId, ...) never start with these.
_CONTAINER_PREFIXES = ("ecn", "sys", "vsm", "Vsm", "ECN", "Document")

# The DPDefinitions tables load_catalog reads.
_WANTED_ROWS = {
    "ecnDatapointType",
    "ecnDataPointTypeEventTypeLink",
    "ecnEventType",
    "ecnEventTypeEventValueTypeLink",
    "ecnEventValueType",
    "ecnEventTypeGroup",
    "ecnEventTypeEventTypeGroupLink",
}

# Platforms this generator emits `access:` on: a subset of the component's
# _ACCESS_DOMAINS (components/vitohome/__init__.py). `climate` is absent because
# the generator emits no climate blocks -- a climate entity combines several
# datapoints and a preset table, which is not derivable per XML row. `event` is
# the error-history archive.
ACCESS_PLATFORMS = ("sensor", "binary_sensor", "text_sensor", "number", "select", "switch", "text")

# Protocol tokens for --protocol / _protocol_for_device().
PROTO_P300 = "P300"
PROTO_KW = "KW"
PROTO_GWG = "GWG"

# Vitosoft FCRead -> vitohome `access:` value, GWG only.
#
# GWG carries the access mode in the telegram's TYPE byte, and its address
# space is PER MODE: in GWG_VBES_00, address 0x01 is five different datapoints
# across four modes (OEM ident on kmbus_eeprom, max boiler temperature on
# eeprom, three outputs on port). The address alone is therefore not a
# datapoint identity; FCRead is the other half.
#
# The openv wiki's Protokoll-GWG "Telegramm Typen" table, vcontrold's GWG
# macros and these FCRead names agree on the same seven bytes. Across the 22
# Vitosoft GWG device tokens the only FCRead values are these seven, blank, and
# KBUS_VIRTUAL_READ (a K-bus tunnel that no GWG access mode reaches).
# kmbus_ram (0x33) has no FCRead: no Vitosoft GWG datapoint uses it.
#
# Blank FCRead means "no access row"; it maps to the engine default, physical.
FCREAD_TO_GWG_ACCESS = {
    "physical_read": "physical",
    "virtual_read": "virtual",
    "eeprom_read": "eeprom",
    "xram_read": "xram",
    "port_read": "port",
    "be_read": "be",
    "kmbus_eeprom_read": "kmbus_eeprom",
}

# Vitosoft FCWrite -> the same `access:` values, GWG only.
#
# The openv wiki gives a write TYPE byte for six of the eight modes (VIRTUAL
# 0xC4, PHYSICAL 0xC8, EEPROM 0xAD, XRAM 0xC3, PORT 0x6D, BE 0x9D) and none for
# either KMBUS mode, so those two are read-only. In the export the writable GWG
# datapoints are EEPROM_WRITE and BE_WRITE, plus KBUS_VIRTUAL_WRITE, which no
# access mode reaches; none is PHYSICAL_WRITE.
FCWRITE_TO_GWG_ACCESS = {
    "physical_write": "physical",
    "virtual_write": "virtual",
    "eeprom_write": "eeprom",
    "xram_write": "xram",
    "port_write": "port",
    "be_write": "be",
}

# On/off label pairs _boolean_pair accepts, compared after _norm_bool_label.
_BOOLEAN_ON_OFF_PAIRS = [
    ("ein", "aus"),
    ("an", "aus"),
    ("aktiv", "inaktiv"),
    ("ja", "nein"),
    ("on", "off"),
    ("yes", "no"),
    ("freigegeben", "gesperrt"),
]

# Mirrors the converter scales in CONVERTERS (components/vitohome/__init__.py).
# Kept in sync by hand; the component's number.py is the runtime authority.
_CONV_SCALE = {
    "noconv": 1.0,
    "div2": 0.5,
    "div10": 0.1,
    "div100": 0.01,
    "div1000": 0.001,
    "sec2hour": 1.0 / 3600.0,
    "mult2": 2.0,
    "mult5": 5.0,
    "mult10": 10.0,
    "mult100": 100.0,
}

# Mirrors CONVERTERS[...].lengths in components/vitohome/__init__.py: the byte
# widths each converter can decode. sensor.py/number.py reject a config whose
# (converter, effective width) is outside this table, so the generator never
# emits such a pair: it falls back to noconv plus a `multiply:` filter carrying
# the true scale (read-only), and a writable is demoted first.
_CONV_LENGTHS = {
    "noconv": (1, 2, 3, 4),
    "div2": (1, 2),
    "div10": (1, 2),
    "div100": (1, 2, 4),
    "div1000": (2, 4),
    "sec2hour": (4,),
    "mult2": (1, 2, 4),
    "mult5": (1, 2, 4),
    "mult10": (1, 2, 4),
    "mult100": (1, 2, 4),
    "rotatebytes": (2,),
}

# Vitosoft's unsigned integer ParameterTypes. The *HighByteFirst variants are
# not listed: they also change the byte order, which is not modelled for
# div2/div10 (no reachable div2/div10 datapoint carries one).
_UNSIGNED_PARAMETER_TYPES = ("byte", "int", "int4")

# Single-telegram READ payload cap for P300/VS2 (mirror of
# components/vitohome/__init__.py::MAX_P300_READ_LENGTH). A wider block read
# NAKs on P300, so such a datapoint becomes a commented hint instead of config
# that fails on the wire.
MAX_P300_READ_LENGTH = 48
MAX_TEXT_BLOCK_LENGTH = MAX_P300_READ_LENGTH
# Mirror of components/vitohome/__init__.py::MAX_RMW_BLOCK: the largest block a
# whole-block read-modify-write sends.
MAX_RMW_BLOCK = 16

# German weekday names for the per-day Schaltzeiten text entities, Monday first
# (vcontrold's base + day*8 convention and the component's decode). (label,
# id-suffix) pairs: the id suffix never changes with the language; the label
# comes from _UI_TEXT.
_WEEKDAYS = (
    ("Montag", "mo"),
    ("Dienstag", "di"),
    ("Mittwoch", "mi"),
    ("Donnerstag", "do"),
    ("Freitag", "fr"),
    ("Samstag", "sa"),
    ("Sonntag", "so"),
)

# Names the generator makes up itself (Vitosoft has no label for them). German
# keeps its established spellings, so a German catalog does not rename these
# entities; every other language gets English. Entity ids never change.
_UI_TEXT = {
    "de": {
        "last_fault": "Letzter Fehler",
        "fault": "Fehler",
        "device_type": "Geraete-Typ",
        "switching_time": "Schaltzeit",
        "weekdays": tuple(day for day, _sfx in _WEEKDAYS),
    },
    "en": {
        "last_fault": "Last fault",
        "fault": "Fault",
        "device_type": "Device type",
        "switching_time": "Switching times",
        "weekdays": ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"),
    },
}


def _ui(lang: str, key: str):
    return _UI_TEXT.get(lang, _UI_TEXT["en"])[key]


# The only Schaltzeiten shape the component's `text` platform can decode: a
# 56-byte weekday program = 7 records of 8 bytes (four ON/OFF switch-point pairs
# per day; vito_text.cpp's SCHALTZEITEN_LEN = 8). The export's other shapes --
# 168/56 (heat pump, 3-byte records), 168/7 (ventilation, 24-byte records), the
# 24/25-byte LON vmarSchaltzeitenGroup blocks and the pre-decomposed 1-byte
# KBUS_HV_Schaltzeit_* fields -- stay custom-decode comments.
_SCHALTZEITEN_RECORD_LEN = 8
_SCHALTZEITEN_DAYS = 7
_SCHALTZEITEN_BLOCK_LEN = _SCHALTZEITEN_RECORD_LEN * _SCHALTZEITEN_DAYS  # 56

# Subdirectory of --export-all output for the GFA companion packages, one per
# burner-control chip. The GFA fault texts are keyed by the chip the installed
# unit reports at 0x7650, not by the device type, so a catalog cannot carry
# them; a companion extends the catalog's GFA history slots by id.
GFA_DIRNAME = "gfa"

_MANIFEST_COLUMNS = (
    "file",
    "token",
    "ident",
    "hw_index",
    "sw_lo",
    "sw_hi",
    "f0_lo",
    "f0_hi",
    "events",  # datapoints LINKED to the unit in the export
    "entities",  # entities actually EMITTED into the file (post filters/profile)
    "bytes",
    "status",
    "ext_raw",
    "gfa_slots",  # GFA burner-control history slots (companion packages, gfa/)
)

# Schema version and file name of the catalog finder's lookup table
# (build_resolve_index).
RESOLVE_SCHEMA = 1
RESOLVE_FILENAME = "resolve.json"
