#include "../src-client/GifAnimation.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <cassert>
#include <iostream>
using namespace sle::client;
std::vector<unsigned char> load(std::string name){std::ifstream s("build/gif-fixtures/"+name+".gif",std::ios::binary);return {(std::istreambuf_iterator<char>(s)),{}};}
int main(){CoInitializeEx(nullptr,COINIT_MULTITHREADED);Microsoft::WRL::ComPtr<IWICImagingFactory> factory;assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))));
 auto decode=[&](std::string name,bool banner=false){auto bytes=load(name);std::string error;auto a=decodeGif(factory.Get(),bytes,banner,std::chrono::steady_clock::now()+std::chrono::seconds(10),error);if(!a)std::cout<<name<<": "<<error<<"\n";return a;};
 for(auto name:{"logo","banner"}){auto a=decode(name,std::string(name)=="banner");assert(a);std::cout<<name<<" "<<a->width<<"x"<<a->height<<" frames="<<a->frames.size()<<" repeats="<<a->repeats<<" memory="<<gifMemoryBytes()<<"\n";assert(a->frameAt(std::chrono::milliseconds(0))==0);assert(a->frames[0]!=a->frames[1]);}
 assert(gifMemoryBytes()==0);
 {auto a=decode("disposal");assert(a&&a->frames.size()==4&&a->repeats==3);
  auto pixel=[&](int frame,int x){return std::vector<unsigned char>(a->frames[frame].begin()+x*4,a->frames[frame].begin()+x*4+4);};
  assert((pixel(0,0)==std::vector<unsigned char>{255,0,0,255}));
  assert((pixel(1,0)==std::vector<unsigned char>{0,255,0,255}));
  assert(pixel(2,0)[3]==0);assert((pixel(2,2)==std::vector<unsigned char>{0,0,255,255}));
  assert(pixel(3,0)[3]==0);assert((pixel(3,1)==std::vector<unsigned char>{0,255,0,255}));assert((pixel(3,2)==std::vector<unsigned char>{255,0,0,255}));
  assert(a->frameAt(std::chrono::milliseconds(100))==1);assert(a->frameAt(std::chrono::milliseconds(400))==0);assert(a->frameAt(std::chrono::milliseconds(1200))==3);
 }
 {auto a=decode("local-palette");assert(a&&a->repeats==1&&a->frames[0][0]==255&&a->frames[0][1]==255&&a->frames[0][2]==0);}
 {auto a=decode("zero-delay");assert(a&&a->delaysMs[0]==100);}
 for(auto name:{"too-many","too-long","too-wide"})assert(!decode(name));
 {auto bytes=load("logo");bytes.resize(25);std::string e;assert(!decodeGif(factory.Get(),bytes,false,std::chrono::steady_clock::now()+std::chrono::seconds(1),e));}
 {std::vector<std::shared_ptr<GifAnimation const>> retained;for(int i=0;i<4;++i){auto a=decode("logo");assert(a);retained.push_back(a);}assert(!decode("logo"));}
 assert(gifMemoryBytes()==0);std::cout<<"GIF composition/disposal/palette/loop/time/budgets and memory release: PASS\n";
}
