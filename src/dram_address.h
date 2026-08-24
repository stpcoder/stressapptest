// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#ifndef STRESSAPPTEST_DRAM_ADDRESS_H_
#define STRESSAPPTEST_DRAM_ADDRESS_H_

#include <stdint.h>

#include "sm8975_mapping.h"

// Physical-address decoding is target-specific.  The old empirical lpddr-v1
// equations were removed because they do not describe the QC SM8975 LPDDR6
// routing used by this repository's target.  sm8975-lp6 is the only supported
// non-none user-facing mapping profile.
enum DramAddressMapProfile {
  DRAM_ADDRESS_MAP_NONE = 0,
  DRAM_ADDRESS_MAP_SM8975_LP6 = 1,

  // sat.cc still uses this internal identifier while parsing its historical
  // spelling. main.cc never exposes that spelling: it translates the public
  // sm8975-lp6 name before ParseArgs() and rejects lpddr-v1 explicitly.
  // This alias contains no legacy mapping logic.
  DRAM_ADDRESS_MAP_LPDDR_V1 = DRAM_ADDRESS_MAP_SM8975_LP6
};

struct DramAddress {
  uint32_t channel;
  uint32_t rank;        // SM8975 chip-select (CS).
  uint32_t subchannel;
  uint32_t bank_group;  // Not independently decoded for SM8975 LP6.
  uint32_t bank;        // SM8975 4-bit BK value.
  uint32_t row;
  uint32_t column;
  uint32_t byte_offset;
};

// Decode one system physical address with the exact equations used by
// stpcoder/lpddr6-packet-mapper.  MAT and DQ/BL/HEX are reported separately by
// the fail-log formatter because MAT is derived from ROW and DQ/BL/HEX also
// require expected/read mismatch information.
inline bool DecodeDramAddress(DramAddressMapProfile profile,
                              uint64_t physical_address,
                              DramAddress *address) {
  if (profile != DRAM_ADDRESS_MAP_SM8975_LP6 || address == 0)
    return false;

  Sm8975Topology topology = {};
  if (!DecodeSm8975Address(physical_address, &topology))
    return false;

  address->channel = topology.channel;
  address->rank = topology.chip_select;
  address->subchannel = topology.subchannel;
  address->bank_group = 0;
  address->bank = topology.bank;
  address->row = topology.row;
  address->column = topology.column;
  address->byte_offset =
      static_cast<uint32_t>(topology.normalized_address & 0x1FULL);
  return true;
}

#endif  // STRESSAPPTEST_DRAM_ADDRESS_H_
