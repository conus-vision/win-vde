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

#include "str_util.hpp"   // W2U8/U82W, GUID helpers, b64, GetWindowsBuild, hostOf, etld1
#include "layout.hpp"     // DeskRec + v4 layout serialization/migration logic
#include "lifecycle.hpp"  // LcState/LcAction, LcOnStartup/LcOnTick/LcOnExit
#include "session.hpp"    // bounded browser-session decoding primitives
#include "appprofile.hpp" // AppProfile, BuiltinProfiles
#include "session_worker.hpp" // bounded browser-session ownership and transitional helpers
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

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
static bool g_autoFix = true;                   // монитор: авто-сохранение и авто-восстановление раскладки
static bool g_degraded = false;                 // недокументированный COM не работает (обновление Windows) -> урезанный режим
static bool g_appFirefox = true, g_appChrome = true, g_appEdge = true;  // какие приложения отслеживать
static const bool SWITCH_AFTER_MOVE = false;    // переключаться на десктоп после переноса окна

#define IDI_APPICON 101    // должен совпадать с ID в vde.rc
#define TIMER_MONITOR 1
#define TIMER_STARTUP 2
#define MONITOR_INTERVAL_MS 5000
#define STARTUP_SETTLE_MS 2500
#define LAUNCH_SETTLE_TICKS 4          // 4 * 5000ms = ~20s launch stabilization
#define IDC_HOTKEY 1001
#define IDC_AUTOFIX 1002
#define IDC_AUTOSTART 1003
#define IDC_APP_FF 1004
#define IDC_APP_CR 1005
#define IDC_APP_ED 1006
#define IDC_LINK_MAIL 1101
#define IDC_LINK_REPO 1102
#define IDC_ABOUT_COPY 1103
#define IDC_HELP_TEXT 1104
static const wchar_t* APP_VERSION = L"1.0.0";

// ---- lifecycle monitor state (this run) ----
static LcState g_lc;
static std::set<std::string> g_seenKeys;      // window fingerprints seen at least once this run
static std::set<std::string> g_observedApps;  // apps observed at least once this run

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
// W2U8/U82W, GUID helpers, base64, GetWindowsBuild → moved to str_util.hpp
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

// Browser-session decoders live in session.hpp. The synchronous callers below
// are explicitly transitional until Task 8 atomically cuts the UI over.
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
// After a Windows update the undocumented vtable/IIDs can shift: QueryService may
// still succeed but calls return garbage. Verify a couple of calls make sense.
static bool SanityCheckServices(){
    if(!g_vdmi) return false;
    UINT n=0; if(FAILED(g_vdmi->GetCount(&n))) return false;
    if(n<1 || n>64) return false;
    IVirtualDesktop* d=nullptr; if(FAILED(g_vdmi->GetCurrentDesktop(&d))||!d) return false;
    GUID g={0}; bool ok=SUCCEEDED(d->GetID(&g)) && !GuidIsZero(g); d->Release(); return ok;
}
static DWORD ReadLastGoodBuild(){ HKEY hk; DWORD v=0,cb=sizeof(v);
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\VirtualDesktopsExtention",0,KEY_READ,&hk)==ERROR_SUCCESS){ RegQueryValueExW(hk,L"LastGoodBuild",0,0,(LPBYTE)&v,&cb); RegCloseKey(hk); } return v; }
static void WriteLastGoodBuild(DWORD b){ HKEY hk;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\VirtualDesktopsExtention",0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)==ERROR_SUCCESS){ RegSetValueExW(hk,L"LastGoodBuild",0,REG_DWORD,(LPBYTE)&b,sizeof(b)); RegCloseKey(hk); } }
// Autostart: HKCU..\Run value pointing at this exe.
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VAL = L"VirtualDesktopsExtention";
static bool GetRunAtLogon(){ HKEY hk; bool r=false;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_READ,&hk)==ERROR_SUCCESS){ r=(RegQueryValueExW(hk,RUN_VAL,0,0,0,0)==ERROR_SUCCESS); RegCloseKey(hk); } return r; }
static void SetRunAtLogon(bool on){ HKEY hk;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_WRITE,&hk)!=ERROR_SUCCESS) return;
    if(on){ wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring q=L"\""+std::wstring(p)+L"\"";
        RegSetValueExW(hk,RUN_VAL,0,REG_SZ,(LPBYTE)q.c_str(),(DWORD)((q.size()+1)*sizeof(wchar_t))); }
    else RegDeleteValueW(hk,RUN_VAL);
    RegCloseKey(hk);
}
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
static std::wstring StripSuffixes(std::wstring t, const std::vector<std::wstring>& sfx){
    for(auto& s:sfx){ size_t l=s.size(); if(t.size()>=l&&_wcsicmp(t.c_str()+(t.size()-l),s.c_str())==0){ t.resize(t.size()-l); return t; } }
    return t;
}
struct LiveWin { HWND hwnd; std::wstring rawTitle; GUID desktop; };
struct EnumCtx { std::vector<LiveWin>* out; const AppProfile* prof; };
static BOOL CALLBACK EnumAppCb(HWND hwnd, LPARAM lp){
    auto* ctx=(EnumCtx*)lp; wchar_t cls[64]={0};
    if(GetClassNameW(hwnd,cls,64)<=0)return TRUE;
    bool clsOk=false; for(auto& c:ctx->prof->classNames) if(c==cls){clsOk=true;break;} if(!clsOk)return TRUE;
    if(!(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE))return TRUE;
    int len=GetWindowTextLengthW(hwnd); if(len<=0)return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    std::wstring img=ProcessImageName(pid); if(!img.empty()&&!EndsWithI(img,ctx->prof->exeName))return TRUE;
    std::wstring title(len+1,0); GetWindowTextW(hwnd,&title[0],len+1); title.resize(wcslen(title.c_str()));
    LiveWin w; w.hwnd=hwnd; w.rawTitle=title; w.desktop=GUID{0}; ctx->out->push_back(w);
    return TRUE;
}
static std::vector<LiveWin> EnumAppWindows(const AppProfile& prof){
    std::vector<LiveWin> v; EnumCtx ctx{&v,&prof}; EnumWindows(EnumAppCb,(LPARAM)&ctx);
    if(g_vdmDoc)for(auto&w:v)g_vdmDoc->GetWindowDesktopId(w.hwnd,&w.desktop); return v;
}

// TRANSITIONAL (removed by Task 8): the legacy monitor remains synchronous,
// but all discovery, bounded I/O, stamps, decompression, and parsing delegate
// to the worker-owned primitives. Nothing is published across a rotation.
static std::vector<WinFp> ReadSessionForTransitional(const AppProfile& profile,
                                                      std::wstring* usedPathOut=nullptr){
    std::wstring before=ResolveBrowserSessionPath(profile);
    SessionStamp beforeStamp;
    if(before.empty() || !GetSessionStamp(before,beforeStamp)) return {};
    FileReadResult read=ReadFileBytesBounded(before,MAX_BROWSER_SESSION_BYTES);
    if(read.status!=FileReadStatus::Ok) return {};
    std::vector<WinFp> parsed;
    if(!ParseBrowserSessionData(profile,read.bytes,parsed)) return {};
    std::wstring after=ResolveBrowserSessionPath(profile);
    SessionStamp afterStamp;
    if(after!=before || !GetSessionStamp(after,afterStamp) || afterStamp!=beforeStamp) return {};
    if(usedPathOut) *usedPathOut=after;
    return parsed;
}

