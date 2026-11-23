// Proof for the engines' generic telegrams, the raw lane's queue_raw_op()
// path (raw_op.h). On a frozen clock:
//
//   P300 (a Lane::P300 request on the P300 exchange):
//     A) function code 05 goes out in the read frame, its RESPONSE is
//        delivered by the frame's own length;
//     B) an ERROR frame raises DEVICE_ERROR and keeps its payload -- one byte
//        (05 for a function code the controller lacks) or two (31 01);
//     C) the next result carries no error code; a read is the same frame
//        as request(01); a write's acknowledgement for a read is ERROR;
//     D) guards: code 0 and codes above 1F are refused;
//   P300 KW detour (a Lane::KW request on the P300 exchange):
//     E) any type byte goes out on the ENQ, the answer is <len> raw bytes;
//     F) a telegram with payload carries it and completes on one ack byte;
//        it chains without the GFA pause;
//   KW (a Lane::KW request on the KW exchange):
//     G) any type byte, <len> bytes back; with payload, one ack byte;
//   GWG (GWGEngine::telegram()):
//     H) an unnamed TYPE byte: 01 TYPE ADDR LEN 04, <len> bytes back; with
//        payload, the write layout and one ack byte;
//     I) a TYPE byte of the access-mode table keeps its mode (EEPROM pairs).
//   Frame limits: every builder takes MAX_PAYLOAD payload bytes, and every
//   engine refuses a read whose answer would be longer.
//
// Built by build_and_run_protocols.sh.

#include <cstdio>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink/optolink.h"
#include "optolink_kw.h"

namespace optolink = esphome::vitohome::optolink;
using Bytes = std::vector<uint8_t>;

