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

// sat.cc : a stress test for stressful testing

// stressapptest (or SAT, from Stressful Application Test) is a test
// designed to stress the system, as well as provide a comprehensive
// memory interface test.

// stressapptest can be run using memory only, or using many system components.

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>

// #define __USE_GNU
// #define __USE_LARGEFILE64
#include <fcntl.h>

#include <list>
#include <new>
#include <string>
#include <vector>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "disk_blocks.h"
#include "logger.h"
#include "os.h"
#include "sat.h"
#include "sattypes.h"
#include "worker.h"

// stressapptest versioning here.
#ifndef PACKAGE_VERSION
static const char* kVersion = "1.0.0";
#else
static const char* kVersion = PACKAGE_VERSION;
#endif

// Global stressapptest reference, for use by signal handler.
// This makes Sat objects not safe for multiple instances.
namespace {
  Sat *g_sat = NULL;

  static const char kDefaultDramFrequencyNode[] =
      "/sys/kernel/debug/aoss_send_message";
  // 비정상 입력에 따른 대규모 객체 생성과 pthread 자원 고갈을 방지합니다.
  // 일반 모바일 CPU 수보다 충분히 큰 진단 Worker 상한입니다.
  static const int kMaxDiagnosticWorkerCount = 256;
  static const int kAllDramFrequencies[] = {
    547, 768, 1017, 1353, 1555, 1708, 2092, 2736, 3196, 4266, 5333
  };

  // 옵션 문자열을 1 이상의 int로 변환합니다. 숫자 뒤의 추가 문자와
  // int 범위 초과 입력을 거부합니다.
  bool ParsePositiveInt(const string &value, int *result) {
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed <= 0 || parsed > INT_MAX) {
      return false;
    }
    *result = static_cast<int>(parsed);
    return true;
  }

  // 0 이상의 int로 변환합니다. 대기 시간과 Pattern offset에
  // 사용하는 공통 입력 검사입니다.
  bool ParseNonNegativeInt(const string &value, int *result) {
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed < 0 || parsed > INT_MAX) {
      return false;
    }
    *result = static_cast<int>(parsed);
    return true;
  }

  // --ddr-freq의 all 또는 쉼표 목록을 입력 순서대로 변환합니다.
  // 항목 하나라도 유효하지 않으면 결과 목록을 비우고 실패를 반환합니다.
  bool ParseDramFrequencyList(const string &value,
                              vector<int> *frequencies) {
    frequencies->clear();
    if (value == "all") {
      frequencies->assign(
          kAllDramFrequencies,
          kAllDramFrequencies +
              sizeof(kAllDramFrequencies) / sizeof(kAllDramFrequencies[0]));
      return true;
    }

    size_t start = 0;
    while (start <= value.size()) {
      size_t comma = value.find(',', start);
      string item = value.substr(start, comma == string::npos
                                    ? string::npos : comma - start);
      int frequency = 0;
      if (!ParsePositiveInt(item, &frequency)) {
        frequencies->clear();
        return false;
      }
      frequencies->push_back(frequency);
      if (comma == string::npos)
        break;
      start = comma + 1;
    }
    return !frequencies->empty();
  }

  // /proc 형식 파일에서 같은 key의 kB 값을 합산합니다. Android 정책이나
  // kernel 설정으로 파일을 읽을 수 없으면 -1을 반환합니다.
  int64 ReadProcKbTotal(const char *path, const char *key) {
    FILE *file = fopen(path, "r");
    if (!file)
      return -1;

    const size_t key_length = strlen(key);
    char line[512];
    int64 total = 0;
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
      if (strncmp(line, key, key_length) != 0)
        continue;
      unsigned long long value = 0;
      if (sscanf(line + key_length, " %llu kB", &value) == 1) {
        total += static_cast<int64>(value);
        found = true;
      }
    }
    fclose(file);
    return found ? total : -1;
  }

  // Signal handler for catching break or kill.
  //
  // This must be installed after g_sat is assigned and while there is a single
  // thread.
  //
  // This must be uninstalled while there is only a single thread, and of course
  // before g_sat is cleared or deleted.
  void SatHandleBreak(int signal) {
    g_sat->Break();
  }
}

