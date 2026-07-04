// vde.cpp — Virtual Desktops Extention for Windows 11
// -----------------------------------------------------------------------------
// Единое приложение:
//   * Резидент в трее + глобальный хоткей Ctrl+Alt+D -> пикер «перенести
//     активное окно на выбранный десктоп».
//   * Сохранение/восстановление раскладки окон Firefox по виртуальным десктопам
//     (домен-фингерпринт из sessionstore + матчинг). Доступно из меню в трее и
//     из командной строки:  vde save | restore | status | list
//
// Перенос — проверенный ImmersiveShell::MoveViewToDesktop; чтение десктопа —
// документированный GetWindowDesktopId. IID — 24H2/25H2 (совместимо с 22631).
//
// Сборка (x64 Native Tools for VS 2017). Флаг /utf-8 — чтобы русские КОММЕНТАРИИ
// не давали предупреждений C4819 (строки UI — английские, на сборку не влияют):
//   cl /utf-8 /EHsc /W3 /std:c++14 vde.cpp /Fe:vde.exe
//
// Иконка: положите файл vde.ico рядом с vde.exe (см. примечание о размерах).
// GUI-часть на реальной машине здесь не прогонялась; COM-логика проверена.
// -----------------------------------------------------------------------------

#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <objectarray.h>
#include <servprov.h>
#include <objbase.h>
#include <stdio.h>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

// =============================== app config ==================================
static const wchar_t* APP_NAME  = L"Virtual Desktops Extention for Windows 11";
static const wchar_t* APP_SHORT = L"Virtual Desktops Extention"; // <=63 chars for balloon title
// Если 'Extention' — опечатка и нужно 'Extension', поменяйте обе строки выше.

static UINT g_hotMods = MOD_CONTROL | MOD_ALT;  // Ctrl+Alt+D по умолчанию
static UINT g_hotVk   = 'D';
static bool g_autoFix = false;                  // авто-фиксация раскладки Firefox
static const bool SWITCH_AFTER_MOVE = false;    // переключаться на десктоп после переноса окна

#define IDI_APPICON 101    // должен совпадать с ID в vde.rc
#define TIMER_AUTOFIX 1
#define AUTOFIX_INTERVAL_MS 10000
#define IDC_HOTKEY 1001
#define IDC_AUTOFIX 1002

// ============================ PER-BUILD IIDs (24H2/25H2) ======================
static const GUID kCLSID_ImmersiveShell =
    { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4,0xBB,0x15,0x63,0x62,0xA2,0xF2,0x39 } };
static const GUID kCLSID_VirtualDesktopManagerInternal =
    { 0xC5E0CDCA, 0x7B6E, 0x41B2, { 0x9F,0xC4,0xD9,0x39,0x75,0xCC,0x46,0x7B } };
static const GUID kIID_IVirtualDesktopManagerInternal =
    { 0x53F5CA0B, 0x158F, 0x4124, { 0x90,0x0C,0x05,0x71,0x58,0x06,0x0B,0x27 } };
static const GUID kIID_IApplicationViewCollection =
    { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF,0x41,0x87,0x47,0x53,0x8F,0x10,0xE5 } };
static const GUID kIID_IVirtualDesktop =
    { 0x3F07F4BE, 0xB107, 0x441A, { 0xAF,0x0F,0x39,0xD8,0x25,0x29,0x07,0x2C } };

