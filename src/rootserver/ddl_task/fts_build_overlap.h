/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "lib/string/ob_string.h"
#include <cstdlib>
#include <cstring>

namespace oceanbase
{
namespace rootserver
{

inline bool is_fts_build_overlap_parser(const common::ObString &parser)
{
  bool eligible = 0 == parser.case_compare("ik") || 0 == parser.case_compare("ik.1");
  if (!eligible && (0 == parser.case_compare("beng") || 0 == parser.case_compare("beng.1"))) {
    // BEng uses the existing single-pair queue only when explicitly opted in.
    // Queue availability and the task's other admission checks still apply.
    const char *gate = std::getenv("SEEKDB_BENG_SCOPED_OVERLAP_EXPERIMENT");
    eligible = gate != nullptr && 0 == std::strcmp(gate, "1");
  }
  return eligible;
}

} // namespace rootserver
} // namespace oceanbase
