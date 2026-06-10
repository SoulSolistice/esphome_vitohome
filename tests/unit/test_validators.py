import pytest
import esphome.config_validation as cv

from vitohome import cpp_string_literal
from vitohome.sensor import (
    _validate_length,
    _validate_converter_length,
    CONF_CONVERTER,
    CONF_LENGTH,
)
from vitohome.binary_sensor import (
    _validate_offset_within_length,
    CONF_BYTE_OFFSET,
    CONF_LENGTH as BIN_CONF_LENGTH,
)


class TestCppStringLiteral:
    def test_plain(self):
        assert cpp_string_literal("Outside Temp") == '"Outside Temp"'

    def test_double_quote_is_escaped(self):
        assert cpp_string_literal('a"b') == '"a\\"b"'

    def test_backslash_is_escaped(self):
        assert cpp_string_literal("a\\b") == '"a\\\\b"'


class TestLengthValidator:
    @pytest.mark.parametrize("v", [1, 2, 4])
    def test_valid(self, v):
        assert _validate_length(v) == v

    @pytest.mark.parametrize("v", [0, 3, 5, 8])
    def test_invalid(self, v):
        with pytest.raises(cv.Invalid):
            _validate_length(v)


class TestConverterLengthCrossCheck:
    def test_div10_len2_ok(self):
        cfg = {CONF_CONVERTER: "div10", CONF_LENGTH: 2}
        assert _validate_converter_length(cfg) is cfg

    def test_div2_len2_rejected(self):
        cfg = {CONF_CONVERTER: "div2", CONF_LENGTH: 2}
        with pytest.raises(cv.Invalid):
            _validate_converter_length(cfg)

    def test_noconv_len4_ok(self):
        cfg = {CONF_CONVERTER: "noconv", CONF_LENGTH: 4}
        assert _validate_converter_length(cfg) is cfg


class TestOffsetWithinLength:
    def test_ok(self):
        cfg = {CONF_BYTE_OFFSET: 0, BIN_CONF_LENGTH: 1}
        assert _validate_offset_within_length(cfg) is cfg

    def test_offset_equals_length_rejected(self):
        cfg = {CONF_BYTE_OFFSET: 1, BIN_CONF_LENGTH: 1}
        with pytest.raises(cv.Invalid):
            _validate_offset_within_length(cfg)
