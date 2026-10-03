// PersistUtil.h — persistence helpers shared by the simple_repeater fork's
// saved configs (filter and battery). Fork-owned code; the lazy dirty-save
// pattern and the per-platform file opens used to be spelled out in each
// config's markDirty()/loop()/load()/save(), which meant keeping several
// copies of the same platform #if blocks in step. New persisted configs
// should use these instead (CliUtil.h does the same for the CLI helpers).

#ifndef _PERSIST_UTIL_H
#define _PERSIST_UTIL_H

#include <Arduino.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef

// Every config saves lazily: a mutation marks it dirty and loop() writes it
// once this delay has passed (the ClientACL pattern), so a burst of CLI edits
// costs one flash write instead of one per edit.
#define LAZY_SAVE_DELAY_MS 3000

// Dirty flag plus write-back timer for one persisted config. Clear it only
// after the config has actually been written (see Config::save callers).
class LazySave {
  bool dirty;
  unsigned long dirty_since;
public:
  LazySave() : dirty(false), dirty_since(0) {}
  void markDirty() { dirty = true; dirty_since = millis(); }
  bool due() const { return dirty && millis() - dirty_since >= LAZY_SAVE_DELAY_MS; }
  void clear() { dirty = false; }
};

// Open for reading, whatever the platform's default mode is.
inline File fsOpenRead(FILESYSTEM* fs, const char* name) {
#if defined(RP2040_PLATFORM)
  return fs->open(name, "r");
#else
  return fs->open(name);
#endif
}

// Open for writing, replacing whatever is there: the platforms that cannot
// truncate in place want the old file removed first.
inline File fsOpenWrite(FILESYSTEM* fs, const char* name) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(name);
  return fs->open(name, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(name, "w");
#else
  return fs->open(name, "w", true);
#endif
}

#endif // _PERSIST_UTIL_H
