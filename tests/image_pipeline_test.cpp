#include "../src-client/ImageProvider.cpp"
#include <cassert>
#include <iostream>
int main(){
 using namespace sle::client;
 image_detail::ComScope com;
 image_detail::ComPtr<IWICImagingFactory> factory;
 assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf()))));
 auto logo=image_detail::normalize(factory.Get(),builtin::logo(),false,image_detail::Clock::now()+std::chrono::seconds(10));
 auto banner=image_detail::normalize(factory.Get(),builtin::banner(),true,image_detail::Clock::now()+std::chrono::seconds(10));
 assert(logo&&banner);
 auto encode=[&](UINT w,UINT h,bool jpeg=false){
  image_detail::ComPtr<IStream> stream;image_detail::ComPtr<IWICBitmapEncoder> encoder;image_detail::ComPtr<IWICBitmapFrameEncode> frame;
  assert(SUCCEEDED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)));
  assert(SUCCEEDED(factory->CreateEncoder(jpeg?GUID_ContainerFormatJpeg:GUID_ContainerFormatPng,nullptr,&encoder)));
  assert(SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)));
  assert(SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(w,h)));
  auto format=jpeg?GUID_WICPixelFormat24bppBGR:GUID_WICPixelFormat32bppBGRA;assert(SUCCEEDED(frame->SetPixelFormat(&format)));
  UINT stride=w*(jpeg?3:4);image_detail::Bytes pixels(static_cast<size_t>(stride)*h,128);
  assert(SUCCEEDED(frame->WritePixels(h,stride,static_cast<UINT>(pixels.size()),pixels.data()))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()));
  STATSTG stat{};HGLOBAL memory{};assert(SUCCEEDED(stream->Stat(&stat,STATFLAG_NONAME))&&SUCCEEDED(GetHGlobalFromStream(stream.Get(),&memory)));
  auto* data=static_cast<unsigned char*>(GlobalLock(memory));image_detail::Bytes bytes(data,data+stat.cbSize.LowPart);GlobalUnlock(memory);return bytes;
 };
 auto check=[&](auto const& bytes){return image_detail::normalize(factory.Get(),bytes,false,image_detail::Clock::now()+std::chrono::seconds(10));};
 assert(check(encode(64,64,true))); // JPEG input becomes static PNG.
 assert(!check(encode(4097,1)));assert(!check(encode(2048,2048))); // Side/pixel bombs.
 image_detail::Bytes corrupt{137,80,78,71,13,10,26,10};assert(!check(corrupt));
 image_detail::Bytes tooLarge(ImageProvider::MaxDownloadBytes+1,0);assert(!check(tooLarge));
 auto truncated=*logo;truncated.resize(30);assert(!check(truncated));
 assert(!image_detail::decodeInline("data:image/png;base64,"+std::string(32772,'A')));
 assert(!image_detail::decodeInline("data:image/png;base64,!!!!"));
 assert(!image_detail::decodeInline("data:image/png;base64,AB=="));
 assert(!image_detail::validReference({"url","file:///c:/private.png"}));
 assert(!image_detail::validReference({"url","https://example.com/x\r\nX: 1"}));
 auto& images=ImageProvider::getInstance();images.initialize("build/image-test-cache");
 assert(!images.builtinPath().empty()&&!images.builtinPath(true).empty());
 // Strict base64 round trip, worker publication, then reuse after reinitialize.
 auto b64=[](std::span<unsigned char const> bytes){static constexpr char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out="data:image/png;base64,";for(size_t i=0;i<bytes.size();i+=3){unsigned v=unsigned(bytes[i])<<16;if(i+1<bytes.size())v|=unsigned(bytes[i+1])<<8;if(i+2<bytes.size())v|=bytes[i+2];out+=table[v>>18];out+=table[(v>>12)&63];out+=i+1<bytes.size()?table[(v>>6)&63]:'=';out+=i+2<bytes.size()?table[v&63]:'=';}return out;};
 sle::ImageReference ref{"data",b64(builtin::logo())};assert(image_detail::decodeInline(ref.value));
 images.start();images.request(ref);
 auto until=image_detail::Clock::now()+std::chrono::seconds(3);while(!images.get(ref)&&image_detail::Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
 assert(images.get(ref));images.stop();
 images.initialize("build/image-test-cache");images.start();images.request(ref);
 until=image_detail::Clock::now()+std::chrono::seconds(3);while(!images.get(ref)&&image_detail::Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));assert(images.get(ref));images.stop();
 // Repeated visible/hidden cycles must release decoded GIFs, using disk only.
 std::ifstream gifStream("build/gif-fixtures/banner.gif",std::ios::binary);
 image_detail::Bytes gifBytes{std::istreambuf_iterator<char>(gifStream),{}};assert(!gifBytes.empty());
 sle::ImageReference gifRef{"url","https://example.com/cached-banner.gif"};
 auto dir=images.builtinPath().parent_path();auto hash=image_detail::sha256(gifBytes);
 auto keyHash=image_detail::sha256(image_detail::referenceKey(gifRef,true));
 auto putText=[&](auto const& p,std::string const& s){assert(image_detail::atomicWrite(p,std::span{reinterpret_cast<unsigned char const*>(s.data()),s.size()}));};
 assert(image_detail::atomicWrite(dir/(hash+".gif"),gifBytes));putText(dir/(keyHash+".gref"),hash);
 putText(dir/(keyHash+".ref"),images.builtinPath(true).stem().string());
 images.start();
 for(int cycle=0;cycle<10;++cycle){images.tick(true);images.request(gifRef,true);
  auto end=image_detail::Clock::now()+std::chrono::seconds(3);
  while(!images.animation(gifRef,true)&&image_detail::Clock::now()<end)std::this_thread::yield();
  assert(images.animation(gifRef,true));images.tick(false);
  end=image_detail::Clock::now()+std::chrono::seconds(3);
  while(gifMemoryBytes()&&image_detail::Clock::now()<end)std::this_thread::yield();
  assert(!images.animation(gifRef,true)&&gifMemoryBytes()==0);
 }
 images.stop();assert(gifMemoryBytes()==0);
 // A successfully cached static replacement invalidates the old GIF mapping.
 assert(image_detail::atomicWrite(dir/(keyHash+".gref"),std::span<unsigned char const>{}));
 images.initialize("build/image-test-cache");images.start();images.tick(true);images.request(gifRef,true);
 until=image_detail::Clock::now()+std::chrono::seconds(3);
 while(!images.get(gifRef,true)&&image_detail::Clock::now()<until)std::this_thread::yield();
 assert(images.get(gifRef,true));images.stop();assert(!images.animation(gifRef,true)&&gifMemoryBytes()==0);
 std::cout<<"WIC PNG/JPEG bounds, inline/URL safety, ten cached GIF open/close cycles return CPU memory to zero: PASS\n";
}
