#pragma once

// The link's counts since the last reset: every request's result, its time
// from dispatch to result, the timed frames, and the facts the exchange
// reports (LinkFact). One `stats` line holds them
// (VitoHomeComponent::log_stats()), the line the field programs report
// (example/diagnostics/).

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "optolink_types.h"

namespace esphome::vitohome {

class VitoStats {
 public:
  void reset(uint32_t now_ms) {
    *this = VitoStats{};
    since_ms_ = now_ms;
  }

  void result(optolink::OptolinkResult status, uint32_t elapsed_ms) {
    const auto i = static_cast<uint8_t>(status);
    if (i < STATUSES)
      ++results_[i];
    ++requests_;
    if (status != optolink::OptolinkResult::OK)
      return;
    if (ok_ms_count_ == 0 || elapsed_ms < ok_ms_min_)
      ok_ms_min_ = elapsed_ms;
    if (elapsed_ms > ok_ms_max_)
      ok_ms_max_ = elapsed_ms;
    ok_ms_sum_ += elapsed_ms;
    ++ok_ms_count_;
  }

  void script(bool answered) { ++(answered ? scripts_ok_ : scripts_failed_); }

  void fact(uint8_t fact) {
    if (fact < optolink::LINK_FACTS)
      ++facts_[fact];
  }

  // Requests and timed frames that ended.
  uint32_t ended() const { return requests_ + scripts_ok_ + scripts_failed_; }
  uint32_t count(optolink::OptolinkResult status) const {
    const auto i = static_cast<uint8_t>(status);
    return i < STATUSES ? results_[i] : 0;
  }
  uint32_t facts(optolink::LinkFact fact) const { return facts_[static_cast<uint8_t>(fact)]; }

  // `s=<seconds> n=<requests> ok=.. timeout=.. length=.. nack=.. crc=..
  // error=.. device_error=.. ms=<min>/<avg>/<max> script_ok=.. script_failed=..
  // link.drained=.. link.retry=.. ...`: every key always, so lines compare
  // column by column; ms over the OK results, `-` without one.
  void format(char *line, std::size_t capacity, uint32_t now_ms) const {
    using optolink::OptolinkResult;
    std::size_t pos = 0;
    auto put = [&](const char *key, uint32_t value) {
      if (pos < capacity)
        pos += clamp_(std::snprintf(line + pos, capacity - pos, " %s=%u", key, static_cast<unsigned>(value)),
                      capacity - pos);
    };
    pos = clamp_(std::snprintf(line, capacity, "s=%u", static_cast<unsigned>((now_ms - since_ms_) / 1000U)), capacity);
    put("n", requests_);
    put("ok", this->count(OptolinkResult::OK));
    put("timeout", this->count(OptolinkResult::TIMEOUT));
    put("length", this->count(OptolinkResult::LENGTH));
    put("nack", this->count(OptolinkResult::NACK));
    put("crc", this->count(OptolinkResult::CRC));
    put("error", this->count(OptolinkResult::ERROR));
    put("device_error", this->count(OptolinkResult::DEVICE_ERROR));
    if (pos < capacity) {
      if (ok_ms_count_ == 0) {
        pos += clamp_(std::snprintf(line + pos, capacity - pos, " ms=-"), capacity - pos);
      } else {
        pos +=
            clamp_(std::snprintf(line + pos, capacity - pos, " ms=%u/%u/%u", static_cast<unsigned>(ok_ms_min_),
                                 static_cast<unsigned>(ok_ms_sum_ / ok_ms_count_), static_cast<unsigned>(ok_ms_max_)),
                   capacity - pos);
      }
    }
    put("script_ok", scripts_ok_);
    put("script_failed", scripts_failed_);
    for (uint8_t f = 0; f < optolink::LINK_FACTS; ++f) {
      char key[24] = "link.";
      const char *name = optolink::link_fact_name(static_cast<optolink::LinkFact>(f));
      std::size_t k = 5;
      for (std::size_t i = 0; name[i] != '\0' && k + 1 < sizeof(key); ++i, ++k)
        key[k] = name[i] == ' ' ? '_' : name[i];
      key[k] = '\0';
      put(key, facts_[f]);
    }
  }

 private:
  // OptolinkResult, CONTINUE to OK.
  static constexpr uint8_t STATUSES = 8;

  static std::size_t clamp_(int written, std::size_t room) {
    if (written < 0)
      return 0;
    const auto n = static_cast<std::size_t>(written);
    return n < room ? n : (room == 0 ? 0 : room - 1);
  }

  uint32_t since_ms_{0};
  uint32_t requests_{0};
  uint32_t results_[STATUSES]{};
  uint32_t ok_ms_min_{0};
  uint32_t ok_ms_max_{0};
  uint32_t ok_ms_count_{0};
  uint64_t ok_ms_sum_{0};
  uint32_t scripts_ok_{0};
  uint32_t scripts_failed_{0};
  uint32_t facts_[optolink::LINK_FACTS]{};
};

}  // namespace esphome::vitohome
