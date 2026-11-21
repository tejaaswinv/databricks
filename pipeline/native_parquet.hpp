#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nativepq {
using SV = std::string_view;
namespace fs = std::filesystem;

enum class Kind : int32_t { I32=1, I64=2, BYTE_ARRAY=6 };
static constexpr int32_t REP_REQUIRED=0, REP_OPTIONAL=1;
static constexpr int32_t CONV_UTF8=0, CONV_DATE=6;
static constexpr int32_t ENC_PLAIN=0, ENC_RLE=3;
static constexpr int32_t CODEC_UNCOMPRESSED=0, PAGE_DATA=0;

enum CType : uint8_t {
  C_STOP=0, C_TRUE=1, C_FALSE=2, C_BYTE=3, C_I16=4, C_I32=5,
  C_I64=6, C_DOUBLE=7, C_BINARY=8, C_LIST=9, C_STRUCT=12
};

struct Compact {
  std::vector<uint8_t> b;
  int last=0;
  std::vector<int> stack;
  void varu(uint64_t v){while(v>=128){b.push_back(uint8_t(v)|128);v>>=7;}b.push_back(uint8_t(v));}
  void z32(int32_t v){uint32_t u=(uint32_t(v)<<1)^uint32_t(v>>31);varu(u);}
  void z64(int64_t v){uint64_t u=(uint64_t(v)<<1)^uint64_t(v>>63);varu(u);}
  void field(int id,uint8_t t){
    int d=id-last;
    if(d>0&&d<=15)b.push_back(uint8_t((d<<4)|t));
    else{b.push_back(t);z32(id);}
    last=id;
  }
  void i32(int id,int32_t v){field(id,C_I32);z32(v);}
  void i64(int id,int64_t v){field(id,C_I64);z64(v);}
  void boolean(int id,bool v){field(id,v?C_TRUE:C_FALSE);}
  void binary(int id,SV s){field(id,C_BINARY);varu(s.size());b.insert(b.end(),s.begin(),s.end());}
  void begin_struct(int id){field(id,C_STRUCT);stack.push_back(last);last=0;}
  void begin_anon(){stack.push_back(last);last=0;}
  void end_struct(){b.push_back(C_STOP);last=stack.back();stack.pop_back();}
  void list_field(int id,size_t n,uint8_t elem){
    field(id,C_LIST);
    if(n<15)b.push_back(uint8_t((n<<4)|elem));
    else{b.push_back(uint8_t(0xf0|elem));varu(n);}
  }
  void stop(){b.push_back(C_STOP);}
};

inline void le32(std::vector<uint8_t>& b,uint32_t v){
  b.push_back(uint8_t(v));b.push_back(uint8_t(v>>8));b.push_back(uint8_t(v>>16));b.push_back(uint8_t(v>>24));
}
inline void le64(std::vector<uint8_t>& b,uint64_t v){
  for(int i=0;i<8;i++)b.push_back(uint8_t(v>>(8*i)));
}
inline void raw(std::ofstream& f,const std::vector<uint8_t>& b){
  if(!b.empty())f.write(reinterpret_cast<const char*>(b.data()),std::streamsize(b.size()));
}

struct Field {
  SV name;
  Kind kind;
  int32_t converted=-1;
  bool optional=false;
};
struct Chunk {
  int64_t offset=0;
  int64_t total=0;
  int64_t values=0;
};
struct RowGroup {
  std::vector<Chunk> cols;
  int64_t rows=0;
  int64_t bytes=0;
};

class FileWriter {
  std::ofstream f_;
  std::vector<Field> schema_;
  std::vector<RowGroup> groups_;
  int64_t rows_=0;
  static constexpr size_t PAGE_ROWS=131072;

  template<class Present>
  std::vector<uint8_t> def_levels(size_t first,size_t count,Present present){
    const size_t groups=(count+7)/8;
    Compact h;h.varu((uint64_t(groups)<<1)|1);
    std::vector<uint8_t> out;
    out.reserve(4+h.b.size()+groups);
    std::vector<uint8_t> body=h.b;
    body.resize(h.b.size()+groups,0);
    for(size_t i=0;i<count;i++)if(present(first+i))body[h.b.size()+i/8]|=uint8_t(1u<<(i&7));
    le32(out,uint32_t(body.size()));
    out.insert(out.end(),body.begin(),body.end());
    return out;
  }

