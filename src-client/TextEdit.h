#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
namespace sle::client {
struct TextEdit {
 std::size_t caret{},anchor{},view{};
 static std::size_t prev(std::string_view s,std::size_t p){if(!p)return 0;--p;while(p&&(static_cast<unsigned char>(s[p])&0xc0)==0x80)--p;return p;}
 static std::size_t next(std::string_view s,std::size_t p){if(p>=s.size())return s.size();++p;while(p<s.size()&&(static_cast<unsigned char>(s[p])&0xc0)==0x80)++p;return p;}
 void clamp(std::string const& s){caret=std::min(caret,s.size());anchor=std::min(anchor,s.size());view=std::min(view,s.size());}
 void end(std::string const& s){caret=anchor=s.size();view=0;}
 bool selected()const{return caret!=anchor;}
 std::string selectedText(std::string const& s)const{return s.substr(std::min(caret,anchor),std::max(caret,anchor)-std::min(caret,anchor));}
 void eraseSelection(std::string& s){if(selected()){auto a=std::min(caret,anchor),b=std::max(caret,anchor);s.erase(a,b-a);caret=anchor=a;}}
 void insert(std::string& s,std::string_view text,std::size_t limit){
  std::string clean;for(char c:text)if(static_cast<unsigned char>(c)>=32&&c!=127)clean+=c;
  auto const keep=s.size()-(std::max(caret,anchor)-std::min(caret,anchor));
  if(keep>=limit)return;auto count=std::min(clean.size(),limit-keep);while(count<clean.size()&&count&&(static_cast<unsigned char>(clean[count])&0xc0)==0x80)--count;
  eraseSelection(s);s.insert(caret,clean,0,count);caret+=count;anchor=caret;
 }
 void left(std::string const& s,bool shift){if(!shift&&selected())caret=std::min(caret,anchor);else caret=prev(s,caret);if(!shift)anchor=caret;}
 void right(std::string const& s,bool shift){if(!shift&&selected())caret=std::max(caret,anchor);else caret=next(s,caret);if(!shift)anchor=caret;}
 void backspace(std::string& s){if(selected())eraseSelection(s);else if(caret){auto p=prev(s,caret);s.erase(p,caret-p);caret=anchor=p;}}
 void del(std::string& s){if(selected())eraseSelection(s);else if(caret<s.size())s.erase(caret,next(s,caret)-caret);}
};
}
