#include "GifAnimation.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace sle::client {
namespace {
std::atomic_size_t memory{};
constexpr std::size_t GlobalBudget=64u*1024*1024, LocalBudget=16u*1024*1024;
using Microsoft::WRL::ComPtr;
unsigned meta(IWICMetadataQueryReader* reader,wchar_t const* key,unsigned fallback=0){
 if(!reader)return fallback;PROPVARIANT v{};unsigned value=fallback;
 if(SUCCEEDED(reader->GetMetadataByName(key,&v))){if(v.vt==VT_UI1)value=v.bVal;else if(v.vt==VT_UI2)value=v.uiVal;else if(v.vt==VT_UI4)value=v.ulVal;else if(v.vt==VT_BOOL)value=v.boolVal!=0;}
 PropVariantClear(&v);return value;
}
}
GifAnimation::~GifAnimation(){memory.fetch_sub(reservedBytes);}
std::size_t gifMemoryBytes(){return memory.load();}
bool isGif(std::span<unsigned char const> b){return b.size()>=6&&(!std::memcmp(b.data(),"GIF87a",6)||!std::memcmp(b.data(),"GIF89a",6));}
std::size_t GifAnimation::frameAt(std::chrono::milliseconds elapsed) const {
 if(frames.empty())return 0;std::uint64_t total{};for(auto d:delaysMs)total+=d;
 auto t=static_cast<std::uint64_t>(std::max<std::int64_t>(0,elapsed.count()));
 if(!total)return 0;if(repeats&&t/total>=repeats)return frames.size()-1;t%=total;
 for(std::size_t i=0;i<delaysMs.size();++i){if(t<delaysMs[i])return i;t-=delaysMs[i];}return frames.size()-1;
}
std::shared_ptr<GifAnimation const> decodeGif(IWICImagingFactory* factory,std::span<unsigned char const> bytes,bool banner,std::chrono::steady_clock::time_point deadline,std::string& error){
 auto fail=[&](char const* e)->std::shared_ptr<GifAnimation const>{error=e;return {};};
 if(!factory||!isGif(bytes)||bytes.size()>2u*1024*1024)return fail("GIF format/size invalid");
 ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICMetadataQueryReader> reader;
 if(FAILED(factory->CreateStream(&stream))||FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),static_cast<DWORD>(bytes.size())))||FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder)))return fail("GIF decoder failed");
 decoder->GetMetadataQueryReader(&reader);UINT count{};if(FAILED(decoder->GetFrameCount(&count))||!count||count>120)return fail("GIF frame count exceeds 120");
 unsigned sw=meta(reader.Get(),L"/logscrdesc/Width"),sh=meta(reader.Get(),L"/logscrdesc/Height");
 if(!sw||!sh||sw>1024||sh>1024)return fail("GIF logical canvas exceeds 1024x1024");
 std::size_t const canvasBytes=std::size_t(sw)*sh*4;
 auto animation=std::make_shared<GifAnimation>();animation->sourceWidth=sw;animation->sourceHeight=sh;
 double scale=std::min({1.0,double(banner?640:256)/sw,double(banner?360:256)/sh});
 // Reserve space for original, restore-previous and local frame canvases too.
 auto available=LocalBudget-3*canvasBytes;
 scale=std::min(scale,std::sqrt(double(available)/double(std::size_t(count)*sw*sh*4)));
 unsigned w=std::max(1u,static_cast<unsigned>(sw*scale)),h=std::max(1u,static_cast<unsigned>(sh*scale));
 std::size_t outputBytes=std::size_t(count)*w*h*4, reserve=outputBytes+3*canvasBytes;
 auto previous=memory.load();do{if(previous>GlobalBudget-reserve)return fail("GIF global memory budget reached");}while(!memory.compare_exchange_weak(previous,previous+reserve));
 animation->reservedBytes=reserve;animation->width=w;animation->height=h;
 // Netscape loop count means repeats after the first play. Absent extension: once.
 PROPVARIANT app{},data{};
 if(reader&&SUCCEEDED(reader->GetMetadataByName(L"/appext/application",&app))&&app.vt==(VT_VECTOR|VT_UI1)&&app.caub.cElems>=11&&
    (!std::memcmp(app.caub.pElems,"NETSCAPE2.0",11)||!std::memcmp(app.caub.pElems,"ANIMEXTS1.0",11))&&
    SUCCEEDED(reader->GetMetadataByName(L"/appext/data",&data))&&data.vt==(VT_VECTOR|VT_UI1)&&data.caub.cElems>=4){auto* p=data.caub.pElems;unsigned loops=p[2]|(unsigned(p[3])<<8);animation->repeats=loops?loops+1:0;}
 PropVariantClear(&app);PropVariantClear(&data);
 std::vector<unsigned char> canvas(canvasBytes,0),saved;
 unsigned totalMs{};
 for(UINT i=0;i<count;++i){
  if(std::chrono::steady_clock::now()>=deadline)return fail("GIF decode timeout");
  ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICMetadataQueryReader> fr;ComPtr<IWICFormatConverter> converter;
  UINT fw{},fh{};if(FAILED(decoder->GetFrame(i,&frame))||FAILED(frame->GetSize(&fw,&fh)))return fail("GIF frame decode failed");
  frame->GetMetadataQueryReader(&fr);unsigned left=meta(fr.Get(),L"/imgdesc/Left"),top=meta(fr.Get(),L"/imgdesc/Top");
  if(!fw||!fh||fw>sw||fh>sh||left>sw-fw||top>sh-fh)return fail("GIF frame outside logical canvas");
  unsigned disposal=meta(fr.Get(),L"/grctlext/Disposal");if(disposal>3)return fail("GIF disposal mode invalid");
  bool transparent=meta(fr.Get(),L"/grctlext/TransparencyFlag")!=0;
  unsigned delay=meta(fr.Get(),L"/grctlext/Delay",10)*10;
  if(delay<20)delay=100; // Browser-like handling of zero/one-centisecond frames.
  totalMs+=delay;if(totalMs>30000)return fail("GIF cycle exceeds 30 seconds");
  if(i==0&&!transparent){
   ComPtr<IWICPalette> palette;UINT n{};WICColor colors[256]{};
   if(SUCCEEDED(factory->CreatePalette(&palette))&&SUCCEEDED(decoder->CopyPalette(palette.Get()))&&SUCCEEDED(palette->GetColors(256,colors,&n))){auto index=meta(reader.Get(),L"/logscrdesc/BackgroundColorIndex");if(index<n){auto c=colors[index];for(std::size_t k=0;k<canvas.size();k+=4){canvas[k]=BYTE(c>>16);canvas[k+1]=BYTE(c>>8);canvas[k+2]=BYTE(c);canvas[k+3]=255;}}}
  }
  if(disposal==3)saved=canvas;
  if(FAILED(factory->CreateFormatConverter(&converter))||FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return fail("GIF pixel conversion failed");
  std::vector<unsigned char> pixels(std::size_t(fw)*fh*4);
  if(FAILED(converter->CopyPixels(nullptr,fw*4,static_cast<UINT>(pixels.size()),pixels.data())))return fail("GIF pixels invalid");
  for(unsigned y=0;y<fh;++y)for(unsigned x=0;x<fw;++x){auto s=(std::size_t(y)*fw+x)*4,d=(std::size_t(y+top)*sw+x+left)*4;if(pixels[s+3])std::memcpy(canvas.data()+d,pixels.data()+s,4);}
  std::vector<unsigned char> output(std::size_t(w)*h*4);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){auto s=(std::size_t(y*sh/h)*sw+x*sw/w)*4,d=(std::size_t(y)*w+x)*4;std::memcpy(output.data()+d,canvas.data()+s,4);}
  animation->frames.push_back(std::move(output));animation->delaysMs.push_back(delay);
  if(disposal==2){
   // Transparent background when flagged; otherwise use the logical-screen palette.
   unsigned char bg[4]{};
   if(!transparent){ComPtr<IWICPalette> palette;UINT n{};WICColor colors[256]{};if(SUCCEEDED(factory->CreatePalette(&palette))&&SUCCEEDED(decoder->CopyPalette(palette.Get()))&&SUCCEEDED(palette->GetColors(256,colors,&n))){auto index=meta(reader.Get(),L"/logscrdesc/BackgroundColorIndex");if(index<n){auto c=colors[index];bg[0]=BYTE(c>>16);bg[1]=BYTE(c>>8);bg[2]=BYTE(c);bg[3]=255;}}}
   for(unsigned y=0;y<fh;++y)for(unsigned x=0;x<fw;++x)std::memcpy(canvas.data()+(std::size_t(y+top)*sw+x+left)*4,bg,4);
  }else if(disposal==3)canvas.swap(saved);
 }
 memory.fetch_sub(reserve-outputBytes);animation->reservedBytes=outputBytes;
 return animation;
}
}
