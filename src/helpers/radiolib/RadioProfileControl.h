#pragma once

#include <stdint.h>

// Lets a feature own the radio's profile scheduling while the dispatcher only
// asks it to select the profile the next transmission needs. Fork-owned.
class RadioProfileControl {
public:
  virtual ~RadioProfileControl() = default;
  virtual bool selectTxProfile(uint8_t profile) = 0;
};