// Opens the logfile for writing if necessary
bool Sat::InitializeLogfile() {
  // Open logfile.
  if (use_logfile_) {
    logfile_ = open(logfilename_,
#if defined(O_DSYNC)
                    O_DSYNC |
#elif defined(O_SYNC)
                    O_SYNC |
#elif defined(O_FSYNC)
                    O_FSYNC |
#endif
                    O_WRONLY | O_CREAT,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (logfile_ < 0) {
      printf("Fatal Error: cannot open file %s for logging\n",
             logfilename_);
      bad_status();
      return false;
    }
    // We seek to the end once instead of opening in append mode because no
    // other processes should be writing to it while this one exists.
    if (lseek(logfile_, 0, SEEK_END) == -1) {
      printf("Fatal Error: cannot seek to end of logfile (%s)\n",
             logfilename_);
      bad_status();
      return false;
    }
    Logger::GlobalLogger()->SetLogFd(logfile_);
  }
  return true;
}

// Check that the environment is known and safe to run on.
// Return 1 if good, 0 if unsuppported.
bool Sat::CheckEnvironment() {
  // Check that this is not a debug build. Debug builds lack
  // enough performance to stress the system.
#if !defined NDEBUG
  if (run_on_anything_) {
    logprintf(1, "Log: Running DEBUG version of SAT, "
                 "with significantly reduced coverage.\n");
  } else {
    logprintf(0, "Process Error: Running DEBUG version of SAT, "
                 "with significantly reduced coverage.\n");
    logprintf(0, "Log: Command line option '-A' bypasses this error.\n");
    bad_status();
    return false;
  }
#elif !defined CHECKOPTS
  #error Build system regression - COPTS disregarded.
#endif

  // Check if the cpu frequency test is enabled and able to run.
  if (cpu_freq_test_) {
    if (!CpuFreqThread::CanRun()) {
      logprintf(0, "Process Error: This platform does not support this "
                "test.\n");
      bad_status();
      return false;
    } else if (cpu_freq_threshold_ <= 0) {
      logprintf(0, "Process Error: The cpu frequency test requires "
                "--cpu_freq_threshold set to a value > 0\n");
      bad_status();
      return false;
    } else if (cpu_freq_round_ < 0) {
      logprintf(0, "Process Error: The --cpu_freq_round option must be greater"
                " than or equal to zero. A value of zero means no rounding.\n");
      bad_status();
      return false;
    }
  }

  // Use all CPUs if nothing is specified.
  if (memory_threads_ == -1) {
    memory_threads_ = os_->num_cpus();
    logprintf(7, "Log: Defaulting to %d copy threads\n", memory_threads_);
  }

  // Use all memory if no size is specified.
  if (size_mb_ == 0)
    size_mb_ = os_->FindFreeMemSize() / kMegabyte;
  size_ = static_cast<int64>(size_mb_) * kMegabyte;

  // Autodetect file locations.
  if (findfiles_ && (file_threads_ == 0)) {
    // Get a space separated sting of disk locations.
    list<string> locations = os_->FindFileDevices();

    // Extract each one.
    while (!locations.empty()) {
      // Copy and remove the disk name.
      string disk = locations.back();
      locations.pop_back();

      logprintf(12, "Log: disk at %s\n", disk.c_str());
      file_threads_++;
      filename_.push_back(disk + "/sat_disk.a");
      file_threads_++;
      filename_.push_back(disk + "/sat_disk.b");
    }
  }

  // We'd better have some memory by this point.
  if (size_ < 1) {
    logprintf(0, "Process Error: No memory found to test.\n");
    bad_status();
    return false;
  }

  if (tag_mode_ && ((file_threads_ > 0) ||
                    (disk_threads_ > 0) ||
                    (net_threads_ > 0))) {
    logprintf(0, "Process Error: Memory tag mode incompatible "
                 "with disk/network DMA.\n");
    bad_status();
    return false;
  }

  // If platform is 32 bit Xeon, floor memory size to multiple of 4.
  if (address_mode_ == 32) {
    size_mb_ = (size_mb_ / 4) * 4;
    size_ = size_mb_ * kMegabyte;
    logprintf(1, "Log: Flooring memory allocation to multiple of 4: %lldMB\n",
              size_mb_);
  }

  // Check if this system is on the whitelist for supported systems.
  if (!os_->IsSupported()) {
    if (run_on_anything_) {
      logprintf(1, "Log: Unsupported system. Running with reduced coverage.\n");
      // This is ok, continue on.
    } else {
      logprintf(0, "Process Error: Unsupported system, "
                   "no error reporting available\n");
      logprintf(0, "Log: Command line option '-A' bypasses this error.\n");
      bad_status();
      return false;
    }
  }

  return true;
}

// Allocates memory to run the test on
bool Sat::AllocateMemory() {
  // Allocate our test memory.
  bool result = os_->AllocateTestMem(size_, paddr_base_);
  if (!result) {
    logprintf(0, "Process Error: failed to allocate memory\n");
    bad_status();
    return false;
  }
  return true;
}

// Sets up access to data patterns
bool Sat::InitializePatterns() {
  // Initialize pattern data.
  patternlist_ = new PatternList();
  if (!patternlist_) {
    logprintf(0, "Process Error: failed to allocate patterns\n");
    bad_status();
    return false;
  }
  patternlist_->SetByteOffset(pattern_byte_offset_);
  if (!patternlist_->Initialize()) {
    logprintf(0, "Process Error: failed to initialize patternlist\n");
    bad_status();
    return false;
  }
  if (!pattern_selector_.empty() &&
      !patternlist_->SetPatternSequence(pattern_selector_)) {
    bad_status();
    return false;
  }
  return true;
}

// Get any valid page, no tag specified.
bool Sat::GetValid(struct page_entry *pe) {
  return GetValid(pe, kDontCareTag);
}


// Fetch and return empty and full pages into the empty and full pools.
bool Sat::GetValid(struct page_entry *pe, int32 tag) {
  bool result = false;
  // Get valid page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    result = finelock_q_->GetValid(pe, tag);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    result = valid_->PopRandom(pe);

  if (result) {
    pe->addr = os_->PrepareTestMem(pe->offset, page_length_);  // Map it.

    // Tag this access and current pattern.
    pe->ts = os_->GetTimestamp();
    pe->lastpattern = pe->pattern;

    return (pe->addr != 0);     // Return success or failure.
  }
  return false;
}

bool Sat::PutValid(struct page_entry *pe) {
  if (pe->addr != 0)
    os_->ReleaseTestMem(pe->addr, pe->offset, page_length_);  // Unmap the page.
  pe->addr = 0;

  // Put valid page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    return finelock_q_->PutValid(pe);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    return valid_->Push(pe);
  else
    return false;
}

// Get an empty page with any tag.
bool Sat::GetEmpty(struct page_entry *pe) {
  return GetEmpty(pe, kDontCareTag);
}

bool Sat::GetEmpty(struct page_entry *pe, int32 tag) {
  bool result = false;
  // Get empty page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    result = finelock_q_->GetEmpty(pe, tag);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    result = empty_->PopRandom(pe);

  if (result) {
    pe->addr = os_->PrepareTestMem(pe->offset, page_length_);  // Map it.
    return (pe->addr != 0);     // Return success or failure.
  }
  return false;
}

bool Sat::PutEmpty(struct page_entry *pe) {
  if (pe->addr != 0)
    os_->ReleaseTestMem(pe->addr, pe->offset, page_length_);  // Unmap the page.
  pe->addr = 0;
  pe->write_dram_frequency = -1;

  // Put empty page depending on implementation.
  if (pe_q_implementation_ == SAT_FINELOCK)
    return finelock_q_->PutEmpty(pe);
  else if (pe_q_implementation_ == SAT_ONELOCK)
    return empty_->Push(pe);
  else
    return false;
}

// 초기화 단계의 단일 Worker가 논리 offset 순서로 Valid entry를 조회합니다.
// Queue의 random cursor, Valid/Empty 상태와 lock 보유 상태는 변경하지 않습니다.
bool Sat::GetValidByOffsetForInitialization(
    uint64 offset, struct page_entry *pe) {
  if (!pe)
    return false;
  bool result = false;
  if (pe_q_implementation_ == SAT_FINELOCK && finelock_q_) {
    result = finelock_q_->GetValidByOffset(offset, pe);
  }
  if (!result)
    return false;

  pe->addr = os_->PrepareTestMem(pe->offset, page_length_);
  if (!pe->addr)
    return false;
  pe->ts = os_->GetTimestamp();
  pe->lastpattern = pe->pattern;
  return true;
}

// GetValidByOffsetForInitialization()에서 준비한 mapping만 해제합니다.
// Entry metadata와 queue 상태는 원래 위치에 유지됩니다.
void Sat::ReleaseInitializationPage(struct page_entry *pe) {
  if (!pe || !pe->addr)
    return;
  os_->ReleaseTestMem(pe->addr, pe->offset, page_length_);
  pe->addr = NULL;
}

// OneLock 초기 검사에서 완료 entry를 Empty queue 객체에 임시 보관합니다.
// Pattern과 write metadata는 지우지 않으며 Runtime용 Empty 상태를 의미하지
// 않습니다. 이 함수는 queue split 전 단일 post-fill Worker만 호출합니다.
bool Sat::HoldInitializationPage(struct page_entry *pe) {
  if (pe_q_implementation_ != SAT_ONELOCK || !empty_ || !pe)
    return false;
  if (pe->addr)
    os_->ReleaseTestMem(pe->addr, pe->offset, page_length_);
  pe->addr = NULL;
  return empty_->Push(pe);
}

// OneLock 임시 queue의 entry를 모두 Valid queue로 복원합니다.
bool Sat::RestoreInitializationPages(int64 count) {
  if (pe_q_implementation_ != SAT_ONELOCK || !empty_ || !valid_)
    return false;
  bool result = true;
  for (int64 i = 0; i < count; ++i) {
    struct page_entry pe;
    if (!empty_->PopRandom(&pe) || !valid_->Push(&pe)) {
      result = false;
      break;
    }
  }
  return result;
}

// Set up the bitmap of physical pages in case we want to see which pages were
// accessed under this run of SAT.
void Sat::AddrMapInit() {
  if (!do_page_map_)
    return;
  // Find about how much physical mem is in the system.
  // TODO(nsanders): Find some way to get the max
  // and min phys addr in the system.
  uint64 maxsize = os_->FindFreeMemSize() * 4;
  sat_assert(maxsize != 0);

  // Make a bitmask of this many pages. Assume that the memory is relatively
  // zero based. This is true on x86, typically.
  // This is one bit per page.
  uint64 arraysize = maxsize / 4096 / 8;
  unsigned char *bitmap = new unsigned char[arraysize];
  sat_assert(bitmap);

  // Mark every page as 0, not seen.
  memset(bitmap, 0, arraysize);

  page_bitmap_size_ = maxsize;
  page_bitmap_ = bitmap;
}

// Add the 4k pages in this block to the array of pages SAT has seen.
void Sat::AddrMapUpdate(struct page_entry *pe) {
  if (!do_page_map_)
    return;

  // Go through 4k page blocks.
  uint64 arraysize = page_bitmap_size_ / 4096 / 8;

  char *base = reinterpret_cast<char*>(pe->addr);
  for (int i = 0; i < page_length_; i += 4096) {
    uint64 paddr = os_->VirtualToPhysical(base + i);

    uint32 offset = paddr / 4096 / 8;
    unsigned char mask = 1 << ((paddr / 4096) % 8);

    if (offset >= arraysize) {
      logprintf(0, "Process Error: Physical address %#llx is "
                   "greater than expected %#llx.\n",
                paddr, page_bitmap_size_);
      sat_assert(0);
    }
    page_bitmap_[offset] |= mask;
  }
}

// Print out the physical memory ranges that SAT has accessed.
void Sat::AddrMapPrint() {
  if (!do_page_map_)
    return;

  uint64 pages = page_bitmap_size_ / 4096;

  uint64 last_page = 0;
  bool valid_range = false;

  logprintf(4, "Log: Printing tested physical ranges.\n");

  for (uint64 i = 0; i < pages; i ++) {
    int offset = i / 8;
    unsigned char mask = 1 << (i % 8);

    bool touched = page_bitmap_[offset] & mask;
    if (touched && !valid_range) {
      valid_range = true;
      last_page = i * 4096;
    } else if (!touched && valid_range) {
      valid_range = false;
      logprintf(4, "Log: %#016llx - %#016llx\n", last_page, (i * 4096) - 1);
    }
  }
  logprintf(4, "Log: Done printing physical ranges.\n");
}

// 모든 SAT 작업 단위를 한 번씩 처리하는 Fill 단계를 실행합니다.
// preset_only=true이면 동일값 사전 기록을 수행하고, 완료 후 모든 entry를
// Empty 상태로 복원하여 다음 Pattern Fill이 전체 영역을 다시 처리하게 합니다.
bool Sat::RunFillPass(bool preset_only, const char *phase) {
  bool result = true;
  WorkerStatus fill_status;
  WorkerVector fill_vector;

  logprintf(5,
            "Log: DIAG phase=%s_begin threads=%d pages=%lld direction=%s "
            "yield_bytes=%d verify_every=%d\n",
            phase, fill_threads_, pages_,
            fill_direction_ == FILL_DIRECTION_UP ? "up" : "down",
            fill_yield_bytes_, preset_only ? 0 : fill_verify_every_);

  for (int i = 0; i < fill_threads_; ++i) {
    FillThread *thread = new FillThread();
    thread->InitThread(i, this, os_, patternlist_, &fill_status);
    thread->SetPresetOnly(preset_only);
    if (i != fill_threads_ - 1)
      thread->SetFillPages(pages_ / fill_threads_);
    else
      thread->SetFillPages(pages_ - pages_ / fill_threads_ * i);
    fill_vector.push_back(thread);
  }

  fill_status.Initialize();
  vector<bool> fill_spawned(fill_vector.size(), false);
  for (size_t i = 0; i < fill_vector.size(); ++i) {
    if (fill_vector[i]->SpawnThread()) {
      fill_spawned[i] = true;
    } else {
      // InitThread()에서 증가한 WorkerStatus 수를 생성 실패 시 복원합니다.
      fill_vector[i]->RemoveUnspawnedWorker();
      result = false;
      bad_status();
    }
  }

  for (size_t i = 0; i < fill_vector.size(); ++i) {
    FillThread *thread = static_cast<FillThread*>(fill_vector[i]);
    if (fill_spawned[i])
      thread->JoinThread();
    initialization_errorcount_ += thread->GetErrorCount();
    if (fill_spawned[i] && thread->GetStatus() != 1) {
      logprintf(0, "Thread %d failed with status %d at %.2f seconds\n",
                thread->ThreadID(), thread->GetStatus(),
                thread->GetRunDurationUSec() * 1.0 / 1000000);
      result = false;
      bad_status();
    }
    delete thread;
  }
  fill_vector.clear();
  fill_status.Destroy();

  if (preset_only && result) {
    // 완료 표시로 사용한 Valid entry를 하나씩 가져와 Empty로 복원합니다.
    // 가져온 entry는 즉시 Empty가 되므로 같은 entry를 중복 처리하지 않습니다.
    for (int64 i = 0; i < pages_; ++i) {
      struct page_entry pe;
      if (!GetValid(&pe) || !PutEmpty(&pe)) {
        logprintf(0,
                  "Process Error: failed to reset preset page %lld/%lld\n",
                  i, pages_);
        result = false;
        bad_status();
        break;
      }
    }
  }

  logprintf(result ? 5 : 0,
            "Log: DIAG phase=%s_end threads=%d pages=%lld status=%d\n",
            phase, fill_threads_, pages_, result);
  LogVmStats(phase);
  return result;
}

// 전체 SAT 작업 단위를 Empty로 등록하고 초기 Pattern을 기록한 뒤 Runtime용
// Valid·Empty 비율과 물리 region tag를 구성합니다.
bool Sat::InitializePages() {
  int result = 1;
  // Runtime Worker가 동시에 보유할 수 있는 최소 Empty 작업 단위 수입니다.
  int64 neededpages = memory_threads_ +
    invert_threads_ +
    check_threads_ +
    net_threads_ +
    file_threads_;

  // FineLock queue는 Valid와 Empty를 한 구조에서 검색하므로 전체의 약 2/5를
  // Empty로 설정합니다. OneLock queue는 Worker 수에 필요한 최소 여유량을
  // 기준으로 Empty 수를 계산합니다.
  if (pe_q_implementation_ == SAT_FINELOCK)
    freepages_ = pages_ / 5 * 2;  // Mark roughly 2/5 of all pages as Empty.
  else
    freepages_ = (pages_ / 100) + (2 * neededpages);

  if (freepages_ < neededpages) {
    logprintf(0, "Process Error: freepages < neededpages.\n");
    logprintf(1, "Stats: Total: %lld, Needed: %lld, Marked free: %lld\n",
              static_cast<int64>(pages_),
              static_cast<int64>(neededpages),
              static_cast<int64>(freepages_));
    bad_status();
    return false;
  }

  if (freepages_ >  pages_/2) {
    logprintf(0, "Process Error: not enough pages for IO\n");
    logprintf(1, "Stats: Total: %lld, Needed: %lld, Available: %lld\n",
              static_cast<int64>(pages_),
              static_cast<int64>(freepages_),
              static_cast<int64>(pages_/2));
    bad_status();
    return false;
  }
  logprintf(12, "Log: Allocating pages, Total: %lld Free: %lld\n",
            pages_,
            freepages_);

  // 시험 영역을 SAT 작업 단위로 나누어 모두 Empty 상태로 등록합니다.
  for (int64 i = 0; i < pages_; i++) {
    struct page_entry pe;
    init_pe(&pe);
    pe.offset = i * page_length_;
    result &= PutEmpty(&pe);
  }

  if (!result) {
    logprintf(0, "Process Error: while initializing empty_ list\n");
    bad_status();
    return false;
  }

  if (prefault_pages_) {
    // 메인 스레드가 각 SAT 작업 단위의 offset 0부터 운영체제 page 크기
    // 간격으로 1 byte를 기록합니다. SAT 작업 단위가 운영체제 page보다 작으면
    // 각 작업 단위의 첫 byte를 기록합니다. 이 store는 Fill 전 매핑·TLB·cache
    // 상태를 변경하며 미할당 주소의 page fault와 물리 할당을 유도합니다.
    long os_page_size = sysconf(_SC_PAGESIZE);
    if (os_page_size <= 0)
      os_page_size = 4096;
    logprintf(5,
              "Log: DIAG phase=prefault_begin pages=%lld os_page_size=%ld\n",
              pages_, os_page_size);
    for (int64 i = 0; i < pages_; ++i) {
      uint64 page_offset = i * page_length_;
      void *addr = os_->PrepareTestMem(page_offset, page_length_);
      if (!addr) {
        logprintf(0,
                  "Process Error: prefault failed at SAT page %lld\n", i);
        bad_status();
        return false;
      }
      volatile unsigned char *bytes =
          static_cast<volatile unsigned char*>(addr);
      for (int64 offset = 0; offset < page_length_; offset += os_page_size)
        bytes[offset] = 0;
      os_->ReleaseTestMem(addr, page_offset, page_length_);
    }
    logprintf(5,
              "Log: DIAG phase=prefault_end pages=%lld os_page_size=%ld\n",
              pages_, os_page_size);
    LogVmStats("prefault");
  }

  const char *fill_preset_name = "none";
  if (fill_preset_ == FILL_PRESET_ZERO)
    fill_preset_name = "zero";
  else if (fill_preset_ == FILL_PRESET_ONE)
    fill_preset_name = "one";
  logprintf(5, "Log: DIAG fill_preset=%s pattern_offset=%d\n",
            fill_preset_name, pattern_byte_offset_);
  logprintf(5,
            "Log: DIAG invert_range=%s range_bytes_per_pass=%lld "
            "sat_block_bytes=%d\n",
            invert_range_ == INVERT_RANGE_FULL ? "full" : "legacy",
            invert_range_bytes(), page_length_);

  if (fill_preset_ != FILL_PRESET_NONE &&
      !RunFillPass(true, "preset_fill"))
    return false;

  // 사전 채움이 완료된 뒤 최종 Pattern을 전체 시험 영역에 기록합니다.
  if (!RunFillPass(false, "initial_fill"))
    return false;

#ifdef STRESSAPPTEST_ENABLE_TEST_HOOKS
  // 상세 비교의 오류 수, 복구, 로그 제한과 종료 요청을 반복 가능하게
  // 검증하는 CI 전용 hook입니다. Release build에는 포함되지 않습니다.
  if (test_corrupt_after_fill_words_ > 0) {
    void *address = os_->PrepareTestMem(0, page_length_);
    if (!address) {
      logprintf(0, "Process Error: test corruption mapping failed\n");
      bad_status();
      return false;
    }
    uint64 *words = static_cast<uint64 *>(address);
    for (int word = 0; word < test_corrupt_after_fill_words_; ++word)
      words[word] ^= 1;
    os_->ReleaseTestMem(address, 0, page_length_);
    logprintf(5,
              "Log: TEST_INJECTION phase=post_initial_fill "
              "sat_block=0 words=%d\n",
              test_corrupt_after_fill_words_);
  }
#endif

  // Fill에서 종료 요청이 기록되면 진단용 대기를 생략합니다.
  if (post_fill_delay_seconds_ > 0 && !error_stop_requested()) {
    logprintf(5,
              "Log: DIAG phase=post_fill_delay_begin seconds=%d\n",
              post_fill_delay_seconds_);
    sat_sleep(post_fill_delay_seconds_);
    logprintf(5,
              "Log: DIAG phase=post_fill_delay_end seconds=%d\n",
              post_fill_delay_seconds_);
  }

  if (verify_after_fill_ && !error_stop_requested()) {
    // Runtime용 Valid·Empty 상태를 구성하기 전에 초기 Pattern을 검사합니다.
    // --stop_on_errors가 설정되면 현재 작업 단위의 상세 검사 후 종료합니다.
    WorkerStatus verify_status;
    PostFillCheckThread *verify_thread = new PostFillCheckThread();
    verify_thread->SetPagesToCheck(pages_);
    verify_thread->InitThread(fill_threads_, this, os_, patternlist_,
                              &verify_status);
    verify_status.Initialize();
    if (!verify_thread->SpawnThread()) {
      verify_thread->RemoveUnspawnedWorker();
      delete verify_thread;
      verify_status.Destroy();
      bad_status();
      return false;
    }
    verify_thread->JoinThread();

    initialization_errorcount_ += verify_thread->GetErrorCount();
    if (verify_thread->GetStatus() != 1) {
      logprintf(0,
                "Process Error: post-fill verification failed with status %d\n",
                verify_thread->GetStatus());
      result = false;
      bad_status();
    }
    delete verify_thread;
    verify_status.Destroy();
    LogVmStats("post_fill_check");

    if (!result)
      return false;
  }

  logprintf(5,
            "Log: DIAG phase=queue_split_begin pages=%lld empty_target=%lld\n",
            pages_, freepages_);
  logprintf(12, "Log: Allocating pages.\n");

  AddrMapInit();

  // 전체 작업 단위를 다시 가져와 물리 region tag와 Runtime 상태를 설정합니다.
  for (int64 i = 0; i < pages_; i++) {
    struct page_entry pe;
    // Only get valid pages with uninitialized tags here.
    if (GetValid(&pe, kInvalidTag)) {
      int64 paddr = os_->VirtualToPhysical(pe.addr);
      int32 region = os_->FindRegion(paddr);
      region_[region]++;
      pe.paddr = paddr;
      pe.tag = 1 << region;
      region_mask_ |= pe.tag;

      // 선택 옵션이 활성화된 경우 접근한 물리 페이지를 bitmap에 기록합니다.
      AddrMapUpdate(&pe);

      // 앞에서 계산한 수만큼 Empty로 전환하고 나머지는 Valid로 유지합니다.
      // Region별 Empty 개수를 별도로 강제하지 않으므로 실제 분포는 실행마다
      // 달라질 수 있습니다.
      if (i < freepages_) {
        result &= PutEmpty(&pe);
      } else {
        result &= PutValid(&pe);
      }
    } else {
      logprintf(0, "Log: didn't tag all pages. %d - %d = %d\n",
                pages_, i, pages_ - i);
      return false;
    }
  }
  logprintf(12, "Log: Done allocating pages.\n");
  logprintf(5,
            "Log: DIAG phase=queue_split_end valid=%lld empty=%lld\n",
            pages_ - freepages_, freepages_);
  LogVmStats("queue_split");

  AddrMapPrint();

  for (int i = 0; i < 32; i++) {
    if (region_mask_ & (1 << i)) {
      region_count_++;
      logprintf(12, "Log: Region %d: %d.\n", i, region_[i]);
    }
  }
  logprintf(5, "Log: Region mask: 0x%x\n", region_mask_);

  // 초기 검사에서 종료 요청이 기록되면 Runtime 전 대기를 수행하지 않습니다.
  if (runtime_start_delay_seconds_ > 0 && !error_stop_requested()) {
    logprintf(5,
              "Log: DIAG phase=runtime_start_delay_begin seconds=%d\n",
              runtime_start_delay_seconds_);
    sat_sleep(runtime_start_delay_seconds_);
    logprintf(5,
              "Log: DIAG phase=runtime_start_delay_end seconds=%d\n",
              runtime_start_delay_seconds_);
  }

  return true;
}

// Print SAT version info.
bool Sat::PrintVersion() {
  logprintf(1, "Stats: SAT revision %s, %d bit binary\n",
            kVersion, address_mode_);
  logprintf(5, "Log: %s from %s\n", Timestamp(), BuildChangelist());

  return true;
}


// Initializes the resources that SAT needs to run.
// This needs to be called before Run(), and after ParseArgs().
// Returns true on success, false on error, and will exit() on help message.
bool Sat::Initialize() {
  g_sat = this;
  diagnostic_start_us_ = sat_get_time_us();

  // Initializes sync'd log file to ensure output is saved.
  if (!InitializeLogfile())
    return false;
  Logger::GlobalLogger()->SetTimestampLogging(log_timestamps_);
  Logger::GlobalLogger()->StartThread();

  logprintf(5, "Log: Commandline - %s\n", cmdline_.c_str());
  PrintVersion();

  std::map<std::string, std::string> options;

  GoogleOsOptions(&options);

  // Initialize OS/Hardware interface.
  os_ = OsLayerFactory(options);
  if (!os_) {
    bad_status();
    return false;
  }

  if (min_hugepages_mbytes_ > 0)
    os_->SetMinimumHugepagesSize(min_hugepages_mbytes_ * kMegabyte);

  if (reserve_mb_ > 0)
    os_->SetReserveSize(reserve_mb_);

  if (channels_.size() > 0) {
    logprintf(6, "Log: Decoding memory: %dx%d bit channels,"
        "%d modules per channel (x%d), decoding hash 0x%x\n",
        channels_.size(), channel_width_, channels_[0].size(),
        channel_width_/channels_[0].size(), channel_hash_);
    os_->SetDramMappingParams(channel_hash_, channel_width_, &channels_);
  }

  if (!os_->Initialize()) {
    logprintf(0, "Process Error: Failed to initialize OS layer\n");
    bad_status();
    delete os_;
    return false;
  }

  // Checks that OS/Build/Platform is supported.
  if (!CheckEnvironment())
    return false;

  if (error_injection_)
    os_->set_error_injection(true);

  // Run SAT in monitor only mode, do not continue to allocate resources.
  if (monitor_mode_) {
    logprintf(5, "Log: Running in monitor-only mode. "
                 "Will not allocate any memory nor run any stress test. "
                 "Only polling ECC errors.\n");
    return true;
  }

  // Allocate the memory to test.
  if (!AllocateMemory())
    return false;
  LogVmStats("allocation");

  logprintf(5, "Stats: Starting SAT, %dM, %d seconds\n",
            static_cast<int>(size_/kMegabyte),
            runtime_seconds_);

  if (!InitializePatterns())
    return false;

  // 초기 Pattern Fill 전에 목록의 첫 DDR 요청값을 전달합니다.
  // Runtime 시작 시 첫 값을 다시 전달하고 sweep 간격을 새로 계산합니다.
  if (!dram_frequencies_.empty() &&
      !ApplyDramFrequency(dram_frequencies_[0])) {
    bad_status();
    return false;
  }

  // Initialize memory allocation.
  pages_ = size_ / page_length_;
  if (diag_phase_summary_) {
    const char *final_mode = skip_final_check_
        ? "skip"
        : (final_check_threads_explicit_ ? "separate" : "legacy_drain");
    const int configured_final_threads = skip_final_check_
        ? 0
        : (final_check_threads_explicit_
               ? final_check_threads_ : fill_threads_);
    const char *fill_preset =
        fill_preset_ == FILL_PRESET_ZERO
            ? "zero"
            : (fill_preset_ == FILL_PRESET_ONE ? "one" : "none");
    logprintf(5,
              "Log: DIAG_CONFIG blocks=%lld block_bytes=%d queue=%s "
              "workers(fill=%d,copy=%d,invert=%d,check=%d) "
              "invert_range=%s final_mode=%s final_threads=%d "
              "ddr_mode=%s ddr_count=%zu ddr_step_s=%d "
              "ddr_sweep_phase=runtime\n",
              pages_, page_length_,
              pe_q_implementation_ == SAT_FINELOCK ? "fine" : "coarse",
              fill_threads_, memory_threads_, invert_threads_, check_threads_,
              invert_range_ == INVERT_RANGE_FULL ? "full" : "legacy",
              final_mode, configured_final_threads,
              dram_frequency_mode(), dram_frequencies_.size(),
              dram_step_seconds_);
    // 비교 실행에서 workload를 바꾸는 옵션의 누락을 확인할 수 있도록
    // 해석된 값을 시작 시 한 번 기록합니다. 시험 메모리에는 접근하지 않습니다.
    logprintf(5,
              "Log: DIAG_CONFIG_OPTIONS pattern_offset=%d "
              "fill_preset=%s prefault=%d fill_direction=%s "
              "fill_verify_every=%d fill_yield_bytes=%d "
              "verify_after_fill=%d post_fill_delay_s=%d "
              "runtime_start_delay_s=%d copy_verify_destination=%d "
              "strict=%d warm=%d tag_mode=%d cpu_stress=%d affinity=%d "
              "block_history=%d vm_stats=%d "
              "error_log_limit=%lld stop_on_errors=%d dram_map=%s\n",
              pattern_byte_offset_, fill_preset, prefault_pages_ ? 1 : 0,
              fill_direction_ == FILL_DIRECTION_DOWN ? "down" : "up",
              fill_verify_every_, fill_yield_bytes_,
              verify_after_fill_ ? 1 : 0, post_fill_delay_seconds_,
              runtime_start_delay_seconds_,
              copy_verify_destination_ ? 1 : 0, strict_,
              warm_, tag_mode_, cpu_stress_threads_, use_affinity_ ? 1 : 0,
              diag_block_history_ ? 1 : 0, diag_vm_stats_ ? 1 : 0,
              error_log_limit_, stop_on_error_ ? 1 : 0,
              dram_address_map_profile_ == DRAM_ADDRESS_MAP_LPDDR_V1
                  ? "lpddr-v1" : "none");
  }
  if (diag_block_history_) {
    block_history_ = new (std::nothrow) BlockHistory[pages_];
    if (!block_history_) {
      logprintf(0,
                "Process Error: failed to allocate block history for "
                "%lld SAT blocks\n",
                pages_);
      bad_status();
      return false;
    }
    for (int64 i = 0; i < pages_; ++i) {
      block_history_[i].generation = 0;
      block_history_[i].write_complete_us = -1;
      block_history_[i].frequency_epoch_begin = 0;
      block_history_[i].frequency_epoch_end = 0;
      block_history_[i].writer_thread = -1;
      block_history_[i].writer_cpu = -1;
      block_history_[i].writer = BLOCK_WRITER_UNKNOWN;
    }
    logprintf(5,
              "Log: DIAG block_history=enabled blocks=%lld bytes=%lld\n",
              pages_,
              static_cast<int64>(sizeof(BlockHistory)) * pages_);
  }

  // Allocate page queue depending on queue implementation switch.
  if (pe_q_implementation_ == SAT_FINELOCK) {
      finelock_q_ = new FineLockPEQueue(pages_, page_length_);
      if (finelock_q_ == NULL)
        return false;
      finelock_q_->set_os(os_);
      os_->set_err_log_callback(finelock_q_->get_err_log_callback());
  } else if (pe_q_implementation_ == SAT_ONELOCK) {
      empty_ = new PageEntryQueue(pages_);
      valid_ = new PageEntryQueue(pages_);
      if ((empty_ == NULL) || (valid_ == NULL))
        return false;
  }

  if (!InitializePages()) {
    logprintf(0, "Process Error: Initialize Pages failed\n");
    return false;
  }

  return true;
}

// Constructor and destructor.
Sat::Sat() {
  // Set defaults, command line might override these.
  runtime_seconds_ = 20;
  page_length_ = kSatPageSize;
  disk_pages_ = kSatDiskPage;
  pages_ = 0;
  size_mb_ = 0;
  size_ = size_mb_ * kMegabyte;
  reserve_mb_ = 0;
  min_hugepages_mbytes_ = 0;
  freepages_ = 0;
  paddr_base_ = 0;
  channel_hash_ = kCacheLineSize;
  channel_width_ = 64;
  pattern_selector_ = "";
  pattern_byte_offset_ = 0;
  post_fill_delay_seconds_ = 0;
  runtime_start_delay_seconds_ = 0;
  dram_frequencies_.clear();
  dram_sweep_ = false;
  dram_step_seconds_ = 3;
  dram_frequency_node_ = kDefaultDramFrequencyNode;
  current_dram_frequency_.store(-1, std::memory_order_relaxed);
  dram_frequency_epoch_.store(0, std::memory_order_relaxed);
  dram_address_map_profile_ = DRAM_ADDRESS_MAP_NONE;

  user_break_ = false;
  verbosity_ = 8;
  Logger::GlobalLogger()->SetVerbosity(verbosity_);
  print_delay_ = 10;
  strict_ = 1;
  warm_ = 0;
  verify_after_fill_ = false;
  copy_verify_destination_ = false;
  prefault_pages_ = false;
  diag_vm_stats_ = false;
  diag_block_history_ = false;
  diag_phase_summary_ = false;
  fill_preset_ = FILL_PRESET_NONE;
  fill_direction_ = FILL_DIRECTION_UP;
  fill_verify_every_ = 0;
  fill_yield_bytes_ = 0;
  // 옵션을 지정하지 않은 실행은 upstream의 Invert workload를 보존합니다.
  // 전체 SAT 작업 단위 반전은 --invert-range full로 명시합니다.
  invert_range_ = INVERT_RANGE_LEGACY;
  final_check_threads_ = 8;
  final_check_threads_explicit_ = false;
  skip_final_check_ = false;
  error_stop_requested_.store(false, std::memory_order_relaxed);
  run_on_anything_ = 0;
  use_logfile_ = false;
  logfile_ = 0;
  log_timestamps_ = true;
  // Detect 32/64 bit binary.
  void *pvoid = 0;
  address_mode_ = sizeof(pvoid) * 8;
  error_injection_ = false;
  crazy_error_injection_ = false;
#ifdef STRESSAPPTEST_ENABLE_TEST_HOOKS
  test_corrupt_after_fill_words_ = 0;
#endif
  max_errorcount_ = 0;  // Zero means no early exit.
  stop_on_error_ = false;
  error_log_limit_ = -1;
  error_log_attempted_.store(0, std::memory_order_relaxed);
  error_log_detailed_.store(0, std::memory_order_relaxed);
  error_log_suppressed_.store(0, std::memory_order_relaxed);
  error_poll_ = true;
  findfiles_ = false;

  do_page_map_ = false;
  page_bitmap_ = 0;
  page_bitmap_size_ = 0;

  // Cache coherency data initialization.
  cc_test_ = false;         // Flag to trigger cc threads.
  cc_cacheline_count_ = 2;  // Two datastructures of cache line size.
  cc_cacheline_size_ = 0;   // Size of a cacheline (0 for auto-detect).
  cc_inc_count_ = 1000;     // Number of times to increment the shared variable.
  cc_cacheline_data_ = 0;   // Cache Line size datastructure.

  // Cpu frequency data initialization.
  cpu_freq_test_ = false;   // Flag to trigger cpu frequency thread.
  cpu_freq_threshold_ = 0;  // Threshold, in MHz, at which a cpu fails.
  cpu_freq_round_ = 10;     // Round the computed frequency to this value.

  sat_assert(0 == pthread_mutex_init(&worker_lock_, NULL));
  sat_assert(0 == pthread_mutex_init(&diagnostic_stats_lock_, NULL));
  file_threads_ = 0;
  net_threads_ = 0;
  listen_threads_ = 0;
  // Default to autodetect number of cpus, and run that many threads.
  memory_threads_ = -1;
  invert_threads_ = 0;
  fill_threads_ = 8;
  check_threads_ = 0;
  cpu_stress_threads_ = 0;
  disk_threads_ = 0;
  total_threads_ = 0;

  use_affinity_ = true;
  region_mask_ = 0;
  region_count_ = 0;
  for (int i = 0; i < 32; i++) {
    region_[i] = 0;
  }
  region_mode_ = 0;

  errorcount_ = 0;
  initialization_errorcount_ = 0;
  statuscount_ = 0;
  vm_stats_last_minor_faults_ = -1;
  vm_stats_last_major_faults_ = -1;
  block_history_ = NULL;
  diagnostic_start_us_ = sat_get_time_us();
  for (int i = 0; i < DIAG_PHASE_COUNT; ++i) {
    diagnostic_phase_stats_[i].blocks = 0;
    diagnostic_phase_stats_[i].read_bytes = 0;
    diagnostic_phase_stats_[i].write_bytes = 0;
    diagnostic_phase_stats_[i].checksum_mismatch_regions = 0;
    diagnostic_phase_stats_[i].word_mismatches = 0;
    diagnostic_phase_stats_[i].first_error_us = -1;
    diagnostic_phase_stats_[i].first_error_epoch = 0;
    diagnostic_phase_stats_[i].first_error_worker_bytes = 0;
  }

  valid_ = 0;
  empty_ = 0;
  finelock_q_ = 0;
  // Default to use fine-grain lock for better performance.
  pe_q_implementation_ = SAT_FINELOCK;

  os_ = 0;
  patternlist_ = 0;
  logfilename_[0] = 0;

  read_block_size_ = 512;
  write_block_size_ = -1;
  segment_size_ = -1;
  cache_size_ = -1;
  blocks_per_segment_ = -1;
  read_threshold_ = -1;
  write_threshold_ = -1;
  non_destructive_ = 1;
  monitor_mode_ = 0;
  tag_mode_ = 0;
  random_threads_ = 0;

  pause_delay_ = 600;
  pause_duration_ = 15;
}

// Destructor.
Sat::~Sat() {
  // We need to have called Cleanup() at this point.
  // We should probably enforce this.
}


#define ARG_KVALUE(argument, variable, value)         \
  if (!strcmp(argv[i], argument)) {                   \
    variable = value;                                 \
    continue;                                         \
  }

#define ARG_IVALUE(argument, variable)                \
  if (!strcmp(argv[i], argument)) {                   \
    i++;                                              \
    if (i < argc)                                     \
      variable = strtoull(argv[i], NULL, 0);          \
    continue;                                         \
  }

#define ARG_SVALUE(argument, variable)                     \
  if (!strcmp(argv[i], argument)) {                        \
    i++;                                                   \
    if (i < argc)                                          \
      snprintf(variable, sizeof(variable), "%s", argv[i]); \
    continue;                                              \
  }

// Configures SAT from command line arguments.
// This will call exit() given a request for
// self-documentation or unexpected args.
bool Sat::ParseArgs(int argc, char **argv) {
  int i;
  uint64 filesize = page_length_ * disk_pages_;

  // Parse each argument.
  for (i = 1; i < argc; i++) {
    // Switch to fall back to corase-grain-lock queue. (for benchmarking)
    ARG_KVALUE("--coarse_grain_lock", pe_q_implementation_, SAT_ONELOCK);

    // Set number of megabyte to use.
    ARG_IVALUE("-M", size_mb_);

    // Specify the amount of megabytes to be reserved for system.
    ARG_IVALUE("--reserve_memory", reserve_mb_);

    // Set minimum megabytes of hugepages to require.
    ARG_IVALUE("-H", min_hugepages_mbytes_);

    // Set number of seconds to run.
    ARG_IVALUE("-s", runtime_seconds_);

    // 0부터 시작하는 ID 또는 Pattern 이름을 하나 이상 입력받습니다.
    if (!strcmp(argv[i], "-P")) {
      if (++i >= argc) {
        logprintf(0,
                  "Process Error: -P requires IDs or names separated by "
                  "commas\n");
        return false;
      }
      pattern_selector_ = argv[i];
      continue;
    }

    // SAT 작업 단위 시작점을 기준으로 Pattern 위치를 이동합니다.
    if (!strcmp(argv[i], "--pattern-byte-offset")) {
      if (++i >= argc ||
          !ParseNonNegativeInt(argv[i], &pattern_byte_offset_) ||
          (pattern_byte_offset_ % static_cast<int>(sizeof(unsigned int))) != 0) {
        logprintf(0,
                  "Process Error: --pattern-byte-offset requires a "
                  "non-negative multiple of 4 bytes\n");
        return false;
      }
      continue;
    }

    // 최종 Pattern을 기록하기 전에 전체 영역에 동일값을 사전 기록합니다.
    if (!strcmp(argv[i], "--fill-preset")) {
      if (++i >= argc) {
        logprintf(0,
                  "Process Error: --fill-preset requires none, zero, or one\n");
        return false;
      }
      if (!strcmp(argv[i], "none")) {
        fill_preset_ = FILL_PRESET_NONE;
      } else if (!strcmp(argv[i], "zero")) {
        fill_preset_ = FILL_PRESET_ZERO;
      } else if (!strcmp(argv[i], "one")) {
        fill_preset_ = FILL_PRESET_ONE;
      } else {
        logprintf(0,
                  "Process Error: --fill-preset requires none, zero, or one\n");
        return false;
      }
      continue;
    }

    ARG_KVALUE("--verify-after-fill", verify_after_fill_, true);
    ARG_KVALUE("--copy-verify-destination", copy_verify_destination_, true);
    ARG_KVALUE("--prefault-pages", prefault_pages_, true);
    ARG_KVALUE("--diag-vm-stats", diag_vm_stats_, true);
    ARG_KVALUE("--diag-block-history", diag_block_history_, true);
    ARG_KVALUE("--diag-phase-summary", diag_phase_summary_, true);

    if (!strcmp(argv[i], "--post-fill-delay")) {
      if (++i >= argc ||
          !ParseNonNegativeInt(argv[i], &post_fill_delay_seconds_)) {
        logprintf(0,
                  "Process Error: --post-fill-delay requires non-negative "
                  "seconds\n");
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--fill-threads")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &fill_threads_) ||
          fill_threads_ > kMaxDiagnosticWorkerCount) {
        logprintf(0,
                  "Process Error: --fill-threads requires a count from 1 "
                  "to %d\n", kMaxDiagnosticWorkerCount);
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--fill-direction")) {
      if (++i >= argc) {
        logprintf(0,
                  "Process Error: --fill-direction requires up or down\n");
        return false;
      }
      if (!strcmp(argv[i], "up")) {
        fill_direction_ = FILL_DIRECTION_UP;
      } else if (!strcmp(argv[i], "down")) {
        fill_direction_ = FILL_DIRECTION_DOWN;
      } else {
        logprintf(0,
                  "Process Error: --fill-direction requires up or down\n");
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--fill-verify-every")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &fill_verify_every_)) {
        logprintf(0,
                  "Process Error: --fill-verify-every requires a positive "
                  "SAT block interval\n");
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--fill-yield-bytes")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &fill_yield_bytes_) ||
          (fill_yield_bytes_ % kCacheLineSize) != 0) {
        logprintf(0,
                  "Process Error: --fill-yield-bytes requires a positive "
                  "multiple of %d bytes\n", kCacheLineSize);
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--runtime-start-delay")) {
      if (++i >= argc ||
          !ParseNonNegativeInt(argv[i], &runtime_start_delay_seconds_)) {
        logprintf(0,
                  "Process Error: --runtime-start-delay requires "
                  "non-negative seconds\n");
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--invert-range")) {
      if (++i >= argc) {
        logprintf(0,
                  "Process Error: --invert-range requires legacy or full\n");
        return false;
      }
      if (!strcmp(argv[i], "legacy")) {
        invert_range_ = INVERT_RANGE_LEGACY;
      } else if (!strcmp(argv[i], "full")) {
        invert_range_ = INVERT_RANGE_FULL;
      } else {
        logprintf(0,
                  "Process Error: --invert-range requires legacy or full\n");
        return false;
      }
      continue;
    }

    if (!strcmp(argv[i], "--final-check-threads")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &final_check_threads_) ||
          final_check_threads_ > kMaxDiagnosticWorkerCount) {
        logprintf(0,
                  "Process Error: --final-check-threads requires a count "
                  "from 1 to %d\n", kMaxDiagnosticWorkerCount);
        return false;
      }
      final_check_threads_explicit_ = true;
      continue;
    }

    ARG_KVALUE("--skip-final-check", skip_final_check_, true);

    // 단일 요청값 또는 Runtime에서 순환할 요청값 목록을 설정합니다.
    if (!strcmp(argv[i], "--ddr-freq")) {
      if (++i >= argc) {
        logprintf(0,
                  "Process Error: --ddr-freq requires one frequency, "
                  "a comma-separated list, or 'all'\n");
        return false;
      }
      if (!dram_frequencies_.empty()) {
        logprintf(0, "Process Error: --ddr-freq may be specified once\n");
        return false;
      }
      if (!ParseDramFrequencyList(argv[i], &dram_frequencies_)) {
        logprintf(0, "Process Error: Invalid --ddr-freq value '%s'\n",
                  argv[i]);
        return false;
      }
      dram_sweep_ = dram_frequencies_.size() > 1;
      continue;
    }

    // Runtime 주파수 순환에서 다음 요청값을 전달할 간격을 설정합니다.
    if (!strcmp(argv[i], "--ddr-step")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &dram_step_seconds_)) {
        logprintf(0,
                  "Process Error: --ddr-step requires positive seconds\n");
        return false;
      }
      continue;
    }

    // 대상 시스템이 제공하는 DDR 제어용 kernel interface 경로를 지정합니다.
    if (!strcmp(argv[i], "--ddr-node")) {
      if (++i >= argc || argv[i][0] == '\0') {
        logprintf(0, "Process Error: --ddr-node requires a path\n");
        return false;
      }
      dram_frequency_node_ = argv[i];
      continue;
    }

    // 오류 로그에 적용할 선택형 물리 주소-DRAM 좌표 변환 프로필을 설정합니다.
    if (!strcmp(argv[i], "--dram-map")) {
      if (++i >= argc) {
        logprintf(0, "Process Error: --dram-map requires a profile\n");
        return false;
      }
      if (!strcmp(argv[i], "lpddr-v1")) {
        dram_address_map_profile_ = DRAM_ADDRESS_MAP_LPDDR_V1;
      } else if (!strcmp(argv[i], "none")) {
        dram_address_map_profile_ = DRAM_ADDRESS_MAP_NONE;
      } else {
        logprintf(0,
                  "Process Error: Invalid --dram-map profile '%s'\n",
                  argv[i]);
        return false;
      }
      continue;
    }

    // Set number of memory copy threads.
    ARG_IVALUE("-m", memory_threads_);

    // Set number of memory invert threads.
    ARG_IVALUE("-i", invert_threads_);

    // Set number of check-only threads.
    ARG_IVALUE("-c", check_threads_);

    // Set number of cache line size datastructures.
    ARG_IVALUE("--cc_inc_count", cc_inc_count_);

    // Set number of cache line size datastructures
    ARG_IVALUE("--cc_line_count", cc_cacheline_count_);

    // Override the detected or assumed cache line size.
    ARG_IVALUE("--cc_line_size", cc_cacheline_size_);

    // Flag set when cache coherency tests need to be run
    ARG_KVALUE("--cc_test", cc_test_, true);

    // Set when the cpu_frequency test needs to be run
    ARG_KVALUE("--cpu_freq_test", cpu_freq_test_, true);

    // Set the threshold in MHz at which the cpu frequency test will fail.
    ARG_IVALUE("--cpu_freq_threshold", cpu_freq_threshold_);

    // Set the rounding value for the cpu frequency test. The default is to
    // round to the nearest 10s value.
    ARG_IVALUE("--cpu_freq_round", cpu_freq_round_);

    // Set number of CPU stress threads.
    ARG_IVALUE("-C", cpu_stress_threads_);

    // Set logfile name.
    ARG_SVALUE("-l", logfilename_);

    // Verbosity level.
    ARG_IVALUE("-v", verbosity_);

    // Runtime 진행 로그의 출력 간격입니다. 누락된 값, 숫자 뒤의 문자,
    // int 범위 초과값을 ParsePositiveInt()에서 함께 거부합니다.
    if (!strcmp(argv[i], "--printsec")) {
      if (++i >= argc || !ParsePositiveInt(argv[i], &print_delay_)) {
        logprintf(0,
                  "Process Error: --printsec requires positive seconds\n");
        return false;
      }
      continue;
    }

    // Turn off timestamps logging.
    ARG_KVALUE("--no_timestamps", log_timestamps_, false);

    // Set maximum number of errors to collect. Stop running after this many.
    ARG_IVALUE("--max_errors", max_errorcount_);

    // Set pattern block size.
    ARG_IVALUE("-p", page_length_);

    // Set pattern block size.
    ARG_IVALUE("--filesize", filesize);

    // NUMA options.
    ARG_KVALUE("--no_affinity", use_affinity_, false);
    ARG_KVALUE("--local_numa", region_mode_, kLocalNuma);
    ARG_KVALUE("--remote_numa", region_mode_, kRemoteNuma);

    // Autodetect tempfile locations.
    ARG_KVALUE("--findfiles", findfiles_, 1);

    // Inject errors to force miscompare code paths
    ARG_KVALUE("--force_errors", error_injection_, true);
    ARG_KVALUE("--force_errors_like_crazy", crazy_error_injection_, true);
    if (crazy_error_injection_)
      error_injection_ = true;

#ifdef STRESSAPPTEST_ENABLE_TEST_HOOKS
    // 공개 release에 포함하지 않는 결정적 memory mismatch 주입 옵션입니다.
    if (!strcmp(argv[i], "--test-corrupt-after-fill-words")) {
      if (++i >= argc ||
          !ParsePositiveInt(argv[i], &test_corrupt_after_fill_words_)) {
        logprintf(0,
                  "Process Error: --test-corrupt-after-fill-words requires "
                  "a positive count\n");
        return false;
      }
      continue;
    }
#endif

    // 상세 mismatch 처리 후 메인 제어 경로에 종료를 요청합니다.
    ARG_KVALUE("--stop_on_errors", stop_on_error_, 1);

    if (!strcmp(argv[i], "--error-log-limit")) {
      int parsed_limit = 0;
      if (++i >= argc ||
          !ParseNonNegativeInt(argv[i], &parsed_limit)) {
        logprintf(0,
                  "Process Error: --error-log-limit requires a "
                  "non-negative count\n");
        return false;
      }
      error_log_limit_ = parsed_limit;
      continue;
    }

    // Don't use internal error polling, allow external detection.
    ARG_KVALUE("--no_errors", error_poll_, 0);

    // Never check data as you go.
    ARG_KVALUE("-F", strict_, 0);

    // Warm the cpu as you go.
    ARG_KVALUE("-W", warm_, 1);

    // Allow runnign on unknown systems with base unimplemented OsLayer
    ARG_KVALUE("-A", run_on_anything_, 1);

    // Size of read blocks for disk test.
    ARG_IVALUE("--read-block-size", read_block_size_);

    // Size of write blocks for disk test.
    ARG_IVALUE("--write-block-size", write_block_size_);

    // Size of segment for disk test.
    ARG_IVALUE("--segment-size", segment_size_);

    // Size of disk cache size for disk test.
    ARG_IVALUE("--cache-size", cache_size_);

    // Number of blocks to test per segment.
    ARG_IVALUE("--blocks-per-segment", blocks_per_segment_);

    // Maximum time a block read should take before warning.
    ARG_IVALUE("--read-threshold", read_threshold_);

    // Maximum time a block write should take before warning.
    ARG_IVALUE("--write-threshold", write_threshold_);

    // Do not write anything to disk in the disk test.
    ARG_KVALUE("--destructive", non_destructive_, 0);

    // Run SAT in monitor mode. No test load at all.
    ARG_KVALUE("--monitor_mode", monitor_mode_, true);

    // Run SAT in address mode. Tag all cachelines by virt addr.
    ARG_KVALUE("--tag_mode", tag_mode_, true);

    // Dump range map of tested pages..
    ARG_KVALUE("--do_page_map", do_page_map_, true);

    // Specify the physical address base to test.
    ARG_IVALUE("--paddr_base", paddr_base_);

    // 전력 부하 pause의 반복 간격입니다. time_t에 대입하기 전에 int
    // 범위의 양의 정수인지 검사하여 파싱 및 시간 계산 overflow를 막습니다.
    if (!strcmp(argv[i], "--pause_delay")) {
      int parsed_pause_delay = 0;
      if (++i >= argc ||
          !ParsePositiveInt(argv[i], &parsed_pause_delay)) {
        logprintf(0,
                  "Process Error: --pause_delay requires positive seconds\n");
        return false;
      }
      pause_delay_ = parsed_pause_delay;
      continue;
    }

    // Specify the duration of each pause (for power spikes).
    ARG_IVALUE("--pause_duration", pause_duration_);

    // Disk device names
    if (!strcmp(argv[i], "-d")) {
      i++;
      if (i < argc) {
        disk_threads_++;
        diskfilename_.push_back(string(argv[i]));
        blocktables_.push_back(new DiskBlockTable());
      }
      continue;
    }

    // Set number of disk random threads for each disk write thread.
    ARG_IVALUE("--random-threads", random_threads_);

    // Set a tempfile to use in a file thread.
    if (!strcmp(argv[i], "-f")) {
      i++;
      if (i < argc) {
        file_threads_++;
        filename_.push_back(string(argv[i]));
      }
      continue;
    }

    // Set a hostname to use in a network thread.
    if (!strcmp(argv[i], "-n")) {
      i++;
      if (i < argc) {
        net_threads_++;
        ipaddrs_.push_back(string(argv[i]));
      }
      continue;
    }

    // Run threads that listen for incoming SAT net connections.
    ARG_KVALUE("--listen", listen_threads_, 1);

    if (CheckGoogleSpecificArgs(argc, argv, &i)) {
      continue;
    }

    ARG_IVALUE("--channel_hash", channel_hash_);
    ARG_IVALUE("--channel_width", channel_width_);

    if (!strcmp(argv[i], "--memory_channel")) {
      i++;
      if (i < argc) {
        char *channel = argv[i];
        channels_.push_back(vector<string>());
        while (char* next = strchr(channel, ',')) {
          channels_.back().push_back(string(channel, next - channel));
          channel = next + 1;
        }
        channels_.back().push_back(string(channel));
      }
      continue;
    }

    // Default:
    PrintVersion();
    PrintHelp();
    if (strcmp(argv[i], "-h") && strcmp(argv[i], "--help")) {
      printf("\n Unknown argument %s\n", argv[i]);
      bad_status();
      exit(1);
    }
    // Forget it, we printed the help, just bail.
    // We don't want to print test status, or any log parser stuff.
    exit(0);
  }

  Logger::GlobalLogger()->SetVerbosity(verbosity_);

  // Update relevant data members with parsed input.
  // Translate MB into bytes.
  size_ = static_cast<int64>(size_mb_) * kMegabyte;

  // Set logfile flag.
  if (strcmp(logfilename_, ""))
    use_logfile_ = true;
  // Checks valid page length.
  if (page_length_ &&
      !(page_length_ & (page_length_ - 1)) &&
      (page_length_ > 1023)) {
    // Prints if we have changed from default.
    if (page_length_ != kSatPageSize)
      logprintf(12, "Log: Updating page size to %d\n", page_length_);
  } else {
    // Revert to default page length.
    logprintf(6, "Process Error: "
              "Invalid page size %d\n", page_length_);
    page_length_ = kSatPageSize;
    return false;
  }

  if (fill_yield_bytes_ > page_length_) {
    logprintf(0,
              "Process Error: --fill-yield-bytes %d exceeds SAT block "
              "size %d\n", fill_yield_bytes_, page_length_);
    return false;
  }

#ifdef STRESSAPPTEST_ENABLE_TEST_HOOKS
  if (test_corrupt_after_fill_words_ >
      page_length_ / static_cast<int>(sizeof(uint64))) {
    logprintf(0,
              "Process Error: --test-corrupt-after-fill-words exceeds "
              "the SAT block size\n");
    return false;
  }
#endif

  // NextOccurance()는 출력과 pause 간격을 나눌셈에 사용합니다.
  // 0 이하의 값을 초기에 거부하여 Runtime 중 나눌셈 오류를 방지합니다.
  if (print_delay_ <= 0 || pause_delay_ <= 0) {
    logprintf(0,
              "Process Error: --printsec and --pause_delay must be greater "
              "than zero\n");
    return false;
  }

  // Set disk_pages_ if filesize or page size changed.
  if (filesize != static_cast<uint64>(page_length_) *
                  static_cast<uint64>(disk_pages_)) {
    disk_pages_ = filesize / page_length_;
    if (disk_pages_ == 0)
      disk_pages_ = 1;
  }

  // Validate memory channel parameters if supplied
  if (channels_.size()) {
    if (channels_.size() == 1) {
      channel_hash_ = 0;
      logprintf(7, "Log: "
          "Only one memory channel...deactivating interleave decoding.\n");
    } else if (channels_.size() > 2) {
      logprintf(6, "Process Error: "
          "Triple-channel mode not yet supported... sorry.\n");
      bad_status();
      return false;
    }
    for (uint i = 0; i < channels_.size(); i++)
      if (channels_[i].size() != channels_[0].size()) {
        logprintf(6, "Process Error: "
            "Channels 0 and %d have a different count of dram modules.\n", i);
        bad_status();
        return false;
      }
    if (channels_[0].size() & (channels_[0].size() - 1)) {
      logprintf(6, "Process Error: "
          "Amount of modules per memory channel is not a power of 2.\n");
      bad_status();
      return false;
    }
    if (channel_width_ < 16
        || channel_width_ & (channel_width_ - 1)) {
      logprintf(6, "Process Error: "
          "Channel width %d is invalid.\n", channel_width_);
      bad_status();
      return false;
    }
    if (channel_width_ / channels_[0].size() < 8) {
      logprintf(6, "Process Error: Chip width x%d must be x8 or greater.\n",
          channel_width_ / channels_[0].size());
      bad_status();
      return false;
    }
  }


  // Print each argument.
  for (int i = 0; i < argc; i++) {
    if (i)
      cmdline_ += " ";
    cmdline_ += argv[i];
  }

  return true;
}

