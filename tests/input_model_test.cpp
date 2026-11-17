#include "../src-client/AddressParser.h"
#include "../src-client/TextEdit.h"
#include <cassert>
#include <iostream>
int main(){using namespace sle::client;
 for(auto s:{"example.com","localhost:19132","LOCALHOST","xn--fsqu00a.xn--0zwm56d","127.0.0.1:19132","  example.com:1  ","[::1]:65535","[2001:db8::1]","[::ffff:192.0.2.1]:19132"}){auto r=parseServerAddress(s);if(!r)std::cerr<<s<<":"<<r.error<<"\n";assert(r);}
 for(auto s:{"","127.0.0.1:19132a","example.com:0","example.com:65536","example.com:","https://example.com","a b.com","::1","[::::]","[:::1]","[:1::]","[1::2:]","[1:2:3]","[::1]a","1.2.3.999","a.-b.com","a..b","a.com:19132:19132","[127.0.0.1]","111111sssdaasdas:19132","server","127.0.0.256","127.0.0","01.2.3.4","1.2.3.4.5","example.123",".example.com","a_b.com","a.com/path","a.com@evil.com"})assert(!parseServerAddress(s));
 auto r=parseServerAddress("[::1]:19132");assert(displayAddress(r.host,r.port)=="[::1]:19132");
 TextEdit e;std::string s="你好abc";e.end(s);e.left(s,false);e.backspace(s);assert(s=="你好ac");e.caret=e.anchor=0;e.right(s,false);e.del(s);assert(s=="你ac");e.anchor=0;e.caret=s.size();e.insert(s,"127.0.0.1:19132",255);assert(s=="127.0.0.1:19132");e.left(s,true);assert(e.selectedText(s)=="2");e.insert(s,"3",255);assert(s=="127.0.0.1:19133");e.anchor=0;e.caret=s.size();e.insert(s,"你好",4);assert(s=="你");
 std::cout<<"Strict host/port/IPv4/IPv6, UTF8 selection/caret/edit boundaries: PASS\n";
}
