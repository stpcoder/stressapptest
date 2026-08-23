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

// sat.h : sat stress test object interface and data structures

#ifndef STRESSAPPTEST_SAT_H_
#define STRESSAPPTEST_SAT_H_

#include <signal.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "finelock_queue.h"
#include "dram_address.h"
#include "queue.h"
#include "sattypes.h"
#include "worker.h"
#include "os.h"

// SAT stress test class.
class Sat {
 public:
  // Enum for page queue implementation switch.
  enum PageQueueType { SAT_ONELOCK, SAT_FINELOCK };
  enum FillPreset { FILL_PRESET_NONE, FILL_PRESET_ZERO, FILL_PRESET_ONE };
  enum FillDirection { FILL_DIRECTION_UP, FILL_DIRECTION_DOWN };
  // Legacy는 upstream의 pointer 단위 계산을 보존하고, Full은 SAT 작업
  // 단위 전체를 각 Invert pass에서 처리합니다.
  enum InvertRange { INVERT_RANGE_LEGACY, INVERT_RANGE_FULL };
  enum BlockWriter {
    BLOCK_WRITER_UNKNOWN = 0,
    BLOCK_WRITER_PRESET,
    BLOCK_WRITER_INITIAL_FILL,
    BLOCK_WRITER_COPY,
    BLOCK_WRITER_INVERT,
    BLOCK_WRITER_REPAIR,
    BLOCK_WRITER_FILE,
    BLOCK_WRITER_NETWORK
  };

  Sat();
  virtual ~Sat();

  // Read configuration from arguments. Called first.
  bool ParseArgs(int argc, char **argv);
  virtual bool CheckGoogleSpecificArgs(int argc, char **argv, int *i);
  // Initialize data structures, subclasses, and resources,
  // based on command line args.
  // Called after ParseArgs().
  bool Initialize();

  // Execute the test. Initialize() and ParseArgs() must be called first.
  // This must be called from a single-threaded program.
  bool Run();

  // Pretty print result summary.
  // Called after Run().
  // Return value is success or failure of the SAT run, *not* of this function!
  bool PrintResults();

  // Pretty print version info.
  bool PrintVersion();

  // Pretty print help.
  virtual void PrintHelp();

  // Clean up allocations and resources.
  // Called last.
  bool Cleanup();

  // Abort Run().  Only for use by Run()-installed signal handlers.
  void Break() { user_break_ = true; }

  // Fetch and return empty and full pages into the empty and full pools.
  bool GetValid(struct page_entry *pe);
  bool PutValid(struct page_entry *pe);
  bool GetEmpty(struct page_entry *pe);
  bool PutEmpty(struct page_entry *pe);
  // Runtime 시작 전 post-fill 검사에서 queue 상태를 변경하지 않고
  // 지정 offset의 Valid metadata와 mapping을 준비합니다.
  bool GetValidByOffsetForInitialization(uint64 offset,
                                         struct page_entry *pe);
  void ReleaseInitializationPage(struct page_entry *pe);
  bool coarse_grain_queue() const {
    return pe_q_implementation_ == SAT_ONELOCK;
  }
  // OneLock post-fill 검사의 처리 완료 entry를 임시 queue로 옮겼다가
  // 검사 종료 후 Valid queue로 복원합니다.
  bool HoldInitializationPage(struct page_entry *pe);
  bool RestoreInitializationPages(int64 count);

  bool GetValid(struct page_entry *pe, int32 tag);
  bool GetEmpty(struct page_entry *pe, int32 tag);

