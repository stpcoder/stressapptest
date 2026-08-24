// Copyright 2009 Google Inc. All Rights Reserved.

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "logger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "sattypes.h"
#include "sm8975_mapping.h"

namespace {

bool ParseHexAfter(const string &line, const char *marker, uint64_t *value) {
  if (value == NULL)
    return false;
  const size_t marker_pos = line.find(marker);
  if (marker_pos == string::npos)
    return false;

  const size_t digits_pos = marker_pos + strlen(marker);
  if (digits_pos >= line.size())
    return false;

  char *end = NULL;
  const char *start = line.c_str() + digits_pos;
  const unsigned long long parsed = strtoull(start, &end, 16);
  if (end == start)
    return false;
  *value = static_cast<uint64_t>(parsed);
  return true;
}

bool ParseLoggedPhysicalAddress(const string &line, uint64_t *value) {
  if (value == NULL)
    return false;

  // Memory-error lines use: "... at <virtual>(0x<physical>:DIMM ...)".
  // rfind() also handles tag-mode lines that contain an earlier "Tag from".
  const size_t at_pos = line.rfind(" at ");
  if (at_pos == string::npos)
    return false;
  const size_t open_pos = line.find('(', at_pos + 4);
  if (open_pos == string::npos)
    return false;
  const size_t hex_pos = line.find("0x", open_pos + 1);
  if (hex_pos == string::npos)
    return false;

  char *end = NULL;
  const char *start = line.c_str() + hex_pos + 2;
  const unsigned long long parsed = strtoull(start, &end, 16);
  if (end == start)
    return false;
  *value = static_cast<uint64_t>(parsed);
  return true;
}

void ReplaceAll(string *text, const string &from, const string &to) {
  if (text == NULL || from.empty())
    return;
  size_t pos = 0;
  while ((pos = text->find(from, pos)) != string::npos) {
    text->replace(pos, from.size(), to);
    pos += to.size();
  }
}

void AppendUnsignedList(const uint32_t *values,
                        unsigned int count,
                        string *output) {
  output->append("[");
  for (unsigned int i = 0; i < count; ++i) {
    if (i != 0)
      output->append(",");
    char value[32];
    snprintf(value, sizeof(value), "%u", values[i]);
    output->append(value);
  }
  output->append("]");
}

void AppendSignedList(const int *values,
                      unsigned int count,
                      string *output) {
  output->append("[");
  for (unsigned int i = 0; i < count; ++i) {
    if (i != 0)
      output->append(",");
    char value[32];
    snprintf(value, sizeof(value), "%d", values[i]);
    output->append(value);
  }
  output->append("]");
}

string FormatSm8975Lp6Rows(uint64_t physical_address,
                           uint64_t expected,
                           uint64_t actual) {
  string output = "map:sm8975-lp6,lp6:[";
  if (physical_address == 0) {
    output.append("unavailable]");
    return output;
  }

  const uint64_t word_base = physical_address & ~0x7ULL;
  bool emitted = false;
  for (unsigned int half = 0; half < 2; ++half) {
    const unsigned int shift = half * 32;
    const uint32_t expected_word =
        static_cast<uint32_t>((expected >> shift) & 0xFFFFFFFFULL);
    const uint32_t actual_word =
        static_cast<uint32_t>((actual >> shift) & 0xFFFFFFFFULL);
    if (expected_word == actual_word)
      continue;

    const uint64_t address = word_base + half * 4ULL;
    Sm8975Topology topology = {};
    if (!DecodeSm8975Address(address, &topology))
      continue;

    Sm8975MismatchMapping mapping = {};
    MapSm8975MismatchWord32(address, expected_word, actual_word, &mapping);

    if (emitted)
      output.append(";");
    emitted = true;

    char fields[256];
    snprintf(fields, sizeof(fields),
             "{addr:0x%09llX,norm:0x%09llX,ch:0x%X,cs:0x%X,sc:0x%X,"
             "bk:0x%X,row:0x%04X,mat:%d,col:0x%02X,dq:",
             static_cast<unsigned long long>(address),
             static_cast<unsigned long long>(topology.normalized_address),
             topology.channel, topology.chip_select, topology.subchannel,
             topology.bank, topology.row, topology.mat, topology.column);
    output.append(fields);
    AppendUnsignedList(mapping.dq, mapping.count, &output);
    output.append(",bl:");
    AppendUnsignedList(mapping.bl, mapping.count, &output);
    output.append(",hex:");
    AppendSignedList(mapping.hex, mapping.count, &output);
    output.append("}");
  }

  if (!emitted)
    output.append("no-data-mismatch");
  output.append("]");
  return output;
}

void RewriteSm8975Lp6FailLine(string *line) {
  if (line == NULL)
    return;
  if (line->find("Hardware Error:") == string::npos ||
      line->find("miscompare") == string::npos)
    return;

  uint64_t physical_address = 0;
  uint64_t actual = 0;
  uint64_t expected = 0;
  if (!ParseLoggedPhysicalAddress(*line, &physical_address) ||
      !ParseHexAfter(*line, "read:0x", &actual) ||
      !ParseHexAfter(*line, "expected:0x", &expected)) {
    return;
  }

  // worker.cc already emits a topology field block between ch: and cur_mode:.
  // Replace that whole block so the public log uses the same vocabulary and
  // row split as lpddr6-packet-mapper: CH/CS/SC/BK/ROW/MAT/COL + DQ/BL/HEX.
  const size_t expected_pos = line->find("expected:0x");
  const size_t coord_start = line->find("ch:", expected_pos);
  if (coord_start == string::npos)
    return;
  const size_t coord_end = line->find(", cur_mode:", coord_start);
  if (coord_end == string::npos)
    return;

  line->replace(coord_start,
                coord_end - coord_start,
                FormatSm8975Lp6Rows(physical_address, expected, actual));
}

}  // namespace