void Sat::PrintHelp() {
  printf("Usage: ./sat(32|64) [options]\n"
         " -M mbytes        megabytes of ram to test\n"
         " --reserve_memory If not using hugepages, the amount of memory to "
         " reserve for the system\n"
         " -H mbytes        minimum megabytes of hugepages to require\n"
         " -s seconds       number of seconds to run\n"
         " -P list          cycle IDs or names across initial-fill blocks\n"
         "                  address order follows queue selection\n"
         " --pattern-byte-offset bytes  shift pattern start (multiple of 4)\n"
         " --fill-preset mode  prefill with none, zero, or one\n"
         " --prefault-pages  touch each OS page before parallel fill\n"
         " --diag-vm-stats  log VM state at execution phase boundaries\n"
         " --diag-block-history  log the last completed writer on mismatch\n"
         " --diag-phase-summary  log resolved config and phase work counts\n"
         " --fill-threads n initial fill workers, 1-256 (default 8)\n"
         " --fill-direction up|down  Fill address traversal (default up)\n"
         " --fill-verify-every n  verify every nth block immediately\n"
         " --fill-yield-bytes bytes  yield at this Fill byte interval\n"
         " --verify-after-fill  check every page before runtime workers\n"
         " --post-fill-delay secs  wait before post-fill check/runtime\n"
         " --runtime-start-delay secs  wait after queue setup\n"
         " --copy-verify-destination  read back each Copy destination\n"
         " --invert-range legacy|full  bytes touched by each Invert pass\n"
         " --final-check-threads n  use 1-256 separate final Valid checkers\n"
         " --skip-final-check  omit the remaining Valid-page check\n"
         " --ddr-freq list  hold one frequency or sweep 'all'/comma list\n"
         " --ddr-step secs  seconds per sweep frequency (default 3)\n"
         " --ddr-node path  target DDR control node\n"
         " --dram-map name  physical-to-DRAM map: none or lpddr-v1\n"
         " -m threads       number of memory copy threads to run\n"
         " -i threads       number of memory invert threads to run\n"
         " -c threads       number of memory check threads to run\n"
         " -C threads       number of memory CPU stress threads to run\n"
         " --findfiles      find locations to do disk IO automatically\n"
         " -d device        add a direct write disk thread with block "
         "device (or file) 'device'\n"
         " -f filename      add a disk thread with "
         "tempfile 'filename'\n"
         " -l logfile       log output to file 'logfile'\n"
         " --no_timestamps  do not prefix timestamps to log messages\n"
         " --max_errors n   exit when the total error count exceeds n\n"
         " -v level         verbosity (0-20), default is 8\n"
         " --printsec secs  positive interval for 'seconds remaining'\n"
         " -W               Use more CPU-stressful memory copy\n"
         " -A               run in degraded mode on incompatible systems\n"
         " -p pagesize      size in bytes of memory chunks\n"
         " --filesize size  size of disk IO tempfiles\n"
         " -n ipaddr        add a network thread connecting to "
         "system at 'ipaddr'\n"
         " --listen         run a thread to listen for and respond "
         "to network threads.\n"
         " --no_errors      run without checking for ECC or other errors\n"
         " --force_errors   inject false errors to test error handling\n"
         " --force_errors_like_crazy   inject a lot of false errors "
         "to test error handling\n"
         " -F               without -W, use memcpy; skip Invert pre/post "
         "checks\n"
         " --stop_on_errors  Request test termination after a detected "
         "miscompare.\n"
         " --error-log-limit n  limit detailed memory miscompare logs\n"
         " --read-block-size     size of block for reading (-d)\n"
         " --write-block-size    size of block for writing (-d). If not "
         "defined, the size of block for writing will be defined as the "
         "size of block for reading\n"
         " --segment-size   size of segments to split disk into (-d)\n"
         " --cache-size     size of disk cache (-d)\n"
         " --blocks-per-segment  number of blocks to read/write per "
         "segment per iteration (-d)\n"
         " --read-threshold      maximum time (in us) a block read should "
         "take (-d)\n"
         " --write-threshold     maximum time (in us) a block write "
         "should take (-d)\n"
         " --random-threads      number of random threads for each disk "
         "write thread (-d)\n"
         " --destructive    write/wipe disk partition (-d)\n"
         " --monitor_mode   only do ECC error polling, no stress load.\n"
         " --cc_test        do the cache coherency testing\n"
         " --cc_inc_count   number of times to increment the "
         "cacheline's member\n"
         " --cc_line_count  number of cache line sized datastructures "
         "to allocate for the cache coherency threads to operate\n"
         " --cc_line_size   override the auto-detected cache line size\n"
         " --cpu_freq_test  enable the cpu frequency test (requires the "
         "--cpu_freq_threshold argument to be set)\n"
         " --cpu_freq_threshold  fail the cpu frequency test if the frequency "
         "goes below this value (specified in MHz)\n"
         " --cpu_freq_round round the computed frequency to this value, if set"
         " to zero, only round to the nearest MHz\n"
         " --paddr_base     allocate memory starting from this address\n"
         " --pause_delay    positive delay (in seconds) between power spikes\n"
         " --pause_duration duration (in seconds) of each pause\n"
         " --no_affinity    do not set any cpu affinity\n"
         " --local_numa     choose memory regions associated with "
         "each CPU to be tested by that CPU\n"
         " --remote_numa    choose memory regions not associated with "
         "each CPU to be tested by that CPU\n"
         " --channel_hash   mask of address bits XORed to determine channel. "
         "Mask 0x40 interleaves cachelines between channels\n"
         " --channel_width bits     width in bits of each memory channel\n"
         " --memory_channel u1,u2   defines a comma-separated list of names "
         "for dram packages in a memory channel. Use multiple times to "
         "define multiple channels.\n");
}

