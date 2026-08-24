// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include <stdio.h>

#include "dram_address.h"
#include "sm8975_mapping.h"

static bool CheckTopology(uint64_t raw,
                          uint64_t normalized,
                          uint32_t channel,
                          uint32_t cs,
                          uint32_t sc,
                          uint32_t bk,
                          uint32_t row,
                          int mat,
                          uint32_t col) {
  Sm8975Topology actual = {};
  if (!DecodeSm8975Address(raw, &actual))
    return false;
  return actual.normalized_address == normalized &&
      actual.channel == channel &&
      actual.chip_select == cs &&
      actual.subchannel == sc &&
      actual.bank == bk &&
      actual.row == row &&
      actual.mat == mat &&
      actual.column == col;
}

int main() {
  // Base removal must treat the 8-digit and 9+-digit QC windows identically.
  if (!CheckTopology(0x80000000ULL, 0x000000000ULL,
                     0, 0, 0, 0, 0x0000, 0, 0x00) ||
      !CheckTopology(0x80000020ULL, 0x000000020ULL,
                     0, 0, 0, 0, 0x0000, 0, 0x01) ||
      !CheckTopology(0x800000000ULL, 0x000000000ULL,
                     0, 0, 0, 0, 0x0000, 0, 0x00) ||
      !CheckTopology(0x800000020ULL, 0x000000020ULL,
                     0, 0, 0, 0, 0x0000, 0, 0x01)) {
    fprintf(stderr, "SM8975 base-removal/topology vector failed\n");
    return 1;
  }

  // Cross-check the representative address previously used in this fork.
  // Under the QC SM8975 LP6 equations it is CS1/BK15/COL0x3D; the removed
  // lpddr-v1 profile produced a different topology and must not be restored.
  if (!CheckTopology(0x90A5EDC3CULL, 0x10A5EDC3CULL,
                     0, 1, 1, 15, 0x4297, 12, 0x3D)) {
    fprintf(stderr, "SM8975 representative topology vector failed\n");
    return 1;
  }

  DramAddress address = {};
  if (!DecodeDramAddress(DRAM_ADDRESS_MAP_SM8975_LP6,
                         0x90A5EDC3CULL, &address) ||
      address.channel != 0 ||
      address.rank != 1 ||
      address.subchannel != 1 ||
      address.bank != 15 ||
      address.row != 0x4297 ||
      address.column != 0x3D) {
    fprintf(stderr, "DramAddress SM8975 adapter failed\n");
    return 1;
  }

  // Match lpddr6-packet-mapper's exact ordered mismatch pairs for data bits
  // 5, 6 and 11: (DQ1,BL1), (DQ1,BL2), (DQ2,BL3), HEX 0,0,1.
  Sm8975MismatchMapping mismatch = {};
  const uint64_t expected = (1ULL << 5) | (1ULL << 6) | (1ULL << 11);
  MapSm8975MismatchBits(0x80000000ULL, expected, 0, &mismatch);
  if (mismatch.count != 3 ||
      mismatch.dq[0] != 1 || mismatch.bl[0] != 1 || mismatch.hex[0] != 0 ||
      mismatch.dq[1] != 1 || mismatch.bl[1] != 2 || mismatch.hex[1] != 0 ||
      mismatch.dq[2] != 2 || mismatch.bl[2] != 3 || mismatch.hex[2] != 1) {
    fprintf(stderr, "SM8975 DQ/BL/HEX packet vector failed\n");
    return 1;
  }

  if (Sm8975MatForRow(0x55F) != 0 ||
      Sm8975MatForRow(0x560) != 1 ||
      Sm8975MatForRow(0x7FFF) != 23 ||
      Sm8975MatForRow(0x8000) != -1) {
    fprintf(stderr, "SM8975 MAT boundary vector failed\n");
    return 1;
  }

  return 0;
}
