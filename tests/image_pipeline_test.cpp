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
 std::cout<<"WIC PNG decode/encode, builtin release, bad base64, URL policy: PASS\n";
}