bool Sat::CheckGoogleSpecificArgs(int argc, char **argv, int *i) {
  // Do nothing, no google-specific argument on public stressapptest
  return false;
}

void Sat::GoogleOsOptions(std::map<std::string, std::string> *options) {
  // Do nothing, no OS-specific argument on public stressapptest
}

// Launch the SAT task threads. Returns 0 on error.
void Sat::InitializeThreads() {
  // Memory copy threads.
  AcquireWorkerLock();

  logprintf(12, "Log: Starting worker threads\n");
  WorkerVector *memory_vector = new WorkerVector();

  // Error polling thread.
  // This may detect ECC corrected errors, disk problems, or
  // any other errors normally hidden from userspace.
  WorkerVector *error_vector = new WorkerVector();
  if (error_poll_) {
    ErrorPollThread *thread = new ErrorPollThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);

    error_vector->insert(error_vector->end(), thread);
  } else {
    logprintf(5, "Log: Skipping error poll thread due to --no_errors flag\n");
  }
  workers_map_.insert(make_pair(kErrorType, error_vector));

  // Only start error poll threads for monitor-mode SAT,
  // skip all other types of worker threads.
  if (monitor_mode_) {
    ReleaseWorkerLock();
    return;
  }

  for (int i = 0; i < memory_threads_; i++) {
    CopyThread *thread = new CopyThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_);

    if ((region_count_ > 1) && (region_mode_)) {
      int32 region = region_find(i % region_count_);
      cpu_set_t *cpuset = os_->FindCoreMask(region);
      sat_assert(cpuset);
      if (region_mode_ == kLocalNuma) {
        // Choose regions associated with this CPU.
        thread->set_cpu_mask(cpuset);
        thread->set_tag(1 << region);
      } else if (region_mode_ == kRemoteNuma) {
        // Choose regions not associated with this CPU..
        thread->set_cpu_mask(cpuset);
        thread->set_tag(region_mask_ & ~(1 << region));
      }
    } else {
      cpu_set_t available_cpus;
      thread->AvailableCpus(&available_cpus);
      int cores = cpuset_count(&available_cpus);
      // Don't restrict thread location if we have more than one
      // thread per core. Not so good for performance.
      if (cpu_stress_threads_ + memory_threads_ <= cores) {
        // Place a thread on alternating cores first.
        // This assures interleaved core use with no overlap.
        int nthcore = i;
        int nthbit = (((2 * nthcore) % cores) +
                      (((2 * nthcore) / cores) % 2)) % cores;
        cpu_set_t all_cores;
        cpuset_set_ab(&all_cores, 0, cores);
        if (!cpuset_isequal(&available_cpus, &all_cores)) {
          // We are assuming the bits are contiguous.
          // Complain if this is not so.
          logprintf(0, "Log: cores = %s, expected %s\n",
                    cpuset_format(&available_cpus).c_str(),
                    cpuset_format(&all_cores).c_str());
        }

        // Set thread affinity.
        thread->set_cpu_mask_to_cpu(nthbit);
      }
    }
    memory_vector->insert(memory_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kMemoryType, memory_vector));

  // File IO threads.
  WorkerVector *fileio_vector = new WorkerVector();
  for (int i = 0; i < file_threads_; i++) {
    FileThread *thread = new FileThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_);
    thread->SetFile(filename_[i].c_str());
    // Set disk threads high priority. They don't take much processor time,
    // but blocking them will delay disk IO.
    thread->SetPriority(WorkerThread::High);

    fileio_vector->insert(fileio_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kFileIOType, fileio_vector));

  // Net IO threads.
  WorkerVector *netio_vector = new WorkerVector();
  WorkerVector *netslave_vector = new WorkerVector();
  if (listen_threads_ > 0) {
    // Create a network slave thread. This listens for connections.
    NetworkListenThread *thread = new NetworkListenThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);

    netslave_vector->insert(netslave_vector->end(), thread);
  }
  for (int i = 0; i < net_threads_; i++) {
    NetworkThread *thread = new NetworkThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);
    thread->SetIP(ipaddrs_[i].c_str());

    netio_vector->insert(netio_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kNetIOType, netio_vector));
  workers_map_.insert(make_pair(kNetSlaveType, netslave_vector));

  // Result check threads.
  WorkerVector *check_vector = new WorkerVector();
  for (int i = 0; i < check_threads_; i++) {
    CheckThread *thread = new CheckThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);

    check_vector->insert(check_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kCheckType, check_vector));

  // Memory invert threads.
  logprintf(12, "Log: Starting invert threads\n");
  WorkerVector *invert_vector = new WorkerVector();
  for (int i = 0; i < invert_threads_; i++) {
    InvertThread *thread = new InvertThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);

    invert_vector->insert(invert_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kInvertType, invert_vector));

  // Disk stress threads.
  WorkerVector *disk_vector = new WorkerVector();
  WorkerVector *random_vector = new WorkerVector();
  logprintf(12, "Log: Starting disk stress threads\n");
  for (int i = 0; i < disk_threads_; i++) {
    // Creating write threads
    DiskThread *thread = new DiskThread(blocktables_[i]);
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &power_spike_status_);
    thread->SetDevice(diskfilename_[i].c_str());
    if (thread->SetParameters(read_block_size_, write_block_size_,
                              segment_size_, cache_size_,
                              blocks_per_segment_,
                              read_threshold_, write_threshold_,
                              non_destructive_)) {
      disk_vector->insert(disk_vector->end(), thread);
    } else {
      logprintf(12, "Log: DiskThread::SetParameters() failed\n");
      delete thread;
    }

    for (int j = 0; j < random_threads_; j++) {
      // Creating random threads
      RandomDiskThread *rthread = new RandomDiskThread(blocktables_[i]);
      rthread->InitThread(total_threads_++, this, os_, patternlist_,
                          &power_spike_status_);
      rthread->SetDevice(diskfilename_[i].c_str());
      if (rthread->SetParameters(read_block_size_, write_block_size_,
                                 segment_size_, cache_size_,
                                 blocks_per_segment_,
                                 read_threshold_, write_threshold_,
                                 non_destructive_)) {
        random_vector->insert(random_vector->end(), rthread);
      } else {
      logprintf(12, "Log: RandomDiskThread::SetParameters() failed\n");
        delete rthread;
      }
    }
  }

  workers_map_.insert(make_pair(kDiskType, disk_vector));
  workers_map_.insert(make_pair(kRandomDiskType, random_vector));

  // CPU stress threads.
  WorkerVector *cpu_vector = new WorkerVector();
  logprintf(12, "Log: Starting cpu stress threads\n");
  for (int i = 0; i < cpu_stress_threads_; i++) {
    CpuStressThread *thread = new CpuStressThread();
    thread->InitThread(total_threads_++, this, os_, patternlist_,
                       &continuous_status_);

    // Don't restrict thread location if we have more than one
    // thread per core. Not so good for performance.
    cpu_set_t available_cpus;
    thread->AvailableCpus(&available_cpus);
    int cores = cpuset_count(&available_cpus);
    if (cpu_stress_threads_ + memory_threads_ <= cores) {
      // Place a thread on alternating cores first.
      // Go in reverse order for CPU stress threads. This assures interleaved
      // core use with no overlap.
      int nthcore = (cores - 1) - i;
      int nthbit = (((2 * nthcore) % cores) +
                    (((2 * nthcore) / cores) % 2)) % cores;
      cpu_set_t all_cores;
      cpuset_set_ab(&all_cores, 0, cores);
      if (!cpuset_isequal(&available_cpus, &all_cores)) {
        logprintf(0, "Log: cores = %s, expected %s\n",
                  cpuset_format(&available_cpus).c_str(),
                  cpuset_format(&all_cores).c_str());
      }

      // Set thread affinity.
      thread->set_cpu_mask_to_cpu(nthbit);
    }

    cpu_vector->insert(cpu_vector->end(), thread);
  }
  workers_map_.insert(make_pair(kCPUType, cpu_vector));

  // CPU Cache Coherency Threads - one for each core available.
  if (cc_test_) {
    WorkerVector *cc_vector = new WorkerVector();
    logprintf(12, "Log: Starting cpu cache coherency threads\n");

    // Allocate the shared datastructure to be worked on by the threads.
    cc_cacheline_data_ = reinterpret_cast<cc_cacheline_data*>(
        malloc(sizeof(cc_cacheline_data) * cc_cacheline_count_));
    sat_assert(cc_cacheline_data_ != NULL);

    // Initialize the strucutre.
    memset(cc_cacheline_data_, 0,
           sizeof(cc_cacheline_data) * cc_cacheline_count_);

    int num_cpus = CpuCount();
    char *num;
    // Calculate the number of cache lines needed just to give each core
    // its own counter.
    int line_size = cc_cacheline_size_;
    if (line_size <= 0) {
      line_size = CacheLineSize();
      if (line_size < kCacheLineSize)
        line_size = kCacheLineSize;
      logprintf(12, "Log: Using %d as cache line size\n", line_size);
    }
    // The number of cache lines needed to hold an array of num_cpus.
    // "num" must be the same type as cc_cacheline_data[X].num or the memory
    // size calculations will fail.
    int needed_lines = (sizeof(*num) * num_cpus + line_size - 1) / line_size;
    // Allocate all the nums once so that we get a single chunk
    // of contiguous memory.
#ifdef HAVE_POSIX_MEMALIGN
    int err_result = posix_memalign(
        reinterpret_cast<void**>(&num),
        line_size, line_size * needed_lines * cc_cacheline_count_);
#else
    num = reinterpret_cast<int*>(memalign(
        line_size, line_size * needed_lines * cc_cacheline_count_));
    int err_result = (num == 0);
#endif
    sat_assert(err_result == 0);

    int cline;
    for (cline = 0; cline < cc_cacheline_count_; cline++) {
      memset(num, 0, sizeof(*num) * num_cpus);
      cc_cacheline_data_[cline].num = num;
      num += (line_size * needed_lines) / sizeof(*num);
    }

    int tnum;
    for (tnum = 0; tnum < num_cpus; tnum++) {
      CpuCacheCoherencyThread *thread =
          new CpuCacheCoherencyThread(cc_cacheline_data_, cc_cacheline_count_,
                                      tnum, num_cpus, cc_inc_count_);
      thread->InitThread(total_threads_++, this, os_, patternlist_,
                         &continuous_status_);
      // Pin the thread to a particular core.
      thread->set_cpu_mask_to_cpu(tnum);

      // Insert the thread into the vector.
      cc_vector->insert(cc_vector->end(), thread);
    }
    workers_map_.insert(make_pair(kCCType, cc_vector));
  }

  if (cpu_freq_test_) {
    // Create the frequency test thread.
    logprintf(5, "Log: Running cpu frequency test: threshold set to %dMHz.\n",
              cpu_freq_threshold_);
    CpuFreqThread *thread = new CpuFreqThread(CpuCount(), cpu_freq_threshold_,
                                              cpu_freq_round_);
    // This thread should be paused when other threads are paused.
    thread->InitThread(total_threads_++, this, os_, NULL,
                       &power_spike_status_);

    WorkerVector *cpu_freq_vector = new WorkerVector();
    cpu_freq_vector->insert(cpu_freq_vector->end(), thread);
    workers_map_.insert(make_pair(kCPUFreqType, cpu_freq_vector));
  }

  ReleaseWorkerLock();
}