namespace {
int g_fail = 0;
int g_responses = 0;
int g_errors = 0;
optolink::OptolinkResult g_last_error = optolink::OptolinkResult::CONTINUE;
Bytes g_payload;
uint16_t g_address = 0;

void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

template<class E> void pump(E &e, int n = 8) {
  for (int i = 0; i < n; ++i)
    e.loop();
}

template<class E> void hook(E &e) {
  kw_calls::wire(
      e,
      [](const uint8_t *d, uint8_t len, uint16_t address) {
        ++g_responses;
        g_payload.assign(d, d + len);
        g_address = address;
      },
      [](optolink::OptolinkResult r, uint16_t) {
        ++g_errors;
        g_last_error = r;
      });
}

// A P300 DEVICE_ERROR's error code, from its result; empty on any other.
Bytes g_error_code;
bool g_code_seen = false;

template<class E> void hook_p300(E &e) {
  kw_calls::wire(
      e,
      [](const uint8_t *d, uint8_t len, uint16_t address) {
        ++g_responses;
        g_payload.assign(d, d + len);
        g_address = address;
        g_error_code.clear();
      },
      [](optolink::OptolinkResult r, uint16_t) {
        ++g_errors;
        g_last_error = r;
        if (!g_code_seen)
          g_error_code.clear();
        g_code_seen = false;
      },
      [](const uint8_t *code, uint8_t n) {
        g_error_code.assign(code, code + n);
        g_code_seen = true;
      });
}

// A P300 frame with its checksum: the sum of every byte after 0x41.
Bytes p300(Bytes body) {
  uint8_t cs = 0;
  for (uint8_t b : body)
    cs += b;
  Bytes frame{0x41};
  frame.insert(frame.end(), body.begin(), body.end());
  frame.push_back(cs);
  return frame;
}

Bytes acked(const Bytes &frame) {
  Bytes out{0x06};
  out.insert(out.end(), frame.begin(), frame.end());
  return out;
}

bool handshake(optolink::P300Exchange<FakeOptolink> &e, FakeOptolink &io) {
  pump(e);
  const bool eot = io.written() == Bytes({0x04});
  io.clear_written();
  io.feed({0x05});
  pump(e);
  const bool sync = io.written() == Bytes({0x16, 0x00, 0x00});
  io.clear_written();
  io.feed({0x06});
  pump(e);
  return eot && sync;
}

void p300_cases() {
  std::printf("-- P300 --\n");
  FakeOptolink io;
  optolink::P300Exchange<FakeOptolink> e(&io);
  hook_p300(e);
  e.begin();
  check(handshake(e, io), "P300 link up");

  // --- A ------------------------------------------------------------------
  int responses = g_responses;
  check(kw_calls::request(e, 0x05, 0x1000, 2), "A: request(05, 1000, 2) accepted");
  pump(e);
  check(io.written() == p300({0x05, 0x00, 0x05, 0x10, 0x00, 0x02}), "A: 41 05 00 05 10 00 02 1C");
  io.clear_written();
  io.feed(acked(p300({0x07, 0x01, 0x05, 0x10, 0x00, 0x02, 0xAA, 0xBB})));
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0xAA, 0xBB}) && g_address == 0x1000,
        "A: the answer AA BB, address 1000");
  io.clear_written();

  // --- B ------------------------------------------------------------------
  int errors = g_errors;
  check(kw_calls::request(e, 0x09, 0x4050, 1), "B: request(09, 4050, 1) accepted");
  pump(e);
  check(io.written() == p300({0x05, 0x00, 0x09, 0x40, 0x50, 0x01}), "B: 41 05 00 09 40 50 01 9F");
  io.clear_written();
  io.feed(acked(p300({0x06, 0x03, 0x09, 0x40, 0x50, 0x01, 0x05})));
  pump(e);
  check(g_errors == errors + 1 && g_last_error == optolink::OptolinkResult::DEVICE_ERROR,
        "B: the ERROR frame is DEVICE_ERROR");
  check(g_error_code == Bytes({0x05}), "B: error code 05");
  io.clear_written();

  errors = g_errors;
  check(kw_calls::request(e, 0x05, 0xFF05, 16), "B: request(05, FF05, 16) accepted");
  pump(e);
  io.clear_written();
  // DataLength 16, two payload bytes: the frame decides.
  io.feed(acked(p300({0x07, 0x03, 0x05, 0xFF, 0x05, 0x10, 0x31, 0x01})));
  pump(e);
  check(g_errors == errors + 1 && g_error_code == Bytes({0x31, 0x01}), "B: error code 31 01");
  io.clear_written();

  // --- C ------------------------------------------------------------------
  responses = g_responses;
  check(kw_calls::read(e, 0x00F8, 1), "C: read() accepted");
  pump(e);
  check(io.written() == p300({0x05, 0x00, 0x01, 0x00, 0xF8, 0x01}), "C: read() is request(01)'s frame");
  io.clear_written();
  io.feed(acked(p300({0x06, 0x01, 0x01, 0x00, 0xF8, 0x01, 0x20})));
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x20}), "C: answered");
  check(g_error_code.empty(), "C: its result carries no error code");
  io.clear_written();

  // An answer with another function code -- a write's acknowledgement for a
  // read -- is no answer: ERROR, nothing delivered.
  responses = g_responses;
  errors = g_errors;
  check(kw_calls::read(e, 0x00F8, 1), "C: read() accepted");
  pump(e);
  io.clear_written();
  io.feed(acked(p300({0x05, 0x01, 0x02, 0x00, 0xF8, 0x01})));
  pump(e);
  check(g_responses == responses && g_errors == errors + 1 && g_last_error == optolink::OptolinkResult::ERROR,
        "C: a write ack for a read is an ERROR");
  check(!e.busy(), "C: and the exchange is free");
  io.clear_written();

  // --- D ------------------------------------------------------------------
  check(!kw_calls::request(e, 0x00, 0x0000, 1), "D: code 0 refused");
  check(!kw_calls::request(e, 0x20, 0x0000, 1), "D: code 20 refused");
  check(!kw_calls::request(e, 0x02, 0x0000, 1), "D: a write code without payload refused");
  check(!e.busy(), "D: nothing in flight");

  // --- E ------------------------------------------------------------------
  responses = g_responses;
  check(kw_calls::kw_telegram(e, 0x7B, 0x0800, 2), "E: kwTelegram(7B, 0800, 2) accepted");
  pump(e);
  check(io.written() == Bytes({0x04}), "E: EOT leaves P300");
  io.clear_written();
  io.feed({0x06, 0x05});
  pump(e);
  check(io.written() == Bytes({0x01, 0x7B, 0x08, 0x00, 0x02}), "E: 01 7B 08 00 02 on the ENQ");
  io.clear_written();
  io.feed({0x12, 0x34});
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x12, 0x34}) && g_address == 0x0800,
        "E: 12 34 delivered, address 0800");

  // --- F ------------------------------------------------------------------
  responses = g_responses;
  const uint8_t data[] = {0x0A, 0x0B};
  check(kw_calls::kw_telegram(e, 0xF4, 0x2306, 2, data), "F: kwTelegram(F4, 2306, 0A 0B) accepted");
  pump(e);
  check(io.written() == Bytes({0xF4, 0x23, 0x06, 0x02, 0x0A, 0x0B}), "F: chained at once, payload carried");
  io.clear_written();
  io.feed({0x00});
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x00}), "F: completes on the one ack byte");
  // A request copies its payload, so the buffer holds what it claims.
  const uint8_t too_long[optolink::P300_KW_MAX_PAYLOAD + 1] = {};
  check(!kw_calls::kw_telegram(e, 0xF4, 0x2306, sizeof(too_long), too_long), "F: payload above KW_MAX_PAYLOAD refused");

  // A non-zero acknowledgement is no acknowledgement: DEVICE_ERROR, and back
  // to P300 through RESET.
  responses = g_responses;
  errors = g_errors;
  check(kw_calls::kw_telegram(e, 0xF4, 0x2306, 1, data), "F: another write on the session accepted");
  pump(e);
  io.clear_written();
  io.feed({0x05});
  pump(e);
  check(g_responses == responses && g_errors == errors + 1 && g_last_error == optolink::OptolinkResult::DEVICE_ERROR,
        "F: a non-zero ack is DEVICE_ERROR");
  check(io.written() == Bytes({0x04}), "F: and RESET sends EOT");
}

