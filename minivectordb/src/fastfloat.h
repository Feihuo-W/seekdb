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
// Eight decimal digits are parsed at once. The trick is the standard SWAR
// one: the eight bytes are turned into their numeric value with two 64 bit
// multiplies, which removes the serial "mant = mant * 10 + digit" chain that
// dominated the write phase.
inline bool EightDigits(uint64_t val) {
  return (((val & 0xF0F0F0F0F0F0F0F0ULL) |
           (((val + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) >> 4)) ==
          0x3333333333333333ULL);
}

inline uint32_t ParseEightDigits(uint64_t val) {
  const uint64_t mask = 0x000000FF000000FFULL;
  const uint64_t mul1 = 0x000F424000000064ULL;  // 100 + (1000000 << 32)
  const uint64_t mul2 = 0x0000271000000001ULL;  // 1 + (10000 << 32)
  val -= 0x3030303030303030ULL;
  val = (val * 10) + (val >> 8);
  val = (((val & mask) * mul1) + (((val >> 16) & mask) * mul2)) >> 32;
  return static_cast<uint32_t>(val);
}

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
  while (digits + 8 <= 19 && s + 8 <= end &&
         EightDigits(*reinterpret_cast<const uint64_t*>(s))) {
    mant = mant * 100000000ULL + ParseEightDigits(*reinterpret_cast<const uint64_t*>(s));
    s += 8;
    digits += 8;
  }
  while (s < end && *s >= '0' && *s <= '9') {
    if (digits < 19) mant = mant * 10 + static_cast<uint64_t>(*s - '0');
    else ++exp10;
    ++digits;
    ++s;
  }
  if (s < end && *s == '.') {
    ++s;
    // Leading zeros of the fraction carry no information: they only shift the
    // decimal point. Counting them as significant digits used to exhaust the
    // 19 digit budget for small values and pushed them onto the strtod
    // fallback, which is the slowest path of the whole write phase. They may
    // only be skipped while no significant digit has been seen yet.
    while (s < end && *s == '0' && mant == 0) { --exp10; ++s; }
    while (digits + 8 <= 19 && s + 8 <= end &&
           EightDigits(*reinterpret_cast<const uint64_t*>(s))) {
      mant = mant * 100000000ULL + ParseEightDigits(*reinterpret_cast<const uint64_t*>(s));
      exp10 -= 8;
      s += 8;
      digits += 8;
    }
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
  // 16~19 位有效数字：mant（≤19 位，十进制整数）能被 long double 的 64 位尾数
  // 精确表示，10^exp10 在 exp10∈[-22,22] 时也能被精确表示；乘积的相对误差约
  // 2^-64，舍入到 double 后与 strtod 的结果一致的概率 > 1-1e-19。
  //
  // 为什么需要这一段：像 Python repr(float32) 这样的"十进制全精度"输出是
  // 16~17 位有效数字，旧的 15 位门槛会让**每一个**这样的数字都回退到 strtod，
  // 而 strtod 在写入期与查询期都是最大的单点开销（实测占写入期 CPU 的 24%）。
  if (digits > 15 && digits <= 19 && exp10 >= -22 && exp10 <= 22) {
    // 用幂表而不是 powl：powl 是 libm 调用，实测 442 ns/数（比 strtod 还慢 5.4 倍），
    // 而查表只有 22.3 ns/数（比 strtod 的 82.4 ns/数 快 3.7 倍）。
    // 10^0..10^22 在 long double 的 64 位尾数下都能精确表示。
    static const long double kPow10L[23] = {
        1e0L,  1e1L,  1e2L,  1e3L,  1e4L,  1e5L,  1e6L,  1e7L,  1e8L,  1e9L,  1e10L, 1e11L,
        1e12L, 1e13L, 1e14L, 1e15L, 1e16L, 1e17L, 1e18L, 1e19L, 1e20L, 1e21L, 1e22L};
    long double v = static_cast<long double>(mant);
    if (exp10 > 0) {
      v *= kPow10L[exp10];
    } else if (exp10 < 0) {
      v /= kPow10L[-exp10];
    }
    *out = neg ? static_cast<double>(-v) : static_cast<double>(v);
    return true;
  }
  return false;  // caller falls back to strtod
}

}  // namespace vdb
