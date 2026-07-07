# ESP-IDF native-toolchain build description for the vendored Optolink engine.
#
# This is the CMake-side twin of ./library.json: PlatformIO's build (used
# under esp32.toolchain: platformio, the current default -- see
# design_notes.md SS11) reads library.json and ignores this file entirely.
# ESP-IDF's native toolchain (esp32.toolchain: esp-idf) uses CMake, not
# PlatformIO's library finder, and does not read library.json at all -- this
# file is what makes the engine buildable under that toolchain. Registered as
# a local ``path:`` dependency of the main component by
# ``components/vitohome/__init__.py::to_code`` via
# ``esp32.add_idf_component(name="optolink", path=...)``.
#
# SRCS is listed explicitly rather than globbed, deliberately kept in the
# same shape as library.json's srcFilter so the two are easy to diff against
# each other by eye. If a source file is ever added under a *new*
# subdirectory (not root, datapoint/, protocol/vs1/, protocol/vs2/, or
# protocol/gwg/), both this list and library.json's srcFilter need a new line
# -- neither one discovers new subdirectories automatically.
#
# INCLUDE_DIRS is ".." (components/vitohome/, this component's parent), not
# empty. Checked against a real native-toolchain compile in this project's
# sandbox validation: ESP-IDF's own compile-options propagation
# (esphome/build_gen/espidf.py::get_project_cmakelists, pinned version
# 2026.6.2) filters CORE.build_flags down to only "-D"/"-W" prefixed flags
# before emitting them project-wide -- the existing project-wide
# ``-I<component_dir>`` flag from to_code() (which the platformio toolchain
# uses to make "optolink/optolink.h" resolve) is silently dropped under the
# native toolchain, and "main"'s own generated idf_component_register()
# (ESPHome-authored, not ours to edit) hardcodes ``INCLUDE_DIRS "." "esphome"``
# with no extension point. Exposing ".." here instead is the ESP-IDF-idiomatic
# fix: "main" already (implicitly) REQUIRES optolink via the ``path:``
# dependency in the generated idf_component.yml, and ESP-IDF automatically
# propagates a required component's public INCLUDE_DIRS to the requiring
# component -- confirmed working end-to-end by the same sandbox compile.
# This exposes the same directory the old ``-I`` flag did (all of
# components/vitohome/, not just optolink/), so it's parity with the
# existing platformio-toolchain behavior, not new over-exposure.
idf_component_register(
    SRCS
        "constants.cpp"
        "datapoint/converter.cpp"
        "datapoint/datapoint.cpp"
        "protocol/vs1/packet_vs1.cpp"
        "protocol/vs1/vs1.cpp"
        "protocol/vs2/packet_vs2.cpp"
        "protocol/vs2/parser_vs2.cpp"
        "protocol/vs2/vs2.cpp"
        "protocol/gwg/gwg.cpp"
        "protocol/gwg/packet_gwg.cpp"
    INCLUDE_DIRS ".."
)
