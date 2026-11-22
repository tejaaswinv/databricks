#define main pipeline_main
#include "../pipeline/engine.cpp"
#undef main
#include <random>
#include <stdexcept>

static void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
static void csv_tests(){
 std::vector<exactcsv::Reader> readers={exactcsv::next_scalar,exactcsv::next};
#if defined(__x86_64__)
 readers.push_back(exactcsv::next_sse2<>);
 if(__builtin_cpu_supports("avx2")&&__builtin_cpu_supports("bmi")&&__builtin_cpu_supports("popcnt"))readers.push_back(exactcsv::next_avx2<>);
 if(__builtin_cpu_supports("avx512bw")&&__builtin_cpu_supports("avx512f")&&__builtin_cpu_supports("bmi")&&__builtin_cpu_supports("popcnt"))readers.push_back(exactcsv::next_avx512<>);
#endif
 std::mt19937_64 rng(113);
 const std::string alphabet="abc01,\"\r \xc3\xa9";
 for(size_t trial=0;trial<100000;trial++){
  int n=trial%3==0?2:trial%3==1?4:7;
  std::string line;
  if(trial%2){
   for(int col=0;col<n;col++){
    if(col)line+=',';
    bool quote=rng()%2;if(quote)line+='"';
    size_t len=rng()%30;for(size_t j=0;j<len;j++)line+=alphabet[rng()%alphabet.size()];
    if(quote)line+='"';
   }
  }else{
   size_t len=rng()%260;for(size_t j=0;j<len;j++)line+=alphabet[rng()%alphabet.size()];
  }
  SV expected[7];bool valid=split_csv(line,n,expected);
  // Exercise both a bounded last row and vector loads that cross into later
  // rows. Reads must remain within the mapping, including exact 64/128 edges.
  for(bool newline:{false,true}){
   std::string bytes=line;
   if(newline){bytes+='\n';bytes.append(256,'x');}
   auto selected=readers;
   if(n==7)selected.push_back(exactcsv::events);
   if(n==4)selected.push_back(exactcsv::clicks);
   for(auto reader:selected){
    const char* p=bytes.data();const char* end=p+bytes.size();SV actual[7];
    bool got=reader(p,end,n,actual);
    check(got==valid,"CSV validity differs");
    check(p==bytes.data()+line.size()+(newline?1:0),"CSV row boundary differs");
    if(valid)for(int col=0;col<n;col++)check(actual[col]==expected[col],"CSV value differs");
   }
  }
 }
}
static void id_tests(){
 std::mt19937_64 rng(117);
 for(size_t i=0;i<100000;i++){
  uint64_t value=rng()&MASK40,parsed=UINT64_MAX;
  auto name=canonical_user_name(value);
  check(canonical_base36_id(name,'u',parsed)&&parsed==value,"base36 round trip");
 }
 for(auto s:{"u","U1","u00","u01","u+1","u 1","u1 ","u_1","ue13wu1og","uzzzzzzzz","u100000000"}){
  uint64_t value=0;check(!canonical_base36_id(s,'u',value),"noncanonical base36 accepted");
 }
 PackedNumPeople map;
 map.add(0,0);map.add(MASK40,0xfffffeU);map.add(37,0xffffffU);
 check(map.find(0)==0&&map.find(MASK40)==0xfffffeU&&map.find(37)==0xffffffU,"wide identity promotion");
 for(uint64_t i=1;i<5000;i++)map.add(i,uint32_t(i));
 check(map.find(0)==0&&map.find(MASK40)==0xfffffeU&&map.find(4999)==4999,"wide identity rehash");
 IdentityCache cache;
 for(size_t i=0;i<100000;i++){
  uint64_t key=rng()%8192;
  check(cache.find(map,key)==map.find(key),"cached identity differs");
 }
}
static void time_tests(){
 int64_t ts=0;
 check(parse_time("1970-01-01T00:00:00Z",ts)&&ts==0,"epoch timestamp");
 check(parse_time("2028-02-29T00:00:00Z",ts),"valid leap day");
 int64_t expected=ts;
 check(parse_time("2028-02-28T23:00:00-01:00",ts)&&ts==expected,"offset crossing day");
 for(size_t width:{size_t(20),size_t(25),size_t(80)}){
  std::string digits=std::to_string(expected);digits.insert(0,width-digits.size(),'0');
  check(parse_time(digits,ts)&&ts==expected,"leading-zero epoch timestamp");
 }
 for(auto s:{"2027-02-29T00:00:00Z","2028-02-30T00:00:00Z","2028-02-29T24:00:00Z","2028-02-29T00:00:60Z","2028-02-29T00:00:00Z\r","2028-02-29T00:00:00+24:00","9223372036854775808","-1"})
  check(!parse_time(s,ts),"invalid timestamp accepted");
}
static void radix_tests(){
#ifdef _OPENMP
 omp_set_dynamic(0);omp_set_num_threads(8);
#endif
 std::mt19937_64 rng(19);
 std::vector<Evt> events(100000);
 for(auto& event:events){event.person=uint32_t(rng()%10000);event.t=uint32_t(rng());}
 auto expected=events;
 auto key=[](const Evt& event){return (uint64_t(event.person)<<32)|event.t;};
 std::sort(expected.begin(),expected.end(),[&](const Evt& a,const Evt& b){return key(a)<key(b);});
 order_events(events,10000);
 for(size_t i=0;i<events.size();i++)check(key(events[i])==key(expected[i]),"parallel radix order differs");
}
static void linked_identity_tests(){
 char pattern[]="/tmp/shopping-tests-XXXXXX";
 char* directory=mkdtemp(pattern);check(directory!=nullptr,"create test directory");
 struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{directory};
 vector<fs::path> paths;
 array<ofstream,8> files;
 for(size_t part=0;part<files.size();part++){
  paths.push_back(fs::path(directory)/("links-"+to_string(part)+".csv"));
  files[part].open(paths.back());files[part]<<"a,b\n";
 }
 for(uint64_t i=0;i<30000;i++){
  string a=canonical_user_name((i*0x9e3779b97f4bULL)&MASK40);
  uint64_t next=i%300==299?i:i+1;
  string b=canonical_user_name((next*0x9e3779b97f4bULL)&MASK40);
  auto& file=files[i%files.size()];
  if(i%2)file<<'"'<<a<<"\",\""<<b<<"\"\n";else file<<b<<','<<a<<'\n';
  if(i%101==0)file<<a<<",SharedDevice\n";
  if(i%997==0)file<<a<<",\" spaced ID \"\n";
  if(i%29==0)file<<"\"malformed,"<<a<<"\n,"<<b<<"\n";
 }
 for(auto& file:files)file.close();
 auto names=[&](UnionFind& dsu){
  vector<SV> smallest(dsu.nodes.size());
  for(size_t i=0;i<dsu.nodes.size();i++){
   int r=dsu.root((int)i);SV name=dsu.nodes[i].value;
   if(smallest[r].empty()||byte_less(name,smallest[r]))smallest[r]=name;
  }
  unordered_map<string,string> result;
  for(size_t i=0;i<dsu.nodes.size();i++)result.emplace(dsu.nodes[i].value,smallest[dsu.root((int)i)]);
  return result;
 };
 vector<Mapping> serial_maps,parallel_maps;
 auto serial=build_links(serial_maps,paths,1);auto expected=names(serial);
 auto parallel=build_links(parallel_maps,paths,8);
 check(names(parallel)==expected,"parallel linked components differ");
 check(parallel.numeric.used+parallel.special_nodes.size()==parallel.nodes.size(),"linked index occupancy differs");
}
static void event_index_tests(){
 std::mt19937_64 rng(64);
 vector<Cand> input;
 for(size_t i=0;i<20000;i++){
  uint64_t key=(rng()%5000)*256+17;
  input.push_back({0,0,key|(i%2?NONCOMPACT_PAYLOAD:0)});
 }
 deque<string> lines;
 for(auto id:{"Event-A","Event-B","Event-A"}){
  lines.push_back(string(id)+",u1,0,1970-01-01T00:00:00Z,view,p1,");
  // Deliberately force distinct string IDs to have the same complete hash,
  // and the same short key as one of the canonical numeric IDs.
  input.push_back({uint64_t(uintptr_t(lines.back().data())),lines.back().size(),NONCANON_ID|17});
 }
 std::rotate(input.begin(),input.end()-3,input.end());
 for(bool force_wide:{false,true}){
  vector<Cand> rows;FlatEventIndex index(rows,1,8,force_wide);
  unordered_map<string,uint32_t> expected;
  for(size_t i=0;i<input.size();i++){
   const auto& c=input[i];
   string key=c.idtag&NONCANON_ID?"string:"+string(cand_eventid(c)):"number:"+to_string(c.idtag&~NONCOMPACT_PAYLOAD);
   auto [it,added]=expected.emplace(key,(uint32_t)rows.size());
   uint32_t found=index.get_or_create(c);
   check(added?found==UINT32_MAX:found==it->second,"event index collision handling differs");
   if(added)rows.push_back(c);
   if(!force_wide&&i==1000)index.widen();
  }
 }
}
int main(){csv_tests();id_tests();time_tests();radix_tests();linked_identity_tests();event_index_tests();std::cout<<"parser, identity, timestamp and person-bucket checks passed\n";}