void kw_cases() {
  std::printf("-- KW --\n");
  FakeOptolink io;
  optolink::KwExchange<FakeOptolink, kw_calls::kNoPause> e(&io);
  hook(e);
  e.begin();

  // --- G ------------------------------------------------------------------
  int responses = g_responses;
  check(kw_calls::telegram(e, 0x7B, 0x0800, 2), "G: telegram(7B, 0800, 2) accepted");
  io.feed({0x05});
  pump(e);
  check(io.written() == Bytes({0x01, 0x7B, 0x08, 0x00, 0x02}), "G: 01 7B 08 00 02");
  io.clear_written();
  io.feed({0x12, 0x34});
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x12, 0x34}), "G: 12 34 delivered");

  responses = g_responses;
  const uint8_t data[] = {0x01};
  check(kw_calls::telegram(e, 0x68, 0x4050, 1, data), "G: telegram(68, 4050, 01) accepted");
  pump(e);
  check(io.written() == Bytes({0x68, 0x40, 0x50, 0x01, 0x01}), "G: 68 40 50 01 01 chained");
  io.clear_written();
  io.feed({0x00});
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x00}), "G: completes on the one ack byte");
  check(!kw_calls::telegram(e, 0x7B, 0x0800, 0), "G: zero length refused");

  // An ignored telegram: the controller's next ENQ, already waiting when the
  // deadline is checked, is not a one-byte answer but the ENQ the telegram
  // goes out again on.
  responses = g_responses;
  const int errors = g_errors;
  check(kw_calls::telegram(e, 0x7B, 0x0800, 1), "G: telegram(7B, 0800, 1) accepted");
  pump(e);
  check(io.written() == Bytes({0x7B, 0x08, 0x00, 0x01}), "G: 7B 08 00 01 chained");
  io.clear_written();
  optolink::optolink_test_clock_advance(kw_calls::kNoPause.answer_timeout_ms);
  pump(e);
  check(g_errors == errors, "G: no error at the answer deadline");
  io.feed({0x05});
  optolink::optolink_test_clock_advance(1);
  pump(e);
  // Silence until the deadline: the telegram goes out once more, on that ENQ.
  check(g_responses == responses && g_errors == errors, "G: the ENQ that follows is no answer");
  check(io.written() == Bytes({0x01, 0x7B, 0x08, 0x00, 0x01}), "G: the telegram again, 01 7B 08 00 01 on that ENQ");
  // An answer just inside the deadline is delivered.
  io.clear_written();
  optolink::optolink_test_clock_advance(kw_calls::kNoPause.answer_timeout_ms);
  io.feed({0x12});
  pump(e);
  check(g_responses == responses + 1 && g_payload == Bytes({0x12}), "G: an answer at the deadline is delivered");
}