  // Accessor functions.
  int verbosity() const { return verbosity_; }
  int logfile() const { return logfile_; }
  int page_length() const { return page_length_; }
  int disk_pages() const { return disk_pages_; }
  int strict() const { return strict_; }
  int tag_mode() const { return tag_mode_; }
  int status() const { return statuscount_; }
  void bad_status() { statuscount_++; }
  int errors() const { return errorcount_; }
  int warm() const { return warm_; }
  bool copy_verify_destination() const { return copy_verify_destination_; }
  bool verify_after_fill() const { return verify_after_fill_; }
  bool diag_vm_stats() const { return diag_vm_stats_; }
  bool diag_block_history() const { return diag_block_history_; }
  bool diag_phase_summary() const { return diag_phase_summary_; }
  // 옵션을 지정하지 않은 실행에서는 기존 Check Worker가 STOP 이후
  // Valid queue를 끝까지 검사합니다. 종료 검사 옵션을 명시한 실행만
  // Runtime Check와 별도 종료 Check를 분리합니다.
  bool legacy_final_check_mode() const {
    return !skip_final_check_ && !final_check_threads_explicit_;
  }
  int64 diagnostic_elapsed_us() const {
    return sat_get_time_us() - diagnostic_start_us_;
  }
  // 종료한 Worker의 local 통계를 한 번만 전역 결과에 합산합니다.
  void MergeDiagnosticPhaseStats(const DiagnosticPhaseStats *stats,
                                 int count);
  // 상세 memory mismatch 로그의 전역 예산을 원자적으로 예약합니다.
  // 오류 수 집계, reread와 expected 복구는 이 반환값과 관계없이 수행합니다.
  bool ClaimDetailedErrorLog() {
    if (error_log_limit_ < 0)
      return true;
    uint64 index = error_log_attempted_.fetch_add(
        1, std::memory_order_acq_rel);
    if (index < static_cast<uint64>(error_log_limit_)) {
      error_log_detailed_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    error_log_suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  FillPreset fill_preset() const { return fill_preset_; }
  FillDirection fill_direction() const { return fill_direction_; }
  int fill_verify_every() const { return fill_verify_every_; }
  int fill_yield_bytes() const { return fill_yield_bytes_; }
  InvertRange invert_range() const { return invert_range_; }
  // 실제 Invert pass가 순회하는 byte 수를 반환합니다. Legacy 계산은
  // upstream의 uint64 word 수를 uint32 pointer에 적용한 범위를 보존합니다.
  int64 invert_range_bytes() const {
    if (invert_range_ == INVERT_RANGE_FULL)
      return page_length_;
    const int block_size = 4096;
    const int uint64_words_per_block = block_size / sizeof(uint64);
    return static_cast<int64>(page_length_ / block_size) *
           uint64_words_per_block * sizeof(unsigned int);
  }
  // SAT 작업 단위의 논리적 번호를 사용하여 즉시 검사 대상을 결정합니다.
  // Fill Worker 완료 순서와 관계없이 같은 N 값은 같은 주소 집합을 선택합니다.
  bool ShouldVerifyFilledPage(uint64 page_offset) const {
    if (fill_verify_every_ <= 0)
      return false;
    uint64 block_number = page_offset / page_length_ + 1;
    return (block_number % fill_verify_every_) == 0;
  }
  bool stop_on_error() const { return stop_on_error_; }
  // Worker가 오류를 검출했을 때 Runtime 시작 또는 실행 지속을 중단하도록
  // 메인 제어 반복문에 전달하는 원자적 종료 요청입니다.
  void RequestErrorStop() {
    error_stop_requested_.store(true, std::memory_order_release);
  }
  bool error_stop_requested() const {
    // 옵션 비활성 경로에서는 hot loop의 atomic load를 생략합니다.
    // stop_on_error_는 Worker 생성 전에 설정되고 실행 중 변경되지 않습니다.
    if (!stop_on_error_)
      return false;
    return error_stop_requested_.load(std::memory_order_acquire);
  }
  bool use_affinity() const { return use_affinity_; }
  int current_dram_frequency() const {
    return current_dram_frequency_.load(std::memory_order_acquire);
  }
  uint64 dram_frequency_epoch() const {
    return dram_frequency_epoch_.load(std::memory_order_acquire);
  }
  // 옵션 활성화 시 SAT 작업 단위의 마지막 추적 write 정보를 갱신합니다.
  // Fill과 Copy는 전체 작업 단위, Invert는 선택 범위 완료 후 호출합니다.
  void RecordBlockWrite(uint64 page_offset,
                        BlockWriter writer,
                        int writer_thread,
                        int writer_cpu,
                        uint64 frequency_epoch_begin,
                        uint64 frequency_epoch_end);
  // 오류 로그에 사용할 마지막 추적 writer 정보를 문자열로 구성합니다.
  bool FormatBlockHistory(uint64 page_offset,
                          char *buffer,
                          size_t buffer_size) const;
  const char *dram_frequency_mode() const {
    if (dram_frequencies_.empty())
      return "none";
    return dram_frequencies_.size() == 1 ? "fixed" : "sweep";
  }
  DramAddressMapProfile dram_address_map_profile() const {
    return dram_address_map_profile_;
  }
  int32 region_mask() const { return region_mask_; }
  // Semi-accessor to find the "nth" region to avoid replicated bit searching..
  int32 region_find(int32 num) const {
    for (int i = 0; i < 32; i++) {
      if ((1 << i) & region_mask_) {
        if (num == 0)
          return i;
        num--;
      }
    }
    return 0;
  }

  // Causes false errors for unittesting.
  // Setting to "true" causes errors to be injected.
  void set_error_injection(bool errors) { error_injection_ = errors; }
  bool error_injection() const { return error_injection_; }

 protected:
  // Opens log file for writing. Returns 0 on failure.
  bool InitializeLogfile();
  // Checks for supported environment. Returns 0 on failure.
  bool CheckEnvironment();
  // Allocates size_ bytes of test memory.
  bool AllocateMemory();
  // Initializes datapattern reference structures.
  bool InitializePatterns();
  // Initializes test memory with datapatterns.
  bool InitializePages();
  // 전체 SAT 작업 단위를 대상으로 사전 채움 또는 최종 Pattern Fill을 실행합니다.
  bool RunFillPass(bool preset_only, const char *phase);

  // Start up worker threads.
  virtual void InitializeThreads();
  // Worker pthread를 생성하고 전체 생성 성공 여부를 반환합니다.
  bool SpawnThreads();
  // Reap worker threads.
  void JoinThreads();
  // Run bandwidth and error analysis.
  virtual void RunAnalysis();
  // Delete worker threads.
  void DeleteThreads();

  // Return the number of cpus in the system.
  int CpuCount();
  // Return the worst-case (largest) cache line size of the system.
  int CacheLineSize();
  // Read int values from kernel file system e.g. sysfs
  int ReadInt(const char *filename, int *value);
  // Collect error counts from threads.
  int64 GetTotalErrorCount();
  // 설정된 kernel interface에 고정 DDR 주파수 요청을 전달합니다.
  bool ApplyDramFrequency(int frequency);
  // 선택된 실행 단계 경계에서 page fault, RSS와 mapping 정보를 기록합니다.
  // 정보 확인 실패는 시험 실패로 처리하지 않고 unknown으로 출력합니다.
  void LogVmStats(const char *phase);
  void PrintDiagnosticPhaseSummary();

  // Command line arguments.
  string cmdline_;
  string pattern_selector_;           // -P로 지정한 Pattern ID·이름 목록.
  int pattern_byte_offset_;           // SAT 작업 단위 기준 Pattern 이동 byte.
  int post_fill_delay_seconds_;       // Fill 종료 후 검사 전 대기 시간.
  int runtime_start_delay_seconds_;   // Queue 구성 후 Runtime 전 대기 시간.

  // 대상 시스템의 DDR 주파수 제어와 오류 시점 기록에 사용하는 상태입니다.
  vector<int> dram_frequencies_;       // 입력 순서를 유지한 요청 주파수 목록.
  bool dram_sweep_;                    // Runtime 요청값이 두 개 이상이면 순환.
  int dram_step_seconds_;              // Runtime 주파수 요청 간격.
  string dram_frequency_node_;         // kernel interface 파일 경로.
  std::atomic<int> current_dram_frequency_;  // 마지막으로 성공한 전달 요청값.
  std::atomic<uint64> dram_frequency_epoch_;  // 성공한 DDR 요청의 세대 번호.
  DramAddressMapProfile dram_address_map_profile_;  // 선택형 주소 해석 방식.

  // Memory and test configuration.
  int runtime_seconds_;               // Seconds to run.
  int page_length_;                   // Length of each memory block.
  int64 pages_;                       // Number of memory blocks.
  int64 size_;                        // Size of memory tested, in bytes.
  int64 size_mb_;                     // Size of memory tested, in MB.
  int64 reserve_mb_;                  // Reserve at least this amount of memory
                                      // for the system, in MB.
  int64 min_hugepages_mbytes_;        // Minimum hugepages size.
  int64 freepages_;                   // How many invalid pages we need.
  int disk_pages_;                    // Number of pages per temp file.
  uint64 paddr_base_;                 // Physical address base.
  uint64 channel_hash_;               // Mask of address bits XORed for channel.
  int channel_width_;                 // Channel width in bits.
  vector< vector<string> > channels_;  // Memory module names per channel.

  // Control flags.
  volatile sig_atomic_t user_break_;  // User has signalled early exit.  Used as
                                      // a boolean.
  int verbosity_;                     // How much to print.
  int print_delay_;                   // Chatty update frequency.
  int strict_;                        // Check results per transaction.
  int warm_;                          // FPU warms CPU while copying.
  bool verify_after_fill_;            // 초기 Fill 뒤 Valid entry 순회 검사.
  bool copy_verify_destination_;      // Copy destination 즉시 readback 검사.
  bool prefault_pages_;               // 병렬 Fill 전 운영체제 페이지 사전 접근.
  bool diag_vm_stats_;                // 단계 경계의 VM 상태 기록.
  bool diag_block_history_;           // 선택형 SAT 작업 단위 write 이력.
  bool diag_phase_summary_;           // 단계별 처리량·오류 요약.
  FillPreset fill_preset_;            // 최종 Pattern 전 동일값 사전 채움 설정.
  FillDirection fill_direction_;      // Fill store의 주소 진행 방향.
  int fill_verify_every_;             // 논리 block 번호 기준 즉시 검사 간격.
  int fill_yield_bytes_;              // Fill 중 sched_yield() 호출 byte 간격.
  InvertRange invert_range_;           // Invert pass가 처리할 주소 범위.
  int final_check_threads_;           // 종료 시점 Valid 검사 전용 Worker 수.
  bool final_check_threads_explicit_;  // 종료 검사 Worker 수를 명시했는지 여부.
  bool skip_final_check_;             // 종료 시점 Valid 검사 생략 여부.
  int address_mode_;                  // 32 or 64 bit binary.
  bool stop_on_error_;                // Mismatch 처리 후 종료 요청을 설정.
  int64 error_log_limit_;              // -1은 상세 memory 오류 로그 무제한.
  std::atomic<uint64> error_log_attempted_;
  std::atomic<uint64> error_log_detailed_;
  std::atomic<uint64> error_log_suppressed_;
  std::atomic<bool> error_stop_requested_;  // Worker가 전달한 종료 요청.
  bool findfiles_;                    // Autodetect tempfile locations.

  bool error_injection_;              // Simulate errors, for unittests.
  bool crazy_error_injection_;        // Simulate lots of errors.
#ifdef STRESSAPPTEST_ENABLE_TEST_HOOKS
  // CI 전용 build에서 initial Fill 직후 첫 SAT 작업 단위를 변형합니다.
  // 일반 Android release에는 이 field와 command-line option이 없습니다.
  int test_corrupt_after_fill_words_;
#endif
  uint64 max_errorcount_;             // Number of errors before forced exit.
  int run_on_anything_;               // Ignore unknown machine ereor.
  bool use_logfile_;                  // Log to a file.
  char logfilename_[255];             // Name of file to log to.
  int logfile_;                       // File handle to log to.
  bool log_timestamps_;               // Whether to add timestamps to log lines.

  // Disk thread options.
  int read_block_size_;               // Size of block to read from disk.
  int write_block_size_;              // Size of block to write to disk.
  int64 segment_size_;                // Size of segment to split disk into.
  int cache_size_;                    // Size of disk cache.
  int blocks_per_segment_;            // Number of blocks to test per segment.
  int read_threshold_;                // Maximum time (in us) a read should take
                                      // before warning of a slow read.
  int write_threshold_;               // Maximum time (in us) a write should
                                      // take before warning of a slow write.
  int non_destructive_;               // Whether to use non-destructive mode for
                                      // the disk test.

  // Generic Options.
  int monitor_mode_;                  // Switch for monitor-only mode SAT.
                                      // This switch trumps most of the other
                                      // argument, as SAT will only run error
                                      // polling threads.
  int tag_mode_;                      // Do tagging of memory and strict
                                      // checking for misplaced cachelines.

  bool do_page_map_;                  // Should we print a list of used pages?
  unsigned char *page_bitmap_;        // Store bitmap of physical pages seen.
  uint64 page_bitmap_size_;           // Length of physical memory represented.

  // Cpu Cache Coherency Options.
  bool cc_test_;                      // Flag to decide whether to start the
                                      // cache coherency threads.
  int cc_cacheline_count_;            // Number of cache line size structures.
  int cc_cacheline_size_;             // Size of a cache line.
  int cc_inc_count_;                  // Number of times to increment the shared
                                      // cache lines structure members.

  // Cpu Frequency Options.
  bool cpu_freq_test_;                // Flag to decide whether to start the
                                      // cpu frequency thread.
  int cpu_freq_threshold_;            // The MHz threshold which will cause
                                      // the test to fail.
  int cpu_freq_round_;                // Round the computed frequency to this
                                      // value.

  // Thread control.
  int file_threads_;                  // Threads of file IO.
  int net_threads_;                   // Threads of network IO.
  int listen_threads_;                // Threads for network IO to connect.
  int memory_threads_;                // Threads of memcpy.
  int invert_threads_;                // Threads of invert.
  int fill_threads_;                  // Threads of memset.
  int check_threads_;                 // Threads of strcmp.
  int cpu_stress_threads_;            // Threads of CPU stress workload.
  int disk_threads_;                  // Threads of disk test.
  int random_threads_;                // Number of random disk threads.
  int total_threads_;                 // Total threads used.
  bool error_poll_;                   // Poll for system errors.

  // Resources.
  cc_cacheline_data *cc_cacheline_data_;  // The cache line sized datastructure
                                          // used by the ccache threads
                                          // (in worker.h).
  vector<string> filename_;           // Filenames for file IO.
  vector<string> ipaddrs_;            // Addresses for network IO.
  vector<string> diskfilename_;       // Filename for disk IO device.
  // Block table for IO device.
  vector<DiskBlockTable*> blocktables_;

  bool use_affinity_;                 // Should stressapptest set cpu affinity?
  int32 region_mask_;                 // Bitmask of available NUMA regions.
  int32 region_count_;                // Count of available NUMA regions.
  int32 region_[32];                  // Pagecount per region.
  int region_mode_;                   // What to do with NUMA hints?
  static const int kLocalNuma = 1;    // Target local memory.
  static const int kRemoteNuma = 2;   // Target remote memory.

  // Results.
  int64 errorcount_;                  // Total hardware incidents seen.
  int64 initialization_errorcount_;   // Runtime 시작 전 검출한 오류 수.
  int statuscount_;                   // Total test errors seen.
  int64 vm_stats_last_minor_faults_;  // 이전 VM snapshot의 minor fault 수.
  int64 vm_stats_last_major_faults_;  // 이전 VM snapshot의 major fault 수.

  // Queue의 page_entry 크기를 기본 실행에서 변경하지 않기 위해 옵션 사용
  // 시에만 별도 배열을 할당합니다.
  struct BlockHistory {
    uint64 generation;
    int64 write_complete_us;
    uint64 frequency_epoch_begin;
    uint64 frequency_epoch_end;
    int writer_thread;
    int writer_cpu;
    BlockWriter writer;
  };
  BlockHistory *block_history_;
  DiagnosticPhaseStats diagnostic_phase_stats_[DIAG_PHASE_COUNT];
  int64 diagnostic_start_us_;
  pthread_mutex_t diagnostic_stats_lock_;

  // Thread type constants and types
  enum ThreadType {
    kMemoryType = 0,
    kFileIOType = 1,
    kNetIOType = 2,
    kNetSlaveType = 3,
    kCheckType = 4,
    kInvertType = 5,
    kDiskType = 6,
    kRandomDiskType = 7,
    kCPUType = 8,
    kErrorType = 9,
    kCCType = 10,
    kCPUFreqType = 11,
  };

  // Helper functions.
  virtual void AcquireWorkerLock();
  virtual void ReleaseWorkerLock();
  pthread_mutex_t worker_lock_;  // Lock access to the worker thread structure.
  typedef vector<WorkerThread*> WorkerVector;
  typedef map<int, WorkerVector*> WorkerMap;
  // Contains all worker threads.
  WorkerMap workers_map_;
  // Delay between power spikes.
  time_t pause_delay_;
  // The duration of each pause (for power spikes).
  time_t pause_duration_;
  // For the workers we pause and resume to create power spikes.
  WorkerStatus power_spike_status_;
  // For the workers we never pause.
  WorkerStatus continuous_status_;

  class OsLayer *os_;                   // Os abstraction: put hacks here.
  class PatternList *patternlist_;      // Access to global data patterns.

  // RunAnalysis methods
  void AnalysisAllStats();              // Summary of all runs.
  void MemoryStats();
  void FileStats();
  void NetStats();
  void CheckStats();
  void InvertStats();
  void DiskStats();

  void QueueStats();

  // Physical page use reporting.
  void AddrMapInit();
  void AddrMapUpdate(struct page_entry *pe);
  void AddrMapPrint();

  // additional memory data from google-specific tests.
  virtual void GoogleMemoryStats(float *memcopy_data,
                                 float *memcopy_bandwidth);

  virtual void GoogleOsOptions(std::map<std::string, std::string> *options);

  // Page queues, only one of (valid_+empty_) or (finelock_q_) will be used
  // at a time. A commandline switch controls which queue implementation will
  // be used.
  class PageEntryQueue *valid_;        // Page queue structure, valid pages.
  class PageEntryQueue *empty_;        // Page queue structure, free pages.
  class FineLockPEQueue *finelock_q_;  // Page queue with fine-grain locks
  Sat::PageQueueType pe_q_implementation_;   // Queue implementation switch

  DISALLOW_COPY_AND_ASSIGN(Sat);
};

Sat *SatFactory();

#endif  // STRESSAPPTEST_SAT_H_
