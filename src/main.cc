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

#include <string.h>

#include "dram_address.h"
#include "sattypes.h"
#include "sat.h"

// ParseArgs()의 기존 공개 계약은 건드리지 않으면서 qc-sm8975를 추가합니다.
// main에서 새 이름을 legacy lpddr-v1 spelling으로 바꾸고 별도 flag로 실제
// decoder를 선택합니다. 기존 --dram-map lpddr-v1 동작은 그대로 유지됩니다.
bool g_qc_sm8975_dram_map_requested = false;

static void PrepareDramMapCompatibilityArgs(int argc, char **argv) {
  static char kLegacyProfileName[] = "lpddr-v1";
  g_qc_sm8975_dram_map_requested = false;

  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], "--dram-map") != 0)
      continue;

    if (strcmp(argv[i + 1], "qc-sm8975") == 0) {
      g_qc_sm8975_dram_map_requested = true;
      argv[i + 1] = kLegacyProfileName;
    } else {
      // 여러 번 지정된 경우 ParseArgs와 동일하게 마지막 --dram-map이
      // 실질적인 설정이 되도록 flag도 마지막 값을 따릅니다.
      g_qc_sm8975_dram_map_requested = false;
    }
    ++i;
  }
}

int main(int argc, char **argv) {
  PrepareDramMapCompatibilityArgs(argc, argv);

  Sat *sat = SatFactory();
  if (sat == NULL) {
    logprintf(0, "Process Error: failed to allocate Sat object\n");
    return 255;
  }

  if (!sat->ParseArgs(argc, argv)) {
    logprintf(0, "Process Error: Sat::ParseArgs() failed\n");
    sat->bad_status();
  } else if (!sat->Initialize()) {
    logprintf(0, "Process Error: Sat::Initialize() failed\n");
    sat->bad_status();
  } else if (!sat->Run()) {
    logprintf(0, "Process Error: Sat::Run() failed\n");
    sat->bad_status();
  }
  sat->PrintResults();
  if (!sat->Cleanup()) {
    logprintf(0, "Process Error: Sat::Cleanup() failed\n");
    sat->bad_status();
  }

  int retval;
  if (sat->status() != 0) {
    logprintf(0, "Process Error: Fatal issue encountered. See above logs for "
              "details.\n");
    retval = 1;
  } else if (sat->errors() != 0) {
    retval = 1;
  } else {
    retval = 0;
  }

  delete sat;
  return retval;
}
