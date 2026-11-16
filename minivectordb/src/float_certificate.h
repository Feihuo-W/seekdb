#pragma once
#include <cfenv>
#include <cmath>
#include <limits>
#if defined(__x86_64__)
#include <immintrin.h>
#endif
namespace vdb_float_certificate {
inline double Up(double x){return std::nextafter(x,std::numeric_limits<double>::infinity());}
inline bool Enabled(int dim){
 if(dim<1||dim>4096||!std::numeric_limits<float>::is_iec559||!std::numeric_limits<double>::is_iec559||
    std::numeric_limits<float>::digits!=24||std::numeric_limits<double>::digits!=53||std::fegetround()!=FE_TONEAREST)return false;
#if defined(__x86_64__)
 if((_mm_getcsr()&((1u<<15)|(1u<<6)))!=0)return false;
#endif
 return true;
}
// Current float kernel uses rounded differences, nonnegative square/FMA and
// nonnegative reductions. For D<=4096, finite F satisfies F<=(1+gF)*A+delta,
// gF=32(D+16)u_float; delta=64(D+16)*FLT_MIN. These exceed a conservative
// 4D+64 operation/path bound, including gradual underflow. Inf/NaN never prune.
// Sequential-double R>= (1-2^-36)A. Thus R<=U implies
// F <= (1+gF)*U/(1-2^-36)+delta. Outward-round that expression in binary64.
// This bound is tied to the existing squared-difference kernel, NOT dot-product
// norm expansion, reassociation/fast-math or arbitrary approximate distances.
// Upper bound of the mathematical threshold with one relative margin instead
// of the chain of outward roundings; see the auxiliary code cutoff.
inline double CutoffBound(double u,int dim){
 if(!(u>=0)||!std::isfinite(u))return std::numeric_limits<double>::infinity();
 const double gf=std::ldexp(double(dim+16),-19);
 const double delta=std::ldexp(double(dim+16),-120);
 const double a=u/(1.0-std::ldexp(1.0,-36));
 const double bound=(1.0+gf)*a+delta;
 return bound*(1.0+std::ldexp(1.0,-40));
}
inline double Cutoff(double u,int dim){
 if(!Enabled(dim))return std::numeric_limits<double>::infinity();
 return CutoffBound(u,dim);
}
inline bool Reject(double approximate,double cutoff){
 return std::isfinite(approximate)&&approximate>=0.0&&approximate>cutoff;
}
}
