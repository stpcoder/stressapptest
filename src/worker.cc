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

// worker.cc : individual tasks that can be run in combination to
// stress the system

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/times.h>

// These are necessary, but on by default
// #define __USE_GNU
// #define __USE_LARGEFILE64
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/unistd.h>  // for gettid

// For size of block device
#include <sys/ioctl.h>
#include <linux/fs.h>
// For asynchronous I/O
#ifdef HAVE_LIBAIO_H
#include <libaio.h>
#endif

#include <sys/syscall.h>

#include <set>
#include <new>
#include <string>

// This file must work with autoconf on its public version,
// so these includes are correct.
#include "error_diag.h"  // NOLINT
#include "dram_address.h"  // NOLINT
#include "invert_workload.h"  // NOLINT
#include "os.h"          // NOLINT
#include "pattern.h"     // NOLINT
#include "queue.h"       // NOLINT
#include "sat.h"         // NOLINT
#include "sattypes.h"    // NOLINT
#include "worker.h"      // NOLINT

// Syscalls
// Why ubuntu, do you hate gettid so bad?
#if !defined(__NR_gettid)
  #define __NR_gettid             224
#endif

#define gettid() syscall(__NR_gettid)
#if !defined(CPU_SETSIZE)
_syscall3(int, sched_getaffinity, pid_t, pid,
          unsigned int, len, cpu_set_t*, mask)
_syscall3(int, sched_setaffinity, pid_t, pid,
          unsigned int, len, cpu_set_t*, mask)
#endif

namespace {
  // Work around the sad fact that there are two (gnu, xsi) incompatible
  // versions of strerror_r floating around google. Awesome.
  bool sat_strerror(int err, char *buf, int len) {
    buf[0] = 0;
    char *errmsg = reinterpret_cast<char*>(strerror_r(err, buf, len));
    int retval = reinterpret_cast<int64>(errmsg);
    if (retval == 0)
      return true;
    if (retval == -1)
      return false;
    if (errmsg != buf) {
      strncpy(buf, errmsg, len - 1);
      buf[len - 1] = 0;
    }
    return true;
  }


  inline uint64 addr_to_tag(void *address) {
    return reinterpret_cast<uint64>(address);
  }

  // Invert workload 공통 루프가 운영체제별 cache clean hint를 호출하도록
  // 정적 함수를 동일한 signature로 연결합니다.
  void InvertFlushHintAdapter(void *address) {
    OsLayer::FastFlushHint(address);
  }
}  // namespace

const char *DiagnosticPhaseName(DiagnosticPhase phase) {
  switch (phase) {
    case DIAG_PHASE_PRESET_FILL: return "preset_fill";
    case DIAG_PHASE_INITIAL_FILL: return "initial_fill";
    case DIAG_PHASE_FILL_IMMEDIATE_CHECK: return "fill/immediate_check";
    case DIAG_PHASE_POST_FILL_CHECK: return "post_fill/full_check";
    case DIAG_PHASE_COPY_TRANSFER: return "copy/transfer";
    case DIAG_PHASE_COPY_DESTINATION_CHECK:
      return "copy/destination_check";
    case DIAG_PHASE_INVERT_PRECHECK: return "invert/precheck";
    case DIAG_PHASE_INVERT_RMW: return "invert/rmw";
    case DIAG_PHASE_INVERT_POSTCHECK: return "invert/postcheck";
    case DIAG_PHASE_RUNTIME_CHECK: return "check/runtime_check";
    case DIAG_PHASE_FINAL_CHECK: return "check/final_check";
    case DIAG_PHASE_COUNT: break;
  }
  return "unknown";
}

#if !defined(O_DIRECT)
// Sometimes this isn't available.
// Disregard if it's not defined.
  #define O_DIRECT            0
#endif

// 상세 비교에서 수집한 오류 정보를 Logger가 처리할 때까지 보관합니다.
struct ErrorRecord {
  ErrorRecord()
      : actual(0),
        reread(0),
        expected(0),
        vaddr(NULL),
        vbyteaddr(NULL),
        paddr(0),
        tagvaddr(NULL),
        tagpaddr(0),
        lastcpu(0),
        patternname(NULL),
        worker_name("unknown"),
        phase("unknown"),
        pattern_byte_offset(0),
        sat_page_offset(~static_cast<uint64>(0)),
        offset_in_page(~static_cast<uint64>(0)),
        write_dram_frequency(-1),
        read_dram_frequency(-1),
        reread_dram_frequency(-1) {}

  uint64 actual;  // This is the actual value read.
  uint64 reread;  // This is the actual value, reread.
  uint64 expected;  // This is what it should have been.
  uint64 *vaddr;  // This is where it was (or wasn't).
  char *vbyteaddr;  // This is byte specific where the data was (or wasn't).
  uint64 paddr;  // This is the bus address, if available.
  uint64 *tagvaddr;  // This holds the tag value if this data was tagged.
  uint64 tagpaddr;  // This holds the physical address corresponding to the tag.
  uint32 lastcpu;  // This holds the CPU recorded as probably writing this data.
  const char *patternname;  // This holds the pattern name of the expected data.
  const char *worker_name;  // Mismatch를 검출한 Worker 종류.
  const char *phase;  // Worker 내부의 검사 단계.
  unsigned int pattern_byte_offset;  // SAT 작업 단위 기준 Pattern byte 위치.
  uint64 sat_page_offset;  // 시험 영역 기준 SAT 작업 단위 시작 byte offset.
  uint64 offset_in_page;  // SAT 작업 단위 기준 mismatch word의 byte offset.
  // 작업 단위의 최근 write pass 시작과 read·reread 직전에 프로그램에
  // 저장되어 있던 마지막 성공 DDR 주파수 요청값입니다.
  int write_dram_frequency;
  int read_dram_frequency;
  int reread_dram_frequency;
};

// 주파수 요청 기록이 없으면 로그에 unknown을 출력합니다.
static void FormatDramFrequencyValue(int frequency,
                                     char *buffer,
                                     size_t buffer_size) {
  if (frequency >= 0)
    snprintf(buffer, buffer_size, "%d", frequency);
  else
    snprintf(buffer, buffer_size, "unknown");
}

// 최근 write pass와 오류 read·reread 시점의 주파수 요청값을 한 로그
// 필드로 구성합니다.
static void FormatDramFrequencies(struct ErrorRecord *error,
                                  char *buffer,
                                  size_t buffer_size) {
  char write_frequency[32];
  char read_frequency[32];
  char reread_frequency[32];
  FormatDramFrequencyValue(error->write_dram_frequency,
                           write_frequency, sizeof(write_frequency));
  FormatDramFrequencyValue(error->read_dram_frequency,
                           read_frequency, sizeof(read_frequency));
  FormatDramFrequencyValue(error->reread_dram_frequency,
                           reread_frequency, sizeof(reread_frequency));
  snprintf(buffer, buffer_size, "write=%s read=%s reread=%s",
           write_frequency, read_frequency, reread_frequency);
}

// 선택한 주소 변환 프로필로 physical address를 DRAM 좌표로 해석합니다.
// Physical address 확인 권한 또는 프로필이 없으면 모든 좌표를 unknown으로 둡니다.
static void FormatDramCoordinates(class Sat *sat,
                                  uint64 physical_address,
                                  char *buffer,
                                  size_t buffer_size) {
  DramAddress address = {};
  if (physical_address == 0 ||
      !DecodeDramAddress(sat->dram_address_map_profile(),
                         physical_address, &address)) {
    snprintf(buffer, buffer_size,
             "ch:unknown,rk:unknown,sc:unknown,bg:unknown,bank:unknown,"
             "row:unknown,col:unknown");
    return;
  }

  snprintf(buffer, buffer_size,
           "ch:%u,rk:%u,sc:%u,bg:%u,bank:%u,row:%x,col:%x",
           address.channel, address.rank, address.subchannel,
           address.bank_group, address.bank, address.row, address.column);
}

// This is a helper function to create new threads with pthreads.
static void *ThreadSpawnerGeneric(void *ptr) {
  WorkerThread *worker = static_cast<WorkerThread*>(ptr);
  worker->StartRoutine();
  return NULL;
}

void WorkerStatus::Initialize() {
  sat_assert(0 == pthread_mutex_init(&num_workers_mutex_, NULL));

  pthread_rwlockattr_t attrs;
  sat_assert(0 == pthread_rwlockattr_init(&attrs));
#ifdef HAVE_PTHREAD_RWLOCKATTR_SETKIND_NP
  // Avoid writer lock starvation.
  sat_assert(0 == pthread_rwlockattr_setkind_np(
                      &attrs, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP));
#endif
  sat_assert(0 == pthread_rwlock_init(&status_rwlock_, &attrs));

#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 == pthread_barrier_init(&pause_barrier_, NULL,
                                       num_workers_ + 1));
  sat_assert(0 == pthread_rwlock_init(&pause_rwlock_, &attrs));
#endif

  sat_assert(0 == pthread_rwlockattr_destroy(&attrs));
}

void WorkerStatus::Destroy() {
  sat_assert(0 == pthread_mutex_destroy(&num_workers_mutex_));
  sat_assert(0 == pthread_rwlock_destroy(&status_rwlock_));
#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 == pthread_barrier_destroy(&pause_barrier_));
#endif
}

void WorkerStatus::PauseWorkers() {
  if (SetStatus(PAUSE) != PAUSE)
    WaitOnPauseBarrier();
}

void WorkerStatus::ResumeWorkers() {
  if (SetStatus(RUN) == PAUSE)
    WaitOnPauseBarrier();
}

void WorkerStatus::StopWorkers() {
  if (SetStatus(STOP) == PAUSE)
    WaitOnPauseBarrier();
}

bool WorkerStatus::ContinueRunning(bool *paused) {
  // This loop is an optimization.  We use it to immediately re-check the status
  // after resuming from a pause, instead of returning and waiting for the next
  // call to this function.
  if (paused) {
    *paused = false;
  }
  for (;;) {
    switch (GetStatus()) {
      case RUN:
        return true;
      case PAUSE:
        // Wait for the other workers to call this function so that
        // PauseWorkers() can return.
        WaitOnPauseBarrier();
        // Wait for ResumeWorkers() to be called.
        WaitOnPauseBarrier();
        // Indicate that a pause occurred.
        if (paused) {
          *paused = true;
        }
        break;
      case STOP:
        return false;
    }
  }
}

bool WorkerStatus::ContinueRunningNoPause() {
  return (GetStatus() != STOP);
}

void WorkerStatus::RemoveSelf() {
  // Acquire a read lock on status_rwlock_ while (status_ != PAUSE).
  for (;;) {
    AcquireStatusReadLock();
    if (status_ != PAUSE)
      break;
    // We need to obey PauseWorkers() just like ContinueRunning() would, so that
    // the other threads won't wait on pause_barrier_ forever.
    ReleaseStatusLock();
    // Wait for the other workers to call this function so that PauseWorkers()
    // can return.
    WaitOnPauseBarrier();
    // Wait for ResumeWorkers() to be called.
    WaitOnPauseBarrier();
  }

  // This lock would be unnecessary if we held a write lock instead of a read
  // lock on status_rwlock_, but that would also force all threads calling
  // ContinueRunning() to wait on this one.  Using a separate lock avoids that.
  AcquireNumWorkersLock();
  // Decrement num_workers_ and reinitialize pause_barrier_, which we know isn't
  // in use because (status != PAUSE).
#ifdef HAVE_PTHREAD_BARRIERS
  sat_assert(0 == pthread_barrier_destroy(&pause_barrier_));
  sat_assert(0 == pthread_barrier_init(&pause_barrier_, NULL, num_workers_));
#endif
  --num_workers_;
  ReleaseNumWorkersLock();

  // Release status_rwlock_.
  ReleaseStatusLock();
}


// Parent thread class.
WorkerThread::WorkerThread() {
  status_ = false;
  pages_copied_ = 0;
  errorcount_ = 0;
  runduration_usec_ = 1;
  priority_ = Normal;
  worker_status_ = NULL;
  thread_spawner_ = &ThreadSpawnerGeneric;
  spawned_ = false;
  diagnostic_phase_stats_ = NULL;
  diagnostic_stats_published_ = false;
  tag_mode_ = false;
}

WorkerThread::~WorkerThread() {
  delete[] diagnostic_phase_stats_;
}

// Constructors. Just init some default values.
FillThread::FillThread() {
  num_pages_to_fill_ = 0;
  preset_only_ = false;
}

// Initialize file name to empty.
FileThread::FileThread() {
  filename_ = "";
  devicename_ = "";
  pass_ = 0;
  page_io_ = true;
  crc_page_ = -1;
  local_page_ = NULL;
}

// If file thread used bounce buffer in memory, account for the extra
// copy for memory bandwidth calculation.
float FileThread::GetMemoryCopiedData() {
  if (!os_->normal_mem())
    return GetCopiedData();
  else
    return 0;
}

// Initialize target hostname to be invalid.
NetworkThread::NetworkThread() {
  snprintf(ipaddr_, sizeof(ipaddr_), "Unknown");
  sock_ = 0;
}

// Initialize?
NetworkSlaveThread::NetworkSlaveThread() {
  sat_assert(0 == pthread_mutex_init(&socket_lock_, NULL));
}

NetworkSlaveThread::~NetworkSlaveThread() {
  CloseOwnedSocket();
  sat_assert(0 == pthread_mutex_destroy(&socket_lock_));
}

// Initialize?
NetworkListenThread::NetworkListenThread() {
}

// Init member variables.
void WorkerThread::InitThread(int thread_num_init,
                              class Sat *sat_init,
                              class OsLayer *os_init,
                              class PatternList *patternlist_init,
                              WorkerStatus *worker_status) {
  sat_assert(worker_status);
  worker_status->AddWorkers(1);

  thread_num_ = thread_num_init;
  sat_ = sat_init;
  os_ = os_init;
  patternlist_ = patternlist_init;
  worker_status_ = worker_status;

  if (sat_->diag_phase_summary()) {
    diagnostic_phase_stats_ =
        new (std::nothrow) DiagnosticPhaseStats[DIAG_PHASE_COUNT];
    if (!diagnostic_phase_stats_) {
      logprintf(0,
                "Process Error: failed to allocate phase statistics for "
                "worker %d\n",
                thread_num_);
      sat_->bad_status();
    } else {
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
    }
  }

  AvailableCpus(&cpu_mask_);
  tag_ = 0xffffffff;

  tag_mode_ = sat_->tag_mode();
}


// Use pthreads to prioritize a system thread.
bool WorkerThread::InitPriority() {
  // This doesn't affect performance that much, and may not be too safe.

  bool ret = BindToCpus(&cpu_mask_);
  if (!ret)
    logprintf(11, "Log: Bind to %s failed.\n",
              cpuset_format(&cpu_mask_).c_str());

  logprintf(11, "Log: Thread %d running on core ID %d mask %s (%s).\n",
            thread_num_, sched_getcpu(),
            CurrentCpusFormat().c_str(),
            cpuset_format(&cpu_mask_).c_str());
#if 0
  if (priority_ == High) {
    sched_param param;
    param.sched_priority = 1;
    // Set the priority; others are unchanged.
    logprintf(0, "Log: Changing priority to SCHED_FIFO %d\n",
              param.sched_priority);
    if (sched_setscheduler(0, SCHED_FIFO, &param)) {
      char buf[256];
      sat_strerror(errno, buf, sizeof(buf));
      logprintf(0, "Process Error: sched_setscheduler "
                   "failed - error %d %s\n",
                errno, buf);
    }
  }
#endif
  return true;
}

// Work()를 실행할 pthread를 생성합니다.
bool WorkerThread::SpawnThread() {
  // 실행 중인 Worker 객체에 pthread를 중복 생성하지 않습니다.
  if (spawned_) {
    logprintf(0, "Process Error: worker thread %d was already spawned\n",
              thread_num_);
    status_ = false;
    return false;
  }

  int result = pthread_create(&thread_, NULL, thread_spawner_, this);
  if (result) {
    char buf[256];
    sat_strerror(result, buf, sizeof(buf));
    logprintf(0, "Process Error: pthread_create "
                  "failed - error %d %s\n", result,
              buf);
    status_ = false;
    return false;
  }

  spawned_ = true;
  return true;
}

// pthread 생성 실패로 실행되지 않은 Worker를 WorkerStatus에서 제외합니다.
// 실행된 Worker는 StartRoutine()의 마지막에서 직접 RemoveSelf()를 호출합니다.
void WorkerThread::RemoveUnspawnedWorker() {
  sat_assert(!spawned_);
  sat_assert(worker_status_);
  worker_status_->RemoveSelf();
}

// Kill the worker thread with SIGINT.
bool WorkerThread::KillThread() {
  if (!spawned_)
    return true;
  return (pthread_kill(thread_, SIGINT) == 0);
}

// Block until thread has exited.
bool WorkerThread::JoinThread() {
  if (!spawned_)
    return true;

  int result = pthread_join(thread_, NULL);

  if (result) {
    logprintf(0, "Process Error: pthread_join failed - error %d\n", result);
    status_ = false;
  }

  if (!result)
    PublishDiagnosticStats();
  spawned_ = false;

  // 0 is pthreads success.
  return (!result);
}


void WorkerThread::StartRoutine() {
  InitPriority();
  StartThreadTimer();
  Work();
  StopThreadTimer();
  worker_status_->RemoveSelf();
}


// Thread work loop. Execute until marked finished.
bool WorkerThread::Work() {
  do {
    logprintf(9, "Log: ...\n");
    // Sleep for 1 second.
    sat_sleep(1);
  } while (IsReadyToRun());

  return false;
}


// Returns CPU mask of CPUs available to this process,
// Conceptually, each bit represents a logical CPU, ie:
//   mask = 3  (11b):   cpu0, 1
//   mask = 13 (1101b): cpu0, 2, 3
bool WorkerThread::AvailableCpus(cpu_set_t *cpuset) {
  CPU_ZERO(cpuset);
#ifdef HAVE_SCHED_GETAFFINITY
  return sched_getaffinity(getppid(), sizeof(*cpuset), cpuset) == 0;
#else
  return 0;
#endif
}


// Returns CPU mask of CPUs this thread is bound to,
// Conceptually, each bit represents a logical CPU, ie:
//   mask = 3  (11b):   cpu0, 1
//   mask = 13 (1101b): cpu0, 2, 3
bool WorkerThread::CurrentCpus(cpu_set_t *cpuset) {
  CPU_ZERO(cpuset);
#ifdef HAVE_SCHED_GETAFFINITY
  return sched_getaffinity(0, sizeof(*cpuset), cpuset) == 0;
#else
  return 0;
#endif
}


// Bind worker thread to specified CPU(s)
//   Args:
//     thread_mask: cpu_set_t representing CPUs, ie
//                  mask = 1  (01b):   cpu0
//                  mask = 3  (11b):   cpu0, 1
//                  mask = 13 (1101b): cpu0, 2, 3
//
//   Returns true on success, false otherwise.
bool WorkerThread::BindToCpus(const cpu_set_t *thread_mask) {
  cpu_set_t process_mask;
  AvailableCpus(&process_mask);
  if (cpuset_isequal(thread_mask, &process_mask))
    return true;

  logprintf(11, "Log: available CPU mask - %s\n",
            cpuset_format(&process_mask).c_str());
  if (!cpuset_issubset(thread_mask, &process_mask)) {
    // Invalid cpu_mask, ie cpu not allocated to this process or doesn't exist.
    logprintf(0, "Log: requested CPUs %s not a subset of available %s\n",
              cpuset_format(thread_mask).c_str(),
              cpuset_format(&process_mask).c_str());
    return false;
  }
#ifdef HAVE_SCHED_GETAFFINITY
  if (sat_->use_affinity()) {
    return (sched_setaffinity(gettid(), sizeof(*thread_mask), thread_mask) == 0);
  } else {
    logprintf(11, "Log: Skipping CPU affinity set.\n");
  }
#endif
  return true;
}


// A worker thread can yield itself to give up CPU until it's scheduled again.
//   Returns true on success, false on error.
bool WorkerThread::YieldSelf() {
  return (sched_yield() == 0);
}

