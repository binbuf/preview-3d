#include "ThreeMfOpcPreflight.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>
#include <zlib.h>

namespace import_worker { namespace {
constexpr uint32_t Local = 0x04034b50, Central = 0x02014b50, Eocd = 0x06054b50, Zip64Locator = 0x07064b50, Zip64Eocd = 0x06064b50;
bool add(uint64_t a, uint64_t b, uint64_t& r) { if (b > (std::numeric_limits<uint64_t>::max)() - a) return false; r=a+b; return true; }
bool r16(std::span<const std::byte>b,uint64_t o,uint16_t&v){if(o>b.size()||b.size()-o<2)return false;v=uint16_t(std::to_integer<uint8_t>(b[size_t(o)]))|uint16_t(uint16_t(std::to_integer<uint8_t>(b[size_t(o+1)]))<<8);return true;}
bool r32(std::span<const std::byte>b,uint64_t o,uint32_t&v){if(o>b.size()||b.size()-o<4)return false;v=uint32_t(std::to_integer<uint8_t>(b[size_t(o)]))|(uint32_t(std::to_integer<uint8_t>(b[size_t(o+1)]))<<8)|(uint32_t(std::to_integer<uint8_t>(b[size_t(o+2)]))<<16)|(uint32_t(std::to_integer<uint8_t>(b[size_t(o+3)]))<<24);return true;}
bool r64(std::span<const std::byte>b,uint64_t o,uint64_t&v){uint32_t l,h;return r32(b,o,l)&&r32(b,o+4,h)&&(v=uint64_t(l)|(uint64_t(h)<<32),true);}
bool nameok(std::span<const std::byte> n,uint32_t depth,std::string& out){if(n.empty()||n.size()>1024)return false;out.clear();uint32_t d=1;size_t start=0;for(size_t i=0;i<n.size();++i){unsigned char c=std::to_integer<unsigned char>(n[i]);if(c<0x20||c==0x7f||c=='\\'||c==':'||(i==0&&c=='/'))return false;if(c=='/'){if(i==start||(i-start==1&&std::to_integer<unsigned char>(n[start])=='.')||(i-start==2&&std::to_integer<unsigned char>(n[start])=='.'&&std::to_integer<unsigned char>(n[start+1])=='.'))return false;start=i+1;if(++d>depth)return false;}if(c>=0x80)return false;out.push_back(char(std::tolower(c)));}if(start==n.size())return n.size()>1&&std::to_integer<unsigned char>(n.back())=='/';const auto final=std::to_integer<unsigned char>(n[start]);return !(n.size()-start==1&&final=='.') && !(n.size()-start==2&&final=='.'&&std::to_integer<unsigned char>(n[start+1])=='.');}
}
ThreeMfOpcError InspectThreeMfOpc(std::span<const std::byte> b, ThreeMfOpcPackage* out,const ThreeMfOpcLimits& lim,const std::function<bool()>& cancelled) {
 if(out)*out={}; if(cancelled&&cancelled())return ThreeMfOpcError::Cancelled; if(b.size()<22)return ThreeMfOpcError::NotZip;
 uint64_t e=b.size()-22,lo=b.size()>65557?b.size()-65557:0;uint32_t sig=0;while(!r32(b,e,sig)||sig!=Eocd){if(e==lo)return ThreeMfOpcError::NotZip;--e;}
 uint16_t disk,cdisk,onDisk,count,comment;uint32_t csize32,coff32;if(!r16(b,e+4,disk)||!r16(b,e+6,cdisk)||!r16(b,e+8,onDisk)||!r16(b,e+10,count)||!r32(b,e+12,csize32)||!r32(b,e+16,coff32)||!r16(b,e+20,comment))return ThreeMfOpcError::Truncated;uint64_t end;if(!add(e,22+uint64_t(comment),end)||end!=b.size())return ThreeMfOpcError::InvalidDirectory;if(disk||cdisk||onDisk!=count)return ThreeMfOpcError::MultiDisk;
 uint64_t count64=count,csize=csize32,coff=coff32; if(count==0xffff||csize32==0xffffffff||coff32==0xffffffff){if(e<20)return ThreeMfOpcError::Truncated;uint32_t ls;uint64_t zoff;if(!r32(b,e-20,ls)||ls!=Zip64Locator||!r64(b,e-12,zoff)||!r32(b,e-4,sig)||sig)return ThreeMfOpcError::InvalidDirectory;uint64_t zsize;if(!r32(b,zoff,sig)||sig!=Zip64Eocd||!r64(b,zoff+4,zsize)||zsize<44||!r64(b,zoff+24,count64)||!r64(b,zoff+32,count64)||!r64(b,zoff+40,csize)||!r64(b,zoff+48,coff))return ThreeMfOpcError::Truncated;}
 if(!count64||count64>lim.maxEntries)return ThreeMfOpcError::EntryLimit;uint64_t cend;if(!add(coff,csize,cend)||cend>e)return ThreeMfOpcError::InvalidDirectory;std::unordered_set<std::string> names;std::vector<std::pair<uint64_t,uint64_t>> ranges;uint64_t expanded=0,cursor=coff;bool content=false,roots=false;std::string model;
 for(uint64_t i=0;i<count64;++i){if(cancelled&&cancelled())return ThreeMfOpcError::Cancelled;uint16_t flags,method,nl,xl,cl,startDisk;uint32_t crc,comp32,uncomp32,off32;if(!r32(b,cursor,sig)||sig!=Central||!r16(b,cursor+8,flags)||!r16(b,cursor+10,method)||!r32(b,cursor+16,crc)||!r32(b,cursor+20,comp32)||!r32(b,cursor+24,uncomp32)||!r16(b,cursor+28,nl)||!r16(b,cursor+30,xl)||!r16(b,cursor+32,cl)||!r16(b,cursor+34,startDisk)||!r32(b,cursor+42,off32))return ThreeMfOpcError::Truncated;if(startDisk)return ThreeMfOpcError::MultiDisk;if(flags&1)return ThreeMfOpcError::Encrypted;if(method!=0&&method!=8)return ThreeMfOpcError::UnsupportedCompression;uint64_t next;if(!add(cursor,46+uint64_t(nl)+xl+cl,next)||next>cend)return ThreeMfOpcError::Truncated;std::string folded;if(!nameok(b.subspan(size_t(cursor+46),nl),lim.maxPathDepth,folded))return ThreeMfOpcError::UnsafePath;if(!names.insert(folded).second)return ThreeMfOpcError::DuplicatePart;
 uint64_t comp=comp32,uncomp=uncomp32,off=off32; if(comp32==0xffffffff||uncomp32==0xffffffff||off32==0xffffffff){uint64_t p=cursor+46+nl,xe=p+xl;while(p+4<=xe){uint16_t id,sz;if(!r16(b,p,id)||!r16(b,p+2,sz)||p+4+sz>xe)return ThreeMfOpcError::Truncated;p+=4;if(id==1){if(uncomp32==0xffffffff&&!r64(b,p,uncomp))return ThreeMfOpcError::Truncated;p+=uncomp32==0xffffffff?8:0;if(comp32==0xffffffff&&!r64(b,p,comp))return ThreeMfOpcError::Truncated;p+=comp32==0xffffffff?8:0;if(off32==0xffffffff&&!r64(b,p,off))return ThreeMfOpcError::Truncated;break;}p+=sz;}}
 const bool directory=!folded.empty()&&folded.back()=='/';if(directory&&(comp||uncomp))return ThreeMfOpcError::InvalidDirectory;if(uncomp>lim.maxEntryBytes)return ThreeMfOpcError::EntryTooLarge;if(!comp?uncomp!=0:uncomp>comp*uint64_t(lim.maxExpansionRatio))return ThreeMfOpcError::ExpansionRatio;if(uncomp>lim.maxExpandedBytes||expanded>lim.maxExpandedBytes-uncomp)return ThreeMfOpcError::AggregateTooLarge;expanded+=uncomp;
 uint16_t lf,lm,ln,lx;if(!r32(b,off,sig)||sig!=Local||!r16(b,off+6,lf)||!r16(b,off+8,lm)||!r16(b,off+26,ln)||!r16(b,off+28,lx)||lf!=flags||lm!=method||ln!=nl)return ThreeMfOpcError::InvalidDirectory;uint64_t data,last;if(!add(off,30+uint64_t(ln)+lx,data)||!add(data,comp,last)||last>coff)return ThreeMfOpcError::Truncated;ranges.emplace_back(off,last);if(!directory){if(folded=="[content_types].xml")content=true;if(folded=="_rels/.rels")roots=true;if(folded.size()>6&&folded.ends_with(".model")&&model.empty())model=folded;if(out)out->parts.push_back({folded,data,comp,uncomp,crc,method});}cursor=next;}
 if(cursor!=cend)return ThreeMfOpcError::InvalidDirectory;std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)return ThreeMfOpcError::Overlap;if(!content)return ThreeMfOpcError::MissingContentTypes;if(!roots)return ThreeMfOpcError::MissingRootRelationships;if(model.empty())return ThreeMfOpcError::MissingStartPart;if(out)out->startPart=model;return ThreeMfOpcError::None;
}