// One GWG transaction on a fresh engine: a completed one re-opens the burst
// window, and a chained telegram would then go out before the ENQ fed here.
void gwg_one(uint8_t type, uint8_t address, uint8_t length, const uint8_t *data, const Bytes &frame,
             const Bytes &answer, const Bytes &delivered, const char *what_frame, const char *what_answer) {
  FakeOptolink io;
  optolink::GWGEngine e(&io);
  hook(e);
  e.begin();
  const int responses = g_responses;
  check(e.telegram(type, address, length, data), "  accepted");
  io.feed({0x05});
  pump(e);
  check(io.written() == frame, what_frame);
  io.clear_written();
  io.feed(answer);
  pump(e);
  check(g_responses == responses + 1 && g_payload == delivered, what_answer);
}

void gwg_cases() {
  std::printf("-- GWG --\n");

  // --- H ------------------------------------------------------------------
  gwg_one(0x31, 0x05, 1, nullptr, Bytes({0x01, 0x31, 0x05, 0x01, 0x04}), Bytes({0x7F}), Bytes({0x7F}),
          "H: 01 31 05 01 04", "H: 7F delivered");
  const uint8_t data[] = {0x42};
  gwg_one(0x99, 0x10, 1, data,
          optolink::GWGEngine::DEFAULT_WRITE_EOT_BEFORE_PAYLOAD ? Bytes({0x01, 0x99, 0x10, 0x01, 0x04, 0x42})
                                                                : Bytes({0x01, 0x99, 0x10, 0x01, 0x42, 0x04}),
          Bytes({0x00}), Bytes({0x00}), "H: with payload, the write layout", "H: completes on the one ack byte");
  {
    FakeOptolink io;
    optolink::GWGEngine e(&io);
    check(!e.telegram(0x31, 0x100, 1), "H: address above FF refused");
  }

  // --- I ------------------------------------------------------------------
  gwg_one(0xAE, 0x05, 1, nullptr, Bytes({0x01, 0xAE, 0x05, 0x01, 0x04}), Bytes({0xF0, 0x0F}), Bytes({0x0F}),
          "I: 01 AE 05 01 04", "I: the complement pair decoded to 0F");
}
}  // namespace

// Each builder takes payloads up to MAX_PAYLOAD; one byte more is refused.
void frame_limit_cases() {
  std::printf("-- frame limits --\n");
  uint8_t data[255];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = static_cast<uint8_t>(i);

  const uint8_t max = optolink::MAX_PAYLOAD;
  optolink::Telegram t{};
  // 41, LL .. the payload (6 + max), the checksum.
  check(optolink::p300_encode(0x02, 0x2000, max, data, t) && t.length == 8 + max,
        "P300: a MAX_PAYLOAD request payload");
  check(!optolink::p300_encode(0x02, 0x2000, max + 1, data, t), "P300: one byte more refused");

  check(optolink::kw_encode(0xF4, 0x2000, max, data, t) && t.length == 4 + max, "KW: MAX_PAYLOAD payload bytes");
  check(!optolink::kw_encode(0xF4, 0x2000, max + 1, data, t), "KW: one byte more refused");
  check(optolink::kw_encode(0xF7, 0x2000, 255, nullptr, t) && t.length == 4,
        "KW: a read is a 4-byte frame whatever its length");

  // The engine puts the 01 in front, so its frame is 5 + max.
  check(optolink::gwg_encode(0x99, 0x10, max, data, false, t) && t.length == 4 + max, "GWG: MAX_PAYLOAD payload bytes");
  check(!optolink::gwg_encode(0x99, 0x10, max + 1, data, false, t), "GWG: one byte more refused");

  FakeOptolink io;
  optolink::P300Exchange<FakeOptolink> vs2(&io);
  check(!kw_calls::read(vs2, 0x2000, max + 1) && !kw_calls::request(vs2, 0x05, 0x2000, max + 1),
        "P300: a read of MAX_PAYLOAD + 1 refused");
  check(kw_calls::read(vs2, 0x2000, max), "P300: a read of MAX_PAYLOAD accepted");
  optolink::KwExchange<FakeOptolink, kw_calls::kNoPause> kw(&io);
  check(!kw_calls::read(kw, 0x2000, max + 1) && kw_calls::read(kw, 0x2000, max), "KW: the same limit");
  optolink::GWGEngine gwg_engine(&io);
  check(!gwg_engine.read(0x10, max + 1) && gwg_engine.read(0x10, max), "GWG: the same limit");
}

int main() {
  std::printf("== generic telegrams (raw_op) ==\n");
  optolink::optolink_test_clock_freeze();
  p300_cases();
  kw_cases();
  gwg_cases();
  frame_limit_cases();
  std::printf("raw engines: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
