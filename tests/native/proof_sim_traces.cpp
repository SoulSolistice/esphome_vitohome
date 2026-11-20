// The simulators against real frame logs (sim/trace_*.h, made by
// sim/import_frames.py). A replay plays the engine: it sends each logged >>>
// line to a simulator and checks that the simulator sends the logged <<<
// bytes before it, in the same order. Before each request the simulator's
// memory is seeded from the answer the log shows, and a request the log
// shows unanswered is ignored, so what is checked is the protocol: the ACKs,
// the framing, lengths and checksums, the EEPROM pairs, the ENQs between
// telegrams. Times are not compared: the frame log merges bytes that arrive
// within 30 ms. The simulators' timing defaults come from the same logs.
//
// An answer the simulator cannot produce -- a garbled EEPROM pair, a NACK for
// a good frame, an answer cut short -- is counted as unmodelled and not
// compared; the device's state after it is the simulator's own.

#include <cstdio>
#include <string>
#include <vector>

#include "optolink/helpers.h"
#include "sim/sim_gwg.h"
#include "sim/sim_vitotronic.h"
#include "sim/trace_gwg_35.h"
#include "sim/trace_p300_vscotho1.h"

namespace optolink = esphome::vitohome::optolink;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  std::printf("  %-70s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_fail;
}

using Bytes = std::vector<uint8_t>;

Bytes line_bytes(const sim::Trace &t, std::size_t i) {
  const sim::TraceLine &l = t.lines[i];
  return Bytes(t.bytes + l.offset, t.bytes + l.offset + l.length);
}

std::string hex(const Bytes &b) {
  std::string s;
  char x[4];
  for (std::size_t i = 0; i < b.size() && i < 24; ++i) {
    std::snprintf(x, sizeof(x), " %02X", b[i]);
    s += x;
  }
  if (b.size() > 24)
    s += " ...";
  return s.empty() ? " (none)" : s;
}

enum class Seed : uint8_t { COMPARE, UNANSWERED, UNMODELLED };

struct Tally {
  int compared = 0;
  int matched = 0;
  int unanswered = 0;
  int unmodelled = 0;
  int shown = 0;
};

// The device lines after engine line `i`, up to the next engine line.
Bytes reply_after(const sim::Trace &t, std::size_t i, std::vector<Bytes> *lines = nullptr) {
  Bytes reply;
  for (std::size_t k = i + 1; k < t.count && t.lines[k].device; ++k) {
    const Bytes b = line_bytes(t, k);
    reply.insert(reply.end(), b.begin(), b.end());
    if (lines != nullptr)
      lines->push_back(b);
  }
  return reply;
}

template<class Dev, class SeedFn> void replay(const sim::Trace &t, Dev &dev, SeedFn seed, Tally &tally) {
  Bytes got;
  Bytes expected;
  bool discard = false;
  std::size_t segment_line = 0;
  auto settle = [&]() {
    // Until the simulator has sent as many bytes as the log shows, or 10 s of
    // its time passed.
    for (uint32_t ms = 0; ms < 10000 && got.size() < expected.size(); ++ms) {
      while (dev.available())
        got.push_back(dev.read());
      if (got.size() >= expected.size())
        break;
      optolink::optolink_test_clock_advance(1);
    }
    if (discard) {
      for (uint32_t ms = 0; ms < 200; ++ms) {
        while (dev.available())
          dev.read();
        optolink::optolink_test_clock_advance(1);
      }
      got.clear();
      discard = false;
      expected.clear();
      return;
    }
    if (!expected.empty()) {
      ++tally.compared;
      const std::size_t n = expected.size() < got.size() ? expected.size() : got.size();
      const Bytes head(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
      if (head == expected) {
        ++tally.matched;
      } else if (tally.shown++ < 4) {
        std::printf("    %s line %zu: log%s\n      simulator%s\n", t.name, segment_line, hex(expected).c_str(),
                    hex(head).c_str());
      }
      got.erase(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n));
    }
    expected.clear();
  };
  for (std::size_t i = 0; i < t.count; ++i) {
    if (t.lines[i].device) {
      const Bytes b = line_bytes(t, i);
      if (expected.empty())
        segment_line = i;
      expected.insert(expected.end(), b.begin(), b.end());
      continue;
    }
    settle();
    // Engine lines with no device line between them are one request, written
    // in parts at their logged gaps (#35's 01, then the rest 36 ms later).
    std::size_t last = i;
    while (last + 1 < t.count && !t.lines[last + 1].device)
      ++last;
    Bytes request;
    for (std::size_t k = i; k <= last; ++k) {
      const Bytes part = line_bytes(t, k);
      request.insert(request.end(), part.begin(), part.end());
    }
    const Seed action = seed(dev, t, last, request);
    dev.ignore_next_telegram(action == Seed::UNANSWERED);
    tally.unanswered += action == Seed::UNANSWERED ? 1 : 0;
    if (action == Seed::UNMODELLED) {
      ++tally.unmodelled;
      discard = true;
    }
    for (std::size_t k = i; k <= last; ++k) {
      if (k != i) {
        const uint32_t gap = t.lines[k].t_ms - t.lines[k - 1].t_ms;
        optolink::optolink_test_clock_advance(gap < 1000 ? gap : 1000);
      }
      const Bytes part = line_bytes(t, k);
      dev.write(part.data(), static_cast<uint8_t>(part.size()));
    }
    i = last;
  }
  settle();
}

