#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

// The dialect forbids commas, quotes and newlines inside values. This allows
// delimiter discovery and quote validation to share a single SIMD scan.
namespace exactcsv {
using SV=std::string_view;
inline bool split(SV s,int n,SV* out){
 if(s.empty())return false;
 size_t from=0;
 for(int k=0;k<n;k++){
  size_t pos=s.find(',',from);
  if(k==n-1){if(pos!=SV::npos)return false;pos=s.size();}
  else if(pos==SV::npos)return false;
  SV v=s.substr(from,pos-from);
  if(!v.empty()&&v.front()=='"'){
   if(v.size()<2||v.back()!='"')return false;
   v.remove_prefix(1);v.remove_suffix(1);
  }
  if(v.find('"')!=SV::npos)return false;
  out[k]=v;from=pos+1;
 }
 return true;
}

inline bool next_scalar(const char*& p,const char* end,int n,SV* out){
 const char* first=p;
 const char* q=static_cast<const char*>(memchr(p,'\n',size_t(end-p)));
 if(!q)q=end;
 p=q==end?end:q+1;
 return split(SV(first,size_t(q-first)),n,out);
}

struct Masks{uint64_t commas,quotes,newlines;};
inline uint64_t below(unsigned n){return n==64?UINT64_MAX:((uint64_t(1)<<n)-1);}
__attribute__((always_inline)) inline bool finish(const char*& p,int n,SV* out,Masks a,Masks b){
 const char* first=p;
 unsigned len;
 if(a.newlines){len=std::countr_zero(a.newlines);uint64_t m=below(len);a.commas&=m;a.quotes&=m;b={};}
 else{unsigned tail=std::countr_zero(b.newlines);len=64+tail;uint64_t m=below(tail);b.commas&=m;b.quotes&=m;}
 p+=len+1;
 if(std::popcount(a.commas)+std::popcount(b.commas)!=n-1)return false;
 unsigned from=0,quoted=0;
 for(int k=0;k<n;k++){
  unsigned to=len;
  if(a.commas){to=std::countr_zero(a.commas);a.commas&=a.commas-1;}
  else if(b.commas){to=64+std::countr_zero(b.commas);b.commas&=b.commas-1;}
  SV value(first+from,to-from);
  if(!value.empty()&&value.front()=='"'){
   if(value.size()<2||value.back()!='"')return false;
   value.remove_prefix(1);value.remove_suffix(1);quoted+=2;
  }
  out[k]=value;from=to+1;
 }
 return quoted==unsigned(std::popcount(a.quotes)+std::popcount(b.quotes));
}

#if defined(__x86_64__)
inline Masks scan_sse2(const char* p){
 Masks m{};
 for(unsigned i=0;i<4;i++){
  __m128i x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(p+i*16));
  m.commas|=uint64_t(unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x,_mm_set1_epi8(',')))))<<(i*16);
  m.quotes|=uint64_t(unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x,_mm_set1_epi8('"')))))<<(i*16);
  m.newlines|=uint64_t(unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(x,_mm_set1_epi8('\n')))))<<(i*16);
 }
 return m;
}
template<int N=0>
inline bool next_sse2(const char*& p,const char* end,int n,SV* out){
 if(end-p<128)return next_scalar(p,end,n,out);
 Masks a=scan_sse2(p),b{};
 if(!a.newlines)b=scan_sse2(p+64);
 if(!(a.newlines|b.newlines))return next_scalar(p,end,n,out);
 return finish(p,N?N:n,out,a,b);
}
__attribute__((target("avx2"))) inline Masks scan_avx2(const char* p){
 Masks m{};
 for(unsigned i=0;i<2;i++){
  __m256i x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+i*32));
  m.commas|=uint64_t(uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x,_mm256_set1_epi8(',')))))<<(i*32);
  m.quotes|=uint64_t(uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x,_mm256_set1_epi8('"')))))<<(i*32);
  m.newlines|=uint64_t(uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x,_mm256_set1_epi8('\n')))))<<(i*32);
 }
 return m;
}
template<int N=0>
__attribute__((target("avx2,popcnt,bmi"))) inline bool next_avx2(const char*& p,const char* end,int n,SV* out){
 if(end-p<128)return next_scalar(p,end,n,out);
 Masks a=scan_avx2(p),b{};
 if(!a.newlines)b=scan_avx2(p+64);
 if(!(a.newlines|b.newlines))return next_scalar(p,end,n,out);
 return finish(p,N?N:n,out,a,b);
}
__attribute__((target("avx512f,avx512bw"))) inline Masks scan_avx512(const char* p){
 __m512i x=_mm512_loadu_si512(p);
 return {uint64_t(_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8(','))),
         uint64_t(_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('"'))),
         uint64_t(_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('\n')))};
}
template<int N=0>
__attribute__((target("avx512f,avx512bw,popcnt,bmi"))) inline bool next_avx512(const char*& p,const char* end,int n,SV* out){
 if(end-p<128)return next_scalar(p,end,n,out);
 Masks a=scan_avx512(p),b{};
 if(!a.newlines)b=scan_avx512(p+64);
 if(!(a.newlines|b.newlines))return next_scalar(p,end,n,out);
 return finish(p,N?N:n,out,a,b);
}
#elif defined(__aarch64__)
inline uint16_t mask16(uint8x16_t x){
 const uint8_t weights[16]={1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
 x=vandq_u8(x,vld1q_u8(weights));
 return uint16_t(vaddv_u8(vget_low_u8(x)))|(uint16_t(vaddv_u8(vget_high_u8(x)))<<8);
}
inline Masks scan_neon(const char* p){
 Masks m{};
 for(unsigned i=0;i<4;i++){
  uint8x16_t x=vld1q_u8(reinterpret_cast<const uint8_t*>(p+i*16));
  m.commas|=uint64_t(mask16(vceqq_u8(x,vdupq_n_u8(','))))<<(i*16);
  m.quotes|=uint64_t(mask16(vceqq_u8(x,vdupq_n_u8('"'))))<<(i*16);
  m.newlines|=uint64_t(mask16(vceqq_u8(x,vdupq_n_u8('\n'))))<<(i*16);
 }
 return m;
}
template<int N=0>
inline bool next_neon(const char*& p,const char* end,int n,SV* out){
 if(end-p<128)return next_scalar(p,end,n,out);
 Masks a=scan_neon(p),b{};
 if(!a.newlines)b=scan_neon(p+64);
 if(!(a.newlines|b.newlines))return next_scalar(p,end,n,out);
 return finish(p,N?N:n,out,a,b);
}
#endif

using Reader=bool (*)(const char*&,const char*,int,SV*);
template<int N=0>
inline Reader select_reader(){
#if defined(__x86_64__)
 __builtin_cpu_init();
 if(__builtin_cpu_supports("avx512bw")&&__builtin_cpu_supports("avx512f")&&__builtin_cpu_supports("bmi")&&__builtin_cpu_supports("popcnt"))return next_avx512<N>;
 if(__builtin_cpu_supports("avx2")&&__builtin_cpu_supports("bmi")&&__builtin_cpu_supports("popcnt"))return next_avx2<N>;
 return next_sse2<N>;
#elif defined(__aarch64__)
 return next_neon<N>;
#else
 return next_scalar;
#endif
}
inline const Reader next=select_reader<>();
inline const Reader events=select_reader<7>();
inline const Reader clicks=select_reader<4>();
} // namespace exactcsv
