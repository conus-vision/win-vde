// layout.hpp — desktop/window layout records + (de)serialization + grace logic.
// Pure logic (no COM / no GUI) so it can be unit-tested (see tests/vdtest.cpp).
#pragma once
#include "str_util.hpp"
#include <string>
#include <vector>
#include <map>

struct DeskRec { int index; GUID guid; std::wstring name; };

inline std::string CountsToStr(const std::map<std::string,int>& c){ std::string s; bool f=true; for(auto& kv:c){ if(!f)s+=","; f=false; s+=kv.first+":"+std::to_string(kv.second);} return s; }
inline std::map<std::string,int> StrToCounts(const std::string& s){ std::map<std::string,int> c; size_t p=0;
    while(p<s.size()){ size_t comma=s.find(',',p); std::string item=s.substr(p,(comma==std::string::npos?s.size():comma)-p); p=(comma==std::string::npos?s.size():comma+1);
        size_t col=item.rfind(':'); if(col!=std::string::npos)c[item.substr(0,col)]=atoi(item.substr(col+1).c_str()); } return c; }
