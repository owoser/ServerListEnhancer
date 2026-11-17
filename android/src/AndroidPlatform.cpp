#include "AndroidPlatform.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace sle::android {
namespace {
JavaVM* vm{};
class Jni {
    bool attached{};
public:
    JNIEnv* e{};
    Jni() {
        if(!vm) throw std::runtime_error("Java VM unavailable");
        if(vm->GetEnv(reinterpret_cast<void**>(&e),JNI_VERSION_1_6)!=JNI_OK) {
            if(vm->AttachCurrentThread(&e,nullptr)!=JNI_OK) throw std::runtime_error("Cannot attach Java thread");
            attached=true;
        }
        if(e->PushLocalFrame(64)<0) {e->ExceptionClear(); if(attached) vm->DetachCurrentThread(); throw std::runtime_error("JNI local frame unavailable");}
    }
    ~Jni(){if(e->ExceptionCheck())e->ExceptionClear();e->PopLocalFrame(nullptr);if(attached)vm->DetachCurrentThread();}
    void check(){if(e->ExceptionCheck()){e->ExceptionClear();throw std::runtime_error("Android platform operation failed");}}
    jclass cls(const char* name){auto c=e->FindClass(name);check();if(!c)throw std::runtime_error(name);return c;}
    jmethodID method(jclass c,const char* name,const char* sig){auto m=e->GetMethodID(c,name,sig);check();return m;}
    template<class... A> jobject obj(jobject o,const char* name,const char* sig,A... a){if(!o)throw std::runtime_error(name);auto c=e->GetObjectClass(o);auto m=method(c,name,sig);e->DeleteLocalRef(c);auto r=e->CallObjectMethod(o,m,a...);check();return r;}
    template<class... A> jint integer(jobject o,const char* name,const char* sig,A... a){if(!o)throw std::runtime_error(name);auto c=e->GetObjectClass(o);auto m=method(c,name,sig);e->DeleteLocalRef(c);auto r=e->CallIntMethod(o,m,a...);check();return r;}
    template<class... A> void call(jobject o,const char* name,const char* sig,A... a){if(!o)throw std::runtime_error(name);auto c=e->GetObjectClass(o);auto m=method(c,name,sig);e->DeleteLocalRef(c);e->CallVoidMethod(o,m,a...);check();}
};
struct Close {
    JNIEnv* e; jobject object; const char* name;
    ~Close(){if(!object)return;auto c=e->GetObjectClass(object);auto m=e->GetMethodID(c,name,"()V");if(m)e->CallVoidMethod(object,m);if(e->ExceptionCheck())e->ExceptionClear();e->DeleteLocalRef(c);}
};
// Close the connection from another attached thread even while response headers block.
class RequestDeadline {
    JNIEnv* owner;
    jobject connection;
    std::mutex mutex;
    std::condition_variable wake;
    bool done{};
    std::thread timer;
public:
    RequestDeadline(Jni& j,jobject conn,int timeoutMs,const std::atomic<bool>* running):owner(j.e),connection(j.e->NewGlobalRef(conn)) {
        j.check();if(!connection)throw std::runtime_error("Cannot retain HTTP connection");
        try {timer=std::thread([this,timeoutMs,running]{
            const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
            std::unique_lock lock(mutex);
            while(!done&&std::chrono::steady_clock::now()<end&&(!running||running->load()))wake.wait_for(lock,std::chrono::milliseconds(50));
            if(done)return;lock.unlock();
            try {Jni callback;callback.call(connection,"disconnect","()V");}catch(...){}
        });}catch(...){owner->DeleteGlobalRef(connection);throw;}
    }
    ~RequestDeadline(){ {std::lock_guard lock(mutex);done=true;}wake.notify_all();if(timer.joinable())timer.join();owner->DeleteGlobalRef(connection);}
};
}
void setJavaVm(JavaVM* value){vm=value;}
Surface surface(){
    try {
        Jni j; auto c=j.cls("android/app/ActivityThread");auto m=j.e->GetStaticMethodID(c,"currentApplication","()Landroid/app/Application;");j.check();
        auto app=j.e->CallStaticObjectMethod(c,m);j.check();
        auto resources=j.obj(app,"getResources","()Landroid/content/res/Resources;");
        auto metrics=j.obj(resources,"getDisplayMetrics","()Landroid/util/DisplayMetrics;");auto mc=j.e->GetObjectClass(metrics);
        auto width=j.e->GetFieldID(mc,"widthPixels","I");auto height=j.e->GetFieldID(mc,"heightPixels","I");auto density=j.e->GetFieldID(mc,"density","F");j.check();
        return {static_cast<float>(j.e->GetIntField(metrics,width)),static_cast<float>(j.e->GetIntField(metrics,height)),j.e->GetFloatField(metrics,density)};
    } catch(...) {return {};}
}
Bytes download(const std::string& url,std::size_t limit,int timeoutMs,const std::atomic<bool>* running){
    Bytes result;
    if(url.size()>512 || (!url.starts_with("http://")&&!url.starts_with("https://")) || url.find_first_of("\r\n")!=url.npos) return {{},"Unsupported image or metadata URL"};
    try {
        Jni j;auto c=j.cls("java/net/URL");auto ctor=j.method(c,"<init>","(Ljava/lang/String;)V");auto str=j.e->NewStringUTF(url.c_str());j.check();auto u=j.e->NewObject(c,ctor,str);j.check();
        auto conn=j.obj(u,"openConnection","()Ljava/net/URLConnection;");Close disconnect{j.e,conn,"disconnect"};
        j.call(conn,"setConnectTimeout","(I)V",timeoutMs);j.call(conn,"setReadTimeout","(I)V",std::min(timeoutMs,1000));
        j.call(conn,"setInstanceFollowRedirects","(Z)V",JNI_FALSE);j.call(conn,"setUseCaches","(Z)V",JNI_FALSE);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
        RequestDeadline cancel(j,conn,timeoutMs,running);
        if(j.integer(conn,"getResponseCode","()I")!=200)throw std::runtime_error("HTTP response is not 200 (redirects are disabled)");
        auto length=j.integer(conn,"getContentLength","()I");if(length>0&&static_cast<std::size_t>(length)>limit)throw std::runtime_error("Response exceeds byte limit");
        auto stream=j.obj(conn,"getInputStream","()Ljava/io/InputStream;");Close close{j.e,stream,"close"};auto buffer=j.e->NewByteArray(8192);j.check();
        while(true){
            if((running&&!running->load())||std::chrono::steady_clock::now()>deadline)throw std::runtime_error("HTTP cancelled or deadline exceeded");
            auto n=j.integer(stream,"read","([B)I",buffer);if(n<0)break;if(n==0)continue;
            if(static_cast<std::size_t>(n)>limit-result.data.size())throw std::runtime_error("Response exceeds byte limit");
            auto before=result.data.size();result.data.resize(before+n);j.e->GetByteArrayRegion(buffer,0,n,reinterpret_cast<jbyte*>(result.data.data()+before));j.check();
        }
        if(result.data.empty())throw std::runtime_error("Empty HTTP body");
    } catch(const std::exception& e){result.data.clear();result.error=e.what();}
    return result;
}
Pixels decodeImage(std::span<const unsigned char> bytes,bool banner){
    Pixels result;
    if(bytes.empty()||bytes.size()>2*1024*1024)return result;
    bool png=bytes.size()>=8&&std::memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)==0;
    bool jpeg=bytes.size()>=3&&bytes[0]==0xff&&bytes[1]==0xd8&&bytes[2]==0xff;
    bool gif=bytes.size()>=6&&(std::memcmp(bytes.data(),"GIF87a",6)==0||std::memcmp(bytes.data(),"GIF89a",6)==0);
    if(!png&&!jpeg&&!gif)return result;
    try {
        Jni j;auto array=j.e->NewByteArray(static_cast<jsize>(bytes.size()));j.check();j.e->SetByteArrayRegion(array,0,bytes.size(),reinterpret_cast<const jbyte*>(bytes.data()));j.check();
        auto optionsClass=j.cls("android/graphics/BitmapFactory$Options");auto opt=j.e->NewObject(optionsClass,j.method(optionsClass,"<init>","()V"));j.check();
        auto bounds=j.e->GetFieldID(optionsClass,"inJustDecodeBounds","Z");auto outW=j.e->GetFieldID(optionsClass,"outWidth","I");auto outH=j.e->GetFieldID(optionsClass,"outHeight","I");auto sample=j.e->GetFieldID(optionsClass,"inSampleSize","I");j.check();
        j.e->SetBooleanField(opt,bounds,JNI_TRUE);
        auto factory=j.cls("android/graphics/BitmapFactory");auto decode=j.e->GetStaticMethodID(factory,"decodeByteArray","([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;");j.check();
        j.e->CallStaticObjectMethod(factory,decode,array,0,static_cast<jint>(bytes.size()),opt);j.check();
        int w=j.e->GetIntField(opt,outW),h=j.e->GetIntField(opt,outH);if(w<=0||h<=0||w>4096||h>4096||static_cast<std::int64_t>(w)*h>4000000)return {};
        int divisor=1;while(w/divisor>(banner?1280:512)||h/divisor>(banner?720:512))divisor*=2;
        j.e->SetBooleanField(opt,bounds,JNI_FALSE);j.e->SetIntField(opt,sample,divisor);
        auto bitmap=j.e->CallStaticObjectMethod(factory,decode,array,0,static_cast<jint>(bytes.size()),opt);j.check();if(!bitmap)return {};
        Close recycle{j.e,bitmap,"recycle"};result.width=j.integer(bitmap,"getWidth","()I");result.height=j.integer(bitmap,"getHeight","()I");
        auto pixels=j.e->NewIntArray(result.width*result.height);j.check();j.call(bitmap,"getPixels","([IIIIIII)V",pixels,0,result.width,0,0,result.width,result.height);
        std::vector<jint> argb(result.width*result.height);j.e->GetIntArrayRegion(pixels,0,argb.size(),argb.data());j.check();result.rgba.resize(argb.size()*4);
        for(std::size_t i=0;i<argb.size();++i){auto p=static_cast<std::uint32_t>(argb[i]);result.rgba[4*i]=p>>16;result.rgba[4*i+1]=p>>8;result.rgba[4*i+2]=p;result.rgba[4*i+3]=p>>24;}
    } catch(...) {return {};}
    return result;
}
Bytes decodeInlinePng(std::string_view value){
    constexpr std::string_view prefix="data:image/png;base64,";Bytes out;
    if(!value.starts_with(prefix))return {{},"Inline logo must be PNG"};value.remove_prefix(prefix.size());
    if(value.empty()||value.size()%4||value.size()>32768)return {{},"Inline logo exceeds 24 KiB"};
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(std::size_t i=0;i<value.size();i+=4){unsigned n{};int pad{};for(int k=0;k<4;++k){char c=value[i+k];auto v=alphabet.find(c);if(c=='='){if(i+4!=value.size()||k<2)return {{},"Invalid base64 padding"};++pad;v=0;}else if(v==alphabet.npos||pad)return {{},"Invalid base64"};n=(n<<6)|v;}
        if((pad==1&&(n&255))||(pad==2&&(n&65535)))return {{},"Noncanonical base64"};out.data.push_back(n>>16);if(pad<2)out.data.push_back(n>>8);if(!pad)out.data.push_back(n);
    }
    if(out.data.size()>24576||out.data.size()<8||std::memcmp(out.data.data(),"\x89PNG\r\n\x1a\n",8))return {{},"Invalid inline PNG"};return out;
}
}