Logger *Logger::GlobalLogger() {
  static Logger logger;
  return &logger;
}

void Logger::VLogF(int priority, const char *format, va_list args) {
  if (priority > verbosity_) {
    return;
  }
  char buffer[4096];
  size_t length = 0;
  if (log_timestamps_) {
    time_t raw_time;
    time(&raw_time);
    struct tm time_struct;
    localtime_r(&raw_time, &time_struct);
    length = strftime(buffer, sizeof(buffer), "%Y/%m/%d-%H:%M:%S(%Z) ",
                      &time_struct);
    LOGGER_ASSERT(length);  // Catch if the buffer is set too small.
  }
  length += vsnprintf(buffer + length, sizeof(buffer) - length, format, args);
  if (length >= sizeof(buffer)) {
    length = sizeof(buffer);
    buffer[sizeof(buffer) - 1] = '\n';
  }

  string line(buffer, length);

  // The legacy spelling survives only inside sat.cc's parser. Never expose it
  // in help/config output: the supported public profile is sm8975-lp6.
  ReplaceAll(&line, "lpddr-v1", "sm8975-lp6");

  if (sm8975_lp6_mapping_)
    RewriteSm8975Lp6FailLine(&line);

  QueueLogLine(new string(line));
}

void Logger::StartThread() {
  LOGGER_ASSERT(!thread_running_);
  thread_running_ = true;
  LOGGER_ASSERT(0 == pthread_create(&thread_, NULL, &StartRoutine, this));
}

void Logger::StopThread() {
  // Allow this to be called before the thread has started.
  if (!thread_running_) {
    return;
  }
  thread_running_ = false;
  int retval = pthread_mutex_lock(&queued_lines_mutex_);
  LOGGER_ASSERT(0 == retval);
  bool need_cond_signal = queued_lines_.empty();
  queued_lines_.push_back(NULL);
  retval = pthread_mutex_unlock(&queued_lines_mutex_);
  LOGGER_ASSERT(0 == retval);
  if (need_cond_signal) {
    retval = pthread_cond_signal(&queued_lines_cond_);
    LOGGER_ASSERT(0 == retval);
  }
  retval = pthread_join(thread_, NULL);
}

