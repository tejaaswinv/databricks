#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
static int64_t dnum(const char* p,int n) {int64_t x=0; for(int i=0;i<n;i++){if(p[i]<'0'||p[i]>'9')return -1; x=x*10+p[i]-'0';} return x;}
static int64_t epoch_days(int y,int m,int d){ // Howard Hinnant: proleptic Gregorian
 y-=m<=2; int era=(y>=0 ? y:y-399)/400; unsigned yoe=(unsigned)(y-era*400); unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned yoe_days=yoe*365+yoe/4-yoe/100+doy; return int64_t(era)*146097+yoe_days-719468;
}
static bool parse_time(SV s,int64_t &val){
 if(digits(s)) { unsigned long long n=0; for(unsigned char ch:s){unsigned d=ch-'0'; if(n>(uint64_t(INT64_MAX)-d)/10)return false;n=n*10+d;}val=(int64_t)n;return true; }
 if(s.size()!=20 && s.size()!=25)return false;
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
struct Node{int parent,weight;string value;};
struct UnionFind{
 vector<Node> nodes;unordered_map<string,int,Hash,Eq> index;
 int add(SV s){auto it=index.find(s);if(it!=index.end())return it->second;int id=nodes.size();nodes.push_back({id,1,string(s)});index.emplace(nodes.back().value,id);return id;}
 int root(int i){int j=i;while(nodes[j].parent!=j)j=nodes[j].parent;while(nodes[i].parent!=i){int x=nodes[i].parent;nodes[i].parent=j;i=x;}return j;}
 void join(SV a,SV b){int x=root(add(a)),y=root(add(b));if(x==y)return;if(nodes[x].weight<nodes[y].weight)swap(x,y);nodes[y].parent=x;nodes[x].weight+=nodes[y].weight;}
};
struct Cand{SV eventid, user, ts, prod; int64_t ingest; int32_t price; uint8_t type;};
struct Evt{uint32_t t;uint32_t person;int32_t price;uint16_t category;uint8_t is_purchase;uint8_t unused;};
struct Click{SV clickid;uint32_t t;uint32_t person;uint16_t campaign;uint16_t unused=0;};
struct Assist{uint32_t person;uint32_t t;uint16_t campaign;uint16_t unused=0;};
struct UserDay{uint32_t person;int32_t day;uint32_t sessions=0, events=0;int64_t revenue=0;uint16_t top_category=UINT16_MAX;};
struct Session{int64_t start;int64_t revenue;uint32_t person;uint32_t userday;uint16_t credited=0;uint8_t purchased=0;uint8_t unused=0;};
struct Buy{uint32_t userday;uint16_t category;int32_t price;};
struct PendingEvt{SV user;uint32_t t;int32_t price;uint16_t category;uint8_t is_purchase;uint8_t unused=0;};
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
    size_t i=hash<SV>{}(e.key)&mask;
    while(!bigger[i].key.empty())i=(i+1)&mask;
    bigger[i]=e;
  }
  buckets.swap(bigger);
 }
 void add_link(SV key,uint32_t person) {
  if(used*10>=buckets.size()*7)grow();
  size_t p=hash<SV>{}(key)&(buckets.size()-1);
  while(!buckets[p].key.empty())p=(p+1)&(buckets.size()-1);
  buckets[p]={key,person};used++;
 }
 uint32_t find_only(SV key) const {
  size_t p=hash<SV>{}(key)&(buckets.size()-1);
  while(!buckets[p].key.empty()) {
    if(buckets[p].key==key)return buckets[p].person;
    p=(p+1)&(buckets.size()-1);
  }
  return UINT32_MAX;
 }
 uint32_t get(SV key) {
  if(used*10>=buckets.size()*7)grow();
  size_t p=hash<SV>{}(key)&(buckets.size()-1);
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
 vector<uint32_t> buckets;
 size_t used = 0;
 vector<Cand>& rows;
 explicit FlatEventIndex(vector<Cand>& v,size_t expected):rows(v) {
   size_t cap=16;while(cap<(size_t)(expected/0.70)+16)cap*=2;
   buckets.assign(cap,0);
 }
 uint32_t get_or_create(SV key) {
   if(used*10>=buckets.size()*7)grow();
   size_t mask=buckets.size()-1;
   size_t p=hash<SV>{}(key)&mask;
   for(;;) {
     uint32_t v=buckets[p];
     if(!v) {
       uint32_t id=(uint32_t)rows.size();
       buckets[p]=id+1;used++;
       return UINT32_MAX;
     }
     if(rows[v-1].eventid==key)return v-1;
     p=(p+1)&mask;
   }
 }
 void grow() {
   vector<uint32_t> nb(buckets.size()*2,0);
   size_t mask=nb.size()-1;
   for(uint32_t v:buckets) if(v) {
     size_t p=hash<SV>{}(rows[v-1].eventid)&mask;
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
  for(uint32_t v:buckets)if(v){size_t p=hash<SV>{}(rows[v-1].clickid)&mask;while(nb[p])p=(p+1)&mask;nb[p]=v;}
  buckets.swap(nb);
 }
 bool insert(SV k){if(used*10>=buckets.size()*7)grow();size_t p=hash<SV>{}(k)&(buckets.size()-1);
  while(buckets[p]){if(rows[buckets[p]-1].clickid==k)return false;p=(p+1)&(buckets.size()-1);}
  buckets[p]=(uint32_t)rows.size()+1;used++;return true;
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
static void write_field(ostream&os,SV s){
 // Values may contain a CR or quotes by spec? CSV field value cannot contain quotes or commas, CR is allowed.
 // Quote CR so a downstream CSV reader doesn't treat it as part of the terminator.
 if(s.find('\r')==SV::npos){os.write(s.data(),s.size());return;}
 os.put('"');os.write(s.data(),s.size());os.put('"');
}
template<class T,class Key>
static void radix64(vector<T>& a, Key key, int passes=4){
 if(a.size()<2)return;
 constexpr size_t R=1u<<16, MASK=R-1;
 int nt=1;
#ifdef _OPENMP
 nt=max(1,omp_get_max_threads());
#endif
 vector<T> tmp(a.size());
 vector<size_t> hist((size_t)nt*R), pos((size_t)nt*R);
 for(int pass=0;pass<passes;pass++){
  fill(hist.begin(),hist.end(),0);
  unsigned shift=pass*16;
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
   int tid=0;
#ifdef _OPENMP
   tid=omp_get_thread_num();
#endif
   size_t lo=a.size()*(size_t)tid/nt, hi=a.size()*(size_t)(tid+1)/nt;
   size_t* h=hist.data()+(size_t)tid*R;
   for(size_t i=lo;i<hi;i++)h[(key(a[i])>>shift)&MASK]++;
  }
  size_t base=0;
  for(size_t b=0;b<R;b++){
   size_t total=0;
   for(int t=0;t<nt;t++)total+=hist[(size_t)t*R+b];
   size_t p=base;
   for(int t=0;t<nt;t++){pos[(size_t)t*R+b]=p;p+=hist[(size_t)t*R+b];}
   base+=total;
  }
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
   int tid=0;
#ifdef _OPENMP
   tid=omp_get_thread_num();
#endif
   size_t lo=a.size()*(size_t)tid/nt, hi=a.size()*(size_t)(tid+1)/nt;
   size_t* p=pos.data()+(size_t)tid*R;
   for(size_t i=lo;i<hi;i++){size_t b=(key(a[i])>>shift)&MASK;tmp[p[b]++]=a[i];}
  }
  a.swap(tmp);
 }
}
static void order_events(vector<Evt>& es){
 radix64(es,[](const Evt&e){return (uint64_t(e.person)<<32)|e.t;},4);
}
static void order_clicks(vector<Click>& cs){
 radix64(cs,[](const Click&c){return (uint64_t(c.person)<<32)|c.t;},4);
}
static void order_assists(vector<Assist>& as){
 // Stable LSD: first (campaign,time), then person.
 radix64(as,[](const Assist&a){return (uint64_t(a.campaign)<<32)|a.t;},3);
 radix64(as,[](const Assist&a){return uint64_t(a.person);},2);
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
 unordered_map<string,uint16_t,Hash,Eq> products;products.reserve(75000);
 {
  Mapping m(data+"/products.csv");foreach_csv(m,2,[&](SV*f){products.emplace(string(f[0]),cat_id(f[1]));});
 }
 uint16_t unknown=cat_id("unknown");
 vector<string> campaigns,channels;
 unordered_map<string,uint16_t,Hash,Eq> campaign_ids;
 {
  Mapping m(data+"/campaigns.csv");foreach_csv(m,2,[&](SV*f){uint16_t id=campaigns.size();campaigns.push_back(string(f[0]));channels.push_back(string(f[1]));campaign_ids.emplace(campaigns.back(),id);});
 }
 uint16_t organic=campaigns.size();campaigns.push_back("organic");channels.push_back("organic");
 UnionFind dsu;
 for(auto&p:input_files(data+"/links")){Mapping m(p);foreach_csv(m,2,[&](SV*f){if(!f[0].empty()&&!f[1].empty())dsu.join(f[0],f[1]);});}
 vector<int32_t> rootperson(dsu.nodes.size(),-1);
 vector<SV> min_name(dsu.nodes.size());
 for(int i=0;i<(int)dsu.nodes.size();i++){int r=dsu.root(i);SV nm=dsu.nodes[i].value;if(min_name[r].empty()||byte_less(nm,min_name[r]))min_name[r]=nm;}
 vector<SV> people;
 vector<uint32_t> link_person(dsu.nodes.size());
 for(int i=0;i<(int)dsu.nodes.size();i++){int r=dsu.root(i);if(rootperson[r]<0){rootperson[r]=people.size();people.push_back(min_name[r]);}link_person[i]=rootperson[r];}
 FastPeople person_map(people,dsu.nodes.size()+2000);
 for(size_t i=0;i<dsu.nodes.size();++i)person_map.add_link(dsu.nodes[i].value,link_person[i]);
 dsu.index.clear();dsu.index.rehash(0);
 auto person_for=[&](SV name)->uint32_t{return person_map.get(name);};
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
   if(split_csv(SV(p,q-p),7,f)&&!f[0].empty()){
     size_t h=hash<SV>{}(f[0]);
     uint8_t tp=f[4]=="purchase"?2:f[4]=="cart"?1:f[4]=="view"?0:3;
     raw[tid*PARTS+(h%PARTS)].push_back({f[0],f[1],f[2],f[5],parse_ingest(f[3]),(tp==2?parse_price(f[6]):0),tp});
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
      uint32_t ix=idx.get_or_create(c.eventid);
      if(ix==UINT32_MAX)cands.push_back(c);
      else if(c.ingest<cands[ix].ingest)cands[ix]=c;
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
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic,1)
#endif
 for(size_t part=0;part<PARTS;part++){
  auto& rows=unique[part];
  auto& good=valid_by_part[part];
  auto& pending=pending_by_part[part];
  good.reserve(rows.size());
  pending.reserve(rows.size()/3+16);
  for(const Cand& c:rows){
   if(c.user.empty()||c.prod.empty()||c.type==3)continue;
   int64_t ts;if(!parse_time(c.ts,ts)||ts<ws||ts>=we||c.ingest-ts>DAY)continue;
   int32_t price=0;uint16_t cat=unknown;
   if(c.type==2){
    if(c.price<=0)continue;
    price=c.price;auto it=products.find(c.prod);if(it!=products.end())cat=it->second;
   }
   uint32_t p=person_map.find_only(c.user);
   uint32_t t=(uint32_t)(ts-ws);
   if(p!=UINT32_MAX)good.push_back({t,p,price,cat,(uint8_t)(c.type==2),0});
   else pending.push_back({c.user,t,price,cat,(uint8_t)(c.type==2),0});
  }
  vector<Cand>().swap(rows);
 }
 vector<vector<Cand>>().swap(unique);
 size_t good_n=0,pending_n=0;
 for(size_t part=0;part<PARTS;part++){good_n+=valid_by_part[part].size();pending_n+=pending_by_part[part].size();}
 vector<Evt> events;events.reserve(good_n+pending_n);
 for(auto& v:valid_by_part){events.insert(events.end(),v.begin(),v.end());vector<Evt>().swap(v);}
 vector<vector<Evt>>().swap(valid_by_part);
 for(auto& v:pending_by_part){
  for(const PendingEvt& e:v)events.push_back({e.t,person_for(e.user),e.price,e.category,e.is_purchase,0});
  vector<PendingEvt>().swap(v);
 }
 vector<vector<PendingEvt>>().swap(pending_by_part);
 cerr<<"valid events="<<events.size()<<" people="<<people.size()<<" pending_ids_rows="<<pending_n<<"\n";lap("event_validate");
 // Clicks: identical duplicates can safely be deduped after validation.
 vector<Click> clicks;clicks.reserve(max<size_t>(1000,events.size()/4));
 FlatClickSet click_seen(clicks,max<size_t>(1000,events.size()/4));
 for(auto&p:input_files(data+"/clicks")){
  maps.emplace_back(p);
  foreach_csv(maps.back(),4,[&](SV*f){
   if(f[0].empty()||f[1].empty())return;
   auto it=campaign_ids.find(f[2]);if(it==campaign_ids.end())return;
   int64_t ts;if(!parse_time(f[3],ts)||ts<ws-WEEK||ts>=we)return;
   if(!click_seen.insert(f[0]))return;
   clicks.push_back({f[0],(uint32_t)(ts-(ws-WEEK)),person_for(f[1]),it->second,0});
  });
 }
 click_seen.buckets.clear();click_seen.buckets.shrink_to_fit();
 cerr<<"valid clicks="<<clicks.size()<<"\n";lap("click_parse_dedup");
 // Sessions from globally sorted valid events, across every linked ID.
 order_events(events);lap("event_sort");
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
 order_clicks(clicks);lap("click_sort");
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
 // Assist counting uses merged intervals for each (person,campaign); no giant range join.
 // This handles all clicks on a shared device and arbitrarily long ID chains.
 vector<Assist> aids;aids.reserve(clicks.size());for(const Click&x:clicks)aids.push_back({x.person,x.t,x.campaign,0});
 vector<Click>().swap(clicks);
 order_assists(aids);lap("assist_sort");
 // Build offset index: person -> [first,last) in the session array.
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
 for(size_t i=0;i<aids.size();){uint32_t p=aids[i].person;uint16_t campaign=aids[i].campaign;
  int64_t from=click_base+(int64_t)aids[i].t,to=from+WEEK;size_t j=i+1;
  while(j<aids.size()&&aids[j].person==p&&aids[j].campaign==campaign){
    int64_t a=click_base+(int64_t)aids[j].t,b=a+WEEK;
    if(a<=to){to=max(to,b);}else{count_assists(p,campaign,from,to);from=a;to=b;}
    j++;
  }
  count_assists(p,campaign,from,to);i=j;
 }
 lap("assist_count");
 fs::create_directories(out);
 // Write unquoted TSV-style output with no field containing tabs/newlines per the input dialect.
 // Input values cannot contain commas, so a strict CSV output is sufficient.
 {
  ofstream f(out+"/user_daily.csv",ios::binary);f<<"user_id,day,sessions,events,revenue_cents,top_category\n";
  for(const UserDay&u:ud){write_field(f,people[u.person]);f<<','<<date_string(u.day)<<','<<u.sessions<<','<<u.events<<','<<u.revenue<<',';
   if(u.top_category!=UINT16_MAX)write_field(f,categories[u.top_category]);f<<'\n';}
 }
 {
  ofstream f(out+"/campaign_daily.csv",ios::binary);f<<"campaign_id,channel,day,sessions,purchasing_sessions,revenue_cents,assisted_sessions\n";
  for(size_t id=0;id<campaigns.size();id++)for(int d=0;d<nday;d++){
   auto &v=camp[id*nday+d];if(v.sessions==0&&v.assists==0)continue;
   write_field(f,campaigns[id]);f<<',';write_field(f,channels[id]);f<<','<<date_string(d+day_base)<<','<<v.sessions<<','<<v.purchasing<<','<<v.revenue<<','<<v.assists<<'\n';
  }
 }
 lap("csv_output");
 cerr<<"engine time="<<chrono::duration<double>(chrono::steady_clock::now()-t0).count()<<"s\n";
 return 0;
}