  void page_header(int32_t slots,size_t def_bytes,size_t value_bytes,bool optional,std::vector<uint8_t>& out){
    const size_t data_bytes=def_bytes+value_bytes;
    if(data_bytes>INT32_MAX)throw std::runtime_error("parquet page too large");
    Compact h;
    h.i32(1,PAGE_DATA);
    h.i32(2,int32_t(data_bytes));
    h.i32(3,int32_t(data_bytes));
    h.begin_struct(5);
    h.i32(1,slots);
    h.i32(2,ENC_PLAIN);
    h.i32(3,ENC_RLE);
    h.i32(4,ENC_RLE);
    h.end_struct();
    h.stop();
    out.swap(h.b);
  }

public:
  FileWriter(const fs::path& path,std::vector<Field> schema):schema_(std::move(schema)){
    f_.exceptions(std::ios::badbit|std::ios::failbit);
    f_.open(path,std::ios::binary|std::ios::trunc);
    f_.write("PAR1",4);
  }

  template<class Present,class Getter>
  Chunk write_string_col(size_t first,size_t last,bool optional,Present present,Getter get){
    Chunk ch;ch.offset=int64_t(f_.tellp());ch.values=int64_t(last-first);
    std::vector<uint8_t> vals,defs,hdr;
    for(size_t row=first;row<last;){
      size_t n=std::min(PAGE_ROWS,last-row);
      vals.clear();vals.reserve(n*12);
      for(size_t i=0;i<n;i++){
        size_t at=row+i;
        if(optional&&!present(at))continue;
        SV s=get(at);
        if(s.size()>UINT32_MAX)throw std::runtime_error("string too large");
        le32(vals,uint32_t(s.size()));
        vals.insert(vals.end(),s.begin(),s.end());
      }
      defs.clear();
      if(optional)defs=def_levels(row,n,present);
      page_header(int32_t(n),defs.size(),vals.size(),optional,hdr);
      raw(f_,hdr);raw(f_,defs);raw(f_,vals);
      ch.total+=int64_t(hdr.size()+defs.size()+vals.size());
      row+=n;
    }
    return ch;
  }

  template<class Getter>
  Chunk write_i64_col(size_t first,size_t last,Getter get){
    Chunk ch;ch.offset=int64_t(f_.tellp());ch.values=int64_t(last-first);
    std::vector<uint8_t> vals,hdr;
    for(size_t row=first;row<last;){
      size_t n=std::min(PAGE_ROWS,last-row);
      vals.clear();vals.reserve(n*8);
      for(size_t i=0;i<n;i++)le64(vals,uint64_t(int64_t(get(row+i))));
      page_header(int32_t(n),0,vals.size(),false,hdr);
      raw(f_,hdr);raw(f_,vals);ch.total+=int64_t(hdr.size()+vals.size());row+=n;
    }
    return ch;
  }

  template<class Getter>
  Chunk write_i32_col(size_t first,size_t last,Getter get){
    Chunk ch;ch.offset=int64_t(f_.tellp());ch.values=int64_t(last-first);
    std::vector<uint8_t> vals,hdr;
    for(size_t row=first;row<last;){
      size_t n=std::min(PAGE_ROWS,last-row);
      vals.clear();vals.reserve(n*4);
      for(size_t i=0;i<n;i++)le32(vals,uint32_t(int32_t(get(row+i))));
      page_header(int32_t(n),0,vals.size(),false,hdr);
      raw(f_,hdr);raw(f_,vals);ch.total+=int64_t(hdr.size()+vals.size());row+=n;
    }
    return ch;
  }

  void add_group(std::vector<Chunk> cols,size_t rows){
    RowGroup g;g.cols=std::move(cols);g.rows=int64_t(rows);
    for(auto&c:g.cols)g.bytes+=c.total;
    rows_+=g.rows;groups_.push_back(std::move(g));
  }