// Return the number of cpus actually present in the machine.
int Sat::CpuCount() {
  return sysconf(_SC_NPROCESSORS_CONF);
}

int Sat::ReadInt(const char *filename, int *value) {
  char line[64];
  int fd = open(filename, O_RDONLY), err = -1;

  if (fd < 0)
    return -1;
  if (read(fd, line, sizeof(line)) > 0) {
    *value = atoi(line);
    err = 0;
  }

  close(fd);
  return err;
}

// 설정된 kernel interface에 고정 DDR 주파수 요청을 전달합니다. Write가
// 성공하면 마지막 성공 요청값으로 저장합니다. 실제 적용 주파수는 대상
// 시스템의 계측값으로 확인합니다.
bool Sat::ApplyDramFrequency(int frequency) {
  char message[128];
  int message_length = snprintf(message, sizeof(message),
                                "{class:ddr, res:fixed, val:%d}\n",
                                frequency);
  if (message_length <= 0 ||
      static_cast<size_t>(message_length) >= sizeof(message)) {
    logprintf(0, "Process Error: Failed to format DDR frequency %d\n",
              frequency);
    return false;
  }

  int open_flags = O_WRONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  int fd = open(dram_frequency_node_.c_str(), open_flags);
  if (fd < 0) {
    logprintf(0,
              "Process Error: DDR_FREQ open failed: value=%d node=%s "
              "errno=%d (%s)\n",
              frequency, dram_frequency_node_.c_str(), errno,
              strerror(errno));
    return false;
  }

  ssize_t written;
  do {
    written = write(fd, message, message_length);
  } while (written < 0 && errno == EINTR);
  int write_errno = errno;
  int close_result = close(fd);
  if (written != message_length) {
    logprintf(0,
              "Process Error: DDR_FREQ write failed: value=%d node=%s "
              "written=%lld expected=%d errno=%d (%s)\n",
              frequency, dram_frequency_node_.c_str(),
              static_cast<int64>(written), message_length, write_errno,
              strerror(write_errno));
    return false;
  }
  if (close_result != 0) {
    logprintf(0,
              "Process Error: DDR_FREQ close failed: value=%d node=%s "
              "errno=%d (%s)\n",
              frequency, dram_frequency_node_.c_str(), errno,
              strerror(errno));
    return false;
  }

  current_dram_frequency_.store(frequency, std::memory_order_release);
  uint64 epoch = dram_frequency_epoch_.fetch_add(
      1, std::memory_order_acq_rel) + 1;
  logprintf(5,
            "Log: DDR_FREQ write=%d epoch=%llu monotonic_us=%lld node=%s\n",
            frequency, epoch, sat_get_time_us(),
            dram_frequency_node_.c_str());
  return true;
}