// 문자열 로그 단계와 고정 통계 slot의 대응을 반환합니다.
DiagnosticPhase WorkerThread::GetDiagnosticPhase(
    const char *worker_name, const char *phase) const {
  if (!worker_name || !phase)
    return DIAG_PHASE_COUNT;
  if (!strcmp(worker_name, "fill") && !strcmp(phase, "immediate_check"))
    return DIAG_PHASE_FILL_IMMEDIATE_CHECK;
  if (!strcmp(worker_name, "post_fill") && !strcmp(phase, "full_check"))
    return DIAG_PHASE_POST_FILL_CHECK;
  if (!strcmp(worker_name, "copy") && !strcmp(phase, "source_check"))
    return DIAG_PHASE_COPY_TRANSFER;
  if (!strcmp(worker_name, "copy") && !strcmp(phase, "destination_check"))
    return DIAG_PHASE_COPY_DESTINATION_CHECK;
  if (!strcmp(worker_name, "invert") && !strcmp(phase, "precheck"))
    return DIAG_PHASE_INVERT_PRECHECK;
  if (!strcmp(worker_name, "invert") && !strcmp(phase, "postcheck"))
    return DIAG_PHASE_INVERT_POSTCHECK;
  if (!strcmp(worker_name, "check") && !strcmp(phase, "runtime_check"))
    return DIAG_PHASE_RUNTIME_CHECK;
  if (!strcmp(worker_name, "check") && !strcmp(phase, "final_check"))
    return DIAG_PHASE_FINAL_CHECK;
  return DIAG_PHASE_COUNT;
}

// 한 SAT 작업 단위 처리가 끝난 시점에 Worker local counter를 갱신합니다.
void WorkerThread::RecordDiagnosticOperation(DiagnosticPhase phase,
                                             uint64 read_bytes,
                                             uint64 write_bytes,
                                             int errors) {
  if (!diagnostic_phase_stats_ || phase >= DIAG_PHASE_COUNT)
    return;
  DiagnosticPhaseStats *stats = &diagnostic_phase_stats_[phase];
  // 상세 비교 경로에서 시점을 기록하지 못한 오류 유형의 fallback입니다.
  // 완료된 이전 작업량을 저장한 뒤 현재 작업 단위의 byte를 합산합니다.
  if (errors > 0 && stats->first_error_us < 0) {
    stats->first_error_us = sat_->diagnostic_elapsed_us();
    stats->first_error_epoch = sat_->dram_frequency_epoch();
    stats->first_error_worker_bytes =
        stats->read_bytes + stats->write_bytes;
  }
  stats->blocks++;
  stats->read_bytes += read_bytes;
  stats->write_bytes += write_bytes;
  if (errors > 0)
    stats->word_mismatches += errors;
}

// Checksum mismatch로 64-bit 상세 비교에 진입한 4 KiB 구간을 집계합니다.
void WorkerThread::RecordDiagnosticChecksumMismatch(
    DiagnosticPhase phase) {
  if (!diagnostic_phase_stats_ || phase >= DIAG_PHASE_COUNT)
    return;
  diagnostic_phase_stats_[phase].checksum_mismatch_regions++;
}

// CheckRegion()이 실제 word mismatch를 확인한 지점에서 최초 시점을
// 기록합니다. 정상 데이터 경로에서는 호출되지 않습니다.
void WorkerThread::RecordDiagnosticFirstError(DiagnosticPhase phase) {
  if (!diagnostic_phase_stats_ || phase >= DIAG_PHASE_COUNT)
    return;
  DiagnosticPhaseStats *stats = &diagnostic_phase_stats_[phase];
  if (stats->first_error_us >= 0)
    return;
  stats->first_error_us = sat_->diagnostic_elapsed_us();
  stats->first_error_epoch = sat_->dram_frequency_epoch();
  stats->first_error_worker_bytes =
      stats->read_bytes + stats->write_bytes;
}

// pthread 종료 후 local 배열을 Sat 전역 통계에 한 번만 전달합니다.
void WorkerThread::PublishDiagnosticStats() {
  if (diagnostic_stats_published_ || !diagnostic_phase_stats_)
    return;
  sat_->MergeDiagnosticPhaseStats(
      diagnostic_phase_stats_, DIAG_PHASE_COUNT);
  diagnostic_stats_published_ = true;
}


// SAT 작업 단위 전체에 지정된 Pattern을 기록합니다. 기본 경로는 기존의
// 연속 64-bit store를 유지하고, 진단 옵션이 설정된 경우 방향·yield·Pattern
// offset을 반영합니다.
bool WorkerThread::FillPage(struct page_entry *pe) {
  // 유효한 작업 단위가 전달되었는지 확인합니다.
  if (pe == 0) {
    logprintf(0, "Process Error: Fill Page entry null\n");
    return 0;
  }

  int write_dram_frequency = sat_->current_dram_frequency();

  // 마지막으로 기록한 CPU 번호를 오류 로그용 상태에 저장합니다.
  pe->lastcpu = sched_getcpu();

  // Pattern은 32-bit 값을 만들고, Fill은 두 값을 묶어 64-bit로 저장합니다.
  uint64 *memwords = static_cast<uint64*>(pe->addr);
  int length = sat_->page_length();
  const bool default_fill_path =
      sat_->fill_direction() == Sat::FILL_DIRECTION_UP &&
      sat_->fill_yield_bytes() == 0 &&
      pe->pattern->byte_offset() == 0;

  // Up 방향, yield 미사용과 Pattern offset 0의 조합에서는 기존 Fill의
  // 주소 순회 반복문과 Pattern index 계산식을 사용합니다.
  if (default_fill_path) {
    if (tag_mode_) {
      for (int i = 0; i < length / wordsize_; ++i) {
        datacast_t data;
        if ((i & 0x7) == 0) {
          data.l64 = addr_to_tag(&memwords[i]);
        } else {
          data.l32.l = pe->pattern->pattern_unshifted(i << 1);
          data.l32.h = pe->pattern->pattern_unshifted((i << 1) + 1);
        }
        memwords[i] = data.l64;
      }
    } else {
      for (int i = 0; i < length / wordsize_; ++i) {
        datacast_t data;
        data.l32.l = pe->pattern->pattern_unshifted(i << 1);
        data.l32.h = pe->pattern->pattern_unshifted((i << 1) + 1);
        memwords[i] = data.l64;
      }
    }
  } else {
    const int words = length / wordsize_;
    const int yield_words = sat_->fill_yield_bytes() / wordsize_;
    for (int step = 0; step < words; ++step) {
      int i = sat_->fill_direction() == Sat::FILL_DIRECTION_UP
                  ? step : words - step - 1;
      datacast_t data;

      if (tag_mode_ && ((i & 0x7) == 0)) {
        data.l64 = addr_to_tag(&memwords[i]);
      } else {
        data.l32.l = pe->pattern->pattern(i << 1);
        data.l32.h = pe->pattern->pattern((i << 1) + 1);
      }
      memwords[i] = data.l64;

      if (yield_words > 0 && ((step + 1) % yield_words) == 0)
        YieldSelf();
    }
  }

  pe->write_dram_frequency = write_dram_frequency;
  return 1;
}


// 사전 채움 단계에서 SAT 작업 단위 전체에 동일한 64-bit 값을 기록합니다.
// 주소 방향과 yield 간격은 최종 Pattern Fill과 동일한 설정을 사용합니다.
bool WorkerThread::FillPageWithConstant(struct page_entry *pe, uint64 value) {
  if (pe == 0) {
    logprintf(0, "Process Error: Constant fill page entry null\n");
    return false;
  }

  uint64 *memwords = static_cast<uint64*>(pe->addr);
  int length = sat_->page_length();
  const int words = length / wordsize_;
  if (sat_->fill_direction() == Sat::FILL_DIRECTION_UP &&
      sat_->fill_yield_bytes() == 0) {
    for (int i = 0; i < words; ++i)
      memwords[i] = value;
    return true;
  }

  const int yield_words = sat_->fill_yield_bytes() / wordsize_;
  for (int step = 0; step < words; ++step) {
    int i = sat_->fill_direction() == Sat::FILL_DIRECTION_UP
                ? step : words - step - 1;
    memwords[i] = value;
    if (yield_words > 0 && ((step + 1) % yield_words) == 0)
      YieldSelf();
  }
  return true;
}


// 이 Fill Worker가 처리할 SAT 작업 단위 수를 설정합니다.
void FillThread::SetFillPages(int64 num_pages_to_fill_init) {
  num_pages_to_fill_ = num_pages_to_fill_init;
}

// 지정한 -P 목록 또는 가중치 기반 선택으로 이 작업 단위의 Pattern을 정합니다.
bool FillThread::FillPageRandom(struct page_entry *pe) {
  // 유효한 작업 단위가 전달되었는지 확인합니다.
  if (pe == 0) {
    logprintf(0, "Process Error: Fill Page entry null\n");
    return 0;
  }
  if (preset_only_) {
    // Queue에서 완료 상태를 표시하기 위해 임시 Pattern 포인터를 설정합니다.
    // 사전 채움 데이터는 이 임시 Pattern으로 검사하지 않습니다.
    pe->pattern = patternlist_->GetPattern(0);
    if (sat_->fill_preset() == Sat::FILL_PRESET_ZERO)
      return FillPageWithConstant(pe, 0x0000000000000000ULL);
    if (sat_->fill_preset() == Sat::FILL_PRESET_ONE)
      return FillPageWithConstant(pe, 0xffffffffffffffffULL);
    logprintf(0, "Process Error: preset Fill requested without a preset\n");
    return false;
  }
  if ((patternlist_ == 0) || (patternlist_->Size() == 0)) {
    logprintf(0, "Process Error: No data patterns available\n");
    return 0;
  }

  // -P 목록은 입력 순서대로 순환하고, 목록이 없으면 가중치로 선택합니다.
  pe->pattern = patternlist_->GetRandomPattern();
  pe->lastcpu = sched_getcpu();

  if (pe->pattern == 0) {
    logprintf(0, "Process Error: Null data pattern\n");
    return 0;
  }

  // 선택한 Pattern을 작업 단위 전체에 기록합니다.
  return FillPage(pe);
}


// 할당된 수만큼 Empty 작업 단위를 가져와 기록하고 Valid로 반환합니다.
bool FillThread::Work() {
  bool result = true;

  logprintf(9, "Log: Starting fill thread %d\n", thread_num_);

  // 지정된 작업 단위 수를 채우거나 오류가 발생할 때까지 반복합니다.
  struct page_entry pe;
  int64 loops = 0;
  while (IsReadyToRun() && (loops < num_pages_to_fill_)) {
    result = result && sat_->GetEmpty(&pe);
    if (!result) {
      logprintf(0, "Process Error: fill_thread failed to pop pages, "
                "bailing\n");
      break;
    }

    // 사전 채움 값 또는 선택한 Pattern을 기록합니다. Block history가
    // 활성화되면 write 전체에 걸친 DDR 요청 epoch 범위를 저장합니다.
    const bool record_history = sat_->diag_block_history();
    uint64 frequency_epoch_begin =
        record_history ? sat_->dram_frequency_epoch() : 0;
    result = result && FillPageRandom(&pe);
    if (!result) break;
    if (record_history) {
      sat_->RecordBlockWrite(
          pe.offset,
          preset_only_ ? Sat::BLOCK_WRITER_PRESET
                       : Sat::BLOCK_WRITER_INITIAL_FILL,
          thread_num_, sched_getcpu(),
          frequency_epoch_begin, sat_->dram_frequency_epoch());
    }
    if (sat_->diag_phase_summary()) {
      RecordDiagnosticOperation(
          preset_only_ ? DIAG_PHASE_PRESET_FILL : DIAG_PHASE_INITIAL_FILL,
          0, sat_->page_length(), 0);
    }

    // --stop_on_errors 요청 이후에도 queue 구성을 위해 Fill은 완료하며,
    // 추가 즉시 검사는 생략하여 후속 오류 로그 생성을 제한합니다.
    if (!preset_only_ && !sat_->error_stop_requested() &&
        sat_->ShouldVerifyFilledPage(pe.offset)) {
      int verify_errors =
          CrcCheckPage(&pe, "fill", "immediate_check");
      if (sat_->diag_phase_summary()) {
        RecordDiagnosticOperation(
            DIAG_PHASE_FILL_IMMEDIATE_CHECK,
            sat_->page_length(), 0, verify_errors);
      }
    }

    // 사전 채움과 최종 Fill 모두 완료 entry를 Valid로 반환합니다. 같은
    // Fill 단계에서 다른 Worker가 해당 entry를 다시 선택할 수 없습니다.
    result = result && sat_->PutValid(&pe);
    if (!result) {
      logprintf(0, "Process Error: fill_thread failed to push pages, "
                "bailing\n");
      break;
    }
    loops++;
  }

  // 처리 수와 종료 상태를 상위 제어 경로에 전달합니다.
  pages_copied_ = loops;
  status_ = result;
  logprintf(9, "Log: Completed %d: Fill thread. Status %d, %d pages filled\n",
            thread_num_, status_, pages_copied_);
  return result;
}


// 상세 비교에서 수집한 첫 read를 기준으로 같은 주소를 다시 load하고 오류
// 위치와 검출 단계를 출력합니다. AArch64 공통 경로의 Flush()는 cache 관리
// 명령 없이 반환하므로 read/write 문자열은 두 CPU load의 관계를 나타내는
// 소프트웨어 분류입니다.
void WorkerThread::ProcessError(struct ErrorRecord *error,
                                int priority,
                                const char *message) {
  char dimm_string[256] = "";
  char dram_frequencies[128];
  char current_dram_frequency[32];
  char dram_coordinates[192];
  char sat_location[128];
  char block_history[256];
  char block_history_suffix[288] = "";

  int core_id = sched_getcpu();

  // 아키텍처별 Flush()를 호출한 뒤 같은 가상 주소를 다시 읽습니다.
  // Reread 직전에 저장된 마지막 성공 DDR 주파수 요청값을 기록합니다.
  os_->Flush(error->vaddr);
  error->reread_dram_frequency = sat_->current_dram_frequency();
  error->reread = *(error->vaddr);
  FormatDramFrequencies(error, dram_frequencies, sizeof(dram_frequencies));

  char *good = reinterpret_cast<char*>(&(error->expected));
  char *bad = reinterpret_cast<char*>(&(error->actual));

  sat_assert(error->expected != error->actual);
  unsigned int offset = 0;
  for (offset = 0; offset < (sizeof(error->expected) - 1); offset++) {
    if (good[offset] != bad[offset])
      break;
  }

  error->vbyteaddr = reinterpret_cast<char*>(error->vaddr) + offset;
  uint64 error_offset_in_page = error->offset_in_page;
  if (error_offset_in_page != ~static_cast<uint64>(0))
    error_offset_in_page += offset;

  if (error->sat_page_offset != ~static_cast<uint64>(0) &&
      error_offset_in_page != ~static_cast<uint64>(0)) {
    snprintf(sat_location, sizeof(sat_location),
             "sat_block:%llu,sat_offset:0x%llx,block_offset:0x%llx",
             error->sat_page_offset / sat_->page_length(),
             error->sat_page_offset, error_offset_in_page);
  } else {
    snprintf(sat_location, sizeof(sat_location),
             "sat_block:unknown,sat_offset:unknown,block_offset:unknown");
  }
  if (error->sat_page_offset != ~static_cast<uint64>(0) &&
      sat_->FormatBlockHistory(error->sat_page_offset,
                               block_history, sizeof(block_history))) {
    snprintf(block_history_suffix, sizeof(block_history_suffix),
             ", block_history(%s)", block_history);
  }

  // 로그의 가상 주소와 물리 주소가 같은 64-bit word를 가리키도록 변환합니다.
  // Word 안의 첫 mismatch byte 위치는 block_offset에 반영합니다.
  error->paddr = os_->VirtualToPhysical(error->vaddr);
  FormatDramCoordinates(sat_, error->paddr,
                        dram_coordinates, sizeof(dram_coordinates));
  FormatDramFrequencyValue(error->reread_dram_frequency,
                           current_dram_frequency,
                           sizeof(current_dram_frequency));

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));

  // 기존 diagnoser와 외부 error report는 유지합니다. 선택형 예산은 상세
  // 문자열 출력에만 적용하여 오류 보고 semantics를 바꾸지 않습니다.
  if (priority < 5) {
    // Run miscompare error through diagnoser for logging and reporting.
    os_->error_diagnoser_->AddMiscompareError(dimm_string,
                                              reinterpret_cast<uint64>
                                              (error->vaddr), 1);

    if (priority <= sat_->verbosity() &&
        sat_->ClaimDetailedErrorLog()) {
      logprintf(priority,
                "%s: miscompare on CPU %d(<-%d) at %p(0x%llx:%s): "
                "read:0x%016llx, reread:0x%016llx, expected:0x%016llx. "
                "'%s'; %s, worker:%s, phase:%s, pattern_offset:%u, "
                "%s, %s, cur_mode:%s, cur_freq:%s, "
                "ddr_freq(%s)%s.\n",
                message,
                core_id,
                error->lastcpu,
                error->vaddr,
                error->paddr,
                dimm_string,
                error->actual,
                error->reread,
                error->expected,
                (error->patternname) ? error->patternname : "None",
                (error->reread == error->expected) ?
                    "read error" : "write error",
                error->worker_name,
                error->phase,
                error->pattern_byte_offset,
                sat_location,
                dram_coordinates,
                sat_->dram_frequency_mode(),
                current_dram_frequency,
                dram_frequencies,
                block_history_suffix);
    }
  }


  // 같은 손상값이 Copy를 통해 다른 작업 단위로 전파되지 않도록 해당
  // 64-bit 위치를 expected 값으로 복구합니다. Block history는 작업 단위
  // 전체를 마지막으로 기록한 주체를 유지하므로 이 word 복구는 기록하지
  // 않습니다. 한 구간의 여러 mismatch가 같은 이전 writer를 표시하게 됩니다.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
  if (sat_->stop_on_error())
    sat_->RequestErrorStop();
}



// File Worker의 source 또는 readback 대상에서 검출한 memory mismatch를
// reread하고 상세 주소와 데이터를 출력합니다. `crc_page_`가 유효한 경우에만
// 파일 readback의 source·destination 위치를 함께 보고합니다.
// 로그의 read error/write error는 actual·reread·expected 관계로 만든
// 소프트웨어 분류이며 실제 DRAM read/write 원인을 확정하지 않습니다.
void FileThread::ProcessError(struct ErrorRecord *error,
                              int priority,
                              const char *message) {
  char dimm_string[256] = "";
  char dram_frequencies[128];
  char current_dram_frequency[32];
  char dram_coordinates[192];

  // 아키텍처별 Flush() 호출 뒤 같은 가상 주소를 다시 읽습니다.
  os_->Flush(error->vaddr);
  error->reread_dram_frequency = sat_->current_dram_frequency();
  error->reread = *(error->vaddr);
  FormatDramFrequencies(error, dram_frequencies, sizeof(dram_frequencies));

  char *good = reinterpret_cast<char*>(&(error->expected));
  char *bad = reinterpret_cast<char*>(&(error->actual));

  sat_assert(error->expected != error->actual);
  unsigned int offset = 0;
  for (offset = 0; offset < (sizeof(error->expected) - 1); offset++) {
    if (good[offset] != bad[offset])
      break;
  }

  error->vbyteaddr = reinterpret_cast<char*>(error->vaddr) + offset;

  // 로그의 가상 주소와 물리 주소는 같은 64-bit word를 가리킵니다.
  // 파일 내부의 정확한 mismatch byte는 vbyteaddr로 별도 계산합니다.
  error->paddr = os_->VirtualToPhysical(error->vaddr);
  FormatDramCoordinates(sat_, error->paddr,
                        dram_coordinates, sizeof(dram_coordinates));
  FormatDramFrequencyValue(error->reread_dram_frequency,
                           current_dram_frequency,
                           sizeof(current_dram_frequency));

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));

  // If crc_page_ is valid, ie checking content read back from file,
  // track src/dst memory addresses. Otherwise catagorize as general
  // mememory miscompare for CRC checking everywhere else.
  if (crc_page_ != -1) {
    int miscompare_byteoffset = static_cast<char*>(error->vbyteaddr) -
                                static_cast<char*>(page_recs_[crc_page_].dst);
    os_->error_diagnoser_->AddHDDMiscompareError(
        devicename_, crc_page_, miscompare_byteoffset,
        page_recs_[crc_page_].src, page_recs_[crc_page_].dst);
  } else {
    os_->error_diagnoser_->AddMiscompareError(
        dimm_string, reinterpret_cast<uint64>(error->vaddr), 1);
  }

  if (priority <= sat_->verbosity() &&
      sat_->ClaimDetailedErrorLog()) {
    logprintf(priority,
              "%s: miscompare on %s at %p(0x%llx:%s): read:0x%016llx, "
              "reread:0x%016llx, expected:0x%016llx. '%s'; %s, %s, "
              "cur_mode:%s, cur_freq:%s, ddr_freq(%s).\n",
              message,
              devicename_.c_str(),
              error->vaddr,
              error->paddr,
              dimm_string,
              error->actual,
              error->reread,
              error->expected,
              (error->patternname) ? error->patternname : "None",
              (error->reread == error->expected) ?
                  "read error" : "write error",
              dram_coordinates,
              sat_->dram_frequency_mode(),
              current_dram_frequency,
              dram_frequencies);
  }

  // 재사용되는 작업 단위에서 같은 mismatch가 반복 집계되지 않도록
  // 해당 64-bit 위치에 expected 값을 기록합니다.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
  if (sat_->stop_on_error())
    sat_->RequestErrorStop();
}


