#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Plays the Optolink UART for host tests. Test fills inbound_ (device->ESP) via
// feed(); everything the engine writes (ESP->device) is captured in written_.
// Chunk boundaries across feed() calls model real UART fragmentation.
class FakeOptolink {
 public:
  bool begin() { return true; }
  void end() {}
  std::size_t write(const uint8_t* d, uint8_t n) {
    written_.insert(written_.end(), d, d + n);
    return n;
  }
  std::size_t available() const { return inbound_.size(); }
  uint8_t read() {
    if (inbound_.empty()) return 0;
    uint8_t b = inbound_.front();
    inbound_.pop_front();
    return b;
  }
  void feed(const std::vector<uint8_t>& bytes) { inbound_.insert(inbound_.end(), bytes.begin(), bytes.end()); }
  const std::vector<uint8_t>& written() const { return written_; }
  void clear_written() { written_.clear(); }

 private:
  std::deque<uint8_t> inbound_;
  std::vector<uint8_t> written_;
};
