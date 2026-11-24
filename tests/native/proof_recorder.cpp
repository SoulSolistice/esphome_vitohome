// Proves the recorder (vito_recorder.h):
//
//   (1) allocate() refuses a size or `after` out of range, a missing clock
//       and a second call;
//   (2) the ring keeps the newest entries, times relative to the oldest;
//   (3) freeze on a timeout: `after` more entries, then nothing; later
//       failures counted; the dump re-arms it;
//   (4) freeze on an error takes a NACK, freeze on a timeout does not;
//   (5) `manual`: no failure freezes it, the dump does;
//   (6) a dump line joins the bytes of one direction and pass, up to 24;
//       results and timed frames carry their status and address;
//   (7) on a port with the hub's hook, a KW read on the KW exchange: its
//       ENQ, 01 + telegram, answer and result, one pass each.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_optolink.h"
#include "kw_calls.h"
#include "optolink_kw.h"
#include "vito_recorder.h"

using esphome::vitohome::RECORDER_FREEZE_ERROR;
using esphome::vitohome::RECORDER_FREEZE_TIMEOUT;
using esphome::vitohome::VitoRecorder;
namespace optolink = esphome::vitohome::optolink;
using optolink::OptolinkResult;

namespace {

uint32_t g_us = 0;
uint32_t fake_micros() { return g_us; }
uint32_t engine_micros() { return optolink_millis() * 1000U; }

int g_failures = 0;
void check(bool ok, const char *what) {
  std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

std::vector<std::string> dump(VitoRecorder &rec) {
  std::vector<std::string> lines;
  if (!rec.start_dump())
    return lines;
  char line[VitoRecorder::LINE_CAPACITY];
  while (rec.next_dump_line(line, sizeof(line)))
    lines.emplace_back(line);
  return lines;
}

bool has(const std::vector<std::string> &lines, const char *text) {
  for (const auto &l : lines)
    if (l.find(text) != std::string::npos)
      return true;
  return false;
}

// The hub's port hook (vito_uart_interface.h) on a FakeOptolink: a write
// stamped before it is handed on, a read when it is taken.
class RecordingPort {
 public:
  RecordingPort(FakeOptolink *port, VitoRecorder *rec) : port_(port), rec_(rec) {}
  bool begin() { return port_->begin(); }
  void end() { port_->end(); }
  std::size_t write(const uint8_t *data, uint8_t length) {
    const uint32_t t = rec_->now();
    const std::size_t n = port_->write(data, length);
    rec_->tx(t, data, length);
    return n;
  }
  uint8_t read() {
    const uint8_t b = port_->read();
    rec_->rx(b);
    return b;
  }
  std::size_t available() { return port_->available(); }

 private:
  FakeOptolink *port_;
  VitoRecorder *rec_;
};

}  // namespace

int main() {
  std::printf("== recorder ==\n");

  // (1)
  {
    VitoRecorder rec;
    check(!rec.allocate(31, 0, 0, fake_micros), "(1) size 31 refused");
    check(!rec.allocate(4097, 0, 0, fake_micros), "(1) size 4097 refused");
    check(!rec.allocate(32, 32, 0, fake_micros), "(1) after == size refused");
    check(!rec.allocate(32, 0, 0, nullptr), "(1) no clock refused");
    check(!rec.enabled(), "(1) still off");
    check(rec.allocate(32, 8, 0, fake_micros), "(1) 32 entries, 8 after");
    check(!rec.allocate(64, 8, 0, fake_micros), "(1) a second allocate refused");
    rec.rx(0x05);
    check(rec.held() == 1, "(1) records once on");
  }

  // (2)
  {
    VitoRecorder rec;
    (void) rec.allocate(32, 0, 0, fake_micros);
    for (uint16_t i = 0; i < 40; ++i) {
      g_us = 1000U * i;
      rec.set_pass(i);
      rec.rx(static_cast<uint8_t>(i));
    }
    check(rec.held() == 32, "(2) 40 bytes into 32: 32 held");
    const auto lines = dump(rec);
    check(lines.size() == 33, "(2) the header and one line a pass");
    check(lines.size() > 1 && lines[1] == "rec +0.000 p8 <<< 08", "(2) the oldest is byte 8, at +0");
    check(lines.size() == 33 && lines[32] == "rec +31.000 p39 <<< 27", "(2) the newest is byte 39, at +31 ms");
    check(rec.held() == 0 && !rec.frozen(), "(2) the dump re-armed it");
  }

  // (3)
  {
    VitoRecorder rec;
    (void) rec.allocate(64, 4, RECORDER_FREEZE_TIMEOUT, fake_micros);
    g_us = 0;
    rec.rx(0x05);
    rec.result(OptolinkResult::OK, 0x00F8);
    check(!rec.frozen(), "(3) an OK does not freeze");
    rec.result(OptolinkResult::NACK, 0x00F8);
    check(!rec.frozen(), "(3) a NACK does not freeze on timeout");
    rec.result(OptolinkResult::TIMEOUT, 0x5525);
    const uint16_t at_trigger = rec.held();
    check(!rec.frozen(), "(3) a timeout: still recording its after-entries");
    const uint8_t four[] = {1, 2, 3, 4};
    rec.tx(0, four, 3);
    check(!rec.frozen(), "(3) 3 of 4 after it");
    rec.rx(0x05);
    check(rec.frozen() && rec.held() == at_trigger + 4, "(3) frozen after the 4th");
    rec.rx(0x06);
    rec.result(OptolinkResult::TIMEOUT, 0x5525);
    rec.result(OptolinkResult::TIMEOUT, 0x5525);
    check(rec.held() == at_trigger + 4, "(3) nothing recorded while frozen");
    check(rec.triggers_since() == 2, "(3) two timeouts since");
    const auto lines = dump(rec);
    check(!lines.empty() && lines[0] == "rec dump: 12 of 64 entries, frozen by a timeout, 2 failure(s) since",
          "(3) the header names the trigger and the count");
    check(has(lines, "= timeout @5525") && has(lines, "! a timeout"), "(3) the result and the trigger");
    check(!rec.frozen() && rec.held() == 0, "(3) re-armed after the dump");
    rec.result(OptolinkResult::TIMEOUT, 0x5525);
    check(!rec.frozen() && rec.triggers_since() == 0, "(3) a new timeout starts a new freeze");
  }

  // (4)
  {
    VitoRecorder rec;
    (void) rec.allocate(32, 0, RECORDER_FREEZE_ERROR, fake_micros);
    rec.result(OptolinkResult::OK, 0x0800);
    check(!rec.frozen(), "(4) error mask: OK goes on");
    rec.result(OptolinkResult::NACK, 0x0800);
    check(rec.frozen(), "(4) error mask, after 0: a NACK freezes at once");
    VitoRecorder both;
    (void) both.allocate(32, 0, RECORDER_FREEZE_ERROR | RECORDER_FREEZE_TIMEOUT, fake_micros);
    both.result(OptolinkResult::TIMEOUT, 0x0800);
    const auto lines = dump(both);
    check(!lines.empty() && lines[0].find("frozen by a timeout") != std::string::npos,
          "(4) both: a timeout is named a timeout");
  }

  // (5)
  {
    VitoRecorder rec;
    (void) rec.allocate(32, 0, 0, fake_micros);
    rec.result(OptolinkResult::TIMEOUT, 0x0800);
    rec.result(OptolinkResult::CRC, 0x0800);
    check(!rec.frozen() && rec.held() == 4, "(5) manual: failures recorded, nothing freezes");
    check(rec.start_dump() && rec.frozen(), "(5) the dump freezes it");
    check(!rec.start_dump(), "(5) a second dump while one runs refused");
    rec.rx(0x05);
    check(rec.held() == 4, "(5) nothing recorded while dumping");
    char line[VitoRecorder::LINE_CAPACITY];
    check(rec.next_dump_line(line, sizeof(line)) && std::strstr(line, "frozen by the dump") != nullptr,
          "(5) the header: frozen by the dump");
    while (rec.next_dump_line(line, sizeof(line))) {
    }
    check(!rec.frozen() && rec.held() == 0, "(5) re-armed after the dump");
  }

  // (6)
  {
    VitoRecorder rec;
    (void) rec.allocate(128, 0, 0, fake_micros);
    g_us = 5000;
    rec.set_pass(1);
    const uint8_t start[] = {0x41};
    const uint8_t body[] = {0x05, 0x00, 0x01, 0x00, 0xF8, 0x02};
    rec.tx(g_us, start, sizeof(start));
    g_us = 255123;
    rec.set_pass(2);
    rec.tx(g_us, body, sizeof(body));
    const uint8_t cs[] = {0x00};
    rec.tx(g_us + 2, cs, 1);
    g_us = 300000;
    rec.set_pass(3);
    rec.rx(0x15);
    rec.rx(0x15);
    rec.result(OptolinkResult::NACK, 0x00F8, true);
    rec.set_pass(4);
    std::vector<uint8_t> long_write(30, 0xAA);
    rec.tx(g_us, long_write.data(), long_write.size());
    const auto lines = dump(rec);
    check(lines.size() == 7, "(6) header, 41, rest, NACKs, result, 24 + 6 bytes");
    check(lines.size() > 1 && lines[1] == "rec +0.000 p1 >>> 41", "(6) the 41 alone in its pass");
    check(lines.size() > 2 && lines[2] == "rec +250.123 p2 >>> 05:00:01:00:F8:02:00",
          "(6) the rest 250.123 ms later, two writes of one pass joined");
    check(lines.size() > 3 && lines[3] == "rec +295.000 p3 <<< 15:15", "(6) the two NACK bytes on one line");
    check(lines.size() > 4 && lines[4] == "rec +295.000 p3 = script nack @00F8", "(6) a timed frame's result");
    check(lines.size() > 6 && std::count(lines[5].begin(), lines[5].end(), ':') == 23 &&
              lines[6] == "rec +295.000 p4 >>> AA:AA:AA:AA:AA:AA",
          "(6) a 30-byte write: 24 on a line, the rest on the next");
  }

  // (7)
  {
    optolink::optolink_test_clock_freeze();
    VitoRecorder rec;
    (void) rec.allocate(64, 0, RECORDER_FREEZE_TIMEOUT, engine_micros);
    FakeOptolink uart;
    RecordingPort port(&uart, &rec);
    optolink::KwExchange<RecordingPort, kw_calls::kNoPause> kw(&port);
    kw.on_result([&rec](const optolink::Result &r) { rec.result(r.status, r.address); });
    kw.begin();
    uint16_t pass = 0;
    auto pump = [&](int n) {
      for (int i = 0; i < n; ++i) {
        rec.set_pass(++pass);
        kw.loop();
      }
    };
    check(kw_calls::read(kw, 0x00F8, 2), "(7) KW read accepted");
    uart.feed({0x05});
    pump(1);
    optolink::optolink_test_clock_advance(30);
    uart.feed({0x20, 0xCB});
    pump(3);
    const auto lines = dump(rec);
    check(lines.size() == 5, "(7) header, ENQ, telegram, answer, result");
    check(lines.size() > 1 && lines[1] == "rec +0.000 p1 <<< 05", "(7) the ENQ");
    check(lines.size() > 2 && lines[2] == "rec +0.000 p1 >>> 01:F7:00:F8:02", "(7) 01 + telegram in its pass");
    check(lines.size() > 3 && lines[3] == "rec +30.000 p2 <<< 20:CB", "(7) the answer 30 ms later");
    check(lines.size() > 4 && lines[4] == "rec +30.000 p2 = ok @00F8", "(7) its result");
  }

  std::printf("recorder: %d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