// Checksum mismatch가 발생한 구간을 64-bit 단위로 상세 비교합니다.
// 각 오류 record에 Worker 단계, SAT 작업 단위 offset과 주파수 값을 저장합니다.
int WorkerThread::CheckRegion(void *addr,
                              class Pattern *pattern,
                              uint32 lastcpu,
                              int64 length,
                              int offset,
                              int64 pattern_offset,
                              int write_dram_frequency,
                              const char *worker_name,
                              const char *phase,
                              uint64 sat_page_offset) {
  uint64 *memblock = static_cast<uint64*>(addr);
  const int kErrorLimit = 128;
  int errors = 0;
  int overflowerrors = 0;  // Count of overflowed errors.
  bool page_error = false;
  int overflow_start_word = -1;
  const DiagnosticPhase diagnostic_phase =
      diagnostic_phase_stats_
          ? GetDiagnosticPhase(worker_name, phase)
          : DIAG_PHASE_COUNT;
  string errormessage("Hardware Error");
  struct ErrorRecord
    recorded[kErrorLimit];  // Queued errors for later printing.

  // For each word in the data region.
  for (int i = 0; i < length / wordsize_; i++) {
    int read_dram_frequency = sat_->current_dram_frequency();
    uint64 actual = memblock[i];
    uint64 expected;

    // Determine the value that should be there.
    datacast_t data;
    int index = 2 * i + pattern_offset;
    data.l32.l = pattern->pattern(index);
    data.l32.h = pattern->pattern(index + 1);
    expected = data.l64;
    // Check tags if necessary.
    if (tag_mode_ && ((reinterpret_cast<uint64>(&memblock[i]) & 0x3f) == 0)) {
      expected = addr_to_tag(&memblock[i]);
    }


    // If the value is incorrect, save an error record for later printing.
    if (actual != expected) {
      RecordDiagnosticFirstError(diagnostic_phase);
      if (errors < kErrorLimit) {
        recorded[errors].actual = actual;
        recorded[errors].expected = expected;
        recorded[errors].vaddr = &memblock[i];
        recorded[errors].patternname = pattern->name();
        recorded[errors].worker_name = worker_name;
        recorded[errors].phase = phase;
        recorded[errors].pattern_byte_offset = pattern->byte_offset();
        recorded[errors].sat_page_offset = sat_page_offset;
        recorded[errors].offset_in_page = offset + i * wordsize_;
        recorded[errors].lastcpu = lastcpu;
        recorded[errors].write_dram_frequency = write_dram_frequency;
        recorded[errors].read_dram_frequency = read_dram_frequency;
        errors++;
      } else {
        page_error = true;
        // 앞에서 보관한 128개 다음의 첫 mismatch 위치입니다. Overflow
        // 상세 비교는 이 위치부터 다시 시작하여 같은 word를 두 번 세지 않습니다.
        overflow_start_word = i;
        // If we have overflowed the error queue, just print the errors now.
        logprintf(10, "Log: Error record overflow, too many miscompares!\n");
        errormessage = "Page Error";
        break;
      }
    }
  }

  // Find if this is a whole block corruption.
  if (page_error && !tag_mode_) {
    int patsize = patternlist_->Size();
    for (int pat = 0; pat < patsize; pat++) {
      class Pattern *altpattern = patternlist_->GetPattern(pat);
      const int kGood = 0;
      const int kBad = 1;
      const int kGoodAgain = 2;
      const int kNoMatch = 3;
      int state = kGood;
      unsigned int badstart = 0;
      unsigned int badend = 0;

      // Don't match against ourself!
      if (pattern == altpattern)
        continue;

      for (int i = 0; i < length / wordsize_; i++) {
        uint64 actual = memblock[i];
        datacast_t expected;
        datacast_t possible;

        // Determine the value that should be there.
        int index = 2 * i + pattern_offset;

        expected.l32.l = pattern->pattern(index);
        expected.l32.h = pattern->pattern(index + 1);

        // 현재 expected Pattern과 목록의 다른 Pattern을 비교하여 연속된
        // 대체 Pattern 구간을 찾습니다.
        possible.l32.l = altpattern->pattern(index);
        possible.l32.h = altpattern->pattern(index + 1);

        if (state == kGood) {
          if (actual == expected.l64) {
            continue;
          } else if (actual == possible.l64) {
            badstart = i;
            badend = i;
            state = kBad;
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        } else if (state == kBad) {
          if (actual == possible.l64) {
            badend = i;
            continue;
          } else if (actual == expected.l64) {
            state = kGoodAgain;
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        } else if (state == kGoodAgain) {
          if (actual == expected.l64) {
            continue;
          } else {
            state = kNoMatch;
            break;
          }
        }
      }

      if ((state == kGoodAgain) || (state == kBad)) {
        unsigned int blockerrors = badend - badstart + 1;
        errormessage = "Block Error";
        // 아래 오류 queue 처리에서 각 mismatch를 한 번씩 출력하고 복구합니다.
        // 여기서는 대체 Pattern으로 일치한 연속 범위만 요약합니다.
        logprintf(0, "Block Error: (%p) pattern %s instead of %s, "
                  "%d bytes from offset 0x%x to 0x%x\n",
                  &memblock[badstart],
                  altpattern->name(), pattern->name(),
                  blockerrors * wordsize_,
                  offset + badstart * wordsize_,
                  offset + badend * wordsize_);
      }
    }
  }


  // Process error queue after all errors have been recorded.
  for (int err = 0; err < errors; err++) {
    int priority = 5;
    if (errorcount_ + err < 30)
      priority = 0;  // Bump up the priority for the first few errors.
    ProcessError(&recorded[err], priority, errormessage.c_str());
  }

  if (page_error) {
    // 앞에서 보관하지 못한 첫 mismatch부터 나머지 word를 처리합니다.
    // 0부터 재검사하면 앞의 128개가 오류 수와 로그에 중복 반영됩니다.
    sat_assert(overflow_start_word >= 0);
    for (int i = overflow_start_word; i < length / wordsize_; i++) {
      int read_dram_frequency = sat_->current_dram_frequency();
      uint64 actual = memblock[i];
      uint64 expected;
      datacast_t data;
      // Determine the value that should be there.
      int index = 2 * i + pattern_offset;

      data.l32.l = pattern->pattern(index);
      data.l32.h = pattern->pattern(index + 1);
      expected = data.l64;

      // Check tags if necessary.
      if (tag_mode_ && ((reinterpret_cast<uint64>(&memblock[i]) & 0x3f) == 0)) {
        expected = addr_to_tag(&memblock[i]);
      }

      // If the value is incorrect, save an error record for later printing.
      if (actual != expected) {
        // If we have overflowed the error queue, print the errors now.
        struct ErrorRecord er;
        er.actual = actual;
        er.expected = expected;
        er.vaddr = &memblock[i];
        er.patternname = pattern->name();
        er.worker_name = worker_name;
        er.phase = phase;
        er.pattern_byte_offset = pattern->byte_offset();
        er.sat_page_offset = sat_page_offset;
        er.offset_in_page = offset + i * wordsize_;
        er.lastcpu = lastcpu;
        er.write_dram_frequency = write_dram_frequency;
        er.read_dram_frequency = read_dram_frequency;

        // Do the error printout. This will take a long time and
        // likely change the machine state.
        ProcessError(&er, 12, errormessage.c_str());
        overflowerrors++;
      }
    }
  }

  // Keep track of observed errors.
  errorcount_ += errors + overflowerrors;
  return errors + overflowerrors;
}

float WorkerThread::GetCopiedData() {
  return pages_copied_ * sat_->page_length() / kMegabyte;
}

// SAT 작업 단위의 완전한 4 KiB 구간은 checksum으로 검사합니다. 남는 구간은
// CheckRegion()에서 64-bit 단위로 직접 비교합니다.
int WorkerThread::CrcCheckPage(struct page_entry *srcpe,
                               const char *worker_name,
                               const char *phase) {
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int errors = 0;

  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();
  const DiagnosticPhase diagnostic_phase =
      diagnostic_phase_stats_
          ? GetDiagnosticPhase(worker_name, phase)
          : DIAG_PHASE_COUNT;
  uint64 *memblock = static_cast<uint64*>(srcpe->addr);
  int blocks = sat_->page_length() / blocksize;
  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *memslice = memblock + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrCrcC(memslice, blocksize, &crc, srcpe);
    } else {
      CalculateAdlerChecksum(memslice, blocksize, &crc);
    }

    // Checksum이 다르면 64-bit 단위 상세 비교를 수행합니다.
    if (!crc.Equals(*expectedcrc)) {
      RecordDiagnosticChecksumMismatch(diagnostic_phase);
      logprintf(11, "Log: CrcCheckPage Falling through to slow compare, "
                "CRC mismatch %s != %s worker=%s phase=%s\n",
                crc.ToHexString().c_str(),
                expectedcrc->ToHexString().c_str(), worker_name, phase);
      int errorcount = CheckRegion(memslice,
                                   srcpe->pattern,
                                   srcpe->lastcpu,
                                   blocksize,
                                   currentblock * blocksize, 0,
                                   srcpe->write_dram_frequency,
                                   worker_name, phase, srcpe->offset);
      if (errorcount == 0) {
        logprintf(0, "Log: CrcCheckPage CRC mismatch %s != %s, "
                     "but no miscompares found. worker=%s phase=%s\n",
                  crc.ToHexString().c_str(),
                  expectedcrc->ToHexString().c_str(), worker_name, phase);
      }
      errors += errorcount;
    }
  }

  // 4 KiB 단위 처리 후 남은 구간은 64-bit 단위로 직접 검사합니다.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *memslice = memblock + blocks * blockwords;
    errors += CheckRegion(memslice,
                          srcpe->pattern,
                          srcpe->lastcpu,
                          leftovers,
                          blocks * blocksize, 0,
                          srcpe->write_dram_frequency,
                          worker_name, phase, srcpe->offset);
  }
  return errors;
}


// Tag mismatch를 reread하고 데이터 주소와 tag 주소 정보를 함께 출력합니다.
// 로그의 read error/write error는 actual·reread·expected 관계로 만든
// 소프트웨어 분류이며 실제 DRAM read/write 원인을 확정하지 않습니다.
void WorkerThread::ProcessTagError(struct ErrorRecord *error,
                                   int priority,
                                   const char *message) {
  char dimm_string[256] = "";
  char tag_dimm_string[256] = "";
  char dram_frequencies[128];
  char current_dram_frequency[32];
  char dram_coordinates[192];
  bool read_error = false;

  int core_id = sched_getcpu();

  // 아키텍처별 Flush() 호출 뒤 같은 가상 주소를 다시 읽습니다.
  os_->Flush(error->vaddr);
  error->reread_dram_frequency = sat_->current_dram_frequency();
  error->reread = *(error->vaddr);
  FormatDramFrequencies(error, dram_frequencies, sizeof(dram_frequencies));

  // 첫 read와 reread의 관계를 로그 분류에 사용합니다.
  if (error->actual != error->reread) {
    read_error = true;
  }

  sat_assert(error->expected != error->actual);

  error->vbyteaddr = reinterpret_cast<char*>(error->vaddr);

  // Find physical address if possible.
  error->paddr = os_->VirtualToPhysical(error->vbyteaddr);
  error->tagpaddr = os_->VirtualToPhysical(error->tagvaddr);
  FormatDramCoordinates(sat_, error->paddr,
                        dram_coordinates, sizeof(dram_coordinates));
  FormatDramFrequencyValue(error->reread_dram_frequency,
                           current_dram_frequency,
                           sizeof(current_dram_frequency));

  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->paddr, dimm_string, sizeof(dimm_string));
  // Pretty print DIMM mapping if available.
  os_->FindDimm(error->tagpaddr, tag_dimm_string, sizeof(tag_dimm_string));

  // Report parseable error.
  if (priority < 5 && priority <= sat_->verbosity() &&
      sat_->ClaimDetailedErrorLog()) {
    logprintf(priority,
              "%s: Tag from %p(0x%llx:%s) (%s) "
              "miscompare on CPU %d(0x%s) at %p(0x%llx:%s): "
              "read:0x%016llx, reread:0x%016llx, expected:0x%016llx. "
              "%s, %s, cur_mode:%s, cur_freq:%s, ddr_freq(%s).\n",
              message,
              error->tagvaddr, error->tagpaddr,
              tag_dimm_string,
              read_error ? "read error" : "write error",
              core_id,
              CurrentCpusFormat().c_str(),
              error->vaddr,
              error->paddr,
              dimm_string,
              error->actual,
              error->reread,
              error->expected,
              read_error ? "read error" : "write error",
              dram_coordinates,
              sat_->dram_frequency_mode(),
              current_dram_frequency,
              dram_frequencies);
  }

  errorcount_ += 1;

  // Overwrite incorrect data with correct data to prevent
  // future miscompares when this data is reused.
  *(error->vaddr) = error->expected;
  os_->Flush(error->vaddr);
  if (sat_->stop_on_error())
    sat_->RequestErrorStop();
}


// Print out and log a tag error.
bool WorkerThread::ReportTagError(
    uint64 *mem64,
    uint64 actual,
    uint64 tag,
    int write_dram_frequency,
    int read_dram_frequency) {
  struct ErrorRecord er;
  er.actual = actual;

  er.expected = tag;
  er.vaddr = mem64;
  er.write_dram_frequency = write_dram_frequency;
  er.read_dram_frequency = read_dram_frequency;

  // Generate vaddr from tag.
  er.tagvaddr = reinterpret_cast<uint64*>(actual);

  ProcessTagError(&er, 0, "Hardware Error");
  return true;
}

// C implementation of Adler memory copy, with memory tagging.
bool WorkerThread::AdlerAddrMemcpyC(uint64 *dstmem64,
                                    uint64 *srcmem64,
                                    unsigned int size_in_bytes,
                                    AdlerChecksum *checksum,
                                    struct page_entry *pe) {
  // Use this data wrapper to access memory with 64bit read/write.
  datacast_t data;
  datacast_t dstdata;
  unsigned int count = size_in_bytes / sizeof(data);

  if (count > ((1U) << 19)) {
    // Size is too large, must be strictly less than 512 KB.
    return false;
  }

  uint64 a1 = 1;
  uint64 a2 = 1;
  uint64 b1 = 0;
  uint64 b2 = 0;

  class Pattern *pattern = pe->pattern;

  unsigned int i = 0;
  while (i < count) {
    // Process 64 bits at a time.
    if ((i & 0x7) == 0) {
      int src_read_frequency = sat_->current_dram_frequency();
      data.l64 = srcmem64[i];
      int dst_read_frequency = sat_->current_dram_frequency();
      dstdata.l64 = dstmem64[i];
      uint64 src_tag = addr_to_tag(&srcmem64[i]);
      uint64 dst_tag = addr_to_tag(&dstmem64[i]);
      // Detect if tags have been corrupted.
      if (data.l64 != src_tag)
        ReportTagError(&srcmem64[i], data.l64, src_tag,
                       pe->write_dram_frequency, src_read_frequency);
      if (dstdata.l64 != dst_tag)
        ReportTagError(&dstmem64[i], dstdata.l64, dst_tag,
                       -1, dst_read_frequency);

      data.l32.l = pattern->pattern(i << 1);
      data.l32.h = pattern->pattern((i << 1) + 1);
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;

      data.l64  = dst_tag;
      dstmem64[i] = data.l64;

    } else {
      data.l64 = srcmem64[i];
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
      dstmem64[i] = data.l64;
    }
    i++;

    data.l64 = srcmem64[i];
    a2 = a2 + data.l32.l;
    b2 = b2 + a2;
    a2 = a2 + data.l32.h;
    b2 = b2 + a2;
    dstmem64[i] = data.l64;
    i++;
  }
  checksum->Set(a1, a2, b1, b2);
  return true;
}

// x86_64 SSE2 assembly implementation of Adler memory copy, with address
// tagging added as a second step. This is useful for debugging failures
// that only occur when SSE / nontemporal writes are used.
bool WorkerThread::AdlerAddrMemcpyWarm(uint64 *dstmem64,
                                       uint64 *srcmem64,
                                       unsigned int size_in_bytes,
                                       AdlerChecksum *checksum,
                                       struct page_entry *pe) {
  // Do ASM copy, ignore checksum.
  AdlerChecksum ignored_checksum;
  os_->AdlerMemcpyWarm(dstmem64, srcmem64, size_in_bytes, &ignored_checksum);

  // Force cache flush of both the source and destination addresses.
  //  length - length of block to flush in cachelines.
  //  mem_increment - number of dstmem/srcmem values per cacheline.
  int length = size_in_bytes / kCacheLineSize;
  int mem_increment = kCacheLineSize / sizeof(*dstmem64);
  OsLayer::FastFlushSync();
  for (int i = 0; i < length; ++i) {
    OsLayer::FastFlushHint(dstmem64 + (i * mem_increment));
    OsLayer::FastFlushHint(srcmem64 + (i * mem_increment));
  }
  OsLayer::FastFlushSync();

  // Check results.
  AdlerAddrCrcC(srcmem64, size_in_bytes, checksum, pe);
  // Patch up address tags.
  TagAddrC(dstmem64, size_in_bytes);
  return true;
}

// Retag pages..
bool WorkerThread::TagAddrC(uint64 *memwords,
                            unsigned int size_in_bytes) {
  // Mask is the bitmask of indexes used by the pattern.
  // It is the pattern size -1. Size is always a power of 2.

  // Select tag or data as appropriate.
  int length = size_in_bytes / wordsize_;
  for (int i = 0; i < length; i += 8) {
    datacast_t data;
    data.l64 = addr_to_tag(&memwords[i]);
    memwords[i] = data.l64;
  }
  return true;
}

// C implementation of Adler memory crc.
bool WorkerThread::AdlerAddrCrcC(uint64 *srcmem64,
                                 unsigned int size_in_bytes,
                                 AdlerChecksum *checksum,
                                 struct page_entry *pe) {
  // Use this data wrapper to access memory with 64bit read/write.
  datacast_t data;
  unsigned int count = size_in_bytes / sizeof(data);

  if (count > ((1U) << 19)) {
    // Size is too large, must be strictly less than 512 KB.
    return false;
  }

  uint64 a1 = 1;
  uint64 a2 = 1;
  uint64 b1 = 0;
  uint64 b2 = 0;

  class Pattern *pattern = pe->pattern;

  unsigned int i = 0;
  while (i < count) {
    // Process 64 bits at a time.
    if ((i & 0x7) == 0) {
      int read_dram_frequency = sat_->current_dram_frequency();
      data.l64 = srcmem64[i];
      uint64 src_tag = addr_to_tag(&srcmem64[i]);
      // Check that tags match expected.
      if (data.l64 != src_tag)
        ReportTagError(&srcmem64[i], data.l64, src_tag,
                       pe->write_dram_frequency, read_dram_frequency);

      data.l32.l = pattern->pattern(i << 1);
      data.l32.h = pattern->pattern((i << 1) + 1);
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
    } else {
      data.l64 = srcmem64[i];
      a1 = a1 + data.l32.l;
      b1 = b1 + a1;
      a1 = a1 + data.l32.h;
      b1 = b1 + a1;
    }
    i++;

    data.l64 = srcmem64[i];
    a2 = a2 + data.l32.l;
    b2 = b2 + a2;
    a2 = a2 + data.l32.h;
    b2 = b2 + a2;
    i++;
  }
  checksum->Set(a1, a2, b1, b2);
  return true;
}

