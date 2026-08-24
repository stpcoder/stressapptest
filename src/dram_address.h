// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

#ifndef STRESSAPPTEST_DRAM_ADDRESS_H_
#define STRESSAPPTEST_DRAM_ADDRESS_H_

#include <stdint.h>

#include "sm8975_mapping.h"

// 물리 주소를 DRAM 좌표로 해석하는 규칙은 memory-controller 설정에
// 종속됩니다. 사용자가 프로필을 명시한 경우에만 주소 변환을 수행합니다.
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

// main.cc가 --dram-map qc-sm8975를 기존 ParseArgs가 이해하는 lpddr-v1로
// 호환 변환한 뒤 이 flag를 설정합니다. 실행 시작 전에 한 번 정해지고 Worker
// thread에서는 읽기만 하므로 별도 동기화가 필요하지 않습니다.
extern bool g_qc_sm8975_dram_map_requested;

// 물리 주소에서 지정한 한 bit를 추출합니다.
inline uint32_t DramAddressBit(uint64_t physical_address, unsigned int bit) {
  return static_cast<uint32_t>((physical_address >> bit) & 1ULL);
}

// 선택한 주소 프로필을 적용합니다. qc-sm8975는 기존 CLI 파서를 침범하지
// 않도록 main.cc의 compatibility shim을 통해 이 함수에 도달합니다.
inline bool DecodeDramAddress(DramAddressMapProfile profile,
                              uint64_t physical_address,
                              DramAddress *address) {
  if (profile != DRAM_ADDRESS_MAP_LPDDR_V1 || address == 0)
    return false;

  if (g_qc_sm8975_dram_map_requested) {
    Sm8975Topology topology = {};
    if (!DecodeSm8975Address(physical_address, &topology))
      return false;
    address->channel = topology.channel;
    address->rank = topology.chip_select;
    address->subchannel = topology.subchannel;
    // lpddr6-packet-mapper에는 검증된 독립 BG 식이 없습니다. 이 값은
    // worker.cc의 legacy formatter를 통과하기 위한 placeholder이며 실제
    // qc-sm8975 fail line에서는 logger.cc가 BG:unknown으로 바꿉니다.
    address->bank_group = 0;
    address->bank = topology.bank;
    address->row = topology.row;
    address->column = topology.column;
    address->byte_offset = static_cast<uint32_t>(physical_address & 0x1fULL);
    return true;
  }

  // 기존 lpddr-v1 프로필은 그대로 유지합니다.
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
