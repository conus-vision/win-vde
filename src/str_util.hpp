// str_util.hpp — pure string / GUID / base64 / host helpers.
// Extracted from vde.cpp so they can be unit-tested (see tests/vdtest.cpp).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
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
    std::string out; out.reserve(((in.size()+2)/3)*4);
    size_t i=0;
    while(in.size()-i>=3){
        uint32_t value=(uint32_t)(unsigned char)in[i]<<16 |
            (uint32_t)(unsigned char)in[i+1]<<8 | (uint32_t)(unsigned char)in[i+2];
        out.push_back(B64[(value>>18)&0x3F]); out.push_back(B64[(value>>12)&0x3F]);
        out.push_back(B64[(value>>6)&0x3F]); out.push_back(B64[value&0x3F]);
        i+=3;
    }
    size_t remaining=in.size()-i;
    if(remaining==1){
        uint32_t value=(uint32_t)(unsigned char)in[i]<<16;
        out.push_back(B64[(value>>18)&0x3F]); out.push_back(B64[(value>>12)&0x3F]);
        out.push_back('='); out.push_back('=');
    } else if(remaining==2){
        uint32_t value=(uint32_t)(unsigned char)in[i]<<16 | (uint32_t)(unsigned char)in[i+1]<<8;
        out.push_back(B64[(value>>18)&0x3F]); out.push_back(B64[(value>>12)&0x3F]);
        out.push_back(B64[(value>>6)&0x3F]); out.push_back('=');
    }
    return out;
}
inline std::string b64dec(const std::string& in) {
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int T[256]; for(int i=0;i<256;i++)T[i]=-1; for(int i=0;i<64;i++)T[(unsigned char)B64[i]]=i;
    std::string out; size_t i=0;
    while(i<in.size()){
        int a=T[(unsigned char)in[i++]]; if(a<0)break;
        if(i>=in.size())break; int b=T[(unsigned char)in[i++]]; if(b<0)break;
        out.push_back((char)(((unsigned)a<<2)|((unsigned)b>>4)));
        if(i>=in.size()||in[i]=='=')break; int c=T[(unsigned char)in[i++]]; if(c<0)break;
        out.push_back((char)((((unsigned)b&0x0F)<<4)|((unsigned)c>>2)));
        if(i>=in.size()||in[i]=='=')break; int d=T[(unsigned char)in[i++]]; if(d<0)break;
        out.push_back((char)((((unsigned)c&0x03)<<6)|(unsigned)d));
    }
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
    if (b64enc(decoded) != in) return false;
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

// Explicit, per-call Win32 seam used by layout storage fault-injection tests.
// It is deliberately a value object: callers can replace one operation without
// mutating process-global state, and the override dies with the test scope.
struct LayoutFsOps {
    std::function<HANDLE(const std::wstring&,DWORD,DWORD,DWORD,DWORD)> openFile;
    std::function<BOOL(HANDLE,void*,DWORD,DWORD&)> readFile;
    std::function<BOOL(HANDLE,const void*,DWORD,DWORD&)> writeFile;
    std::function<BOOL(HANDLE)> flushFile;
    std::function<BOOL(HANDLE)> closeHandle;
    std::function<BOOL(const std::wstring&,const std::wstring&,const std::wstring&,DWORD)> replaceFile;
    std::function<BOOL(const std::wstring&,const std::wstring&,DWORD)> moveFile;
    std::function<BOOL(const std::wstring&,const std::wstring&,BOOL)> copyFile;
    std::function<BOOL(const std::wstring&)> deleteFile;
    std::function<DWORD(const std::wstring&)> getAttributes;
    std::function<BOOL(HANDLE,unsigned long long&)> getSize;
    std::function<BOOL(HANDLE,unsigned long long&)> getMtime;

    LayoutFsOps(){
        openFile=[](const std::wstring& path,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE {
            return CreateFileW(path.c_str(),access,share,nullptr,creation,flags,nullptr);
        };
        readFile=[](HANDLE file,void* buffer,DWORD requested,DWORD& read)->BOOL {
            return ReadFile(file,buffer,requested,&read,nullptr);
        };
        writeFile=[](HANDLE file,const void* buffer,DWORD requested,DWORD& written)->BOOL {
            return WriteFile(file,buffer,requested,&written,nullptr);
        };
        flushFile=[](HANDLE file)->BOOL { return FlushFileBuffers(file); };
        closeHandle=[](HANDLE handle)->BOOL { return CloseHandle(handle); };
        replaceFile=[](const std::wstring& replaced,const std::wstring& replacement,
                const std::wstring& backup,DWORD flags)->BOOL {
            return ReplaceFileW(replaced.c_str(),replacement.c_str(),backup.c_str(),flags,nullptr,nullptr);
        };
        moveFile=[](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL {
            return MoveFileExW(from.c_str(),to.c_str(),flags);
        };
        copyFile=[](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL {
            return CopyFileW(from.c_str(),to.c_str(),failIfExists);
        };
        deleteFile=[](const std::wstring& path)->BOOL { return DeleteFileW(path.c_str()); };
        getAttributes=[](const std::wstring& path)->DWORD { return GetFileAttributesW(path.c_str()); };
        getSize=[](HANDLE file,unsigned long long& size)->BOOL {
            LARGE_INTEGER value{};
            if(!GetFileSizeEx(file,&value)) return FALSE;
            if(value.QuadPart<0){ SetLastError(ERROR_FILE_INVALID); return FALSE; }
            size=(unsigned long long)value.QuadPart;
            return TRUE;
        };
        getMtime=[](HANDLE file,unsigned long long& mtime)->BOOL {
            FILETIME value{};
            if(!GetFileTime(file,nullptr,nullptr,&value)) return FALSE;
            ULARGE_INTEGER combined{};
            combined.LowPart=value.dwLowDateTime;
            combined.HighPart=value.dwHighDateTime;
            mtime=combined.QuadPart;
            return TRUE;
        };
    }
};

enum class FileReadStatus { Ok, Missing, Unavailable, TooLarge };

struct FileReadResult {
    FileReadStatus status=FileReadStatus::Unavailable;
    std::string bytes;
    DWORD win32Error=ERROR_SUCCESS;
    std::string error;
};

struct FileReadMetadata {
    unsigned long long size=0;
    unsigned long long mtime=0;
};

namespace str_util_detail {

inline bool IsMissingFileError(DWORD error){
    return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
}

inline DWORD UsefulWin32Error(DWORD fallback){
    DWORD error=GetLastError();
    return error==ERROR_SUCCESS ? fallback : error;
}

inline FileReadResult ReadFailure(FileReadStatus status,DWORD error,const std::string& operation){
    FileReadResult result;
    result.status=status;
    result.win32Error=error;
    result.error=operation+" (Win32 error "+std::to_string((unsigned long long)error)+")";
    return result;
}

inline FileReadResult CloseAfterFailure(HANDLE file,const LayoutFsOps& ops,FileReadResult result){
    if(file!=INVALID_HANDLE_VALUE) ops.closeHandle(file);
    result.bytes.clear();
    return result;
}

} // namespace str_util_detail

inline FileReadResult ReadFileBytesBoundedWithMetadata(const std::wstring& path,
        unsigned long long limit,const LayoutFsOps& ops,FileReadMetadata* metadataOut){
    using namespace str_util_detail;
    DWORD attributes=ops.getAttributes(path);
    if(attributes==INVALID_FILE_ATTRIBUTES){
        DWORD error=UsefulWin32Error(ERROR_GEN_FAILURE);
        return ReadFailure(IsMissingFileError(error) ? FileReadStatus::Missing : FileReadStatus::Unavailable,
            error,"GetFileAttributesW failed");
    }
    if(attributes&FILE_ATTRIBUTE_DIRECTORY)
        return ReadFailure(FileReadStatus::Unavailable,ERROR_DIRECTORY,"path names a directory");

    // Deny in-place writers while the snapshot is read. Delete sharing remains
    // safe: a rename keeps this handle bound to one immutable file object.
    HANDLE file=ops.openFile(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN);
    if(file==INVALID_HANDLE_VALUE){
        DWORD error=UsefulWin32Error(ERROR_OPEN_FAILED);
        return ReadFailure(IsMissingFileError(error) ? FileReadStatus::Missing : FileReadStatus::Unavailable,
            error,"CreateFileW failed");
    }

    unsigned long long initialSize=0, initialMtime=0;
    if(!ops.getSize(file,initialSize)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"GetFileSizeEx failed"));
    }
    if(!ops.getMtime(file,initialMtime)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"GetFileTime failed"));
    }
    if(initialSize>limit){
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::TooLarge,ERROR_FILE_TOO_LARGE,"file exceeds read limit"));
    }
    if(initialSize>(unsigned long long)(std::numeric_limits<size_t>::max)()){
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::TooLarge,ERROR_FILE_TOO_LARGE,"file exceeds addressable size"));
    }

    std::string bytes;
    try {
        if((size_t)initialSize>bytes.max_size())
            return CloseAfterFailure(file,ops,
                ReadFailure(FileReadStatus::TooLarge,ERROR_FILE_TOO_LARGE,"file exceeds string capacity"));
        bytes.reserve((size_t)initialSize);
    } catch(const std::bad_alloc&) {
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::Unavailable,ERROR_NOT_ENOUGH_MEMORY,"file buffer allocation failed"));
    } catch(const std::length_error&) {
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::TooLarge,ERROR_FILE_TOO_LARGE,"file exceeds string capacity"));
    }

    char buffer[64*1024];
    while((unsigned long long)bytes.size()<initialSize){
        unsigned long long remaining=initialSize-(unsigned long long)bytes.size();
        DWORD requested=(DWORD)(std::min)(remaining,(unsigned long long)sizeof(buffer));
        DWORD read=0;
        if(!ops.readFile(file,buffer,requested,read)){
            DWORD error=UsefulWin32Error(ERROR_READ_FAULT);
            return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"ReadFile failed"));
        }
        if(read==0){
            return CloseAfterFailure(file,ops,
                ReadFailure(FileReadStatus::Unavailable,ERROR_HANDLE_EOF,"file shrank during read"));
        }
        if(read>requested || (unsigned long long)bytes.size()>limit-read){
            FileReadStatus status=(unsigned long long)bytes.size()+read>limit ?
                FileReadStatus::TooLarge : FileReadStatus::Unavailable;
            return CloseAfterFailure(file,ops,
                ReadFailure(status,status==FileReadStatus::TooLarge ? ERROR_FILE_TOO_LARGE : ERROR_READ_FAULT,
                    "invalid ReadFile byte count"));
        }
        try {
            bytes.append(buffer,read);
        } catch(const std::bad_alloc&) {
            return CloseAfterFailure(file,ops,
                ReadFailure(FileReadStatus::Unavailable,ERROR_NOT_ENOUGH_MEMORY,"file buffer allocation failed"));
        } catch(const std::length_error&) {
            return CloseAfterFailure(file,ops,
                ReadFailure(FileReadStatus::TooLarge,ERROR_FILE_TOO_LARGE,"file exceeds string capacity"));
        }
    }

    auto changedFailure=[&](unsigned long long observed,const char* operation)->FileReadResult {
        // Contract choice: an observed size above the caller's bound is
        // TooLarge; every other post-open size change is Unavailable. Neither
        // case can publish the bytes accumulated so far.
        FileReadStatus status=observed>limit ? FileReadStatus::TooLarge : FileReadStatus::Unavailable;
        return ReadFailure(status,status==FileReadStatus::TooLarge ? ERROR_FILE_TOO_LARGE : ERROR_FILE_INVALID,
            operation);
    };
    unsigned long long checkedSize=0, checkedMtime=0;
    if(!ops.getSize(file,checkedSize)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"post-read size query failed"));
    }
    if(checkedSize!=initialSize)
        return CloseAfterFailure(file,ops,changedFailure(checkedSize,"file size changed during read"));
    if(!ops.getMtime(file,checkedMtime)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"post-read time query failed"));
    }
    if(checkedMtime!=initialMtime)
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::Unavailable,ERROR_FILE_INVALID,"file timestamp changed during read"));

    char extra=0;
    DWORD extraRead=0;
    if(!ops.readFile(file,&extra,1,extraRead)){
        DWORD error=UsefulWin32Error(ERROR_READ_FAULT);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"EOF verification failed"));
    }
    if(extraRead!=0){
        FileReadStatus status=initialSize>=limit ? FileReadStatus::TooLarge : FileReadStatus::Unavailable;
        return CloseAfterFailure(file,ops,
            ReadFailure(status,status==FileReadStatus::TooLarge ? ERROR_FILE_TOO_LARGE : ERROR_FILE_INVALID,
                "file grew during EOF verification"));
    }

    if(!ops.getSize(file,checkedSize)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"final size query failed"));
    }
    if(checkedSize!=initialSize)
        return CloseAfterFailure(file,ops,changedFailure(checkedSize,"file size changed after EOF verification"));
    if(!ops.getMtime(file,checkedMtime)){
        DWORD error=UsefulWin32Error(ERROR_FILE_INVALID);
        return CloseAfterFailure(file,ops,ReadFailure(FileReadStatus::Unavailable,error,"final time query failed"));
    }
    if(checkedMtime!=initialMtime)
        return CloseAfterFailure(file,ops,
            ReadFailure(FileReadStatus::Unavailable,ERROR_FILE_INVALID,"file timestamp changed after EOF verification"));
    if(!ops.closeHandle(file)){
        DWORD error=UsefulWin32Error(ERROR_INVALID_HANDLE);
        return ReadFailure(FileReadStatus::Unavailable,error,"CloseHandle failed");
    }

    FileReadResult result;
    result.status=FileReadStatus::Ok;
    result.bytes.swap(bytes);
    result.win32Error=ERROR_SUCCESS;
    if(metadataOut){ metadataOut->size=initialSize; metadataOut->mtime=initialMtime; }
    return result;
}

inline FileReadResult ReadFileBytesBounded(const std::wstring& path,unsigned long long limit,
        const LayoutFsOps& ops){
    return ReadFileBytesBoundedWithMetadata(path,limit,ops,nullptr);
}

inline FileReadResult ReadFileBytesBounded(const std::wstring& path,unsigned long long limit){
    LayoutFsOps ops;
    return ReadFileBytesBoundedWithMetadata(path,limit,ops,nullptr);
}

inline bool ReadFileBytes(const std::wstring& path,std::string& out){
    FileReadResult read=ReadFileBytesBounded(path,(std::numeric_limits<unsigned long long>::max)());
    if(read.status!=FileReadStatus::Ok) return false;
    out.swap(read.bytes);
    return true;
}
inline bool FileExists(const std::wstring& p){ DWORD a=GetFileAttributesW(p.c_str()); return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY); }