// Source의 완전한 4 KiB 구간은 checksum을 계산하면서 destination에
// 복사합니다. 남는 구간은 직접 비교한 뒤 복사합니다. Checksum mismatch 후
// 상세 비교에서 오류 위치를 찾지 못하면 Tag mode가 아닐 때 복사된 데이터를
// source에 다시 기록하여 재검사합니다. 오류가 남으면 destination을 expected
// Pattern으로 다시 채우므로 이 경로에서는 추가 read·write가 발생합니다.
int WorkerThread::CrcCopyPage(struct page_entry *dstpe,
                              struct page_entry *srcpe,
                              const char *worker_name,
                              const char *phase) {
  int errors = 0;
  int destination_write_frequency = sat_->current_dram_frequency();
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Source와 destination의 작업 단위 시작 주소입니다.
  uint64 *targetmembase = static_cast<uint64*>(dstpe->addr);
  uint64 *sourcemembase = static_cast<uint64*>(srcpe->addr);
  // Source Pattern의 사전 계산 checksum을 사용합니다.
  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();
  const DiagnosticPhase diagnostic_phase =
      diagnostic_phase_stats_
          ? GetDiagnosticPhase(worker_name, phase)
          : DIAG_PHASE_COUNT;

  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *targetmem = targetmembase + currentblock * blockwords;
    uint64 *sourcemem = sourcemembase + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrMemcpyC(targetmem, sourcemem, blocksize, &crc, srcpe);
    } else {
      AdlerMemcpyC(targetmem, sourcemem, blocksize, &crc);
    }

    // Source checksum이 다르면 64-bit 단위 상세 비교를 수행합니다.
    if (!crc.Equals(*expectedcrc)) {
      RecordDiagnosticChecksumMismatch(diagnostic_phase);
      logprintf(11, "Log: CrcCopyPage Falling through to slow compare, "
                "CRC mismatch %s != %s worker=%s phase=%s\n",
                crc.ToHexString().c_str(),
                expectedcrc->ToHexString().c_str(), worker_name, phase);
      int errorcount = CheckRegion(sourcemem,
                                   srcpe->pattern,
                                   srcpe->lastcpu,
                                   blocksize,
                                   currentblock * blocksize, 0,
                                   srcpe->write_dram_frequency,
                                   worker_name, phase, srcpe->offset);
      if (errorcount == 0) {
        logprintf(0, "Log: CrcCopyPage CRC mismatch %s != %s, "
                     "but no miscompares found. Retrying with fresh data. "
                     "worker=%s phase=%s\n",
                  crc.ToHexString().c_str(),
                  expectedcrc->ToHexString().c_str(), worker_name, phase);
        if (!tag_mode_) {
          // 첫 checksum 계산에서 destination으로 복사한 데이터를 source에
          // 다시 기록한 뒤 동일 구간을 한 번 더 비교합니다.
          int repair_write_frequency = sat_->current_dram_frequency();
          memcpy(sourcemem, targetmem, blocksize);
          errorcount = CheckRegion(sourcemem,
                                   srcpe->pattern,
                                   srcpe->lastcpu,
                                   blocksize,
                                   currentblock * blocksize, 0,
                                   repair_write_frequency,
                                   worker_name, phase, srcpe->offset);
          if (errorcount == 0) {
            int core_id = sched_getcpu();
            logprintf(0, "Process Error: CPU %d(0x%s) CrcCopyPage "
                         "CRC mismatch %s != %s, "
                         "but no miscompares found on second pass.\n",
                      core_id, CurrentCpusFormat().c_str(),
                      crc.ToHexString().c_str(),
                      expectedcrc->ToHexString().c_str());
            struct ErrorRecord er;
            er.write_dram_frequency = repair_write_frequency;
            er.read_dram_frequency = sat_->current_dram_frequency();
            er.actual = sourcemem[0];
            er.expected = 0xbad00000ull << 32;
            er.vaddr = sourcemem;
            er.lastcpu = srcpe->lastcpu;
            logprintf(0, "Process Error: lastCPU %d\n", srcpe->lastcpu);
            er.patternname = srcpe->pattern->name();
            er.worker_name = worker_name;
            er.phase = phase;
            er.pattern_byte_offset = srcpe->pattern->byte_offset();
            er.sat_page_offset = srcpe->offset;
            er.offset_in_page = currentblock * blocksize;
            ProcessError(&er, 0, "Hardware Error");
            errors += 1;
            errorcount_ ++;
          }
        }
      }
      errors += errorcount;
    }
  }

  // 4 KiB 단위 처리 후 남은 구간을 검사하고 복사합니다.
  // Tag mode에서는 source 가상 주소 tag를 destination에 사용할 수
  // 없으므로 destination 주소를 기준으로 tag를 다시 생성합니다.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *targetmem = targetmembase + blocks * blockwords;
    uint64 *sourcemem = sourcemembase + blocks * blockwords;

    errors += CheckRegion(sourcemem,
                          srcpe->pattern,
                          srcpe->lastcpu,
                          leftovers,
                          blocks * blocksize, 0,
                          srcpe->write_dram_frequency,
                          worker_name, phase, srcpe->offset);
    memcpy(targetmem, sourcemem, leftovers);
    if (tag_mode_)
      TagAddrC(targetmem, leftovers);
  }

  // Destination의 데이터 상태와 마지막 write 정보를 갱신합니다.
  dstpe->pattern = srcpe->pattern;
  dstpe->lastcpu = sched_getcpu();
  dstpe->write_dram_frequency = destination_write_frequency;

  // Source 오류를 검출한 경우 destination 전체를 expected Pattern으로 채웁니다.
  if (errors) {
    // TODO(nsanders): Maybe we should patch rather than fill? Filling may
    // cause bad data to be propogated across the page.
    FillPage(dstpe);
  }
  return errors;
}



// SAT 작업 단위를 높은 주소에서 낮은 주소 방향으로 순회하며 각 32-bit 값을
// 읽고 반전한 뒤 같은 주소에 저장합니다. 한 cache line마다 FastFlushHint()를
// 호출하고 반전 pass 시작 시 저장된 마지막 성공 주파수 요청값을 기록합니다.
// AArch64의 FastFlushHint()는 `dc cvau`로 PoU까지 clean하며 D-cache invalidate나
// LPDDR 직접 write 완료를 보장하지 않습니다. 실제 처리 범위는
// --invert-range 설정으로 결정합니다.
int InvertThread::InvertPageDown(struct page_entry *srcpe) {
  int write_dram_frequency = sat_->current_dram_frequency();
  const int invert_flush_interval = kCacheLineSize / sizeof(unsigned int);
  const int64 words =
      sat_->invert_range_bytes() / sizeof(unsigned int);

  OsLayer::FastFlushSync();
  bool inverted = InvertWordsDown(
      static_cast<unsigned int *>(srcpe->addr), words,
      invert_flush_interval, InvertFlushHintAdapter);
  OsLayer::FastFlushSync();
  sat_assert(inverted);
  srcpe->lastcpu = sched_getcpu();
  srcpe->write_dram_frequency = write_dram_frequency;
  return 0;
}

// SAT 작업 단위를 낮은 주소에서 높은 주소 방향으로 순회하며 각 32-bit 값을
// 읽고 반전한 뒤 같은 주소에 저장합니다. 한 cache line마다 FastFlushHint()를
// 호출하고 반전 pass 시작 시 저장된 마지막 성공 주파수 요청값을 기록합니다.
// Full은 `-p` 전체를 처리하고 Legacy는 upstream의 pointer 단위 계산을
// 보존합니다. AArch64의 FastFlushHint()는 `dc cvau`로 PoU까지 clean하며
// D-cache invalidate나 LPDDR 직접 write 완료를 보장하지 않습니다.
int InvertThread::InvertPageUp(struct page_entry *srcpe) {
  int write_dram_frequency = sat_->current_dram_frequency();
  const int invert_flush_interval = kCacheLineSize / sizeof(unsigned int);
  const int64 words =
      sat_->invert_range_bytes() / sizeof(unsigned int);

  OsLayer::FastFlushSync();
  bool inverted = InvertWordsUp(
      static_cast<unsigned int *>(srcpe->addr), words,
      invert_flush_interval, InvertFlushHintAdapter);
  OsLayer::FastFlushSync();
  sat_assert(inverted);

  srcpe->lastcpu = sched_getcpu();
  srcpe->write_dram_frequency = write_dram_frequency;
  return 0;
}

// 한 Invert 반복은 선택 범위를 네 번 읽고 네 번 다시 기록합니다.
// Legacy 범위에서도 통계가 전체 SAT 작업 단위를 처리한 것으로 과대 계산되지
// 않도록 실제 선택 범위를 기준으로 MiB를 계산합니다.
float InvertThread::GetMemoryCopiedData() {
  return bytes_processed_ / static_cast<float>(kMegabyte);
}

// Warm copy 경로로 source checksum을 계산하면서 destination에 복사합니다.
// Checksum mismatch 후 상세 비교에서 오류 위치를 찾지 못하면 Tag mode가
// 아닐 때 복사된 데이터를 source에 다시 기록하여 재검사합니다. 오류가 남으면
// destination을 expected Pattern으로 다시 채우므로 이 경로에서는 추가
// read·write가 발생합니다.
int WorkerThread::CrcWarmCopyPage(struct page_entry *dstpe,
                                  struct page_entry *srcpe,
                                  const char *worker_name,
                                  const char *phase) {
  int errors = 0;
  int destination_write_frequency = sat_->current_dram_frequency();
  const int blocksize = 4096;
  const int blockwords = blocksize / wordsize_;
  int blocks = sat_->page_length() / blocksize;

  // Source와 destination의 작업 단위 시작 주소입니다.
  uint64 *targetmembase = static_cast<uint64*>(dstpe->addr);
  uint64 *sourcemembase = static_cast<uint64*>(srcpe->addr);
  // Source Pattern의 사전 계산 checksum을 사용합니다.
  const AdlerChecksum *expectedcrc = srcpe->pattern->crc();
  const DiagnosticPhase diagnostic_phase =
      diagnostic_phase_stats_
          ? GetDiagnosticPhase(worker_name, phase)
          : DIAG_PHASE_COUNT;

  for (int currentblock = 0; currentblock < blocks; currentblock++) {
    uint64 *targetmem = targetmembase + currentblock * blockwords;
    uint64 *sourcemem = sourcemembase + currentblock * blockwords;

    AdlerChecksum crc;
    if (tag_mode_) {
      AdlerAddrMemcpyWarm(targetmem, sourcemem, blocksize, &crc, srcpe);
    } else {
      os_->AdlerMemcpyWarm(targetmem, sourcemem, blocksize, &crc);
    }

    // Source checksum이 다르면 64-bit 단위 상세 비교를 수행합니다.
    if (!crc.Equals(*expectedcrc)) {
      RecordDiagnosticChecksumMismatch(diagnostic_phase);
      logprintf(11, "Log: CrcWarmCopyPage Falling through to slow compare, "
                "CRC mismatch %s != %s worker=%s phase=%s\n",
                crc.ToHexString().c_str(),
                expectedcrc->ToHexString().c_str(), worker_name, phase);
      int errorcount = CheckRegion(sourcemem,
                                   srcpe->pattern,
                                   srcpe->lastcpu,
                                   blocksize,
                                   currentblock * blocksize, 0,
                                   srcpe->write_dram_frequency,
                                   worker_name, phase, srcpe->offset);
      if (errorcount == 0) {
        logprintf(0, "Log: CrcWarmCopyPage CRC mismatch expected: %s != actual: %s, "
                     "but no miscompares found. Retrying with fresh data. "
                     "worker=%s phase=%s\n",
                  expectedcrc->ToHexString().c_str(),
                  crc.ToHexString().c_str(), worker_name, phase);
        if (!tag_mode_) {
          // 첫 checksum 계산에서 destination으로 복사한 데이터를 source에
          // 다시 기록한 뒤 동일 구간을 한 번 더 비교합니다.
          int repair_write_frequency = sat_->current_dram_frequency();
          memcpy(sourcemem, targetmem, blocksize);
          errorcount = CheckRegion(sourcemem,
                                   srcpe->pattern,
                                   srcpe->lastcpu,
                                   blocksize,
                                   currentblock * blocksize, 0,
                                   repair_write_frequency,
                                   worker_name, phase, srcpe->offset);
          if (errorcount == 0) {
            int core_id = sched_getcpu();
            logprintf(0, "Process Error: CPU %d(0x%s) CrciWarmCopyPage "
                         "CRC mismatch %s != %s, "
                         "but no miscompares found on second pass.\n",
                      core_id, CurrentCpusFormat().c_str(),
                      crc.ToHexString().c_str(),
                      expectedcrc->ToHexString().c_str());
            struct ErrorRecord er;
            er.write_dram_frequency = repair_write_frequency;
            er.read_dram_frequency = sat_->current_dram_frequency();
            er.actual = sourcemem[0];
            er.expected = 0xbad;
            er.vaddr = sourcemem;
            er.lastcpu = srcpe->lastcpu;
            er.patternname = srcpe->pattern->name();
            er.worker_name = worker_name;
            er.phase = phase;
            er.pattern_byte_offset = srcpe->pattern->byte_offset();
            er.sat_page_offset = srcpe->offset;
            er.offset_in_page = currentblock * blocksize;
            ProcessError(&er, 0, "Hardware Error");
            errors ++;
            errorcount_ ++;
          }
        }
      }
      errors += errorcount;
    }
  }

  // 4 KiB 단위 처리 후 남은 구간을 검사하고 복사합니다.
  // Warm Copy의 Tag mode도 destination 주소를 기준으로 tag를
  // 다시 생성하여 source 주소 tag가 남지 않도록 처리합니다.
  int leftovers = sat_->page_length() % blocksize;
  if (leftovers) {
    uint64 *targetmem = targetmembase + blocks * blockwords;
    uint64 *sourcemem = sourcemembase + blocks * blockwords;

    errors += CheckRegion(sourcemem,
                          srcpe->pattern,
                          srcpe->lastcpu,
                          leftovers,
                          blocks * blocksize, 0,
                          srcpe->write_dram_frequency,
                          worker_name, phase, srcpe->offset);
    memcpy(targetmem, sourcemem, leftovers);
    if (tag_mode_)
      TagAddrC(targetmem, leftovers);
  }

  // Destination의 데이터 상태와 마지막 write 정보를 갱신합니다.
  dstpe->pattern = srcpe->pattern;
  dstpe->lastcpu = sched_getcpu();
  dstpe->write_dram_frequency = destination_write_frequency;


  // Source 오류를 검출한 경우 destination 전체를 expected Pattern으로 채웁니다.
  if (errors) {
    // TODO(nsanders): Maybe we should patch rather than fill? Filling may
    // cause bad data to be propogated across the page.
    FillPage(dstpe);
  }
  return errors;
}



