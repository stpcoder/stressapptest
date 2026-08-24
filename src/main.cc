// Copyright 2006 Google Inc. All Rights Reserved.

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

// sat.cc : a stress test for stressful testing

#include <stdio.h>
#include <string.h>

#include "logger.h"
#include "sattypes.h"
#include "sat.h"

// sat.cc historically recognizes the internal spelling "lpddr-v1".  The old
// mapping equations behind that name have been removed from dram_address.h.
// Until the large legacy argument parser is renamed, translate the only public
// profile name (sm8975-lp6) to that internal token before ParseArgs().
//
// Direct use of lpddr-v1 is rejected so there is no user-visible path back to
// the removed empirical mapping.
static bool PrepareDramMapArgs(int argc, char **argv) {
  static char kInternalProfileName[] = "lpddr-v1";
  bool sm8975_lp6_enabled = false;

  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], "--dram-map") != 0)
      continue;

    if (strcmp(argv[i + 1], "lpddr-v1") == 0) {
      fprintf(stderr,
              "Process Error: --dram-map lpddr-v1 was removed because its "
              "address equations do not match QC SM8975 LPDDR6. Use "
              "--dram-map sm8975-lp6.\n");
      return false;
    }

    if (strcmp(argv[i + 1], "sm8975-lp6") == 0) {
      argv[i + 1] = kInternalProfileName;
      sm8975_lp6_enabled = true;
    } else {
      // ParseArgs uses the last occurrence. Keep the logger gate aligned with
      // that behavior for --dram-map none or an invalid later value.
      sm8975_lp6_enabled = false;
    }
    ++i;
  }

  Logger::GlobalLogger()->SetSm8975Lp6Mapping(sm8975_lp6_enabled);
  return true;
}

int main(int argc, char **argv) {
  if (!PrepareDramMapArgs(argc, argv))
    return 1;

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
