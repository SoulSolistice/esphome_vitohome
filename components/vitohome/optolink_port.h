// Include guards, not #pragma once: see optolink_types.h.
#ifndef VITOHOME_OPTOLINK_PORT_H
#define VITOHOME_OPTOLINK_PORT_H

// The port an engine drives: any object with begin(), end(), write(), read()
// and available(), the hub's ESPHomeUARTInterface on a device, a FakeOptolink
// or a simulator in the native tests (docs/design_notes.md, section 3).
//
// PortRef refers to it without owning it: a pointer and one static table of
// functions per port type, so no allocation, no virtual base for the port to
// derive from, and nothing that can fail at construction. The port must
// outlive the engine.

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace esphome::vitohome::optolink {

class PortRef {
 public:
  template<class Port> explicit PortRef(Port *port) : port_(port), ops_(&kOps<Port>) { assert(port != nullptr); }

  bool begin() const { return ops_->begin(port_); }
  void end() const { ops_->end(port_); }
  std::size_t write(const uint8_t *data, uint8_t length) const { return ops_->write(port_, data, length); }
  uint8_t read() const { return ops_->read(port_); }
  std::size_t available() const { return ops_->available(port_); }

 private:
  struct Ops {
    bool (*begin)(void *);
    void (*end)(void *);
    std::size_t (*write)(void *, const uint8_t *, uint8_t);
    uint8_t (*read)(void *);
    std::size_t (*available)(void *);
  };

  template<class Port>
  static constexpr Ops kOps{
      [](void *p) -> bool { return static_cast<Port *>(p)->begin(); },
      [](void *p) { static_cast<Port *>(p)->end(); },
      [](void *p, const uint8_t *data, uint8_t length) -> std::size_t {
        return static_cast<Port *>(p)->write(data, length);
      },
      [](void *p) -> uint8_t { return static_cast<Port *>(p)->read(); },
      [](void *p) -> std::size_t { return static_cast<Port *>(p)->available(); },
  };

  void *port_;
  const Ops *ops_;
};

}  // namespace esphome::vitohome::optolink

#endif  // VITOHOME_OPTOLINK_PORT_H