// ============================ Undocumented interfaces =========================
struct __declspec(uuid("372E1D3B-38D3-42E4-A15B-8AB2B178F513"))
IApplicationView : public IUnknown {};
struct __declspec(uuid("3F07F4BE-B107-441A-AF0F-39D82529072C"))
IVirtualDesktop : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IApplicationView*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetID(GUID*) = 0;
};
struct __declspec(uuid("1841C6D7-4F9D-42C0-AF41-8747538F10E5"))
IApplicationViewCollection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IApplicationView**) = 0;
};
struct __declspec(uuid("53F5CA0B-158F-4124-900C-057158060B27"))
IVirtualDesktopManagerInternal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IApplicationView*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IApplicationView*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(IVirtualDesktop*, int, IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDesktop(IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(IVirtualDesktop*, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDesktop(GUID*, IVirtualDesktop**) = 0;
};

// ================================ string utils ================================
static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring GuidToString(const GUID& g) { wchar_t b[64]={0}; StringFromGUID2(g,b,64); return std::wstring(b); }
static bool StringToGuid(const std::wstring& s, GUID& out) { return SUCCEEDED(CLSIDFromString(s.c_str(), &out)); }
static bool GuidEq(const GUID& a, const GUID& b) { return IsEqualGUID(a, b) != 0; }
static bool GuidIsZero(const GUID& g) { GUID z = {0}; return GuidEq(g, z); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const std::string& in) {
    std::string out; int val=0, bits=-6;
    for (unsigned char c : in) { val=(val<<8)+c; bits+=8; while(bits>=0){ out.push_back(B64[(val>>bits)&0x3F]); bits-=6; } }
    if (bits>-6) out.push_back(B64[((val<<8)>>(bits+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}
static std::string b64dec(const std::string& in) {
    int T[256]; for(int i=0;i<256;i++)T[i]=-1; for(int i=0;i<64;i++)T[(unsigned char)B64[i]]=i;
    std::string out; int val=0, bits=-8;
    for (unsigned char c : in){ if(c=='='||T[c]==-1)break; val=(val<<6)+T[c]; bits+=6; if(bits>=0){out.push_back(char((val>>bits)&0xFF)); bits-=8;} }
    return out;
}
static DWORD GetWindowsBuild() {
    typedef LONG (WINAPI *P)(PRTL_OSVERSIONINFOW);
    HMODULE h=GetModuleHandleW(L"ntdll.dll");
    if(h){ P p=(P)GetProcAddress(h,"RtlGetVersion"); if(p){ RTL_OSVERSIONINFOW vi={0}; vi.dwOSVersionInfoSize=sizeof(vi); if(p(&vi)==0) return vi.dwBuildNumber; } }
    return 0;
}
static std::wstring DesktopNameFromRegistry(const GUID& g) {
    std::wstring key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops\\"+GuidToString(g);
    HKEY hk=nullptr; std::wstring res;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_READ,&hk)==ERROR_SUCCESS){
        wchar_t name[256]; DWORD cb=sizeof(name),type=0;
        if(RegQueryValueExW(hk,L"Name",nullptr,&type,(LPBYTE)name,&cb)==ERROR_SUCCESS && type==REG_SZ) res=name;
        RegCloseKey(hk);
    }
    return res;
}

// ===================== mozLz4 + JSON (проверено отдельно) =====================
static long lz4_block_decompress(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
    size_t sp=0, dp=0;
    while (sp<srcLen) {
        uint8_t token=src[sp++]; size_t litLen=token>>4;
        if(litLen==15){ uint8_t b; do{ if(sp>=srcLen)return -1; b=src[sp++]; litLen+=b; }while(b==255); }
        if(sp+litLen>srcLen||dp+litLen>dstCap)return -1;
        memcpy(dst+dp,src+sp,litLen); dp+=litLen; sp+=litLen;
        if(dp==dstCap)break; if(sp>=srcLen)break;
        if(sp+2>srcLen)return -1;
        size_t offset=(size_t)src[sp]|((size_t)src[sp+1]<<8); sp+=2;
        if(offset==0||offset>dp)return -1;
        size_t matchLen=token&0x0F;
        if(matchLen==15){ uint8_t b; do{ if(sp>=srcLen)return -1; b=src[sp++]; matchLen+=b; }while(b==255); }
        matchLen+=4;
        if(dp+matchLen>dstCap)return -1;
        size_t mp=dp-offset; for(size_t i=0;i<matchLen;++i) dst[dp+i]=dst[mp+i];
        dp+=matchLen; if(dp==dstCap)break;
    }
    return (long)dp;
}
static bool mozlz4_decompress(const uint8_t* data, size_t len, std::string& out) {
    static const uint8_t MAGIC[8]={0x6D,0x6F,0x7A,0x4C,0x7A,0x34,0x30,0x00};
    if(len<12||memcmp(data,MAGIC,8)!=0)return false;
    uint32_t usize=(uint32_t)data[8]|((uint32_t)data[9]<<8)|((uint32_t)data[10]<<16)|((uint32_t)data[11]<<24);
    out.resize(usize); if(usize==0)return true;
    long n=lz4_block_decompress(data+12,len-12,(uint8_t*)&out[0],usize);
    return n==(long)usize;
}
struct JValue {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t=NUL;
    bool b=false; double num=0; std::string str;
    std::vector<JValue> arr; std::map<std::string,JValue> obj;
    const JValue* find(const std::string& k) const { if(t!=OBJ)return nullptr; auto it=obj.find(k); return it==obj.end()?nullptr:&it->second; }
    int asInt(int def=0) const { return t==NUM?(int)num:def; }
    const std::string& asStr() const { static std::string e; return t==STR?str:e; }
};
struct JParser {
    const char* p; const char* end; bool ok=true;
    JParser(const std::string& s):p(s.data()),end(s.data()+s.size()){}
    void ws(){ while(p<end&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'))++p; }
    bool parse(JValue& v){ ws(); bool r=value(v); return r&&ok; }
    void appendUtf8(std::string& s, unsigned cp){
        if(cp<=0x7F)s.push_back((char)cp);
        else if(cp<=0x7FF){s.push_back((char)(0xC0|(cp>>6)));s.push_back((char)(0x80|(cp&0x3F)));}
        else if(cp<=0xFFFF){s.push_back((char)(0xE0|(cp>>12)));s.push_back((char)(0x80|((cp>>6)&0x3F)));s.push_back((char)(0x80|(cp&0x3F)));}
        else{s.push_back((char)(0xF0|(cp>>18)));s.push_back((char)(0x80|((cp>>12)&0x3F)));s.push_back((char)(0x80|((cp>>6)&0x3F)));s.push_back((char)(0x80|(cp&0x3F)));}
    }
    unsigned hex4(){ unsigned v=0; for(int i=0;i<4&&p<end;++i){char c=*p++;v<<=4; if(c>='0'&&c<='9')v|=c-'0'; else if(c>='a'&&c<='f')v|=c-'a'+10; else if(c>='A'&&c<='F')v|=c-'A'+10; else{ok=false;return 0;}} return v; }
    bool str(std::string& out){
        if(p>=end||*p!='"'){ok=false;return false;} ++p;
        while(p<end){ char c=*p++;
            if(c=='"')return true;
            if(c=='\\'){ if(p>=end){ok=false;return false;} char e=*p++;
                switch(e){
                    case '"':out.push_back('"');break; case '\\':out.push_back('\\');break; case '/':out.push_back('/');break;
                    case 'b':out.push_back('\b');break; case 'f':out.push_back('\f');break; case 'n':out.push_back('\n');break;
                    case 'r':out.push_back('\r');break; case 't':out.push_back('\t');break;
                    case 'u':{ unsigned cp=hex4();
                        if(cp>=0xD800&&cp<=0xDBFF){ if(p+1<end&&p[0]=='\\'&&p[1]=='u'){p+=2; unsigned lo=hex4(); if(lo>=0xDC00&&lo<=0xDFFF) cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00);} }
                        appendUtf8(out,cp); break; }
                    default: ok=false; return false;
                }
            } else out.push_back(c);
        }
        ok=false; return false;
    }
    bool value(JValue& v){ ws(); if(p>=end){ok=false;return false;} char c=*p;
        if(c=='"'){v.t=JValue::STR;return str(v.str);}
        if(c=='{')return object(v);
        if(c=='[')return array(v);
        if(c=='t'){if(end-p>=4&&!memcmp(p,"true",4)){p+=4;v.t=JValue::BOOL;v.b=true;return true;}ok=false;return false;}
        if(c=='f'){if(end-p>=5&&!memcmp(p,"false",5)){p+=5;v.t=JValue::BOOL;v.b=false;return true;}ok=false;return false;}
        if(c=='n'){if(end-p>=4&&!memcmp(p,"null",4)){p+=4;v.t=JValue::NUL;return true;}ok=false;return false;}
        const char* s=p; if(c=='-')++p;
        while(p<end&&((*p>='0'&&*p<='9')||*p=='.'||*p=='e'||*p=='E'||*p=='+'||*p=='-'))++p;
        if(p==s){ok=false;return false;}
        v.t=JValue::NUM; v.num=strtod(std::string(s,p-s).c_str(),nullptr); return true;
    }
    bool object(JValue& v){ v.t=JValue::OBJ; ++p; ws(); if(p<end&&*p=='}'){++p;return true;}
        while(p<end){ ws(); std::string k; if(!str(k))return false; ws(); if(p>=end||*p!=':'){ok=false;return false;} ++p;
            JValue cv; if(!value(cv))return false; v.obj[k]=std::move(cv); ws();
            if(p<end&&*p==','){++p;continue;} if(p<end&&*p=='}'){++p;return true;} ok=false;return false; }
        ok=false; return false; }
    bool array(JValue& v){ v.t=JValue::ARR; ++p; ws(); if(p<end&&*p==']'){++p;return true;}
        while(p<end){ JValue cv; if(!value(cv))return false; v.arr.push_back(std::move(cv)); ws();
            if(p<end&&*p==','){++p;continue;} if(p<end&&*p==']'){++p;return true;} ok=false;return false; }
        ok=false; return false; }
};
static std::string hostOf(const std::string& url){
    size_t s=url.find("://"); if(s==std::string::npos)return ""; s+=3;
    size_t e=url.find_first_of("/:?#",s);
    std::string h=url.substr(s,(e==std::string::npos?url.size():e)-s);
    if(h.size()>4&&h.compare(0,4,"www.")==0)h=h.substr(4);
    for(auto&c:h) if(c>='A'&&c<='Z')c+=32;
    return h;
}
static std::string etld1(const std::string& host){
    if(host.empty())return "";
    std::vector<std::string> parts; size_t p=0;
    while(true){ size_t d=host.find('.',p); parts.push_back(host.substr(p,(d==std::string::npos?host.size():d)-p)); if(d==std::string::npos)break; p=d+1; }
    if(parts.size()<=2)return host;
    static const std::set<std::string> two={"co","com","org","net","gov","edu","ac"};
    const std::string& last=parts[parts.size()-1]; const std::string& penult=parts[parts.size()-2];
    if(last.size()==2&&two.count(penult)) return parts[parts.size()-3]+"."+penult+"."+last;
    return penult+"."+last;
}
struct SSWindow { std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount=0; };
static std::vector<SSWindow> extractWindows(const JValue& root){
    std::vector<SSWindow> out;
    const JValue* wins=root.find("windows"); if(!wins||wins->t!=JValue::ARR)return out;
    for(const auto& w: wins->arr){
        const JValue* tabs=w.find("tabs"); if(!tabs||tabs->t!=JValue::ARR)continue;
        int selected=1; if(const JValue* s=w.find("selected"))selected=s->asInt(1);
        SSWindow sw; sw.tabCount=(int)tabs->arr.size();
        for(size_t ti=0; ti<tabs->arr.size(); ++ti){
            const JValue& tab=tabs->arr[ti];
            const JValue* entries=tab.find("entries"); if(!entries||entries->t!=JValue::ARR||entries->arr.empty())continue;
            int idx=(int)entries->arr.size(); if(const JValue* e=tab.find("index"))idx=e->asInt(idx);
            if(idx<1)idx=1; if(idx>(int)entries->arr.size())idx=(int)entries->arr.size();
            const JValue& cur=entries->arr[idx-1];
            std::string url=cur.find("url")?cur.find("url")->asStr():"";
            std::string dom=etld1(hostOf(url)); if(!dom.empty())sw.counts[dom]++;
            if((int)ti==selected-1){ sw.activeTitle=cur.find("title")?cur.find("title")->asStr():""; sw.activeDomain=dom; }
        }
        out.push_back(std::move(sw));
    }
    return out;
}

// ============================== services ======================================
static IServiceProvider*               g_shell  = nullptr;
static IVirtualDesktopManagerInternal* g_vdmi   = nullptr;
static IApplicationViewCollection*     g_avc    = nullptr;
static IVirtualDesktopManager*         g_vdmDoc = nullptr;
static bool InitServices(){
    if(FAILED(CoCreateInstance(kCLSID_ImmersiveShell,nullptr,CLSCTX_LOCAL_SERVER,__uuidof(IServiceProvider),(void**)&g_shell)))return false;
    if(FAILED(g_shell->QueryService(kCLSID_VirtualDesktopManagerInternal,kIID_IVirtualDesktopManagerInternal,(void**)&g_vdmi)))return false;
    if(FAILED(g_shell->QueryService(kIID_IApplicationViewCollection,kIID_IApplicationViewCollection,(void**)&g_avc)))return false;
    CoCreateInstance(CLSID_VirtualDesktopManager,nullptr,CLSCTX_INPROC_SERVER,IID_IVirtualDesktopManager,(void**)&g_vdmDoc);
    return true;
}
static void ReleaseServices(){ if(g_vdmDoc)g_vdmDoc->Release(); if(g_avc)g_avc->Release(); if(g_vdmi)g_vdmi->Release(); if(g_shell)g_shell->Release(); }
static IVirtualDesktop* GetDesktopByIndex(UINT index){ IObjectArray* a=nullptr; if(FAILED(g_vdmi->GetDesktops(&a))||!a)return nullptr; IVirtualDesktop* d=nullptr; a->GetAt(index,kIID_IVirtualDesktop,(void**)&d); a->Release(); return d; }
static IVirtualDesktop* GetDesktopByGuid(const GUID& t){ IObjectArray* a=nullptr; if(FAILED(g_vdmi->GetDesktops(&a))||!a)return nullptr; UINT n=0;a->GetCount(&n); IVirtualDesktop* r=nullptr;
    for(UINT i=0;i<n;++i){ IVirtualDesktop* d=nullptr; if(SUCCEEDED(a->GetAt(i,kIID_IVirtualDesktop,(void**)&d))&&d){ GUID g={0};d->GetID(&g); if(IsEqualGUID(g,t)){r=d;break;} d->Release(); } } a->Release(); return r; }
static int GetDesktopIndexByGuid(const GUID& t){ IObjectArray* a=nullptr; if(FAILED(g_vdmi->GetDesktops(&a))||!a)return -1; UINT n=0;a->GetCount(&n); int idx=-1;
    for(UINT i=0;i<n;++i){ IVirtualDesktop* d=nullptr; if(SUCCEEDED(a->GetAt(i,kIID_IVirtualDesktop,(void**)&d))&&d){ GUID g={0};d->GetID(&g); if(IsEqualGUID(g,t)){idx=(int)i;d->Release();break;} d->Release(); } } a->Release(); return idx; }
static bool VerifyOnDesktop(HWND hwnd, const GUID& dest, int tries=4){
    if(!g_vdmDoc)return true;
    for(int i=0;i<tries;++i){ GUID cur={0}; if(SUCCEEDED(g_vdmDoc->GetWindowDesktopId(hwnd,&cur))&&GuidEq(cur,dest))return true; Sleep(150); }
    return false;
}
static bool MoveWindowToDesktop(HWND hwnd, IVirtualDesktop* pDest, const GUID& destGuid){
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    if(pid==GetCurrentProcessId()&&g_vdmDoc){ if(SUCCEEDED(g_vdmDoc->MoveWindowToDesktop(hwnd,destGuid)))return VerifyOnDesktop(hwnd,destGuid); }
    IApplicationView* view=nullptr; if(FAILED(g_avc->GetViewForHwnd(hwnd,&view))||!view)return false;
    HRESULT hr=g_vdmi->MoveViewToDesktop(view,pDest); view->Release();
    if(FAILED(hr))return false;
    return VerifyOnDesktop(hwnd,destGuid);
}

// ============================ Firefox windows =================================
static std::wstring ProcessImageName(DWORD pid){ HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if(!h)return L""; wchar_t b[MAX_PATH]; DWORD sz=MAX_PATH; std::wstring r; if(QueryFullProcessImageNameW(h,0,b,&sz))r.assign(b,sz); CloseHandle(h); return r; }
static bool EndsWithI(const std::wstring& s, const std::wstring& suf){ if(s.size()<suf.size())return false; return CompareStringOrdinal(s.c_str()+(s.size()-suf.size()),(int)suf.size(),suf.c_str(),(int)suf.size(),TRUE)==CSTR_EQUAL; }
static std::wstring StripFirefoxSuffix(std::wstring t){
    static const wchar_t* sfx[]={L" \x2014 Mozilla Firefox (Private Browsing)",L" - Mozilla Firefox (Private Browsing)",L" \x2014 Mozilla Firefox",L" - Mozilla Firefox"};
    for(auto s:sfx){ size_t l=wcslen(s); if(t.size()>=l&&_wcsicmp(t.c_str()+(t.size()-l),s)==0){t.resize(t.size()-l);return t;} }
    if(_wcsicmp(t.c_str(),L"Mozilla Firefox")==0)return L"";
    return t;
}
struct LiveWin { HWND hwnd; std::wstring rawTitle; GUID desktop; };
static BOOL CALLBACK EnumFF(HWND hwnd, LPARAM lp){
    auto* out=(std::vector<LiveWin>*)lp; wchar_t cls[64]={0};
    if(GetClassNameW(hwnd,cls,64)<=0)return TRUE;
    if(wcscmp(cls,L"MozillaWindowClass")!=0)return TRUE;
    if(!(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE))return TRUE;
    int len=GetWindowTextLengthW(hwnd); if(len<=0)return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    std::wstring img=ProcessImageName(pid); if(!img.empty()&&!EndsWithI(img,L"firefox.exe"))return TRUE;
    std::wstring title(len+1,0); GetWindowTextW(hwnd,&title[0],len+1); title.resize(wcslen(title.c_str()));
    LiveWin w; w.hwnd=hwnd; w.rawTitle=title; w.desktop=GUID{0}; out->push_back(w);
    return TRUE;
}
static std::vector<LiveWin> EnumFirefoxWindows(){ std::vector<LiveWin> v; EnumWindows(EnumFF,(LPARAM)&v); if(g_vdmDoc)for(auto&w:v)g_vdmDoc->GetWindowDesktopId(w.hwnd,&w.desktop); return v; }

// ============================ sessionstore IO =================================
static bool ReadFileBytes(const std::wstring& path, std::string& out){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    char buf[65536]; DWORD rd=0; out.clear();
    while(ReadFile(f,buf,sizeof(buf),&rd,nullptr)&&rd)out.append(buf,rd);
    CloseHandle(f); return true;
}
static bool FileExists(const std::wstring& p){ DWORD a=GetFileAttributesW(p.c_str()); return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring FirefoxProfileDirUncached();
static std::wstring FirefoxProfileDir(){ static std::wstring c; if(c.empty()) c=FirefoxProfileDirUncached(); return c; }
static std::wstring FirefoxProfileDirUncached(){
    wchar_t appdata[MAX_PATH]={0}; if(!GetEnvironmentVariableW(L"APPDATA",appdata,MAX_PATH))return L"";
    std::wstring base=std::wstring(appdata)+L"\\Mozilla\\Firefox";
    std::string text; if(!ReadFileBytes(base+L"\\profiles.ini",text))return L"";
    std::vector<std::pair<std::string,std::map<std::string,std::string>>> sections;
    { size_t pos=0;
      while(pos<text.size()){ size_t nl=text.find('\n',pos); std::string line=text.substr(pos,(nl==std::string::npos?text.size():nl)-pos); pos=(nl==std::string::npos?text.size():nl+1);
        while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.pop_back();
        size_t a=0; while(a<line.size()&&line[a]==' ')a++; if(a)line=line.substr(a);
        if(line.empty()||line[0]==';'||line[0]=='#')continue;
        if(line[0]=='['&&line.back()==']'){ sections.push_back({line.substr(1,line.size()-2),{}}); continue; }
        size_t eq=line.find('='); if(eq!=std::string::npos&&!sections.empty())sections.back().second[line.substr(0,eq)]=line.substr(eq+1);
      } }
    auto resolve=[&](const std::string& rel, bool relative)->std::wstring{ std::string p=rel; for(auto&c:p)if(c=='/')c='\\'; std::wstring wp=U82W(p); return relative?(base+L"\\"+wp):wp; };
    for(auto& sec:sections) if(sec.first.rfind("Install",0)==0){ auto it=sec.second.find("Default"); if(it!=sec.second.end()&&!it->second.empty())return resolve(it->second,true); }
    for(auto& sec:sections) if(sec.first.rfind("Profile",0)==0){ auto d=sec.second.find("Default"),pth=sec.second.find("Path"); if(d!=sec.second.end()&&d->second=="1"&&pth!=sec.second.end()){ bool rel=sec.second.count("IsRelative")?sec.second["IsRelative"]=="1":true; return resolve(pth->second,rel); } }
    for(auto& sec:sections) if(sec.first.rfind("Profile",0)==0){ auto pth=sec.second.find("Path"); if(pth!=sec.second.end()&&pth->second.find(".default-release")!=std::string::npos){ bool rel=sec.second.count("IsRelative")?sec.second["IsRelative"]=="1":true; return resolve(pth->second,rel); } }
    return L"";
}
static std::vector<SSWindow> ReadSessionstore(std::wstring* usedPathOut=nullptr){
    std::wstring prof=FirefoxProfileDir(); if(prof.empty())return {};
    const wchar_t* cand[]={L"\\sessionstore-backups\\recovery.jsonlz4",L"\\sessionstore-backups\\recovery.baklz4",L"\\sessionstore.jsonlz4",L"\\sessionstore-backups\\previous.jsonlz4"};
    for(auto c:cand){ std::wstring path=prof+c; if(!FileExists(path))continue; std::string bytes; if(!ReadFileBytes(path,bytes))continue;
        std::string json; if(!mozlz4_decompress((const uint8_t*)bytes.data(),bytes.size(),json))continue;
        JParser jp(json); JValue root; if(!jp.parse(root))continue;
        if(usedPathOut)*usedPathOut=path; return extractWindows(root); }
    return {};
}

// ============================ fingerprint / scoring ===========================
struct Fp {
    HWND hwnd=nullptr; GUID desktop={0}; int deskIndex=-1;
    std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount=0;
    bool hasDomains() const { return !counts.empty(); }
};
static std::vector<Fp> BuildLiveFingerprints(int* boundCount=nullptr){
    std::vector<LiveWin> live=EnumFirefoxWindows();
    std::vector<SSWindow> ss=ReadSessionstore();
    std::vector<bool> used(ss.size(),false); int bound=0; std::vector<Fp> out;
    for(auto& w:live){
        std::string sTitle=W2U8(StripFirefoxSuffix(w.rawTitle));
        Fp fp; fp.hwnd=w.hwnd; fp.desktop=w.desktop; fp.activeTitle=sTitle;
        int bi=-1; for(size_t i=0;i<ss.size();++i) if(!used[i]&&ss[i].activeTitle==sTitle){bi=(int)i;break;}
        if(bi>=0){ used[bi]=true; bound++; fp.counts=ss[bi].counts; fp.activeDomain=ss[bi].activeDomain; fp.tabCount=ss[bi].tabCount; if(!ss[bi].activeTitle.empty())fp.activeTitle=ss[bi].activeTitle; }
        out.push_back(std::move(fp));
    }
    if(boundCount)*boundCount=bound;
    return out;
}
static double Score(const Fp& s, const Fp& l){
    if(s.hasDomains()&&l.hasDomains()){
        double dot=0,na=0,nb=0;
        for(auto& kv:s.counts){ na+=double(kv.second)*kv.second; auto it=l.counts.find(kv.first); if(it!=l.counts.end())dot+=double(kv.second)*it->second; }
        for(auto& kv:l.counts) nb+=double(kv.second)*kv.second;
        double C=(na&&nb)?dot/(std::sqrt(na)*std::sqrt(nb)):0;
        int inter=0; std::set<std::string> uni; for(auto& kv:s.counts)uni.insert(kv.first);
        for(auto& kv:l.counts){ if(s.counts.count(kv.first))inter++; uni.insert(kv.first); }
        double J=uni.empty()?0:double(inter)/uni.size();
        double act=(s.activeTitle==l.activeTitle&&!s.activeTitle.empty())?1.0:((!s.activeDomain.empty()&&s.activeDomain==l.activeDomain)?0.5:0.0);
        int mx=std::max(std::max(s.tabCount,l.tabCount),1);
        double tab=1.0-std::min(1.0,std::abs(s.tabCount-l.tabCount)/double(mx));
        return 0.40*C+0.25*J+0.20*act+0.15*tab;
    }
    return (!s.activeTitle.empty()&&s.activeTitle==l.activeTitle)?1.0:0.0;
}

// ============================ snapshot storage ================================
static std::wstring SnapshotPath(){
    wchar_t base[MAX_PATH]={0}; GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    std::wstring dir=std::wstring(base)+L"\\VirtualDesktopsExtention"; CreateDirectoryW(dir.c_str(),nullptr);
    return dir+L"\\layout.txt";
}
static std::string CountsToStr(const std::map<std::string,int>& c){ std::string s; bool f=true; for(auto& kv:c){ if(!f)s+=","; f=false; s+=kv.first+":"+std::to_string(kv.second);} return s; }
static std::map<std::string,int> StrToCounts(const std::string& s){ std::map<std::string,int> c; size_t p=0;
    while(p<s.size()){ size_t comma=s.find(',',p); std::string item=s.substr(p,(comma==std::string::npos?s.size():comma)-p); p=(comma==std::string::npos?s.size():comma+1);
        size_t col=item.rfind(':'); if(col!=std::string::npos)c[item.substr(0,col)]=atoi(item.substr(col+1).c_str()); } return c; }
struct DeskRec { int index; GUID guid; std::wstring name; };
static bool WriteSnapshot(const std::vector<DeskRec>& desks, const std::vector<Fp>& wins){
    HANDLE f=CreateFileW(SnapshotPath().c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    std::string out="# VDE snapshot v2\n";
    for(auto& d:desks){ char l[128]; sprintf_s(l,"D\t%d\t",d.index); out+=l; out+=W2U8(GuidToString(d.guid)); out+="\t"; out+=b64enc(W2U8(d.name)); out+="\n"; }
    for(auto& w:wins){ char l[128]; sprintf_s(l,"W\t%d\t",w.deskIndex); out+=l; out+=W2U8(GuidToString(w.desktop)); out+="\t"; out+=b64enc(w.activeTitle); out+="\t"; out+=w.activeDomain; out+="\t"; out+=std::to_string(w.tabCount); out+="\t"; out+=CountsToStr(w.counts); out+="\n"; }
    DWORD wr=0; BOOL ok=WriteFile(f,out.data(),(DWORD)out.size(),&wr,nullptr); CloseHandle(f); return ok&&wr==out.size();
}
static bool ReadSnapshot(std::vector<DeskRec>& desks, std::vector<Fp>& wins){
    std::string data; if(!ReadFileBytes(SnapshotPath(),data))return false; size_t pos=0;
    while(pos<data.size()){ size_t nl=data.find('\n',pos); std::string line=data.substr(pos,(nl==std::string::npos?data.size():nl)-pos); pos=(nl==std::string::npos?data.size():nl+1);
        if(!line.empty()&&line.back()=='\r')line.pop_back(); if(line.empty()||line[0]=='#')continue;
        std::vector<std::string> col; size_t p=0; while(true){ size_t t=line.find('\t',p); col.push_back(line.substr(p,(t==std::string::npos?line.size():t)-p)); if(t==std::string::npos)break; p=t+1; }
        if(col.size()<4)continue;
        if(col[0]=="D"){ DeskRec d; d.index=atoi(col[1].c_str()); StringToGuid(U82W(col[2]),d.guid); d.name=U82W(b64dec(col[3])); desks.push_back(d); }
        else if(col[0]=="W"){ Fp w; w.deskIndex=atoi(col[1].c_str()); StringToGuid(U82W(col[2]),w.desktop); w.activeTitle=b64dec(col[3]);
            if(col.size()>=7){ w.activeDomain=col[4]; w.tabCount=atoi(col[5].c_str()); w.counts=StrToCounts(col[6]); } wins.push_back(w); }
    }
    return true;
}

// ============================ save / restore cores ============================
// Возвращают краткую сводку (UTF-8) для трей-балуна; CLI печатает её и детали.
static std::string RunSave(){
    auto fps=BuildLiveFingerprints();
    if(fps.empty())return "No Firefox windows found. Nothing to save.";
    std::vector<DeskRec> desks; UINT count=0; g_vdmi->GetCount(&count);
    for(UINT i=0;i<count;++i){ IVirtualDesktop* d=GetDesktopByIndex(i); if(!d)continue; GUID g={0};d->GetID(&g); DeskRec dr; dr.index=(int)i;dr.guid=g;dr.name=DesktopNameFromRegistry(g); desks.push_back(dr); d->Release(); }
    std::vector<Fp> recs; int withDom=0;
    for(auto& f:fps){ if(GuidIsZero(f.desktop))continue; Fp r=f; r.deskIndex=GetDesktopIndexByGuid(f.desktop); if(r.hasDomains())withDom++; recs.push_back(r); }
    if(!WriteSnapshot(desks,recs))return "Failed to write snapshot.";
    char b[160]; sprintf_s(b,"Saved %d window(s) (%d with domain data) across %u desktop(s).",(int)recs.size(),withDom,count);
    return b;
}
static std::string RunRestore(std::vector<std::string>* linesOut=nullptr){
    std::vector<DeskRec> savedDesks; std::vector<Fp> saved;
    if(!ReadSnapshot(savedDesks,saved)||saved.empty())return "No usable snapshot. Run 'save' first.";
    auto live=BuildLiveFingerprints();
    if(live.empty())return "No Firefox windows to restore.";
    UINT count=0; g_vdmi->GetCount(&count);
    const double T_FLOOR=0.35,T_ACCEPT=0.55;
    struct Pair{ double sc; int si,li; }; std::vector<Pair> pairs;
    for(int i=0;i<(int)saved.size();++i)for(int j=0;j<(int)live.size();++j){ double sc=Score(saved[i],live[j]); if(sc>=T_FLOOR)pairs.push_back({sc,i,j}); }
    std::sort(pairs.begin(),pairs.end(),[](const Pair&a,const Pair&b){return a.sc>b.sc;});
    std::vector<int> usedS(saved.size(),0),usedL(live.size(),0),assignL2S(live.size(),-1); int matched=0;
    for(auto& p:pairs) if(!usedS[p.si]&&!usedL[p.li]&&p.sc>=T_ACCEPT){ usedS[p.si]=usedL[p.li]=1; assignL2S[p.li]=p.si; matched++; }
    int moved=0,failed=0;
    for(int li=0;li<(int)live.size();++li){ Fp& L=live[li];
        if(assignL2S[li]<0){ if(linesOut)linesOut->push_back("[no match] "+L.activeTitle); continue; }
        const Fp& S=saved[assignL2S[li]];
        IVirtualDesktop* dest=nullptr; GUID destGuid={0};
        if(GetDesktopIndexByGuid(S.desktop)>=0){ dest=GetDesktopByGuid(S.desktop); destGuid=S.desktop; }
        else if(S.deskIndex>=0&&(UINT)S.deskIndex<count){ dest=GetDesktopByIndex((UINT)S.deskIndex); if(dest)dest->GetID(&destGuid); }
        if(!dest){ if(linesOut)linesOut->push_back("[no target] "+L.activeTitle); failed++; continue; }
        if(!GuidIsZero(L.desktop)&&GuidEq(L.desktop,destGuid)){ if(linesOut)linesOut->push_back("[already there] "+L.activeTitle); dest->Release(); moved++; continue; }
        bool ok=MoveWindowToDesktop(L.hwnd,dest,destGuid); dest->Release();
        if(ok){ moved++; if(linesOut)linesOut->push_back("[moved] "+L.activeTitle); }
        else  { failed++; if(linesOut)linesOut->push_back("[FAILED] "+L.activeTitle); }
    }
    char b[160]; sprintf_s(b,"Restore: matched %d/%d, moved %d, failed %d.",matched,(int)live.size(),moved,failed);
    return b;
}

// ================================ CLI mode ===================================
static int CliRun(const std::wstring& cmd){
    if(cmd==L"list"||cmd==L"status"){
        UINT count=0; g_vdmi->GetCount(&count);
        printf("Virtual desktops: %u\n",count);
        for(UINT i=0;i<count;++i){ IVirtualDesktop* d=GetDesktopByIndex(i); if(!d)continue; GUID g={0};d->GetID(&g);
            std::wstring nm=DesktopNameFromRegistry(g); std::string nmU8=nm.empty()?std::string():("("+W2U8(nm)+")");
            printf("  [%u] %s  %s\n",i,W2U8(GuidToString(g)).c_str(),nmU8.c_str()); d->Release(); }
        if(cmd==L"status"){ std::wstring up; auto ss=ReadSessionstore(&up);
            if(!up.empty())printf("\nsessionstore: %s (%d windows)\n",W2U8(up).c_str(),(int)ss.size());
            else printf("\nsessionstore: NOT available (title-only mode)\n");
            int bound=0; auto fps=BuildLiveFingerprints(&bound);
            printf("Firefox live windows: %d (bound to sessionstore: %d)\n",(int)fps.size(),bound);
            for(auto& f:fps){ int idx=GuidIsZero(f.desktop)?-1:GetDesktopIndexByGuid(f.desktop);
                printf("  hwnd=0x%p desktop=[%d] tabs=%d active=\"%s\"\n",(void*)f.hwnd,idx,f.tabCount,f.activeTitle.c_str());
                printf("        domains:"); for(auto& kv:f.counts)printf(" %s:%d",kv.first.c_str(),kv.second); printf("\n"); }
        }
        return 0;
    }
    if(cmd==L"save"){ std::string s=RunSave(); printf("%s\n",s.c_str()); printf("Snapshot: %s\n",W2U8(SnapshotPath()).c_str()); return 0; }
    if(cmd==L"restore"){ std::vector<std::string> lines; std::string s=RunRestore(&lines); for(auto& l:lines)printf("  %s\n",l.c_str()); printf("\n%s\n",s.c_str()); return 0; }
    printf("Usage: vde <save|restore|status|list>\n");
    printf("  (no args) -> run resident in tray; Ctrl+Alt+D opens the desktop picker\n");
    return 2;
}

// ================================ GUI: picker ================================
struct WinItem { HWND hwnd; std::wstring title; HICON icon; };
struct Tile { GUID guid; std::wstring name; int index; std::vector<WinItem> windows; RECT rc; };
static std::vector<Tile> g_tiles;
static int  g_sel=0;
static HWND g_target=nullptr; static std::wstring g_targetTitle;
static HWND g_main=nullptr;
static HWND g_settings=nullptr;
static HINSTANCE g_inst=nullptr;
static HFONT g_uiFont=nullptr;
static ULONGLONG g_lastMtime=0;
static unsigned long long g_lastLayoutSig=0;
static const UINT WM_TRAY=WM_APP+1;
static NOTIFYICONDATAW g_nid={0};
static int g_dpi=96;
static int S(int v){ return MulDiv(v,g_dpi,96); }   // px@96dpi -> px@текущий DPI
static int TILE_W=240,TILE_H=150,PAD=16,HEADER=44;  // базовые (96 dpi); пересчёт в InitMetrics
static int g_cols=1,g_rows=1;
static void InitMetrics(){
    HDC dc=GetDC(nullptr); g_dpi=GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(nullptr,dc);
    TILE_W=S(240); TILE_H=S(150); PAD=S(16); HEADER=S(44);
}

static bool IsAltTabWindow(HWND h){ if(!IsWindowVisible(h))return false; if(GetWindowTextLengthW(h)<=0)return false; LONG_PTR ex=GetWindowLongPtrW(h,GWL_EXSTYLE); if(ex&WS_EX_TOOLWINDOW)return false; if(GetAncestor(h,GA_ROOTOWNER)!=h)return false; return true; }
static HICON WindowIcon(HWND h){ DWORD_PTR r=0; SendMessageTimeoutW(h,WM_GETICON,ICON_SMALL2,0,SMTO_ABORTIFHUNG,200,&r); HICON i=(HICON)r;
    if(!i)i=(HICON)GetClassLongPtrW(h,GCLP_HICONSM); if(!i){r=0;SendMessageTimeoutW(h,WM_GETICON,ICON_BIG,0,SMTO_ABORTIFHUNG,200,&r);i=(HICON)r;}
    if(!i)i=(HICON)GetClassLongPtrW(h,GCLP_HICON); if(!i)i=LoadIconW(nullptr,IDI_APPLICATION); return i; }
static BOOL CALLBACK EnumAll(HWND hwnd, LPARAM){
    if(!IsAltTabWindow(hwnd))return TRUE; GUID g={0};
    if(!g_vdmDoc||FAILED(g_vdmDoc->GetWindowDesktopId(hwnd,&g))||GuidIsZero(g))return TRUE;
    for(auto& t:g_tiles) if(GuidEq(t.guid,g)){ int len=GetWindowTextLengthW(hwnd); std::wstring title(len+1,0); GetWindowTextW(hwnd,&title[0],len+1); title.resize(wcslen(title.c_str()));
        WinItem wi; wi.hwnd=hwnd; wi.title=title; wi.icon=WindowIcon(hwnd); t.windows.push_back(wi); break; }
    return TRUE;
}
static void BuildModel(){
    g_tiles.clear(); UINT count=0; if(FAILED(g_vdmi->GetCount(&count)))return;
    for(UINT i=0;i<count;++i){ IVirtualDesktop* d=GetDesktopByIndex(i); if(!d)continue; GUID g={0};d->GetID(&g);d->Release();
        Tile t; t.guid=g; t.index=(int)i; t.name=DesktopNameFromRegistry(g); if(t.name.empty())t.name=L"Desktop "+std::to_wstring(i+1); g_tiles.push_back(t); }
    EnumWindows(EnumAll,0);
    g_sel=0; GUID cur={0};
    if(g_vdmDoc&&g_target&&SUCCEEDED(g_vdmDoc->GetWindowDesktopId(g_target,&cur)))
        for(size_t i=0;i<g_tiles.size();++i) if(GuidEq(g_tiles[i].guid,cur)){g_sel=(int)i;break;}
}
static void LayoutTiles(int clientW){
    int n=(int)g_tiles.size();
    g_cols=std::max(1,std::min(n,std::max(1,(clientW-PAD)/(TILE_W+PAD)))); g_cols=std::min(g_cols,5);
    g_rows=(n+g_cols-1)/g_cols;
    for(int i=0;i<n;++i){ int r=i/g_cols,c=i%g_cols; RECT rc; rc.left=PAD+c*(TILE_W+PAD); rc.top=HEADER+PAD+r*(TILE_H+PAD); rc.right=rc.left+TILE_W; rc.bottom=rc.top+TILE_H; g_tiles[i].rc=rc; }
}
static SIZE DesiredClientSize(){ int n=(int)g_tiles.size(); int cols=std::min(std::max(1,n),5); int rows=(n+cols-1)/cols; SIZE s; s.cx=PAD+cols*(TILE_W+PAD); s.cy=HEADER+PAD+rows*(TILE_H+PAD); return s; }
static void Paint(HDC hdcReal, RECT client){
    HDC hdc=CreateCompatibleDC(hdcReal); HBITMAP bmp=CreateCompatibleBitmap(hdcReal,client.right,client.bottom); HBITMAP old=(HBITMAP)SelectObject(hdc,bmp);
    HBRUSH bg=CreateSolidBrush(RGB(28,28,32)); FillRect(hdc,&client,bg); DeleteObject(bg); SetBkMode(hdc,TRANSPARENT);
    HFONT fT=CreateFontW(S(22),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HFONT fN=CreateFontW(S(18),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HFONT fI=CreateFontW(S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    SelectObject(hdc,fT); SetTextColor(hdc,RGB(235,235,235));
    RECT h=client; h.left=PAD; h.top=S(12); h.bottom=HEADER;
    bool ctrlHeld=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    std::wstring head;
    if(ctrlHeld) head=L"Move window:  "+(g_targetTitle.empty()?L"(no window)":g_targetTitle);
    else { std::wstring nm=(g_sel>=0&&g_sel<(int)g_tiles.size())?g_tiles[g_sel].name:L""; head=L"Switch to:  "+nm; }
    DrawTextW(hdc,head.c_str(),-1,&h,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_VCENTER);
    SelectObject(hdc,fI); SetTextColor(hdc,RGB(150,150,160));
    RECT hint=client; hint.left=PAD; hint.right=client.right-PAD; hint.top=S(12); hint.bottom=HEADER;
    DrawTextW(hdc,L"Click: switch    Ctrl+Click: move window",-1,&hint,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    GUID cur={0}; bool haveCur=g_vdmDoc&&g_target&&SUCCEEDED(g_vdmDoc->GetWindowDesktopId(g_target,&cur));
    for(size_t i=0;i<g_tiles.size();++i){ const Tile& t=g_tiles[i]; bool isSel=((int)i==g_sel); bool isCur=haveCur&&GuidEq(t.guid,cur);
        HBRUSH tb=CreateSolidBrush(isCur?RGB(45,48,58):RGB(38,38,44)); FillRect(hdc,&t.rc,tb); DeleteObject(tb);
        HPEN pen=CreatePen(PS_SOLID,isSel?S(3):1,isSel?RGB(0,160,255):RGB(70,70,80)); HPEN op=(HPEN)SelectObject(hdc,pen); HBRUSH ob=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
        Rectangle(hdc,t.rc.left,t.rc.top,t.rc.right,t.rc.bottom); SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(pen);
        SelectObject(hdc,fN); SetTextColor(hdc,RGB(245,245,245));
        RECT nr=t.rc; nr.left+=S(12); nr.top+=S(8); nr.right-=S(10); nr.bottom=nr.top+S(24);
        std::wstring title=std::to_wstring(t.index+1)+L". "+t.name; DrawTextW(hdc,title.c_str(),-1,&nr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,fI); SetTextColor(hdc,RGB(200,200,205));
        int y=nr.bottom+S(6),shown=0,maxShow=5;
        for(auto& w:t.windows){ if(shown>=maxShow)break; if(y+S(20)>t.rc.bottom-S(8))break;
            if(w.icon)DrawIconEx(hdc,t.rc.left+S(12),y,w.icon,S(16),S(16),0,nullptr,DI_NORMAL);
            RECT ir; ir.left=t.rc.left+S(34); ir.top=y; ir.right=t.rc.right-S(10); ir.bottom=y+S(18); DrawTextW(hdc,w.title.c_str(),-1,&ir,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            y+=S(20); shown++; }
        int extra=(int)t.windows.size()-shown;
        if(extra>0){ SetTextColor(hdc,RGB(140,140,150)); RECT er; er.left=t.rc.left+S(12); er.top=y; er.right=t.rc.right-S(10); er.bottom=y+S(18); std::wstring more=L"+"+std::to_wstring(extra)+L" more"; DrawTextW(hdc,more.c_str(),-1,&er,DT_LEFT|DT_SINGLELINE); }
    }
    BitBlt(hdcReal,0,0,client.right,client.bottom,hdc,0,0,SRCCOPY);
    DeleteObject(fT); DeleteObject(fN); DeleteObject(fI); SelectObject(hdc,old); DeleteObject(bmp); DeleteDC(hdc);
}
static void HidePicker(){ ShowWindow(g_main,SW_HIDE); }
static void Commit(int idx){ if(idx<0||idx>=(int)g_tiles.size())return; HidePicker(); if(g_target&&IsWindow(g_target)){ IVirtualDesktop* d=GetDesktopByIndex((UINT)g_tiles[idx].index); if(d){ GUID g={0};d->GetID(&g); MoveWindowToDesktop(g_target,d,g); if(SWITCH_AFTER_MOVE)g_vdmi->SwitchDesktop(d); d->Release(); } } }
static void ShowPicker(){
    g_target=GetForegroundWindow(); if(g_target==g_main)g_target=nullptr;
    if(g_target&&IsWindow(g_target)){ int len=GetWindowTextLengthW(g_target); std::wstring t(len+1,0); GetWindowTextW(g_target,&t[0],len+1); t.resize(wcslen(t.c_str())); g_targetTitle=t; } else g_targetTitle.clear();
    BuildModel(); SIZE sz=DesiredClientSize();
    HMONITOR mon=MonitorFromWindow(g_target?g_target:g_main,MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi={sizeof(mi)}; GetMonitorInfo(mon,&mi);
    RECT wr={0,0,sz.cx,sz.cy}; AdjustWindowRectEx(&wr,WS_POPUP,FALSE,WS_EX_TOOLWINDOW|WS_EX_TOPMOST);
    int ww=wr.right-wr.left,wh=wr.bottom-wr.top;
    int wx=mi.rcWork.left+((mi.rcWork.right-mi.rcWork.left)-ww)/2, wy=mi.rcWork.top+((mi.rcWork.bottom-mi.rcWork.top)-wh)/2;
    SetWindowPos(g_main,HWND_TOPMOST,wx,wy,ww,wh,SWP_NOACTIVATE);
    RECT cr; GetClientRect(g_main,&cr); LayoutTiles(cr.right);
    ShowWindow(g_main,SW_SHOW); SetForegroundWindow(g_main); InvalidateRect(g_main,nullptr,FALSE);
}
static void MoveSel(int dx,int dy){ if(g_tiles.empty())return; int r=g_sel/g_cols,c=g_sel%g_cols; c+=dx;r+=dy; int n=(int)g_tiles.size();
    if(c<0)c=0; if(c>=g_cols)c=g_cols-1; if(r<0)r=0; int idx=r*g_cols+c; if(idx>=n)idx=n-1; if(idx<0)idx=0; g_sel=idx; InvalidateRect(g_main,nullptr,FALSE); }

// ================================ GUI: tray ==================================
static HICON LoadAppIcon(int cx,int cy){
    // 1) встроенный ресурс (vde.res, см. vde.rc)
    HICON h=(HICON)LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_APPICON),IMAGE_ICON,cx,cy,LR_DEFAULTCOLOR);
    if(h)return h;
    // 2) внешний файл vde.ico рядом с exe (если ресурс не вшит)
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr,path,MAX_PATH); std::wstring p=path;
    size_t s=p.find_last_of(L"\\/"); if(s!=std::wstring::npos)p=p.substr(0,s+1); p+=L"vde.ico";
    h=(HICON)LoadImageW(nullptr,p.c_str(),IMAGE_ICON,cx,cy,LR_LOADFROMFILE);
    if(h)return h;
    // 3) системная заглушка
    return LoadIconW(nullptr,IDI_APPLICATION);
}
static void TrayAdd(HWND hwnd){
    g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=hwnd; g_nid.uID=1; g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; g_nid.uCallbackMessage=WM_TRAY;
    g_nid.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON));
    wcsncpy_s(g_nid.szTip,APP_NAME,_TRUNCATE);
    Shell_NotifyIconW(NIM_ADD,&g_nid);
}
static void TrayRemove(){ Shell_NotifyIconW(NIM_DELETE,&g_nid); }
static void Balloon(const std::wstring& text){
    g_nid.uFlags=NIF_INFO; wcsncpy_s(g_nid.szInfo,text.c_str(),_TRUNCATE); wcsncpy_s(g_nid.szInfoTitle,APP_SHORT,_TRUNCATE); g_nid.dwInfoFlags=NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY,&g_nid); g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
}

// ============================ settings / autofix =============================
static std::wstring CurrentSessionstorePath(){
    std::wstring prof=FirefoxProfileDir(); if(prof.empty())return L"";
    const wchar_t* cand[]={L"\\sessionstore-backups\\recovery.jsonlz4",L"\\sessionstore-backups\\recovery.baklz4",L"\\sessionstore.jsonlz4",L"\\sessionstore-backups\\previous.jsonlz4"};
    for(auto c:cand){ std::wstring p=prof+c; if(FileExists(p))return p; }
    return L"";
}
static ULONGLONG FileMtime(const std::wstring& p){ WIN32_FILE_ATTRIBUTE_DATA fd; if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&fd))return 0; ULARGE_INTEGER u; u.LowPart=fd.ftLastWriteTime.dwLowDateTime; u.HighPart=fd.ftLastWriteTime.dwHighDateTime; return u.QuadPart; }

// Дешёвая подпись раскладки: только окна Firefox + их десктоп (без разбора sessionstore).
// Порядко-независимая (сумма пер-оконных хэшей), чтобы смена z-порядка при фокусе не считалась изменением.
static BOOL CALLBACK EnumSig(HWND hwnd, LPARAM lp){
    unsigned long long* acc=(unsigned long long*)lp;
    wchar_t cls[64]={0};
    if(GetClassNameW(hwnd,cls,64)<=0) return TRUE;
    if(wcscmp(cls,L"MozillaWindowClass")!=0) return TRUE;
    if(!(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)) return TRUE;
    if(GetWindowTextLengthW(hwnd)<=0) return TRUE;
    GUID g={0};
    if(!g_vdmDoc||FAILED(g_vdmDoc->GetWindowDesktopId(hwnd,&g))) return TRUE;
    unsigned long long h=1469598103934665603ULL; // FNV-1a
    unsigned long long hv=(unsigned long long)(uintptr_t)hwnd;
    const unsigned char* b=(const unsigned char*)&hv; for(size_t i=0;i<sizeof(hv);++i){ h^=b[i]; h*=1099511628211ULL; }
    b=(const unsigned char*)&g; for(size_t i=0;i<sizeof(g);++i){ h^=b[i]; h*=1099511628211ULL; }
    *acc += h;
    return TRUE;
}
static unsigned long long LayoutSignature(){ unsigned long long acc=0; EnumWindows(EnumSig,(LPARAM)&acc); return acc; }

static void LoadSettings(){
    HKEY hk;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\VirtualDesktopsExtention",0,KEY_READ,&hk)==ERROR_SUCCESS){
        DWORD v=0,cb=sizeof(v);
        cb=sizeof(v); if(RegQueryValueExW(hk,L"HotkeyMods",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_hotMods=v;
        cb=sizeof(v); if(RegQueryValueExW(hk,L"HotkeyVk",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_hotVk=v;
        cb=sizeof(v); if(RegQueryValueExW(hk,L"AutoFix",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_autoFix=(v!=0);
        RegCloseKey(hk);
    }
}
static void SaveSettings(){
    HKEY hk;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\VirtualDesktopsExtention",0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)==ERROR_SUCCESS){
        DWORD v;
        v=g_hotMods;     RegSetValueExW(hk,L"HotkeyMods",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        v=g_hotVk;       RegSetValueExW(hk,L"HotkeyVk",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        v=g_autoFix?1:0; RegSetValueExW(hk,L"AutoFix",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        RegCloseKey(hk);
    }
}
static bool ApplyHotkey(){
    UnregisterHotKey(g_main,1);
    return RegisterHotKey(g_main,1,g_hotMods|MOD_NOREPEAT,g_hotVk)!=0;
}
static void ApplyAutoFix(){
    if(g_autoFix){ g_lastMtime=0; g_lastLayoutSig=0; SetTimer(g_main,TIMER_AUTOFIX,AUTOFIX_INTERVAL_MS,nullptr); }
    else KillTimer(g_main,TIMER_AUTOFIX);
}

// Переключиться на десктоп. SwitchDesktop = слот 6 vtable (совпадает на 23H2/24H2).
// Фокус-данс через Progman, как в референсе MScholtes: без него система может
// «вернуть» исходный десктоп из-за активного окна -> переключение уходило не туда.
static void GoToDesktop(int idx){
    if(idx<0||idx>=(int)g_tiles.size())return;
    HidePicker();
    IVirtualDesktop* d=GetDesktopByIndex((UINT)g_tiles[idx].index);
    if(!d)return;
    HWND prog=FindWindowW(L"Progman",L"Program Manager");
    DWORD dummy=0;
    DWORD deskTh=prog?GetWindowThreadProcessId(prog,&dummy):0;
    DWORD fgTh=GetWindowThreadProcessId(GetForegroundWindow(),&dummy);
    DWORD curTh=GetCurrentThreadId();
    if(prog&&deskTh&&fgTh&&fgTh!=curTh){
        AttachThreadInput(deskTh,curTh,TRUE);
        AttachThreadInput(fgTh,curTh,TRUE);
        SetForegroundWindow(prog);
        AttachThreadInput(fgTh,curTh,FALSE);
        AttachThreadInput(deskTh,curTh,FALSE);
    }
    g_vdmi->SwitchDesktop(d);
    if(prog) ShowWindow(prog,SW_MINIMIZE);
    d->Release();
}
// Клик = переключение на десктоп; Ctrl = перенести активное окно туда.
static void Activate(int idx, bool ctrlMove){
    if(idx<0||idx>=(int)g_tiles.size())return;
    if(ctrlMove) Commit(idx);
    else         GoToDesktop(idx);
}

// --------------------------- settings window ---------------------------------
static void SetChildFont(HWND parent){ if(!g_uiFont)return; for(HWND c=GetWindow(parent,GW_CHILD); c; c=GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,WM_SETFONT,(WPARAM)g_uiFont,TRUE); }

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        CreateWindowW(L"STATIC",L"Global hotkey:",WS_CHILD|WS_VISIBLE,S(16),S(20),S(110),S(20),hwnd,nullptr,g_inst,nullptr);
        HWND hk=CreateWindowW(L"msctls_hotkey32",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER,S(130),S(17),S(200),S(24),hwnd,(HMENU)IDC_HOTKEY,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Automatically fix Firefox window layout",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(16),S(58),S(330),S(22),hwnd,(HMENU)IDC_AUTOFIX,g_inst,nullptr);
        CreateWindowW(L"STATIC",L"Saves the layout when Firefox changes (~10 s) so Restore works after reboot.",WS_CHILD|WS_VISIBLE,S(16),S(84),S(332),S(34),hwnd,nullptr,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,S(176),S(128),S(75),S(28),hwnd,(HMENU)IDOK,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(260),S(128),S(75),S(28),hwnd,(HMENU)IDCANCEL,g_inst,nullptr);
        SetChildFont(hwnd);
        // init values
        WORD hf=0;
        if(g_hotMods&MOD_SHIFT)hf|=HOTKEYF_SHIFT;
        if(g_hotMods&MOD_CONTROL)hf|=HOTKEYF_CONTROL;
        if(g_hotMods&MOD_ALT)hf|=HOTKEYF_ALT;
        SendMessageW(hk,HKM_SETHOTKEY,MAKEWORD((BYTE)g_hotVk,(BYTE)hf),0);
        SendMessageW(GetDlgItem(hwnd,IDC_AUTOFIX),BM_SETCHECK,g_autoFix?BST_CHECKED:BST_UNCHECKED,0);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==IDOK){
            WORD r=(WORD)SendMessageW(GetDlgItem(hwnd,IDC_HOTKEY),HKM_GETHOTKEY,0,0);
            BYTE vk=LOBYTE(r), hf=HIBYTE(r);
            if(vk!=0){
                UINT mods=0;
                if(hf&HOTKEYF_SHIFT)mods|=MOD_SHIFT;
                if(hf&HOTKEYF_CONTROL)mods|=MOD_CONTROL;
                if(hf&HOTKEYF_ALT)mods|=MOD_ALT;
                g_hotVk=vk; g_hotMods=mods;
            }
            g_autoFix = (IsDlgButtonChecked(hwnd,IDC_AUTOFIX)==BST_CHECKED);
            SaveSettings();
            bool ok=ApplyHotkey();
            ApplyAutoFix();
            DestroyWindow(hwnd);
            if(!ok) MessageBoxW(nullptr,L"Could not register that hotkey (it may be in use by another app).",APP_NAME,MB_ICONWARNING);
            return 0;
        }
        if(LOWORD(wp)==IDCANCEL){ DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_CTLCOLORSTATIC:{ HDC dc=(HDC)wp; SetBkMode(dc,TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_settings=nullptr; return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void OpenSettings(){
    if(g_settings){ SetForegroundWindow(g_settings); return; }
    static bool reg=false;
    if(!reg){ WNDCLASSW wc={0}; wc.lpfnWndProc=SettingsProc; wc.hInstance=g_inst; wc.lpszClassName=L"VdeSettings";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
        RegisterClassW(&wc); reg=true; }
    int W=S(364),H=S(204);
    RECT wr={0,0,W,H}; AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;
    g_settings=CreateWindowExW(0,L"VdeSettings",L"Settings - Virtual Desktops Extention",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_settings){ ShowWindow(g_settings,SW_SHOW); SetForegroundWindow(g_settings); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_HOTKEY: ShowPicker(); return 0;
    case WM_PAINT:{ PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps); RECT cr; GetClientRect(hwnd,&cr); Paint(hdc,cr); EndPaint(hwnd,&ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:{
        bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(wp==VK_CONTROL){ InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(wp==VK_ESCAPE){HidePicker();return 0;}
        if(wp==VK_RETURN||wp==VK_SPACE){Activate(g_sel,ctrl);return 0;}
        if(wp==VK_LEFT){MoveSel(-1,0);return 0;} if(wp==VK_RIGHT){MoveSel(1,0);return 0;}
        if(wp==VK_UP){MoveSel(0,-1);return 0;} if(wp==VK_DOWN){MoveSel(0,1);return 0;}
        if(wp==VK_TAB){ bool sh=(GetKeyState(VK_SHIFT)&0x8000)!=0; int n=(int)g_tiles.size(); if(n){g_sel=(g_sel+(sh?-1:1)+n)%n; InvalidateRect(hwnd,nullptr,FALSE);} return 0; }
        if(wp>='1'&&wp<='9'){Activate((int)(wp-'1'),ctrl);return 0;} if(wp=='0'){Activate(9,ctrl);return 0;}
        return 0; }
    case WM_KEYUP:
        if(wp==VK_CONTROL) InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    case WM_LBUTTONDOWN:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ Activate((int)i,ctrl); return 0; } return 0; }
    case WM_MOUSEMOVE:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ if(g_sel!=(int)i){g_sel=(int)i; InvalidateRect(hwnd,nullptr,FALSE);} break; } return 0; }
    case WM_TIMER:
        if(wp==TIMER_AUTOFIX && g_autoFix){
            std::wstring p=CurrentSessionstorePath();        // профиль кэширован -> дёшево
            if(!p.empty()){
                ULONGLONG m=FileMtime(p);
                if(m && m!=g_lastMtime){                      // 1) файл реально изменился
                    g_lastMtime=m;
                    unsigned long long sig=LayoutSignature(); // 2) изменилась ли раскладка окно->десктоп
                    if(sig!=g_lastLayoutSig){                 //    тяжёлый разбор+запись только тогда
                        g_lastLayoutSig=sig;
                        RunSave();
                    }
                }
            }
        }
        return 0;
    case WM_ACTIVATE: if(LOWORD(wp)==WA_INACTIVE)HidePicker(); return 0;
    case WM_TRAY:
        if(LOWORD(lp)==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt); HMENU m=CreatePopupMenu();
            AppendMenuW(m,MF_STRING,200,L"Open desktop picker");
            AppendMenuW(m,MF_SEPARATOR,0,nullptr);
            AppendMenuW(m,MF_STRING,201,L"Save Firefox layout");
            AppendMenuW(m,MF_STRING,202,L"Restore Firefox layout");
            AppendMenuW(m,MF_SEPARATOR,0,nullptr);
            AppendMenuW(m,MF_STRING,203,L"Settings...");
            AppendMenuW(m,MF_STRING,209,L"Exit");
            SetForegroundWindow(hwnd);
            int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr); DestroyMenu(m);
            if(cmd==200)ShowPicker();
            else if(cmd==201)Balloon(U82W(RunSave()));
            else if(cmd==202)Balloon(U82W(RunRestore()));
            else if(cmd==203)OpenSettings();
            else if(cmd==209)DestroyWindow(hwnd);
        } else if(LOWORD(lp)==WM_LBUTTONDBLCLK) ShowPicker();
        return 0;
    case WM_DESTROY: TrayRemove(); UnregisterHotKey(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

// ================================ entry ======================================
static int RunGui(HINSTANCE hInst){
    g_inst=hInst;
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_HOTKEY_CLASS|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    InitMetrics();
    g_uiFont=CreateFontW(-S(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    LoadSettings();

    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"VdeWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
    wc.hIconSm=LoadAppIcon(GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);
    g_main=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"VdeWindow",APP_NAME,WS_POPUP,0,0,400,300,nullptr,nullptr,hInst,nullptr);
    if(!g_main)return 3;
    TrayAdd(g_main);
    bool hk=ApplyHotkey();
    if(!hk) MessageBoxW(nullptr,L"Could not register the global hotkey.\n"
                               L"Another app may be using it. Change it in Settings,\n"
                               L"or open the picker via double-click on the tray icon.",APP_NAME,MB_ICONWARNING);
    ApplyAutoFix();
    Balloon(L"Running. Press your hotkey to move the active window to a desktop.");
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){
        if(g_settings && IsDialogMessageW(g_settings,&msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if(g_uiFont)DeleteObject(g_uiFont);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int){
    SetProcessDPIAware();
    int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring cmd = (argc>=2)?argv[1]:L"";
    if(argv)LocalFree(argv);
    bool cli = (cmd==L"save"||cmd==L"restore"||cmd==L"status"||cmd==L"list"||cmd==L"-h"||cmd==L"--help"||cmd==L"/?");

    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 1;

    int rc;
    if(cli){
        if(AttachConsole(ATTACH_PARENT_PROCESS)){ FILE* f; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr); }
        SetConsoleOutputCP(CP_UTF8);
        if(!InitServices()){ printf("Failed to init virtual-desktop services (IID layout may not match this build).\n"); rc=3; }
        else rc=CliRun(cmd);
    } else {
        if(!InitServices()){ MessageBoxW(nullptr,L"Could not start virtual-desktop services.\nThe interface IID layout may not match this Windows build.",APP_NAME,MB_ICONERROR); rc=2; }
        else rc=RunGui(hInst);
    }
    ReleaseServices(); CoUninitialize();
    return rc;
}