// 한 SAT 작업 단위에서 추적하는 write 작업이 완료된 뒤 provenance를
// 갱신합니다. Invert legacy는 선택 범위 완료를 기록합니다.
// Queue가 같은 작업 단위를 한 Worker에만 전달하므로 별도 hot-path lock을
// 사용하지 않습니다. 옵션 비활성 시에는 배열 접근과 시간 확인을 생략합니다.
void Sat::RecordBlockWrite(uint64 page_offset,
                           BlockWriter writer,
                           int writer_thread,
                           int writer_cpu,
                           uint64 frequency_epoch_begin,
                           uint64 frequency_epoch_end) {
  if (!diag_block_history_ || !block_history_ || page_length_ <= 0)
    return;
  uint64 block = page_offset / page_length_;
  if (block >= static_cast<uint64>(pages_))
    return;

  BlockHistory *history = &block_history_[block];
  history->generation++;
  history->write_complete_us = sat_get_time_us();
  history->frequency_epoch_begin = frequency_epoch_begin;
  history->frequency_epoch_end = frequency_epoch_end;
  history->writer_thread = writer_thread;
  history->writer_cpu = writer_cpu;
  history->writer = writer;
}

// 오류가 발생한 SAT 작업 단위의 마지막 추적 write 정보를 구성합니다.
bool Sat::FormatBlockHistory(uint64 page_offset,
                             char *buffer,
                             size_t buffer_size) const {
  if (!diag_block_history_ || !block_history_ || !buffer ||
      buffer_size == 0 || page_length_ <= 0) {
    return false;
  }
  uint64 block = page_offset / page_length_;
  if (block >= static_cast<uint64>(pages_))
    return false;

  const BlockHistory *history = &block_history_[block];
  const char *writer = "unknown";
  switch (history->writer) {
    case BLOCK_WRITER_PRESET: writer = "preset"; break;
    case BLOCK_WRITER_INITIAL_FILL: writer = "initial_fill"; break;
    case BLOCK_WRITER_COPY: writer = "copy"; break;
    case BLOCK_WRITER_INVERT: writer = "invert"; break;
    case BLOCK_WRITER_REPAIR: writer = "repair"; break;
    case BLOCK_WRITER_FILE: writer = "file"; break;
    case BLOCK_WRITER_NETWORK: writer = "network"; break;
    case BLOCK_WRITER_UNKNOWN: break;
  }

  int64 age_us = history->write_complete_us >= 0
      ? sat_get_time_us() - history->write_complete_us
      : -1;
  const char *frequency_span =
      history->frequency_epoch_begin == history->frequency_epoch_end
          ? "single" : "mixed";
  snprintf(buffer, buffer_size,
           "last_writer:%s,generation:%llu,writer_thread:%d,writer_cpu:%d,"
           "write_age_us:%lld,freq_epoch_begin:%llu,freq_epoch_end:%llu,"
           "freq_span:%s",
           writer, history->generation, history->writer_thread,
           history->writer_cpu, age_us,
           history->frequency_epoch_begin, history->frequency_epoch_end,
           frequency_span);
  return true;
}

// Worker 종료 후 local 통계를 합산합니다. Worker당 한 번만 호출되므로
// 작업 단위 hot loop에서는 전역 lock과 atomic counter를 사용하지 않습니다.
void Sat::MergeDiagnosticPhaseStats(const DiagnosticPhaseStats *stats,
                                    int count) {
  if (!diag_phase_summary_ || !stats)
    return;
  sat_assert(0 == pthread_mutex_lock(&diagnostic_stats_lock_));
  const int limit = count < DIAG_PHASE_COUNT ? count : DIAG_PHASE_COUNT;
  for (int i = 0; i < limit; ++i) {
    DiagnosticPhaseStats *total = &diagnostic_phase_stats_[i];
    total->blocks += stats[i].blocks;
    total->read_bytes += stats[i].read_bytes;
    total->write_bytes += stats[i].write_bytes;
    total->checksum_mismatch_regions += stats[i].checksum_mismatch_regions;
    total->word_mismatches += stats[i].word_mismatches;
    if (stats[i].first_error_us >= 0 &&
        (total->first_error_us < 0 ||
         stats[i].first_error_us < total->first_error_us)) {
      total->first_error_us = stats[i].first_error_us;
      total->first_error_epoch = stats[i].first_error_epoch;
      total->first_error_worker_bytes =
          stats[i].first_error_worker_bytes;
    }
  }
  sat_assert(0 == pthread_mutex_unlock(&diagnostic_stats_lock_));
}

