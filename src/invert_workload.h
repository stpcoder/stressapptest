// Copyright 2026
//
// Licensed under the Apache License, Version 2.0.

#ifndef STRESSAPPTEST_INVERT_WORKLOAD_H_
#define STRESSAPPTEST_INVERT_WORKLOAD_H_

#include <stdint.h>

typedef void (*InvertFlushHint)(void *address);

// 낮은 주소에서 높은 주소로 32-bit word를 읽고 반전하여 같은 주소에
// 기록합니다. flush_interval_words마다 전달된 cache hint를 호출합니다.
// 호출자는 전후 ordering 동작을 별도로 수행합니다.
static inline bool InvertWordsUp(unsigned int *base,
                                 int64_t words,
                                 int flush_interval_words,
                                 InvertFlushHint flush_hint) {
  if (!base || words < 0 || flush_interval_words <= 0 ||
      (words % flush_interval_words) != 0 || !flush_hint) {
    return false;
  }

  unsigned int *iter = base;
  unsigned int *end = base + words;
  while (iter != end) {
    for (int i = 0; i < flush_interval_words; ++i) {
      *iter = ~(*iter);
      ++iter;
    }
    flush_hint(iter - flush_interval_words);
  }
  return true;
}

// 높은 주소에서 낮은 주소로 32-bit word를 읽고 반전하여 같은 주소에
// 기록합니다. 처리 범위와 cache hint 간격은 InvertWordsUp()과 같습니다.
static inline bool InvertWordsDown(unsigned int *base,
                                   int64_t words,
                                   int flush_interval_words,
                                   InvertFlushHint flush_hint) {
  if (!base || words < 0 || flush_interval_words <= 0 ||
      (words % flush_interval_words) != 0 || !flush_hint) {
    return false;
  }

  unsigned int *iter = base + words;
  while (iter != base) {
    for (int i = 0; i < flush_interval_words; ++i) {
      --iter;
      *iter = ~(*iter);
    }
    flush_hint(iter);
  }
  return true;
}

#endif  // STRESSAPPTEST_INVERT_WORKLOAD_H_
