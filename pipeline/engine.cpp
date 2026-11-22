#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
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
#include <unordered_set>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "native_parquet.hpp"
#ifdef _OPENMP
#include <parallel/algorithm>
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
 if(s.size()<2||s[0]!=prefix||(s.size()>2&&s[1]=='0'))return false;
 uint64_t v=0;
 for(size_t i=1;i<s.size();i++){
  unsigned char c=s[i];unsigned d;
  if(c>='0'&&c<='9')d=c-'0';
  else if(c>='a'&&c<='z')d=c-'a'+10;
  else return false;
  if(d>=36||v>((1ULL<<40)-1-d)/36)return false;
  v=v*36+d;
 }
 out=v;return true;
}
static int64_t dnum(const char* p,int n) {int64_t x=0; for(int i=0;i<n;i++){if(p[i]<'0'||p[i]>'9')return -1; x=x*10+p[i]-'0';} return x;}
static int64_t epoch_days(int y,int m,int d){ // Howard Hinnant: proleptic Gregorian
 y-=m<=2; int era=(y>=0 ? y:y-399)/400; unsigned yoe=(unsigned)(y-era*400); unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned yoe_days=yoe*365+yoe/4-yoe/100+doy; return int64_t(era)*146097+yoe_days-719468;
}
static bool parse_time(SV s,int64_t &val){
 // F1/F2 are fixed-width. Everything else can only be F3 and is validated in one pass.
 if(s.size()!=20 && s.size()!=25){
  if(s.empty())return false;
  uint64_t n=0;
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
 int y=dnum(p,4),m=dnum(p+5,2),d=dnum(p+8,2),h=dnum(p+11,2),mi=dnum(p+14,2),se=dnum(p+17,2);
 if(y<1||m<1||m>12||d<1||h>23||mi>59||se>59||h<0||mi<0||se<0)return false;
 int md[12]={31,28,31,30,31,30,31,31,30,31,30,31};
 if(m==2 && (y%4==0 && (y%100!=0||y%400==0)))md[1]=29;
 if(d>md[m-1])return false;
 int offset=0;
 if(s.size()==25){int oh=dnum(p+20,2),om=dnum(p+23,2);if(oh>23||om>59||oh<0||om<0)return false;offset=oh*60+om;if(p[19]=='-')offset=-offset;}
 val=(epoch_days(y,m,d)*86400LL + h*3600+mi*60+se-int64_t(offset)*60)*1000;
 return true;
}
static int32_t epoch_day(int64_t ts){int64_t d=ts/DAY; if(ts<0 && ts%DAY)d--;return (int32_t)d;}
static string date_string(int32_t z) {
 z+=719468; int era=(z>=0?z:z-146096)/146097; unsigned doe=(unsigned)(z-era*146097);
 unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365; int y=(int)yoe+era*400;
 unsigned doy=doe-(365*yoe+yoe/4-yoe/100); unsigned mp=(5*doy+2)/153;
 unsigned d=doy-(153*mp+2)/5+1;unsigned m=mp+(mp<10?3:-9);y+=(m<=2);
 char buf[20];snprintf(buf,sizeof(buf),"%04d-%02u-%02u",y,m,d);return string(buf);
}
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
 while(p<end){const char*q=(const char*)memchr(p,'\n',end-p);if(!q)q=end;
  if(split_csv(SV(p,q-p),n,fields))action(fields);
  p=(q==end)?end:q+1;
 }
}
struct Node{int parent,weight;SV value;};
struct UnionFind{
 vector<Node> nodes;vector<uint32_t> buckets;size_t used=0;
 UnionFind(){buckets.assign(1024,0);}
 void grow(){
  vector<uint32_t> nb(buckets.size()*2,0);size_t mask=nb.size()-1;
  for(uint32_t v:buckets)if(v){size_t p=Hash{}(nodes[v-1].value)&mask;while(nb[p])p=(p+1)&mask;nb[p]=v;}
  buckets.swap(nb);
 }
 int add(SV s){
  if(used*10>=buckets.size()*7)grow();size_t p=Hash{}(s)&(buckets.size()-1);
  while(buckets[p]){uint32_t v=buckets[p];if(nodes[v-1].value==s)return (int)v-1;p=(p+1)&(buckets.size()-1);}
  int id=(int)nodes.size();nodes.push_back({id,1,s});buckets[p]=(uint32_t)id+1;used++;return id;
 }
 int root(int i){int j=i;while(nodes[j].parent!=j)j=nodes[j].parent;while(nodes[i].parent!=i){int x=nodes[i].parent;nodes[i].parent=j;i=x;}return j;}
 void join(SV a,SV b){int x=root(add(a)),y=root(add(b));if(x==y)return;if(nodes[x].weight<nodes[y].weight)swap(x,y);nodes[y].parent=x;nodes[x].weight+=nodes[y].weight;}
};
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
struct ClickCand{SV clickid,user;uint64_t idtag;uint32_t t;uint32_t person;uint16_t campaign;uint16_t unused=0;};
struct Assist{uint32_t person;uint32_t t;uint16_t campaign;uint16_t unused=0;};
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
struct FlatNumPeople{
 vector<uint64_t> keys;vector<uint32_t> vals;size_t used=0;
 explicit FlatNumPeople(size_t expected=16){size_t cap=16;while(cap<expected*2)cap<<=1;keys.assign(cap,0);vals.resize(cap);}
 static size_t h(uint64_t x){return (size_t)(x*0x9E3779B97F4A7C15ULL);}
 void grow(){
  vector<uint64_t> nk(keys.size()*2,0);vector<uint32_t> nv(nk.size());size_t mask=nk.size()-1;
  for(size_t i=0;i<keys.size();i++)if(keys[i]){
   size_t p=h(keys[i])&mask;while(nk[p])p=(p+1)&mask;nk[p]=keys[i];nv[p]=vals[i];
  }
  keys.swap(nk);vals.swap(nv);
 }
 uint32_t find(uint64_t key)const{
  uint64_t stored=key+1;size_t p=h(stored)&(keys.size()-1);
  while(keys[p]){if(keys[p]==stored)return vals[p];p=(p+1)&(keys.size()-1);}return UINT32_MAX;
 }
 void add(uint64_t key,uint32_t person){
  if(used*10>=keys.size()*7)grow();uint64_t stored=key+1;size_t p=h(stored)&(keys.size()-1);
  while(keys[p]){if(keys[p]==stored){vals[p]=person;return;}p=(p+1)&(keys.size()-1);}
  keys[p]=stored;vals[p]=person;used++;
 }
};
struct FlatEventIndex {
 vector<uint32_t> buckets;
 size_t used = 0;
 vector<Cand>& rows;
 explicit FlatEventIndex(vector<Cand>& v,size_t expected):rows(v) {
   size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;
   buckets.assign(cap,0);
 }
 uint32_t get_or_create(const Cand& c) {
   if(used*10>=buckets.size()*7)grow();
   size_t mask=buckets.size()-1;
   uint64_t ckey=c.idtag&~NONCOMPACT_PAYLOAD;
   size_t p=(size_t)ckey&mask;
   for(;;) {
     uint32_t v=buckets[p];
     if(!v) {
       uint32_t id=(uint32_t)rows.size();
       buckets[p]=id+1;used++;
       return UINT32_MAX;
     }
     const Cand& old=rows[v-1];uint64_t okey=old.idtag&~NONCOMPACT_PAYLOAD;
     if(ckey==okey){
      if((ckey&NONCANON_ID)==0 || cand_eventid(c)==cand_eventid(old))return v-1;
     }
     p=(p+1)&mask;
   }
 }
 void grow() {
   vector<uint32_t> nb(buckets.size()*2,0);
   size_t mask=nb.size()-1;
   for(uint32_t v:buckets) if(v) {
     size_t p=(size_t)(rows[v-1].idtag&~NONCOMPACT_PAYLOAD)&mask;
     while(nb[p])p=(p+1)&mask;
     nb[p]=v;
   }
   buckets.swap(nb);
 }
};
struct FlatClickSet {
 vector<uint32_t> buckets;
 vector<Click> &rows;
 size_t used=0;
 explicit FlatClickSet(vector<Click>& r,size_t expected):rows(r){size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;buckets.resize(cap);}
 void grow(){vector<uint32_t> nb(buckets.size()*2);size_t mask=nb.size()-1;
  for(uint32_t v:buckets)if(v){size_t p=Hash{}(rows[v-1].clickid)&mask;while(nb[p])p=(p+1)&mask;nb[p]=v;}
  buckets.swap(nb);
 }
 bool insert(SV k){if(used*10>=buckets.size()*7)grow();size_t p=Hash{}(k)&(buckets.size()-1);
  while(buckets[p]){if(rows[buckets[p]-1].clickid==k)return false;p=(p+1)&(buckets.size()-1);}
  buckets[p]=(uint32_t)rows.size()+1;used++;return true;
 }
};
struct FlatClickCandIndex {
 vector<uint32_t> buckets;
 vector<ClickCand>& rows;
 explicit FlatClickCandIndex(vector<ClickCand>& r,size_t expected):rows(r){
  size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;buckets.resize(cap);
 }
 bool insert(const ClickCand& c){
  size_t p=(size_t)c.idtag&(buckets.size()-1);
  while(buckets[p]){
   const ClickCand& old=rows[buckets[p]-1];
   if(c.idtag==old.idtag){
    if((c.idtag>>63)==0 || c.clickid==old.clickid)return false;
   }
   p=(p+1)&(buckets.size()-1);
  }
  buckets[p]=(uint32_t)rows.size()+1;
  return true;
 }
};
static int64_t parse_ingest(SV s){
 // rules.md guarantees ingest_ts is valid F1; avoid the generic validator on every raw event.
 const char* p=s.data();
 int y=(p[0]-'0')*1000+(p[1]-'0')*100+(p[2]-'0')*10+(p[3]-'0');
 int mo=(p[5]-'0')*10+(p[6]-'0'), d=(p[8]-'0')*10+(p[9]-'0');
 int h=(p[11]-'0')*10+(p[12]-'0'), mi=(p[14]-'0')*10+(p[15]-'0'), se=(p[17]-'0')*10+(p[18]-'0');
 return (epoch_days(y,mo,d)*86400LL+h*3600+mi*60+se)*1000;
}
static int32_t parse_price(SV s) {
 if(!digits(s))return -1;
 int32_t v=0;
 for(char c:s){v=v*10+(c-'0');if(v>10000000)return -1;}
 return v>0?v:-1;
}
static void fail(const string&s){cerr<<s<<'\n';exit(1);}
struct FastOut{
 FILE* f=nullptr;vector<char> buf;size_t at=0;
 explicit FastOut(const string& path):buf(1<<20){f=fopen(path.c_str(),"wb");if(!f){perror("fopen");exit(2);}}
 ~FastOut(){flush();if(f)fclose(f);}
 void flush(){if(at){fwrite(buf.data(),1,at,f);at=0;}}
 void raw(const char* p,size_t n){if(n>buf.size()){flush();fwrite(p,1,n,f);return;}if(at+n>buf.size())flush();memcpy(buf.data()+at,p,n);at+=n;}
 void str(SV v){raw(v.data(),v.size());}
 void ch(char c){if(at==buf.size())flush();buf[at++]=c;}
 template<class T> void num(T v){char x[32];auto r=to_chars(x,x+sizeof(x),v);raw(x,(size_t)(r.ptr-x));}
 void field(SV v){if(v.find('\r')==SV::npos){str(v);return;}ch('"');str(v);ch('"');}
 void date(int32_t z){
  z+=719468;int era=(z>=0?z:z-146096)/146097;unsigned doe=(unsigned)(z-era*146097);
  unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;int y=(int)yoe+era*400;
  unsigned doy=doe-(365*yoe+yoe/4-yoe/100);unsigned mp=(5*doy+2)/153;
  unsigned d=doy-(153*mp+2)/5+1,m=mp+(mp<10?3:-9);y+=(m<=2);
  char x[10];x[0]='0'+(y/1000)%10;x[1]='0'+(y/100)%10;x[2]='0'+(y/10)%10;x[3]='0'+y%10;x[4]='-';
  x[5]='0'+m/10;x[6]='0'+m%10;x[7]='-';x[8]='0'+d/10;x[9]='0'+d%10;raw(x,10);
 }
};
template<class T,class Time>
static void bucket_person_time(vector<T>& a,size_t person_count,Time time_key){
 if(a.size()<2)return;
 vector<size_t> off(person_count+1,0);
 for(const T& x:a)off[(size_t)x.person+1]++;
 for(size_t i=1;i<off.size();i++)off[i]+=off[i-1];
 vector<size_t> pos=off;
 vector<T> tmp(a.size());
 for(const T& x:a)tmp[pos[x.person]++]=x;
 a.swap(tmp);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
 for(size_t p=0;p<person_count;p++){
  size_t lo=off[p],hi=off[p+1],n=hi-lo;
  if(n<2)continue;
  T* b=a.data()+lo;
  if(n<=16){
   for(size_t i=1;i<n;i++){
    T v=b[i];size_t j=i;
    while(j&&time_key(v)<time_key(b[j-1])){b[j]=b[j-1];--j;}
    b[j]=v;
   }
  }else{
   sort(b,b+n,[&](const T&x,const T&y){return time_key(x)<time_key(y);});
  }
 }
}
static void order_events(vector<Evt>& es,size_t people){
 bucket_person_time(es,people,[](const Evt&e){return e.t;});
}
static void order_clicks(vector<Click>& cs,size_t people){
 bucket_person_time(cs,people,[](const Click&c){return c.t;});
}
int main(){
 const string data=getenv("DATA_DIR")?getenv("DATA_DIR"):"/data";
 const string out=getenv("OUTPUT_DIR")?getenv("OUTPUT_DIR"):"/output";
 int ncpu=getenv("NCPU")?atoi(getenv("NCPU")):8;
#ifdef _OPENMP
 omp_set_num_threads(max(1,ncpu));
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
 UnionFind dsu;
 for(auto&p:input_files(data+"/links")){maps.emplace_back(p);foreach_csv(maps.back(),2,[&](SV*f){if(!f[0].empty()&&!f[1].empty())dsu.join(f[0],f[1]);});}
 vector<int32_t> rootperson(dsu.nodes.size(),-1);
 vector<SV> min_name(dsu.nodes.size());
 for(int i=0;i<(int)dsu.nodes.size();i++){int r=dsu.root(i);SV nm=dsu.nodes[i].value;if(min_name[r].empty()||byte_less(nm,min_name[r]))min_name[r]=nm;}
 vector<SV> people;
 vector<uint32_t> link_person(dsu.nodes.size());
 for(int i=0;i<(int)dsu.nodes.size();i++){int r=dsu.root(i);if(rootperson[r]<0){rootperson[r]=people.size();people.push_back(min_name[r]);}link_person[i]=rootperson[r];}
 FastPeople person_map(people,max<size_t>(2048,dsu.nodes.size()/32+128));
 FlatNumPeople numeric_people(dsu.nodes.size()+4096);
 for(size_t i=0;i<dsu.nodes.size();++i){
  uint64_t uk;
  if(canonical_base36_id(dsu.nodes[i].value,'u',uk))numeric_people.add(uk,link_person[i]);
  else person_map.add_link(dsu.nodes[i].value,link_person[i]);
 }
 dsu.buckets.clear();dsu.buckets.shrink_to_fit();
 auto person_find=[&](SV name)->uint32_t{
  uint64_t uk;if(canonical_base36_id(name,'u',uk))return numeric_people.find(uk);
  return person_map.find_only(name);
 };
 auto person_for=[&](SV name)->uint32_t{
  uint64_t uk;
  if(canonical_base36_id(name,'u',uk)){
   uint32_t p=numeric_people.find(uk);if(p!=UINT32_MAX)return p;
   p=(uint32_t)people.size();people.push_back(name);numeric_people.add(uk,p);return p;
  }
  return person_map.get(name);
 };
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
 constexpr size_t PARTS=16;
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
   const char*q=(const char*)memchr(p,'\n',end-p);if(!q)q=end;
   SV line(p,q-p);
   if(split_csv(line,7,f)&&!f[0].empty()){
     uint64_t n=0,tag,uk=0,x0,x1;int64_t ingest=parse_ingest(f[3]);
     bool event_canon=canonical_base36_id(f[0],'e',n);
     bool compact=event_canon && canonical_base36_id(f[1],'u',uk);
     uint8_t tp=f[4]=="purchase"?2:f[4]=="cart"?1:f[4]=="view"?0:3;
     int64_t ts;uint32_t toff=0x7fffffffU;
     if(parse_time(f[2],ts)&&ts>=ws&&ts<we)toff=(uint32_t)(ts-ws);
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
   p=(q==end)?end:q+1;
  }
 }
 lap("event_parse");
 vector<vector<Cand>> unique(PARTS);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<PARTS;part++){
  size_t n=0;for(size_t t=0;t<workers;t++)n+=raw[t*PARTS+part].size();
  vector<Cand> cands;cands.reserve(n);
  FlatEventIndex idx(cands,n);
  for(size_t t=0;t<workers;t++){
   auto& rows=raw[t*PARTS+part];
   for(const Cand& c:rows){
      uint32_t ix=idx.get_or_create(c);
      if(ix==UINT32_MAX)cands.push_back(c);
      else if(cand_ingest_ms(c,ws)<cand_ingest_ms(cands[ix],ws))cands[ix]=c;
   }
   vector<Cand>().swap(rows);
  }
  unique[part]=move(cands);
 }
 vector<vector<Cand>>().swap(raw);lap("event_dedup");
 // Validate winners in parallel. The linked-ID table is immutable here, so its lookups
 // are thread-safe. Only IDs absent from links are deferred for sequential insertion.
 vector<vector<Evt>> valid_by_part(PARTS);
 vector<vector<PendingEvt>> pending_by_part(PARTS);
 vector<vector<PendingNum>> pending_num_by_part(PARTS);
 auto validate_t0=chrono::steady_clock::now();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<PARTS;part++){
  auto& rows=unique[part];
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
   uint32_t p=(uk!=UINT64_MAX)?numeric_people.find(uk):person_map.find_hashed(user,user_hash);
   if(p!=UINT32_MAX)good.push_back({t,p,price,cat,(uint8_t)(tp==2),0});
   else if(uk!=UINT64_MAX)pending_num.push_back({uk,t,price,cat,(uint8_t)(tp==2),0});
   else pending.push_back({user,t,price,user_hash,cat,(uint8_t)(tp==2),0});
  }
  vector<Cand>().swap(rows);
 }
 vector<vector<Cand>>().swap(unique);
 auto validate_t1=chrono::steady_clock::now();
 size_t good_n=0,pending_n=0,pending_num_n=0;
 for(size_t part=0;part<PARTS;part++){good_n+=valid_by_part[part].size();pending_n+=pending_by_part[part].size();pending_num_n+=pending_num_by_part[part].size();}
 vector<Evt> events;events.reserve(good_n+pending_n+pending_num_n);
 for(auto& v:valid_by_part){events.insert(events.end(),v.begin(),v.end());vector<Evt>().swap(v);}
 vector<vector<Evt>>().swap(valid_by_part);
 deque<string> owned_user_names;
 for(auto&src:pending_num_by_part){
  for(const PendingNum&e:src){
   uint32_t p=numeric_people.find(e.key);
   if(p==UINT32_MAX){
    p=(uint32_t)people.size();owned_user_names.push_back(canonical_user_name(e.key));people.push_back(owned_user_names.back());numeric_people.add(e.key,p);
   }
   events.push_back({e.t,p,e.price,e.category,e.is_purchase,0});
  }
  vector<PendingNum>().swap(src);
 }
 vector<vector<PendingNum>>().swap(pending_num_by_part);
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
 cerr<<"valid events="<<events.size()<<" people="<<people.size()<<" pending_ids_rows="<<(pending_n+pending_num_n)<<"\n";lap("event_validate");
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
   const char*q=(const char*)memchr(p,'\n',end-p);if(!q)q=end;
   if(split_csv(SV(p,q-p),4,f)&&!f[0].empty()&&!f[1].empty()){
    uint16_t cid=campaign_for(f[2]);
    if(cid!=UINT16_MAX){
     int64_t ts;
     if(parse_time(f[3],ts)&&ts>=ws-WEEK&&ts<we){
      uint64_t n,tag;
      if(canonical_base36_id(f[0],'k',n))tag=n;
      else tag=(1ULL<<63)|(uint64_t(Hash{}(f[0]))&0x7fffffffffffffffULL);
      uint64_t uk;uint32_t person;
      if(canonical_base36_id(f[1],'u',uk))person=numeric_people.find(uk);
      else {size_t uh=Hash{}(f[1]);person=person_map.find_hashed(f[1],uh);}
      click_raw[tid*CPARTS+((size_t)tag%CPARTS)].push_back({f[0],f[1],tag,(uint32_t)(ts-(ws-WEEK)),person,cid,0});
     }
    }
   }
   p=(q==end)?end:q+1;
  }
 }
 vector<vector<ClickCand>> click_unique(CPARTS);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<CPARTS;part++){
  size_t n=0;for(size_t t=0;t<workers;t++)n+=click_raw[t*CPARTS+part].size();
  vector<ClickCand> rows;rows.reserve(n);FlatClickCandIndex idx(rows,n);
  for(size_t t=0;t<workers;t++){
   auto& src=click_raw[t*CPARTS+part];
   for(const ClickCand& c:src)if(idx.insert(c))rows.push_back(c);
   vector<ClickCand>().swap(src);
  }
  click_unique[part]=move(rows);
 }
 vector<vector<ClickCand>>().swap(click_raw);
 size_t click_n=0;for(auto&v:click_unique)click_n+=v.size();
 vector<Click> clicks;clicks.reserve(click_n);
 for(auto&v:click_unique){
  for(const ClickCand& c:v){
   uint32_t p=c.person==UINT32_MAX?person_for(c.user):c.person;
   clicks.push_back({c.clickid,c.t,p,c.campaign,0});
  }
  vector<ClickCand>().swap(v);
 }
 vector<vector<ClickCand>>().swap(click_unique);
 cerr<<"valid clicks="<<clicks.size()<<"\n";lap("click_parse_dedup");
 // Sessions from globally sorted valid events, across every linked ID.
 order_events(events,people.size());lap("event_bucket_sort");
 vector<UserDay> ud;ud.reserve(people.size()*2);
 vector<Session> sessions;sessions.reserve(events.size()/4+1);
 vector<Buy> buys;buys.reserve(events.size()/32+1);
 uint32_t lastperson=UINT32_MAX,lastt=0;
 for(const Evt&e:events){
  if(e.person!=lastperson||uint64_t(e.t)-lastt>(uint64_t)GAP){
   int64_t start=ws+(int64_t)e.t;
   int32_t d=epoch_day(start);
   if(ud.empty()||ud.back().person!=e.person||ud.back().day!=d)ud.push_back({e.person,d});
   ud.back().sessions++;
   sessions.push_back({start,0,e.person,(uint32_t)(ud.size()-1),organic,0,0});
  }
  lastperson=e.person;lastt=e.t;
  Session& s=sessions.back();UserDay&u=ud[s.userday];u.events++;
  if(e.is_purchase){s.purchased=1;s.revenue+=e.price;u.revenue+=e.price;buys.push_back({s.userday,e.category,e.price});}
 }
 vector<Evt>().swap(events);
 cerr<<"sessions="<<sessions.size()<<" user_days="<<ud.size()<<"\n";lap("sessionize");
 // Purchase category totals are per person-day, NOT per session, with byte-order ties.
 sort(buys.begin(),buys.end(),[](const Buy&a,const Buy&b){return a.userday<b.userday||(a.userday==b.userday&&a.category<b.category);});
 uint32_t curr=UINT32_MAX;uint16_t bestcat=UINT16_MAX;int64_t bestamt=-1;
 for(size_t i=0;i<buys.size();){size_t j=i+1;int64_t amt=buys[i].price;while(j<buys.size()&&buys[j].userday==buys[i].userday&&buys[j].category==buys[i].category)amt+=buys[j++].price;
  if(buys[i].userday!=curr){curr=buys[i].userday;bestamt=-1;bestcat=UINT16_MAX;}
  uint16_t cat=buys[i].category;
  if(amt>bestamt||(amt==bestamt && (bestcat==UINT16_MAX||byte_less(categories[cat],categories[bestcat])))){bestamt=amt;bestcat=cat;ud[curr].top_category=cat;}
  i=j;
 }
 vector<Buy>().swap(buys);lap("top_category");
 vector<CampDay> camp((size_t)campaigns.size()*nday);
 auto campref=[&](uint16_t id,int32_t day)->CampDay&{return camp[(size_t)id*nday+day-day_base];};
 order_clicks(clicks,people.size());lap("click_bucket_sort");
 // A single latest click cursor gives correct inclusive, 7-day, byte-wise tie attribution.
 size_t c=0;uint32_t prev_person=UINT32_MAX;
 int64_t lastclicktime=INT64_MIN;uint16_t lastcampaign=organic;
 const int64_t click_base=ws-WEEK;
 for(Session& s:sessions){
  if(s.person!=prev_person){prev_person=s.person;lastclicktime=INT64_MIN;lastcampaign=organic;
   while(c<clicks.size()&&clicks[c].person<s.person)c++;
  }
  uint32_t session_ct=(uint32_t)(s.start-click_base);
  while(c<clicks.size()&&clicks[c].person==s.person&&clicks[c].t<=session_ct){
   uint32_t t=clicks[c].t;size_t j=c+1;size_t best=c;
   while(j<clicks.size()&&clicks[j].person==s.person&&clicks[j].t==t){
    if(byte_less(clicks[j].clickid,clicks[best].clickid))best=j;
    j++;
   }
   lastclicktime=click_base+(int64_t)t;lastcampaign=clicks[best].campaign;c=j;
  }
  uint16_t credited=(lastclicktime!=INT64_MIN && s.start-lastclicktime<=WEEK)?lastcampaign:organic;
  s.credited=credited;
  auto &a=campref(credited,epoch_day(s.start));a.sessions++;a.revenue+=s.revenue;a.purchasing+=s.purchased;
 }
 lap("attribution");
 // Assist counting reuses clicks already sorted by (person,time). For each person,
 // campaign time lists inherit time order, so no second global assist sort is needed.
 vector<pair<size_t,size_t>> spans(people.size(),{0,0});
 for(size_t i=0;i<sessions.size();){size_t j=i+1;while(j<sessions.size()&&sessions[j].person==sessions[i].person)j++;spans[sessions[i].person]={i,j};i=j;}
 auto count_assists=[&](uint32_t person,uint16_t campaign,int64_t a,int64_t b){
  auto span=spans[person];if(span.first==span.second)return;
  auto beg=sessions.begin()+span.first, end=sessions.begin()+span.second;
  auto lo=lower_bound(beg,end,a,[](const Session&s,int64_t t){return s.start<t;});
  auto hi=upper_bound(lo,end,b,[](int64_t t,const Session&s){return t<s.start;});
  while(lo<hi){int32_t d=epoch_day(lo->start);int64_t until=(int64_t(d)+1)*DAY;
   auto stop=lower_bound(lo,hi,until,[](const Session&s,int64_t t){return s.start<t;});
   campref(campaign,d).assists+=stop-lo;lo=stop;
  }
 };
 vector<vector<uint32_t>> campaign_times(campaigns.size()-1);
 vector<uint16_t> touched;touched.reserve(min<size_t>(campaigns.size(),256));
 for(size_t i=0;i<clicks.size();){
  uint32_t p=clicks[i].person;size_t j=i+1;
  while(j<clicks.size()&&clicks[j].person==p)j++;
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
    if(a<=to)to=max(to,b);else{count_assists(p,campaign,from,to);from=a;to=b;}
   }
   count_assists(p,campaign,from,to);
   times.clear();
  }
  i=j;
 }
 vector<Click>().swap(clicks);
 lap("assist_count");
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