  void finish(){
    Compact m;
    m.i32(1,1);
    m.list_field(2,schema_.size()+1,C_STRUCT);
    m.begin_anon();m.binary(4,"schema");m.i32(5,int32_t(schema_.size()));m.end_struct();
    for(const auto& fld:schema_){
      m.begin_anon();
      m.i32(1,int32_t(fld.kind));
      m.i32(3,fld.optional?REP_OPTIONAL:REP_REQUIRED);
      m.binary(4,fld.name);
      if(fld.converted>=0)m.i32(6,fld.converted);
      m.end_struct();
    }
    m.i64(3,rows_);
    m.list_field(4,groups_.size(),C_STRUCT);
    for(const auto& g:groups_){
      m.begin_anon();
      m.list_field(1,g.cols.size(),C_STRUCT);
      for(size_t i=0;i<g.cols.size();i++){
        const auto& c=g.cols[i];const auto& fld=schema_[i];
        m.begin_anon();
        m.i64(2,c.offset);
        m.begin_struct(3);
        m.i32(1,int32_t(fld.kind));
        m.list_field(2,fld.optional?2:1,C_I32);
        m.z32(ENC_PLAIN);if(fld.optional)m.z32(ENC_RLE);
        m.list_field(3,1,C_BINARY);m.varu(fld.name.size());m.b.insert(m.b.end(),fld.name.begin(),fld.name.end());
        m.i32(4,CODEC_UNCOMPRESSED);
        m.i64(5,c.values);
        m.i64(6,c.total);
        m.i64(7,c.total);
        m.i64(9,c.offset);
        m.end_struct();
        m.end_struct();
      }
      m.i64(2,g.bytes);m.i64(3,g.rows);m.end_struct();
    }
    m.binary(6,"native-shopping-sessions");
    m.stop();
    raw(f_,m.b);
    uint32_t n=uint32_t(m.b.size());
    char tail[8]={char(n),char(n>>8),char(n>>16),char(n>>24),'P','A','R','1'};
    f_.write(tail,8);
    f_.close();
  }
};

template<class UGet,class DGet,class SGet,class EGet,class RGet,class HasTop,class TGet>
inline void write_users(const fs::path& path,size_t first,size_t last,UGet uid,DGet day,SGet sessions,EGet events,RGet revenue,HasTop has_top,TGet top){
  std::vector<Field> schema={{"user_id",Kind::BYTE_ARRAY,CONV_UTF8,false},{"day",Kind::I32,CONV_DATE,false},
    {"sessions",Kind::I64,-1,false},{"events",Kind::I64,-1,false},{"revenue_cents",Kind::I64,-1,false},
    {"top_category",Kind::BYTE_ARRAY,CONV_UTF8,true}};
  FileWriter w(path,schema);std::vector<Chunk> c;c.reserve(6);
  c.push_back(w.write_string_col(first,last,false,[](size_t){return true;},uid));
  c.push_back(w.write_i32_col(first,last,day));
  c.push_back(w.write_i64_col(first,last,sessions));
  c.push_back(w.write_i64_col(first,last,events));
  c.push_back(w.write_i64_col(first,last,revenue));
  c.push_back(w.write_string_col(first,last,true,has_top,top));
  if(last>first)w.add_group(std::move(c),last-first);w.finish();
}

template<class IGet,class CGet,class DGet,class SGet,class PGet,class RGet,class AGet>
inline void write_campaigns(const fs::path& path,size_t rows,IGet id,CGet channel,DGet day,SGet sessions,PGet purchasing,RGet revenue,AGet assists){
  std::vector<Field> schema={{"campaign_id",Kind::BYTE_ARRAY,CONV_UTF8,false},{"channel",Kind::BYTE_ARRAY,CONV_UTF8,false},
    {"day",Kind::I32,CONV_DATE,false},{"sessions",Kind::I64,-1,false},{"purchasing_sessions",Kind::I64,-1,false},
    {"revenue_cents",Kind::I64,-1,false},{"assisted_sessions",Kind::I64,-1,false}};
  FileWriter w(path,schema);std::vector<Chunk> c;c.reserve(7);
  c.push_back(w.write_string_col(0,rows,false,[](size_t){return true;},id));
  c.push_back(w.write_string_col(0,rows,false,[](size_t){return true;},channel));
  c.push_back(w.write_i32_col(0,rows,day));
  c.push_back(w.write_i64_col(0,rows,sessions));
  c.push_back(w.write_i64_col(0,rows,purchasing));
  c.push_back(w.write_i64_col(0,rows,revenue));
  c.push_back(w.write_i64_col(0,rows,assists));
  if(rows)w.add_group(std::move(c),rows);w.finish();
}
} // namespace nativepq