// 단계별 처리 block과 논리 read/write 양을 한 번씩 출력합니다.
// first_error_worker_bytes는 가장 먼저 오류를 보고한 Worker가 같은 phase에서
// 오류 작업 단위를 시작하기 전에 완료한 논리 byte 수입니다.
void Sat::PrintDiagnosticPhaseSummary() {
  if (!diag_phase_summary_)
    return;
  for (int i = 0; i < DIAG_PHASE_COUNT; ++i) {
    const DiagnosticPhaseStats *stats = &diagnostic_phase_stats_[i];
    if (stats->blocks == 0 && stats->checksum_mismatch_regions == 0 &&
        stats->word_mismatches == 0) {
      continue;
    }
    logprintf(5,
              "Log: DIAG_SUMMARY phase=%s blocks=%llu read_bytes=%llu "
              "write_bytes=%llu checksum_mismatch_regions=%llu "
              "word_mismatches=%llu first_error_us=%lld "
              "first_error_epoch=%llu first_error_worker_bytes=%llu\n",
              DiagnosticPhaseName(static_cast<DiagnosticPhase>(i)),
              stats->blocks, stats->read_bytes, stats->write_bytes,
              stats->checksum_mismatch_regions, stats->word_mismatches,
              stats->first_error_us, stats->first_error_epoch,
              stats->first_error_worker_bytes);
  }
}

// 실행 단계 경계의 VM 상태를 한 줄로 기록합니다. 이 함수는
// --diag-vm-stats가 없으면 syscall과 /proc 접근 없이 즉시 반환합니다.
void Sat::LogVmStats(const char *phase) {
  if (!diag_vm_stats_)
    return;

  struct rusage usage;
  const bool have_usage = getrusage(RUSAGE_SELF, &usage) == 0;
  int64 minor_faults = have_usage ? usage.ru_minflt : -1;
  int64 major_faults = have_usage ? usage.ru_majflt : -1;
  int64 minor_delta =
      have_usage && vm_stats_last_minor_faults_ >= 0
          ? minor_faults - vm_stats_last_minor_faults_
          : -1;
  int64 major_delta =
      have_usage && vm_stats_last_major_faults_ >= 0
          ? major_faults - vm_stats_last_major_faults_
          : -1;
  if (have_usage) {
    vm_stats_last_minor_faults_ = minor_faults;
    vm_stats_last_major_faults_ = major_faults;
  }

  int64 max_rss_kb = have_usage ? usage.ru_maxrss : -1;
#if defined(__APPLE__)
  // macOS getrusage()의 ru_maxrss 단위는 byte입니다.
  if (max_rss_kb >= 0)
    max_rss_kb /= 1024;
#endif

  const int64 vm_rss_kb =
      ReadProcKbTotal("/proc/self/status", "VmRSS:");
  int64 anon_huge_kb =
      ReadProcKbTotal("/proc/self/smaps_rollup", "AnonHugePages:");
  if (anon_huge_kb < 0)
    anon_huge_kb = ReadProcKbTotal("/proc/self/smaps", "AnonHugePages:");
  long os_page_size = sysconf(_SC_PAGESIZE);

  logprintf(5,
            "Log: DIAG_VM phase=%s monotonic_us=%lld backend=%s "
            "mapping=%s os_page_bytes=%ld minor_faults=%lld "
            "minor_delta=%lld major_faults=%lld major_delta=%lld "
            "vm_rss_kb=%lld max_rss_kb=%lld anon_huge_kb=%lld "
            "voluntary_cs=%lld involuntary_cs=%lld\n",
            phase, sat_get_time_us(),
            os_ ? os_->test_memory_backend() : "unknown",
            os_ && os_->dynamic_test_mapping() ? "dynamic" : "static",
            os_page_size,
            minor_faults, minor_delta, major_faults, major_delta,
            vm_rss_kb, max_rss_kb, anon_huge_kb,
            have_usage ? static_cast<int64>(usage.ru_nvcsw) : -1,
            have_usage ? static_cast<int64>(usage.ru_nivcsw) : -1);
}

// Return the worst case (largest) cache line size of the various levels of
// cache actually prsent in the machine.
int Sat::CacheLineSize() {
  int max_linesize, linesize;
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
  max_linesize = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", &max_linesize);
#endif
#ifdef _SC_LEVEL2_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL2_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index1/coherency_line_size", &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
#ifdef _SC_LEVEL3_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL3_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index2/coherency_line_size", &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
#ifdef _SC_LEVEL4_DCACHE_LINESIZE
  linesize = sysconf(_SC_LEVEL4_DCACHE_LINESIZE);
#else
  ReadInt("/sys/devices/system/cpu/cpu0/cache/index3/coherency_line_size", &linesize);
#endif
  if (linesize > max_linesize) max_linesize = linesize;
  return max_linesize;
}

// Notify and reap worker threads.
void Sat::JoinThreads() {
  logprintf(12, "Log: Joining worker threads\n");
  power_spike_status_.StopWorkers();
  continuous_status_.StopWorkers();

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      logprintf(12, "Log: Joining thread %d\n", (*it)->ThreadID());
      (*it)->JoinThread();
    }
  }
  ReleaseWorkerLock();

  QueueStats();
  LogVmStats("runtime");

  // Runtime 종료 시점에 Valid queue에 남은 작업 단위를 검사할 Worker를
  // 생성합니다. 옵션 미지정 실행은 기존 코드와 같이 Fill Worker 수를
  // 사용하고, --final-check-threads를 명시한 실행만 지정값을 사용합니다.
  logprintf(12, "Log: Finished countdown, begin to result check\n");
  WorkerStatus reap_check_status;
  WorkerVector reap_check_vector;
  const bool final_check_enabled =
      !monitor_mode_ && !skip_final_check_ && !error_stop_requested();

  // monitor mode, 명시적 생략 또는 오류 종료 요청에서는 종료 검사 Worker를
  // 생성하지 않습니다. 오류 종료 뒤의 추가 mismatch 로그를 제한합니다.
  if (final_check_enabled) {
    const int reap_check_threads =
        final_check_threads_explicit_ ? final_check_threads_ : fill_threads_;
    // Initialize the check threads.
    for (int i = 0; i < reap_check_threads; i++) {
      CheckThread *thread = new CheckThread();
      thread->InitThread(total_threads_++, this, os_, patternlist_,
                         &reap_check_status);
      thread->SetCheckPhase("final_check");
      logprintf(12, "Log: Finished countdown, begin to result check\n");
      reap_check_vector.push_back(thread);
    }
  } else if (skip_final_check_ || error_stop_requested()) {
    logprintf(5, "Log: DIAG phase=final_check_skipped reason=%s\n",
              error_stop_requested() ? "stop_on_errors" : "option");
  }

  reap_check_status.Initialize();
  // Check threads should be marked to stop ASAP.
  reap_check_status.StopWorkers();

  // 생성에 성공한 종료 검사 Worker만 join 대상에 남깁니다.
  WorkerVector spawned_reap_check_vector;
  for (WorkerVector::const_iterator it = reap_check_vector.begin();
       it != reap_check_vector.end(); ++it) {
    logprintf(12, "Log: Spawning thread %d\n", (*it)->ThreadID());
    if ((*it)->SpawnThread()) {
      spawned_reap_check_vector.push_back(*it);
    } else {
      (*it)->RemoveUnspawnedWorker();
      delete (*it);
      bad_status();
    }
  }
  reap_check_vector.swap(spawned_reap_check_vector);
  spawned_reap_check_vector.clear();

  // Join the check threads.
  for (WorkerVector::const_iterator it = reap_check_vector.begin();
       it != reap_check_vector.end(); ++it) {
    logprintf(12, "Log: Joining thread %d\n", (*it)->ThreadID());
    (*it)->JoinThread();
  }

  // Reap all children. Stopped threads should have already ended.
  // Result checking threads will end when they have finished
  // result checking.
  logprintf(12, "Log: Join all outstanding threads\n");

  // Find all errors.
  errorcount_ = GetTotalErrorCount();

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      logprintf(12, "Log: Reaping thread status %d\n", (*it)->ThreadID());
      if ((*it)->GetStatus() != 1) {
        logprintf(0, "Process Error: Thread %d failed with status %d at "
                  "%.2f seconds\n",
                  (*it)->ThreadID(), (*it)->GetStatus(),
                  (*it)->GetRunDurationUSec()*1.0/1000000);
        bad_status();
      }
      int priority = 12;
      if ((*it)->GetErrorCount())
        priority = 5;
      logprintf(priority, "Log: Thread %d found %lld hardware incidents\n",
                (*it)->ThreadID(), (*it)->GetErrorCount());
    }
  }
  ReleaseWorkerLock();


  // Add in any errors from check threads.
  for (WorkerVector::const_iterator it = reap_check_vector.begin();
       it != reap_check_vector.end(); ++it) {
    logprintf(12, "Log: Reaping thread status %d\n", (*it)->ThreadID());
    if ((*it)->GetStatus() != 1) {
      logprintf(0, "Process Error: Thread %d failed with status %d at "
                "%.2f seconds\n",
                (*it)->ThreadID(), (*it)->GetStatus(),
                (*it)->GetRunDurationUSec()*1.0/1000000);
      bad_status();
    }
    errorcount_ += (*it)->GetErrorCount();
    int priority = 12;
    if ((*it)->GetErrorCount())
      priority = 5;
    logprintf(priority, "Log: Thread %d found %lld hardware incidents\n",
              (*it)->ThreadID(), (*it)->GetErrorCount());
    delete (*it);
  }
  reap_check_vector.clear();
  reap_check_status.Destroy();
  LogVmStats(final_check_enabled ? "final_check" : "final_check_skipped");
}

// Print queuing information.
void Sat::QueueStats() {
  if (pe_q_implementation_ == SAT_FINELOCK && finelock_q_) {
    finelock_q_->QueueAnalysis();
    return;
  }

  // OneLock queue에는 FineLock 전용 touch/tries histogram이 없습니다.
  // 통계 부재를 정상 상태로 기록하고 null queue를 역참조하지 않습니다.
  logprintf(12,
            "Log: Queue histogram unavailable for coarse-grain queue\n");
}

void Sat::AnalysisAllStats() {
  float max_runtime_sec = 0.;
  float total_data = 0.;
  float total_bandwidth = 0.;
  float thread_runtime_sec = 0.;

  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      thread_runtime_sec = (*it)->GetRunDurationUSec()*1.0/1000000.;
      total_data += (*it)->GetMemoryCopiedData();
      total_data += (*it)->GetDeviceCopiedData();
      if (thread_runtime_sec > max_runtime_sec) {
        max_runtime_sec = thread_runtime_sec;
      }
    }
  }

  total_bandwidth = total_data / max_runtime_sec;

  logprintf(0, "Stats: Completed: %.2fM in %.2fs %.2fMB/s, "
            "with %d hardware incidents, %d errors\n",
            total_data,
            max_runtime_sec,
            total_bandwidth,
            errorcount_,
            statuscount_);
}

void Sat::MemoryStats() {
  float memcopy_data = 0.;
  float memcopy_bandwidth = 0.;
  WorkerMap::const_iterator mem_it = workers_map_.find(
      static_cast<int>(kMemoryType));
  WorkerMap::const_iterator file_it = workers_map_.find(
      static_cast<int>(kFileIOType));
  sat_assert(mem_it != workers_map_.end());
  sat_assert(file_it != workers_map_.end());
  for (WorkerVector::const_iterator it = mem_it->second->begin();
       it != mem_it->second->end(); ++it) {
    memcopy_data += (*it)->GetMemoryCopiedData();
    memcopy_bandwidth += (*it)->GetMemoryBandwidth();
  }
  for (WorkerVector::const_iterator it = file_it->second->begin();
       it != file_it->second->end(); ++it) {
    memcopy_data += (*it)->GetMemoryCopiedData();
    memcopy_bandwidth += (*it)->GetMemoryBandwidth();
  }
  GoogleMemoryStats(&memcopy_data, &memcopy_bandwidth);
  logprintf(4, "Stats: Memory Copy: %.2fM at %.2fMB/s\n",
            memcopy_data,
            memcopy_bandwidth);
}

void Sat::GoogleMemoryStats(float *memcopy_data,
                            float *memcopy_bandwidth) {
  // Do nothing, should be implemented by subclasses.
}

void Sat::FileStats() {
  float file_data = 0.;
  float file_bandwidth = 0.;
  WorkerMap::const_iterator file_it = workers_map_.find(
      static_cast<int>(kFileIOType));
  sat_assert(file_it != workers_map_.end());
  for (WorkerVector::const_iterator it = file_it->second->begin();
       it != file_it->second->end(); ++it) {
    file_data += (*it)->GetDeviceCopiedData();
    file_bandwidth += (*it)->GetDeviceBandwidth();
  }
  logprintf(4, "Stats: File Copy: %.2fM at %.2fMB/s\n",
            file_data,
            file_bandwidth);
}

void Sat::CheckStats() {
  float check_data = 0.;
  float check_bandwidth = 0.;
  WorkerMap::const_iterator check_it = workers_map_.find(
      static_cast<int>(kCheckType));
  sat_assert(check_it != workers_map_.end());
  for (WorkerVector::const_iterator it = check_it->second->begin();
       it != check_it->second->end(); ++it) {
    check_data += (*it)->GetMemoryCopiedData();
    check_bandwidth += (*it)->GetMemoryBandwidth();
  }
  logprintf(4, "Stats: Data Check: %.2fM at %.2fMB/s\n",
            check_data,
            check_bandwidth);
}

void Sat::NetStats() {
  float net_data = 0.;
  float net_bandwidth = 0.;
  WorkerMap::const_iterator netio_it = workers_map_.find(
      static_cast<int>(kNetIOType));
  WorkerMap::const_iterator netslave_it = workers_map_.find(
      static_cast<int>(kNetSlaveType));
  sat_assert(netio_it != workers_map_.end());
  sat_assert(netslave_it != workers_map_.end());
  for (WorkerVector::const_iterator it = netio_it->second->begin();
       it != netio_it->second->end(); ++it) {
    net_data += (*it)->GetDeviceCopiedData();
    net_bandwidth += (*it)->GetDeviceBandwidth();
  }
  for (WorkerVector::const_iterator it = netslave_it->second->begin();
       it != netslave_it->second->end(); ++it) {
    net_data += (*it)->GetDeviceCopiedData();
    net_bandwidth += (*it)->GetDeviceBandwidth();
  }
  logprintf(4, "Stats: Net Copy: %.2fM at %.2fMB/s\n",
            net_data,
            net_bandwidth);
}

