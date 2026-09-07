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

  operator bool() const { return valid; }

  size_t read(uint8_t* buf, size_t len) {
    size_t n = len < (backing->size() - pos) ? len : (backing->size() - pos);
    memcpy(buf, backing->data() + pos, n);
    pos += n;
    return n;
  }

  size_t write(const uint8_t* buf, size_t len) {
    backing->insert(backing->end(), buf, buf + len);
    return len;
  }

  void close() {}
};

// In-memory stand-in for the Arduino FS class (exists/remove/mkdir/open).
class NativeFS {
public:
  std::map<std::string, std::vector<uint8_t>> files;

  bool exists(const char* path) { return files.count(path) > 0; }
  void remove(const char* path) { files.erase(path); }
  void mkdir(const char*) {}

  NativeFile open(const char* path) {
    NativeFile f;
    auto it = files.find(path);
    if (it != files.end()) { f.backing = &it->second; f.valid = true; }
    return f;
  }

  NativeFile open(const char* path, const char* mode) { return open(path); }

  // Arduino (ESP32) flavour: open(..., "w", true) truncates/creates.
  NativeFile open(const char* path, const char* mode, bool truncate) {
    files[path].clear();
    NativeFile f;
    f.backing = &files[path];
    f.valid = true;
    return f;
  }
};

using FILESYSTEM = NativeFS;
using File = NativeFile;