ThreeMfOpcError ExtractThreeMfOpcPart(std::span<const std::byte> bytes,
                                      const ThreeMfOpcPart& part,
                                      std::vector<std::byte>& output,
                                      uint64_t maxExpandedBytes,
                                      const std::function<bool()>& cancelled) {
 output.clear();
 if(cancelled&&cancelled())return ThreeMfOpcError::Cancelled;
 if(part.expandedBytes>maxExpandedBytes||part.expandedBytes>SIZE_MAX)return ThreeMfOpcError::EntryTooLarge;
 uint64_t end;if(!add(part.dataOffset,part.compressedBytes,end)||end>bytes.size())return ThreeMfOpcError::Truncated;
 try{output.resize(static_cast<size_t>(part.expandedBytes));}catch(const std::bad_alloc&){return ThreeMfOpcError::EntryTooLarge;}
 const auto input=bytes.subspan(static_cast<size_t>(part.dataOffset),static_cast<size_t>(part.compressedBytes));
 if(part.method==0){if(part.compressedBytes!=part.expandedBytes){output.clear();return ThreeMfOpcError::InvalidDirectory;}if(!input.empty())std::memcpy(output.data(),input.data(),input.size());}
 else if(part.method==8){
  z_stream stream{};if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK){output.clear();return ThreeMfOpcError::InvalidDirectory;}
  uint64_t inOffset=0,outOffset=0;int result=Z_OK;
  while(result==Z_OK){
   if(cancelled&&cancelled()){inflateEnd(&stream);output.clear();return ThreeMfOpcError::Cancelled;}
   if(stream.avail_in==0&&inOffset<input.size()){const auto count=(std::min)(uint64_t(UINT_MAX),uint64_t(input.size())-inOffset);stream.next_in=reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()+inOffset));stream.avail_in=static_cast<uInt>(count);inOffset+=count;}
   if(stream.avail_out==0&&outOffset<output.size()){const auto count=(std::min)(uint64_t(UINT_MAX),uint64_t(output.size())-outOffset);stream.next_out=reinterpret_cast<Bytef*>(output.data()+outOffset);stream.avail_out=static_cast<uInt>(count);outOffset+=count;}
   result=inflate(&stream,Z_NO_FLUSH);
   if(result==Z_BUF_ERROR)break;
  }
  const bool valid=result==Z_STREAM_END&&stream.total_in==part.compressedBytes&&stream.total_out==part.expandedBytes;
  inflateEnd(&stream);if(!valid){output.clear();return ThreeMfOpcError::InvalidDirectory;}
 } else {output.clear();return ThreeMfOpcError::UnsupportedCompression;}
 if(cancelled&&cancelled()){output.clear();return ThreeMfOpcError::Cancelled;}
 uLong crc=crc32(0,nullptr,0);for(size_t offset=0;offset<output.size();){const auto count=(std::min)(size_t(UINT_MAX),output.size()-offset);crc=crc32(crc,reinterpret_cast<const Bytef*>(output.data()+offset),static_cast<uInt>(count));offset+=count;}
 if(uint32_t(crc)!=part.crc32){output.clear();return ThreeMfOpcError::InvalidDirectory;}
 return ThreeMfOpcError::None;
}
}
