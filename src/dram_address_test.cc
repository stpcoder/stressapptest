// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#include <stdio.h>

#include "dram_address.h"

struct TestVector {
  uint64_t physical_address;
  DramAddress expected;
};

int main() {
  const TestVector vectors[] = {
    {0x91d48843cULL, {0, 0, 1, 3, 0, 0x4752, 0x01, 0x1c}},
    {0x91d7f9f18ULL, {3, 0, 1, 1, 2, 0x475f, 0x08, 0x18}},
    {0x0ef3b3b7cULL, {3, 0, 0, 3, 1, 0x3bce, 0x1b, 0x1c}},
    {0x9044306bcULL, {2, 0, 1, 1, 3, 0x4110, 0x05, 0x1c}},
    {0x917f1927cULL, {2, 0, 0, 2, 3, 0x45fc, 0x0b, 0x1c}},
    {0x9194c487cULL, {0, 0, 0, 0, 0, 0x4653, 0x23, 0x1c}},
    {0x90a5edc3cULL, {0, 0, 1, 1, 2, 0x4297, 0x29, 0x1c}},
    {0x915bce23cULL, {2, 0, 0, 0, 0, 0x456f, 0x31, 0x1c}},
    {0x906892e3cULL, {2, 0, 1, 2, 0, 0x41a2, 0x11, 0x1c}},
    {0x92f3bc87cULL, {0, 0, 0, 0, 2, 0x4bce, 0x23, 0x1c}},
    {0x91ba234dcULL, {0, 0, 1, 1, 2, 0x46e8, 0x1e, 0x1c}},
    {0x9117a88bcULL, {0, 0, 0, 1, 1, 0x445e, 0x05, 0x1c}},
  };

  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    DramAddress actual = {};
    if (!DecodeDramAddress(DRAM_ADDRESS_MAP_LPDDR_V1,
                           vectors[i].physical_address, &actual) ||
        actual.channel != vectors[i].expected.channel ||
        actual.rank != vectors[i].expected.rank ||
        actual.subchannel != vectors[i].expected.subchannel ||
        actual.bank_group != vectors[i].expected.bank_group ||
        actual.bank != vectors[i].expected.bank ||
        actual.row != vectors[i].expected.row ||
        actual.column != vectors[i].expected.column ||
        actual.byte_offset != vectors[i].expected.byte_offset) {
      fprintf(stderr, "DRAM address vector %zu failed\n", i);
      return 1;
    }
  }

  return 0;
}