// Runtime 검사와 종료 검사는 동일한 checksum 경로를 사용합니다.
// 옵션 미지정 실행의 Runtime Check Worker는 기존 코드와 같이 STOP 이후
// Valid queue를 끝까지 검사하여 Empty로 옮깁니다. --skip-final-check 또는
// 명시적 --final-check-threads 실행에서는 보유 entry를 Valid로 반환하고
// 별도 종료 정책에 따라 다음 entry 선택을 중지합니다.
bool CheckThread::Work() {
  struct page_entry pe;
  bool result = true;
  int64 loops = 0;
  const bool final_check = (check_phase_ == "final_check");
  const bool legacy_final_check =
      !final_check && sat_->legacy_final_check_mode();
  bool draining = final_check;

  logprintf(9, "Log: Starting Check thread %d\n", thread_num_);

  while (true) {
    // --stop_on_errors가 설정되면 Runtime과 종료 검사 모두
    // 현재 entry 반환 후 새 entry를 가져오지 않습니다.
    if (sat_->error_stop_requested())
      break;

    // 분리형 종료 정책만 반복 시작 시 STOP을 확인합니다. 기존 경로는
    // 원본 코드와 같은 위치인 작업 단위 검사 뒤에서 상태를 확인합니다.
    bool runtime_running = true;
    if (!final_check && !legacy_final_check) {
      runtime_running = IsReadyToRunNoPause();
      if (!runtime_running)
        break;
    }

    // STOP 이후 기존 Runtime Check Worker가 수행하는 drain은 종료 검사
    // 단계로 집계합니다. STOP 전 시작한 한 작업 단위는 Runtime으로 남습니다.
    const char *operation_phase =
        draining ? "final_check" : check_phase_.c_str();

    result = result && sat_->GetValid(&pe);
    if (!result) {
      const bool unexpected_empty =
          !final_check &&
          (legacy_final_check ? IsReadyToRunNoPause() : runtime_running);
      if (unexpected_empty)
        logprintf(0, "Process Error: check_thread failed to pop pages, "
                  "bailing\n");
      else
        result = true;
      break;
    }

    // 선택한 작업 단위의 전체 checksum을 검사합니다.
    int check_errors =
        CrcCheckPage(&pe, "check", operation_phase);
    if (sat_->diag_phase_summary()) {
      RecordDiagnosticOperation(
          draining ? DIAG_PHASE_FINAL_CHECK : DIAG_PHASE_RUNTIME_CHECK,
          sat_->page_length(), 0, check_errors);
    }

    // Runtime이 계속되거나 별도 종료 정책을 사용하는 entry는 Valid로
    // 반환합니다. 기존 종료 drain과 별도 final Worker는 Empty로 옮깁니다.
    const bool stopped_after_check =
        !final_check && !IsReadyToRunNoPause();
    if (final_check ||
        (legacy_final_check && stopped_after_check &&
         !sat_->error_stop_requested())) {
      result = result && sat_->PutEmpty(&pe);
      if (legacy_final_check)
        draining = true;
    } else {
      result = result && sat_->PutValid(&pe);
    }
    if (!result) {
      logprintf(0, "Process Error: check_thread failed to push pages, "
                "bailing\n");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;
  logprintf(9, "Log: Completed %d: Check thread. Status %d, %d pages checked\n",
            thread_num_, status_, pages_copied_);
  return result;
}


// Runtime queue 구성 전에 초기 Pattern이 지정된 SAT 작업 단위를 논리 offset
// 순서로 순회합니다. Queue 상태와 random cursor를 변경하지 않으며 entry
// metadata 전체를 별도 vector에 보관하지 않습니다.
bool PostFillCheckThread::Work() {
  bool result = true;
  int64 checked_pages = 0;
  const bool coarse_queue = sat_->coarse_grain_queue();

  logprintf(5,
            "Log: DIAG phase=post_fill_check_begin pages=%lld thread=%d "
            "order=%s\n",
            pages_to_check_, thread_num_,
            coarse_queue ? "queue_random" : "logical_offset");

  for (int64 i = 0; i < pages_to_check_; ++i) {
    struct page_entry pe;
    uint64 page_offset = i * sat_->page_length();
    bool page_ready = coarse_queue
        ? sat_->GetValid(&pe)
        : sat_->GetValidByOffsetForInitialization(page_offset, &pe);
    if (!page_ready) {
      logprintf(0,
                "Process Error: post-fill check could not get page "
                "%lld/%lld requested_offset=0x%llx\n",
                i, pages_to_check_, page_offset);
      result = false;
      break;
    }
    int check_errors =
        CrcCheckPage(&pe, "post_fill", "full_check");
    if (sat_->diag_phase_summary()) {
      RecordDiagnosticOperation(
          DIAG_PHASE_POST_FILL_CHECK,
          sat_->page_length(), 0, check_errors);
    }

    if (coarse_queue) {
      if (!sat_->HoldInitializationPage(&pe)) {
        sat_->PutValid(&pe);
        logprintf(0,
                  "Process Error: post-fill check could not hold page\n");
        result = false;
        break;
      }
    } else {
      sat_->ReleaseInitializationPage(&pe);
    }
    checked_pages++;

    if (sat_->error_stop_requested())
      break;
  }

  if (coarse_queue && !sat_->RestoreInitializationPages(checked_pages)) {
    logprintf(0,
              "Process Error: post-fill check could not restore pages\n");
    result = false;
  }

  pages_copied_ = checked_pages;
  status_ = result;
  logprintf(result ? 5 : 0,
            "Log: DIAG phase=post_fill_check_end pages=%lld errors=%lld "
            "status=%d\n",
            checked_pages, errorcount_, status_);
  return result;
}


// Valid 원본과 Empty 대상을 하나씩 가져와 복사합니다. -W를 지정하면 warm
// checksum 경로가 우선합니다. -W가 없을 때 기본값은 일반 checksum 경로이며
// -F를 지정하면 memcpy 경로를 사용합니다. 대상은 Valid, 원본은 Empty가 됩니다.
bool CopyThread::Work() {
  struct page_entry src;
  struct page_entry dst;
  bool result = true;
  int64 loops = 0;

  logprintf(9, "Log: Starting copy thread %d: cpu %s, "
            "mem %x, warm: %d, has_vector: %d\n",
            thread_num_, cpuset_format(&cpu_mask_).c_str(), tag_,
            sat_->warm(), os_->has_vector());

  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // Valid source와 Empty destination을 각각 하나씩 확보합니다.
    if (!sat_->GetValid(&src, tag_)) {
      logprintf(0, "Process Error: copy_thread failed to pop pages, "
                "bailing\n");
      result = false;
      break;
    }
    if (!sat_->GetEmpty(&dst, tag_)) {
      // Destination 획득 실패 시 이미 확보한 source를 Valid 상태로
      // 반환하여 FineLock의 page lock과 OneLock의 entry를 보존합니다.
      if (!sat_->PutValid(&src))
        logprintf(0, "Process Error: copy_thread failed to return source\n");
      logprintf(0, "Process Error: copy_thread failed to pop pages, "
                "bailing\n");
      result = false;
      break;
    }

    // Force errors for unittests.
    if (sat_->error_injection()) {
      if ((random() % 50000) == 8) {
        char *addr = reinterpret_cast<char*>(src.addr);
        int offset = random() % sat_->page_length();
        addr[offset] = 0xba;
      }
    }

    // 옵션에 따라 warm checksum, 기본 checksum 또는 memcpy 경로를 선택합니다.
    // Copy 전체가 끝난 뒤 destination의 마지막 writer를 갱신합니다.
    const bool record_history = sat_->diag_block_history();
    uint64 frequency_epoch_begin =
        record_history ? sat_->dram_frequency_epoch() : 0;
    int copy_errors = 0;
    if (sat_->warm()) {
      copy_errors =
          CrcWarmCopyPage(&dst, &src, "copy", "source_check");
    } else if (sat_->strict()) {
      copy_errors = CrcCopyPage(&dst, &src, "copy", "source_check");
    } else {
      int destination_write_frequency = sat_->current_dram_frequency();
      memcpy(dst.addr, src.addr, sat_->page_length());
      // -F Copy에서도 destination 가상 주소를 기준으로
      // cache-line tag를 재생성하여 source tag 복사를 방지합니다.
      if (tag_mode_)
        TagAddrC(static_cast<uint64*>(dst.addr), sat_->page_length());
      dst.pattern = src.pattern;
      dst.lastcpu = sched_getcpu();
      dst.write_dram_frequency = destination_write_frequency;
    }
    if (record_history) {
      sat_->RecordBlockWrite(
          dst.offset,
          copy_errors ? Sat::BLOCK_WRITER_REPAIR : Sat::BLOCK_WRITER_COPY,
          thread_num_, sched_getcpu(),
          frequency_epoch_begin, sat_->dram_frequency_epoch());
    }
    if (sat_->diag_phase_summary()) {
      RecordDiagnosticOperation(
          DIAG_PHASE_COPY_TRANSFER,
          sat_->page_length(), sat_->page_length(), copy_errors);
    }

    // destination을 Valid queue에 반환하기 전에 선택형 readback 검사를
    // 수행합니다. 이 검사는 추가 read와 cache 상태 변화를 발생시킵니다.
    if (sat_->copy_verify_destination()) {
      int destination_errors =
          CrcCheckPage(&dst, "copy", "destination_check");
      if (sat_->diag_phase_summary()) {
        RecordDiagnosticOperation(
            DIAG_PHASE_COPY_DESTINATION_CHECK,
            sat_->page_length(), 0, destination_errors);
      }
    }

    // 첫 반환이 실패해도 두 번째 entry 반환을 시도하여 queue 누락을 막습니다.
    bool dst_returned = sat_->PutValid(&dst);
    bool src_returned = sat_->PutEmpty(&src);
    result = dst_returned && src_returned;

    // 작업 단위 복사가 끝난 지점에서 실행권을 양보합니다. 다른 Copy Worker가
    // 작업 단위 내부의 연속 복사 구간을 실행할 기회를 확보할 수 있습니다.
    YieldSelf();

    if (!result) {
      logprintf(0, "Process Error: copy_thread failed to push pages, "
                "bailing\n");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;
  logprintf(9, "Log: Completed %d: Copy thread. Status %d, %d pages copied\n",
            thread_num_, status_, pages_copied_);
  return result;
}

// Valid 작업 단위 하나를 선택하여 up·down·down·up 네 번의 범위 반전 저장을
// 수행합니다. Strict mode에서는 반전 전·후 checksum을 검사합니다.
bool InvertThread::Work() {
  struct page_entry src;
  bool result = true;
  int64 loops = 0;

  logprintf(9, "Log: Starting invert thread %d\n", thread_num_);

  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // 반전할 Valid 작업 단위를 하나 확보합니다.
    result = result && sat_->GetValid(&src);
    if (!result) {
      logprintf(0, "Process Error: invert_thread failed to pop pages, "
                "bailing\n");
      break;
    }

    // 첫 반전 전에 저장된 Pattern의 checksum을 확인합니다.
    if (sat_->strict()) {
      int precheck_errors =
          CrcCheckPage(&src, "invert", "precheck");
      if (sat_->diag_phase_summary()) {
        RecordDiagnosticOperation(
            DIAG_PHASE_INVERT_PRECHECK,
            sat_->page_length(), 0, precheck_errors);
      }
    }

    // 각 범위 반전 저장 뒤에 실행권을 양보하여 다른 Worker의 실행 기회를
    // 확보합니다. 다음 반전은 같은 작업 단위를 계속 사용합니다.
    const bool record_history = sat_->diag_block_history();
    uint64 frequency_epoch_begin =
        record_history ? sat_->dram_frequency_epoch() : 0;
    InvertPageUp(&src);
    YieldSelf();
    InvertPageDown(&src);
    YieldSelf();
    InvertPageDown(&src);
    YieldSelf();
    InvertPageUp(&src);
    YieldSelf();
    if (record_history) {
      sat_->RecordBlockWrite(
          src.offset, Sat::BLOCK_WRITER_INVERT,
          thread_num_, sched_getcpu(),
          frequency_epoch_begin, sat_->dram_frequency_epoch());
    }
    if (sat_->diag_phase_summary()) {
      RecordDiagnosticOperation(
          DIAG_PHASE_INVERT_RMW,
          sat_->invert_range_bytes() * 4,
          sat_->invert_range_bytes() * 4, 0);
    }

    // up·down·down·up 네 번의 선택 범위 반전 후 checksum을 확인합니다.
    // Legacy 범위 밖의 데이터는 네 pass 동안 변경되지 않습니다.
    if (sat_->strict()) {
      int postcheck_errors =
          CrcCheckPage(&src, "invert", "postcheck");
      if (sat_->diag_phase_summary()) {
        RecordDiagnosticOperation(
            DIAG_PHASE_INVERT_POSTCHECK,
            sat_->page_length(), 0, postcheck_errors);
      }
    }

    result = result && sat_->PutValid(&src);
    if (!result) {
      logprintf(0, "Process Error: invert_thread failed to push pages, "
                "bailing\n");
      break;
    }
    loops++;
  }

  pages_copied_ = loops * 2;
  bytes_processed_ = loops * sat_->invert_range_bytes() * 8;
  status_ = result;
  logprintf(9, "Log: Completed %d: Copy thread. Status %d, %d pages copied\n",
            thread_num_, status_, pages_copied_);
  return result;
}


// Set file name to use for File IO.
void FileThread::SetFile(const char *filename_init) {
  filename_ = filename_init;
  devicename_ = os_->FindFileDevice(filename_);
}

// Open the file for access.
bool FileThread::OpenFile(int *pfile) {
  int flags = O_RDWR | O_CREAT | O_SYNC;
  int fd = open(filename_.c_str(), flags | O_DIRECT, 0644);
  if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
    fd = open(filename_.c_str(), flags, 0644);  // Try without O_DIRECT
    os_->ActivateFlushPageCache();  // Not using O_DIRECT fixed EINVAL
  }
  if (fd < 0) {
    logprintf(0, "Process Error: Failed to create file %s!!\n",
              filename_.c_str());
    pages_copied_ = 0;
    return false;
  }
  *pfile = fd;
  return true;
}

// Close the file.
bool FileThread::CloseFile(int fd) {
  close(fd);
  return true;
}

// Check sector tagging.
bool FileThread::SectorTagPage(struct page_entry *src, int block) {
  int page_length = sat_->page_length();
  struct FileThread::SectorTag *tag =
    (struct FileThread::SectorTag *)(src->addr);

  // Tag each sector.
  unsigned char magic = ((0xba + thread_num_) & 0xff);
  for (int sec = 0; sec < page_length / 512; sec++) {
    tag[sec].magic = magic;
    tag[sec].block = block & 0xff;
    tag[sec].sector = sec & 0xff;
    tag[sec].pass = pass_ & 0xff;
  }
  return true;
}

bool FileThread::WritePageToFile(int fd, struct page_entry *src) {
  int page_length = sat_->page_length();
  // Fill the file with our data.
  int64 size = write(fd, src->addr, page_length);

  if (size != page_length) {
    os_->ErrorReport(devicename_.c_str(), "write-error", 1);
    errorcount_++;
    logprintf(0, "Block Error: file_thread failed to write, "
              "bailing\n");
    return false;
  }
  return true;
}

// Write the data to the file.
bool FileThread::WritePages(int fd) {
  int strict = sat_->strict();

  // Start fresh at beginning of file for each batch of pages.
  lseek(fd, 0, SEEK_SET);
  for (int i = 0; i < sat_->disk_pages(); i++) {
    struct page_entry src;
    if (!GetValidPage(&src))
      return false;
    // Save expected pattern.
    page_recs_[i].pattern = src.pattern;
    page_recs_[i].src = src.addr;

    // Check data correctness.
    if (strict)
      CrcCheckPage(&src, "file", "source_check");

    SectorTagPage(&src, i);

    bool result = WritePageToFile(fd, &src);

    if (!PutEmptyPage(&src))
      return false;

    if (!result)
      return false;
  }
  return os_->FlushPageCache();  // If O_DIRECT worked, this will be a NOP.
}

// Copy data from file into memory block.
bool FileThread::ReadPageFromFile(int fd, struct page_entry *dst) {
  int page_length = sat_->page_length();
  int write_dram_frequency = sat_->current_dram_frequency();

  // Do the actual read.
  int64 size = read(fd, dst->addr, page_length);
  if (size != page_length) {
    os_->ErrorReport(devicename_.c_str(), "read-error", 1);
    logprintf(0, "Block Error: file_thread failed to read, "
              "bailing\n");
    errorcount_++;
    return false;
  }
  dst->write_dram_frequency = write_dram_frequency;
  return true;
}

// Check sector tagging.
bool FileThread::SectorValidatePage(const struct PageRec &page,
                                    struct page_entry *dst, int block) {
  // Error injection.
  static int calls = 0;
  calls++;

  // Do sector tag compare.
  int firstsector = -1;
  int lastsector = -1;
  bool badsector = false;
  int page_length = sat_->page_length();

  // Cast data block into an array of tagged sectors.
  struct FileThread::SectorTag *tag =
  (struct FileThread::SectorTag *)(dst->addr);

  sat_assert(sizeof(*tag) == 512);

  // Error injection.
  if (sat_->error_injection()) {
    if (calls == 2) {
      for (int badsec = 8; badsec < 17; badsec++)
        tag[badsec].pass = 27;
    }
    if (calls == 18) {
      (static_cast<int32*>(dst->addr))[27] = 0xbadda7a;
    }
  }

  // Check each sector for the correct tag we added earlier,
  // then revert the tag to the to normal data pattern.
  unsigned char magic = ((0xba + thread_num_) & 0xff);
  for (int sec = 0; sec < page_length / 512; sec++) {
    // Check magic tag.
    if ((tag[sec].magic != magic) ||
        (tag[sec].block != (block & 0xff)) ||
        (tag[sec].sector != (sec & 0xff)) ||
        (tag[sec].pass != (pass_ & 0xff))) {
      // Offset calculation for tag location.
      int offset = sec * sizeof(SectorTag);
      if (tag[sec].block != (block & 0xff))
        offset += 1 * sizeof(uint8);
      else if (tag[sec].sector != (sec & 0xff))
        offset += 2 * sizeof(uint8);
      else if (tag[sec].pass != (pass_ & 0xff))
        offset += 3 * sizeof(uint8);

      // 같은 sector mismatch를 하나의 incident로 집계합니다. Diagnoser
      // 호출은 외부 보고를 수행하며 Worker 오류 수를 별도로 증가시키지 않습니다.
      os_->error_diagnoser_->AddHDDSectorTagError(devicename_, tag[sec].block,
                                                  offset,
                                                  tag[sec].sector,
                                                  page.src, page.dst);

      errorcount_ += 1;
      logprintf(5, "Sector Error: Sector tag @ 0x%x, pass %d/%d. "
                "sec %x/%x, block %d/%d, magic %x/%x, File: %s \n",
                block * page_length + 512 * sec,
                (pass_ & 0xff), (unsigned int)tag[sec].pass,
                sec, (unsigned int)tag[sec].sector,
                block, (unsigned int)tag[sec].block,
                magic, (unsigned int)tag[sec].magic,
                filename_.c_str());

      // Keep track of first and last bad sector.
      if (firstsector == -1)
        firstsector = (block * page_length / 512) + sec;
      lastsector = (block * page_length / 512) + sec;
      badsector = true;
    }
    // Patch tag back to proper pattern.
    unsigned int *addr = (unsigned int *)(&tag[sec]);
    *addr = dst->pattern->pattern(512 * sec / sizeof(*addr));
  }

  // If we found sector errors:
  if (badsector == true) {
    logprintf(5, "Log: file sector miscompare at offset %x-%x. File: %s\n",
              firstsector * 512,
              ((lastsector + 1) * 512) - 1,
              filename_.c_str());

    // 오류 데이터를 복구한 뒤 공통 종료 요청 경로를 사용합니다. Logger
    // queue와 다른 Worker가 정상 정리 절차를 완료할 수 있습니다.
    for (int block = (firstsector * 512) / page_length;
        block <= (lastsector * 512) / page_length;
        block++) {
      unsigned int *memblock = static_cast<unsigned int *>(dst->addr);
      int length = page_length / wordsize_;
      for (int i = 0; i < length; i++) {
        memblock[i] = dst->pattern->pattern(i);
      }
    }
    if (sat_->stop_on_error())
      sat_->RequestErrorStop();
  }
  return true;
}

// Get memory for an incoming data transfer..
bool FileThread::PagePrepare() {
  // We can only do direct IO to SAT pages if it is normal mem.
  page_io_ = os_->normal_mem();

  // Init a local buffer if we need it.
  if (!page_io_) {
#ifdef HAVE_POSIX_MEMALIGN
    int result = posix_memalign(&local_page_, 512, sat_->page_length());
#else
    local_page_ = memalign(512, sat_->page_length());
    int result = (local_page_ == 0);
#endif
    if (result) {
      logprintf(0, "Process Error: disk thread posix_memalign "
                   "returned %d (fail)\n",
                result);
      status_ = false;
      return false;
    }
  }
  return true;
}


// Remove memory allocated for data transfer.
bool FileThread::PageTeardown() {
  // Free a local buffer if we need to.
  if (!page_io_) {
    free(local_page_);
  }
  return true;
}



// Get memory for an incoming data transfer..
bool FileThread::GetEmptyPage(struct page_entry *dst) {
  if (page_io_) {
    if (!sat_->GetEmpty(dst))
      return false;
  } else {
    dst->addr = local_page_;
    dst->offset = 0;
    dst->pattern = 0;
    dst->lastcpu = 0;
    dst->write_dram_frequency = -1;
  }
  return true;
}

// Get memory for an outgoing data transfer..
bool FileThread::GetValidPage(struct page_entry *src) {
  struct page_entry tmp;
  if (!sat_->GetValid(&tmp))
    return false;
  if (page_io_) {
    *src = tmp;
    return true;
  } else {
    src->addr = local_page_;
    src->offset = 0;
    CrcCopyPage(src, &tmp, "file", "source_copy_check");
    if (!sat_->PutValid(&tmp))
      return false;
  }
  return true;
}


// Throw out a used empty page.
bool FileThread::PutEmptyPage(struct page_entry *src) {
  if (page_io_) {
    if (!sat_->PutEmpty(src))
      return false;
  }
  return true;
}

// Throw out a used, filled page.
bool FileThread::PutValidPage(struct page_entry *src) {
  if (page_io_) {
    if (!sat_->PutValid(src))
      return false;
  }
  return true;
}

// Copy data from file into memory blocks.
bool FileThread::ReadPages(int fd) {
  int page_length = sat_->page_length();
  int strict = sat_->strict();
  bool result = true;

  // Read our data back out of the file, into it's new location.
  lseek(fd, 0, SEEK_SET);
  for (int i = 0; i < sat_->disk_pages(); i++) {
    struct page_entry dst;
    if (!GetEmptyPage(&dst))
      return false;
    // Retrieve expected pattern.
    dst.pattern = page_recs_[i].pattern;
    dst.lastcpu = sched_getcpu();
    // Update page recordpage record.
    page_recs_[i].dst = dst.addr;

    // Read from the file into destination page.
    const bool record_history =
        page_io_ && sat_->diag_block_history();
    uint64 read_epoch_begin =
        record_history ? sat_->dram_frequency_epoch() : 0;
    if (!ReadPageFromFile(fd, &dst)) {
        PutEmptyPage(&dst);
        return false;
    }
    if (record_history) {
      sat_->RecordBlockWrite(
          dst.offset, Sat::BLOCK_WRITER_FILE,
          thread_num_, sched_getcpu(),
          read_epoch_begin, sat_->dram_frequency_epoch());
    }

    SectorValidatePage(page_recs_[i], &dst, i);

    // Ensure that the transfer ended up with correct data.
    if (strict) {
      // Record page index currently CRC checked.
      crc_page_ = i;
      int errors = CrcCheckPage(&dst, "file", "destination_check");
      if (errors) {
        logprintf(5, "Log: file miscompare at block %d, "
                  "offset %x-%x. File: %s\n",
                  i, i * page_length, ((i + 1) * page_length) - 1,
                  filename_.c_str());
        result = false;
      }
      crc_page_ = -1;
      // CrcCheckPage()가 상세 비교에서 Worker 오류 수를 직접 증가시킵니다.
      // 반환값은 이 page의 성공 여부 판정에만 사용합니다.
    }
    if (!PutValidPage(&dst))
      return false;
  }
  return result;
}

// File IO work loop. Execute until marked done.
bool FileThread::Work() {
  bool result = true;
  int64 loops = 0;

  logprintf(9, "Log: Starting file thread %d, file %s, device %s\n",
            thread_num_,
            filename_.c_str(),
            devicename_.c_str());

  if (!PagePrepare()) {
    status_ = false;
    return false;
  }

  // Open the data IO file.
  int fd = 0;
  if (!OpenFile(&fd)) {
    status_ = false;
    return false;
  }

  pass_ = 0;

  // Load patterns into page records.
  page_recs_ = new struct PageRec[sat_->disk_pages()];
  for (int i = 0; i < sat_->disk_pages(); i++) {
    page_recs_[i].pattern = new class Pattern();
  }

  // Loop until done.
  // --stop_on_errors 요청 후에는 새 파일 write/read cycle을 시작하지 않습니다.
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // Do the file write.
    if (!(result = result && WritePages(fd)))
      break;

    // Do the file read.
    if (!(result = result && ReadPages(fd)))
      break;

    loops++;
    pass_ = loops;
  }

  pages_copied_ = loops * sat_->disk_pages();

  // Clean up.
  CloseFile(fd);
  PageTeardown();

  logprintf(9, "Log: Completed %d: file thread status %d, %d pages copied\n",
            thread_num_, status_, pages_copied_);
  // Failure to read from device indicates hardware,
  // rather than procedural SW error.
  status_ = true;
  return true;
}

