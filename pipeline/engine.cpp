#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "native_parquet.hpp"
#include "csv_reader.hpp"
#ifdef _OPENMP
#include <omp.h>
#endif
using namespace std;
namespace fs = std::filesystem;
using SV = string_view;
static constexpr int64_t DAY=86400000LL, WEEK=7*DAY, HOUR=3600000LL, GAP=1800000LL;
struct Hash { using is_transparent=void; size_t operator()(SV s) const { return hash<SV>{}(s); } };
struct Eq {using is_transparent=void; bool operator()(SV a,SV b) const {return a==b;} };
static bool byte_less(SV a,SV b) {return lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),[](char x,char y){return (unsigned char)x<(unsigned char)y;});}
static bool digits(SV v) {if(v.empty())return false; for(unsigned char ch:v)if(ch<'0'||ch>'9')return false;return true;}
static inline bool product_number(SV s,uint32_t& out){
 if(s.size()<2||s.size()>7||s[0]!='p'||(s.size()>2&&s[1]=='0'))return false;
 uint32_t v=0;for(size_t i=1;i<s.size();i++){unsigned char c=s[i];if(c<'0'||c>'9')return false;v=v*10+(c-'0');}
 out=v;return true;
}
static inline bool campaign_number(SV s,uint32_t& out){
 if(s.size()<3||s.size()>8||s[0]!='c'||s[1]!='_'||(s.size()>3&&s[2]=='0'))return false;
 uint32_t v=0;for(size_t i=2;i<s.size();i++){unsigned char c=s[i];if(c<'0'||c>'9')return false;v=v*10+(c-'0');}
 out=v;return true;
}
static bool canonical_base36_id(SV s,char prefix,uint64_t& out){
 if(s.size()<2||s.size()>9||s[0]!=prefix||(s.size()>2&&s[1]=='0'))return false;
 static constexpr auto decode=[] {
  array<uint8_t,256> a{};a.fill(0xff);
  for(unsigned c='0';c<='9';c++)a[c]=c-'0';
  for(unsigned c='a';c<='z';c++)a[c]=c-'a'+10;
  return a;
 }();
 const auto* p=reinterpret_cast<const unsigned char*>(s.data()+1);
 uint64_t v=0;unsigned bad=0;
 // Pairwise decoding removes the per-character overflow test and the long
 // multiply dependency chain. Eight base-36 digits need one final range test.
 if(s.size()==9){
  unsigned a=decode[p[0]],b=decode[p[1]],c=decode[p[2]],d=decode[p[3]];
  unsigned e=decode[p[4]],f=decode[p[5]],g=decode[p[6]],h=decode[p[7]];
  bad=a|b|c|d|e|f|g|h;
  v=uint64_t(a*36+b)*2176782336ULL+uint64_t(c*36+d)*1679616ULL+
    uint64_t(e*36+f)*1296ULL+g*36+h;
 }else{
  for(size_t i=0;i<s.size()-1;i++){unsigned d=decode[p[i]];bad|=d;v=v*36+d;}
 }
 if((bad&0xc0)||v>=(1ULL<<40))return false;
 out=v;return true;
}
static int64_t dnum(const char* p,int n) {int64_t x=0; for(int i=0;i<n;i++){if(p[i]<'0'||p[i]>'9')return -1; x=x*10+p[i]-'0';} return x;}
static constexpr int64_t epoch_days(int y,int m,int d){ // Howard Hinnant: proleptic Gregorian
 y-=m<=2; int era=(y>=0 ? y:y-399)/400; unsigned yoe=(unsigned)(y-era*400); unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned yoe_days=yoe*365+yoe/4-yoe/100+doy; return int64_t(era)*146097+yoe_days-719468;
}
static constexpr auto year_starts=[] {
 array<int32_t,10000> a{};
 for(int y=0;y<10000;y++)a[y]=(int32_t)epoch_days(y,1,1);
 return a;
}();
static constexpr int month_starts[12]={0,31,59,90,120,151,181,212,243,273,304,334};
static inline bool leap_year(int y){return y%4==0&&(y%100!=0||y%400==0);}
static inline int32_t calendar_day(int y,int m,int d){
 return year_starts[y]+month_starts[m-1]+d-1+(m>2&&leap_year(y));
}
static bool parse_time(SV s,int64_t &val,const char* known_date=nullptr,int32_t known_day=0){
 // F1/F2 are fixed-width. Everything else can only be F3 and is validated in one pass.
 if((s.size()!=20 && s.size()!=25)||s[4]!='-'){
  if(s.empty())return false;
  uint64_t n=0;
  if(s.size()<=18){
   // Eighteen decimal digits always fit int64_t. Only longer inputs need a
   // per-digit overflow check (including arbitrarily many leading zeroes).
   for(unsigned char ch:s){if(ch<'0'||ch>'9')return false;n=n*10+(ch-'0');}
   val=(int64_t)n;return true;
  }
  for(unsigned char ch:s){
   if(ch<'0'||ch>'9')return false;
   unsigned d=ch-'0';if(n>(uint64_t(INT64_MAX)-d)/10)return false;n=n*10+d;
  }
  val=(int64_t)n;return true;
 }
 const char* p=s.data();
 if(p[4]!='-'||p[7]!='-'||p[10]!='T'||p[13]!=':'||p[16]!=':')return false;
 if(s.size()==20 && p[19]!='Z')return false;
 if(s.size()==25 && ((p[19]!='+'&&p[19]!='-')||p[22]!=':'))return false;
 int h=dnum(p+11,2),mi=dnum(p+14,2),se=dnum(p+17,2);
 if(h>23||mi>59||se>59||h<0||mi<0||se<0)return false;
 int32_t day;
 if(known_date&&memcmp(p,known_date,10)==0){
  // ingest_ts is guaranteed valid F1. An identical date is already validated.
  day=known_day;
 }else{
  int y=dnum(p,4),m=dnum(p+5,2),d=dnum(p+8,2);
  if(y<1||m<1||m>12||d<1)return false;
  constexpr int md[12]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(d>md[m-1]+(m==2&&leap_year(y)))return false;
  day=calendar_day(y,m,d);
 }
 int offset=0;
 if(s.size()==25){int oh=dnum(p+20,2),om=dnum(p+23,2);if(oh>23||om>59||oh<0||om<0)return false;offset=oh*60+om;if(p[19]=='-')offset=-offset;}
 val=(day*86400LL + h*3600+mi*60+se-int64_t(offset)*60)*1000;
 return true;
}
static int32_t epoch_day(int64_t ts){int64_t d=ts/DAY; if(ts<0 && ts%DAY)d--;return (int32_t)d;}
struct Mapping { const char* p=nullptr;size_t n=0; Mapping()=default;
 Mapping(const fs::path& path){int fd=open(path.c_str(),O_RDONLY);if(fd<0){perror("open");exit(2);}struct stat st{};if(fstat(fd,&st)){perror("stat");exit(2);}n=st.st_size;if(n){void* m=mmap(nullptr,n,PROT_READ,MAP_PRIVATE,fd,0);if(m==MAP_FAILED){perror("mmap");exit(2);}p=(char*)m;}close(fd);}
 Mapping(const Mapping&)=delete;Mapping& operator=(const Mapping&)=delete;
 Mapping(Mapping&&o)noexcept:p(o.p),n(o.n){o.p=nullptr;o.n=0;}
 Mapping&operator=(Mapping&&o)noexcept{if(p)munmap((void*)p,n);p=o.p;n=o.n;o.p=nullptr;o.n=0;return *this;}
 ~Mapping(){if(p)munmap((void*)p,n);}
};
static vector<fs::path> input_files(const string& dir){vector<fs::path> files; if(!fs::exists(dir))return files;for(auto&e:fs::directory_iterator(dir))if(e.is_regular_file()&&e.path().extension()==".csv")files.push_back(e.path());sort(files.begin(),files.end());return files;}
static bool split_csv(SV s,int n,SV *out){
 if(s.empty())return false;
 if(memchr(s.data(),'"',s.size())==nullptr){
  size_t from=0;
  for(int k=0;k<n;k++){
   size_t pos=s.find(',',from);
   if(k==n-1){if(pos!=SV::npos)return false;pos=s.size();}
   else if(pos==SV::npos)return false;
   out[k]=s.substr(from,pos-from);from=pos+1;
  }
  return true;
 }
 size_t from=0;for(int k=0;k<n;k++){
  size_t pos=s.find(',',from);
  if(k==n-1){if(pos!=SV::npos)return false;pos=s.size();}
  else if(pos==SV::npos)return false;
  SV v=s.substr(from,pos-from);
  if(!v.empty()&&v.front()=='"'){if(v.size()<2||v.back()!='"')return false;v.remove_prefix(1);v.remove_suffix(1);}
  if(v.find('"')!=SV::npos)return false;
  out[k]=v;from=pos+1;
 }
 return true;
}
template<class F>static void foreach_csv(const Mapping&m,int n,F action){
 if(!m.n)return;
 const char *p=m.p,*end=m.p+m.n;
 const char *first=(const char*)memchr(p,'\n',end-p);if(!first)return;p=first+1; // header
 SV fields[7];
 while(p<end){
  if(exactcsv::next(p,end,n,fields))action(fields);
 }
}
static void advise_huge_pages(void* pointer,size_t bytes,bool collapse=false){
#if defined(__linux__) && defined(MADV_HUGEPAGE)
 if(bytes<(4u<<20))return;
 long native_page=sysconf(_SC_PAGESIZE);if(native_page<=0)return;
 size_t page=(size_t)native_page;
 uintptr_t begin=(uintptr_t)pointer,end=begin+bytes;
 begin=(begin+page-1)/page*page;end=end/page*page;
 if(begin>=end)return;
 madvise((void*)begin,end-begin,MADV_HUGEPAGE);
#ifdef MADV_COLLAPSE
 if(collapse)madvise((void*)begin,end-begin,MADV_COLLAPSE);
#endif
#endif
}
// Canonical 40-bit IDs and small internal indexes share one cache-line-friendly
// slot. Larger indexes transparently switch to separate full-width values.
struct PackedNumPeople {
 static constexpr uint64_t KEY_MASK=(1ULL<<40)-1;
 vector<uint64_t> slots;
 vector<uint32_t> wide_values;
 size_t used=0;
 explicit PackedNumPeople(size_t expected=16){size_t cap=16;while(cap<expected*10/7+16)cap*=2;slots.resize(cap);}
 static size_t hash_key(uint64_t key){return (key+1)*0x9E3779B97F4A7C15ULL;}
 uint64_t key_at(size_t i)const{return wide_values.empty()?(slots[i]&KEY_MASK):slots[i]-1;}
 uint32_t value_at(size_t i)const{return wide_values.empty()?uint32_t(slots[i]>>40)-1:wide_values[i];}
 void widen(){
  wide_values.resize(slots.size());
  for(size_t i=0;i<slots.size();i++)if(slots[i]){
   wide_values[i]=uint32_t(slots[i]>>40)-1;slots[i]=(slots[i]&KEY_MASK)+1;
  }
 }
 void set_at(size_t i,uint64_t key,uint32_t value){
  if(wide_values.empty()&&value>=0xffffffU)widen();
  if(wide_values.empty())slots[i]=key|(uint64_t(value+1)<<40);
  else{slots[i]=key+1;wide_values[i]=value;}
 }
 void grow(){
  PackedNumPeople bigger;bigger.slots.assign(slots.size()*2,0);
  if(!wide_values.empty())bigger.wide_values.resize(bigger.slots.size());
  for(size_t i=0;i<slots.size();i++)if(slots[i])bigger.add(key_at(i),value_at(i));
  *this=std::move(bigger);
 }
 uint32_t find(uint64_t key)const{
  size_t p=hash_key(key)&(slots.size()-1);
  while(slots[p]){if(key_at(p)==key)return value_at(p);p=(p+1)&(slots.size()-1);}
  return UINT32_MAX;
 }
 void add(uint64_t key,uint32_t value){
  if(used*10>=slots.size()*7)grow();
  size_t p=hash_key(key)&(slots.size()-1);
  while(slots[p]){if(key_at(p)==key){set_at(p,key,value);return;}p=(p+1)&(slots.size()-1);}
  set_at(p,key,value);used++;
 }
 void remap(const vector<uint32_t>& values){
#ifdef _OPENMP
#pragma omp parallel for if(slots.size()>65536)
#endif
  for(size_t i=0;i<slots.size();i++)if(slots[i])set_at(i,key_at(i),values[value_at(i)]);
 }
};
// This cache is scoped to a phase with an immutable identity index. Separate
// instances after standalone interning prevent stale negative click lookups.
struct IdentityCache {
 static constexpr size_t SIZE=2048;
 array<uint64_t,SIZE> keys{};
 array<uint32_t,SIZE> values{};
 uint32_t find(const PackedNumPeople& index,uint64_t key){
  size_t slot=key&(SIZE-1);uint64_t stored=key+1;
  if(keys[slot]==stored)return values[slot];
  uint32_t value=index.find(key);keys[slot]=stored;values[slot]=value;return value;
 }
};
struct Node{int parent,weight;SV value;};
struct UnionFind{
 vector<Node> nodes;vector<uint32_t> buckets;vector<uint32_t> special_nodes;size_t used=0;
 PackedNumPeople numeric;
 explicit UnionFind(size_t expected):numeric(expected){nodes.reserve(expected);buckets.assign(1024,0);}
 void grow(){
  vector<uint32_t> nb(buckets.size()*2,0);size_t mask=nb.size()-1;
  for(uint32_t v:buckets)if(v){size_t p=Hash{}(nodes[v-1].value)&mask;while(nb[p])p=(p+1)&mask;nb[p]=v;}
  buckets.swap(nb);
 }
 int add(SV s){
  uint64_t key;
  if(canonical_base36_id(s,'u',key)){
   uint32_t found=numeric.find(key);if(found!=UINT32_MAX)return (int)found;
   int id=(int)nodes.size();nodes.push_back({id,1,s});numeric.add(key,(uint32_t)id);return id;
  }
  if(used*10>=buckets.size()*7)grow();size_t p=Hash{}(s)&(buckets.size()-1);
  while(buckets[p]){uint32_t v=buckets[p];if(nodes[v-1].value==s)return (int)v-1;p=(p+1)&(buckets.size()-1);}
  int id=(int)nodes.size();nodes.push_back({id,1,s});buckets[p]=(uint32_t)id+1;special_nodes.push_back((uint32_t)id);used++;return id;
 }
 int root(int i){int j=i;while(nodes[j].parent!=j)j=nodes[j].parent;while(nodes[i].parent!=i){int x=nodes[i].parent;nodes[i].parent=j;i=x;}return j;}
 void join(SV a,SV b){int x=root(add(a)),y=root(add(b));if(x==y)return;if(nodes[x].weight<nodes[y].weight)swap(x,y);nodes[y].parent=x;nodes[x].weight+=nodes[y].weight;}
};
struct LinkWork{const char* begin;const char* end;};
static int concurrent_root(vector<Node>& nodes,int i){
 for(;;){
  int p=__atomic_load_n(&nodes[i].parent,__ATOMIC_ACQUIRE);
  if(p==i)return i;
  int grand=__atomic_load_n(&nodes[p].parent,__ATOMIC_ACQUIRE);
  __atomic_compare_exchange_n(&nodes[i].parent,&p,grand,true,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
  i=p;
 }
}
static UnionFind build_links(vector<Mapping>& maps,const vector<fs::path>& paths,int ncpu){
 vector<LinkWork> chunks;size_t raw_nodes=0,bytes=0,first_map=maps.size();
 for(const auto& path:paths){
  maps.emplace_back(path);const auto& m=maps.back();bytes+=m.n;
  if(!m.n)continue;
  const char* end=m.p+m.n;
  const char* header=static_cast<const char*>(memchr(m.p,'\n',m.n));
  if(!header)continue;
  const char* first=header+1;if(first==end)continue;
  raw_nodes+=2*(size_t(std::count(first,end,'\n'))+(end[-1]!='\n'));
  size_t span=end-first,np=max<size_t>(1,(span+262143)/262144);
  const char* begin=first;
  for(size_t part=1;part<=np;part++){
   const char* stop=end;
   if(part<np){const char* at=first+span*part/np;const char* q=static_cast<const char*>(memchr(at,'\n',end-at));if(q)stop=q+1;}
   if(begin<stop)chunks.push_back({begin,stop});begin=stop;
  }
 }
 // Reserve enough slots for every raw endpoint before any worker starts.
 // The serial implementation remains the exact fallback for wider indexes.
 bool parallel=ncpu>1&&raw_nodes>20000&&raw_nodes<0xfffffeU;
 UnionFind dsu(max<size_t>(256,parallel?raw_nodes:bytes/16));
 if(!parallel){
  for(size_t i=first_map;i<maps.size();i++)foreach_csv(maps[i],2,[&](SV* f){if(!f[0].empty()&&!f[1].empty())dsu.join(f[0],f[1]);});
  return dsu;
 }
 dsu.nodes.resize(raw_nodes);
 alignas(64) atomic<uint32_t> next_node(0);
 unordered_map<SV,uint32_t,Hash,Eq> unusual;unusual.reserve(raw_nodes/1000+16);
 mutex unusual_mutex;
 auto intern=[&](SV value)->int{
  uint64_t key;
  if(canonical_base36_id(value,'u',key)){
   size_t pos=PackedNumPeople::hash_key(key)&(dsu.numeric.slots.size()-1);
   for(;;){
    auto* slot=&dsu.numeric.slots[pos];
    uint64_t entry=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!entry){
     uint64_t pending=key|(0xffffffULL<<40);
     if(__atomic_compare_exchange_n(slot,&entry,pending,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
      uint32_t id=next_node.fetch_add(1,memory_order_relaxed);
      dsu.nodes[id]={(int)id,1,value};
      __atomic_store_n(slot,key|(uint64_t(id+1)<<40),__ATOMIC_RELEASE);
      return (int)id;
     }
    }
    if((entry&PackedNumPeople::KEY_MASK)==key){
     while((entry>>40)==0xffffffULL)entry=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
     return int(entry>>40)-1;
    }
    pos=(pos+1)&(dsu.numeric.slots.size()-1);
   }
  }
  lock_guard<mutex> lock(unusual_mutex);
  auto it=unusual.find(value);if(it!=unusual.end())return (int)it->second;
  uint32_t id=next_node.fetch_add(1,memory_order_relaxed);
  dsu.nodes[id]={(int)id,1,value};unusual.emplace(value,id);return (int)id;
 };
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t chunk=0;chunk<chunks.size();chunk++){
  const char* p=chunks[chunk].begin;const char* end=chunks[chunk].end;SV f[2];
  while(p<end)if(exactcsv::next(p,end,2,f)&&!f[0].empty()&&!f[1].empty()){
   int x=intern(f[0]),y=intern(f[1]);
   for(;;){
    x=concurrent_root(dsu.nodes,x);y=concurrent_root(dsu.nodes,y);
    if(x==y)break;
    if(x>y)swap(x,y);
    int expected=y;
    if(__atomic_compare_exchange_n(&dsu.nodes[y].parent,&expected,x,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
   }
  }
 }
 dsu.nodes.resize(next_node.load(memory_order_relaxed));dsu.numeric.used=dsu.nodes.size()-unusual.size();
 for(const auto& [value,id]:unusual)dsu.special_nodes.push_back(id);
 return dsu;
}
static constexpr uint64_t NONCOMPACT_PAYLOAD=1ULL<<62, NONCANON_ID=1ULL<<63, MASK40=(1ULL<<40)-1;
struct Cand{uint64_t x0,x1,idtag;}; // 24 bytes: compact generated payload or exact wide fallback
static inline SV cand_line(const Cand& c){return SV((const char*)(uintptr_t)c.x0,(size_t)(c.x1&0xffffffULL));}
static inline SV cand_eventid(const Cand& c){SV f[7];return split_csv(cand_line(c),7,f)?f[0]:SV{};}
static inline uint64_t cand_user_key(const Cand& c){return c.x0&MASK40;}
static inline uint32_t cand_time(const Cand& c){return (uint32_t)((c.x0>>40)|((c.x1&0x7fULL)<<24));}
static inline int32_t cand_price(const Cand& c){return (int32_t)((c.x1>>7)&0xffffffULL);}
static inline uint16_t cand_category(const Cand& c){return (uint16_t)((c.x1>>31)&0xffULL);}
static inline uint8_t cand_type(const Cand& c){return (uint8_t)((c.x1>>39)&3ULL);}
static inline bool cand_product_empty(const Cand& c){return ((c.x1>>41)&1ULL)!=0;}
static inline int64_t sign40(uint64_t x){return (int64_t)(x<<24)>>24;}
static inline int64_t cand_ingest_ms(const Cand& c,int64_t ws){
 if((c.idtag&(NONCANON_ID|NONCOMPACT_PAYLOAD))==0){
  uint64_t sec=(c.x1>>42)&0x3fffffULL;
  return ws-WEEK+(int64_t)sec*1000;
 }
 return sign40(c.x1>>24)*1000;
}
static string canonical_user_name(uint64_t v){
 char b[16];int at=15;b[at--]=0;static constexpr char d[]="0123456789abcdefghijklmnopqrstuvwxyz";
 do{b[at--]=d[v%36];v/=36;}while(v);b[at]='u';return string(b+at);
}
struct Evt{uint32_t t;uint32_t person;int32_t price;uint16_t category;uint8_t is_purchase;uint8_t unused;};
struct Click{SV clickid;uint32_t t;uint32_t person;uint16_t campaign;uint16_t unused=0;};
struct ClickCand{SV clickid;uint64_t idtag;uint32_t t;uint32_t person;uint16_t campaign;uint16_t unused=0;};
struct UserDay{uint32_t person;int32_t day;uint32_t sessions=0, events=0;int64_t revenue=0;uint16_t top_category=UINT16_MAX;};
struct Session{int64_t start;int64_t revenue;uint32_t person;uint32_t userday;uint16_t credited=0;uint8_t purchased=0;uint8_t unused=0;};
struct Buy{uint32_t userday;uint16_t category;int32_t price;};
struct PendingEvt{SV user;uint32_t t;int32_t price;uint32_t user_hash;uint16_t category;uint8_t is_purchase;uint8_t unused=0;};
struct PendingNum{uint64_t key;uint32_t t;int32_t price;uint16_t category;uint8_t is_purchase;uint8_t unused=0;};
struct CampDay{int64_t sessions=0,purchasing=0,revenue=0,assists=0;};
// Open-addressing dedup index: cache-friendly, compact, collision-safe.
// An entry stores the 1-based index into Cand; full keys are compared on match.
struct FastPeople {
 struct Slot {SV key; uint32_t person=0;};
 vector<Slot> buckets;
 size_t used=0;
 vector<SV>& people;
 explicit FastPeople(vector<SV>& names,size_t expected):people(names) {
  size_t cap=16;while(cap < expected * 2)cap <<= 1;
  buckets.resize(cap);
 }
 void grow() {
  vector<Slot> bigger(buckets.size()*2);
  size_t mask=bigger.size()-1;
  for(const auto& e:buckets)if(!e.key.empty()){
    size_t i=Hash{}(e.key)&mask;
    while(!bigger[i].key.empty())i=(i+1)&mask;
    bigger[i]=e;
  }
  buckets.swap(bigger);
 }
 void add_link(SV key,uint32_t person) {
  if(used*10>=buckets.size()*7)grow();
  size_t p=Hash{}(key)&(buckets.size()-1);
  while(!buckets[p].key.empty())p=(p+1)&(buckets.size()-1);
  buckets[p]={key,person};used++;
 }
 uint32_t find_hashed(SV key,size_t h) const {
  size_t p=h&(buckets.size()-1);
  while(!buckets[p].key.empty()) {
    if(buckets[p].key==key)return buckets[p].person;
    p=(p+1)&(buckets.size()-1);
  }
  return UINT32_MAX;
 }
 uint32_t find_only(SV key) const {return find_hashed(key,Hash{}(key));}
 uint32_t get(SV key) {
  if(used*10>=buckets.size()*7)grow();
  size_t p=Hash{}(key)&(buckets.size()-1);
  while(!buckets[p].key.empty()) {
    if(buckets[p].key==key)return buckets[p].person;
    p=(p+1)&(buckets.size()-1);
  }
  uint32_t id=(uint32_t)people.size();
  people.push_back(key);
  buckets[p]={key,id}; used++;
  return id;
 }
};
struct FlatEventIndex {
 vector<uint64_t> packed;
 vector<uint32_t> wide;
 size_t used=0;
 vector<Cand>& rows;
 unsigned partition_bits;
 explicit FlatEventIndex(vector<Cand>& v,size_t expected,unsigned bits,bool force_wide=false):rows(v),partition_bits(bits){
  size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;
  if(expected<0xffffffU&&!force_wide)packed.resize(cap);else wide.resize(cap);
 }
 void widen(){
  wide.resize(packed.size());
  for(size_t i=0;i<packed.size();i++)wide[i]=uint32_t(packed[i]>>40);
  vector<uint64_t>().swap(packed);
 }
 uint32_t get_or_create(const Cand& c){
  if(!packed.empty()&&rows.size()>=0xffffffU)widen();
  size_t capacity=packed.empty()?wide.size():packed.size();
  if(used*10>=capacity*7){grow();capacity=packed.empty()?wide.size():packed.size();}
  uint64_t key=c.idtag&~NONCOMPACT_PAYLOAD;size_t mask=capacity-1,p=(size_t)(key>>partition_bits)&mask;
  if(!packed.empty()){
   for(;;){
    uint64_t slot=packed[p];
    if(!slot){packed[p]=(uint64_t(rows.size()+1)<<40)|(key&MASK40);used++;return UINT32_MAX;}
    // Keep the key beside the row index. Ordinary hash collisions no longer
    // fetch a random candidate cache line just to inspect its event ID.
    if((slot&MASK40)==(key&MASK40)){
     uint32_t at=uint32_t(slot>>40)-1;const auto& old=rows[at];
     if(key==(old.idtag&~NONCOMPACT_PAYLOAD)&&((key&NONCANON_ID)==0||cand_eventid(c)==cand_eventid(old)))return at;
    }
    p=(p+1)&mask;
   }
  }
  for(;;){
   uint32_t value=wide[p];
   if(!value){wide[p]=(uint32_t)rows.size()+1;used++;return UINT32_MAX;}
   const auto& old=rows[value-1];
   if(key==(old.idtag&~NONCOMPACT_PAYLOAD)&&((key&NONCANON_ID)==0||cand_eventid(c)==cand_eventid(old)))return value-1;
   p=(p+1)&mask;
  }
 }
 void grow(){
  if(!packed.empty()){
   vector<uint64_t> bigger(packed.size()*2);size_t mask=bigger.size()-1;
   for(uint64_t slot:packed)if(slot){
    uint32_t at=uint32_t(slot>>40)-1;size_t p=(size_t)((rows[at].idtag&~NONCOMPACT_PAYLOAD)>>partition_bits)&mask;
    while(bigger[p])p=(p+1)&mask;bigger[p]=slot;
   }
   packed.swap(bigger);
  }else{
   vector<uint32_t> bigger(wide.size()*2);size_t mask=bigger.size()-1;
   for(uint32_t value:wide)if(value){
    size_t p=(size_t)((rows[value-1].idtag&~NONCOMPACT_PAYLOAD)>>partition_bits)&mask;
    while(bigger[p])p=(p+1)&mask;bigger[p]=value;
   }
   wide.swap(bigger);
  }
 }
};
struct FlatClickCandIndex {
 vector<uint32_t> buckets;
 vector<Click>& rows;
 vector<uint64_t>& tags;
 unsigned partition_bits;
 FlatClickCandIndex(vector<Click>& r,vector<uint64_t>& k,size_t expected,unsigned bits):rows(r),tags(k),partition_bits(bits){
  size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;buckets.resize(cap);
 }
 bool insert(const ClickCand& c){
  size_t p=(size_t)(c.idtag>>partition_bits)&(buckets.size()-1);
  while(buckets[p]){
   size_t at=buckets[p]-1;
   if(c.idtag==tags[at]){
    if((c.idtag>>63)==0 || c.clickid==rows[at].clickid)return false;
   }
   p=(p+1)&(buckets.size()-1);
  }
  buckets[p]=(uint32_t)rows.size()+1;
  return true;
 }
};
static int64_t parse_ingest(SV s,int32_t& day){
 // rules.md guarantees ingest_ts is valid F1; avoid the generic validator on every raw event.
 const char* p=s.data();
 int y=(p[0]-'0')*1000+(p[1]-'0')*100+(p[2]-'0')*10+(p[3]-'0');
 int mo=(p[5]-'0')*10+(p[6]-'0'), d=(p[8]-'0')*10+(p[9]-'0');
 int h=(p[11]-'0')*10+(p[12]-'0'), mi=(p[14]-'0')*10+(p[15]-'0'), se=(p[17]-'0')*10+(p[18]-'0');
 day=calendar_day(y,mo,d);
 return (day*86400LL+h*3600+mi*60+se)*1000;
}
static int32_t parse_price(SV s) {
 if(s.empty())return -1;
 int32_t v=0;
 for(unsigned char c:s){if(c<'0'||c>'9')return -1;v=v*10+(c-'0');if(v>10000000)return -1;}
 return v>0?v:-1;
}
static void fail(const string&s){cerr<<s<<'\n';exit(1);}
template<class T,class Key>
static void radix64(vector<T>& a, Key key, int passes=4){
 if(a.size()<2)return;
 constexpr size_t R=1u<<16, MASK=R-1;
 int nt=1;
#ifdef _OPENMP
 nt=max(1,omp_get_max_threads());
#endif
 if(a.size()<32768){sort(a.begin(),a.end(),[&](const T& x,const T& y){return key(x)<key(y);});return;}
 vector<T> tmp(a.size());
 vector<size_t> hist((size_t)nt*R), pos((size_t)nt*R);
 for(int pass=0;pass<passes;pass++){
  fill(hist.begin(),hist.end(),0);
  unsigned shift=pass*16;
  int active=nt;
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
   int tid=0;
#ifdef _OPENMP
   tid=omp_get_thread_num();
#pragma omp single
   active=omp_get_num_threads();
#endif
   size_t lo=a.size()*(size_t)tid/active, hi=a.size()*(size_t)(tid+1)/active;
   size_t* h=hist.data()+(size_t)tid*R;
   for(size_t i=lo;i<hi;i++)h[(key(a[i])>>shift)&MASK]++;
  }
  size_t base=0;
  for(size_t b=0;b<R;b++){
   size_t total=0;
   for(int t=0;t<active;t++)total+=hist[(size_t)t*R+b];
   size_t p=base;
   for(int t=0;t<active;t++){pos[(size_t)t*R+b]=p;p+=hist[(size_t)t*R+b];}
   base+=total;
  }
#ifdef _OPENMP
#pragma omp parallel num_threads(active)
#endif
  {
   int tid=0;
#ifdef _OPENMP
   tid=omp_get_thread_num();
#endif
   size_t lo=a.size()*(size_t)tid/active, hi=a.size()*(size_t)(tid+1)/active;
   size_t* p=pos.data()+(size_t)tid*R;
   for(size_t i=lo;i<hi;i++){size_t b=(key(a[i])>>shift)&MASK;tmp[p[b]++]=a[i];}
  }
  a.swap(tmp);
 }
}
static void order_events(vector<Evt>& es){
 radix64(es,[](const Evt&e){return (uint64_t(e.person)<<32)|e.t;},4);
}
static void order_clicks(vector<Click>& cs,size_t person_count){
 if(cs.size()<2)return;
 // Dense person bucket + tiny local time sorts. This avoids a four-pass global radix
 // over the click array while preserving contiguous (person,time) order exactly.
 vector<size_t> off(person_count+1,0);
 for(const Click& c:cs)off[(size_t)c.person+1]++;
 for(size_t p=1;p<off.size();p++)off[p]+=off[p-1];
 vector<size_t> pos=off;
 vector<Click> tmp(cs.size());
 for(const Click& c:cs)tmp[pos[c.person]++]=c;
 cs.swap(tmp);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,256)
#endif
 for(size_t p=0;p<person_count;p++){
  size_t lo=off[p],hi=off[p+1],n=hi-lo;
  if(n<2)continue;
  Click* q=cs.data()+lo;
  if(n<=16){
   for(size_t i=1;i<n;i++){
    Click v=q[i];size_t j=i;
    while(j&&v.t<q[j-1].t){q[j]=q[j-1];--j;}
    q[j]=v;
   }
  }else sort(q,q+n,[](const Click&a,const Click&b){return a.t<b.t;});
 }
}
int main(){
 const string data=getenv("DATA_DIR")?getenv("DATA_DIR"):"/data";
 const string out=getenv("OUTPUT_DIR")?getenv("OUTPUT_DIR"):"/output";
 int ncpu=max(1,getenv("NCPU")?atoi(getenv("NCPU")):8);
#ifdef _OPENMP
 omp_set_dynamic(0);
 omp_set_num_threads(ncpu);
#endif
 auto t0=chrono::steady_clock::now();
 auto mark=t0;
 auto lap=[&](const char* name){auto now=chrono::steady_clock::now();cerr<<"phase "<<name<<"="<<chrono::duration<double>(now-mark).count()<<"s\n";mark=now;};
 // Manifest: strict ISO timestamps at start and end. No JSON dependency.
 ifstream manifest(data+"/manifest.json"); string json((istreambuf_iterator<char>(manifest)),{});
 auto field=[&](const string&key){auto p=json.find('"'+key+'"');if(p==string::npos)fail("manifest missing "+key);p=json.find(':',p);p=json.find('"',p);auto q=json.find('"',p+1);return json.substr(p+1,q-p-1);};
 int64_t ws,we; if(!parse_time(field("window_start"),ws)||!parse_time(field("window_end"),we))fail("bad manifest");
 const int32_t day_base=epoch_day(ws);const int nday=epoch_day(we-1)-day_base+1;
 // Read clean dimensions.
 vector<Mapping> maps;maps.reserve(300);
 unordered_map<string,uint16_t,Hash,Eq> cat_ids;
 vector<string> categories;
 auto cat_id=[&](SV s){auto it=cat_ids.find(s);if(it!=cat_ids.end())return it->second;uint16_t id=categories.size();categories.push_back(string(s));cat_ids.emplace(categories.back(),id);return id;};
 unordered_map<string,uint16_t,Hash,Eq> products;products.reserve(1024);
 array<uint16_t,100000> product_fast;product_fast.fill(UINT16_MAX);
 {
  Mapping m(data+"/products.csv");foreach_csv(m,2,[&](SV*f){
   uint16_t cat=cat_id(f[1]);uint32_t n;
   if(product_number(f[0],n)&&n<product_fast.size()){
    if(product_fast[n]==UINT16_MAX)product_fast[n]=cat;
   }else products.emplace(string(f[0]),cat);
  });
 }
 uint16_t unknown=cat_id("unknown");
 vector<string> campaigns,channels;
 unordered_map<string,uint16_t,Hash,Eq> campaign_ids;
 array<uint16_t,10000> campaign_fast;campaign_fast.fill(UINT16_MAX);
 {
  Mapping m(data+"/campaigns.csv");foreach_csv(m,2,[&](SV*f){
   uint16_t id=campaigns.size();campaigns.push_back(string(f[0]));channels.push_back(string(f[1]));uint32_t n;
   if(campaign_number(f[0],n)&&n<campaign_fast.size()){
    if(campaign_fast[n]==UINT16_MAX)campaign_fast[n]=id;
   }else campaign_ids.emplace(campaigns.back(),id);
  });
 }
 auto campaign_for=[&](SV x)->uint16_t{
  uint32_t n;if(campaign_number(x,n)&&n<campaign_fast.size())return campaign_fast[n];
  auto it=campaign_ids.find(x);return it==campaign_ids.end()?UINT16_MAX:it->second;
 };
 uint16_t organic=campaigns.size();campaigns.push_back("organic");channels.push_back("organic");
 auto link_paths=input_files(data+"/links");
 auto links_start=chrono::steady_clock::now();
 UnionFind dsu=build_links(maps,link_paths,ncpu);
 auto links_built=chrono::steady_clock::now();
 vector<int32_t> rootperson(dsu.nodes.size(),-1),min_node(dsu.nodes.size(),-1);
 vector<uint32_t> roots(dsu.nodes.size());
#ifdef _OPENMP
#pragma omp parallel for if(dsu.nodes.size()>20000)
#endif
 for(size_t i=0;i<dsu.nodes.size();i++){
  int r=concurrent_root(dsu.nodes,(int)i);roots[i]=(uint32_t)r;
  int32_t previous=__atomic_load_n(&min_node[r],__ATOMIC_ACQUIRE);
  while(previous<0||byte_less(dsu.nodes[i].value,dsu.nodes[previous].value)){
   if(__atomic_compare_exchange_n(&min_node[r],&previous,(int32_t)i,true,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
  }
 }
 vector<SV> people;people.reserve(dsu.nodes.size()/2);
 for(size_t i=0;i<min_node.size();i++)if(min_node[i]>=0){rootperson[i]=(int32_t)people.size();people.push_back(dsu.nodes[min_node[i]].value);}
 vector<uint32_t> link_person(dsu.nodes.size());
#ifdef _OPENMP
#pragma omp parallel for if(dsu.nodes.size()>20000)
#endif
 for(size_t i=0;i<dsu.nodes.size();i++)link_person[i]=(uint32_t)rootperson[roots[i]];
 FastPeople person_map(people,max<size_t>(2048,dsu.nodes.size()/32+128));
 PackedNumPeople numeric_people=std::move(dsu.numeric);
 numeric_people.remap(link_person);
 advise_huge_pages(numeric_people.slots.data(),numeric_people.slots.size()*sizeof(uint64_t),true);
 for(uint32_t i:dsu.special_nodes)person_map.add_link(dsu.nodes[i].value,link_person[i]);
 dsu.buckets.clear();dsu.buckets.shrink_to_fit();
 cerr<<"subphase link_build="<<chrono::duration<double>(links_built-links_start).count()<<"s canonicalize="<<chrono::duration<double>(chrono::steady_clock::now()-links_built).count()<<"s\n";
 cerr<<"links nodes="<<dsu.nodes.size()<<"\n";lap("links_dims");
 // Event dedup happens before *all* event validation.
 // Batch parse in parallel, then independently deduplicate by exact event ID.
 auto event_paths=input_files(data+"/events");
 uint64_t total_bytes=0, sample_bytes=0, sample_lines=0;
 for(const auto&p:event_paths)total_bytes+=fs::file_size(p);
 for(const auto&p:event_paths){
  if(sample_lines>=5000)break;
  ifstream f(p,ios::binary);string line;getline(f,line);
  while(sample_lines<5000 && getline(f,line)){sample_bytes+=line.size()+1;sample_lines++;}
 }
 size_t event_capacity=2000000;
 if(sample_lines&&sample_bytes)event_capacity=max<size_t>(1000,(long double)total_bytes*sample_lines/sample_bytes*1.05L);
 struct Work{const char* begin;const char* end;};
 vector<Work> chunks;
 for(auto&p:event_paths){
  maps.emplace_back(p);const auto&m=maps.back();
  if(!m.n)continue;
  const char* end=m.p+m.n;
  const char* header=(const char*)memchr(m.p,'\n',m.n);
  if(!header)continue;
  const char* first=header+1;
  if(first==end)continue;
  size_t span=end-first;
  size_t np=max<size_t>(1,min<size_t>(128,(span+262143)/262144));
  vector<const char*> bounds(np+1);
  bounds[0]=first;bounds[np]=end;
  for(size_t j=1;j<np;j++){
   const char* p=first+span*j/np;
   const char* q=(const char*)memchr(p,'\n',end-p);
   bounds[j]=q?q+1:end;
  }
  for(size_t j=0;j<np;j++)if(bounds[j]<bounds[j+1])chunks.push_back({bounds[j],bounds[j+1]});
 }
 constexpr size_t PARTS=256;
 const size_t workers=max(1,ncpu);
 vector<vector<Cand>> raw(workers*PARTS);
 for(auto& v:raw)v.reserve((size_t)(event_capacity*1.04/workers/PARTS)+32);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t ch=0;ch<chunks.size();ch++){
  size_t tid=0;
#ifdef _OPENMP
  tid=(size_t)omp_get_thread_num();
#endif
  const char* p=chunks[ch].begin;const char* end=chunks[ch].end;
  SV f[7];
  while(p<end){
   const char* first=p;
   if(exactcsv::events(p,end,7,f)&&!f[0].empty()){
     const char* last=p==end&&p[-1]!='\n'?end:p-1;
     SV line(first,last-first);
     uint64_t n=0,tag,uk=0,x0,x1;int32_t ingest_day;int64_t ingest=parse_ingest(f[3],ingest_day);
     bool event_canon=canonical_base36_id(f[0],'e',n);
     bool compact=event_canon && canonical_base36_id(f[1],'u',uk);
     uint8_t tp=f[4]=="purchase"?2:f[4]=="cart"?1:f[4]=="view"?0:3;
     int64_t ts;uint32_t toff=0x7fffffffU;
     if(parse_time(f[2],ts,f[3].data(),ingest_day)&&ts>=ws&&ts<we)toff=(uint32_t)(ts-ws);
     uint32_t price_code=0;if(tp==2){int32_t pv=parse_price(f[6]);if(pv>0)price_code=(uint32_t)pv;}
     bool prod_empty=f[5].empty();uint16_t cat=unknown;
     if(tp==2&&!prod_empty&&price_code){
      uint32_t pn;
      if(product_number(f[5],pn)&&pn<product_fast.size())cat=product_fast[pn]==UINT16_MAX?unknown:product_fast[pn];
      else {auto it=products.find(f[5]);if(it!=products.end())cat=it->second;}
     }
     int64_t ingest_sec=ingest/1000;
     int64_t ingest_code=ingest_sec-(ws-WEEK)/1000;
     compact=compact && cat<=255 && ingest_code>=0 && ingest_code<(1LL<<22);
     if(compact){
      tag=n;
      x0=(uk&MASK40)|((uint64_t)(toff&0xffffffU)<<40);
      x1=((uint64_t)(toff>>24)&0x7fULL)|((uint64_t)price_code<<7)|((uint64_t)cat<<31)|((uint64_t)tp<<39)|((uint64_t)prod_empty<<41)|((uint64_t)ingest_code<<42);
     }else{
      tag=event_canon?(n|NONCOMPACT_PAYLOAD):(NONCANON_ID|(uint64_t(Hash{}(f[0]))&0x3fffffffffffffffULL));
      if(line.size()>0xffffffULL)continue;
      x0=(uint64_t)(uintptr_t)line.data();
      uint64_t sec40=(uint64_t)ingest_sec&0xffffffffffULL;
      x1=(uint64_t)line.size()|(sec40<<24);
     }
     raw[tid*PARTS+((size_t)(tag&~NONCOMPACT_PAYLOAD)%PARTS)].push_back({x0,x1,tag});
   }
  }
 }
 lap("event_parse");
 vector<IdentityCache> event_people_cache(workers);
 vector<vector<Evt>> valid_by_part(PARTS);
 vector<vector<PendingEvt>> pending_by_part(PARTS);
 vector<vector<PendingNum>> pending_num_by_part(PARTS);
 auto validate_t0=chrono::steady_clock::now();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<PARTS;part++){
  size_t lookup_tid=0;
#ifdef _OPENMP
  lookup_tid=(size_t)omp_get_thread_num();
#endif
  auto& cache=event_people_cache[lookup_tid];
  size_t n=0;for(size_t t=0;t<workers;t++)n+=raw[t*PARTS+part].size();
  vector<Cand> cands;cands.reserve(n);
  FlatEventIndex idx(cands,n,countr_zero(PARTS));
  for(size_t t=0;t<workers;t++){
   auto& rows=raw[t*PARTS+part];
   for(const Cand& c:rows){
      uint32_t ix=idx.get_or_create(c);
      if(ix==UINT32_MAX)cands.push_back(c);
      else if(cand_ingest_ms(c,ws)<cand_ingest_ms(cands[ix],ws))cands[ix]=c;
   }
   vector<Cand>().swap(rows);
  }
  auto& rows=cands;
  auto& good=valid_by_part[part];
  auto& pending=pending_by_part[part];auto& pending_num=pending_num_by_part[part];
  good.reserve(rows.size());pending.reserve(32);pending_num.reserve(rows.size()/3+16);
  SV f[7];
  for(const Cand& c:rows){
   SV user,prod;uint8_t tp;uint32_t t,user_hash=0;uint64_t uk=UINT64_MAX;int32_t price=0;uint16_t cat=unknown;
   if((c.idtag&(NONCANON_ID|NONCOMPACT_PAYLOAD))==0){
    uk=cand_user_key(c);t=cand_time(c);tp=cand_type(c);price=cand_price(c);cat=cand_category(c);
    if(cand_product_empty(c)||tp==3||t==0x7fffffffU)continue;
    if(cand_ingest_ms(c,ws)-(ws+(int64_t)t)>DAY)continue;
    if(tp==2&&price<=0)continue;
   }else{
    SV fline=cand_line(c);if(!split_csv(fline,7,f))continue;
    user=f[1];prod=f[5];
    if(!canonical_base36_id(user,'u',uk))user_hash=(uint32_t)Hash{}(user);
    tp=f[4]=="purchase"?2:f[4]=="cart"?1:f[4]=="view"?0:3;
    if(user.empty()||prod.empty()||tp==3)continue;
    int64_t ts;if(!parse_time(f[2],ts)||ts<ws||ts>=we||cand_ingest_ms(c,ws)-ts>DAY)continue;
    t=(uint32_t)(ts-ws);
    if(tp==2){
     price=parse_price(f[6]);if(price<=0)continue;uint32_t pn;
     if(product_number(prod,pn)&&pn<product_fast.size())cat=product_fast[pn]==UINT16_MAX?unknown:product_fast[pn];
     else {auto it=products.find(prod);if(it!=products.end())cat=it->second;}
    }
   }
   uint32_t p=(uk!=UINT64_MAX)?cache.find(numeric_people,uk):person_map.find_hashed(user,user_hash);
   if(p!=UINT32_MAX)good.push_back({t,p,price,cat,(uint8_t)(tp==2),0});
   else if(uk!=UINT64_MAX)pending_num.push_back({uk,t,price,cat,(uint8_t)(tp==2),0});
   else pending.push_back({user,t,price,user_hash,cat,(uint8_t)(tp==2),0});
  }
  vector<Cand>().swap(rows);
 }
 vector<vector<Cand>>().swap(raw);
 lap("event_dedup_validate");
 auto validate_t1=chrono::steady_clock::now();
 size_t good_n=0,pending_n=0,pending_num_n=0;
 for(size_t part=0;part<PARTS;part++){good_n+=valid_by_part[part].size();pending_n+=pending_by_part[part].size();pending_num_n+=pending_num_by_part[part].size();}
 vector<Evt> events;events.reserve(good_n+pending_n+pending_num_n);
 for(auto& v:valid_by_part){events.insert(events.end(),v.begin(),v.end());vector<Evt>().swap(v);}
 vector<vector<Evt>>().swap(valid_by_part);
 // Partition unresolved numeric identities once, then intern each shard without
 // locks. Repeated events no longer revisit one large mutable identity table.
 constexpr size_t NUM_SHARDS=256;
 vector<size_t> numeric_counts(PARTS*NUM_SHARDS),numeric_offsets(PARTS*NUM_SHARDS);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
 for(size_t part=0;part<PARTS;part++){
  auto* counts=numeric_counts.data()+part*NUM_SHARDS;
  for(const auto& e:pending_num_by_part[part])counts[e.key&(NUM_SHARDS-1)]++;
 }
 array<size_t,NUM_SHARDS+1> numeric_bounds{};
 size_t numeric_at=0;
 for(size_t sh=0;sh<NUM_SHARDS;sh++){
  numeric_bounds[sh]=numeric_at;
  for(size_t part=0;part<PARTS;part++){
   numeric_offsets[part*NUM_SHARDS+sh]=numeric_at;
   numeric_at+=numeric_counts[part*NUM_SHARDS+sh];
  }
 }
 numeric_bounds[NUM_SHARDS]=numeric_at;
 vector<PendingNum> numeric_rows(pending_num_n);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
 for(size_t part=0;part<PARTS;part++){
  auto* positions=numeric_offsets.data()+part*NUM_SHARDS;
  for(const auto& e:pending_num_by_part[part])numeric_rows[positions[e.key&(NUM_SHARDS-1)]++]=e;
  vector<PendingNum>().swap(pending_num_by_part[part]);
 }
 vector<vector<PendingNum>>().swap(pending_num_by_part);
 array<deque<string>,NUM_SHARDS> owned_user_names;
 array<vector<pair<uint64_t,uint32_t>>,NUM_SHARDS> new_numeric;
 uint32_t numeric_base=(uint32_t)people.size();
 people.resize((size_t)numeric_base+pending_num_n);
 events.resize(good_n+pending_num_n);
 atomic<uint32_t> next_numeric(numeric_base);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t sh=0;sh<NUM_SHARDS;sh++){
  size_t first=numeric_bounds[sh],last=numeric_bounds[sh+1];
  if(first==last)continue;
  PackedNumPeople local(max<size_t>(16,(last-first)/24));
  auto& names=owned_user_names[sh];auto& identities=new_numeric[sh];
  for(size_t i=first;i<last;i++){
   const auto& e=numeric_rows[i];
   // All keys in a shard share eight low bits. Remove those bits in the
   // private table too, while retaining the complete ID in the global index.
   uint64_t local_key=e.key/NUM_SHARDS;
   uint32_t p=local.find(local_key);
   if(p==UINT32_MAX){
    p=next_numeric.fetch_add(1,memory_order_relaxed);
    names.push_back(canonical_user_name(e.key));people[p]=names.back();
    local.add(local_key,p);identities.push_back({e.key,p});
   }
   events[good_n+i]={e.t,p,e.price,e.category,e.is_purchase,0};
  }
 }
 people.resize(next_numeric.load(memory_order_relaxed));
 for(const auto& identities:new_numeric)for(const auto& [key,p]:identities)numeric_people.add(key,p);
 vector<PendingNum>().swap(numeric_rows);
 // Resolve standalone IDs in parallel through exact sharded maps. IDs are internal only,
 // so assignment order is irrelevant; every equal byte string still gets exactly one person.
 constexpr size_t ID_SHARDS=256;
 array<unordered_map<SV,uint32_t,Hash,Eq>,ID_SHARDS> standalone;
 array<mutex,ID_SHARDS> standalone_mu;
 size_t per_shard=max<size_t>(16,pending_n/24/ID_SHARDS+8);
 for(auto& m:standalone)m.reserve(per_shard);
 uint32_t people_base=(uint32_t)people.size();
 people.resize((size_t)people_base+pending_n);
 atomic<uint32_t> next_person(people_base);
 vector<vector<Evt>> resolved_pending(PARTS);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<PARTS;part++){
  auto& src=pending_by_part[part];
  auto& dst=resolved_pending[part];
  dst.reserve(src.size());
  for(const PendingEvt& e:src){
   size_t h=e.user_hash, sh=h&(ID_SHARDS-1);
   uint32_t p;
   {
    lock_guard<mutex> lock(standalone_mu[sh]);
    auto& m=standalone[sh];
    auto it=m.find(e.user);
    if(it==m.end()){
     p=next_person.fetch_add(1,memory_order_relaxed);
     m.emplace(e.user,p);
     people[p]=e.user;
    }else p=it->second;
   }
   dst.push_back({e.t,p,e.price,e.category,e.is_purchase,0});
  }
  vector<PendingEvt>().swap(src);
 }
 uint32_t final_people=next_person.load(memory_order_relaxed);
 people.resize(final_people);
 // Make the newly assigned IDs available to click processing. This is once per unique
 // ID rather than once per event.
 for(uint32_t p=people_base;p<final_people;p++)person_map.add_link(people[p],p);
 for(auto& v:resolved_pending){events.insert(events.end(),v.begin(),v.end());vector<Evt>().swap(v);}
 vector<vector<Evt>>().swap(resolved_pending);
 vector<vector<PendingEvt>>().swap(pending_by_part);
 auto validate_t2=chrono::steady_clock::now();
 cerr<<"subphase winner_validate="<<chrono::duration<double>(validate_t1-validate_t0).count()<<"s standalone_resolve="<<chrono::duration<double>(validate_t2-validate_t1).count()<<"s\n";
 cerr<<"valid events="<<events.size()<<" people="<<people.size()<<" pending_ids_rows="<<(pending_n+pending_num_n)<<"\n";lap("identity_resolve");
 // Clicks: parse + validate in parallel, then exact-dedup independent ID shards.
 auto click_paths=input_files(data+"/clicks");
 vector<Work> click_chunks;
 for(auto&p:click_paths){
  maps.emplace_back(p);const auto&m=maps.back();
  if(!m.n)continue;
  const char* end=m.p+m.n;
  const char* header=(const char*)memchr(m.p,'\n',m.n);if(!header)continue;
  const char* first=header+1;if(first==end)continue;
  size_t span=end-first;
  size_t np=max<size_t>(1,min<size_t>(64,(span+262143)/262144));
  vector<const char*> bounds(np+1);bounds[0]=first;bounds[np]=end;
  for(size_t j=1;j<np;j++){const char* p=first+span*j/np;const char* q=(const char*)memchr(p,'\n',end-p);bounds[j]=q?q+1:end;}
  for(size_t j=0;j<np;j++)if(bounds[j]<bounds[j+1])click_chunks.push_back({bounds[j],bounds[j+1]});
 }
 constexpr size_t CPARTS=16;
 size_t click_est=max<size_t>(1000,events.size()/4);
 vector<IdentityCache> click_people_cache(workers);
 vector<vector<ClickCand>> click_raw(workers*CPARTS);
 for(auto&v:click_raw)v.reserve(click_est/workers/CPARTS+16);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
 for(size_t ch=0;ch<click_chunks.size();ch++){
  size_t tid=0;
#ifdef _OPENMP
  tid=(size_t)omp_get_thread_num();
#endif
  const char* p=click_chunks[ch].begin;const char* end=click_chunks[ch].end;SV f[4];
  while(p<end){
   if(exactcsv::clicks(p,end,4,f)&&!f[0].empty()&&!f[1].empty()){
    uint16_t cid=campaign_for(f[2]);
    if(cid!=UINT16_MAX){
     int64_t ts;
     if(parse_time(f[3],ts)&&ts>=ws-WEEK&&ts<we){
      uint64_t n,tag;
      if(canonical_base36_id(f[0],'k',n))tag=n;
      else tag=(1ULL<<63)|(uint64_t(Hash{}(f[0]))&0x7fffffffffffffffULL);
      uint64_t uk;uint32_t person;
      if(canonical_base36_id(f[1],'u',uk))person=click_people_cache[tid].find(numeric_people,uk);
      else {size_t uh=Hash{}(f[1]);person=person_map.find_hashed(f[1],uh);}
      // A click for a person with no surviving event can affect neither table.
      // Dropping it also avoids interning identities that can never have a session.
      if(person==UINT32_MAX)continue;
      click_raw[tid*CPARTS+((size_t)tag%CPARTS)].push_back({f[0],tag,(uint32_t)(ts-(ws-WEEK)),person,cid,0});
     }
    }
   }
  }
 }
 vector<vector<Click>> click_unique(CPARTS);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<CPARTS;part++){
  size_t n=0;for(size_t t=0;t<workers;t++)n+=click_raw[t*CPARTS+part].size();
  vector<Click> rows;rows.reserve(n);vector<uint64_t> tags;tags.reserve(n);
  FlatClickCandIndex idx(rows,tags,n,countr_zero(CPARTS));
  for(size_t t=0;t<workers;t++){
   auto& src=click_raw[t*CPARTS+part];
   for(const ClickCand& c:src)if(idx.insert(c)){
    rows.push_back({c.clickid,c.t,c.person,c.campaign,0});tags.push_back(c.idtag);
   }
   vector<ClickCand>().swap(src);
  }
  click_unique[part]=std::move(rows);
 }
 vector<vector<ClickCand>>().swap(click_raw);
 array<size_t,CPARTS+1> click_bounds{};
 for(size_t part=0;part<CPARTS;part++)click_bounds[part+1]=click_bounds[part]+click_unique[part].size();
 vector<Click> clicks(click_bounds.back());
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
 for(size_t part=0;part<CPARTS;part++){
  auto& rows=click_unique[part];
  std::copy(rows.begin(),rows.end(),clicks.begin()+click_bounds[part]);
  vector<Click>().swap(rows);
 }
 vector<vector<Click>>().swap(click_unique);
 cerr<<"valid clicks="<<clicks.size()<<"\n";lap("click_parse_dedup");
 // Sessions from globally sorted valid events, across every linked ID.
 order_events(events);lap("event_sort");
 // Count output ranges first so independent groups of whole people can write
 // directly into final arrays. No locks or concatenation of session copies.
 size_t session_parts=min<size_t>(workers*4,max<size_t>(1,events.size()/65536));
 vector<size_t> event_bounds(session_parts+1);event_bounds.back()=events.size();
 for(size_t part=1;part<session_parts;part++){
  size_t at=events.size()*part/session_parts;
  while(at<events.size()&&events[at].person==events[at-1].person)at++;
  event_bounds[part]=at;
 }
 struct SessionCounts{size_t users=0,sessions=0;};
 vector<SessionCounts> counts(session_parts+1);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<session_parts;part++){
  SessionCounts count;
  uint32_t previous=UINT32_MAX,last_time=0;int32_t last_day=INT32_MIN;
  for(size_t i=event_bounds[part];i<event_bounds[part+1];i++){
   const Evt& e=events[i];
   if(e.person!=previous||uint64_t(e.t)-last_time>(uint64_t)GAP){
    int32_t day=epoch_day(ws+(int64_t)e.t);
    if(e.person!=previous||day!=last_day){count.users++;last_day=day;}
    count.sessions++;
   }
   previous=e.person;last_time=e.t;
  }
  counts[part+1]=count;
 }
 for(size_t part=1;part<counts.size();part++){
  counts[part].users+=counts[part-1].users;
  counts[part].sessions+=counts[part-1].sessions;
 }
 vector<UserDay> ud(counts.back().users);
 vector<Session> sessions(counts.back().sessions);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<session_parts;part++){
  size_t ui=counts[part].users,si=counts[part].sessions;
  uint32_t previous=UINT32_MAX,last_time=0;int32_t last_day=INT32_MIN;
  vector<int64_t> category_totals(categories.size(),0);
  auto finalize=[&](){
   if(ui==counts[part].users)return;
   uint16_t best=UINT16_MAX;int64_t amount=0;
   for(uint16_t cat=0;cat<category_totals.size();cat++){
    int64_t value=category_totals[cat];
    if(value>amount||(value==amount&&value>0&&(best==UINT16_MAX||byte_less(categories[cat],categories[best])))){amount=value;best=cat;}
   }
   ud[ui-1].top_category=best;
  };
  for(size_t i=event_bounds[part];i<event_bounds[part+1];i++){
   const Evt& e=events[i];
   if(e.person!=previous||uint64_t(e.t)-last_time>(uint64_t)GAP){
    int64_t start=ws+(int64_t)e.t;int32_t day=epoch_day(start);
    if(e.person!=previous||day!=last_day){
     finalize();fill(category_totals.begin(),category_totals.end(),0);
     ud[ui++]={e.person,day};last_day=day;
    }
    ud[ui-1].sessions++;
    sessions[si++]={start,0,e.person,(uint32_t)(ui-1),organic,0,0};
   }
   previous=e.person;last_time=e.t;
   auto& session=sessions[si-1];auto& user=ud[ui-1];user.events++;
   if(e.is_purchase){
    session.purchased=1;session.revenue+=e.price;user.revenue+=e.price;
    category_totals[e.category]+=e.price;
   }
  }
  finalize();
 }
 vector<Evt>().swap(events);
 cerr<<"sessions="<<sessions.size()<<" user_days="<<ud.size()<<"\n";lap("sessionize");
 lap("top_category");
 vector<CampDay> camp((size_t)campaigns.size()*nday);
 order_clicks(clicks,people.size());lap("click_sort");
 // Whole-person work units keep attribution cursors and assist intervals local
 // to one worker. Aggregate into private campaign arrays, then reduce once.
 const int64_t click_base=ws-WEEK;
 vector<pair<size_t,size_t>> spans(people.size(),{0,0}),click_spans(people.size(),{0,0});
 for(size_t i=0;i<sessions.size();){size_t j=i+1;while(j<sessions.size()&&sessions[j].person==sessions[i].person)j++;spans[sessions[i].person]={i,j};i=j;}
 for(size_t i=0;i<clicks.size();){size_t j=i+1;while(j<clicks.size()&&clicks[j].person==clicks[i].person)j++;click_spans[clicks[i].person]={i,j};i=j;}
 vector<vector<CampDay>> campaign_totals(workers,vector<CampDay>(camp.size()));
