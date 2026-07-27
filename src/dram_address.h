// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#ifndef STRESSAPPTEST_DRAM_ADDRESS_H_
#define STRESSAPPTEST_DRAM_ADDRESS_H_

#include <stdint.h>

// Physical-to-DRAM address maps are platform profiles. They must be selected
// explicitly because the bit layout and XOR hashing are memory-controller
// configuration data rather than an architectural AArch64 property.
enum DramAddressMapProfile {
  DRAM_ADDRESS_MAP_NONE = 0,
  DRAM_ADDRESS_MAP_LPDDR_V1
};

struct DramAddress {
  uint32_t channel;
  uint32_t rank;
  uint32_t subchannel;
  uint32_t bank_group;
  uint32_t bank;
  uint32_t row;
  uint32_t column;
  uint32_t byte_offset;
};

inline uint32_t DramAddressBit(uint64_t physical_address, unsigned int bit) {
  return static_cast<uint32_t>((physical_address >> bit) & 1ULL);
}

// Decode the opt-in lpddr-v1 profile. The profile is rank-0-only because the
// validation vectors available for this profile contain no rank-1 address.
inline bool DecodeDramAddress(DramAddressMapProfile profile,
                              uint64_t physical_address,
                              DramAddress *address) {
  if (profile != DRAM_ADDRESS_MAP_LPDDR_V1 || address == 0)
    return false;

  address->channel = static_cast<uint32_t>((physical_address >> 8) & 0x3);
  address->rank = 0;
  address->subchannel = DramAddressBit(physical_address, 10);

  const uint32_t bg0 = 1U ^ DramAddressBit(physical_address, 11) ^
      DramAddressBit(physical_address, 20) ^
      DramAddressBit(physical_address, 29) ^
      DramAddressBit(physical_address, 30);
  const uint32_t bg1 = DramAddressBit(physical_address, 15) ^
      DramAddressBit(physical_address, 19) ^
      DramAddressBit(physical_address, 21) ^
      DramAddressBit(physical_address, 24);
  address->bank_group = bg0 | (bg1 << 1);

  const uint32_t bank0 = DramAddressBit(physical_address, 16) ^
      DramAddressBit(physical_address, 21) ^
      DramAddressBit(physical_address, 23) ^
      DramAddressBit(physical_address, 30);
  const uint32_t bank1 = DramAddressBit(physical_address, 17) ^
      DramAddressBit(physical_address, 19) ^
      DramAddressBit(physical_address, 23) ^
      DramAddressBit(physical_address, 27) ^
      DramAddressBit(physical_address, 30);
  address->bank = bank0 | (bank1 << 1);

  address->row = static_cast<uint32_t>((physical_address >> 18) & 0xffff);
  address->column =
      (static_cast<uint32_t>((physical_address >> 12) & 0x7) << 3) |
      static_cast<uint32_t>((physical_address >> 5) & 0x7);
  address->byte_offset = static_cast<uint32_t>(physical_address & 0x1f);
  return true;
}

#endif  // STRESSAPPTEST_DRAM_ADDRESS_H_
