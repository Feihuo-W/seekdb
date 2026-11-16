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
  bool all_valid = true;
  std::vector<int8_t> codes;
  std::vector<uint8_t> valid_rows;
  void Clear() {
    table = 0; dim = 0; exponent = 0; ready = false; all_valid = true;
    std::vector<int8_t>().swap(codes);
    std::vector<uint8_t>().swap(valid_rows);
  }
};

// Scaling a float32 by a power of two in the permitted exponent range is exact
// in binary64 (including input subnormals). Round integer ties away from zero;
// explicitly verify the half-step error instead of assuming a quantizer bound.
// C36: 把缩放因子提出来，按 2 的幂缩放用一次乘法完成。
// scale 必须是 2^(-exponent)，且 exponent 被调用方限制在 [-120,120] ⇒ scale 是
// 精确可表示的 double，于是 x * scale 与 ldexp(x, -exponent) 逐位相同
// （正常范围内都精确；溢出都得到 ±inf；下溢到次正规数时按 IEEE 754 做同样的
// 就近舍入）。码值、剪枝判定与查询结果都不变。
inline bool EncodeScaled(float x, double scale, int8_t* code) {
  if (!std::isfinite(x)) return false;
  const double scaled = static_cast<double>(x) * scale;
  if (!std::isfinite(scaled) || std::fabs(scaled) > 127.0) return false;
  const double rounded = std::round(scaled);
  if (std::fabs(scaled - rounded) > 0.5 || std::fabs(rounded) > 127.0) return false;
  *code = static_cast<int8_t>(rounded);
  return true;
}

inline bool Encode(float x, int exponent, int8_t* code) {
  return EncodeScaled(x, std::ldexp(1.0, -exponent), code);
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
// C30：与 SquaredAvx2 逐位等价的 AVX2 内核，归约显式写成 SIMD 树形，
// 并提供"查询码已扩宽为 int16"的变体（省掉每行每 16 维一次 vpmovsxbw）。
#if defined(__x86_64__)
__attribute__((target("avx2"))) inline uint64_t SquaredAvx2Tree(
    const int8_t* a, const int8_t* b, int dim) {
  __m256i acc = _mm256_setzero_si256();
  int i = 0;
  for (; i + 16 <= dim; i += 16) {
    const __m256i x = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)));
    const __m256i y = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)));
    const __m256i d = _mm256_sub_epi16(x, y);
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(d, d));
  }
  const __m256i wlo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(acc));
  const __m256i whi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(acc, 1));
  const __m256i sums = _mm256_add_epi64(wlo, whi);
  __m128i pair = _mm_add_epi64(_mm256_castsi256_si128(sums),
                               _mm256_extracti128_si256(sums, 1));
  pair = _mm_add_epi64(pair, _mm_unpackhi_epi64(pair, pair));
  uint64_t sum = static_cast<uint64_t>(_mm_cvtsi128_si64(pair));
  for (; i < dim; ++i) { int d = int(a[i]) - int(b[i]); sum += unsigned(d * d); }
  return sum;
}
// C30: 查询码已扩宽为 int16 的版本（dim 必须是 16 的整数倍，否则回退到 SquaredAvx2Tree）。
__attribute__((target("avx2"))) inline void SquaredAvx2Tree2Wide(
    const int8_t* a, const int8_t* b2, const int16_t* q16, int dim, uint64_t* out0,
    uint64_t* out1) {
  __m256i acc0 = _mm256_setzero_si256();
  __m256i acc1 = _mm256_setzero_si256();
  for (int i = 0; i < dim; i += 16) {
    const __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q16 + i));
    const __m256i x0 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)));
    const __m256i x1 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b2 + i)));
    const __m256i d0 = _mm256_sub_epi16(x0, y);
    const __m256i d1 = _mm256_sub_epi16(x1, y);
    acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(d0, d0));
    acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(d1, d1));
  }
  const __m256i wlo0 = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(acc0));
  const __m256i whi0 = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(acc0, 1));
  const __m256i sums0 = _mm256_add_epi64(wlo0, whi0);
  __m128i pair0 = _mm_add_epi64(_mm256_castsi256_si128(sums0),
                                _mm256_extracti128_si256(sums0, 1));
  pair0 = _mm_add_epi64(pair0, _mm_unpackhi_epi64(pair0, pair0));
  *out0 = static_cast<uint64_t>(_mm_cvtsi128_si64(pair0));
  const __m256i wlo1 = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(acc1));
  const __m256i whi1 = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(acc1, 1));
  const __m256i sums1 = _mm256_add_epi64(wlo1, whi1);
  __m128i pair1 = _mm_add_epi64(_mm256_castsi256_si128(sums1),
                                _mm256_extracti128_si256(sums1, 1));
  pair1 = _mm_add_epi64(pair1, _mm_unpackhi_epi64(pair1, pair1));
  *out1 = static_cast<uint64_t>(_mm_cvtsi128_si64(pair1));
}
#endif
#if defined(__x86_64__)
// Preconditions: 1<=dim<=4096; original signed codes are in [-127,127].
// SSE2 sign-extension by unpacking each byte with its compare-generated sign.
// Differences fit int16, and the complete positive sum is <=264257536, hence
// neither madd pairs nor any int32 lane/merge overflow. No SSSE3/AVX required.
__attribute__((target("sse2"))) inline uint64_t SquaredSse2(
    const int8_t* a, const int8_t* b, int dim) {
  const __m128i zero = _mm_setzero_si128();
  __m128i acc_lo = zero, acc_hi = zero;
  int i = 0;
  for (; i + 16 <= dim; i += 16) {
    const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
    const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
    const __m128i sa = _mm_cmpgt_epi8(zero, va);
    const __m128i sb = _mm_cmpgt_epi8(zero, vb);
    const __m128i alo = _mm_unpacklo_epi8(va, sa);
    const __m128i ahi = _mm_unpackhi_epi8(va, sa);
    const __m128i blo = _mm_unpacklo_epi8(vb, sb);
    const __m128i bhi = _mm_unpackhi_epi8(vb, sb);
    const __m128i dlo = _mm_sub_epi16(alo, blo);
    const __m128i dhi = _mm_sub_epi16(ahi, bhi);
    acc_lo = _mm_add_epi32(acc_lo, _mm_madd_epi16(dlo, dlo));
    acc_hi = _mm_add_epi32(acc_hi, _mm_madd_epi16(dhi, dhi));
  }
  const __m128i acc = _mm_add_epi32(acc_lo, acc_hi);
  alignas(16) uint32_t lanes[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(lanes), acc);
  uint64_t sum = 0;
  for (unsigned lane : lanes) sum += lane;
  for (; i < dim; ++i) {
    const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
    sum += static_cast<unsigned>(d * d);
  }
  return sum;
}
#endif

