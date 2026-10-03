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

// Dirty flag plus write-back timer for one persisted config.
//
// Elapsed time is computed in uint32_t so that a millis() wrap is handled for a
// full cycle. The host's `unsigned long` is 64-bit, so writing these fields as
// unsigned long would silently make host tests unable to model the wrap the MCU
// actually has.
class LazySave {
  bool dirty;
  uint32_t dirty_since;
public:
  LazySave() : dirty(false), dirty_since(0) {}
  void markDirty() { dirty = true; dirty_since = millis(); }
  bool due() const { return dirty && (uint32_t)(millis() - dirty_since) >= LAZY_SAVE_DELAY_MS; }

  // The config reached the disk. Only call this after a confirmed write.
  void clear() { dirty = false; }

  // A save attempt failed: stay dirty, but wait out another full delay before
  // trying again. Without this, due() stays true on every loop iteration and a
  // full or failing filesystem is opened and truncated at firmware-loop speed.
  void retryLater() { if (dirty) dirty_since = millis(); }

  // Forget pending work entirely — a reload or a reset-to-defaults, which must
  // not inherit an edit the file no longer reflects.
  void reset() { dirty = false; dirty_since = 0; }
};

// Config integrity is length-checked, not checksummed: a truncated file is
// detected on load, but a torn write that happens to be exactly the right
// length is not. That is deliberate — no record checksum, because the on-disk
// format is frozen, and no sidecar file, because a second file is a second
// thing to go missing. A checksum belongs in the record, and the record can only
// change when the config version does: when a v7 is needed anyway, put it
// there. Every config this firmware writes must keep loading without one.

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
