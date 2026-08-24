// Copyright 2026 stressapptest contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
//
// Qualcomm SM8975 LPDDR6 address/packet mapping shared by the fail logger.
// The equations and packet ordering intentionally mirror
// stpcoder/lpddr6-packet-mapper (sm8975_decode.py + engine.py).

#ifndef STRESSAPPTEST_SM8975_MAPPING_H_
#define STRESSAPPTEST_SM8975_MAPPING_H_

#include <stdint.h>

struct Sm8975Topology {
  uint32_t channel;
  uint32_t chip_select;
  uint32_t subchannel;
  uint32_t bank;
  uint32_t row;
  int mat;
  uint32_t column;
  uint64_t normalized_address;
};

struct Sm8975MismatchMapping {
  unsigned int count;
  uint32_t dq[64];
  uint32_t bl[64];
  int hex[64];
};

inline uint32_t Sm8975AddressBit(uint64_t value, unsigned int bit) {
  return static_cast<uint32_t>((value >> bit) & 1ULL);
}

// Keep the exact base-removal contract used by lpddr6-packet-mapper.
// The normalized controller address is always limited to 36 bits.
inline uint64_t NormalizeSm8975Address(uint64_t raw_address) {
  uint64_t decoded;
  if (raw_address < 0x100000000ULL)
    decoded = raw_address - 0x80000000ULL;
  else
    decoded = raw_address - 0x800000000ULL;
  return decoded & 0xFFFFFFFFFULL;
}

inline int Sm8975MatForRow(uint32_t row) {
  static const uint32_t kRanges[][2] = {
    {0x000, 0x55F}, {0x560, 0xABF}, {0xAC0, 0x101F},
    {0x1020, 0x153F}, {0x1540, 0x1A9F}, {0x1AA0, 0x1FFF},
    {0x2000, 0x255F}, {0x2560, 0x2ABF}, {0x2AC0, 0x301F},
    {0x3020, 0x353F}, {0x3540, 0x3A9F}, {0x3AA0, 0x3FFF},
    {0x4000, 0x455F}, {0x4560, 0x4ABF}, {0x4AC0, 0x501F},
    {0x5020, 0x553F}, {0x5540, 0x5A9F}, {0x5AA0, 0x5FFF},
    {0x6000, 0x655F}, {0x6560, 0x6ABF}, {0x6AC0, 0x701F},
    {0x7020, 0x753F}, {0x7540, 0x7A9F}, {0x7AA0, 0x7FFF}
  };
  for (unsigned int i = 0; i < sizeof(kRanges) / sizeof(kRanges[0]); ++i) {
    if (row >= kRanges[i][0] && row <= kRanges[i][1])
      return static_cast<int>(i);
  }
  return -1;
}

inline bool DecodeSm8975Address(uint64_t raw_address,
                                Sm8975Topology *topology) {
  if (topology == 0)
    return false;

  const uint64_t decoded = NormalizeSm8975Address(raw_address);

  const uint32_t a7 = Sm8975AddressBit(decoded, 7);
  const uint32_t a8 = Sm8975AddressBit(decoded, 8);
  const uint32_t a9 = Sm8975AddressBit(decoded, 9);
  const uint32_t a10 = Sm8975AddressBit(decoded, 10);
  const uint32_t a11 = Sm8975AddressBit(decoded, 11);
  const uint32_t a12 = Sm8975AddressBit(decoded, 12);
  const uint32_t a13 = Sm8975AddressBit(decoded, 13);
  const uint32_t a14 = Sm8975AddressBit(decoded, 14);
  const uint32_t a15 = Sm8975AddressBit(decoded, 15);
  const uint32_t a16 = Sm8975AddressBit(decoded, 16);
  const uint32_t a17 = Sm8975AddressBit(decoded, 17);
  const uint32_t a18 = Sm8975AddressBit(decoded, 18);
  const uint32_t a19 = Sm8975AddressBit(decoded, 19);
  const uint32_t a20 = Sm8975AddressBit(decoded, 20);
  const uint32_t a21 = Sm8975AddressBit(decoded, 21);
  const uint32_t a22 = Sm8975AddressBit(decoded, 22);
  const uint32_t a23 = Sm8975AddressBit(decoded, 23);
  const uint32_t a24 = Sm8975AddressBit(decoded, 24);
  const uint32_t a25 = Sm8975AddressBit(decoded, 25);
  const uint32_t a26 = Sm8975AddressBit(decoded, 26);
  const uint32_t a27 = Sm8975AddressBit(decoded, 27);
  const uint32_t a28 = Sm8975AddressBit(decoded, 28);
  const uint32_t a29 = Sm8975AddressBit(decoded, 29);
  const uint32_t a30 = Sm8975AddressBit(decoded, 30);
  const uint32_t a31 = Sm8975AddressBit(decoded, 31);
  const uint32_t a32 = Sm8975AddressBit(decoded, 32);
  const uint32_t a33 = Sm8975AddressBit(decoded, 33);

  const uint32_t ch0 = a8;
  const uint32_t ch1 = a9 ^ a11 ^ a13 ^ a15 ^ a17 ^ a19;
  topology->channel = (ch1 << 1) | ch0;
  topology->chip_select = a32 ^ a33;
  topology->subchannel = a10 ^ a11 ^ a12 ^ a13;

  const uint32_t bk0 =
      a16 ^ a19 ^ a20 ^ a22 ^ a24 ^ a25 ^ a26 ^ a27 ^ a31;
  const uint32_t bk1 =
      a17 ^ a20 ^ a21 ^ a23 ^ a25 ^ a26 ^ a27 ^ a28 ^ a32;
  const uint32_t bk2 =
      a7 ^ a18 ^ a21 ^ a22 ^ a24 ^ a26 ^ a27 ^ a28 ^ a29;
  const uint32_t bk3 =
      a13 ^ a18 ^ a19 ^ a21 ^ a23 ^ a24 ^ a25 ^ a26 ^ a30;
  topology->bank = (bk3 << 3) | (bk2 << 2) | (bk1 << 1) | bk0;

  topology->column =
      (a15 << 5) | (a14 << 4) | (a12 << 3) | (a11 << 2) |
      (Sm8975AddressBit(decoded, 6) << 1) |
      Sm8975AddressBit(decoded, 5);

  topology->row = 0;
  for (unsigned int index = 0; index < 15; ++index)
    topology->row |= Sm8975AddressBit(decoded, 18 + index) << index;
  topology->mat = Sm8975MatForRow(topology->row);
  topology->normalized_address = decoded;
  return true;
}

