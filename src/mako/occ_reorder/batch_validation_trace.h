#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mako {

inline bool BatchValidationTraceEnabled() {
  const char* env = std::getenv("MAKO_BATCH_VALIDATION_TRACE");
  if (!env) {
    return false;
  }
  {
    static bool logged = false;
    if (!logged) {
      std::fprintf(stderr,
                   "[batch_validation] trace flag raw value=\"%s\"\n",
                   env[0] ? env : "<empty>");
      logged = true;
    }
  }
  std::string flag(env);
  if (flag.empty()) {
    return false;
  }
  std::transform(flag.begin(),
                 flag.end(),
                 flag.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return !(flag == "0" || flag == "false" || flag == "off" || flag == "no");
}

}  // namespace mako