// ============================ fingerprint / scoring ===========================
struct Fp {
    std::string app;
    HWND hwnd=nullptr; GUID desktop={0}; int deskIndex=-1;
    std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount=0;
    bool hasDomains() const { return !counts.empty(); }
};
static std::vector<AppProfile> ActiveProfiles(){ return BuiltinProfiles(g_appFirefox,g_appChrome,g_appEdge); }
static std::vector<WinFp> ReadSessionFor(const AppProfile& prof){
    return prof.session==AppProfile::NONE?std::vector<WinFp>():ReadSessionForTransitional(prof);
}
static std::vector<Fp> BuildLiveFingerprintsFor(const AppProfile& prof, int* boundCount=nullptr){
    std::vector<LiveWin> live=EnumAppWindows(prof);
    std::vector<WinFp> ss=ReadSessionFor(prof);
    std::vector<bool> used(ss.size(),false); int bound=0; std::vector<Fp> out;
    for(auto& w:live){
        std::string sTitle=W2U8(StripSuffixes(w.rawTitle,prof.titleSuffixes));
        Fp fp; fp.app=prof.id; fp.hwnd=w.hwnd; fp.desktop=w.desktop; fp.activeTitle=sTitle;
        int bi=-1; for(size_t i=0;i<ss.size();++i) if(!used[i]&&ss[i].activeTitle==sTitle){bi=(int)i;break;}   // bind by active-tab title
        if(bi>=0){ used[bi]=true; bound++; fp.counts=ss[bi].counts; fp.activeDomain=ss[bi].activeDomain; fp.tabCount=ss[bi].tabCount; if(!ss[bi].activeTitle.empty())fp.activeTitle=ss[bi].activeTitle; }
        out.push_back(std::move(fp));
    }
    if(boundCount)*boundCount=bound;
    return out;
}
static bool AnyAppPresent(){ for(auto& prof:ActiveProfiles()) if(!EnumAppWindows(prof).empty()) return true; return false; }
// HWND -> lowercased text of ALL tabs (titles + domains) for each browser window; used to search across all tabs, not just the active one.
static std::map<HWND,std::wstring> BuildTabBlobs(){
    std::map<HWND,std::wstring> out;
    for(auto& prof:ActiveProfiles()){
        if(prof.session==AppProfile::NONE) continue;
        auto live=EnumAppWindows(prof); auto ss=ReadSessionFor(prof);
        std::vector<bool> used(ss.size(),false);
        for(auto& w:live){
            std::string sTitle=W2U8(StripSuffixes(w.rawTitle,prof.titleSuffixes));
            int bi=-1; for(size_t i=0;i<ss.size();++i) if(!used[i]&&ss[i].activeTitle==sTitle){ bi=(int)i; break; }
            if(bi>=0){ used[bi]=true; std::wstring blob=U82W(ss[bi].tabsBlob); if(!blob.empty())CharLowerW(&blob[0]); out[w.hwnd]=std::move(blob); }
        }
    }
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
static UnixSeconds UtcNowSeconds(){
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks{}; ticks.LowPart=ft.dwLowDateTime; ticks.HighPart=ft.dwHighDateTime;
    const ULONGLONG WINDOWS_TO_UNIX_EPOCH=116444736000000000ULL;
    if(ticks.QuadPart<WINDOWS_TO_UNIX_EPOCH)return 0;
    return (UnixSeconds)((ticks.QuadPart-WINDOWS_TO_UNIX_EPOCH)/10000000ULL);
}
static std::wstring DataDir(){
    wchar_t base[MAX_PATH]={0}; GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    std::wstring dir=std::wstring(base)+L"\\VirtualDesktopsExtention"; CreateDirectoryW(dir.c_str(),nullptr); return dir;
}
static std::wstring LayoutPath(bool manual){ return DataDir()+(manual?L"\\layout-manual.txt":L"\\layout-auto.txt"); }
static void MigrateLegacyLayout(){
    std::wstring legacy=DataDir()+L"\\layout.txt", autoP=LayoutPath(false);
    if(FileExists(legacy) && !FileExists(autoP)) MoveFileW(legacy.c_str(), autoP.c_str());
}
static bool WriteTextFile(const std::wstring& path, const std::string& text){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false; DWORD wr=0; BOOL ok=WriteFile(f,text.data(),(DWORD)text.size(),&wr,nullptr); CloseHandle(f); return ok&&wr==text.size();
}
static bool CurrentDesktops(std::vector<DeskRec>& desksOut, std::string* errorOut=nullptr){
    auto fail=[&](const std::string& message)->bool{ if(errorOut)*errorOut=message; return false; };
    if(!g_vdmi)return fail("virtual desktop manager is unavailable");
    UINT count=0; if(FAILED(g_vdmi->GetCount(&count)))return fail("failed to get virtual desktop count");
    std::vector<DeskRec> desks;
    for(UINT i=0;i<count;++i){
        IVirtualDesktop* d=GetDesktopByIndex(i); if(!d)return fail("failed to get virtual desktop");
        GUID g={0}; HRESULT idHr=d->GetID(&g);
        if(FAILED(idHr)||GuidIsZero(g)){ d->Release(); return fail("failed to get virtual desktop GUID"); }
        DeskRec dr; dr.index=(int)i; dr.guid=g; dr.name=DesktopNameFromRegistry(g); desks.push_back(dr); d->Release();
    }
    desksOut.swap(desks); if(errorOut)errorOut->clear(); return true;
}
// Currently-open windows (Phase 1: Firefox) as LayoutWin with desktop assigned.
// Accumulates each window's FingerprintKey into `seen` and app into `apps` (for grace bookkeeping).
static std::vector<LayoutWin> CollectPresentWindows(std::set<std::string>* seen, std::set<std::string>* apps){
    std::vector<LayoutWin> out;
    for(auto& prof:ActiveProfiles()) for(auto& f:BuildLiveFingerprintsFor(prof)){
        if(GuidIsZero(f.desktop))continue;
        LayoutWin w; w.app=f.app; w.deskIndex=GetDesktopIndexByGuid(f.desktop);
        w.desktop=f.desktop; w.activeTitle=f.activeTitle; w.activeDomain=f.activeDomain; w.tabCount=f.tabCount; w.counts=f.counts; w.missingRuns=0;
        out.push_back(w);
        if(seen) seen->insert(FingerprintKey(w.app,w.counts,w.activeTitle));
        if(apps) apps->insert(w.app);
    }
    return out;
}

// ============================ save / restore cores ============================
// Возвращают краткую сводку (UTF-8) для трей-балуна; CLI печатает её и детали.
static Fp SavedToFp(const LayoutWin& w){
    Fp f; f.desktop=w.desktop; f.deskIndex=w.deskIndex; f.activeTitle=w.activeTitle;
    f.activeDomain=w.activeDomain; f.counts=w.counts; f.tabCount=w.tabCount; return f;
}
// Manual save: full snapshot of currently-open windows → layout-manual.txt (no grace).
static std::string RunSaveManual(){
    if(g_degraded) return "Virtual-desktop features are unavailable on this Windows build.";
    auto present=CollectPresentWindows(nullptr,nullptr);
    if(present.empty()) return "No browser windows found. Nothing to save.";
    std::vector<DeskRec> desks; std::string prepareError, snapshot;
    if(!CurrentDesktops(desks,&prepareError)) return "Failed to collect desktops: "+prepareError;
    if(!BuildCheckedLayoutSnapshot(desks,present,UtcNowSeconds(),snapshot,&prepareError)) return "Failed to prepare manual layout: "+prepareError;
    if(!WriteTextFile(LayoutPath(true), snapshot)) return "Failed to write manual layout.";
    char b[160]; sprintf_s(b,"Saved layout: %d window(s).",(int)present.size()); return b;
}
// Auto save: merge present windows into layout-auto.txt. Never touches the file when
// no windows are open (anti-wipe). Absent windows are kept (grace, aged only on exit).
static std::string RunSaveAuto(std::set<std::string>* seen, std::set<std::string>* apps){
    if(g_degraded) return "";
    UnixSeconds nowUtc=UtcNowSeconds();
    auto present=CollectPresentWindows(seen,apps);
    if(present.empty()) return "";
    std::vector<DeskRec> desks; std::string prepareError, snapshot;
    if(!CurrentDesktops(desks,&prepareError)) return "Failed to collect desktops: "+prepareError;
    std::string existingBytes; bool haveExisting=ReadFileBytes(LayoutPath(false),existingBytes);
    if(!BuildAutoLayoutSnapshot(haveExisting?&existingBytes:nullptr,desks,present,nowUtc,snapshot,&prepareError))
        return "Failed to prepare auto layout: "+prepareError;
    WriteTextFile(LayoutPath(false), snapshot);
    return "auto-saved";
}
static std::string RunRestore(bool manual, std::vector<std::string>* linesOut=nullptr, int* movedOut=nullptr){
    if(g_degraded) return "Virtual-desktop features are unavailable on this Windows build.";
    std::vector<DeskRec> savedDesks; std::vector<LayoutWin> saved;
    { std::string t; if(!ReadFileBytes(LayoutPath(manual),t) || !ParseLayout(t,savedDesks,saved,UtcNowSeconds()) || saved.empty())
        return manual?"No saved layout. Use 'Save windows layout' first.":"No auto layout yet."; }
    std::vector<Fp> live; for(auto& prof:ActiveProfiles()){ auto l=BuildLiveFingerprintsFor(prof); for(auto& x:l) live.push_back(std::move(x)); }
    if(live.empty())return "No browser windows to restore.";
    UINT count=0; g_vdmi->GetCount(&count);
    const double T_FLOOR=0.35,T_ACCEPT=0.55;
    struct Pair{ double sc; int si,li; }; std::vector<Pair> pairs;
    for(int i=0;i<(int)saved.size();++i){ Fp S=SavedToFp(saved[i]);
        for(int j=0;j<(int)live.size();++j){ if(saved[i].app!=live[j].app)continue; double sc=Score(S,live[j]); if(sc>=T_FLOOR)pairs.push_back({sc,i,j}); } }   // only match same app
    std::sort(pairs.begin(),pairs.end(),[](const Pair&a,const Pair&b){return a.sc>b.sc;});
    std::vector<int> usedS(saved.size(),0),usedL(live.size(),0),assignL2S(live.size(),-1); int matched=0;
    for(auto& p:pairs) if(!usedS[p.si]&&!usedL[p.li]&&p.sc>=T_ACCEPT){ usedS[p.si]=usedL[p.li]=1; assignL2S[p.li]=p.si; matched++; }
    int already=0,failed=0,realMoved=0;
    for(int li=0;li<(int)live.size();++li){ Fp& L=live[li];
        if(assignL2S[li]<0){ if(linesOut)linesOut->push_back("[no match] "+L.activeTitle); continue; }
        const LayoutWin& S=saved[assignL2S[li]];
        IVirtualDesktop* dest=nullptr; GUID destGuid={0};
        if(GetDesktopIndexByGuid(S.desktop)>=0){ dest=GetDesktopByGuid(S.desktop); destGuid=S.desktop; }
        else if(S.deskIndex>=0&&(UINT)S.deskIndex<count){ dest=GetDesktopByIndex((UINT)S.deskIndex); if(dest)dest->GetID(&destGuid); }
        if(!dest){ if(linesOut)linesOut->push_back("[no target] "+L.activeTitle); failed++; continue; }
        if(!GuidIsZero(L.desktop)&&GuidEq(L.desktop,destGuid)){ if(linesOut)linesOut->push_back("[already there] "+L.activeTitle); dest->Release(); already++; continue; }
        bool ok=MoveWindowToDesktop(L.hwnd,dest,destGuid); dest->Release();
        if(ok){ realMoved++; if(linesOut)linesOut->push_back("[moved] "+L.activeTitle); }
        else  { failed++; if(linesOut)linesOut->push_back("[FAILED] "+L.activeTitle); }
    }
    if(movedOut)*movedOut=realMoved;                        // real moves only (excludes "already in place")
    char b[200];
    if(matched==0)                     sprintf_s(b,"Restore: no matching windows (%d open, %d saved).",(int)live.size(),(int)saved.size());
    else if(realMoved==0 && failed==0) sprintf_s(b,"Restore: nothing to move - all %d matched window(s) already in place.",matched);
    else if(failed==0)                 sprintf_s(b,"Restore: moved %d, %d already in place (%d/%d matched).",realMoved,already,matched,(int)live.size());
    else                               sprintf_s(b,"Restore: moved %d, %d already in place, %d failed (%d/%d matched).",realMoved,already,failed,matched,(int)live.size());
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
        if(cmd==L"status"){
            for(auto& prof:ActiveProfiles()){ int bound=0; auto fps=BuildLiveFingerprintsFor(prof,&bound);
                if(fps.empty())continue;
                printf("\n%s: %d live window(s) (bound to session data: %d)\n",prof.id.c_str(),(int)fps.size(),bound);
                for(auto& f:fps){ int idx=GuidIsZero(f.desktop)?-1:GetDesktopIndexByGuid(f.desktop);
                    printf("  hwnd=0x%p desktop=[%d] tabs=%d active=\"%s\"\n",(void*)f.hwnd,idx,f.tabCount,f.activeTitle.c_str());
                    printf("        domains:"); for(auto& kv:f.counts)printf(" %s:%d",kv.first.c_str(),kv.second); printf("\n"); }
            }
        }
        return 0;
    }
    if(cmd==L"save"){ std::string s=RunSaveManual(); printf("%s\n",s.c_str()); printf("Manual layout: %s\n",W2U8(LayoutPath(true)).c_str()); return 0; }
    if(cmd==L"restore"){ std::vector<std::string> lines; std::string s=RunRestore(true,&lines); for(auto& l:lines)printf("  %s\n",l.c_str()); printf("\n%s\n",s.c_str()); return 0; }
    if(cmd==L"restore-auto"){ std::vector<std::string> lines; std::string s=RunRestore(false,&lines); for(auto& l:lines)printf("  %s\n",l.c_str()); printf("\n%s\n",s.c_str()); return 0; }
    printf("Usage: vde <save|restore|restore-auto|status|list>\n");
    printf("  save          save current window layout to layout-manual.txt\n");
    printf("  restore       restore from layout-manual.txt\n");
    printf("  restore-auto  restore from the last auto-saved layout\n");
    printf("  (no args) -> run resident in tray; Ctrl+Alt+D opens the desktop picker\n");
    return 2;
}