void Sat::InvertStats() {
  float invert_data = 0.;
  float invert_bandwidth = 0.;
  WorkerMap::const_iterator invert_it = workers_map_.find(
      static_cast<int>(kInvertType));
  sat_assert(invert_it != workers_map_.end());
  for (WorkerVector::const_iterator it = invert_it->second->begin();
       it != invert_it->second->end(); ++it) {
    invert_data += (*it)->GetMemoryCopiedData();
    invert_bandwidth += (*it)->GetMemoryBandwidth();
  }
  logprintf(4, "Stats: Invert Data: %.2fM at %.2fMB/s\n",
            invert_data,
            invert_bandwidth);
}

void Sat::DiskStats() {
  float disk_data = 0.;
  float disk_bandwidth = 0.;
  WorkerMap::const_iterator disk_it = workers_map_.find(
      static_cast<int>(kDiskType));
  WorkerMap::const_iterator random_it = workers_map_.find(
      static_cast<int>(kRandomDiskType));
  sat_assert(disk_it != workers_map_.end());
  sat_assert(random_it != workers_map_.end());
  for (WorkerVector::const_iterator it = disk_it->second->begin();
       it != disk_it->second->end(); ++it) {
    disk_data += (*it)->GetDeviceCopiedData();
    disk_bandwidth += (*it)->GetDeviceBandwidth();
  }
  for (WorkerVector::const_iterator it = random_it->second->begin();
       it != random_it->second->end(); ++it) {
    disk_data += (*it)->GetDeviceCopiedData();
    disk_bandwidth += (*it)->GetDeviceBandwidth();
  }

  logprintf(4, "Stats: Disk: %.2fM at %.2fMB/s\n",
            disk_data,
            disk_bandwidth);
}

// Process worker thread data for bandwidth information, and error results.
// You can add more methods here just subclassing SAT.
void Sat::RunAnalysis() {
  AnalysisAllStats();
  MemoryStats();
  FileStats();
  NetStats();
  CheckStats();
  InvertStats();
  DiskStats();
}

// Get total error count, summing across all threads..
int64 Sat::GetTotalErrorCount() {
  int64 errors = initialization_errorcount_;

  AcquireWorkerLock();
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      errors += (*it)->GetErrorCount();
    }
  }
  ReleaseWorkerLock();
  return errors;
}


bool Sat::SpawnThreads() {
  bool result = true;
  logprintf(12, "Log: Initializing WorkerStatus objects\n");
  power_spike_status_.Initialize();
  continuous_status_.Initialize();
  logprintf(12, "Log: Spawning worker threads\n");
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      logprintf(12, "Log: Spawning thread %d\n", (*it)->ThreadID());
      if (!(*it)->SpawnThread()) {
        // 생성되지 않은 Worker가 pause barrier 수에 남지 않도록
        // InitThread()에서 등록한 수를 즉시 복원합니다.
        (*it)->RemoveUnspawnedWorker();
        bad_status();
        result = false;
      }
    }
  }
  return result;
}

// Delete used worker thread objects.
void Sat::DeleteThreads() {
  logprintf(12, "Log: Deleting worker threads\n");
  for (WorkerMap::const_iterator map_it = workers_map_.begin();
       map_it != workers_map_.end(); ++map_it) {
    for (WorkerVector::const_iterator it = map_it->second->begin();
         it != map_it->second->end(); ++it) {
      logprintf(12, "Log: Deleting thread %d\n", (*it)->ThreadID());
      delete (*it);
    }
    delete map_it->second;
  }
  workers_map_.clear();
  logprintf(12, "Log: Destroying WorkerStatus objects\n");
  power_spike_status_.Destroy();
  continuous_status_.Destroy();
}

namespace {
// Calculates the next time an action in Sat::Run() should occur, based on a
// schedule derived from a start point and a regular frequency.
//
// Using frequencies instead of intervals with their accompanying drift allows
// users to better predict when the actions will occur throughout a run.
//
// Arguments:
//   frequency: seconds
//   start: unixtime
//   now: unixtime
//
// Returns: unixtime
inline time_t NextOccurance(time_t frequency, time_t start, time_t now) {
  return start + frequency + (((now - start) / frequency) * frequency);
}
}

// Run the actual test.
bool Sat::Run() {
  // 초기 Fill 또는 post-fill 검사에서 첫 오류가 검출된 경우 Runtime Worker를
  // 생성하지 않습니다. 초기 단계에서 누적한 오류 수는 최종 결과에 반영합니다.
  if (error_stop_requested()) {
    errorcount_ = GetTotalErrorCount();
    logprintf(5,
              "Log: DIAG phase=runtime_skipped reason=stop_on_errors "
              "errors=%lld\n",
              errorcount_);
    logprintf(5,
              "Log: DIAG phase=final_check_skipped reason=stop_on_errors\n");
    LogVmStats("runtime_skipped");
    PrintDiagnosticPhaseSummary();
    return true;
  }

  size_t dram_frequency_index = 0;
  // Runtime 시작 직전에 첫 요청값을 다시 전달합니다. Sweep은 이 시점을
  // 기준으로 --ddr-step 간격마다 다음 목록 값으로 이동합니다.
  if (!dram_frequencies_.empty() &&
      !ApplyDramFrequency(dram_frequencies_[dram_frequency_index])) {
    bad_status();
    return false;
  }

  // Install signal handlers to gracefully exit in the middle of a run.
  //
  // Why go through this whole rigmarole?  It's the only standards-compliant
  // (C++ and POSIX) way to handle signals in a multithreaded program.
  // Specifically:
  //
  // 1) (C++) The value of a variable not of type "volatile sig_atomic_t" is
  //    unspecified upon entering a signal handler and, if modified by the
  //    handler, is unspecified after leaving the handler.
  //
  // 2) (POSIX) After the value of a variable is changed in one thread, another
  //    thread is only guaranteed to see the new value after both threads have
  //    acquired or released the same mutex or rwlock, synchronized to the
  //    same barrier, or similar.
  //
  // #1 prevents the use of #2 in a signal handler, so the signal handler must
  // be called in the same thread that reads the "volatile sig_atomic_t"
  // variable it sets.  We enforce that by blocking the signals in question in
  // the worker threads, forcing them to be handled by this thread.
  logprintf(12, "Log: Installing signal handlers\n");
  sigset_t new_blocked_signals;
  sigemptyset(&new_blocked_signals);
  sigaddset(&new_blocked_signals, SIGINT);
  sigaddset(&new_blocked_signals, SIGTERM);
  sigset_t prev_blocked_signals;
  pthread_sigmask(SIG_BLOCK, &new_blocked_signals, &prev_blocked_signals);
  sighandler_t prev_sigint_handler = signal(SIGINT, SatHandleBreak);
  sighandler_t prev_sigterm_handler = signal(SIGTERM, SatHandleBreak);

  // Kick off all the worker threads.
  logprintf(12, "Log: Launching worker threads\n");
  InitializeThreads();
  bool run_ok = SpawnThreads();
  pthread_sigmask(SIG_SETMASK, &prev_blocked_signals, NULL);

  logprintf(12, "Log: Starting countdown with %d seconds\n", runtime_seconds_);

  // In seconds.
  const bool dram_sweep_active =
      dram_sweep_ && dram_frequencies_.size() > 1;
  const time_t sleep_frequency = dram_sweep_active ? 1 : 5;
  // All of these are in seconds.  You probably want them to be >=
  // kSleepFrequency and multiples of kSleepFrequency, but neither is necessary.
  static const time_t kInjectionFrequency = 10;
  // print_delay_ determines "seconds remaining" chatty update.

  const time_t start = time(NULL);
  const time_t end = start + runtime_seconds_;
  time_t now = start;
  time_t next_print = start + print_delay_;
  time_t next_pause = start + pause_delay_;
  time_t next_resume = 0;
  time_t next_dram_frequency =
      dram_sweep_active ? start + dram_step_seconds_ : 0;
  time_t next_injection;
  if (crazy_error_injection_) {
    next_injection = start + kInjectionFrequency;
  } else {
    next_injection = 0;
  }

  // 생성 실패 시 이미 생성된 Worker를 즉시 정지하고 회수합니다.
  while (run_ok && now < end) {
    // This is an int because it's for logprintf().
    const int seconds_remaining = end - now;

    if (user_break_ || error_stop_requested()) {
      // Handle early exit.
      logprintf(0, "Log: Exiting early (%d seconds remaining), reason=%s\n",
                seconds_remaining,
                error_stop_requested() ? "stop_on_errors" : "signal");
      break;
    }

    // If we have an error limit, check it here and see if we should exit.
    if (max_errorcount_ != 0) {
      uint64 errors = GetTotalErrorCount();
      if (errors > max_errorcount_) {
        logprintf(0, "Log: Exiting early (%d seconds remaining) "
                     "due to excessive failures (%lld)\n",
                  seconds_remaining,
                  errors);
        break;
      }
    }

    if (now >= next_print) {
      // Print a count down message.
      logprintf(5, "Log: Seconds remaining: %d\n", seconds_remaining);
      next_print = NextOccurance(print_delay_, start, now);
    }

    if (next_injection && now >= next_injection) {
      // Inject an error.
      logprintf(4, "Log: Injecting error (%d seconds remaining)\n",
                seconds_remaining);
      struct page_entry src;
      GetValid(&src);
      src.pattern = patternlist_->GetPattern(0);
      PutValid(&src);
      next_injection = NextOccurance(kInjectionFrequency, start, now);
    }

    if (next_dram_frequency && now >= next_dram_frequency) {
      // 두 번째 값부터 목록 끝까지 이동한 후 첫 값으로 돌아갑니다.
      dram_frequency_index =
          (dram_frequency_index + 1) % dram_frequencies_.size();
      if (!ApplyDramFrequency(dram_frequencies_[dram_frequency_index])) {
        bad_status();
        run_ok = false;
        break;
      }
      next_dram_frequency =
          NextOccurance(dram_step_seconds_, start, now);
    }

    if (next_pause && now >= next_pause) {
      // Tell worker threads to pause in preparation for a power spike.
      logprintf(4, "Log: Pausing worker threads in preparation for power spike "
                "(%d seconds remaining)\n", seconds_remaining);
      power_spike_status_.PauseWorkers();
      logprintf(12, "Log: Worker threads paused\n");
      next_pause = 0;
      next_resume = now + pause_duration_;
    }

    if (next_resume && now >= next_resume) {
      // Tell worker threads to resume in order to cause a power spike.
      logprintf(4, "Log: Resuming worker threads to cause a power spike (%d "
                "seconds remaining)\n", seconds_remaining);
      power_spike_status_.ResumeWorkers();
      logprintf(12, "Log: Worker threads resumed\n");
      next_pause = NextOccurance(pause_delay_, start, now);
      next_resume = 0;
    }

    sat_sleep(NextOccurance(sleep_frequency, start, now) - now);
    now = time(NULL);
  }

  JoinThreads();
  PrintDiagnosticPhaseSummary();

  logprintf(0, "Stats: Found %lld hardware incidents\n", errorcount_);

  if (!monitor_mode_)
    RunAnalysis();

  DeleteThreads();

  if (current_dram_frequency() >= 0) {
    logprintf(5,
              "Log: DDR_FREQ retained=%d after test completion\n",
              current_dram_frequency());
  }

  logprintf(12, "Log: Uninstalling signal handlers\n");
  signal(SIGINT, prev_sigint_handler);
  signal(SIGTERM, prev_sigterm_handler);

  return run_ok;
}

// Clean up all resources.
bool Sat::Cleanup() {
  g_sat = NULL;
  Logger::GlobalLogger()->StopThread();
  Logger::GlobalLogger()->SetStdoutOnly();
  if (logfile_) {
    close(logfile_);
    logfile_ = 0;
  }
  if (patternlist_) {
    patternlist_->Destroy();
    delete patternlist_;
    patternlist_ = 0;
  }
  if (os_) {
    os_->FreeTestMem();
    delete os_;
    os_ = 0;
  }
  if (empty_) {
    delete empty_;
    empty_ = 0;
  }
  if (valid_) {
    delete valid_;
    valid_ = 0;
  }
  if (finelock_q_) {
    delete finelock_q_;
    finelock_q_ = 0;
  }
  if (block_history_) {
    delete[] block_history_;
    block_history_ = NULL;
  }
  if (page_bitmap_) {
    delete[] page_bitmap_;
  }

  for (size_t i = 0; i < blocktables_.size(); i++) {
    delete blocktables_[i];
  }

  if (cc_cacheline_data_) {
    // The num integer arrays for all the cacheline structures are
    // allocated as a single chunk. The pointers in the cacheline struct
    // are populated accordingly. Hence calling free on the first
    // cacheline's num's address is going to free the entire array.
    // TODO(aganti): Refactor this to have a class for the cacheline
    // structure (currently defined in worker.h) and clean this up
    // in the destructor of that class.
    if (cc_cacheline_data_[0].num) {
      free(cc_cacheline_data_[0].num);
    }
    free(cc_cacheline_data_);
  }

  sat_assert(0 == pthread_mutex_destroy(&worker_lock_));
  sat_assert(0 == pthread_mutex_destroy(&diagnostic_stats_lock_));

  return true;
}


// Pretty print really obvious results.
bool Sat::PrintResults() {
  bool result = true;

  if (error_log_limit_ >= 0) {
    logprintf(5,
              "Log: DIAG_ERROR_LOG limit=%lld detailed=%llu "
              "suppressed=%llu\n",
              error_log_limit_,
              error_log_detailed_.load(std::memory_order_acquire),
              error_log_suppressed_.load(std::memory_order_acquire));
  }

  logprintf(4, "\n");
  if (statuscount_) {
    logprintf(4, "Status: FAIL - test encountered procedural errors\n");
    result = false;
  } else if (errorcount_) {
    logprintf(4, "Status: FAIL - test discovered HW problems\n");
    result = false;
  } else {
    logprintf(4, "Status: PASS - please verify no corrected errors\n");
  }
  logprintf(4, "\n");

  return result;
}

// Helper functions.
void Sat::AcquireWorkerLock() {
  sat_assert(0 == pthread_mutex_lock(&worker_lock_));
}
void Sat::ReleaseWorkerLock() {
  sat_assert(0 == pthread_mutex_unlock(&worker_lock_));
}

void logprintf(int priority, const char *format, ...) {
  va_list args;
  va_start(args, format);
  Logger::GlobalLogger()->VLogF(priority, format, args);
  va_end(args);
}

// Stop the logging thread and verify any pending data is written to the log.
void logstop() {
  Logger::GlobalLogger()->StopThread();
}
