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

#include "rootserver/ddl_task/fts_build_overlap.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using oceanbase::common::ObString;
using oceanbase::rootserver::is_fts_build_overlap_parser;

namespace
{

constexpr char EXPERIMENT[] = "SEEKDB_BENG_SCOPED_OVERLAP_EXPERIMENT";
int checks = 0;
int failures = 0;

class ScopedEnvironment
{
public:
  ScopedEnvironment()
  {
    const char *value = std::getenv(EXPERIMENT);
    existed_ = value != nullptr;
    if (existed_) {
      original_ = value;
    }
  }

  ~ScopedEnvironment()
  {
    set(existed_ ? original_.c_str() : nullptr);
  }

  void set(const char *value)
  {
    const int ret = value == nullptr ? unsetenv(EXPERIMENT) : setenv(EXPERIMENT, value, 1);
    if (ret != 0) {
      std::perror("setting experiment environment");
      std::exit(EXIT_FAILURE);
    }
  }

private:
  bool existed_ = false;
  std::string original_;
};

void expect(const ObString &parser, bool expected, const char *label)
{
  ++checks;
  const bool actual = is_fts_build_overlap_parser(parser);
  if (actual != expected) {
    ++failures;
    const char *gate = std::getenv(EXPERIMENT);
    std::fprintf(stderr, "FAIL %s: gate=%s expected=%d actual=%d\n",
                 label, gate == nullptr ? "<unset>" : gate, expected, actual);
  }
}

void check_parsers(bool beng_enabled)
{
  for (const char *parser : {"ik", "IK", "iK", "ik.1", "IK.1"}) {
    expect(ObString(parser), true, parser);
  }
  for (const char *parser : {"beng", "BENG", "BeNg", "beng.1", "BENG.1"}) {
    expect(ObString(parser), beng_enabled, parser);
  }
  for (const char *parser : {"", "ngram", "space", "jieba", "ik.2", "ik.10",
                            "beng.2", "beng.10", "beng.01", "beng.", "beng ",
                            " beng", "beng.1x", "xbeng", "beng.extra", "中文"}) {
    expect(ObString(parser), false, parser);
  }
  expect(ObString(), false, "null parser");

  // Parser names are length-delimited ObStrings, not necessarily C strings.
  const char ik[] = {'I', 'K'};
  const char beng[] = {'B', 'e', 'N', 'g', '.', '1'};
  const char suffix[] = {'b', 'e', 'n', 'g', '.', '1', 'x'};
  const char embedded_null[] = {'b', 'e', 'n', 'g', '\0', '1'};
  expect(ObString(sizeof(ik), ik), true, "nonterminated IK");
  expect(ObString(sizeof(beng), beng), beng_enabled, "nonterminated BEng.1");
  expect(ObString(6, suffix), beng_enabled, "bounded BEng.1 prefix");
  expect(ObString(sizeof(suffix), suffix), false, "BEng.1 trailing byte");
  expect(ObString(3, beng), false, "truncated BEng");
  expect(ObString(sizeof(embedded_null), embedded_null), false, "embedded NUL");
}

} // namespace

int main()
{
  ScopedEnvironment environment;
  // Only the exact opt-in value "1" enables BEng. IK admission is independent.
  environment.set(nullptr);
  check_parsers(false);
  for (const char *value : {"", "0", "00", "01", "10", "true", "false", "yes", " 1", "1 ", "1\n"}) {
    environment.set(value);
    check_parsers(false);
  }
  environment.set("1");
  check_parsers(true);
  environment.set("0");
  check_parsers(false);
  environment.set(nullptr);
  check_parsers(false);
  std::printf("%s: %d parser admission checks, %d failures\n",
              failures == 0 ? "PASS" : "FAIL", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