bool NetworkThread::IsNetworkStopSet() {
  return !IsReadyToRunNoPause();
}

bool NetworkSlaveThread::IsNetworkStopSet() {
  // This thread has no completion status.
  // It finishes whever there is no more data to be
  // passed back.
  return true;
}

// Set ip name to use for Network IO.
void NetworkThread::SetIP(const char *ipaddr_init) {
  strncpy(ipaddr_, ipaddr_init, 255);
}

// Create a socket.
// Return 0 on error.
bool NetworkThread::CreateSocket(int *psocket) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == -1) {
    logprintf(0, "Process Error: Cannot open socket\n");
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  *psocket = sock;
  return true;
}

// Close the socket.
bool NetworkThread::CloseSocket(int sock) {
  close(sock);
  return true;
}

// Initiate the tcp connection.
bool NetworkThread::Connect(int sock) {
  struct sockaddr_in dest_addr;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kNetworkPort);
  memset(&(dest_addr.sin_zero), '\0', sizeof(dest_addr.sin_zero));

  // Translate dot notation to u32.
  if (inet_aton(ipaddr_, &dest_addr.sin_addr) == 0) {
    logprintf(0, "Process Error: Cannot resolve %s\n", ipaddr_);
    pages_copied_ = 0;
    status_ = false;
    return false;
  }

  if (-1 == connect(sock, reinterpret_cast<struct sockaddr *>(&dest_addr),
                    sizeof(struct sockaddr))) {
    logprintf(0, "Process Error: Cannot connect %s\n", ipaddr_);
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  return true;
}

// TCP listener socket에 주소와 port를 bind하고 connection queue를 엽니다.
// bind 또는 listen 실패는 listener Worker 실패로 반환합니다.
bool NetworkListenThread::Listen() {
  struct sockaddr_in sa;

  memset(&(sa.sin_zero), '\0', sizeof(sa.sin_zero));

  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = INADDR_ANY;
  sa.sin_port = htons(kNetworkPort);

  if (-1 == ::bind(sock_, (struct sockaddr*)&sa, sizeof(struct sockaddr))) {
    char buf[256];
    sat_strerror(errno, buf, sizeof(buf));
    logprintf(0, "Process Error: Cannot bind socket: %s\n", buf);
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  if (-1 == ::listen(sock_, 3)) {
    char buf[256];
    sat_strerror(errno, buf, sizeof(buf));
    logprintf(0, "Process Error: Cannot listen on socket: %s\n", buf);
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  return true;
}

// Listener socket에 읽기 event가 생길 때까지 최대 5초 대기합니다.
// 반환값은 연결 준비 1, timeout 0, select 오류 -1입니다.
int NetworkListenThread::Wait() {
  for (;;) {
    fd_set rfds;
    struct timeval tv;

    // select()가 수정하는 fd_set과 timeout을 매 호출 전에 다시 설정합니다.
    FD_ZERO(&rfds);
    FD_SET(sock_, &rfds);
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    int retval = select(sock_ + 1, &rfds, NULL, NULL, &tv);
    if (retval >= 0)
      return retval > 0 ? 1 : 0;

    // Signal로 중단된 경우 실행 상태를 확인하고 안전하게 다시 대기합니다.
    if (errno == EINTR) {
      if (!IsReadyToRun())
        return 0;
      continue;
    }

    char buf[256];
    sat_strerror(errno, buf, sizeof(buf));
    logprintf(0, "Process Error: Cannot wait on listen socket: %s\n", buf);
    return -1;
  }
}

// 준비된 연결을 accept하고 새 socket descriptor를 반환합니다.
bool NetworkListenThread::GetConnection(int *pnewsock) {
  struct sockaddr_in sa;
  socklen_t size = sizeof(struct sockaddr_in);

  int newsock = accept(sock_, reinterpret_cast<struct sockaddr *>(&sa), &size);
  if (newsock < 0)  {
    char buf[256];
    sat_strerror(errno, buf, sizeof(buf));
    logprintf(0, "Process Error: Cannot accept connection: %s\n", buf);
    pages_copied_ = 0;
    status_ = false;
    return false;
  }
  *pnewsock = newsock;
  return true;
}

// Send a page, return false if a page was not sent.
bool NetworkThread::SendPage(int sock, struct page_entry *src) {
  int page_length = sat_->page_length();
  char *address = static_cast<char*>(src->addr);

  // Send our data over the network.
  int size = page_length;
  while (size) {
    int transferred = send(sock, address + (page_length - size), size, 0);
    if ((transferred == 0) || (transferred == -1)) {
      if (!IsNetworkStopSet()) {
        char buf[256] = "";
        sat_strerror(errno, buf, sizeof(buf));
        logprintf(0, "Process Error: Thread %d, "
                     "Network write failed, bailing. (%s)\n",
                  thread_num_, buf);
        status_ = false;
      }
      return false;
    }
    size = size - transferred;
  }
  return true;
}

// Receive a page. Return false if a page was not received.
bool NetworkThread::ReceivePage(int sock, struct page_entry *dst) {
  int page_length = sat_->page_length();
  char *address = static_cast<char*>(dst->addr);
  int write_dram_frequency = sat_->current_dram_frequency();

  // Maybe we will get our data back again, maybe not.
  int size = page_length;
  while (size) {
    int transferred = recv(sock, address + (page_length - size), size, 0);
    if ((transferred == 0) || (transferred == -1)) {
      // Typically network slave thread should exit as network master
      // thread stops sending data.
      if (IsNetworkStopSet()) {
        int err = errno;
        if (transferred == 0 && err == 0) {
          // Two system setups will not sync exactly,
          // allow early exit, but log it.
          logprintf(0, "Log: Net thread did not receive any data, exiting.\n");
        } else {
          char buf[256] = "";
          sat_strerror(err, buf, sizeof(buf));
          // Print why we failed.
          logprintf(0, "Process Error: Thread %d, "
                       "Network read failed, bailing (%s).\n",
                    thread_num_, buf);
          status_ = false;
          // Print arguments and results.
          logprintf(0, "Log: recv(%d, address %x, size %x, 0) == %x, err %d\n",
                    sock, address + (page_length - size),
                    size, transferred, err);
          if ((transferred == 0) &&
              (page_length - size < 512) &&
              (page_length - size > 0)) {
            // Print null terminated data received, to see who's been
            // sending us supicious unwanted data.
            address[page_length - size] = 0;
            logprintf(0, "Log: received  %d bytes: '%s'\n",
                      page_length - size, address);
          }
        }
      }
      return false;
    }
    size = size - transferred;
  }
  dst->write_dram_frequency = write_dram_frequency;
  return true;
}

// Network IO work loop. Execute until marked done.
// Return true if the thread ran as expected.
bool NetworkThread::Work() {
  logprintf(9, "Log: Starting network thread %d, ip %s\n",
            thread_num_,
            ipaddr_);

  // Make a socket.
  int sock = 0;
  if (!CreateSocket(&sock))
    return false;

  // Network IO loop requires network slave thread to have already initialized.
  // We will sleep here for awhile to ensure that the slave thread will be
  // listening by the time we connect.
  // Sleep for 15 seconds.
  sat_sleep(15);
  logprintf(9, "Log: Starting execution of network thread %d, ip %s\n",
            thread_num_,
            ipaddr_);

  // 대기 중 종료 요청이 기록되면 연결과 SAT queue 접근을 시작하지 않습니다.
  if (sat_->error_stop_requested()) {
    CloseSocket(sock);
    status_ = true;
    return true;
  }

  // Connect to a slave thread.
  if (!Connect(sock)) {
    CloseSocket(sock);
    return false;
  }

  // Loop until done.
  bool result = true;
  int strict = sat_->strict();
  int64 loops = 0;
  // 현재 전송을 마친 뒤 종료 요청을 확인하고 새 SAT 작업 단위를 받습니다.
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    struct page_entry src;
    struct page_entry dst;
    if (!sat_->GetValid(&src)) {
      logprintf(0, "Process Error: net_thread failed to pop pages, "
                "bailing\n");
      result = false;
      break;
    }
    if (!sat_->GetEmpty(&dst)) {
      // 먼저 확보한 source를 원래 상태로 반환하여 queue 누락을 막습니다.
      sat_->PutValid(&src);
      logprintf(0, "Process Error: net_thread failed to pop pages, "
                "bailing\n");
      result = false;
      break;
    }

    // Check data correctness.
    if (strict)
      CrcCheckPage(&src, "network", "source_check");

    // Do the network write.
    if (!SendPage(sock, &src)) {
      // 전송이 완료되지 않으면 source와 destination의 상태를 유지합니다.
      sat_->PutValid(&src);
      sat_->PutEmpty(&dst);
      result = false;
      break;
    }

    // Update pattern reference to reflect new contents.
    dst.pattern = src.pattern;
    dst.lastcpu = sched_getcpu();

    // Do the network read.
    const bool record_history = sat_->diag_block_history();
    uint64 receive_epoch_begin =
        record_history ? sat_->dram_frequency_epoch() : 0;
    if (!ReceivePage(sock, &dst)) {
      // 부분 수신된 destination은 Empty로 반환하여 expected 대상으로
      // 사용되지 않도록 처리합니다.
      sat_->PutValid(&src);
      sat_->PutEmpty(&dst);
      result = false;
      break;
    }
    if (record_history) {
      sat_->RecordBlockWrite(
          dst.offset, Sat::BLOCK_WRITER_NETWORK,
          thread_num_, sched_getcpu(),
          receive_epoch_begin, sat_->dram_frequency_epoch());
    }

    // Ensure that the transfer ended up with correct data.
    if (strict)
      CrcCheckPage(&dst, "network", "destination_check");

    // Return all of our pages to the queue.
    // 첫 반환이 실패해도 두 번째 작업 단위의 반환을 시도합니다.
    bool dst_returned = sat_->PutValid(&dst);
    bool src_returned = sat_->PutEmpty(&src);
    result = dst_returned && src_returned;
    if (!result) {
      logprintf(0, "Process Error: net_thread failed to push pages, "
                "bailing\n");
      break;
    }
    loops++;
  }

  pages_copied_ = loops;
  status_ = result;

  // Clean up.
  CloseSocket(sock);

  logprintf(9, "Log: Completed %d: network thread status %d, "
               "%d pages copied\n",
            thread_num_, status_, pages_copied_);
  return result;
}

// Spawn slave threads for incoming connections.
bool NetworkListenThread::SpawnSlave(int newsock, int threadid) {
  logprintf(12, "Log: Listen thread spawning slave\n");

  // Spawn slave thread, to reflect network traffic back to sender.
  ChildWorker *child_worker = new ChildWorker;
  child_worker->thread.SetSock(newsock);
  child_worker->thread.InitThread(threadid, sat_, os_, patternlist_,
                                  &child_worker->status);
  child_worker->status.Initialize();
  if (!child_worker->thread.SpawnThread()) {
    child_worker->thread.RemoveUnspawnedWorker();
    child_worker->status.Destroy();
    child_worker->thread.CloseOwnedSocket();
    delete child_worker;
    return false;
  }
  child_workers_.push_back(child_worker);

  return true;
}

// Reap slave threads.
bool NetworkListenThread::ReapSlaves() {
  bool result = true;
  // Gather status and reap threads.
  logprintf(12, "Log: Joining all outstanding threads\n");

  // 상대 peer가 연결을 유지해도 child의 blocking recv/send가 반환하도록
  // 모든 accepted socket을 먼저 shutdown합니다. 실제 close는 child가
  // 자신의 정리 경로에서 한 번만 수행합니다.
  for (size_t i = 0; i < child_workers_.size(); i++)
    child_workers_[i]->thread.ShutdownSocket();

  for (size_t i = 0; i < child_workers_.size(); i++) {
    NetworkSlaveThread& child_thread = child_workers_[i]->thread;
    logprintf(12, "Log: Joining slave thread %d\n", i);
    child_thread.JoinThread();
    child_thread.CloseOwnedSocket();
    if (child_thread.GetStatus() != 1) {
      logprintf(0, "Process Error: Slave Thread %d failed with status %d\n", i,
                child_thread.GetStatus());
      result = false;
    }
    errorcount_ += child_thread.GetErrorCount();
    logprintf(9, "Log: Slave Thread %d found %lld miscompares\n", i,
              child_thread.GetErrorCount());
    pages_copied_ += child_thread.GetPageCount();
  }

  return result;
}

// Network connection을 받아 Slave Worker를 생성하고 종료 시 회수합니다.
// Slave 생성 또는 회수 실패를 listener Worker 상태에 반영합니다.
bool NetworkListenThread::Work() {
  bool result = true;
  logprintf(9, "Log: Starting network listen thread %d\n",
            thread_num_);

  // Make a socket.
  sock_ = 0;
  if (!CreateSocket(&sock_)) {
    status_ = false;
    return false;
  }
  logprintf(9, "Log: Listen thread created sock\n");

  // Allows incoming connections to be queued up by socket library.
  int newsock = 0;
  if (!Listen()) {
    CloseSocket(sock_);
    status_ = false;
    return false;
  }
  logprintf(12, "Log: Listen thread waiting for incoming connections\n");

  // Wait on incoming connections, and spawn worker threads for them.
  int threadcount = 0;
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // Poll for connections that we can accept().
    const int wait_result = Wait();
    if (wait_result < 0) {
      result = false;
      break;
    }
    if (wait_result > 0) {
      // Accept those connections.
      logprintf(12, "Log: Listen thread found incoming connection\n");
      if (!GetConnection(&newsock)) {
        result = false;
        break;
      }
      if (!SpawnSlave(newsock, threadcount)) {
        logprintf(0, "Process Error: failed to spawn network slave %d\n",
                  threadcount);
        result = false;
        break;
      }
      threadcount++;
    }
  }

  // Gather status and join spawned threads.
  result = ReapSlaves() && result;

  // Delete the child workers.
  for (ChildVector::iterator it = child_workers_.begin();
       it != child_workers_.end(); ++it) {
    (*it)->status.Destroy();
    delete *it;
  }
  child_workers_.clear();

  CloseSocket(sock_);

  status_ = result;
  logprintf(9,
            "Log: Completed %d: network listen thread status %d, "
            "%d pages copied\n",
            thread_num_, status_, pages_copied_);
  return result;
}

// Set network reflector socket struct.
void NetworkSlaveThread::SetSock(int sock) {
  sat_assert(0 == pthread_mutex_lock(&socket_lock_));
  sock_ = sock;
  sat_assert(0 == pthread_mutex_unlock(&socket_lock_));
}

// 다른 thread에서 blocking network I/O를 종료할 때 사용합니다.
// shutdown()은 file descriptor를 닫지 않으므로 child 정리의 close와
// 중복되지 않습니다.
void NetworkSlaveThread::ShutdownSocket() {
  sat_assert(0 == pthread_mutex_lock(&socket_lock_));
  if (sock_ > 0)
    shutdown(sock_, SHUT_RDWR);
  sat_assert(0 == pthread_mutex_unlock(&socket_lock_));
}

// Child와 listener가 같은 descriptor를 중복 close하거나 재사용된 descriptor에
// shutdown을 호출하지 않도록 lock 안에서 close와 무효화를 함께 수행합니다.
void NetworkSlaveThread::CloseOwnedSocket() {
  sat_assert(0 == pthread_mutex_lock(&socket_lock_));
  if (sock_ > 0) {
    CloseSocket(sock_);
    sock_ = 0;
  }
  sat_assert(0 == pthread_mutex_unlock(&socket_lock_));
}

// Network reflector IO work loop. Execute until marked done.
// Return false on fatal software error.
bool NetworkSlaveThread::Work() {
  logprintf(9, "Log: Starting network slave thread %d\n",
            thread_num_);

  // Verify that we have a socket.
  int sock = sock_;
  if (!sock) {
    status_ = false;
    return false;
  }

  // Loop until done.
  int64 loops = 0;
  // Init a local buffer for storing data.
  void *local_page = NULL;
#ifdef HAVE_POSIX_MEMALIGN
  int result = posix_memalign(&local_page, 512, sat_->page_length());
#else
  local_page = memalign(512, sat_->page_length());
  int result = (local_page == 0);
#endif
  if (result) {
    logprintf(0, "Process Error: net slave posix_memalign "
                 "returned %d (fail)\n",
              result);
    status_ = false;
    CloseOwnedSocket();
    return false;
  }

  struct page_entry page;
  page.addr = local_page;

  // This thread will continue to run as long as the thread on the other end of
  // the socket is still sending and receiving data.
  while (1) {
    // Do the network read.
    if (!ReceivePage(sock, &page))
      break;

    // Do the network write.
    if (!SendPage(sock, &page))
      break;

    loops++;
  }

  pages_copied_ = loops;
  // No results provided from this type of thread.
  status_ = true;

  // Clean up.
  CloseOwnedSocket();
  free(local_page);

  logprintf(9,
            "Log: Completed %d: network slave thread status %d, "
            "%d pages copied\n",
            thread_num_, status_, pages_copied_);
  return true;
}

// Thread work loop. Execute until marked finished.
bool ErrorPollThread::Work() {
  logprintf(9, "Log: Starting system error poll thread %d\n", thread_num_);

  // This calls a generic error polling function in the Os abstraction layer.
  do {
    errorcount_ += os_->ErrorPoll();
    os_->ErrorWait();
  } while (IsReadyToRun() && !sat_->error_stop_requested());

  logprintf(9, "Log: Finished system error poll thread %d: %d errors\n",
            thread_num_, errorcount_);
  status_ = true;
  return true;
}

// Worker thread to heat up CPU.
// This thread does not evaluate pass/fail or software error.
bool CpuStressThread::Work() {
  logprintf(9, "Log: Starting CPU stress thread %d\n", thread_num_);

  do {
    // Run ludloff's platform/CPU-specific assembly workload.
    os_->CpuStressWorkload();
    YieldSelf();
  } while (IsReadyToRun() && !sat_->error_stop_requested());

  logprintf(9, "Log: Finished CPU stress thread %d:\n",
            thread_num_);
  status_ = true;
  return true;
}

CpuCacheCoherencyThread::CpuCacheCoherencyThread(cc_cacheline_data *data,
                                                 int cacheline_count,
                                                 int thread_num,
                                                 int thread_count,
                                                 int inc_count) {
  cc_cacheline_data_ = data;
  cc_cacheline_count_ = cacheline_count;
  cc_thread_num_ = thread_num;
  cc_thread_count_ = thread_count;
  cc_inc_count_ = inc_count;
}

// A very simple psuedorandom generator.  Since the random number is based
// on only a few simple logic operations, it can be done quickly in registers
// and the compiler can inline it.
uint64 CpuCacheCoherencyThread::SimpleRandom(uint64 seed) {
  return (seed >> 1) ^ (-(seed & 1) & kRandomPolynomial);
}