// Runtime ISA selection resolved once per query instead of once per row.
inline bool UseAvx2() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

// Compile-time selected kernel. The scan loop is instantiated per ISA so a
// loop body never contains both kernels.
template <bool Avx2>
inline uint64_t SquaredFor(const int8_t* a, const int8_t* b, int dim) {
#if defined(__x86_64__)
  if constexpr (Avx2) return SquaredAvx2(a, b, dim);
  else return SquaredSse2(a, b, dim);
#else
  (void)Avx2;
  return SquaredScalar(a, b, dim);
#endif
}

using CodeDistance = uint64_t (*)(const int8_t*, const int8_t*, int);
inline CodeDistance Resolve() {
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) return &SquaredAvx2;
  return &SquaredSse2;
#else
  return &SquaredScalar;
#endif
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
// Upper bound of the mathematical threshold, with the outward rounding chain
// replaced by a single relative margin. The chain of outward roundings was
// worth about a hundred nanoseconds per call and was executed for every heap
// worst element change. The margin (2^-40) is far above the accumulated
// rounding error of the replaced chain (a few ulp), so the returned value is
// still an upper bound of the threshold and the screening stays valid.
inline uint64_t CutoffBound(double u, int exponent, int dim) {
  if (dim < 1 || dim > kMaxDim || exponent < -120 || exponent > 120 ||
      !(u >= 0) || !std::isfinite(u)) return UINT64_MAX;
  const double one_minus_gamma = 1.0 - std::ldexp(1.0, -36); // exact binary64
  const double root_u = std::sqrt(u / one_minus_gamma);
  const double step = std::ldexp(1.0, exponent);
  const double radius = root_u + step * std::sqrt(static_cast<double>(dim));
  const double scaled = radius / step;
  const double squared = scaled * scaled;
  const uint64_t maximum = static_cast<uint64_t>(dim) * 254u * 254u;
  if (!std::isfinite(squared) || squared >= static_cast<double>(maximum)) return UINT64_MAX;
  return static_cast<uint64_t>(std::ceil(squared * (1.0 + std::ldexp(1.0, -40))));
}

// Same bound, but the floating point environment is checked by the caller.
// The caller is the query that already checked it once before it enabled the
// auxiliary code path, and the environment cannot change inside one query.
inline uint64_t Cutoff(double u, int exponent, int dim) {
  if (!EnvironmentSafe()) return UINT64_MAX;
  return CutoffBound(u, exponent, dim);
}
} } // namespace vdb::safe_q8
