#pragma once

// Recorded outputs a proof is held to: tests/native/fixtures/<name>.txt, one
// case per "== <key>" line followed by the lines of its output. A run compares
// what it produces with the file; with VITOHOME_RECORD=1 in the environment it
// writes the file instead. A deliberate change to the wire is re-recorded and
// reviewed as the file's diff.
//
// The directory is fixtures/ next to the working directory, or
// $VITOHOME_FIXTURES.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Golden {
 public:
  using Lines = std::vector<std::string>;

  explicit Golden(const char *name) {
    const char *dir = std::getenv("VITOHOME_FIXTURES");
    this->path_ = std::string(dir != nullptr ? dir : "fixtures") + "/" + name + ".txt";
    const char *record = std::getenv("VITOHOME_RECORD");
    this->recording_ = record != nullptr && std::strcmp(record, "1") == 0;
    if (!this->recording_)
      this->load_();
  }

  bool recording() const { return this->recording_; }

  // Empty, or what differs from the recorded case.
  std::string check(const std::string &key, const Lines &lines) {
    if (!this->seen_.insert(key).second)
      return "case recorded twice: " + key;
    if (this->recording_) {
      this->order_.emplace_back(key, lines);
      return {};
    }
    const auto it = this->cases_.find(key);
    if (it == this->cases_.end())
      return "no recorded case (VITOHOME_RECORD=1 records it): " + key;
    const Lines &want = it->second;
    const std::size_t n = want.size() < lines.size() ? want.size() : lines.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (want[i] != lines[i])
        return "line " + std::to_string(i + 1) + ": recorded \"" + want[i] + "\", now \"" + lines[i] + "\"";
    }
    if (want.size() != lines.size()) {
      return want.size() > lines.size() ? "ends early; recorded line " + std::to_string(n + 1) + ": \"" + want[n] + "\""
                                        : "goes on; line " + std::to_string(n + 1) + ": \"" + lines[n] + "\"";
    }
    return {};
  }

  // Writes the file when recording; else the recorded cases no run checked,
  // each printed. Returns the number of problems.
  int finish() {
    if (this->recording_) {
      std::FILE *f = std::fopen(this->path_.c_str(), "wb");
      if (f == nullptr) {
        std::printf("  cannot write %s\n", this->path_.c_str());
        return 1;
      }
      for (const auto &c : this->order_) {
        std::fprintf(f, "== %s\n", c.first.c_str());
        for (const std::string &l : c.second)
          std::fprintf(f, "%s\n", l.c_str());
      }
      std::fclose(f);
      std::printf("  recorded %zu case(s) to %s\n", this->order_.size(), this->path_.c_str());
      return 0;
    }
    int stale = 0;
    for (const auto &c : this->cases_) {
      if (this->seen_.count(c.first) == 0) {
        std::printf("  recorded case no run checked: %s\n", c.first.c_str());
        ++stale;
      }
    }
    if (this->cases_.empty()) {
      std::printf("  no recorded cases in %s\n", this->path_.c_str());
      ++stale;
    }
    return stale;
  }

  std::size_t cases() const { return this->recording_ ? this->order_.size() : this->cases_.size(); }

 private:
  void load_() {
    std::FILE *f = std::fopen(this->path_.c_str(), "rb");
    if (f == nullptr)
      return;
    std::string line;
    Lines *current = nullptr;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
      if (c == '\r')
        continue;
      if (c != '\n') {
        line += static_cast<char>(c);
        continue;
      }
      if (line.rfind("== ", 0) == 0) {
        current = &this->cases_[line.substr(3)];
      } else if (current != nullptr) {
        current->push_back(line);
      }
      line.clear();
    }
    std::fclose(f);
  }

  std::string path_;
  bool recording_{false};
  std::map<std::string, Lines> cases_;
  std::vector<std::pair<std::string, Lines>> order_;
  std::set<std::string> seen_;
};

// "AA BB CC", or "-" for none.
inline std::string golden_hex(const uint8_t *bytes, std::size_t n) {
  if (n == 0)
    return "-";
  std::string out;
  out.reserve(n * 3);
  char hex[4];
  for (std::size_t i = 0; i < n; ++i) {
    std::snprintf(hex, sizeof(hex), i == 0 ? "%02X" : " %02X", bytes[i]);
    out += hex;
  }
  return out;
}