// P300: seeds a read from its answer frame, an error frame's address as
// unserved, a procedure call's answer. `i` is the request's last line.
Seed seed_p300(sim::SimVitotronic &dev, const sim::Trace &t, std::size_t i, const Bytes &request) {
  // The engine's 06 for the previous answer may lead the merged lines.
  std::size_t start = 0;
  while (start < request.size() && request[start] == 0x06)
    ++start;
  const Bytes m(request.begin() + static_cast<std::ptrdiff_t>(start), request.end());
  if (m.size() < 8 || m[0] != 0x41)
    return Seed::COMPARE;
  const Bytes reply = reply_after(t, i);
  const uint16_t address = static_cast<uint16_t>(m[4] << 8 | m[5]);
  const uint8_t function = m[3] & 0x1F;
  if (reply.empty())
    return Seed::UNANSWERED;
  if (reply[0] != 0x06 || reply.size() < 9 || reply[1] != 0x41)
    return Seed::UNMODELLED;  // a NACK for a good frame, or no answer frame
  const uint8_t ll = reply[2];
  if (reply.size() < static_cast<std::size_t>(ll) + 4)
    return Seed::UNMODELLED;
  const uint8_t type = reply[3];
  if (type == 0x03) {
    if (reply[7] != 0x01 || reply[8] != 0x01)
      return Seed::UNMODELLED;  // only "unserved" is a configured error
    dev.config().unserved.emplace_back(address, address);
  } else if (function == 0x01) {
    for (uint8_t k = 0; k < reply[7]; ++k)
      dev.memory.set(static_cast<uint16_t>(address + k), reply[8 + k]);
  } else if (function == 0x07) {
    dev.config().rpc_answer.assign(reply.begin() + 8, reply.begin() + 3 + ll);
  }
  return Seed::COMPARE;
}

// GWG: seeds a read from its answer, EEPROM pairs as cells or unserved.
// `i` is the request's last line.
Seed seed_gwg(sim::SimGwg &dev, const sim::Trace &t, std::size_t i, const Bytes &m) {
  const bool paced = !m.empty() && m[0] == 0x01;
  const Bytes body(m.begin() + (paced ? 1 : 0), m.end());
  if (body.size() < 4 || body[3] != 0x04)
    return Seed::UNMODELLED;  // a write or an open frame: not in these logs
  static constexpr uint8_t kRead[8] = {0xCB, 0xC7, 0xAE, 0xC5, 0x6E, 0x9E, 0x33, 0x43};
  int mode = -1;
  for (int k = 0; k < 8; ++k) {
    if (kRead[k] == body[0])
      mode = k;
  }
  if (mode < 0)
    return Seed::UNMODELLED;
  const uint8_t address = body[1];
  const uint8_t length = body[2];
  const std::size_t want = mode == 2 ? 2u * length : length;
  std::vector<Bytes> lines;
  const Bytes reply = reply_after(t, i, &lines);
  // An answer comes in one frame line; a lone ENQ, or nothing, is none.
  if (lines.empty() || (lines[0] == Bytes{0x05} && want != 1))
    return Seed::UNANSWERED;
  if (reply.size() < want)
    return Seed::UNMODELLED;
  for (uint8_t k = 0; k < length; ++k) {
    const uint8_t a = static_cast<uint8_t>(address + k);
    if (mode == 2) {
      const uint8_t c = reply[2 * k];
      const uint8_t v = reply[2 * k + 1];
      if (c == 0xFF && v == 0xFF) {
        dev.config().eeprom_unserved[a] = true;
      } else if (static_cast<uint8_t>(c + v) == 0xFF) {
        dev.config().eeprom_unserved[a] = false;
        dev.memory[2][a] = v;
      } else {
        return Seed::UNMODELLED;  // a garbled pair
      }
    } else {
      dev.memory[static_cast<std::size_t>(mode)][a] = reply[k];
    }
  }
  return Seed::COMPARE;
}

void report(const sim::Trace &t, const Tally &tally) {
  std::printf("  %s: %zu lines, %d segments compared, %d matched; %d unanswered, %d unmodelled\n", t.name, t.count,
              tally.compared, tally.matched, tally.unanswered, tally.unmodelled);
  char what[96];
  std::snprintf(what, sizeof(what), "%s: every compared segment as logged", t.name);
  check(tally.compared > 0 && tally.matched == tally.compared, what);
}

}  // namespace

int main() {
  optolink::optolink_test_clock_freeze();
  std::printf("-- P300: %s --\n", kTrace_p300_vscotho1.source);
  {
    sim::SimVitotronic dev;
    Tally tally;
    replay(kTrace_p300_vscotho1, dev, seed_p300, tally);
    report(kTrace_p300_vscotho1, tally);
  }
  std::printf("-- GWG: %s --\n", kTrace_gwg_35.source);
  {
    sim::SimGwg dev;
    Tally tally;
    replay(kTrace_gwg_35, dev, seed_gwg, tally);
    report(kTrace_gwg_35, tally);
  }
  std::printf("simulator traces: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