Logger::Logger()
    : verbosity_(20),
      log_fd_(-1),
      thread_running_(false),
      log_timestamps_(true),
      sm8975_lp6_mapping_(false) {
  LOGGER_ASSERT(0 == pthread_mutex_init(&queued_lines_mutex_, NULL));
  LOGGER_ASSERT(0 == pthread_cond_init(&queued_lines_cond_, NULL));
  LOGGER_ASSERT(0 == pthread_cond_init(&full_queue_cond_, NULL));
}

Logger::~Logger() {
  LOGGER_ASSERT(0 == pthread_mutex_destroy(&queued_lines_mutex_, NULL));
  LOGGER_ASSERT(0 == pthread_cond_destroy(&queued_lines_cond_));
  LOGGER_ASSERT(0 == pthread_cond_destroy(&full_queue_cond_));
}

void Logger::QueueLogLine(string *line) {
  LOGGER_ASSERT(line != NULL);
  LOGGER_ASSERT(0 == pthread_mutex_lock(&queued_lines_mutex_));
  if (thread_running_) {
    if (queued_lines_.size() >= kMaxQueueSize) {
      LOGGER_ASSERT(0 == pthread_cond_wait(&full_queue_cond_,
                                           &queued_lines_mutex_));
    }
    if (queued_lines_.empty()) {
      LOGGER_ASSERT(0 == pthread_cond_signal(&queued_lines_cond_));
    }
    queued_lines_.push_back(line);
  } else {
    WriteAndDeleteLogLine(line);
  }
  LOGGER_ASSERT(0 == pthread_mutex_unlock(&queued_lines_mutex_));
}

void Logger::WriteAndDeleteLogLine(string *line) {
  LOGGER_ASSERT(line != NULL);
  ssize_t bytes_written;
  if (log_fd_ >= 0) {
    bytes_written = write(log_fd_, line->data(), line->size());
    LOGGER_ASSERT(bytes_written == static_cast<ssize_t>(line->size()));
  }
  bytes_written = write(STDOUT_FILENO, line->data(), line->size());
  LOGGER_ASSERT(bytes_written == static_cast<ssize_t>(line->size()));
  delete line;
}

void *Logger::StartRoutine(void *ptr) {
  Logger *self = static_cast<Logger*>(ptr);
  self->ThreadMain();
  return NULL;
}

void Logger::ThreadMain() {
  vector<string*> local_queue;
  LOGGER_ASSERT(0 == pthread_mutex_lock(&queued_lines_mutex_));

  for (;;) {
    if (queued_lines_.empty()) {
      LOGGER_ASSERT(0 == pthread_cond_wait(&queued_lines_cond_,
                                           &queued_lines_mutex_));
      continue;
    }

    // We move the log lines into a local queue so we can release the lock
    // while writing them to disk, preventing other threads from blocking on
    // our writes.
    local_queue.swap(queued_lines_);
    if (local_queue.size() >= kMaxQueueSize) {
      LOGGER_ASSERT(0 == pthread_cond_broadcast(&full_queue_cond_));
    }

    // Unlock while we process our local queue.
    LOGGER_ASSERT(0 == pthread_mutex_unlock(&queued_lines_mutex_));
    for (vector<string*>::const_iterator it = local_queue.begin();
         it != local_queue.end(); ++it) {
      if (*it == NULL) {
        // NULL is guaranteed to be at the end.
        return;
      }
      WriteAndDeleteLogLine(*it);
    }
    local_queue.clear();
    // We must hold the lock at the start of each iteration of this for loop.
    LOGGER_ASSERT(0 == pthread_mutex_lock(&queued_lines_mutex_));
  }
}
