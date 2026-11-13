#pragma once
// Exact TopK accelerator: integer codes are exclusion certificates, never answers.
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace vdb { namespace safe_q8 {
constexpr size_t kBudget = 8u << 20;
constexpr int kMaxDim = 4096;
inline double Up(double x) { return std::nextafter(x, std::numeric_limits<double>::infinity()); }

struct Cache {
  uint32_t table = 0;
  int dim = 0;
  int exponent = 0;
  bool ready = false;
  std::vector<int8_t> codes;
  void Clear() {
    table = 0; dim = 0; exponent = 0; ready = false;
    std::vector<int8_t>().swap(codes);
  }
};

// Scaling a float32 by a power of two in the permitted exponent range is exact
// in binary64 (including input subnormals). Round integer ties away from zero;
// explicitly verify the half-step error instead of assuming a quantizer bound.
inline bool Encode(float x, int exponent, int8_t* code) {
  if (!std::isfinite(x)) return false;
  const double scaled = std::ldexp(static_cast<double>(x), -exponent);
  if (!std::isfinite(scaled) || std::fabs(scaled) > 127.0) return false;
  const double rounded = std::round(scaled);
  if (std::fabs(scaled - rounded) > 0.5 || std::fabs(rounded) > 127.0) return false;
  *code = static_cast<int8_t>(rounded);
  return true;
}

inline uint64_t SquaredScalar(const int8_t* a, const int8_t* b, int dim) {
  uint64_t sum = 0;
  for (int i = 0; i < dim; ++i) {
    const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
    sum += static_cast<unsigned>(d * d);
  }
  return sum;
}
#if defined(__x86_64__)
__attribute__((target("avx2"))) inline uint64_t SquaredAvx2(const int8_t* a, const int8_t* b, int dim) {
  __m256i acc = _mm256_setzero_si256();
  int i = 0;
  for (; i + 16 <= dim; i += 16) {
    const __m256i x = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)));
    const __m256i y = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)));
    const __m256i d = _mm256_sub_epi16(x, y);
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
  }
  // dim<=4096 and |d|<=254: all lanes and total fit signed int32.
  alignas(32) uint32_t lanes[8];
  _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
  uint64_t sum = 0;
  for (unsigned v : lanes) sum += v;
  for (; i < dim; ++i) { int d = int(a[i]) - int(b[i]); sum += unsigned(d * d); }
  return sum;
}
#endif
using CodeDistance = uint64_t (*)(const int8_t*, const int8_t*, int);
inline CodeDistance Resolve() {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) return &SquaredAvx2;
#endif
  return &SquaredScalar;
}

// Compare integer squared code distance S to a conservatively inflated cutoff.
// Each endpoint is within sqrt(dim)*step/2 of its reconstruction. If the exact
// scalar binary64 distance R is <= heap U, the real squared distance A satisfies
// A <= U/(1-gamma). gamma=2^-36 comfortably exceeds gamma_(dim+4) for dim<=4096
// (subtraction, multiplication and sequential accumulation, including FMA).
// Finite float32 squared differences are normal and finite in binary64.
// Outward rounding of every positive operation then yields
// S <= ((sqrt(U/(1-gamma)) + step*sqrt(dim))/step)^2.
// Only strict S>ceil(cutoff) is discarded. Equality and uncertain cases survive.
// Restrict to FE_TONEAREST and no FTZ/DAZ so original values keep their semantics.
inline bool EnvironmentSafe() {
  if (!std::numeric_limits<double>::is_iec559 || std::numeric_limits<double>::digits != 53 ||
      std::numeric_limits<float>::digits != 24) return false;
  if (std::fegetround() != FE_TONEAREST) return false;
#if defined(__x86_64__)
  if ((_mm_getcsr() & ((1u << 15) | (1u << 6))) != 0) return false;
#endif
  return true;
}
inline uint64_t Cutoff(double u, int exponent, int dim) {
  if (dim < 1 || dim > kMaxDim || exponent < -120 || exponent > 120 ||
      !(u >= 0) || !std::isfinite(u) || !EnvironmentSafe()) return UINT64_MAX;
  const double one_minus_gamma = 1.0 - std::ldexp(1.0, -36); // exact binary64
  const double root_u = Up(std::sqrt(Up(u / one_minus_gamma)));
  const double step = std::ldexp(1.0, exponent);
  const double errors = Up(step * Up(std::sqrt(static_cast<double>(dim))));
  const double radius = Up(root_u + errors);
  const double scaled = Up(radius / step);
  const double squared = Up(scaled * scaled);
  const uint64_t maximum = static_cast<uint64_t>(dim) * 254u * 254u;
  if (!std::isfinite(squared) || squared >= static_cast<double>(maximum)) return UINT64_MAX;
  return static_cast<uint64_t>(std::ceil(squared));
}
} } // namespace vdb::safe_q8
