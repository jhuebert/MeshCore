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

// ---------------------------------------------------------------- staged save
//
// Truncating the live config in place destroys the only good copy the moment a
// write fails or power drops — and a battery-powered repeater is exactly where
// brownouts happen. So a save is staged: write a scratch file, read it BACK and
// validate it, keep the previous good file as a backup, and only then promote.
// The caller keeps its dirty flag until this reports success.
//
// Config record bytes are unchanged. This is machinery around the file, not a
// new format: `.tmp` and `.bak` are filesystem state, and neither is ever read
// as a config on its own terms (a boot ignores `.tmp` entirely — an uncommitted
// scratch file is not a config).
//
// Hardware note: this sequence is recoverable at the application level. It is
// not a proof of atomicity against a brownout at the filesystem layer.

#define PERSIST_TMP_SUFFIX ".tmp"
#define PERSIST_BAK_SUFFIX ".bak"

// `name` + suffix, bounded.
inline void persistPath(char* dst, size_t sz, const char* name, const char* suffix) {
  if (sz == 0) return;
  size_t n = strlen(name);
  size_t s = strlen(suffix);
  if (n + s >= sz) { dst[0] = 0; return; }
  memcpy(dst, name, n);
  memcpy(dst + n, suffix, s + 1);
}

// Write one config file's records, and validate a file's records. Both get an
// open File by reference and the caller's context; neither may depend on live
// feature state, because validation runs on a scratch file that must be checked
// without disturbing counters, rate history or suspension.
typedef bool (*PersistWriteFn)(File& f, void* ctx);
typedef bool (*PersistValidateFn)(File& f, void* ctx);

// Save `name` as a staged transaction. Returns true only once the new canonical
// file is committed AND read back as valid; on any failure the caller must keep
// its dirty flag and retry later. Either the complete old config or the complete
// new one remains loadable — never a mixture.
// Three questions, three callbacks, because conflating them is a bug waiting to
// happen: `writeFn` writes records; `writebackFn` checks the scratch file holds
// exactly what we just wrote; `usableFn` only asks whether an existing file is a
// config this firmware could load at all — used for the canonical and backup, so
// a stale-but-valid config is never mistaken for an unusable one.
inline bool saveStaged(FILESYSTEM* fs, const char* name,
                       PersistWriteFn writeFn, void* write_ctx,
                       PersistValidateFn writebackFn, void* writeback_ctx,
                       PersistValidateFn usableFn, void* usable_ctx) {
  char tmp[64], bak[64];
  persistPath(tmp, sizeof(tmp), name, PERSIST_TMP_SUFFIX);
  persistPath(bak, sizeof(bak), name, PERSIST_BAK_SUFFIX);

  // 1. write the scratch file. fsOpenWrite() recreates it, which the platforms
  //    that append to an existing file require.
  File f = fsOpenWrite(fs, tmp);
  if (!f) return false;
  bool ok = writeFn(f, write_ctx);
  f.flush();
  f.close();
  if (!ok) { fs->remove(tmp); return false; }

  // 2. read the scratch file back through the same validator. This is the only
  //    check this layer can make on the bytes: flush()/close() return void, so a
  //    successful write API call is not evidence the file is usable.
  {
    File check = fsOpenRead(fs, tmp);
    if (!check) { fs->remove(tmp); return false; }
    ok = writebackFn(check, writeback_ctx);
    check.close();
  }
  if (!ok) { fs->remove(tmp); return false; }

  // 3. preserve the current good config as the backup. An INVALID canonical
  //    must never displace a valid backup, so it is only removed (never
  //    rotated in) — which also means promotion needs no rename-over-existing.
  if (fs->exists(name)) {
    bool canonical_good = false;
    {
      File check = fsOpenRead(fs, name);
      if (check) { canonical_good = usableFn(check, usable_ctx); check.close(); }
    }
    if (canonical_good) {
      if (fs->exists(bak) && !fs->remove(bak)) return false;
      if (!fs->rename(name, bak)) return false;
    } else if (!fs->remove(name)) {
      return false;   // cannot clear it, so promotion would need rename-over
    }
  }

  // 4. promote. The backup is still valid if this fails.
  if (!fs->rename(tmp, name)) return false;
  return true;
}

// Which file a load took its config from, so the caller can mark a repair save.
enum PersistLoadSource {
  PERSIST_LOAD_NONE = 0,      // nothing usable: the caller keeps its defaults
  PERSIST_LOAD_CANONICAL,     // the live config
  PERSIST_LOAD_RECOVERED,     // the last-good backup: schedule a repair save
};

// Decide WHICH file to load: the canonical one when it validates, otherwise the
// last-good backup. This returns the chosen PATH rather than an open File because
// the nRF52/STM32 file type has no default constructor, so a File cannot be
// handed back through an out-parameter on every platform. The caller opens it.
//
// Only one file is open at a time — the Arduino filesystem API documents
// single-open use, so each attempt is opened, validated and closed in turn.
inline PersistLoadSource chooseConfigToLoad(FILESYSTEM* fs, const char* name,
                                            PersistValidateFn usableFn, void* ctx,
                                            char* out_path, size_t out_sz) {
  if (fs->exists(name)) {
    File f = fsOpenRead(fs, name);
    if (f) {
      bool ok = usableFn(f, ctx);
      f.close();
      if (ok) {
        if (strlen(name) < out_sz) { strcpy(out_path, name); return PERSIST_LOAD_CANONICAL; }
        return PERSIST_LOAD_NONE;
      }
    }
  }
  char bak[64];
  persistPath(bak, sizeof(bak), name, PERSIST_BAK_SUFFIX);
  if (fs->exists(bak)) {
    File f = fsOpenRead(fs, bak);
    if (f) {
      bool ok = usableFn(f, ctx);
      f.close();
      if (ok) {
        if (strlen(bak) < out_sz) { strcpy(out_path, bak); return PERSIST_LOAD_RECOVERED; }
        return PERSIST_LOAD_NONE;
      }
    }
  }
  return PERSIST_LOAD_NONE;
}

#endif // _PERSIST_UTIL_H
