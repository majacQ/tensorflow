/* Copyright 2025 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// A tool for computing GPU statistics from an XSpace protobuf.

#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"
#include "xla/debug_options_flags.h"
#include "xla/tools/compute_gpu_device_stats.h"
#include "xla/tsl/util/command_line_flags.h"
#include "tsl/platform/init_main.h"

namespace {

const char* const kUsage = R"(
    This tool computes GPU statistics from an XSpace protobuf.

    Usage:

      bazel run compute_gpu_device_stats -- --input=path/to/xspace.pb

    Output:
      Device Time: 12345.67 us
      Device Memcpy Time: 1234.56 us
    )";
}  // namespace

int main(int argc, char* argv[]) {
  std::string input;
  std::vector<tsl::Flag> flag_list = {tsl::Flag("input", &input, "input file")};
  xla::AppendDebugOptionsFlags(&flag_list);
  const std::string usage_string =
      absl::StrCat(kUsage, "\n\n", tsl::Flags::Usage(argv[0], flag_list));
  bool parse_ok = tsl::Flags::Parse(&argc, argv, flag_list);
  tsl::port::InitMain(usage_string.c_str(), &argc, &argv);

  if (!parse_ok) {
    LOG(QFATAL) << usage_string;
  }
  if (input.empty()) {
    LOG(QFATAL) << "Must specify input file with --input=<filename>";
  }

  absl::Status status = xla::gpu::Run(input);
  if (!status.ok()) {
    LOG(ERROR) << status;
    return 1;
  }
  return 0;
}