// Worked thread to test the cache coherency of the CPUs
// Return false on fatal sw error.
bool CpuCacheCoherencyThread::Work() {
  logprintf(9, "Log: Starting the Cache Coherency thread %d\n",
            cc_thread_num_);
  int64 time_start, time_end;

  // Use a slightly more robust random number for the initial
  // value, so the random sequences from the simple generator will
  // be more divergent.
#ifdef HAVE_RAND_R
  unsigned int seed = static_cast<unsigned int>(gettid());
  uint64 r = static_cast<uint64>(rand_r(&seed));
  r |= static_cast<uint64>(rand_r(&seed)) << 32;
#else
  srand(time(NULL));
  uint64 r = static_cast<uint64>(rand());  // NOLINT
  r |= static_cast<uint64>(rand()) << 32;  // NOLINT
#endif

  time_start = sat_get_time_us();

  uint64 total_inc = 0;  // Total increments done by the thread.
  // 상세 오류가 종료를 요청하면 cache coherency 반복을 종료합니다.
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    for (int i = 0; i < cc_inc_count_; i++) {
      // Choose a datastructure in random and increment the appropriate
      // member in that according to the offset (which is the same as the
      // thread number.
      r = SimpleRandom(r);
      int cline_num = r % cc_cacheline_count_;
      int offset;
      // Reverse the order for odd numbered threads in odd numbered cache
      // lines.  This is designed for massively multi-core systems where the
      // number of cores exceeds the bytes in a cache line, so "distant" cores
      // get a chance to exercize cache coherency between them.
      if (cline_num & cc_thread_num_ & 1)
        offset = (cc_thread_count_ & ~1) - cc_thread_num_;
      else
        offset = cc_thread_num_;
      // Increment the member of the randomely selected structure.
      (cc_cacheline_data_[cline_num].num[offset])++;
    }

    total_inc += cc_inc_count_;

    // Calculate if the local counter matches with the global value
    // in all the cache line structures for this particular thread.
    int cc_global_num = 0;
    for (int cline_num = 0; cline_num < cc_cacheline_count_; cline_num++) {
      int offset;
      // Perform the same offset calculation from above.
      if (cline_num & cc_thread_num_ & 1)
        offset = (cc_thread_count_ & ~1) - cc_thread_num_;
      else
        offset = cc_thread_num_;
      cc_global_num += cc_cacheline_data_[cline_num].num[offset];
      // Reset the cachline member's value for the next run.
      cc_cacheline_data_[cline_num].num[offset] = 0;
    }
    if (sat_->error_injection())
      cc_global_num = -1;

    // Since the count is only stored in a byte, to squeeze more into a
    // single cache line, only compare it as a byte.  In the event that there
    // is something detected, the chance that it would be missed by a single
    // thread is 1 in 256.  If it affects all cores, that makes the chance
    // of it being missed terribly minute.  It seems unlikely any failure
    // case would be off by more than a small number.
    if ((cc_global_num & 0xff) != (cc_inc_count_ & 0xff)) {
      errorcount_++;
      logprintf(0, "Hardware Error: global(%d) and local(%d) do not match\n",
                cc_global_num, cc_inc_count_);
      if (sat_->stop_on_error())
        sat_->RequestErrorStop();
    }
  }
  time_end = sat_get_time_us();

  int64 us_elapsed = time_end - time_start;
  // inc_rate is the no. of increments per second.
  double inc_rate = total_inc * 1e6 / us_elapsed;

  logprintf(4, "Stats: CC Thread(%d): Time=%llu us,"
            " Increments=%llu, Increments/sec = %.6lf\n",
            cc_thread_num_, us_elapsed, total_inc, inc_rate);
  logprintf(9, "Log: Finished CPU Cache Coherency thread %d:\n",
            cc_thread_num_);
  status_ = true;
  return true;
}

DiskThread::DiskThread(DiskBlockTable *block_table) {
  read_block_size_ = kSectorSize;   // default 1 sector (512 bytes)
  write_block_size_ = kSectorSize;  // this assumes read and write block size
                                    // are the same
  segment_size_ = -1;               // use the entire disk as one segment
  cache_size_ = 16 * 1024 * 1024;   // assume 16MiB cache by default
  // Use a queue such that 3/2 times as much data as the cache can hold
  // is written before it is read so that there is little chance the read
  // data is in the cache.
  queue_size_ = ((cache_size_ / write_block_size_) * 3) / 2;
  blocks_per_segment_ = 32;

  read_threshold_ = 100000;         // 100ms is a reasonable limit for
  write_threshold_ = 100000;        // reading/writing a sector

  read_timeout_ = 5000000;          // 5 seconds should be long enough for a
  write_timeout_ = 5000000;         // timout for reading/writing

  device_sectors_ = 0;
  non_destructive_ = 0;

#ifdef HAVE_LIBAIO_H
  aio_ctx_ = 0;
#endif
  block_table_ = block_table;
  update_block_table_ = 1;

  block_buffer_ = NULL;

  blocks_written_ = 0;
  blocks_read_ = 0;
}

DiskThread::~DiskThread() {
  if (block_buffer_)
    free(block_buffer_);
}

// Set filename for device file (in /dev).
void DiskThread::SetDevice(const char *device_name) {
  device_name_ = device_name;
}

// Set various parameters that control the behaviour of the test.
// -1 is used as a sentinel value on each parameter (except non_destructive)
// to indicate that the parameter not be set.
bool DiskThread::SetParameters(int read_block_size,
                               int write_block_size,
                               int64 segment_size,
                               int64 cache_size,
                               int blocks_per_segment,
                               int64 read_threshold,
                               int64 write_threshold,
                               int non_destructive) {
  if (read_block_size != -1) {
    // Blocks must be aligned to the disk's sector size.
    if (read_block_size % kSectorSize != 0) {
      logprintf(0, "Process Error: Block size must be a multiple of %d "
                "(thread %d).\n", kSectorSize, thread_num_);
      return false;
    }

    read_block_size_ = read_block_size;
  }

  if (write_block_size != -1) {
    // Write blocks must be aligned to the disk's sector size and to the
    // block size.
    if (write_block_size % kSectorSize != 0) {
      logprintf(0, "Process Error: Write block size must be a multiple "
                "of %d (thread %d).\n", kSectorSize, thread_num_);
      return false;
    }
    if (write_block_size % read_block_size_ != 0) {
      logprintf(0, "Process Error: Write block size must be a multiple "
                "of the read block size, which is %d (thread %d).\n",
                read_block_size_, thread_num_);
      return false;
    }

    write_block_size_ = write_block_size;

  } else {
    // Make sure write_block_size_ is still valid.
    if (read_block_size_ > write_block_size_) {
      logprintf(5, "Log: Assuming write block size equal to read block size, "
                "which is %d (thread %d).\n", read_block_size_,
                thread_num_);
      write_block_size_ = read_block_size_;
    } else {
      if (write_block_size_ % read_block_size_ != 0) {
        logprintf(0, "Process Error: Write block size (defined as %d) must "
                  "be a multiple of the read block size, which is %d "
                  "(thread %d).\n", write_block_size_, read_block_size_,
                  thread_num_);
        return false;
      }
    }
  }

  if (cache_size != -1) {
    cache_size_ = cache_size;
  }

  if (blocks_per_segment != -1) {
    if (blocks_per_segment <= 0) {
      logprintf(0, "Process Error: Blocks per segment must be greater than "
                   "zero.\n (thread %d)", thread_num_);
      return false;
    }

    blocks_per_segment_ = blocks_per_segment;
  }

  if (read_threshold != -1) {
    if (read_threshold <= 0) {
      logprintf(0, "Process Error: Read threshold must be greater than "
                   "zero (thread %d).\n", thread_num_);
      return false;
    }

    read_threshold_ = read_threshold;
  }

  if (write_threshold != -1) {
    if (write_threshold <= 0) {
      logprintf(0, "Process Error: Write threshold must be greater than "
                   "zero (thread %d).\n", thread_num_);
      return false;
    }

    write_threshold_ = write_threshold;
  }

  if (segment_size != -1) {
    // Segments must be aligned to the disk's sector size.
    if (segment_size % kSectorSize != 0) {
      logprintf(0, "Process Error: Segment size must be a multiple of %d"
                " (thread %d).\n", kSectorSize, thread_num_);
      return false;
    }

    segment_size_ = segment_size / kSectorSize;
  }

  non_destructive_ = non_destructive;

  // Having a queue of 150% of blocks that will fit in the disk's cache
  // should be enough to force out the oldest block before it is read and hence,
  // making sure the data comes form the disk and not the cache.
  queue_size_ = ((cache_size_ / write_block_size_) * 3) / 2;
  // Updating DiskBlockTable parameters
  if (update_block_table_) {
    block_table_->SetParameters(kSectorSize, write_block_size_,
                                device_sectors_, segment_size_,
                                device_name_);
  }
  return true;
}

// Open a device, return false on failure.
bool DiskThread::OpenDevice(int *pfile) {
  int flags = O_RDWR | O_SYNC | O_LARGEFILE;
  int fd = open(device_name_.c_str(), flags | O_DIRECT, 0);
  if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
    fd = open(device_name_.c_str(), flags, 0);  // Try without O_DIRECT
    os_->ActivateFlushPageCache();
  }
  if (fd < 0) {
    logprintf(0, "Process Error: Failed to open device %s (thread %d)!!\n",
              device_name_.c_str(), thread_num_);
    return false;
  }
  *pfile = fd;

  return GetDiskSize(fd);
}

// Retrieves the size (in bytes) of the disk/file.
// Return false on failure.
bool DiskThread::GetDiskSize(int fd) {
  struct stat device_stat;
  if (fstat(fd, &device_stat) == -1) {
    logprintf(0, "Process Error: Unable to fstat disk %s (thread %d).\n",
              device_name_.c_str(), thread_num_);
    return false;
  }

  // For a block device, an ioctl is needed to get the size since the size
  // of the device file (i.e. /dev/sdb) is 0.
  if (S_ISBLK(device_stat.st_mode)) {
    uint64 block_size = 0;

    if (ioctl(fd, BLKGETSIZE64, &block_size) == -1) {
      logprintf(0, "Process Error: Unable to ioctl disk %s (thread %d).\n",
                device_name_.c_str(), thread_num_);
      return false;
    }

    // Zero size indicates nonworking device..
    if (block_size == 0) {
      os_->ErrorReport(device_name_.c_str(), "device-size-zero", 1);
      ++errorcount_;
      status_ = true;  // Avoid a procedural error.
      return false;
    }

    device_sectors_ = block_size / kSectorSize;

  } else if (S_ISREG(device_stat.st_mode)) {
    device_sectors_ = device_stat.st_size / kSectorSize;

  } else {
    logprintf(0, "Process Error: %s is not a regular file or block "
              "device (thread %d).\n", device_name_.c_str(),
              thread_num_);
    return false;
  }

  logprintf(12, "Log: Device sectors: %lld on disk %s (thread %d).\n",
            device_sectors_, device_name_.c_str(), thread_num_);

  if (update_block_table_) {
    block_table_->SetParameters(kSectorSize, write_block_size_,
                                device_sectors_, segment_size_,
                                device_name_);
  }

  return true;
}

bool DiskThread::CloseDevice(int fd) {
  close(fd);
  return true;
}

// Return the time in microseconds.
int64 DiskThread::GetTime() {
  return sat_get_time_us();
}

// 아직 read 검사를 마치지 못한 block을 in-flight queue와 공유 table에서
// 제거합니다. 종료 요청 후에는 추가 장치 I/O를 수행하지 않습니다.
bool DiskThread::RemoveInFlightBlocks() {
  bool result = true;
  while (!in_flight_sectors_.empty()) {
    BlockData *block = in_flight_sectors_.front();
    in_flight_sectors_.pop();
    if (!block_table_->RemoveBlock(block))
      result = false;
  }
  return result;
}

// Do randomized reads and (possibly) writes on a device.
// Return false on fatal SW error, true on SW success,
// regardless of whether HW failed.
bool DiskThread::DoWork(int fd) {
  int64 block_num = 0;
  int64 num_segments;

  if (segment_size_ == -1) {
    num_segments = 1;
  } else {
    num_segments = device_sectors_ / segment_size_;
    if (device_sectors_ % segment_size_ != 0)
      num_segments++;
  }

  // Disk size should be at least 3x cache size.  See comment later for
  // details.
  sat_assert(device_sectors_ * kSectorSize > 3 * cache_size_);

  // This disk test works by writing blocks with a certain pattern to
  // disk, then reading them back and verifying it against the pattern
  // at a later time.  A failure happens when either the block cannot
  // be written/read or when the read block is different than what was
  // written.  If a block takes too long to write/read, then a warning
  // is given instead of an error since taking too long is not
  // necessarily an error.
  //
  // To prevent the read blocks from coming from the disk cache,
  // enough blocks are written before read such that a block would
  // be ejected from the disk cache by the time it is read.
  //
  // TODO(amistry): Implement some sort of read/write throttling.  The
  //                flood of asynchronous I/O requests when a drive is
  //                unplugged is causing the application and kernel to
  //                become unresponsive.

  // 현재 disk write/read cycle을 마친 뒤 종료 요청을 확인합니다.
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // Write blocks to disk.
    logprintf(16, "Log: Write phase %sfor disk %s (thread %d).\n",
              non_destructive_ ? "(disabled) " : "",
              device_name_.c_str(), thread_num_);
    while (IsReadyToRunNoPause() &&
           in_flight_sectors_.size() <
               static_cast<size_t>(queue_size_ + 1)) {
      // Confine testing to a particular segment of the disk.
      int64 segment = (block_num / blocks_per_segment_) % num_segments;
      if (!non_destructive_ &&
          (block_num % blocks_per_segment_ == 0)) {
        logprintf(20, "Log: Starting to write segment %lld out of "
                  "%lld on disk %s (thread %d).\n",
                  segment, num_segments, device_name_.c_str(),
                  thread_num_);
      }
      block_num++;

      BlockData *block = block_table_->GetUnusedBlock(segment);
      if (block == NULL)
        continue;

      // If an unused sequence of sectors could not be found, skip to the
      // next block to process.  Soon, a new segment will come and new
      // sectors will be able to be allocated.  This effectively puts a
      // minumim on the disk size at 3x the stated cache size, or 48MiB
      // if a cache size is not given (since the cache is set as 16MiB
      // by default).  Given that todays caches are at the low MiB range
      // and drive sizes at the mid GB, this shouldn't pose a problem.
      // The 3x minimum comes from the following:
      //   1. In order to allocate 'y' blocks from a segment, the
      //      segment must contain at least 2y blocks or else an
      //      allocation may not succeed.
      //   2. Assume the entire disk is one segment.
      //   3. A full write phase consists of writing blocks corresponding to
      //      3/2 cache size.
      //   4. Therefore, the one segment must have 2 * 3/2 * cache
      //      size worth of blocks = 3 * cache size worth of blocks
      //      to complete.
      // In non-destructive mode, don't write anything to disk.
      if (!non_destructive_) {
        if (!WriteBlockToDisk(fd, block)) {
          block_table_->RemoveBlock(block);
          RemoveInFlightBlocks();
          return true;
        }
        blocks_written_++;
      }

      // Block is either initialized by writing, or in nondestructive case,
      // initialized by being added into the datastructure for later reading.
      block->initialized();

      in_flight_sectors_.push(block);
    }
    if (!os_->FlushPageCache()) {  // If O_DIRECT worked, this will be a NOP.
      RemoveInFlightBlocks();
      return false;
    }

    // Verify blocks on disk.
    logprintf(20, "Log: Read phase for disk %s (thread %d).\n",
              device_name_.c_str(), thread_num_);
    while (IsReadyToRunNoPause() && !in_flight_sectors_.empty()) {
      BlockData *block = in_flight_sectors_.front();
      in_flight_sectors_.pop();
      if (!ValidateBlockOnDisk(fd, block)) {
        block_table_->RemoveBlock(block);
        RemoveInFlightBlocks();
        return true;
      }
      block_table_->RemoveBlock(block);
      blocks_read_++;
    }
  }

  // WorkerStatus STOP은 inner read loop도 종료하므로 남은 reference를
  // 장치 접근 없이 table에서 제거합니다.
  if (!RemoveInFlightBlocks())
    return false;

  pages_copied_ = blocks_written_ + blocks_read_;
  return true;
}

// Do an asynchronous disk I/O operation.
// Return false if the IO is not set up.
bool DiskThread::AsyncDiskIO(IoOp op, int fd, void *buf, int64 size,
                            int64 offset, int64 timeout) {
#ifdef HAVE_LIBAIO_H
  // Use the Linux native asynchronous I/O interface for reading/writing.
  // A read/write consists of three basic steps:
  //    1. create an io context.
  //    2. prepare and submit an io request to the context
  //    3. wait for an event on the context.

  struct {
    const int opcode;
    const char *op_str;
    const char *error_str;
  } operations[2] = {
    { IO_CMD_PREAD, "read", "disk-read-error" },
    { IO_CMD_PWRITE, "write", "disk-write-error" }
  };

  struct iocb cb;
  memset(&cb, 0, sizeof(cb));

  cb.aio_fildes = fd;
  cb.aio_lio_opcode = operations[op].opcode;
  cb.u.c.buf = buf;
  cb.u.c.nbytes = size;
  cb.u.c.offset = offset;

  struct iocb *cbs[] = { &cb };
  if (io_submit(aio_ctx_, 1, cbs) != 1) {
    int error = errno;
    char buf[256];
    sat_strerror(error, buf, sizeof(buf));
    logprintf(0, "Process Error: Unable to submit async %s "
                 "on disk %s (thread %d). Error %d, %s\n",
              operations[op].op_str, device_name_.c_str(),
              thread_num_, error, buf);
    return false;
  }

  struct io_event event;
  memset(&event, 0, sizeof(event));
  struct timespec tv;
  tv.tv_sec = timeout / 1000000;
  tv.tv_nsec = (timeout % 1000000) * 1000;
  if (io_getevents(aio_ctx_, 1, 1, &event, &tv) != 1) {
    // A ctrl-c from the keyboard will cause io_getevents to fail with an
    // EINTR error code.  This is not an error and so don't treat it as such,
    // but still log it.
    int error = errno;
    if (error == EINTR) {
      logprintf(5, "Log: %s interrupted on disk %s (thread %d).\n",
                operations[op].op_str, device_name_.c_str(),
                thread_num_);
    } else {
      os_->ErrorReport(device_name_.c_str(), operations[op].error_str, 1);
      errorcount_ += 1;
      logprintf(0, "Hardware Error: Timeout doing async %s to sectors "
                   "starting at %lld on disk %s (thread %d).\n",
                operations[op].op_str, offset / kSectorSize,
                device_name_.c_str(), thread_num_);
    }

    // Don't bother checking return codes since io_cancel seems to always fail.
    // Since io_cancel is always failing, destroying and recreating an I/O
    // context is a workaround for canceling an in-progress I/O operation.
    // TODO(amistry): Find out why io_cancel isn't working and make it work.
    io_cancel(aio_ctx_, &cb, &event);
    io_destroy(aio_ctx_);
    aio_ctx_ = 0;
    if (io_setup(5, &aio_ctx_)) {
      int error = errno;
      char buf[256];
      sat_strerror(error, buf, sizeof(buf));
      logprintf(0, "Process Error: Unable to create aio context on disk %s"
                " (thread %d) Error %d, %s\n",
                device_name_.c_str(), thread_num_, error, buf);
    }

    return false;
  }

  // event.res contains the number of bytes written/read or
  // error if < 0, I think.
  if (event.res != static_cast<uint64>(size)) {
    errorcount_++;
    os_->ErrorReport(device_name_.c_str(), operations[op].error_str, 1);

    int64 result = static_cast<int64>(event.res);
    if (result < 0) {
      switch (result) {
        case -EIO:
          logprintf(0, "Hardware Error: Low-level I/O error while doing %s to "
                       "sectors starting at %lld on disk %s (thread %d).\n",
                    operations[op].op_str, offset / kSectorSize,
                    device_name_.c_str(), thread_num_);
          break;
        default:
          logprintf(0, "Hardware Error: Unknown error while doing %s to "
                       "sectors starting at %lld on disk %s (thread %d).\n",
                    operations[op].op_str, offset / kSectorSize,
                    device_name_.c_str(), thread_num_);
      }
    } else {
      logprintf(0, "Hardware Error: Unable to %s to sectors starting at "
                   "%lld on disk %s (thread %d).\n",
                operations[op].op_str, offset / kSectorSize,
                device_name_.c_str(), thread_num_);
    }
    return false;
  }

  return true;
#else  // !HAVE_LIBAIO_H
  return false;
#endif
}

// Write a block to disk.
// Return false if the block is not written.
bool DiskThread::WriteBlockToDisk(int fd, BlockData *block) {
  memset(block_buffer_, 0, block->size());

  // Fill block buffer with a pattern
  struct page_entry pe;
  if (!sat_->GetValid(&pe)) {
    // Even though a valid page could not be obatined, it is not an error
    // since we can always fill in a pattern directly, albeit slower.
    unsigned int *memblock = static_cast<unsigned int *>(block_buffer_);
    block->set_pattern(patternlist_->GetRandomPattern());

    logprintf(11, "Log: Warning, using pattern fill fallback in "
                  "DiskThread::WriteBlockToDisk on disk %s (thread %d).\n",
              device_name_.c_str(), thread_num_);

    for (unsigned int i = 0; i < block->size()/wordsize_; i++) {
      memblock[i] = block->pattern()->pattern(i);
    }
  } else {
    memcpy(block_buffer_, pe.addr, block->size());
    block->set_pattern(pe.pattern);
    sat_->PutValid(&pe);
  }

  logprintf(12, "Log: Writing %lld sectors starting at %lld on disk %s"
            " (thread %d).\n",
            block->size()/kSectorSize, block->address(),
            device_name_.c_str(), thread_num_);

  int64 start_time = GetTime();

  if (!AsyncDiskIO(ASYNC_IO_WRITE, fd, block_buffer_, block->size(),
                   block->address() * kSectorSize, write_timeout_)) {
    return false;
  }

  int64 end_time = GetTime();
  logprintf(12, "Log: Writing time: %lld us (thread %d).\n",
            end_time - start_time, thread_num_);
  if (end_time - start_time > write_threshold_) {
    logprintf(5, "Log: Write took %lld us which is longer than threshold "
                 "%lld us on disk %s (thread %d).\n",
              end_time - start_time, write_threshold_, device_name_.c_str(),
              thread_num_);
  }

  return true;
}