// Visible HEX region table from the QC bench mapper.  Missing positions are
// deliberately -1 because those DQ/BL cells are non-data holes in normal mode.
inline int Sm8975HexRegion(uint32_t dq, uint32_t bl) {
  static const int kHexTable[6][6] = {
    {0, 1, 2, 3, 4, 5},
    {6, 7, 8, 9, 10, 11},
    {12, 13, -1, 14, 15, -1},
    {16, 17, 18, 19, 20, 21},
    {22, 23, 24, 25, 26, 27},
    {28, 29, -1, 30, 31, -1}
  };
  if (dq >= 12 || bl >= 24)
    return -1;
  return kHexTable[bl / 4][dq / 2];
}

// Map one LPDDR6 normal-mode user-data bit to its exact DQ/BL pair.  The loop
// mirrors engine.py's assignment order instead of using a separately-derived
// shortcut, so the fixed-low holes stay identical to the report generator.
inline bool Sm8975DataBitToCoordinate(unsigned int data_bit,
                                      uint32_t *dq_out,
                                      uint32_t *bl_out,
                                      int *hex_out) {
  if (data_bit >= 256 || dq_out == 0 || bl_out == 0 || hex_out == 0)
    return false;

  unsigned int current = 0;
  for (uint32_t bl_group = 0; bl_group < 6; ++bl_group) {
    for (uint32_t dq = 0; dq < 12; ++dq) {
      const bool hole =
          (bl_group == 2 || bl_group == 5) &&
          (dq == 4 || dq == 5 || dq == 10 || dq == 11);
      if (hole)
        continue;
      for (uint32_t bit_in_cell = 0; bit_in_cell < 4; ++bit_in_cell) {
        const uint32_t bl = bl_group * 4 + bit_in_cell;
        if (current == data_bit) {
          *dq_out = dq;
          *bl_out = bl;
          *hex_out = Sm8975HexRegion(dq, bl);
          return true;
        }
        ++current;
      }
    }
  }
  return false;
}

// stressapptest reports a 64-bit failed word.  lpddr6-packet-mapper first
// aligns that physical address to the containing 64-bit word, splits it into
// two 32-bit words, then maps each WR/RD mismatch bit according to its position
// in the containing 32-byte LPDDR6 packet.  Preserve that order exactly.
inline void MapSm8975MismatchBits(uint64_t physical_address,
                                  uint64_t expected,
                                  uint64_t actual,
                                  Sm8975MismatchMapping *mapping) {
  if (mapping == 0)
    return;
  mapping->count = 0;

  const uint64_t word_base = physical_address & ~0x7ULL;
  for (unsigned int half = 0; half < 2; ++half) {
    const unsigned int shift = half * 32;
    const uint32_t expected_word =
        static_cast<uint32_t>((expected >> shift) & 0xFFFFFFFFULL);
    const uint32_t actual_word =
        static_cast<uint32_t>((actual >> shift) & 0xFFFFFFFFULL);
    if (expected_word == actual_word)
      continue;

    const uint64_t address = word_base + half * 4;
    const uint64_t packet_base = address - (address % 32ULL);
    const unsigned int word_index =
        static_cast<unsigned int>((address - packet_base) / 4ULL);

    for (unsigned int bit = 0; bit < 32; ++bit) {
      if (((expected_word >> bit) & 1U) == ((actual_word >> bit) & 1U))
        continue;
      const unsigned int data_bit = word_index * 32 + bit;
      uint32_t dq = 0;
      uint32_t bl = 0;
      int hex = -1;
      if (!Sm8975DataBitToCoordinate(data_bit, &dq, &bl, &hex))
        continue;
      const unsigned int index = mapping->count;
      if (index >= 64)
        return;
      mapping->dq[index] = dq;
      mapping->bl[index] = bl;
      mapping->hex[index] = hex;
      mapping->count = index + 1;
    }
  }
}

#endif  // STRESSAPPTEST_SM8975_MAPPING_H_
