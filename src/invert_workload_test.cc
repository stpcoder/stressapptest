#include <stdint.h>
#include <stdio.h>

#include <vector>

#include "invert_workload.h"

namespace {

int flush_hints = 0;
std::vector<uintptr_t> flush_addresses;

void CountFlushHint(void *address) {
  if (address) {
    ++flush_hints;
    flush_addresses.push_back(reinterpret_cast<uintptr_t>(address));
  }
}

int64_t LegacyRangeBytes(int page_length) {
  const int block_size = 4096;
  return static_cast<int64_t>(page_length / block_size) *
         (block_size / sizeof(uint64_t)) * sizeof(unsigned int);
}

bool CheckOneDirection(int page_length, bool full, bool down) {
  const int guard_words = 16;
  const int64_t range_bytes =
      full ? page_length : LegacyRangeBytes(page_length);
  const int64_t range_words = range_bytes / sizeof(unsigned int);
  const int page_words = page_length / sizeof(unsigned int);
  const int flush_words = 64 / sizeof(unsigned int);

  std::vector<unsigned int> buffer(
      guard_words + page_words + guard_words);
  for (size_t i = 0; i < buffer.size(); ++i)
    buffer[i] = 0x13570000U ^ static_cast<unsigned int>(i);
  const std::vector<unsigned int> before = buffer;

  flush_hints = 0;
  flush_addresses.clear();
  bool result = down
      ? InvertWordsDown(&buffer[guard_words], range_words,
                        flush_words, CountFlushHint)
      : InvertWordsUp(&buffer[guard_words], range_words,
                      flush_words, CountFlushHint);
  if (!result)
    return false;

  for (int i = 0; i < guard_words; ++i) {
    if (buffer[i] != before[i])
      return false;
  }
  for (int i = 0; i < page_words; ++i) {
    unsigned int expected = i < range_words
        ? ~before[guard_words + i]
        : before[guard_words + i];
    if (buffer[guard_words + i] != expected)
      return false;
  }
  for (int i = guard_words + page_words;
       i < static_cast<int>(buffer.size()); ++i) {
    if (buffer[i] != before[i])
      return false;
  }
  if (flush_hints != range_words / flush_words ||
      flush_addresses.size() != static_cast<size_t>(flush_hints)) {
    return false;
  }

  const uintptr_t base_address =
      reinterpret_cast<uintptr_t>(&buffer[guard_words]);
  for (int hint = 0; hint < flush_hints; ++hint) {
    const int64_t word_offset = down
        ? range_words - (hint + 1) * flush_words
        : hint * flush_words;
    const uintptr_t expected_address =
        base_address + word_offset * sizeof(unsigned int);
    if (flush_addresses[hint] != expected_address)
      return false;
  }
  return true;
}

bool CheckFourPassRestore(int page_length, bool full) {
  const int guard_words = 16;
  const int64_t range_bytes =
      full ? page_length : LegacyRangeBytes(page_length);
  const int64_t range_words = range_bytes / sizeof(unsigned int);
  const int page_words = page_length / sizeof(unsigned int);
  const int flush_words = 64 / sizeof(unsigned int);
  std::vector<unsigned int> buffer(
      guard_words + page_words + guard_words);
  for (size_t i = 0; i < buffer.size(); ++i)
    buffer[i] = 0x24680000U ^ static_cast<unsigned int>(i);
  const std::vector<unsigned int> before = buffer;

  if (!InvertWordsUp(&buffer[guard_words], range_words,
                     flush_words, CountFlushHint) ||
      !InvertWordsDown(&buffer[guard_words], range_words,
                       flush_words, CountFlushHint) ||
      !InvertWordsDown(&buffer[guard_words], range_words,
                       flush_words, CountFlushHint) ||
      !InvertWordsUp(&buffer[guard_words], range_words,
                     flush_words, CountFlushHint)) {
    return false;
  }
  return buffer == before;
}

}  // namespace

int main() {
  const int page_lengths[] = {1024, 2048, 4096, 1024 * 1024};
  for (size_t i = 0; i < sizeof(page_lengths) / sizeof(page_lengths[0]); ++i) {
    for (int full = 0; full <= 1; ++full) {
      if (!CheckOneDirection(page_lengths[i], full != 0, false) ||
          !CheckOneDirection(page_lengths[i], full != 0, true) ||
          !CheckFourPassRestore(page_lengths[i], full != 0)) {
        fprintf(stderr,
                "invert workload test failed: page=%d mode=%s\n",
                page_lengths[i], full ? "full" : "legacy");
        return 1;
      }
    }
  }
  printf("invert workload tests passed\n");
  return 0;
}
