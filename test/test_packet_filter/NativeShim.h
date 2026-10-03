// NativeShim.h — host-test support for the repeater packet filter, force-
// included by [env:native_packet_filter] in platformio.ini.
//
// PacketFilter (and the core helpers it pulls in) use the Arduino-flavoured
// FILESYSTEM/File typedefs and ltoa(); neither exists on the host. This shim
// provides minimal in-memory stand-ins so the filter code builds unmodified.
//
// Must be force-included (-include) BEFORE any filter/core header, because
// PacketFilter.h declares FILESYSTEM* parameters.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

char* ltoa(long value, char* str, int base);   // Arduino libc extra; defined in NativeTestStubs.cpp

// In-memory stand-in for the Arduino File class (only the operations
// FilterRules::load()/save() use). Writes go straight to the owning NativeFS
// entry so saved data is visible to later exists()/open() calls.
class NativeFile {
public:
  std::vector<uint8_t>* backing = nullptr;
  size_t pos = 0;
  bool valid = false;
  bool fs_fail_write = false;   // set from the owning store's fail_write

  operator bool() const { return valid; }

  size_t read(uint8_t* buf, size_t len) {
    // a zero-length or past-EOF read must not form a data pointer: an empty
    // backing store has data() == nullptr, and memcpy() from it is UB even for
    // n == 0
    if (backing == nullptr || pos >= backing->size() || len == 0) return 0;
    size_t n = len < (backing->size() - pos) ? len : (backing->size() - pos);
    memcpy(buf, backing->data() + pos, n);
    pos += n;
    return n;
  }

  size_t write(const uint8_t* buf, size_t len) {
    if (backing == nullptr || fs_fail_write) return 0;
    backing->insert(backing->end(), buf, buf + len);
    pos = backing->size();   // append-only shim: the handle ends up at EOF
    return len;
  }

  void flush() {}
  void close() {}
};

// In-memory stand-in for the Arduino FS class (exists/remove/mkdir/open).
class NativeFS {
public:
  std::map<std::string, std::vector<uint8_t>> files;

  bool exists(const char* path) { return files.count(path) > 0; }
  void remove(const char* path) { files.erase(path); }
  void mkdir(const char*) {}

  // Fault injection for the persistence tests: a real filesystem can fail to
  // open or to write, and the retry/recovery paths must be exercised, not
  // assumed. Off by default so every other test sees a working store.
  bool fail_open = false;    // open() for writing returns an invalid handle
  bool fail_write = false;   // write() accepts nothing
  // how many times a write-open was attempted, so a test can see the retry
  // cadence rather than infer it from whether a file appeared
  size_t write_open_attempts = 0;
  // truncate a file to `keep` bytes after a successful open+write, modelling a
  // power loss partway through the record
  size_t truncate_to = SIZE_MAX;

  NativeFile open(const char* path) {
    NativeFile f;
    auto it = files.find(path);
    if (it != files.end()) { f.backing = &it->second; f.valid = true; }
    return f;
  }
  // Arduino (RP2040) flavour: "r" reads, "w" creates/truncates. Honouring the
  // mode is what lets a test exercise the RP2040 open paths.
  NativeFile open(const char* path, const char* mode) {
    if (mode != nullptr && mode[0] == 'w') {
      write_open_attempts++;
      if (fail_open) return NativeFile();
      files[path].clear();
      NativeFile f;
      f.backing = &files[path];
      f.valid = true;
      f.fs_fail_write = fail_write;
      return f;
    }
    return open(path);
  }

  bool rename(const char* from, const char* to) {
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(it);
    return true;
  }

  // Arduino (ESP32) flavour: open(..., "w", true) truncates/creates.
  NativeFile open(const char* path, const char* mode, bool truncate) {
    write_open_attempts++;
    if (fail_open) return NativeFile();   // nothing written: an invalid handle
    files[path].clear();
    NativeFile f;
    f.backing = &files[path];
    f.valid = true;
    f.fs_fail_write = fail_write;
    return f;
  }
};

using FILESYSTEM = NativeFS;
using File = NativeFile;
