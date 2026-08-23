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
  // 실제 장치 로그를 사용하지 않고 0 주소와 단일 bit 입력으로 각 필드의
  // 기본 추출 경로를 검사합니다. 모든 값은 단위 시험용 합성 입력입니다.
  const TestVector vectors[] = {
    {0x00000000ULL, {0, 0, 0, 1, 0, 0, 0, 0}},
    {0x00000100ULL, {1, 0, 0, 1, 0, 0, 0, 0}},
    {0x00000400ULL, {0, 0, 1, 1, 0, 0, 0, 0}},
    {0x00000800ULL, {0, 0, 0, 0, 0, 0, 0, 0}},
    {0x00008000ULL, {0, 0, 0, 3, 0, 0, 0, 0}},
    {0x00010000ULL, {0, 0, 0, 1, 1, 0, 0, 0}},
    {0x00020000ULL, {0, 0, 0, 1, 2, 0, 0, 0}},
    {0x00040000ULL, {0, 0, 0, 1, 0, 1, 0, 0}},
    {0x00001000ULL, {0, 0, 0, 1, 0, 0, 8, 0}},
    {0x00000020ULL, {0, 0, 0, 1, 0, 0, 1, 0}},
    {0x0000001fULL, {0, 0, 0, 1, 0, 0, 0, 0x1f}},
    {0xffffffffULL, {3, 0, 1, 1, 2, 0x3fff, 0x3f, 0x1f}},
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