// Verify a block on disk.
// Return true if the block was read, also increment errorcount
// if the block had data errors or performance problems.
bool DiskThread::ValidateBlockOnDisk(int fd, BlockData *block) {
  int64 blocks = block->size() / read_block_size_;
  int64 bytes_read = 0;
  int64 current_blocks;
  int64 current_bytes;
  uint64 address = block->address();

  logprintf(20, "Log: Reading sectors starting at %lld on disk %s "
            "(thread %d).\n",
            address, device_name_.c_str(), thread_num_);

  // Read block from disk and time the read.  If it takes longer than the
  // threshold, complain.
  if (lseek(fd, address * kSectorSize, SEEK_SET) == -1) {
    logprintf(0, "Process Error: Unable to seek to sector %lld in "
              "DiskThread::ValidateSectorsOnDisk on disk %s "
              "(thread %d).\n", address, device_name_.c_str(), thread_num_);
    return false;
  }
  int64 start_time = GetTime();

  // Split a large write-sized block into small read-sized blocks and
  // read them in groups of randomly-sized multiples of read block size.
  // This assures all data written on disk by this particular block
  // will be tested using a random reading pattern.
  while (blocks != 0) {
    // Test all read blocks in a written block.
    current_blocks = (random() % blocks) + 1;
    current_bytes = current_blocks * read_block_size_;

    memset(block_buffer_, 0, current_bytes);

    logprintf(20, "Log: Reading %lld sectors starting at sector %lld on "
              "disk %s (thread %d)\n",
              current_bytes / kSectorSize,
              (address * kSectorSize + bytes_read) / kSectorSize,
              device_name_.c_str(), thread_num_);

    if (!AsyncDiskIO(ASYNC_IO_READ, fd, block_buffer_, current_bytes,
                     address * kSectorSize + bytes_read,
                     write_timeout_)) {
      return false;
    }

    int64 end_time = GetTime();
    logprintf(20, "Log: Reading time: %lld us (thread %d).\n",
              end_time - start_time, thread_num_);
    if (end_time - start_time > read_threshold_) {
      logprintf(5, "Log: Read took %lld us which is longer than threshold "
                "%lld us on disk %s (thread %d).\n",
                end_time - start_time, read_threshold_,
                device_name_.c_str(), thread_num_);
    }

    // In non-destructive mode, don't compare the block to the pattern since
    // the block was never written to disk in the first place.
    if (!non_destructive_) {
      if (CheckRegion(block_buffer_, block->pattern(), 0, current_bytes,
                      0, bytes_read)) {
        os_->ErrorReport(device_name_.c_str(), "disk-pattern-error", 1);
        errorcount_ += 1;
        logprintf(0, "Hardware Error: Pattern mismatch in block starting at "
                  "sector %lld in DiskThread::ValidateSectorsOnDisk on "
                  "disk %s (thread %d).\n",
                  address, device_name_.c_str(), thread_num_);
      }
    }

    bytes_read += current_blocks * read_block_size_;
    blocks -= current_blocks;
  }

  return true;
}

// Direct device access thread.
// Return false on software error.
bool DiskThread::Work() {
  int fd;

  logprintf(9, "Log: Starting disk thread %d, disk %s\n",
            thread_num_, device_name_.c_str());

  srandom(time(NULL));

  if (!OpenDevice(&fd)) {
    status_ = false;
    return false;
  }

  // Allocate a block buffer aligned to 512 bytes since the kernel requires it
  // when using direct IO.
#ifdef HAVE_POSIX_MEMALIGN
  int memalign_result = posix_memalign(&block_buffer_, kBufferAlignment,
                                       sat_->page_length());
#else
  block_buffer_ = memalign(kBufferAlignment, sat_->page_length());
  int memalign_result = (block_buffer_ == 0);
#endif
  if (memalign_result) {
    CloseDevice(fd);
    logprintf(0, "Process Error: Unable to allocate memory for buffers "
                 "for disk %s (thread %d) posix memalign returned %d.\n",
              device_name_.c_str(), thread_num_, memalign_result);
    status_ = false;
    return false;
  }

#ifdef HAVE_LIBAIO_H
  if (io_setup(5, &aio_ctx_)) {
    CloseDevice(fd);
    logprintf(0, "Process Error: Unable to create aio context for disk %s"
              " (thread %d).\n",
              device_name_.c_str(), thread_num_);
    status_ = false;
    return false;
  }
#endif

  bool result = DoWork(fd);

  status_ = result;

#ifdef HAVE_LIBAIO_H
  io_destroy(aio_ctx_);
#endif
  CloseDevice(fd);

  logprintf(9, "Log: Completed %d (disk %s): disk thread status %d, "
               "%d pages copied\n",
            thread_num_, device_name_.c_str(), status_, pages_copied_);
  return result;
}

RandomDiskThread::RandomDiskThread(DiskBlockTable *block_table)
    : DiskThread(block_table) {
  update_block_table_ = 0;
}

RandomDiskThread::~RandomDiskThread() {
}

// Workload for random disk thread.
bool RandomDiskThread::DoWork(int fd) {
  logprintf(11, "Log: Random phase for disk %s (thread %d).\n",
            device_name_.c_str(), thread_num_);
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    BlockData *block = block_table_->GetRandomBlock();
    if (block == NULL) {
      logprintf(12, "Log: No block available for device %s (thread %d).\n",
                device_name_.c_str(), thread_num_);
    } else {
      ValidateBlockOnDisk(fd, block);
      block_table_->ReleaseBlock(block);
      blocks_read_++;
    }
  }
  pages_copied_ = blocks_read_;
  return true;
}

MemoryRegionThread::MemoryRegionThread() {
  error_injection_ = false;
  pages_ = NULL;
}

MemoryRegionThread::~MemoryRegionThread() {
  if (pages_ != NULL)
    delete pages_;
}

// Set a region of memory or MMIO to be tested.
// Return false if region could not be mapped.
bool MemoryRegionThread::SetRegion(void *region, int64 size) {
  int plength = sat_->page_length();
  int npages = size / plength;
  if (size % plength) {
    logprintf(0, "Process Error: region size is not a multiple of SAT "
              "page length\n");
    return false;
  } else {
    if (pages_ != NULL)
      delete pages_;
    pages_ = new PageEntryQueue(npages);
    char *base_addr = reinterpret_cast<char*>(region);
    region_ = base_addr;
    for (int i = 0; i < npages; i++) {
      struct page_entry pe;
      init_pe(&pe);
      pe.addr = reinterpret_cast<void*>(base_addr + i * plength);
      pe.offset = i * plength;

      pages_->Push(&pe);
    }
    return true;
  }
}

// Memory region 또는 MMIO 검사 mismatch의 상세 주소와 데이터를 출력합니다.
// Check 단계 로그의 read error/write error는 actual·reread·expected 관계로
// 만든 소프트웨어 분류이며 실제 DRAM read/write 원인을 확정하지 않습니다.
void MemoryRegionThread::ProcessError(struct ErrorRecord *error,
                                      int priority,
                                      const char *message) {
  uint32 buffer_offset;
  if (phase_ == kPhaseCopy) {
    // If the error occurred on the Copy Phase, it means that
    // the source data (i.e., the main memory) is wrong. so
    // just pass it to the original ProcessError to call a
    // bad-dimm error
    WorkerThread::ProcessError(error, priority, message);
  } else if (phase_ == kPhaseCheck) {
    char dram_frequencies[128];
    char current_dram_frequency[32];
    char dram_coordinates[192];
    // Check 단계의 mismatch 주소를 reread하여 상세 로그 필드를 구성합니다.
    os_->Flush(error->vaddr);
    error->reread_dram_frequency = sat_->current_dram_frequency();
    error->reread = *(error->vaddr);
    FormatDramFrequencies(error, dram_frequencies, sizeof(dram_frequencies));
    char *good = reinterpret_cast<char*>(&(error->expected));
    char *bad = reinterpret_cast<char*>(&(error->actual));
    sat_assert(error->expected != error->actual);
    unsigned int offset = 0;
    for (offset = 0; offset < (sizeof(error->expected) - 1); offset++) {
      if (good[offset] != bad[offset])
        break;
    }

    error->vbyteaddr = reinterpret_cast<char*>(error->vaddr) + offset;

    buffer_offset = error->vbyteaddr - region_;

    // 로그의 가상 주소와 물리 주소는 같은 64-bit word를 가리킵니다.
    error->paddr = os_->VirtualToPhysical(error->vaddr);
    FormatDramCoordinates(sat_, error->paddr,
                          dram_coordinates, sizeof(dram_coordinates));
    FormatDramFrequencyValue(error->reread_dram_frequency,
                             current_dram_frequency,
                             sizeof(current_dram_frequency));
    logprintf(priority,
              "%s: miscompare on %s, CRC check at %p(0x%llx), "
              "offset %llx: read:0x%016llx, reread:0x%016llx "
              "expected:0x%016llx. %s, %s, cur_mode:%s, cur_freq:%s, "
              "ddr_freq(%s).\n",
              message,
              identifier_.c_str(),
              error->vaddr,
              error->paddr,
              buffer_offset,
              error->actual,
              error->reread,
              error->expected,
              (error->reread == error->expected) ?
                  "read error" : "write error",
              dram_coordinates,
              sat_->dram_frequency_mode(),
              current_dram_frequency,
              dram_frequencies);
    if (sat_->stop_on_error())
      sat_->RequestErrorStop();
  } else {
    logprintf(0, "Process Error: memory region thread raised an "
              "unexpected error.");
  }
}

// Workload for testion memory or MMIO regions.
// Return false on software error.
bool MemoryRegionThread::Work() {
  struct page_entry source_pe;
  struct page_entry memregion_pe;
  bool result = true;
  int64 loops = 0;
  const uint64 error_constant = 0x00ba00000000ba00LL;

  // For error injection.
  int64 *addr = 0x0;
  int offset = 0;
  int64 data = 0;

  logprintf(9, "Log: Starting Memory Region thread %d\n", thread_num_);

  // 한 번 받은 두 작업 단위는 queue에 반환한 뒤 종료 요청을 확인합니다.
  while (IsReadyToRun() && !sat_->error_stop_requested()) {
    // Getting pages from SAT and queue.
    phase_ = kPhaseNoPhase;
    result = result && sat_->GetValid(&source_pe);
    if (!result) {
      logprintf(0, "Process Error: memory region thread failed to pop "
                "pages from SAT, bailing\n");
      break;
    }

    result = result && pages_->PopRandom(&memregion_pe);
    if (!result) {
      logprintf(0, "Process Error: memory region thread failed to pop "
                "pages from queue, bailing\n");
      break;
    }

    // Error injection for CRC copy.
    if ((sat_->error_injection() || error_injection_) && loops == 1) {
      addr = reinterpret_cast<int64*>(source_pe.addr);
      offset = random() % (sat_->page_length() / wordsize_);
      data = addr[offset];
      addr[offset] = error_constant;
    }

    // Copying SAT page into memory region.
    phase_ = kPhaseCopy;
    CrcCopyPage(&memregion_pe, &source_pe,
                "memory_region", "source_copy_check");
    memregion_pe.pattern = source_pe.pattern;
    memregion_pe.lastcpu = sched_getcpu();

    // Error injection for CRC Check.
    if ((sat_->error_injection() || error_injection_) && loops == 2) {
      addr = reinterpret_cast<int64*>(memregion_pe.addr);
      offset = random() % (sat_->page_length() / wordsize_);
      data = addr[offset];
      addr[offset] = error_constant;
    }

    // Checking page content in memory region.
    phase_ = kPhaseCheck;
    CrcCheckPage(&memregion_pe, "memory_region", "destination_check");

    phase_ = kPhaseNoPhase;
    // Storing pages on their proper queues.
    result = result && sat_->PutValid(&source_pe);
    if (!result) {
      logprintf(0, "Process Error: memory region thread failed to push "
                "pages into SAT, bailing\n");
      break;
    }
    result = result && pages_->Push(&memregion_pe);
    if (!result) {
      logprintf(0, "Process Error: memory region thread failed to push "
                "pages into queue, bailing\n");
      break;
    }

    if ((sat_->error_injection() || error_injection_) &&
        loops >= 1 && loops <= 2) {
      addr[offset] = data;
    }

    loops++;
    YieldSelf();
  }

  pages_copied_ = loops;
  status_ = result;
  logprintf(9, "Log: Completed %d: Memory Region thread. Status %d, %d "
            "pages checked\n", thread_num_, status_, pages_copied_);
  return result;
}

// The list of MSRs to read from each cpu.
const CpuFreqThread::CpuRegisterType CpuFreqThread::kCpuRegisters[] = {
  { kMsrTscAddr, "TSC" },
  { kMsrAperfAddr, "APERF" },
  { kMsrMperfAddr, "MPERF" },
};

CpuFreqThread::CpuFreqThread(int num_cpus, int freq_threshold, int round)
  : num_cpus_(num_cpus),
    freq_threshold_(freq_threshold),
    round_(round) {
  sat_assert(round >= 0);
  if (round == 0) {
    // If rounding is off, force rounding to the nearest MHz.
    round_ = 1;
    round_value_ = 0.5;
  } else {
    round_value_ = round/2.0;
  }
}

CpuFreqThread::~CpuFreqThread() {
}

// Compute the difference between the currently read MSR values and the
// previously read values and store the results in delta. If any of the
// values did not increase, or the TSC value is too small, returns false.
// Otherwise, returns true.
bool CpuFreqThread::ComputeDelta(CpuDataType *current, CpuDataType *previous,
                                 CpuDataType *delta) {
  // Loop through the msrs.
  for (int msr = 0; msr < kMsrLast; msr++) {
    if (previous->msrs[msr] > current->msrs[msr]) {
      logprintf(0, "Log: Register %s went backwards 0x%llx to 0x%llx "
                "skipping interval\n", kCpuRegisters[msr], previous->msrs[msr],
                current->msrs[msr]);
      return false;
    } else {
      delta->msrs[msr] = current->msrs[msr] - previous->msrs[msr];
    }
  }

  // Check for TSC < 1 Mcycles over interval.
  if (delta->msrs[kMsrTsc] < (1000 * 1000)) {
    logprintf(0, "Log: Insanely slow TSC rate, TSC stops in idle?\n");
    return false;
  }
  timersub(&current->tv, &previous->tv, &delta->tv);

  return true;
}

// Compute the change in values of the MSRs between current and previous,
// set the frequency in MHz of the cpu. If there is an error computing
// the delta, return false. Othewise, return true.
bool CpuFreqThread::ComputeFrequency(CpuDataType *current,
                                     CpuDataType *previous, int *freq) {
  CpuDataType delta;
  if (!ComputeDelta(current, previous, &delta)) {
    return false;
  }

  double interval = delta.tv.tv_sec + delta.tv.tv_usec / 1000000.0;
  double frequency = 1.0 * delta.msrs[kMsrTsc] / 1000000
                     * delta.msrs[kMsrAperf] / delta.msrs[kMsrMperf] / interval;

  // Use the rounding value to round up properly.
  int computed = static_cast<int>(frequency + round_value_);
  *freq = computed - (computed % round_);
  return true;
}

// This is the task function that the thread executes.
bool CpuFreqThread::Work() {
  cpu_set_t cpuset;
  if (!AvailableCpus(&cpuset)) {
    logprintf(0, "Process Error: Cannot get information about the cpus.\n");
    return false;
  }

  // Start off indicating the test is passing.
  status_ = true;

  int curr = 0;
  int prev = 1;
  uint32 num_intervals = 0;
  bool paused = false;
  bool valid;
  bool pass = true;

  vector<CpuDataType> data[2];
  data[0].resize(num_cpus_);
  data[1].resize(num_cpus_);
  while (IsReadyToRun(&paused) && !sat_->error_stop_requested()) {
    if (paused) {
      // Reset the intervals and restart logic after the pause.
      num_intervals = 0;
    }
    if (num_intervals == 0) {
      // If this is the first interval, then always wait a bit before
      // starting to collect data.
      sat_sleep(kStartupDelay);
    }

    // Get the per cpu counters.
    valid = true;
    for (int cpu = 0; cpu < num_cpus_; cpu++) {
      if (CPU_ISSET(cpu, &cpuset)) {
        if (!GetMsrs(cpu, &data[curr][cpu])) {
          logprintf(0, "Failed to get msrs on cpu %d.\n", cpu);
          valid = false;
          break;
        }
      }
    }
    if (!valid) {
      // Reset the number of collected intervals since something bad happened.
      num_intervals = 0;
      continue;
    }

    num_intervals++;

    // Only compute a delta when we have at least two intervals worth of data.
    if (num_intervals > 2) {
      for (int cpu = 0; cpu < num_cpus_; cpu++) {
        if (CPU_ISSET(cpu, &cpuset)) {
          int freq;
          if (!ComputeFrequency(&data[curr][cpu], &data[prev][cpu],
                                &freq)) {
            // Reset the number of collected intervals since an unknown
            // error occurred.
            logprintf(0, "Log: Cannot get frequency of cpu %d.\n", cpu);
            num_intervals = 0;
            break;
          }
          logprintf(15, "Cpu %d Freq %d\n", cpu, freq);
          if (freq < freq_threshold_) {
            errorcount_++;
            pass = false;
            logprintf(0, "Log: Cpu %d frequency is too low, frequency %d MHz "
                      "threshold %d MHz.\n", cpu, freq, freq_threshold_);
          }
        }
      }
    }

    sat_sleep(kIntervalPause);

    // Swap the values in curr and prev (these values flip between 0 and 1).
    curr ^= 1;
    prev ^= 1;
  }

  return pass;
}


// Get the MSR values for this particular cpu and save them in data. If
// any error is encountered, returns false. Otherwise, returns true.
bool CpuFreqThread::GetMsrs(int cpu, CpuDataType *data) {
  for (int msr = 0; msr < kMsrLast; msr++) {
    if (!os_->ReadMSR(cpu, kCpuRegisters[msr].msr, &data->msrs[msr])) {
      return false;
    }
  }
  // Save the time at which we acquired these values.
  gettimeofday(&data->tv, NULL);

  return true;
}

// Returns true if this test can run on the current machine. Otherwise,
// returns false.
bool CpuFreqThread::CanRun() {
#if defined(STRESSAPPTEST_CPU_X86_64) || defined(STRESSAPPTEST_CPU_I686)
  unsigned int eax, ebx, ecx, edx;

  // Check that the TSC feature is supported.
  // This check is valid for both Intel and AMD.
  eax = 1;
  cpuid(&eax, &ebx, &ecx, &edx);
  if (!(edx & (1 << 5))) {
    logprintf(0, "Process Error: No TSC support.\n");
    return false;
  }

  // Check the highest extended function level supported.
  // This check is valid for both Intel and AMD.
  eax = 0x80000000;
  cpuid(&eax, &ebx, &ecx, &edx);
  if (eax < 0x80000007) {
    logprintf(0, "Process Error: No invariant TSC support.\n");
    return false;
  }

  // Non-Stop TSC is advertised by CPUID.EAX=0x80000007: EDX.bit8
  // This check is valid for both Intel and AMD.
  eax = 0x80000007;
  cpuid(&eax, &ebx, &ecx, &edx);
  if ((edx & (1 << 8)) == 0) {
    logprintf(0, "Process Error: No non-stop TSC support.\n");
    return false;
  }

  // APERF/MPERF is advertised by CPUID.EAX=0x6: ECX.bit0
  // This check is valid for both Intel and AMD.
  eax = 0x6;
  cpuid(&eax, &ebx, &ecx, &edx);
  if ((ecx & 1) == 0) {
    logprintf(0, "Process Error: No APERF MSR support.\n");
    return false;
  }
  return true;
#else
  logprintf(0, "Process Error: "
               "cpu_freq_test is only supported on X86 processors.\n");
  return false;
#endif
}
