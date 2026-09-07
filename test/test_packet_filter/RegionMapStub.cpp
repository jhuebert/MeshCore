// RegionMapStub.cpp — native-test stand-in for src/helpers/RegionMap.cpp,
// which is not linked into the packet filter test env (it needs Stream::printf
// and transport-key machinery the tests don't exercise). Only
// RegionMap::findByNamePrefix() — used by the `region=` CLI predicate — is
// resolved here, from a fixed table so tests are deterministic.
//
// NOTE: the stub touches no RegionMap members, so tests may pass a null
// RegionMap* to filterCLI().

#include <helpers/RegionMap.h>

#include <cstring>

static RegionEntry g_test_regions[] = {
  { 1, 0, 0, "TestNorth" },
  { 2, 0, 0, "TestSouth" },
  { 3, 1, 0, "TestNorthEast" },
};

RegionEntry* RegionMap::findByNamePrefix(const char* prefix) {
  for (auto& r : g_test_regions) {
    if (strncmp(r.name, prefix, strlen(prefix)) == 0) return &r;
  }
  return nullptr;
}
