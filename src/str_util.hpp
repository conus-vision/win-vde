// str_util.hpp — pure string / GUID / base64 / host helpers.
// Extracted from vde.cpp so they can be unit-tested (see tests/vdtest.cpp).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <cstdlib>
#pragma comment(lib, "ole32.lib")

inline std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::wstring GuidToString(const GUID& g) { wchar_t b[64]={0}; StringFromGUID2(g,b,64); return std::wstring(b); }
inline bool StringToGuid(const std::wstring& s, GUID& out) { return SUCCEEDED(CLSIDFromString(s.c_str(), &out)); }
inline bool GuidEq(const GUID& a, const GUID& b) { return IsEqualGUID(a, b) != 0; }
inline bool GuidIsZero(const GUID& g) { GUID z = {0}; return GuidEq(g, z); }

inline std::string b64enc(const std::string& in) {
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val=0, bits=-6;
    for (unsigned char c : in) { val=(val<<8)+c; bits+=8; while(bits>=0){ out.push_back(B64[(val>>bits)&0x3F]); bits-=6; } }
    if (bits>-6) out.push_back(B64[((val<<8)>>(bits+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}
inline std::string b64dec(const std::string& in) {
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int T[256]; for(int i=0;i<256;i++)T[i]=-1; for(int i=0;i<64;i++)T[(unsigned char)B64[i]]=i;
    std::string out; int val=0, bits=-8;
    for (unsigned char c : in){ if(c=='='||T[c]==-1)break; val=(val<<6)+T[c]; bits+=6; if(bits>=0){out.push_back(char((val>>bits)&0xFF)); bits-=8;} }
    return out;
}
inline bool ParseI64Strict(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long value = _strtoi64(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = value;
    return true;
}
inline bool ParseIntStrict(const std::string& s, int& out) {
    long long value = 0;
    if (!ParseI64Strict(s, value) || value < INT_MIN || value > INT_MAX) return false;
    out = (int)value;
    return true;
}
inline bool b64decStrict(const std::string& in, std::string& out) {
    if (in.size() % 4 != 0) return false;
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    bool alphabet[256] = {};
    for (int i=0; i<64; ++i) alphabet[(unsigned char)B64[i]] = true;

    size_t firstPadding = in.size();
    for (size_t i=0; i<in.size(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') {
            if (firstPadding == in.size()) firstPadding = i;
        } else {
            if (!alphabet[c] || firstPadding != in.size()) return false;
        }
    }
    if (firstPadding != in.size()) {
        size_t padding = in.size() - firstPadding;
        if (padding > 2 || firstPadding < in.size() - 2) return false;
    }

    std::string decoded = b64dec(in);
    out.swap(decoded);
    return true;
}
inline bool ParseCountsStrict(const std::string& s, std::map<std::string,int>& out) {
    std::map<std::string,int> parsed;
    if (!s.empty()) {
        size_t pos = 0;
        for (;;) {
            size_t comma = s.find(',', pos);
            size_t end = comma == std::string::npos ? s.size() : comma;
            if (end == pos) return false;
            std::string item = s.substr(pos, end - pos);
            size_t colon = item.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == item.size()) return false;
            std::string domain = item.substr(0, colon);
            int count = 0;
            if (!ParseIntStrict(item.substr(colon + 1), count) || count <= 0) return false;
            if (!parsed.emplace(domain, count).second) return false;
            if (comma == std::string::npos) break;
            pos = comma + 1;
            if (pos == s.size()) return false;
        }
    }
    out.swap(parsed);
    return true;
}
inline DWORD GetWindowsBuild() {
    typedef LONG (WINAPI *P)(PRTL_OSVERSIONINFOW);
    HMODULE h=GetModuleHandleW(L"ntdll.dll");
    if(h){ P p=(P)GetProcAddress(h,"RtlGetVersion"); if(p){ RTL_OSVERSIONINFOW vi={0}; vi.dwOSVersionInfoSize=sizeof(vi); if(p(&vi)==0) return vi.dwBuildNumber; } }
    return 0;
}
inline std::string hostOf(const std::string& url){
    size_t s=url.find("://"); if(s==std::string::npos)return ""; s+=3;
    size_t e=url.find_first_of("/:?#",s);
    std::string h=url.substr(s,(e==std::string::npos?url.size():e)-s);
    if(h.size()>4&&h.compare(0,4,"www.")==0)h=h.substr(4);
    for(auto&c:h) if(c>='A'&&c<='Z')c+=32;
    return h;
}
inline std::string etld1(const std::string& host){
    if(host.empty())return "";
    std::vector<std::string> parts; size_t p=0;
    while(true){ size_t d=host.find('.',p); parts.push_back(host.substr(p,(d==std::string::npos?host.size():d)-p)); if(d==std::string::npos)break; p=d+1; }
    if(parts.size()<=2)return host;
    static const std::set<std::string> two={"co","com","org","net","gov","edu","ac"};
    const std::string& last=parts[parts.size()-1]; const std::string& penult=parts[parts.size()-2];
    if(last.size()==2&&two.count(penult)) return parts[parts.size()-3]+"."+penult+"."+last;
    return penult+"."+last;
}

inline bool ReadFileBytes(const std::wstring& path, std::string& out){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    char buf[65536]; DWORD rd=0; out.clear();
    while(ReadFile(f,buf,sizeof(buf),&rd,nullptr)&&rd)out.append(buf,rd);
    CloseHandle(f); return true;
}
inline bool FileExists(const std::wstring& p){ DWORD a=GetFileAttributesW(p.c_str()); return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY); }