#ifdef _OPENMP
#pragma omp parallel
#endif
 {
  size_t tid=0;
#ifdef _OPENMP
  tid=(size_t)omp_get_thread_num();
#endif
  auto& totals=campaign_totals[tid];
  auto aggregate=[&](uint16_t campaign,int32_t day)->CampDay&{return totals[(size_t)campaign*nday+day-day_base];};
  auto count_assists=[&](uint32_t person,uint16_t campaign,int64_t a,int64_t b){
   auto span=spans[person];
   auto beg=sessions.begin()+span.first, end=sessions.begin()+span.second;
   auto lo=lower_bound(beg,end,a,[](const Session&s,int64_t t){return s.start<t;});
   auto hi=upper_bound(lo,end,b,[](int64_t t,const Session&s){return t<s.start;});
   while(lo<hi){int32_t d=epoch_day(lo->start);int64_t until=(int64_t(d)+1)*DAY;
    auto stop=lower_bound(lo,hi,until,[](const Session&s,int64_t t){return s.start<t;});
    aggregate(campaign,d).assists+=stop-lo;lo=stop;
   }
  };
  vector<vector<uint32_t>> campaign_times(campaigns.size()-1);
  vector<uint16_t> touched;touched.reserve(min<size_t>(campaigns.size(),256));
#ifdef _OPENMP
#pragma omp for schedule(dynamic,32)
#endif
  for(size_t person=0;person<people.size();person++){
   auto [sb,se]=spans[person];if(sb==se)continue;
   auto [i,j]=click_spans[person];size_t c=i;
   int64_t lastclicktime=INT64_MIN;uint16_t lastcampaign=organic;
   for(size_t si=sb;si<se;si++){
    auto& session=sessions[si];uint32_t session_ct=(uint32_t)(session.start-click_base);
    while(c<j&&clicks[c].t<=session_ct){
     uint32_t t=clicks[c].t;size_t stop=c+1,best=c;
     while(stop<j&&clicks[stop].t==t){
      if(byte_less(clicks[stop].clickid,clicks[best].clickid))best=stop;
      stop++;
     }
     lastclicktime=click_base+(int64_t)t;lastcampaign=clicks[best].campaign;c=stop;
    }
    uint16_t credited=(lastclicktime!=INT64_MIN&&session.start-lastclicktime<=WEEK)?lastcampaign:organic;
    auto& value=aggregate(credited,epoch_day(session.start));
    value.sessions++;value.purchasing+=session.purchased;value.revenue+=session.revenue;
   }
   touched.clear();
   for(size_t z=i;z<j;z++){
    uint16_t campaign=clicks[z].campaign;
    auto& times=campaign_times[campaign];
    if(times.empty())touched.push_back(campaign);
    times.push_back(clicks[z].t);
   }
   for(uint16_t campaign:touched){
    auto& times=campaign_times[campaign];
    int64_t from=click_base+(int64_t)times[0],to=from+WEEK;
    for(size_t z=1;z<times.size();z++){
     int64_t a=click_base+(int64_t)times[z],b=a+WEEK;
     if(a<=to)to=max(to,b);else{count_assists((uint32_t)person,campaign,from,to);from=a;to=b;}
    }
    count_assists((uint32_t)person,campaign,from,to);
    times.clear();
   }
  }
 }
 for(const auto& totals:campaign_totals)for(size_t i=0;i<camp.size();i++){
  camp[i].sessions+=totals[i].sessions;camp[i].purchasing+=totals[i].purchasing;
  camp[i].revenue+=totals[i].revenue;camp[i].assists+=totals[i].assists;
 }
 vector<Click>().swap(clicks);
 lap("attribution_assists");
 fs::create_directories(out);
 fs::create_directories(out+"/user_daily");
 fs::create_directories(out+"/campaign_daily");

 struct CampRow{uint16_t id;int32_t day;};
 vector<CampRow> camp_rows;camp_rows.reserve(campaigns.size()*min(nday,16));
 for(size_t id=0;id<campaigns.size();id++)for(int d=0;d<nday;d++){
  auto &v=camp[id*nday+d];if(v.sessions||v.assists)camp_rows.push_back({(uint16_t)id,d+day_base});
 }

 size_t user_parts=1;
 if(!ud.empty())user_parts=min<size_t>(max(1,ncpu),max<size_t>(1,(ud.size()+199999)/200000));
 size_t tasks=user_parts+1;
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t task=0;task<tasks;task++){
  if(task<user_parts){
   size_t first=ud.size()*task/user_parts,last=ud.size()*(task+1)/user_parts;
   string path=out+"/user_daily/part-"+to_string(task)+".parquet";
   nativepq::write_users(path,first,last,
    [&](size_t i)->SV{return people[ud[i].person];},
    [&](size_t i)->int32_t{return ud[i].day;},
    [&](size_t i)->int64_t{return ud[i].sessions;},
    [&](size_t i)->int64_t{return ud[i].events;},
    [&](size_t i)->int64_t{return ud[i].revenue;},
    [&](size_t i)->bool{return ud[i].top_category!=UINT16_MAX;},
    [&](size_t i)->SV{return categories[ud[i].top_category];});
  }else{
   nativepq::write_campaigns(out+"/campaign_daily/part-0.parquet",camp_rows.size(),
    [&](size_t i)->SV{return campaigns[camp_rows[i].id];},
    [&](size_t i)->SV{return channels[camp_rows[i].id];},
    [&](size_t i)->int32_t{return camp_rows[i].day;},
    [&](size_t i)->int64_t{auto&r=camp[(size_t)camp_rows[i].id*nday+camp_rows[i].day-day_base];return r.sessions;},
    [&](size_t i)->int64_t{auto&r=camp[(size_t)camp_rows[i].id*nday+camp_rows[i].day-day_base];return r.purchasing;},
    [&](size_t i)->int64_t{auto&r=camp[(size_t)camp_rows[i].id*nday+camp_rows[i].day-day_base];return r.revenue;},
    [&](size_t i)->int64_t{auto&r=camp[(size_t)camp_rows[i].id*nday+camp_rows[i].day-day_base];return r.assists;});
  }
 }
 lap("parquet_output");
 cerr<<"engine time="<<chrono::duration<double>(chrono::steady_clock::now()-t0).count()<<"s\n";
 return 0;
}
