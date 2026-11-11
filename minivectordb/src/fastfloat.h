#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace vdb {

// Fast decimal -> double conversion.
//
// For decimals with at most 15 significant digits and a decimal exponent in
// [-22, 22] both the mantissa and 10^k are exactly representable in double,
// so a single IEEE division/multiplication yields the correctly rounded
// result - identical to strtod. Anything else falls back to strtod.
inline bool FastParseDouble(const char* p, const char* end, double* out, const char** next) {
  const char* s = p;
  bool neg = false;
  if (s < end && (*s == '+' || *s == '-')) {
    neg = (*s == '-');
    ++s;
  }
  if (s >= end || !(*s >= '0' && *s <= '9')) return false;

  uint64_t mant = 0;
  int digits = 0;
  int exp10 = 0;
  while (s < end && *s >= '0' && *s <= '9') {
    if (digits < 19) mant = mant * 10 + static_cast<uint64_t>(*s - '0');
    else ++exp10;
    ++digits;
    ++s;
  }
  if (s < end && *s == '.') {
    ++s;
    while (s < end && *s >= '0' && *s <= '9') {
      if (digits < 19) {
        mant = mant * 10 + static_cast<uint64_t>(*s - '0');
        --exp10;
      }
      ++digits;
      ++s;
    }
  }
  if (digits == 0) return false;
  if (s < end && (*s == 'e' || *s == 'E')) {
    const char* save = s;
    ++s;
    bool eneg = false;
    if (s < end && (*s == '+' || *s == '-')) {
      eneg = (*s == '-');
      ++s;
    }
    int e = 0;
    bool ed = false;
    while (s < end && *s >= '0' && *s <= '9') {
      if (e < 100000) e = e * 10 + (*s - '0');
      ed = true;
      ++s;
    }
    if (ed) {
      exp10 += eneg ? -e : e;
    } else {
      s = save;
    }
  }
  *next = s;
  if (digits <= 15 && exp10 >= -22 && exp10 <= 22) {
    static const double kPow10[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    double v = static_cast<double>(mant);
    if (exp10 >= 0) v *= kPow10[exp10];
    else v /= kPow10[-exp10];
    *out = neg ? -v : v;
    return true;
  }
  return false;  // caller falls back to strtod
}

}  // namespace vdb
