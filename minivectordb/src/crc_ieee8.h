#pragma once
#include <cstddef>
#include <cstdint>
namespace vdb_crc {
// Reflected IEEE CRC-32, polynomial 0xEDB88320; existing WAL bytes unchanged.
struct Tables {
  uint32_t t[8][256];
  constexpr Tables() : t{} {
    for (unsigned i=0;i<256;++i) {
      uint32_t c=i;
      for (int bit=0;bit<8;++bit) c=(c&1)?0xEDB88320u^(c>>1):c>>1;
      t[0][i]=c;
    }
    for (unsigned k=1;k<8;++k)
      for (unsigned i=0;i<256;++i) {
        uint32_t c=t[k-1][i]; t[k][i]=t[0][c&255]^(c>>8);
      }
  }
};
inline constexpr Tables tables{};
inline uint32_t Crc(const void* data,size_t n) {
 const uint8_t* p=static_cast<const uint8_t*>(data);
 uint32_t c=0xffffffffu;
 while(n>=8) {
   const uint32_t x=c^uint32_t(p[0])^(uint32_t(p[1])<<8)^(uint32_t(p[2])<<16)^(uint32_t(p[3])<<24);
   c=tables.t[7][x&255]^tables.t[6][(x>>8)&255]^tables.t[5][(x>>16)&255]^tables.t[4][x>>24]^
     tables.t[3][p[4]]^tables.t[2][p[5]]^tables.t[1][p[6]]^tables.t[0][p[7]];
   p+=8;n-=8;
 }
 while(n--) c=tables.t[0][(c^*p++)&255]^(c>>8);
 return c^0xffffffffu;
}
}
