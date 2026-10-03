// RegionMapStub.cpp — native-test stand-in for src/helpers/RegionMap.cpp,
// which is not linked into the packet filter test env (it needs Stream::printf
// and transport-key machinery the tests don't exercise). Only
// RegionMap::findByNamePrefix() — used by the `region=` CLI predicate — is
// resolved here, from a fixed table so tests are deterministic.
//
// The real constructor is provided here too, so tests can hold an actual
// RegionMap instance: calling findByNamePrefix() through a null RegionMap* is
// undefined behaviour even when the callee never touches `this`.

#include <helpers/RegionMap.h>

#include <cstring>

static RegionEntry g_test_regions[] = {
  { 1, 0, 0, "TestNorth" },
  { 2, 0, 0, "TestSouth" },
  { 3, 1, 0, "TestNorthEast" },
};

RegionMap::RegionMap(TransportKeyStore& store) : _store(&store) {
  next_id = 1; num_regions = 0;
  default_id = home_id = 0;
  wildcard.id = wildcard.parent = 0;
  wildcard.flags = 0;
  strcpy(wildcard.name, "*");
}

RegionEntry* RegionMap::findByNamePrefix(const char* prefix) {
  for (auto& r : g_test_regions) {
    if (strncmp(r.name, prefix, strlen(prefix)) == 0) return &r;
  }
  return nullptr;
}