// ================================ GUI: picker ================================
struct WinItem { HWND hwnd; std::wstring title; std::wstring titleLC; std::wstring search; HICON icon; };   // search = titleLC (+ all-tab text for browser windows)
struct Tile { GUID guid; std::wstring name; int index; std::vector<WinItem> windows; RECT rc; int scroll=0; };
static std::vector<Tile> g_tiles;
static int  g_sel=0;
static HWND g_target=nullptr; static std::wstring g_targetTitle;
static HWND g_main=nullptr;
static std::unique_ptr<SessionWorker> g_sessionWorker; // Task 8 starts submitting work.
static HWND g_settings=nullptr;
static HINSTANCE g_inst=nullptr;
static HFONT g_uiFont=nullptr;
static unsigned long long g_lastLayoutSig=0;
static const UINT WM_TRAY=WM_APP+1;
static NOTIFYICONDATAW g_nid={0};
static int g_dpi=96;
static int S(int v){ return MulDiv(v,g_dpi,96); }   // px@96dpi -> px@текущий DPI

static void StopSessionWorker(HWND messageWindow){
    if(g_sessionWorker){ g_sessionWorker->Stop(); g_sessionWorker.reset(); }
    DrainPostedSessionResults(messageWindow);
}
static int TILE_W=240,TILE_H=150,PAD=16,HEADER=44,SEARCH_H=40;  // базовые (96 dpi); пересчёт в InitMetrics
static int g_cols=1,g_rows=1;
static HFONT g_fPT=nullptr,g_fPN=nullptr,g_fPI=nullptr,g_fPX=nullptr;   // cached picker fonts (avoid re-create per repaint)
static int g_lastHoverRow=-1;                                          // last tooltip row (avoid redundant TTM churn)
static void InitMetrics(){
    HDC dc=GetDC(nullptr); g_dpi=GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(nullptr,dc);
    TILE_W=S(240); TILE_H=S(150); PAD=S(16); HEADER=S(38); SEARCH_H=S(58);
    if(g_fPT)DeleteObject(g_fPT); if(g_fPN)DeleteObject(g_fPN); if(g_fPI)DeleteObject(g_fPI); if(g_fPX)DeleteObject(g_fPX);
    g_fPT=CreateFontW(S(20),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPN=CreateFontW(S(17),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPI=CreateFontW(S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPX=CreateFontW(S(30),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
}
// ---- picker search / scroll / tooltip state ----
static HWND g_search=nullptr; static WNDPROC g_searchOrigProc=nullptr; static std::wstring g_searchText;
static HWND g_tip=nullptr;
struct RowRec { RECT rc; std::wstring full; bool trunc; };
static std::vector<RowRec> g_hoverRows;                  // window rows drawn this Paint (for hover tooltip)
static std::wstring LowerW(std::wstring s){ if(!s.empty()) CharLowerW(&s[0]); return s; }
static bool MatchesSearch(const std::wstring& title){ return g_searchText.empty() || LowerW(title).find(g_searchText)!=std::wstring::npos; }
// ---- picker palette (per mockup) ----
static const COLORREF CLR_BG=RGB(20,20,24), CLR_TILE=RGB(28,28,33), CLR_TILE_DIM=RGB(22,22,26), CLR_SEARCH=RGB(34,33,38),
    CLR_ACTIVE=RGB(0xF2,0x96,0x05) /*#f29605*/, CLR_PASSIVE=RGB(0x6B,0x60,0x4F) /*#6b604f*/, CLR_BORDER=RGB(58,55,52),
    CLR_TEXT=RGB(208,206,210), CLR_HEAD=RGB(238,238,242), CLR_HINT=RGB(150,145,135), CLR_DIM=RGB(110,108,112),
    CLR_SCROLL_TRK=RGB(40,40,46), CLR_SCROLL_THB=RGB(96,92,86);
static RECT g_clearBtn={0,0,0,0};       // × clear-search hit rect (empty when hidden)
static bool g_searchActive=false;       // active border: set when user clicks the field or types; cleared by × / on open
static HFONT g_searchFont=nullptr;
static RECT SearchBoxRect(int clientW){ RECT r; r.left=PAD; r.top=S(12); r.right=clientW-PAD; r.bottom=S(12)+S(40); return r; }
static void FillRoundRect(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF border, int bw){
    HBRUSH b=CreateSolidBrush(fill); HPEN p=CreatePen(PS_SOLID,bw,border);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,b); HPEN op=(HPEN)SelectObject(hdc,p);
    RoundRect(hdc,r.left,r.top,r.right,r.bottom,rad,rad);
    SelectObject(hdc,ob); SelectObject(hdc,op); DeleteObject(b); DeleteObject(p);
}

static bool IsAltTabWindow(HWND h){ if(!IsWindowVisible(h))return false; if(GetWindowTextLengthW(h)<=0)return false; LONG_PTR ex=GetWindowLongPtrW(h,GWL_EXSTYLE); if(ex&WS_EX_TOOLWINDOW)return false; if(GetAncestor(h,GA_ROOTOWNER)!=h)return false; return true; }
static HICON WindowIcon(HWND h){ DWORD_PTR r=0; SendMessageTimeoutW(h,WM_GETICON,ICON_SMALL2,0,SMTO_ABORTIFHUNG,200,&r); HICON i=(HICON)r;
    if(!i)i=(HICON)GetClassLongPtrW(h,GCLP_HICONSM); if(!i){r=0;SendMessageTimeoutW(h,WM_GETICON,ICON_BIG,0,SMTO_ABORTIFHUNG,200,&r);i=(HICON)r;}
    if(!i)i=(HICON)GetClassLongPtrW(h,GCLP_HICON); if(!i)i=LoadIconW(nullptr,IDI_APPLICATION); return i; }
static BOOL CALLBACK EnumAll(HWND hwnd, LPARAM){
    if(!IsAltTabWindow(hwnd))return TRUE; GUID g={0};
    if(!g_vdmDoc||FAILED(g_vdmDoc->GetWindowDesktopId(hwnd,&g))||GuidIsZero(g))return TRUE;
    for(auto& t:g_tiles) if(GuidEq(t.guid,g)){ int len=GetWindowTextLengthW(hwnd); std::wstring title(len+1,0); GetWindowTextW(hwnd,&title[0],len+1); title.resize(wcslen(title.c_str()));
        WinItem wi; wi.hwnd=hwnd; wi.title=title; wi.titleLC=title; if(!wi.titleLC.empty())CharLowerW(&wi.titleLC[0]); wi.search=wi.titleLC; wi.icon=WindowIcon(hwnd); t.windows.push_back(wi); break; }
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
static bool g_tabBlobsBuilt=false;
// Lazily fold each browser window's all-tab text into its search field (first time the user searches).
static void EnsureTabSearch(){
    if(g_tabBlobsBuilt) return; g_tabBlobsBuilt=true;
    auto blobs=BuildTabBlobs(); if(blobs.empty()) return;
    for(auto& t:g_tiles) for(auto& w:t.windows){ auto it=blobs.find(w.hwnd); if(it!=blobs.end() && !it->second.empty()) w.search = w.titleLC + L" " + it->second; }
}
static void LayoutTiles(int clientW){
    int n=(int)g_tiles.size();
    g_cols=std::max(1,std::min(n,std::max(1,(clientW-PAD)/(TILE_W+PAD)))); g_cols=std::min(g_cols,5);
    g_rows=(n+g_cols-1)/g_cols;
    for(int i=0;i<n;++i){ int r=i/g_cols,c=i%g_cols; RECT rc; rc.left=PAD+c*(TILE_W+PAD); rc.top=SEARCH_H+HEADER+PAD+r*(TILE_H+PAD); rc.right=rc.left+TILE_W; rc.bottom=rc.top+TILE_H; g_tiles[i].rc=rc; }
}
static SIZE DesiredClientSize(){ int n=(int)g_tiles.size(); int cols=std::min(std::max(1,n),5); int rows=(n+cols-1)/cols; SIZE s; s.cx=PAD+cols*(TILE_W+PAD); s.cy=SEARCH_H+HEADER+PAD+rows*(TILE_H+PAD); return s; }
static HDC g_memDC=nullptr; static HBITMAP g_memBmp=nullptr; static int g_memW=0,g_memH=0;   // cached double-buffer
static void Paint(HDC hdcReal, RECT client){
    g_hoverRows.clear();
    if(!g_memDC) g_memDC=CreateCompatibleDC(hdcReal);
    if(!g_memBmp || g_memW!=client.right || g_memH!=client.bottom){
        if(g_memBmp) DeleteObject(g_memBmp);
        g_memBmp=CreateCompatibleBitmap(hdcReal,client.right,client.bottom); SelectObject(g_memDC,g_memBmp);
        g_memW=client.right; g_memH=client.bottom;
    }
    HDC hdc=g_memDC;
    HBRUSH bg=CreateSolidBrush(CLR_BG); FillRect(hdc,&client,bg); DeleteObject(bg); SetBkMode(hdc,TRANSPARENT);
    HFONT fT=g_fPT, fN=g_fPN, fI=g_fPI, fX=g_fPX;   // cached (created in InitMetrics)
    bool ctrlHeld=(GetKeyState(VK_CONTROL)&0x8000)!=0;

    // subtle rounded outer border
    { HPEN p=CreatePen(PS_SOLID,1,CLR_BORDER); HPEN op=(HPEN)SelectObject(hdc,p); HBRUSH ob=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
      RoundRect(hdc,0,0,client.right-1,client.bottom-1,S(18),S(18)); SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(p); }

    // ---- search box (rounded) + clear (x) button ----
    RECT sb=SearchBoxRect(client.right);
    FillRoundRect(hdc, sb, S(12), CLR_SEARCH, g_searchActive?CLR_ACTIVE:CLR_PASSIVE, g_searchActive?S(2):S(1));   // active only after user clicks/types
    g_clearBtn.left=g_clearBtn.right=0;
    if(g_searchActive){                                          // big × (no circle) — shown whenever the field is active
        int cs=S(30), cx=sb.right-S(10)-cs, cy=sb.top+((sb.bottom-sb.top)-cs)/2;
        g_clearBtn={cx,cy,cx+cs,cy+cs};
        SelectObject(hdc,fX); SetTextColor(hdc,CLR_HINT); RECT xr=g_clearBtn; DrawTextW(hdc,L"\x2715",-1,&xr,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }

    // ---- header: left title + right Ctrl+Click hint ----
    int headTop=SEARCH_H, headBot=SEARCH_H+HEADER;
    SelectObject(hdc,fI); SetTextColor(hdc,CLR_HINT);
    const wchar_t* hint=L"Ctrl+Click - Move current window to selected desktop";
    SIZE hs; GetTextExtentPoint32W(hdc,hint,(int)wcslen(hint),&hs);
    RECT hr={PAD,headTop,client.right-PAD,headBot}; DrawTextW(hdc,hint,-1,&hr,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(hdc,fT); SetTextColor(hdc,CLR_HEAD);
    std::wstring head;
    if(ctrlHeld) head=L"Move window:  "+(g_targetTitle.empty()?L"(no window)":g_targetTitle);
    else { std::wstring nm=(g_sel>=0&&g_sel<(int)g_tiles.size())?g_tiles[g_sel].name:L""; head=L"Switch to: "+nm; }
    RECT h2={PAD,headTop,client.right-PAD-hs.cx-S(24),headBot}; DrawTextW(hdc,head.c_str(),-1,&h2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_VCENTER);

    // ---- tiles ----
    bool searching=!g_searchText.empty();
    for(size_t i=0;i<g_tiles.size();++i){ Tile& t=g_tiles[i]; bool isSel=((int)i==g_sel);
        std::vector<const WinItem*> fw; for(auto& w:t.windows) if(g_searchText.empty()||w.search.find(g_searchText)!=std::wstring::npos) fw.push_back(&w);   // search = title + all tabs (browsers)
        bool dim = searching && fw.empty();
        FillRoundRect(hdc, t.rc, S(10), dim?CLR_TILE_DIM:CLR_TILE, isSel?CLR_ACTIVE:CLR_PASSIVE, isSel?S(2):S(1));
        SelectObject(hdc,fN); SetTextColor(hdc,dim?CLR_DIM:CLR_HEAD);
        RECT nr=t.rc; nr.left+=S(14); nr.top+=S(10); nr.right-=S(12); nr.bottom=nr.top+S(22);
        std::wstring title=std::to_wstring(t.index+1)+L". "+t.name; DrawTextW(hdc,title.c_str(),-1,&nr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,fI); SetTextColor(hdc,CLR_TEXT);
        int rowH=S(22), listTop=nr.bottom+S(6), listBot=t.rc.bottom-S(10);
        int visRows=std::max(0,(listBot-listTop)/rowH); int maxScroll=std::max(0,(int)fw.size()-visRows);
        if(t.scroll>maxScroll)t.scroll=maxScroll; if(t.scroll<0)t.scroll=0;
        bool hasScroll=(int)fw.size()>visRows; int rowRight=t.rc.right-(hasScroll?S(18):S(14));
        int y=listTop;
        for(int k=t.scroll; k<(int)fw.size(); ++k){ if(y+rowH>listBot+S(3))break; const WinItem* w=fw[k];
            if(w->icon)DrawIconEx(hdc,t.rc.left+S(14),y,w->icon,S(16),S(16),0,nullptr,DI_NORMAL);
            RECT ir; ir.left=t.rc.left+S(38); ir.top=y; ir.right=rowRight; ir.bottom=y+S(18);
            DrawTextW(hdc,w->title.c_str(),-1,&ir,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            SIZE ts; GetTextExtentPoint32W(hdc,w->title.c_str(),(int)w->title.size(),&ts);
            RowRec rr; rr.rc=ir; rr.full=w->title; rr.trunc=(ts.cx>(ir.right-ir.left)); g_hoverRows.push_back(rr);
            y+=rowH;
        }
        if(hasScroll){                                   // custom rounded scrollbar
            RECT trk={t.rc.right-S(9),listTop,t.rc.right-S(5),listBot}; FillRoundRect(hdc,trk,S(2),CLR_SCROLL_TRK,CLR_SCROLL_TRK,1);
            int trkH=listBot-listTop, thbH=std::max(S(24),trkH*visRows/std::max(1,(int)fw.size()));
            int thbY=listTop+(maxScroll>0?(trkH-thbH)*t.scroll/maxScroll:0);
            RECT thb={t.rc.right-S(9),thbY,t.rc.right-S(5),thbY+thbH}; FillRoundRect(hdc,thb,S(2),CLR_SCROLL_THB,CLR_SCROLL_THB,1);
        }
    }
    BitBlt(hdcReal,0,0,client.right,client.bottom,hdc,0,0,SRCCOPY);
}
static void TipDeactivate(){ if(g_tip){ TOOLINFOW ti={0}; ti.cbSize=sizeof(ti); ti.hwnd=g_main; ti.uId=1; SendMessageW(g_tip,TTM_TRACKACTIVATE,FALSE,(LPARAM)&ti); } }
static void HidePicker(){ TipDeactivate(); ShowWindow(g_main,SW_HIDE); }
// Search EDIT subclass: forward navigation keys to the grid; let letters/numbers type.
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp){
    if((m==WM_KEYDOWN||m==WM_KEYUP)&&wp==VK_CONTROL){ InvalidateRect(g_main,nullptr,FALSE); return 0; }
    if(m==WM_KEYDOWN){ switch(wp){
        case VK_ESCAPE: case VK_RETURN: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_TAB:
            SendMessageW(g_main,WM_KEYDOWN,wp,lp); return 0; } }
    if(m==WM_CHAR){ if(wp==L'\r'||wp==27||wp==L'\t') return 0;    // swallow -> no message beep
        if(!g_searchActive){ g_searchActive=true; InvalidateRect(g_main,nullptr,FALSE); } }   // typing activates the field
    if(m==WM_LBUTTONDOWN && !g_searchActive){ g_searchActive=true; InvalidateRect(g_main,nullptr,FALSE); }   // click activates
    return CallWindowProcW(g_searchOrigProc,h,m,wp,lp);
}
static void EnsurePickerChildren(){
    if(!g_searchFont) g_searchFont=CreateFontW(-S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    if(!g_search){
        g_search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,10,10,g_main,nullptr,g_inst,nullptr);   // borderless (rounded box drawn behind)
        if(g_search){ SendMessageW(g_search,WM_SETFONT,(WPARAM)g_searchFont,TRUE);
            SendMessageW(g_search,EM_SETCUEBANNER,TRUE,(LPARAM)L"Type window name to search");
            g_searchOrigProc=(WNDPROC)SetWindowLongPtrW(g_search,GWLP_WNDPROC,(LONG_PTR)EditProc); }
    }
    if(!g_tip){
        g_tip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASS,nullptr,WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,0,0,0,0,g_main,nullptr,g_inst,nullptr);
        if(g_tip){ TOOLINFOW ti={0}; ti.cbSize=sizeof(ti); ti.uFlags=TTF_TRACK|TTF_ABSOLUTE; ti.hwnd=g_main; ti.uId=1; ti.lpszText=(LPWSTR)L"";
            SendMessageW(g_tip,TTM_ADDTOOLW,0,(LPARAM)&ti); SendMessageW(g_tip,TTM_SETMAXTIPWIDTH,0,S(600)); }
    }
}
static void Commit(int idx){ if(idx<0||idx>=(int)g_tiles.size())return; HidePicker(); if(g_target&&IsWindow(g_target)){ IVirtualDesktop* d=GetDesktopByIndex((UINT)g_tiles[idx].index); if(d){ GUID g={0};d->GetID(&g); MoveWindowToDesktop(g_target,d,g); if(SWITCH_AFTER_MOVE)g_vdmi->SwitchDesktop(d); d->Release(); } } }
static void ShowPicker(){
    if(g_degraded) return;   // desktop COM unavailable; startup dialog + tray tip already explain
    g_target=GetForegroundWindow(); if(g_target==g_main)g_target=nullptr;
    if(g_target&&IsWindow(g_target)){ int len=GetWindowTextLengthW(g_target); std::wstring t(len+1,0); GetWindowTextW(g_target,&t[0],len+1); t.resize(wcslen(t.c_str())); g_targetTitle=t; } else g_targetTitle.clear();
    BuildModel(); g_searchText.clear(); g_searchActive=false; g_tabBlobsBuilt=false; for(auto& t:g_tiles) t.scroll=0;
    SIZE sz=DesiredClientSize();
    HMONITOR mon=MonitorFromWindow(g_target?g_target:g_main,MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi={sizeof(mi)}; GetMonitorInfo(mon,&mi);
    RECT wr={0,0,sz.cx,sz.cy}; AdjustWindowRectEx(&wr,WS_POPUP,FALSE,WS_EX_TOOLWINDOW|WS_EX_TOPMOST);
    int ww=wr.right-wr.left,wh=wr.bottom-wr.top;
    int wx=mi.rcWork.left+((mi.rcWork.right-mi.rcWork.left)-ww)/2, wy=mi.rcWork.top+((mi.rcWork.bottom-mi.rcWork.top)-wh)/2;
    SetWindowPos(g_main,HWND_TOPMOST,wx,wy,ww,wh,SWP_NOACTIVATE);
    RECT cr; GetClientRect(g_main,&cr); LayoutTiles(cr.right);
    EnsurePickerChildren();
    if(g_search){ SetWindowTextW(g_search,L""); RECT sb=SearchBoxRect(cr.right);
        int eLeft=sb.left+S(14), eRight=sb.right-S(44), eH=S(22), eTop=sb.top+((sb.bottom-sb.top)-eH)/2;
        MoveWindow(g_search,eLeft,eTop,eRight-eLeft,eH,TRUE); ShowWindow(g_search,SW_SHOW); }
    ShowWindow(g_main,SW_SHOW); SetForegroundWindow(g_main);
    if(g_search) SetFocus(g_search);
    InvalidateRect(g_main,nullptr,FALSE);
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
    wcsncpy_s(g_nid.szTip, g_degraded ? L"Virtual Desktops Extension (compatibility issue - see About)" : APP_NAME, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD,&g_nid);
}
static void TrayRemove(){ Shell_NotifyIconW(NIM_DELETE,&g_nid); }
static void Balloon(const std::wstring& text){
    g_nid.uFlags=NIF_INFO; wcsncpy_s(g_nid.szInfo,text.c_str(),_TRUNCATE); wcsncpy_s(g_nid.szInfoTitle,APP_SHORT,_TRUNCATE); g_nid.dwInfoFlags=NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY,&g_nid); g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
}

// ============================ settings / autofix =============================
// Дешёвая подпись раскладки: только окна Firefox + их десктоп (без разбора sessionstore).
// Порядко-независимая (сумма пер-оконных хэшей), чтобы смена z-порядка при фокусе не считалась изменением.
// Cheap order-independent signature of {window -> desktop} across all tracked apps.
// Changes when any tracked window moves desktops or appears/disappears -> triggers auto-save.
static unsigned long long LayoutSignature(){
    unsigned long long acc=0;
    for(auto& prof:ActiveProfiles()) for(auto& w:EnumAppWindows(prof)){
        if(GuidIsZero(w.desktop)) continue;
        unsigned long long h=1469598103934665603ULL; // FNV-1a
        unsigned long long hv=(unsigned long long)(uintptr_t)w.hwnd;
        const unsigned char* b=(const unsigned char*)&hv; for(size_t i=0;i<sizeof(hv);++i){ h^=b[i]; h*=1099511628211ULL; }
        b=(const unsigned char*)&w.desktop; for(size_t i=0;i<sizeof(w.desktop);++i){ h^=b[i]; h*=1099511628211ULL; }
        acc += h;
    }
    return acc;
}

static void LoadSettings(){
    HKEY hk;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\VirtualDesktopsExtention",0,KEY_READ,&hk)==ERROR_SUCCESS){
        DWORD v=0,cb=sizeof(v);
        cb=sizeof(v); if(RegQueryValueExW(hk,L"HotkeyMods",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_hotMods=v;
        cb=sizeof(v); if(RegQueryValueExW(hk,L"HotkeyVk",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_hotVk=v;
        cb=sizeof(v); if(RegQueryValueExW(hk,L"AutoFix",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_autoFix=(v!=0);
        cb=sizeof(v); if(RegQueryValueExW(hk,L"AppFirefox",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_appFirefox=(v!=0);
        cb=sizeof(v); if(RegQueryValueExW(hk,L"AppChrome",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_appChrome=(v!=0);
        cb=sizeof(v); if(RegQueryValueExW(hk,L"AppEdge",0,0,(LPBYTE)&v,&cb)==ERROR_SUCCESS)g_appEdge=(v!=0);
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
        v=g_appFirefox?1:0; RegSetValueExW(hk,L"AppFirefox",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        v=g_appChrome?1:0;  RegSetValueExW(hk,L"AppChrome",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        v=g_appEdge?1:0;    RegSetValueExW(hk,L"AppEdge",0,REG_DWORD,(LPBYTE)&v,sizeof(v));
        RegCloseKey(hk);
    }
}
static bool ApplyHotkey(){
    UnregisterHotKey(g_main,1);
    return RegisterHotKey(g_main,1,g_hotMods|MOD_NOREPEAT,g_hotVk)!=0;
}
static void ApplyAutoFix(){   // enable/disable the lifecycle monitor timer
    if(g_autoFix && !g_degraded){ g_lastLayoutSig=0; SetTimer(g_main,TIMER_MONITOR,MONITOR_INTERVAL_MS,nullptr); }
    else KillTimer(g_main,TIMER_MONITOR);
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
        CreateWindowW(L"BUTTON",L"Auto-save && auto-restore layout",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(16),S(56),S(330),S(22),hwnd,(HMENU)IDC_AUTOFIX,g_inst,nullptr);
        CreateWindowW(L"STATIC",L"Automatically saves your layout and restores it after a reboot or browser restart.",WS_CHILD|WS_VISIBLE,S(16),S(80),S(332),S(32),hwnd,nullptr,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Start with Windows (run at logon)",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(16),S(116),S(330),S(22),hwnd,(HMENU)IDC_AUTOSTART,g_inst,nullptr);
        CreateWindowW(L"STATIC",L"Track these apps:",WS_CHILD|WS_VISIBLE,S(16),S(146),S(330),S(20),hwnd,nullptr,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Firefox",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(16),S(168),S(90),S(22),hwnd,(HMENU)IDC_APP_FF,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Chrome",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(120),S(168),S(90),S(22),hwnd,(HMENU)IDC_APP_CR,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Edge",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,S(224),S(168),S(90),S(22),hwnd,(HMENU)IDC_APP_ED,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"OK",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,S(176),S(206),S(75),S(28),hwnd,(HMENU)IDOK,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(260),S(206),S(75),S(28),hwnd,(HMENU)IDCANCEL,g_inst,nullptr);
        SetChildFont(hwnd);
        // init values
        WORD hf=0;
        if(g_hotMods&MOD_SHIFT)hf|=HOTKEYF_SHIFT;
        if(g_hotMods&MOD_CONTROL)hf|=HOTKEYF_CONTROL;
        if(g_hotMods&MOD_ALT)hf|=HOTKEYF_ALT;
        SendMessageW(hk,HKM_SETHOTKEY,MAKEWORD((BYTE)g_hotVk,(BYTE)hf),0);
        SendMessageW(GetDlgItem(hwnd,IDC_AUTOFIX),BM_SETCHECK,g_autoFix?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(GetDlgItem(hwnd,IDC_AUTOSTART),BM_SETCHECK,GetRunAtLogon()?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(GetDlgItem(hwnd,IDC_APP_FF),BM_SETCHECK,g_appFirefox?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(GetDlgItem(hwnd,IDC_APP_CR),BM_SETCHECK,g_appChrome?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(GetDlgItem(hwnd,IDC_APP_ED),BM_SETCHECK,g_appEdge?BST_CHECKED:BST_UNCHECKED,0);
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
            SetRunAtLogon(IsDlgButtonChecked(hwnd,IDC_AUTOSTART)==BST_CHECKED);
            g_appFirefox=(IsDlgButtonChecked(hwnd,IDC_APP_FF)==BST_CHECKED);
            g_appChrome =(IsDlgButtonChecked(hwnd,IDC_APP_CR)==BST_CHECKED);
            g_appEdge   =(IsDlgButtonChecked(hwnd,IDC_APP_ED)==BST_CHECKED);
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
    int W=S(364),H=S(280);
    RECT wr={0,0,W,H}; AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;
    g_settings=CreateWindowExW(0,L"VdeSettings",L"Settings - Virtual Desktops Extention",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_settings){ ShowWindow(g_settings,SW_SHOW); SetForegroundWindow(g_settings); }
}

// --------------------------- About window ------------------------------------
static HWND g_about=nullptr;
static void AboutCopy(HWND hwnd){
    std::wstring s=std::wstring(L"Virtual Desktops Extension v")+APP_VERSION+L" | info@conus.vision | Windows build "+std::to_wstring(GetWindowsBuild());
    if(OpenClipboard(hwnd)){ EmptyClipboard();
        size_t bytes=(s.size()+1)*sizeof(wchar_t); HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,bytes);
        if(h){ void* d=GlobalLock(h); if(d){ memcpy(d,s.c_str(),bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT,h); } }
        CloseClipboard();
    }
}
static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        int y=S(16);
        std::wstring title=std::wstring(L"Virtual Desktops Extension for Windows 11  \x2014  v")+APP_VERSION;
        CreateWindowW(L"STATIC",title.c_str(),WS_CHILD|WS_VISIBLE,S(16),y,S(352),S(20),hwnd,nullptr,g_inst,nullptr); y+=S(26);
        CreateWindowW(L"STATIC",L"Saves and restores browser windows across Windows 11 virtual desktops.",WS_CHILD|WS_VISIBLE,S(16),y,S(352),S(34),hwnd,nullptr,g_inst,nullptr); y+=S(40);
        CreateWindowW(L"STATIC",L"Author:  Volodymyr Moskvin",WS_CHILD|WS_VISIBLE,S(16),y,S(352),S(20),hwnd,nullptr,g_inst,nullptr); y+=S(24);
        CreateWindowW(L"SysLink",L"Email:  <a href=\"mailto:info@conus.vision\">info@conus.vision</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(16),y,S(352),S(20),hwnd,(HMENU)IDC_LINK_MAIL,g_inst,nullptr); y+=S(24);
        CreateWindowW(L"SysLink",L"GitHub:  <a href=\"https://github.com/conus-vision/win-vde\">github.com/conus-vision/win-vde</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(16),y,S(352),S(20),hwnd,(HMENU)IDC_LINK_REPO,g_inst,nullptr); y+=S(28);
        std::wstring lic=std::wstring(L"License: MIT        Windows build: ")+std::to_wstring(GetWindowsBuild());
        CreateWindowW(L"STATIC",lic.c_str(),WS_CHILD|WS_VISIBLE,S(16),y,S(352),S(20),hwnd,nullptr,g_inst,nullptr); y+=S(32);
        CreateWindowW(L"BUTTON",L"Copy",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(200),y,S(78),S(28),hwnd,(HMENU)IDC_ABOUT_COPY,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,S(288),y,S(80),S(28),hwnd,(HMENU)IDOK,g_inst,nullptr);
        SetChildFont(hwnd);
        return 0;
    }
    case WM_NOTIFY:{ NMHDR* n=(NMHDR*)lp;
        if(n->code==NM_CLICK||n->code==NM_RETURN){
            if(n->idFrom==IDC_LINK_MAIL) ShellExecuteW(hwnd,L"open",L"mailto:info@conus.vision",nullptr,nullptr,SW_SHOW);
            else if(n->idFrom==IDC_LINK_REPO) ShellExecuteW(hwnd,L"open",L"https://github.com/conus-vision/win-vde",nullptr,nullptr,SW_SHOW);
        } return 0; }
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_ABOUT_COPY){ AboutCopy(hwnd); return 0; }
        if(LOWORD(wp)==IDOK){ DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_CTLCOLORSTATIC:{ HDC dc=(HDC)wp; SetBkMode(dc,TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_about=nullptr; return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void OpenAbout(){
    if(g_about){ SetForegroundWindow(g_about); return; }
    static bool reg=false;
    if(!reg){ WNDCLASSW wc={0}; wc.lpfnWndProc=AboutProc; wc.hInstance=g_inst; wc.lpszClassName=L"VdeAbout";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
        RegisterClassW(&wc); reg=true; }
    int W=S(396),H=S(286);
    RECT wr={0,0,W,H}; AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;
    g_about=CreateWindowExW(0,L"VdeAbout",L"About - Virtual Desktops Extension",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_about){ ShowWindow(g_about,SW_SHOW); SetForegroundWindow(g_about); }
}

// --------------------------- compatibility-issue window ----------------------
static HWND g_compat=nullptr;
static bool g_compatBuildChanged=false;
static LRESULT CALLBACK CompatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        int y=S(14);
        const wchar_t* head = g_compatBuildChanged
            ? L"A recent Windows update changed the undocumented virtual-desktop interfaces this tool relies on."
            : L"The undocumented virtual-desktop interfaces this tool relies on are not available on this Windows build.";
        CreateWindowW(L"STATIC",head,WS_CHILD|WS_VISIBLE,S(16),y,S(424),S(34),hwnd,nullptr,g_inst,nullptr); y+=S(40);
        CreateWindowW(L"STATIC",L"Moving other apps' windows between virtual desktops has no public Windows API, so win-vde uses undocumented system interfaces. This is expected \x2014 it is not a fault in your system.",WS_CHILD|WS_VISIBLE,S(16),y,S(424),S(50),hwnd,nullptr,g_inst,nullptr); y+=S(54);
        std::wstring bld=std::wstring(L"Your Windows build: ")+std::to_wstring(GetWindowsBuild())+L".";
        CreateWindowW(L"STATIC",bld.c_str(),WS_CHILD|WS_VISIBLE,S(16),y,S(424),S(20),hwnd,nullptr,g_inst,nullptr); y+=S(24);
        CreateWindowW(L"STATIC",L"Please email the build number so a fix can be posted, or watch the project page:",WS_CHILD|WS_VISIBLE,S(16),y,S(424),S(20),hwnd,nullptr,g_inst,nullptr); y+=S(24);
        CreateWindowW(L"SysLink",L"<a href=\"mailto:info@conus.vision\">info@conus.vision</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(16),y,S(424),S(20),hwnd,(HMENU)IDC_LINK_MAIL,g_inst,nullptr); y+=S(22);
        CreateWindowW(L"SysLink",L"<a href=\"https://github.com/conus-vision/win-vde\">github.com/conus-vision/win-vde</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(16),y,S(424),S(20),hwnd,(HMENU)IDC_LINK_REPO,g_inst,nullptr); y+=S(30);
        CreateWindowW(L"BUTTON",L"Copy details",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(258),y,S(104),S(28),hwnd,(HMENU)IDC_ABOUT_COPY,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,S(372),y,S(70),S(28),hwnd,(HMENU)IDOK,g_inst,nullptr);
        SetChildFont(hwnd);
        return 0;
    }
    case WM_NOTIFY:{ NMHDR* n=(NMHDR*)lp; if(n->code==NM_CLICK||n->code==NM_RETURN){
        if(n->idFrom==IDC_LINK_MAIL) ShellExecuteW(hwnd,L"open",L"mailto:info@conus.vision",nullptr,nullptr,SW_SHOW);
        else if(n->idFrom==IDC_LINK_REPO) ShellExecuteW(hwnd,L"open",L"https://github.com/conus-vision/win-vde",nullptr,nullptr,SW_SHOW);
    } return 0; }
    case WM_COMMAND:
        if(LOWORD(wp)==IDC_ABOUT_COPY){ AboutCopy(hwnd); return 0; }
        if(LOWORD(wp)==IDOK){ DestroyWindow(hwnd); return 0; }
        return 0;
    case WM_CTLCOLORSTATIC:{ HDC dc=(HDC)wp; SetBkMode(dc,TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_compat=nullptr; return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void ShowCompatIssue(bool buildChanged){
    g_compatBuildChanged=buildChanged;
    static bool reg=false;
    if(!reg){ WNDCLASSW wc={0}; wc.lpfnWndProc=CompatProc; wc.hInstance=g_inst; wc.lpszClassName=L"VdeCompat";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
        RegisterClassW(&wc); reg=true; }
    int W=S(472),H=S(300);
    RECT wr={0,0,W,H}; AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;
    g_compat=CreateWindowExW(WS_EX_TOPMOST,L"VdeCompat",L"Virtual Desktops Extension \x2014 compatibility issue",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_compat){ ShowWindow(g_compat,SW_SHOW); SetForegroundWindow(g_compat); }
}

// --------------------------- Help window -------------------------------------
static const wchar_t* HELP_TEXT =
L"What this solves\r\n"
L"Windows 11 does not remember which virtual desktop your app windows were on after a reboot, and browsers change their window handles when they restore a session - so nothing external can recognize the windows. win-vde fingerprints each browser window by the domains of its tabs, matches old windows to new ones after a restart, and moves each one back to the desktop it was saved on.\r\n\r\n"
L"Picker  (hotkey Ctrl+Alt+D)\r\n"
L"A grid of your virtual desktops. Click a desktop to switch to it; Ctrl+click to move the active window there. Type in the box to filter windows by name, scroll a tile with the mouse wheel, and hover a clipped name to see it in full.\r\n\r\n"
L"Menu\r\n"
L"- Save windows layout: save the current windows to a manual checkpoint file.\r\n"
L"- Restore saved windows layout: put windows back from that manual checkpoint.\r\n"
L"- Restore last auto saved layout: put windows back from the rolling automatic layout.\r\n\r\n"
L"Settings\r\n"
L"- Auto-save & auto-restore layout: the utility watches your browsers and restores the layout automatically at startup and about 20 seconds after a browser launches. Just closing windows never erases the layout - a window is only forgotten after it has been gone for a few runs, so reopening it restores it.\r\n"
L"- Track these apps: choose which of Firefox, Chrome and Edge to manage.\r\n"
L"- Start with Windows: launch the utility at sign-in.\r\n\r\n"
L"Notes\r\n"
L"Only the virtual desktop of each window is restored; on-screen size and position are left to the browser. Moving other apps' windows between desktops relies on undocumented Windows interfaces, so a Windows update can temporarily break it - the app will tell you if that happens.";
static HWND g_help=nullptr;
static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        CreateWindowExW(0,L"EDIT",HELP_TEXT,WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,S(14),S(12),S(500),S(320),hwnd,(HMENU)IDC_HELP_TEXT,g_inst,nullptr);
        CreateWindowW(L"SysLink",L"Email:  <a href=\"mailto:info@conus.vision\">info@conus.vision</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(14),S(342),S(250),S(20),hwnd,(HMENU)IDC_LINK_MAIL,g_inst,nullptr);
        CreateWindowW(L"SysLink",L"GitHub:  <a href=\"https://github.com/conus-vision/win-vde\">github.com/conus-vision/win-vde</a>",WS_CHILD|WS_VISIBLE|WS_TABSTOP,S(14),S(366),S(320),S(20),hwnd,(HMENU)IDC_LINK_REPO,g_inst,nullptr);
        CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON|WS_TABSTOP,S(434),S(360),S(80),S(28),hwnd,(HMENU)IDOK,g_inst,nullptr);
        SetChildFont(hwnd);
        return 0;
    }
    case WM_NOTIFY:{ NMHDR* n=(NMHDR*)lp; if(n->code==NM_CLICK||n->code==NM_RETURN){
        if(n->idFrom==IDC_LINK_MAIL) ShellExecuteW(hwnd,L"open",L"mailto:info@conus.vision",nullptr,nullptr,SW_SHOW);
        else if(n->idFrom==IDC_LINK_REPO) ShellExecuteW(hwnd,L"open",L"https://github.com/conus-vision/win-vde",nullptr,nullptr,SW_SHOW);
    } return 0; }
    case WM_COMMAND: if(LOWORD(wp)==IDOK){ DestroyWindow(hwnd); return 0; } return 0;
    case WM_CTLCOLORSTATIC:{ HDC dc=(HDC)wp; HWND ctl=(HWND)lp;
        if(GetDlgCtrlID(ctl)==IDC_HELP_TEXT){ SetTextColor(dc,RGB(0,0,0)); SetBkColor(dc,RGB(255,255,255)); return (LRESULT)GetStockObject(WHITE_BRUSH); }
        SetBkMode(dc,TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_help=nullptr; return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void OpenHelp(){
    if(g_help){ SetForegroundWindow(g_help); return; }
    static bool reg=false;
    if(!reg){ WNDCLASSW wc={0}; wc.lpfnWndProc=HelpProc; wc.hInstance=g_inst; wc.lpszClassName=L"VdeHelp";
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
        wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
        RegisterClassW(&wc); reg=true; }
    int W=S(532),H=S(410);
    RECT wr={0,0,W,H}; AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2, sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;
    g_help=CreateWindowExW(0,L"VdeHelp",L"Help - Virtual Desktops Extension",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_help){ ShowWindow(g_help,SW_SHOW); SetForegroundWindow(g_help); }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_HOTKEY: ShowPicker(); return 0;
    case WM_PAINT:{ PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps); RECT cr; GetClientRect(hwnd,&cr); Paint(hdc,cr); EndPaint(hwnd,&ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: if((HWND)lp==g_search){ HDC dc=(HDC)wp; SetTextColor(dc,CLR_TEXT); SetBkColor(dc,CLR_SEARCH);
        static HBRUSH sbr=nullptr; if(!sbr)sbr=CreateSolidBrush(CLR_SEARCH); return (LRESULT)sbr; }
        return DefWindowProcW(hwnd,msg,wp,lp);
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
        if(g_clearBtn.right>g_clearBtn.left && PtInRect(&g_clearBtn,pt)){ SetWindowTextW(g_search,L""); g_searchActive=false; SetFocus(g_search); InvalidateRect(hwnd,nullptr,FALSE); return 0; }   // clear + deactivate border, keep caret
        { RECT cr; GetClientRect(hwnd,&cr); RECT sb=SearchBoxRect(cr.right); if(PtInRect(&sb,pt)){ g_searchActive=true; SetFocus(g_search); InvalidateRect(hwnd,nullptr,FALSE); return 0; } }   // clicked the search field -> activate
        if(g_searchActive){ g_searchActive=false; InvalidateRect(hwnd,nullptr,FALSE); }   // clicked outside the field -> deactivate border
        for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ Activate((int)i,ctrl); return 0; } return 0; }
    case WM_MOUSEMOVE:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        int hovRow=-1; for(size_t i=0;i<g_hoverRows.size();++i) if(PtInRect(&g_hoverRows[i].rc,pt)){ hovRow=(int)i; break; }
        if(hovRow>=0 && g_hoverRows[hovRow].trunc && g_tip){    // R9: full name on hover when truncated
            if(hovRow!=g_lastHoverRow){                        // update text/activate only on row change (avoid churn/lag)
                TOOLINFOW ti={0}; ti.cbSize=sizeof(ti); ti.hwnd=hwnd; ti.uId=1; ti.lpszText=(LPWSTR)g_hoverRows[hovRow].full.c_str();
                SendMessageW(g_tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&ti);
                SendMessageW(g_tip,TTM_TRACKACTIVATE,TRUE,(LPARAM)&ti);
                g_lastHoverRow=hovRow;
            }
            POINT sp=pt; ClientToScreen(hwnd,&sp); SendMessageW(g_tip,TTM_TRACKPOSITION,0,(LPARAM)MAKELONG(sp.x+S(16),sp.y+S(20)));
        } else if(g_lastHoverRow!=-1){ TipDeactivate(); g_lastHoverRow=-1; }
        for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ if(g_sel!=(int)i){g_sel=(int)i; InvalidateRect(hwnd,nullptr,FALSE);} break; }
        TRACKMOUSEEVENT tme={sizeof(tme)}; tme.dwFlags=TME_LEAVE; tme.hwndTrack=hwnd; TrackMouseEvent(&tme);
        return 0; }
    case WM_MOUSELEAVE: TipDeactivate(); g_lastHoverRow=-1; return 0;
    case WM_MOUSEWHEEL:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&pt); int delta=GET_WHEEL_DELTA_WPARAM(wp);   // R8: scroll a tile's window list
        for(auto& t:g_tiles) if(PtInRect(&t.rc,pt)){ t.scroll += (delta<0?1:-1); if(t.scroll<0)t.scroll=0; InvalidateRect(hwnd,nullptr,FALSE); break; }
        return 0; }
    case WM_COMMAND:                                            // R7: live-filter as the search text changes
        if(g_search && (HWND)lp==g_search && HIWORD(wp)==EN_CHANGE){
            int n=GetWindowTextLengthW(g_search); std::wstring s(n+1,0); GetWindowTextW(g_search,&s[0],n+1); s.resize(wcslen(s.c_str()));
            g_searchText=LowerW(s); if(!g_searchText.empty()) EnsureTabSearch(); InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    case WM_TIMER:
        if(wp==TIMER_STARTUP){                                // one-shot: startup settle finished
            KillTimer(hwnd,TIMER_STARTUP);
            if(g_autoFix && AnyAppPresent()){ int mv=0; std::string s=RunRestore(false,nullptr,&mv); if(mv>0)Balloon(U82W(s)); g_lastLayoutSig=LayoutSignature(); }   // no balloon if nothing moved
            return 0;
        }
        if(wp==TIMER_MONITOR && g_autoFix && !g_degraded){
            bool present = AnyAppPresent();
            LcAction a = LcOnTick(g_lc, present, LAUNCH_SETTLE_TICKS);
            if(a==LcAction::DoRestore){                        // app appeared & stabilized ⇒ restore
                int mv=0; std::string s=RunRestore(false,nullptr,&mv); if(mv>0)Balloon(U82W(s)); g_lastLayoutSig=LayoutSignature();   // no balloon if nothing moved
            } else if(a==LcAction::AutoSave){                  // steady state: track + save-on-change
                g_observedApps.insert("firefox");
                std::set<std::string> s; CollectPresentWindows(&s,nullptr); for(auto& k:s) g_seenKeys.insert(k);
                unsigned long long sig=LayoutSignature();
                if(sig!=g_lastLayoutSig){ g_lastLayoutSig=sig; RunSaveAuto(&g_seenKeys,&g_observedApps); }
            }
        }
        return 0;
    case WM_ACTIVATE: if(LOWORD(wp)==WA_INACTIVE)HidePicker(); return 0;
    case WM_SESSION_RESULT: {
        // Task 8 supplies generation/profile acceptance. Until then no requests
        // are submitted, but ownership is still consumed immediately and safely.
        std::unique_ptr<SessionResult> result((SessionResult*)lp);
        return 0;
    }
    case WM_TRAY:
        if(LOWORD(lp)==WM_RBUTTONUP){
            POINT pt; GetCursorPos(&pt); HMENU m=CreatePopupMenu();
            AppendMenuW(m,MF_STRING,200,L"Open desktop picker");
            AppendMenuW(m,MF_SEPARATOR,0,nullptr);
            AppendMenuW(m,MF_STRING,201,L"Save windows layout");
            AppendMenuW(m,MF_STRING,202,L"Restore saved windows layout");
            AppendMenuW(m,MF_STRING,204,L"Restore last auto saved layout");
            AppendMenuW(m,MF_SEPARATOR,0,nullptr);
            AppendMenuW(m,MF_STRING,203,L"Settings...");
            AppendMenuW(m,MF_STRING,206,L"Help...");
            AppendMenuW(m,MF_STRING,205,L"About...");
            AppendMenuW(m,MF_STRING,209,L"Exit");
            SetForegroundWindow(hwnd);
            int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,nullptr); DestroyMenu(m);
            if(cmd==200)ShowPicker();
            else if(cmd==201)Balloon(U82W(RunSaveManual()));
            else if(cmd==202)Balloon(U82W(RunRestore(true)));
            else if(cmd==204)Balloon(U82W(RunRestore(false)));
            else if(cmd==203)OpenSettings();
            else if(cmd==206)OpenHelp();
            else if(cmd==205)OpenAbout();
            else if(cmd==209)DestroyWindow(hwnd);
        } else if(LOWORD(lp)==WM_LBUTTONDBLCLK) ShowPicker();
        return 0;
    case WM_DESTROY:
        StopSessionWorker(hwnd); // join before the message-only owner disappears
        if(g_autoFix && !g_degraded && LcOnExit(g_lc, AnyAppPresent())==LcAction::FinalSave){   // save only if windows are open (R2)
            RunSaveAuto(&g_seenKeys,&g_observedApps);
            std::vector<DeskRec> dd; std::vector<LayoutWin> recs; std::string t;   // age grace counters by one run
            UnixSeconds nowUtc=UtcNowSeconds();
            if(ReadFileBytes(LayoutPath(false),t) && ParseLayout(t,dd,recs,nowUtc)){
                auto kept = ReconcileGrace(recs, g_seenKeys, g_observedApps, MISSING_RUNS_MAX);
                std::vector<DeskRec> desks; std::string error, snapshot;
                if(CurrentDesktops(desks,&error) && BuildCheckedLayoutSnapshot(desks,kept,nowUtc,snapshot,&error))
                    WriteTextFile(LayoutPath(false), snapshot);
            }
        }
        TrayRemove(); UnregisterHotKey(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

// ================================ entry ======================================
static std::wstring HotkeyString(){   // e.g. "Ctrl+Alt+D" from the current hotkey
    std::wstring s;
    if(g_hotMods&MOD_CONTROL)s+=L"Ctrl+";
    if(g_hotMods&MOD_ALT)s+=L"Alt+";
    if(g_hotMods&MOD_SHIFT)s+=L"Shift+";
    if(g_hotMods&MOD_WIN)s+=L"Win+";
    if((g_hotVk>='A'&&g_hotVk<='Z')||(g_hotVk>='0'&&g_hotVk<='9')) s+=(wchar_t)g_hotVk;
    else { wchar_t nm[32]={0}; if(GetKeyNameTextW((MapVirtualKeyW(g_hotVk,MAPVK_VK_TO_VSC)<<16),nm,32)>0)s+=nm; else s+=(wchar_t)g_hotVk; }
    return s;
}
static int RunGui(HINSTANCE hInst){
    g_inst=hInst;
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_HOTKEY_CLASS|ICC_STANDARD_CLASSES|ICC_LINK_CLASS|ICC_BAR_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&icc);
    InitMetrics();
    g_uiFont=CreateFontW(-S(12),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    LoadSettings();

    WNDCLASSEXW wc={0}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"VdeWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON));
    wc.hIconSm=LoadAppIcon(GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);
    g_main=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"VdeWindow",APP_NAME,WS_POPUP|WS_CLIPCHILDREN,0,0,400,300,nullptr,nullptr,hInst,nullptr);
    if(!g_main)return 3;
    try { g_sessionWorker.reset(new SessionWorker(g_main)); }
    catch(...) { g_sessionWorker.reset(); }
    { DWORD pref=2; DwmSetWindowAttribute(g_main,33,&pref,sizeof(pref)); }   // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (Win11)
    TrayAdd(g_main);
    bool hk=ApplyHotkey();
    if(!hk) MessageBoxW(nullptr,L"Could not register the global hotkey.\n"
                               L"Another app may be using it. Change it in Settings,\n"
                               L"or open the picker via double-click on the tray icon.",APP_NAME,MB_ICONWARNING);
    ApplyAutoFix();   // start the lifecycle monitor timer if enabled
    if(!g_degraded){ bool present=AnyAppPresent(); LcAction a=LcOnStartup(g_lc,present);   // R1: restore now if windows already open
      if(g_autoFix && a==LcAction::StartupRestore) SetTimer(g_main,TIMER_STARTUP,STARTUP_SETTLE_MS,nullptr); }
    std::wstring hkStr=HotkeyString(), tip;
    if(g_degraded)     tip=L"Running in limited mode (compatibility issue). See About for details.";
    else if(g_autoFix) tip=L"Running. Auto-restore is on. Press "+hkStr+L" to open the desktop picker.";
    else               tip=L"Running. Press "+hkStr+L" to move the active window to a desktop.";
    Balloon(tip);
    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){
        if(g_settings && IsDialogMessageW(g_settings,&msg)) continue;
        if(g_about && IsDialogMessageW(g_about,&msg)) continue;
        if(g_compat && IsDialogMessageW(g_compat,&msg)) continue;
        if(g_help && IsDialogMessageW(g_help,&msg)) continue;
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
    bool cli = (cmd==L"save"||cmd==L"restore"||cmd==L"restore-auto"||cmd==L"status"||cmd==L"list"||cmd==L"-h"||cmd==L"--help"||cmd==L"/?");

    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 1;

    MigrateLegacyLayout();   // rename legacy layout.txt -> layout-auto.txt (once)

    int rc;
    if(cli){
        if(AttachConsole(ATTACH_PARENT_PROCESS)){ FILE* f; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr); }
        SetConsoleOutputCP(CP_UTF8);
        if(!InitServices() || !SanityCheckServices()){ printf("Virtual-desktop services unavailable: a Windows update may have changed the undocumented interfaces. Please report your build (%lu) to info@conus.vision or https://github.com/conus-vision/win-vde\n",(unsigned long)GetWindowsBuild()); rc=3; }
        else { WriteLastGoodBuild(GetWindowsBuild()); rc=CliRun(cmd); }
    } else {
        g_inst=hInst; InitMetrics();
        bool good = InitServices() && SanityCheckServices();
        if(good){ WriteLastGoodBuild(GetWindowsBuild()); rc=RunGui(hInst); }
        else {                                              // R12: don't die silently — explain + degraded tray
            g_degraded=true;
            DWORD last=ReadLastGoodBuild();
            ShowCompatIssue(last!=0 && last!=GetWindowsBuild());
            rc=RunGui(hInst);
        }
    }
    ReleaseServices(); CoUninitialize();
    return rc;
}
