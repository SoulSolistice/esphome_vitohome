"""Copy the engines into tests/native/reference/optolink/ under the namespace
esphome::vitohome::optolink_ref (README.md). Run once, from the repository
root: the copy is frozen, so the script refuses to replace one."""

import pathlib
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
src = root / "components/vitohome/optolink"
dst = root / "tests/native/reference/optolink"
if dst.exists():
    sys.exit(f"{dst} exists: the reference copy is frozen")

copied = [
    "constants.h",
    "constants.cpp",
    "interface/serial_interface.h",
    "interface/generic_interface.h",
    "protocol/vs1/packet_vs1.h",
    "protocol/vs1/packet_vs1.cpp",
    "protocol/vs1/vs1.h",
    "protocol/vs1/vs1.cpp",
    "protocol/vs2/packet_vs2.h",
    "protocol/vs2/packet_vs2.cpp",
    "protocol/vs2/parser_vs2.h",
    "protocol/vs2/parser_vs2.cpp",
    "protocol/vs2/vs2.h",
    "protocol/vs2/vs2.cpp",
    "protocol/gwg/packet_gwg.h",
    "protocol/gwg/packet_gwg.cpp",
    "protocol/gwg/gwg.h",
    "protocol/gwg/gwg.cpp",
]

for rel in copied:
    text = (src / rel).read_text(encoding="utf-8")
    if "esphome::vitohome::optolink_ref" in text:
        sys.exit(f"{rel}: already renamed")
    n = text.count("esphome::vitohome::optolink")
    if n == 0:
        sys.exit(f"{rel}: no namespace to rename")
    text = text.replace("esphome::vitohome::optolink", "esphome::vitohome::optolink_ref")
    out = dst / rel
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"{rel}: {n} renamed")

(dst / "helpers.h").write_text(
    """#pragma once

// The reference engines run on the live engines' clock, so one test script
// drives both sides of the differential target.

#include "../../../../components/vitohome/optolink/helpers.h"

namespace esphome::vitohome::optolink_ref {
using ::esphome::vitohome::optolink::optolink_test_clock_advance;
using ::esphome::vitohome::optolink::optolink_test_clock_freeze;
using ::esphome::vitohome::optolink::optolink_test_clock_release;
}  // namespace esphome::vitohome::optolink_ref
""",
    encoding="utf-8",
    newline="\n",
)
(dst / "logging.h").write_text(
    """#pragma once

#include "../../../../components/vitohome/optolink/logging.h"
""",
    encoding="utf-8",
    newline="\n",
)
(dst / "optolink.h").write_text(
    """#pragma once

// The reference engines (README.md): what the fuzz harnesses include in place
// of optolink/optolink.h when they build the reference side.

#include "constants.h"
#include "helpers.h"
#include "protocol/gwg/gwg.h"
#include "protocol/gwg/packet_gwg.h"
#include "protocol/vs1/packet_vs1.h"
#include "protocol/vs1/vs1.h"
#include "protocol/vs2/packet_vs2.h"
#include "protocol/vs2/parser_vs2.h"
#include "protocol/vs2/vs2.h"
""",
    encoding="utf-8",
    newline="\n",
)
print("ok")
