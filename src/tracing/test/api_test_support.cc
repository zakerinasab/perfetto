/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/tracing/test/api_test_support.h"

#include "perfetto/base/time.h"
#include "perfetto/ext/base/proc_utils.h"

namespace perfetto {
namespace test {

int32_t GetCurrentProcessId() {
  return static_cast<int32_t>(base::GetProcessId());
}

uint64_t GetTraceTimeNs() {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return static_cast<uint64_t>(perfetto::base::GetBootTimeNs().count());
#else
  return static_cast<uint64_t>(perfetto::base::GetWallTimeNs().count());
#endif
}

}  // namespace test
}  // namespace perfetto
