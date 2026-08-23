// Copyright 2006 Google Inc. All Rights Reserved.

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//      http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// pattern.h : global pattern references and initialization

// This file implements easy access to statically declared
// data patterns.

#ifndef STRESSAPPTEST_PATTERN_H_
#define STRESSAPPTEST_PATTERN_H_

#include <atomic>
#include <string>
#include <vector>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "adler32memcpy.h"
#include "sattypes.h"

// 2 = 128 bit bus, 1 = 64 bit bus, 0 = 32 bit bus
const int kBusShift = 2;

// Pattern and CRC data structure
struct PatternData {
  const char *name;          // Name of this pattern.
  unsigned int *pat;         // Data array.
  unsigned int mask;         // Size - 1. data[index & mask] is always valid.
  unsigned char weight[4];   // Weighted frequency of this pattern.
                             // Each pattern has 32,64,128,256 width versions.
                             // All weights are added up, a random number is
                             // chosen between 0-sum(weights), and the
                             // appropriate pattern is chosen. Thus a weight of
                             // 1 is rare, a weight of 10 is 2x as likely to be
                             // chosen as a weight of 5.
};

// Data structure to access data patterns.
class Pattern {
 public:
  Pattern();
  ~Pattern();
  // Fill pattern data and calculate CRC.
  int Initialize(const struct PatternData &pattern_init,
                 int buswidth,
                 bool invert,
                 int weight,
                 unsigned int byte_offset);

  // Pattern byte offset이 0인 Fill에서 주소의 Pattern 값을 계산합니다.
  // Word offset 덧셈을 생략하여 기존 Fill 계산 경로를 유지합니다.
  unsigned int pattern_unshifted(unsigned int offset) {
    unsigned int data =
        pattern_->pat[(offset >> busshift_) & pattern_->mask];
    if (inverse_)
      data = ~data;
    return data;
  }

  // offset은 SAT 작업 단위 시작점부터 계산한 32-bit word 번호입니다.
  // 256-bit Pattern의 busshift_ 값은 3이므로 Pattern 배열의 한 원소를
  // 8개 word, 즉 32 byte 동안 반복합니다. Fill은 두 word를 묶어 한 번의
  // 64-bit store를 수행합니다. 설정된 byte offset도 word 번호에 반영합니다.
  unsigned int pattern(unsigned int offset) {
    if (word_offset_ == 0)
      return pattern_unshifted(offset);
    unsigned int data = pattern_->pat[
        ((offset + word_offset_) >> busshift_) & pattern_->mask];
    if (inverse_)
      data = ~data;
    return data;
  }
  const AdlerChecksum *crc() {return crc_;}
  unsigned int mask() {return pattern_->mask;}
  unsigned int weight() {return weight_;}
  unsigned int byte_offset() {return word_offset_ * sizeof(unsigned int);}
  const char *name() {return name_.c_str();}

 private:
  int CalculateCrc();
  const struct PatternData *pattern_;
  int busshift_;        // Target data bus width.
  unsigned int word_offset_;  // Pattern 시작 위치를 나타내는 32-bit word 수.
  bool inverse_;        // Invert the data from the original pattern.
  AdlerChecksum *crc_;  // CRC of this pattern.
  string name_;         // The human readable pattern name.
  int weight_;          // This is the likelihood that this
                        // pattern will be chosen.
  // We want to copy this!
  // DISALLOW_COPY_AND_ASSIGN(Pattern);
};

// Object used to access global pattern list.
class PatternList {
 public:
  PatternList();
  ~PatternList();
  // Initialize pointers to global data patterns, and calculate CRC.
  int Initialize();
  int Destroy();

  // 모든 Pattern의 시작 위치를 byte 단위로 이동합니다. Initialize() 전에
  // 호출해야 하며 입력값은 32-bit Pattern word 경계에 정렬되어야 합니다.
  void SetByteOffset(unsigned int byte_offset) {
    pattern_byte_offset_ = byte_offset;
  }

  // Return the pattern designated by index i.
  Pattern *GetPattern(int i);
  // 쉼표로 구분한 0 기반 ID 또는 전체 이름을 입력 순서대로 등록합니다.
  bool SetPatternSequence(const string &selectors);
  // 선택 목록이 있으면 순서대로 순환하고, 목록이 없으면 가중치로 선택합니다.
  Pattern *GetRandomPattern();
  // Return the number of patterns available.
  int Size() {return size_;}

 private:
  vector<class Pattern> patterns_;
  int weightcount_;  // Total count of pattern weights.
  unsigned int size_;
  int initialized_;
  vector<int> selected_pattern_ids_;
  std::atomic<unsigned int> selected_pattern_cursor_;
  unsigned int pattern_byte_offset_;
  DISALLOW_COPY_AND_ASSIGN(PatternList);
};

// CrcIncrement allows an abstracted way to add a 32bit
// value into a running CRC. This function should be fast, and
// generate meaningful CRCs for the types of data patterns that
// we are using here.
// This CRC formula may not be optimal, but it does work.
// It may be improved in the future.
static inline uint32 CrcIncrement(uint32 crc, uint32 expected, int index) {
  uint32 addition = (expected ^ index);
  uint32 carry = (addition & crc) >> 31;

  return crc + addition + carry;
}


#endif  // STRESSAPPTEST_PATTERN_H_
