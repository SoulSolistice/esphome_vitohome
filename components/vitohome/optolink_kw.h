// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_KW_H
#define VITOHOME_OPTOLINK_KW_H

// The KW stack: the KW codec on an ENQ-paced link, in an exchange.
//
//   ENQ hold         50 ms: the master answers inside the device's sync window
//   chain window     500 ms after an answer: a VScotHO1_72 answers a chained
//                    telegram up to 0.91 s after its answer and ignores one
//                    from 0.95 s (2026-11-20); later work waits for the ENQ,
//                    2.97 s after the answer
//   answer deadline  1 s of silence while an answer is due; a telegram met
//                    by silence goes out once more on the next ENQ (a
//                    VScotHO1_72 left 17 of 6066 unanswered, 2026-11-21)
//   same pass        an ENQ is answered, and a telegram chained, in the pass
//                    that allows it
//   EOT              after 4 s without an ENQ, past that 2.97 s
//   watchdog         4 s per request, from submit, an open frame's send or a
//                    retry

#include <cstdint>

#include "optolink_codec_kw.h"
#include "optolink_exchange.h"
#include "optolink_link_enq.h"

namespace esphome::vitohome::optolink {

inline constexpr EnqLinkParams KW_LINK_PARAMS{50, 500, 1000, 4000, true, 1};
inline constexpr uint32_t KW_WATCHDOG_MS = 4000;

// `Params`: the link's timing, KW_LINK_PARAMS but in the tests.
template<class Port, const EnqLinkParams &Params = KW_LINK_PARAMS>
class KwExchange : public Exchange<KwCodec, EnqLink<Port>> {
 public:
  explicit KwExchange(Port *port) : Exchange<KwCodec, EnqLink<Port>>(port, Params, KW_WATCHDOG_MS) {}
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_KW_H
