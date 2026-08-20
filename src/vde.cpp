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
#include <memory>
#include <limits>
#include <deque>

#include "str_util.hpp"   // W2U8/U82W, GUID helpers, b64, GetWindowsBuild, hostOf, etld1
#include "layout.hpp"     // DeskRec + v4 layout serialization/migration logic
#include "layout_store.hpp"
#include "lifecycle.hpp"
#include "move_queue.hpp"
#include "window_identity.hpp"
#include "picker_state.hpp"
#include "reconcile_worker.hpp"
#include "session.hpp"    // bounded browser-session decoding primitives
#include "appprofile.hpp" // AppProfile, BuiltinProfiles
#include "session_worker.hpp"
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
#define TIMER_MOVE_VERIFY 3
#define TIMER_HEARTBEAT 4
#define TIMER_AUTO_FLUSH 5
#define WM_MOVE_CANCEL_RETRY (WM_APP + 14)
#define WM_AUTO_TIMER_RETRY (WM_APP + 15)
#define MONITOR_INTERVAL_MS 5000
#define MOVE_VERIFY_INTERVAL_MS 150
#define AUTO_FLUSH_INTERVAL_MS 500
#define CONFLICT_RECHECK_INTERVAL_MS (60U * 1000U)
#define HEARTBEAT_INTERVAL_MS (6U * 60U * 60U * 1000U)
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

static HWND g_main=nullptr;
static void Balloon(const std::wstring& text);

struct RuntimeRecordBinding {
    std::string app;
    std::string recordId;
    WindowIdentityKey identity;
    uint64_t causalGeneration=0;
};

struct MoveRuntimeBinding {
    FastWin window;
    GUID destination={0};
    RestoreBudgetKey budgetKey;
    bool hasBudgetKey=false;
    bool issueAwaitingVerify=false;
    bool retireAfterVerify=false;
    bool cancelRequested=false;
    IdentityRecaptureRetryBudget identityRecaptureBudget;
    IssuedMoveRetirementTracker retirement;
};

struct ReservedAutoIdentity {
    MoveToken token;
    WindowIdentityKey identity;
    std::string app;
    std::string recordId;
    GUID originDesktop={0};
    LayoutWin provisionalOriginRecord;
    bool hasProvisionalOriginRecord=false;
};

enum class AsyncOperationOwner {
    AutoReconcile, ManualSave, ManualRestore, Search, MetadataProbe
};

struct SessionRoute {
    AsyncOperationOwner owner=AsyncOperationOwner::AutoReconcile;
    uint64_t operationId=0;
    std::string app;
    SessionPurpose purpose=SessionPurpose::MetadataProbe;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
    uint64_t deadlineMs=0;
};

struct AutoRestoreOperation {
    uint64_t operationId=0;
    std::string app;
    uint64_t lifecycleGeneration=0;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
    uint64_t windowSetSignature=0;
    uint64_t layoutSignature=0;
    uint64_t sourceSignature=0;
    uint64_t sessionRequestId=0;
    uint64_t sessionDataGeneration=0;
    ReconcileFreshness freshness=ReconcileFreshness::Fresh;
    bool reconcilePending=false;
    ReconcileWorkMode reconcileMode=ReconcileWorkMode::Plan;
    std::vector<FastWin> reconcileFast;
    std::unique_ptr<ReconcileResult> reconcile;
    MoveTerminalOutcomes successfulLive;
    std::set<uint64_t> liveJobIds;
    size_t outstanding=0;
    bool hadExhausted=false;
    bool hadFailure=false;
    bool cancellationPending=false;
};

struct PickerMoveOperation {
    uint64_t operationId=0;
    std::set<uint64_t> liveJobIds;
    std::string app;
    uint64_t lifecycleSaveGeneration=0;
    uint64_t lifecycleLayoutSignature=0;
    uint64_t lifecycleSessionSignature=0;
    bool completionReported=false;
};

struct SearchOperation {
    uint64_t operationId=0;
    size_t outstanding=0;
    std::map<std::string,AppFastSnapshot> snapshots;
    std::set<std::string> waitingReconcileApps;
};

struct MetadataProbeOperation {
    uint64_t operationId=0;
    std::string app;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
};

struct ManualSaveOperation {
    uint64_t operationId=0;
    OperationAppProfiles profiles;
    std::map<std::string,AppFastSnapshot> snapshots;
    std::vector<DeskRec> desktops;
    std::map<std::string,std::vector<LayoutWin> > preparedLive;
    std::set<std::string> waitingReconcileApps;
    size_t outstanding=0;
    bool failed=false;
    bool completionReported=false;
};

struct ManualMoveOperation {
    uint64_t operationId=0;
    OperationAppProfiles profiles;
    std::map<std::string,AppFastSnapshot> snapshots;
    std::vector<DeskRec> currentDesktops;
    std::vector<LayoutWin> saved;
    std::set<std::string> waitingSessionApps;
    std::set<std::string> waitingReconcileApps;
    std::set<uint64_t> liveJobIds;
    size_t outstanding=0;
    size_t succeeded=0;
    size_t failed=0;
    size_t already=0;
    bool completionReported=false;
    bool cancellationPending=false;
};

static std::vector<LayoutWin> g_autoRecords;
static std::vector<DeskRec> g_autoDesktops;
static std::map<std::string,LcState> g_lifecycleByApp;
static std::map<std::string,RuntimeRecordBinding> g_recordByRuntime;
static std::map<std::string,std::string> g_pendingRecordByRuntime;
static std::map<std::string,std::string> g_provisionalRecordByRuntime;
static std::map<std::string,ReservedAutoIdentity> g_reservedAutoIdentities;
static std::map<uint64_t,MoveRuntimeBinding> g_moveRuntime;
static std::map<uint64_t,SessionRoute> g_sessionRoutes;
static std::map<uint64_t,AutoRestoreOperation> g_pendingAutoOperations;
static std::map<uint64_t,PickerMoveOperation> g_pickerOperations;
static std::map<uint64_t,SearchOperation> g_searchOperations;
static std::map<uint64_t,MetadataProbeOperation> g_metadataProbeOperations;
static std::map<uint64_t,ManualSaveOperation> g_manualSaveOperations;
static std::map<uint64_t,ManualMoveOperation> g_manualMoveOperations;
static std::map<std::string,uint64_t> g_lastFreshSessionSignature;
static MoveQueue g_moveQueue;
static RestoreBudgets g_restoreBudgets;
static CheckpointController g_checkpointController;
static RuntimeQuiescenceState g_runtimeQuiescence;
static AsyncSessionRouteGate g_sessionRouteGate;
static AsyncReconcileDeadlineGate g_reconcileDeadlines;
static DirtyFlushController g_dirtyFlush;
static AutoLoadRetryState g_autoLoadRetry;
static bool g_autoDirty=false;
static bool g_autoLoaded=false;
static bool g_autoWritesAllowed=false;
static bool g_preserveBackupOnNextWrite=false;
static bool g_flushTimerArmed=false;
static uint64_t g_flushTimerDueMs=0;
static bool g_heartbeatTimerArmed=false;
static bool g_moveCancellationPending=false;
static MoveCancellationRetryState g_moveCancellationRetry;
static LayoutRevision g_autoRevision;
static LayoutPublishCandidate g_pendingAutoPublishCandidate;
static std::map<std::string,RecordDelta> g_dirtyRecordDeltas;
static std::map<std::string,ValidatedRecordTouch> g_validatedTouches;
static std::map<std::string,DeferredRecordConflict> g_deferredRecordConflicts;
static uint64_t g_nextSessionRequestId=0;
static uint64_t g_nextOperationId=0;
static uint64_t g_nextMoveJobId=0;
static std::unique_ptr<SessionWorker> g_sessionWorker;
static std::unique_ptr<ReconcileWorker> g_reconcileWorker;

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

// Browser-session decoding is owned by SessionWorker in GUI mode; CLI mode uses
// the same bounded reader directly because it has no window-message loop.
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
static void ReleaseServices(){
    if(g_vdmDoc){ g_vdmDoc->Release(); g_vdmDoc=nullptr; }
    if(g_avc){ g_avc->Release(); g_avc=nullptr; }
    if(g_vdmi){ g_vdmi->Release(); g_vdmi=nullptr; }
    if(g_shell){ g_shell->Release(); g_shell=nullptr; }
}
// After a Windows update the undocumented vtable/IIDs can shift: QueryService may
// still succeed but calls return garbage. Verify a couple of calls make sense.
static bool SanityCheckServices(){
    if(!DesktopServicesReady(g_vdmi!=nullptr,g_avc!=nullptr,g_vdmDoc!=nullptr))
        return false;
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

// ============================ Firefox windows =================================
static std::wstring StripSuffixes(std::wstring t, const std::vector<std::wstring>& sfx){
    for(auto& s:sfx){ size_t l=s.size(); if(t.size()>=l&&_wcsicmp(t.c_str()+(t.size()-l),s.c_str())==0){ t.resize(t.size()-l); return t; } }
    return t;
}

class UniqueWinHandle {
public:
    UniqueWinHandle()=default;
    explicit UniqueWinHandle(HANDLE handle):handle_(handle){}
    ~UniqueWinHandle(){ reset(); }
    UniqueWinHandle(const UniqueWinHandle&)=delete;
    UniqueWinHandle& operator=(const UniqueWinHandle&)=delete;
    UniqueWinHandle(UniqueWinHandle&& other) noexcept :handle_(other.release()){}
    UniqueWinHandle& operator=(UniqueWinHandle&& other) noexcept {
        if(this!=&other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_!=INVALID_HANDLE_VALUE; }
    HANDLE release(){ HANDLE value=handle_; handle_=nullptr; return value; }
    void reset(HANDLE value=nullptr){
        if(handle_ && handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_=value;
    }
private:
    HANDLE handle_=nullptr;
};

template<class T>
class ScopedComPtr {
public:
    ScopedComPtr()=default;
    explicit ScopedComPtr(T* value):value_(value){}
    ~ScopedComPtr(){ reset(); }
    ScopedComPtr(const ScopedComPtr&)=delete;
    ScopedComPtr& operator=(const ScopedComPtr&)=delete;
    ScopedComPtr(ScopedComPtr&& other) noexcept :value_(other.release()){}
    ScopedComPtr& operator=(ScopedComPtr&& other) noexcept {
        if(this!=&other) reset(other.release());
        return *this;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_!=nullptr; }
    T* release(){ T* value=value_; value_=nullptr; return value; }
    void reset(T* value=nullptr){ if(value_) value_->Release(); value_=value; }
private:
    T* value_=nullptr;
};

struct ProcessSnapshot {
    std::wstring image;
    uint64_t started=0;
};

static ProcessSnapshot ReadProcessSnapshot(DWORD pid){
    ProcessSnapshot value;
    std::vector<wchar_t> path;
    try { path.resize(32768,L'\0'); }
    catch(...) { return value; }
    UniqueWinHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if(!process) return value;
    DWORD pathSize=static_cast<DWORD>(path.size());
    if(QueryFullProcessImageNameW(process.get(),0,path.data(),&pathSize))
        value.image.assign(path.data(),pathSize);
    FILETIME created{},exited{},kernel{},user{};
    if(GetProcessTimes(process.get(),&created,&exited,&kernel,&user)){
        ULARGE_INTEGER ticks{};
        ticks.LowPart=created.dwLowDateTime;
        ticks.HighPart=created.dwHighDateTime;
        value.started=ticks.QuadPart;
    }
    return value;
}

static std::vector<AppProfile> ActiveProfiles(){
    return BuiltinProfiles(g_appFirefox,g_appChrome,g_appEdge);
}

static uint64_t ProfileConfigSignature(const AppProfile& profile){
    SnapshotSignatureBuilder signature;
    signature.addString(profile.id).addWideString(profile.exeName)
             .addUnsigned(static_cast<uint64_t>(profile.session))
             .addWideString(profile.userDataDir);
    for(const std::wstring& value : profile.classNames)
        signature.addWideString(value);
    for(const std::wstring& value : profile.titleSuffixes)
        signature.addWideString(value);
    return signature.value();
}

struct FastEnumContext {
    const std::vector<AppProfile>* profiles=nullptr;
    std::map<std::string,AppFastSnapshot>* snapshots=nullptr;
    std::map<DWORD,ProcessSnapshot> processes;
    bool allocationFailure=false;
};

static void MarkClassProfilesIncomplete(FastEnumContext& context,
                                        const wchar_t* className){
    for(const AppProfile& profile : *context.profiles)
        if(std::find(profile.classNames.begin(),profile.classNames.end(),className)!=
           profile.classNames.end())
            (*context.snapshots)[profile.id].enumerationComplete=false;
}

static BOOL CALLBACK EnumFastWindow(HWND hwnd,LPARAM parameter){
    FastEnumContext& context=*reinterpret_cast<FastEnumContext*>(parameter);
    try {
        if(!(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)) return TRUE;
        wchar_t className[64]={0};
        const int classLength=GetClassNameW(hwnd,className,64);
        if(!AcceptFastClassNameRead(
                classLength,*context.profiles,*context.snapshots)) return TRUE;
        bool trackedClass=false;
        for(const AppProfile& profile : *context.profiles)
            if(std::find(profile.classNames.begin(),profile.classNames.end(),className)!=
               profile.classNames.end()){
                trackedClass=true;
                break;
            }
        if(!trackedClass) return TRUE;

        DWORD pid=0;
        if(!GetWindowThreadProcessId(hwnd,&pid) || pid==0){
            MarkClassProfilesIncomplete(context,className);
            return TRUE;
        }
        auto process=context.processes.find(pid);
        if(process==context.processes.end())
            process=context.processes.emplace(pid,ReadProcessSnapshot(pid)).first;
        if(process->second.image.empty() || process->second.started==0){
            MarkClassProfilesIncomplete(context,className);
            return TRUE;
        }
        const AppProfile* profile=ClassifyBrowserCandidate(
            className,process->second.image,*context.profiles);
        if(!profile) return TRUE;

        int titleLength=GetWindowTextLengthW(hwnd);
        std::wstring title;
        if(titleLength>0){
            title.resize(static_cast<size_t>(titleLength)+1,L'\0');
            int copied=GetWindowTextW(hwnd,&title[0],titleLength+1);
            if(copied<=0){
                (*context.snapshots)[profile->id].enumerationComplete=false;
                return TRUE;
            }
            title.resize(static_cast<size_t>(copied));
        }
        FastWin window;
        window.app=profile->id;
        window.hwnd=hwnd;
        window.pid=pid;
        window.processStart=process->second.started;
        window.title=title;
        HRESULT desktopResult=g_vdmDoc
            ? g_vdmDoc->GetWindowDesktopId(hwnd,&window.desktop)
            : E_NOINTERFACE;
        if(FAILED(desktopResult) || GuidIsZero(window.desktop))
            (*context.snapshots)[profile->id].desktopLookupsComplete=false;
        (*context.snapshots)[profile->id].windows.push_back(std::move(window));
        return TRUE;
    } catch(...) {
        context.allocationFailure=true;
        for(auto& entry : *context.snapshots)
            entry.second.enumerationComplete=false;
        return FALSE;
    }
}

static std::map<std::string,AppFastSnapshot> CollectFastSnapshots(
        const std::vector<AppProfile>& profiles){
    static SnapshotVersionTracker versions;
    std::map<std::string,AppFastSnapshot> result;
    for(const AppProfile& profile : profiles) result[profile.id];
    FastEnumContext context;
    context.profiles=&profiles;
    context.snapshots=&result;
    if(!EnumWindows(EnumFastWindow,reinterpret_cast<LPARAM>(&context)) ||
       context.allocationFailure)
        for(auto& entry : result) entry.second.enumerationComplete=false;
    for(const AppProfile& profile : profiles)
        FinalizeFastSnapshot(profile.id,ProfileConfigSignature(profile),
                             versions,result[profile.id]);
    return result;
}

static std::map<std::string,AppFastSnapshot> CollectFastSnapshots(){
    return CollectFastSnapshots(ActiveProfiles());
}

static bool TryReadProcessStart(DWORD pid,uint64_t& started) noexcept {
    if(pid==0) return false;
    UniqueWinHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if(!process) return false;
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process.get(),&created,&exited,&kernel,&user))
        return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart=created.dwLowDateTime;
    ticks.HighPart=created.dwHighDateTime;
    if(ticks.QuadPart==0) return false;
    started=ticks.QuadPart;
    return true;
}

static WindowIdentityRecapture RecaptureGenericWindowIdentity(
        const WindowIdentityKey& expected) noexcept {
    if(expected.hwnd==0 || expected.pid==0 || expected.processStart==0)
        return WindowIdentityRecapture::Lost;
    HWND hwnd=reinterpret_cast<HWND>(expected.hwnd);
    if(!IsWindow(hwnd)) return WindowIdentityRecapture::Lost;
    DWORD pid=0;
    if(!GetWindowThreadProcessId(hwnd,&pid))
        return WindowIdentityRecapture::Indeterminate;
    if(pid!=expected.pid) return WindowIdentityRecapture::Lost;
    uint64_t started=0;
    if(!TryReadProcessStart(pid,started))
        return WindowIdentityRecapture::Indeterminate;
    return started==expected.processStart
        ? WindowIdentityRecapture::Match : WindowIdentityRecapture::Lost;
}

static bool CaptureGenericWindowIdentity(const WindowIdentityKey& expected,
                                          ProcessSnapshot* processOut=nullptr){
    if(RecaptureGenericWindowIdentity(expected)!=WindowIdentityRecapture::Match)
        return false;
    if(!processOut) return true;
    ProcessSnapshot process=ReadProcessSnapshot(expected.pid);
    if(process.started==0 || process.started!=expected.processStart) return false;
    *processOut=std::move(process);
    return true;
}

static PopupBrowserClassification ClassifyTrackedBrowserWindow(
        const WindowIdentityKey& expected,std::string& appOut) noexcept {
    try {
        ProcessSnapshot process;
        if(!CaptureGenericWindowIdentity(expected,&process) ||
           process.image.empty()) return PopupBrowserClassification::Failed;
        HWND hwnd=reinterpret_cast<HWND>(expected.hwnd);
        wchar_t className[64]={0};
        if(GetClassNameW(hwnd,className,64)<=0)
            return PopupBrowserClassification::Failed;
        std::vector<AppProfile> profiles=ActiveProfiles();
        const AppProfile* profile=ClassifyBrowserCandidate(
            className,process.image,profiles);
        if(!profile) return PopupBrowserClassification::NotTracked;
        std::string app=profile->id;
        appOut.swap(app);
        return PopupBrowserClassification::Tracked;
    } catch(...) { return PopupBrowserClassification::Failed; }
}
// ============================ snapshot storage ================================
static UnixSeconds UtcNowSeconds(){
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ticks{}; ticks.LowPart=ft.dwLowDateTime; ticks.HighPart=ft.dwHighDateTime;
    const ULONGLONG WINDOWS_TO_UNIX_EPOCH=116444736000000000ULL;
    if(ticks.QuadPart<WINDOWS_TO_UNIX_EPOCH)return 0;
    return (UnixSeconds)((ticks.QuadPart-WINDOWS_TO_UNIX_EPOCH)/10000000ULL);
}
static uint64_t MonotonicNowMs(){ return static_cast<uint64_t>(GetTickCount64()); }
static uint64_t TakeNonzeroId(uint64_t& counter){
    if(counter==(std::numeric_limits<uint64_t>::max)()) counter=0;
    ++counter;
    return counter;
}
static std::wstring DataDir(){
    wchar_t base[MAX_PATH]={0}; GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    std::wstring dir=std::wstring(base)+L"\\VirtualDesktopsExtention"; CreateDirectoryW(dir.c_str(),nullptr); return dir;
}
static std::wstring LayoutPath(bool manual){ return DataDir()+(manual?L"\\layout-manual.txt":L"\\layout-auto.txt"); }
static void ReportStorageError(const std::wstring& message);
static bool MigrateLegacyLayout(){
    LegacyLayoutMigrationResult migration=MigrateLegacyLayout(
        DataDir()+L"\\layout.txt",LayoutPath(false),UtcNowSeconds());
    if(!migration.succeeded())
        ReportStorageError(L"Legacy layout migration could not be completed; the original file was preserved and startup will retry.");
    return migration.succeeded();
}
static bool CurrentDesktops(std::vector<DeskRec>& desksOut, std::string* errorOut=nullptr){
    auto fail=[&](const std::string& message)->bool{ if(errorOut)*errorOut=message; return false; };
    if(!g_vdmi)return fail("virtual desktop manager is unavailable");
    UINT count=0; if(FAILED(g_vdmi->GetCount(&count)))return fail("failed to get virtual desktop count");
    if(count==0 || count>MAX_LAYOUT_RECORDS)return fail("invalid virtual desktop count");
    std::vector<DeskRec> desks;
    try { desks.reserve(count); }
    catch(...) { return fail("out of memory collecting virtual desktops"); }
    for(UINT i=0;i<count;++i){
        ScopedComPtr<IVirtualDesktop> desktop(GetDesktopByIndex(i));
        if(!desktop)return fail("failed to get virtual desktop");
        GUID g={0}; HRESULT idHr=desktop->GetID(&g);
        if(FAILED(idHr)||GuidIsZero(g)) return fail("failed to get virtual desktop GUID");
        try {
            DeskRec record; record.index=(int)i; record.guid=g;
            record.name=DesktopNameFromRegistry(g); desks.push_back(std::move(record));
        } catch(...) { return fail("out of memory collecting virtual desktops"); }
    }
    desksOut.swap(desks); if(errorOut)errorOut->clear(); return true;
}

static void ReportStorageError(const std::wstring& message){
    const uint64_t now=MonotonicNowMs();
    std::string key;
    try { key=W2U8(message); }
    catch(...) { key="storage-error"; }
    if(g_dirtyFlush.shouldReportError(key,now) && g_main) Balloon(message);
}

static void ScheduleAutoFlush(UINT delayMs=AUTO_FLUSH_INTERVAL_MS){
    if(!g_main || !g_autoDirty || !g_dirtyFlush.dirty() || !g_autoLoaded ||
       !g_autoWritesAllowed || !g_autoFix || g_degraded) return;
    const uint64_t now=MonotonicNowMs();
    const uint64_t requestedDue=now>(std::numeric_limits<uint64_t>::max)()-delayMs
        ? (std::numeric_limits<uint64_t>::max)() : now+delayMs;
    if(g_flushTimerArmed && g_flushTimerDueMs!=0 &&
       g_flushTimerDueMs<=requestedDue) return;
    if(SetTimer(g_main,TIMER_AUTO_FLUSH,delayMs,nullptr)){
        g_flushTimerArmed=true;
        g_flushTimerDueMs=requestedDue;
        return;
    }
    g_flushTimerArmed=false;
    g_flushTimerDueMs=0;
    ReportStorageError(L"Automatic layout flush timer could not be started; final checkpoints remain enabled.");
}

static void MaintainAutoFlushTimer() noexcept {
    try {
        const bool eligible=g_main && g_autoLoaded && g_autoWritesAllowed &&
            g_autoFix && !g_degraded;
        if(ShouldMaintainDirtyFlush(
                eligible,g_autoDirty && g_dirtyFlush.dirty(),
                g_flushTimerArmed))
            ScheduleAutoFlush();
    } catch(...) {}
}

static void MarkAutoDirty(bool schedule=true){
    g_autoDirty=true;
    g_dirtyFlush.markDirty(MonotonicNowMs());
    if(schedule) ScheduleAutoFlush();
}

static bool QueueRecordDelta(RecordDeltaKind kind,const LayoutWin* before,
                             const LayoutWin& desired,bool erase,
                             UnixSeconds changedUtc,uint64_t causalGeneration);

static bool LoadAutoLayout(){
    const UnixSeconds nowUtc=UtcNowSeconds();
    LayoutLoadResult loaded=LoadLayoutWithBackup(LayoutPath(false),nowUtc);
    if(loaded.status==LayoutLoadStatus::Unavailable){
        g_autoWritesAllowed=false;
        ReportStorageError(L"The saved layout is temporarily unavailable; automatic changes are paused and will retry.");
        return false;
    }

    std::vector<DeskRec> desktops;
    std::vector<LayoutWin> records;
    std::vector<LayoutWin> expiredRecords;
    if(loaded.status==LayoutLoadStatus::Valid ||
       loaded.status==LayoutLoadStatus::Recovered){
        ExpiredLayoutPartition partition;
        if(!PartitionExpiredLayoutRecords(loaded.wins,nowUtc,partition)){
            g_autoWritesAllowed=false;
            ReportStorageError(L"The saved layout could not be prepared in memory; automatic changes are paused and will retry.");
            return false;
        }
        try { desktops=loaded.desks; }
        catch(...) {
            g_autoWritesAllowed=false;
            ReportStorageError(L"The desktop snapshot could not be prepared in memory; automatic changes are paused and will retry.");
            return false;
        }
        records.swap(partition.retained);
        expiredRecords.swap(partition.expired);
    }
    g_autoDesktops.swap(desktops);
    g_autoRecords.swap(records);
    g_autoWritesAllowed=loaded.writesAllowed;
    SwapLayoutRevisionNoThrow(g_autoRevision,loaded.revision);
    g_preserveBackupOnNextWrite=loaded.status==LayoutLoadStatus::Recovered;
    g_autoDirty=loaded.status==LayoutLoadStatus::Recovered ||
                loaded.status==LayoutLoadStatus::CorruptPreserved ||
                ((loaded.status==LayoutLoadStatus::Valid) && loaded.sourceVersion<4);
    g_dirtyRecordDeltas.clear();
    g_validatedTouches.clear();
    g_deferredRecordConflicts.clear();
    ClearLayoutPublishCandidateNoThrow(g_pendingAutoPublishCandidate);
    g_dirtyFlush.clearDirty();
    if(g_autoDirty) g_dirtyFlush.markDirty(MonotonicNowMs());
    if(!expiredRecords.empty()){
        const uint64_t generation=TakeNonzeroId(g_nextOperationId);
        for(const LayoutWin& expired : expiredRecords)
            QueueRecordDelta(RecordDeltaKind::ExpireDelete,&expired,expired,
                             true,nowUtc,generation);
    }
    if(loaded.status==LayoutLoadStatus::Recovered)
        ReportStorageError(L"The primary layout was invalid; VDE restored the valid backup.");
    else if(loaded.status==LayoutLoadStatus::CorruptPreserved)
        ReportStorageError(L"Invalid layout copies were preserved for diagnostics; VDE started with an empty layout.");
    if(g_autoDirty) ScheduleAutoFlush();
    return true;
}

static std::map<std::string,uint64_t> CurrentCausalGenerations(){
    std::map<std::string,uint64_t> generations;
    for(const auto& entry : g_recordByRuntime){
        const RuntimeRecordBinding& binding=entry.second;
        auto found=generations.find(binding.recordId);
        if(found==generations.end() || found->second<binding.causalGeneration)
            generations[binding.recordId]=binding.causalGeneration;
    }
    return generations;
}

static const LayoutWin* FindAutoRecord(const std::string& recordId){
    for(const LayoutWin& record : g_autoRecords)
        if(record.recordId==recordId) return &record;
    return nullptr;
}

static bool QueueRecordDelta(RecordDeltaKind kind,const LayoutWin* before,
                             const LayoutWin& desired,bool erase,
                             UnixSeconds changedUtc,uint64_t causalGeneration){
    RecordDelta next;
    next.kind=kind;
    next.erase=erase;
    next.record=desired;
    next.baseRevision=g_autoRevision;
    next.baseRecordPresent=before!=nullptr;
    if(before) next.baseRecord=*before;
    next.changedUtc=changedUtc;
    next.causalGeneration=causalGeneration;
    const bool causalAccepted=kind==RecordDeltaKind::ExplicitUpsert ||
        kind==RecordDeltaKind::ValidatedRuntimeUpsert;
    std::vector<LayoutWin> stagedRecords;
    std::map<std::string,RecordDelta> stagedDeltas;
    std::map<std::string,DeferredRecordConflict> stagedConflicts;
    const RecordDeltaStageResult result=StageRecordDeltaMutation(
        g_autoRecords,g_dirtyRecordDeltas,g_deferredRecordConflicts,next,
        causalAccepted,stagedRecords,stagedDeltas,stagedConflicts);
    if(result!=RecordDeltaStageResult::Accepted){
        g_dirtyFlush.setConflict(!g_deferredRecordConflicts.empty(),
                                 MonotonicNowMs());
        if(result==RecordDeltaStageResult::DeferredConflict) MarkAutoDirty();
        return false;
    }
    g_autoRecords.swap(stagedRecords);
    g_dirtyRecordDeltas.swap(stagedDeltas);
    g_deferredRecordConflicts.swap(stagedConflicts);
    g_dirtyFlush.setConflict(!g_deferredRecordConflicts.empty(),
                             MonotonicNowMs());
    MarkAutoDirty();
    return true;
}

static bool PersistAutoLayout(){
    if(!g_autoLoaded || !g_autoWritesAllowed){ g_autoDirty=true; return false; }
    ScopedLayoutLock lock;
    if(!lock.acquired()){ g_autoDirty=true; return false; }
    const std::wstring path=LayoutPath(false);
    LayoutRevisionReadResult currentRead=
        ReadLayoutRevisionObservationLocked(path);
    LayoutRevision& current=currentRead.revision;
    const LayoutPublishCandidateObservation candidateObservation=
        ObserveLayoutPublishCandidateNoThrow(
            g_pendingAutoPublishCandidate,currentRead.status,current,
            g_autoRevision);
    if(currentRead.status==LayoutRevisionReadStatus::Unavailable ||
       candidateObservation==
           LayoutPublishCandidateObservation::RetainedUnavailable){
        g_autoDirty=true;
        return false;
    }
    const bool adoptedArmedCandidate=candidateObservation==
        LayoutPublishCandidateObservation::Adopted;
    const LayoutRevision& observedRevision=adoptedArmedCandidate
        ? g_autoRevision : current;
    if(DeferredRecordConflictsBlockPublish(
            g_deferredRecordConflicts,observedRevision)){
        g_autoDirty=true;
        g_dirtyFlush.setConflict(true,MonotonicNowMs());
        return false;
    }

    if(!adoptedArmedCandidate && !SameRevision(current,g_autoRevision)){
        LayoutLoadResult latest=LoadLayoutWithBackupLocked(path,UtcNowSeconds());
        if(!latest.usable()){
            g_autoDirty=true;
            return false;
        }
        g_preserveBackupOnNextWrite=PreserveExistingBackupForPublish(
            g_preserveBackupOnNextWrite,latest.status);
        const std::map<std::string,uint64_t> causal=CurrentCausalGenerations();
        RecordDeltaRebasePreparation preparedDeltas;
        if(!PrepareRecordDeltasForRebase(
                latest.wins,g_dirtyRecordDeltas,causal,preparedDeltas)){
            g_autoDirty=true;
            return false;
        }
        std::set<std::string> nextConflicts;
        nextConflicts.swap(preparedDeltas.deferredRecordIds);
        RebaseResult rebased=RebaseRecordDeltas(
            latest.wins,latest.revision,preparedDeltas.deltas,UtcNowSeconds());
        nextConflicts.insert(
            rebased.deferredConflictRecordIds.begin(),
            rebased.deferredConflictRecordIds.end());
        std::map<std::string,ValidatedRecordTouch> touchesForRebase;
        if(!PrepareValidatedTouchesForRebase(
                g_validatedTouches,rebased.appliedDeleteRecordIds,
                touchesForRebase)){
            g_autoDirty=true;
            return false;
        }
        TouchRebaseResult touched=ReapplyValidatedTouches(
            rebased.records,touchesForRebase,causal);
        nextConflicts.insert(
            touched.deferredRecordIds.begin(),touched.deferredRecordIds.end());
        RebasedResidualJournal residual;
        if(!BuildRebasedResidualJournal(
                g_dirtyRecordDeltas,g_validatedTouches,preparedDeltas,
                rebased,touched,latest.wins,latest.revision,residual)){
            g_autoDirty=true;
            return false;
        }
        RebasedAutoLayoutPublication publication;
        if(!BuildRebasedAutoLayoutPublication(
                touched.records,latest.desks,latest.revision,nextConflicts,
                residual.deltas,residual.touches,publication)){
            g_autoDirty=true;
            return false;
        }
        // All adopted-disk metadata is staged against the same B snapshot.
        // Publication is a no-throw group of swaps; no failure can expose a
        // new revision with conflict metadata copied from the old A state.
        g_autoRecords.swap(publication.records);
        g_autoDesktops.swap(publication.desktops);
        SwapLayoutRevisionNoThrow(g_autoRevision,publication.revision);
        g_dirtyRecordDeltas.swap(publication.deltas);
        g_validatedTouches.swap(publication.touches);
        g_deferredRecordConflicts.swap(publication.conflicts);
        if(!g_deferredRecordConflicts.empty()){
            g_autoDirty=true;
            g_dirtyFlush.setConflict(true,MonotonicNowMs());
            return false;
        }
    }

    std::vector<DeskRec> desktops;
    std::string error,text;
    if(!CurrentDesktops(desktops,&error)){
        g_autoDirty=true;
        ReportStorageError(L"Could not collect virtual desktops; automatic layout remains pending.");
        return false;
    }
    std::vector<LayoutWin> checkedRecords=g_autoRecords;
    if(!BuildCheckedLayoutSnapshot(
            desktops,checkedRecords,UtcNowSeconds(),text,&error)){
        g_autoDirty=true;
        ReportStorageError(L"Automatic layout validation failed; the previous copy was kept.");
        return false;
    }
    LayoutPublishCandidate candidate;
    if(!BuildLayoutPublishCandidate(path,text,candidate)){
        g_autoDirty=true;
        ReportStorageError(L"Automatic layout publication could not be prepared in memory; the previous copy was kept.");
        return false;
    }
    SwapLayoutPublishCandidateNoThrow(
        g_pendingAutoPublishCandidate,candidate);
    const CapturedLayoutPublishResult published=
      PublishLayoutWithCapturedRevision(
        [&](LayoutRevision& revision){
            return AtomicWriteText(
                path,text,&error,g_preserveBackupOnNextWrite,&revision);
        },
        [&](LayoutRevision& revision) noexcept {
            // The primary already contains this exact candidate, but a later
            // backup/cleanup step failed.  Adopt only its verified revision;
            // retain records, deltas and dirty state so the next idempotent
            // attempt settles storage without rebasing against our own write.
            CommitPublishedLayoutRevisionNoThrow(g_autoRevision,revision);
            ClearLayoutPublishCandidateNoThrow(
                g_pendingAutoPublishCandidate);
            g_autoDirty=true;
        },
        [&](LayoutRevision& revision) noexcept {
            g_autoRecords.swap(checkedRecords);
            g_autoDesktops.swap(desktops);
            CommitPublishedLayoutRevisionNoThrow(g_autoRevision,revision);
            g_preserveBackupOnNextWrite=false;
            g_dirtyRecordDeltas.clear();
            g_validatedTouches.clear();
            g_deferredRecordConflicts.clear();
            ClearLayoutPublishCandidateNoThrow(
                g_pendingAutoPublishCandidate);
            g_autoDirty=false;
        });
    if(published!=CapturedLayoutPublishResult::Succeeded){
        g_autoDirty=true;
        ReportStorageError(L"Could not save the automatic layout; the previous copy was kept.");
        return false;
    }
    return true;
}

static bool FlushAutoLayout(bool force){
    const uint64_t now=MonotonicNowMs();
    g_dirtyFlush.setConflict(!g_deferredRecordConflicts.empty(),now);
    const DirtyFlushResult result=g_dirtyFlush.flush(
        now,force,[](){ return PersistAutoLayout(); });
    if(result==DirtyFlushResult::Succeeded ||
       result==DirtyFlushResult::NotDirty ||
       result==DirtyFlushResult::Cleared){
        g_autoDirty=g_dirtyFlush.dirty();
        if(g_autoDirty) ScheduleAutoFlush();
        return !g_autoDirty;
    }
    if(result==DirtyFlushResult::SucceededDirtyAgain){
        g_autoDirty=true;
        ScheduleAutoFlush();
        return false;
    }
    if(result==DirtyFlushResult::Failed){
        g_autoDirty=true;
        ScheduleAutoFlush(g_dirtyFlush.conflicted()
            ? CONFLICT_RECHECK_INTERVAL_MS : 5000);
        return false;
    }
    if(result==DirtyFlushResult::Deferred){
        const uint64_t due=g_dirtyFlush.dueAtMs();
        UINT delay=AUTO_FLUSH_INTERVAL_MS;
        if(due>now){
            const uint64_t remaining=due-now;
            delay=static_cast<UINT>((std::min<uint64_t>)(
                remaining,(std::numeric_limits<UINT>::max)()));
            if(delay==0) delay=1;
        }
        ScheduleAutoFlush(delay);
    } else if(result==DirtyFlushResult::ConflictSuppressed){
        ScheduleAutoFlush(CONFLICT_RECHECK_INTERVAL_MS);
    }
    return false;
}

static const AppProfile* FindActiveProfile(const std::string& app,
                                           std::vector<AppProfile>& storage){
    storage=ActiveProfiles();
    for(const AppProfile& profile : storage)
        if(profile.id==app) return &profile;
    return nullptr;
}

static int SnapshotDesktopIndex(const GUID& guid){
    for(const DeskRec& desktop : g_autoDesktops)
        if(GuidEq(desktop.guid,guid)) return desktop.index;
    return -1;
}

static uint64_t SessionSourceSignature(const SessionResult& result){
    SnapshotSignatureBuilder signature;
    signature.addWideString(result.path)
        .addUnsigned(result.sourceStampKnown ? 1 : 0);
    if(result.sourceStampKnown){
        signature.addUnsigned(result.sourceStamp.size)
            .addUnsigned(result.sourceStamp.mtime)
            .addUnsigned(result.sourceStamp.changeTime)
            .addUnsigned(result.sourceStamp.fileIdHigh)
            .addUnsigned(result.sourceStamp.fileIdLow)
            .addUnsigned(result.sourceStamp.volumeSerial);
    }
    return signature.value();
}

// CLI commands have no message loop; they may run the same bounded preparation
// synchronously. GUI paths always queue it through ReconcileWorker.
static ReconcileRequest MakeCliLiveRequest(
        const AppProfile& profile,const AppFastSnapshot& snapshot,
        const std::shared_ptr<const std::vector<WinFp> >& sessionWindows,
        const std::vector<DeskRec>& desktops){
    ReconcileRequest request;
    request.app=profile.id;
    request.buildLiveFromInputs=true;
    request.fastWindows=snapshot.windows;
    request.sessionWindows=sessionWindows;
    request.desktops=desktops;
    request.titleSuffixes=profile.titleSuffixes;
    return request;
}

static void ConfigureWorkerLiveBuild(
        ReconcileRequest& request,ReconcileWorkMode mode,
        const AppProfile& profile,const AppFastSnapshot& snapshot,
        const std::shared_ptr<const std::vector<WinFp> >& sessionWindows,
        const std::vector<DeskRec>& desktops){
    request.workMode=mode;
    request.buildLiveFromInputs=true;
    request.fastWindows=snapshot.windows;
    request.sessionWindows=sessionWindows;
    request.desktops=desktops;
    request.titleSuffixes=profile.titleSuffixes;
}

static bool SameRecordSemantic(const LayoutWin& left,const LayoutWin& right){
    return left.recordId==right.recordId && left.app==right.app &&
        left.deskIndex==right.deskIndex && GuidEq(left.desktop,right.desktop) &&
        left.activeTitle==right.activeTitle &&
        left.activeDomain==right.activeDomain && left.tabCount==right.tabCount &&
        left.counts==right.counts &&
        left.missingSinceUtc==right.missingSinceUtc &&
        left.provisional==right.provisional;
}

static void RememberValidatedTouch(const LayoutWin& record,
                                   uint64_t causalGeneration){
    ValidatedRecordTouch touch;
    touch.recordId=record.recordId;
    touch.lastSeenUtc=record.lastSeenUtc;
    touch.causalGeneration=causalGeneration;
    auto found=g_validatedTouches.find(touch.recordId);
    if(found==g_validatedTouches.end() ||
       found->second.lastSeenUtc<touch.lastSeenUtc)
        g_validatedTouches[touch.recordId]=touch;
}

static bool UpsertAutoRecord(const LayoutWin& desired,RecordDeltaKind kind,
                             UnixSeconds changedUtc,uint64_t causalGeneration){
    for(LayoutWin& current : g_autoRecords){
        if(current.recordId!=desired.recordId) continue;
        const LayoutWin before=current;
        if(!SameRecordSemantic(before,desired)){
            if(!QueueRecordDelta(kind,&before,desired,false,changedUtc,
                                 causalGeneration)) return false;
        } else {
            current=desired;
        }
        RememberValidatedTouch(desired,causalGeneration);
        return true;
    }
    if(g_autoRecords.size()>=MAX_LAYOUT_RECORDS) return false;
    if(!QueueRecordDelta(kind,nullptr,desired,false,changedUtc,
                         causalGeneration)) return false;
    RememberValidatedTouch(desired,causalGeneration);
    return true;
}

static bool EraseAutoRecord(const LayoutWin& previous,RecordDeltaKind kind,
                            UnixSeconds changedUtc,uint64_t causalGeneration){
    for(size_t index=0;index<g_autoRecords.size();++index){
        if(g_autoRecords[index].recordId!=previous.recordId) continue;
        const LayoutWin before=g_autoRecords[index];
        if(!QueueRecordDelta(kind,&before,before,true,changedUtc,
                             causalGeneration)) return false;
        g_validatedTouches.erase(previous.recordId);
        return true;
    }
    return false;
}

static bool PersistPickerMovedWindow(const MoveResult& result,
                                     const MoveRuntimeBinding& runtime,
                                     const std::string& app){
    try {
        const WindowIdentityKey identity=IdentityOf(runtime.window);
        const std::string runtimeKey=RuntimeKey(identity);
        auto reserved=g_reservedAutoIdentities.find(runtimeKey);
        if(reserved==g_reservedAutoIdentities.end() ||
           !SameMoveToken(reserved->second.token,result.token) ||
           !SameIdentity(reserved->second.identity,identity) ||
           app.empty() || GuidIsZero(runtime.destination)) return false;

        const int deskIndex=GetDesktopIndexByGuid(runtime.destination);
        if(deskIndex<0) return false;
        const UnixSeconds nowUtc=UtcNowSeconds();
        if(nowUtc<=0 || result.token.operationId==0) return false;

        std::string recordId;
        if(!SelectPopupPersistRecordId(
                reserved->second.recordId,
                [&](std::string& selected){
                    return SelectPendingPopupRecordId(
                        identity,app,g_pendingRecordByRuntime,
                        [&](const std::string& candidate,
                            const std::string& candidateApp,
                            std::string& canonical){
                            GUID parsed{};
                            if(!ParseNonzeroLayoutGuid(
                                    candidate,parsed,&canonical)) return false;
                            for(const LayoutWin& saved : g_autoRecords){
                                GUID savedGuid{};
                                std::string savedCanonical;
                                if(ParseNonzeroLayoutGuid(
                                        saved.recordId,savedGuid,
                                        &savedCanonical) &&
                                   savedCanonical==canonical &&
                                   saved.app==candidateApp) return true;
                            }
                            return false;
                        },selected);
                },
                [&](std::string& selected){
                    selected=NewRecordId();
                    return !selected.empty();
                },recordId)) return false;
        GUID parsedId{};
        std::string canonicalId;
        if(!ParseNonzeroLayoutGuid(recordId,parsedId,&canonicalId)) return false;
        recordId.swap(canonicalId);

        const LayoutWin* before=nullptr;
        for(const LayoutWin& current : g_autoRecords)
            if(current.recordId==recordId){ before=&current; break; }
        if(before && before->app!=app) return false;

        LayoutWin desired;
        if(before) desired=*before;
        else if(reserved->second.hasProvisionalOriginRecord &&
                reserved->second.provisionalOriginRecord.recordId==recordId &&
                reserved->second.provisionalOriginRecord.app==app)
            desired=reserved->second.provisionalOriginRecord;
        else {
            desired.recordId=recordId;
            desired.app=app;
            desired.activeTitle=W2U8(runtime.window.title);
            desired.provisional=true;
        }
        desired.recordId=recordId;
        desired.app=app;
        desired.desktop=runtime.destination;
        desired.deskIndex=deskIndex;
        if(!before) desired.provisional=true;
        MarkSeen(desired,nowUtc);

        RecordDelta delta;
        delta.kind=RecordDeltaKind::ExplicitUpsert;
        delta.record=desired;
        delta.baseRevision=g_autoRevision;
        delta.baseRecordPresent=before!=nullptr;
        if(before) delta.baseRecord=*before;
        delta.changedUtc=nowUtc;
        delta.causalGeneration=result.token.operationId;
        std::vector<LayoutWin> stagedRecords;
        std::map<std::string,RecordDelta> stagedDeltas;
        std::map<std::string,DeferredRecordConflict> stagedConflicts;
        if(StageRecordDeltaMutation(
                g_autoRecords,g_dirtyRecordDeltas,g_deferredRecordConflicts,
                delta,true,stagedRecords,stagedDeltas,stagedConflicts)!=
           RecordDeltaStageResult::Accepted) return false;

        std::map<std::string,ValidatedRecordTouch> stagedTouches=
            g_validatedTouches;
        ValidatedRecordTouch touch;
        touch.recordId=recordId;
        touch.lastSeenUtc=desired.lastSeenUtc;
        touch.causalGeneration=result.token.operationId;
        auto previousTouch=stagedTouches.find(recordId);
        if(previousTouch==stagedTouches.end() ||
           previousTouch->second.lastSeenUtc<touch.lastSeenUtc)
            stagedTouches[recordId]=touch;

        std::map<std::string,RuntimeRecordBinding> stagedBindings=
            g_recordByRuntime;
        RuntimeRecordBinding binding;
        binding.app=app;
        binding.recordId=recordId;
        binding.identity=identity;
        binding.causalGeneration=result.token.operationId;
        stagedBindings[runtimeKey]=binding;
        std::map<std::string,std::string> stagedPending=
            g_pendingRecordByRuntime;
        std::map<std::string,std::string> stagedProvisional=
            g_provisionalRecordByRuntime;
        stagedPending.erase(runtimeKey);
        stagedProvisional.erase(runtimeKey);

        // Staging may allocate.  Recheck the full HWND/PID/process-start
        // identity at the no-throw publication boundary so a reused HWND can
        // never inherit the browser record prepared for its predecessor.
        if(RecaptureGenericWindowIdentity(identity)!=
           WindowIdentityRecapture::Match) return false;
        g_autoRecords.swap(stagedRecords);
        g_dirtyRecordDeltas.swap(stagedDeltas);
        g_deferredRecordConflicts.swap(stagedConflicts);
        g_validatedTouches.swap(stagedTouches);
        g_recordByRuntime.swap(stagedBindings);
        g_pendingRecordByRuntime.swap(stagedPending);
        g_provisionalRecordByRuntime.swap(stagedProvisional);
        g_dirtyFlush.setConflict(!g_deferredRecordConflicts.empty(),
                                 MonotonicNowMs());
        MarkAutoDirty(false);
        return FlushAutoLayout(true);
    } catch(...) { return false; }
}

static void MarkAppMissingFromLastSeen(const std::string& app,UnixSeconds nowUtc,
                                       uint64_t causalGeneration){
    if(causalGeneration==0) return;
    for(size_t index=g_autoRecords.size();index>0;--index){
        LayoutWin before=g_autoRecords[index-1];
        if(before.app!=app) continue;
        LayoutWin after=before;
        MarkMissing(after,nowUtc);
        if(IsExpired(after,nowUtc)){
            EraseAutoRecord(before,RecordDeltaKind::ExpireDelete,nowUtc,
                            causalGeneration);
        } else if(!SameRecordSemantic(before,after)){
            QueueRecordDelta(RecordDeltaKind::MissingMark,&before,after,false,
                             nowUtc,causalGeneration);
        }
    }
}

static void PruneStaleRuntimeState(
        const std::map<std::string,AppFastSnapshot>& snapshots){
    std::map<std::string,WindowIdentityKey> live;
    std::set<std::string> completeApps;
    bool allComplete=!snapshots.empty();
    try {
        for(const auto& app : snapshots){
            if(!app.second.enumerationComplete){ allComplete=false; continue; }
            completeApps.insert(app.first);
            for(const FastWin& window : app.second.windows)
                live[RuntimeKey(window)]=IdentityOf(window);
        }
    } catch(...) { return; }
    auto reserved=[](const std::string& runtime){
        return g_reservedAutoIdentities.count(runtime)!=0;
    };
    for(auto it=g_recordByRuntime.begin();it!=g_recordByRuntime.end();){
        if(completeApps.count(it->second.app)==0 || reserved(it->first)){
            ++it; continue;
        }
        auto found=live.find(it->first);
        if(found==live.end() || !SameIdentity(found->second,it->second.identity)){
            g_pendingRecordByRuntime.erase(it->first);
            it=g_recordByRuntime.erase(it);
        } else ++it;
    }
    auto staleStateKey=[&](const std::string& runtime,
                           const std::string& recordId){
        if(live.count(runtime)!=0 || reserved(runtime)) return false;
        const LayoutWin* record=FindAutoRecord(recordId);
        if(record) return completeApps.count(record->app)!=0;
        return allComplete;
    };
    for(auto it=g_pendingRecordByRuntime.begin();
        it!=g_pendingRecordByRuntime.end();){
        if(staleStateKey(it->first,it->second))
            it=g_pendingRecordByRuntime.erase(it);
        else ++it;
    }
    for(auto it=g_provisionalRecordByRuntime.begin();
        it!=g_provisionalRecordByRuntime.end();){
        if(staleStateKey(it->first,it->second))
            it=g_provisionalRecordByRuntime.erase(it);
        else ++it;
    }
    if(allComplete){
        std::set<std::string> liveAndReserved;
        try {
            for(const auto& item : live) liveAndReserved.insert(item.first);
            for(const auto& item : g_reservedAutoIdentities)
                liveAndReserved.insert(item.first);
            g_restoreBudgets.pruneToLiveIdentities(liveAndReserved);
        } catch(...) {}
    }
}

static std::set<std::string> UpdateBoundRecords(
        const std::string& app,const AppFastSnapshot& snapshot,
        const std::vector<LayoutWin>& live,ReconcileFreshness freshness,
        UnixSeconds nowUtc){
    std::set<std::string> reserved;
    for(size_t index=0;index<snapshot.windows.size() && index<live.size();++index){
        const FastWin& fast=snapshot.windows[index];
        const std::string runtime=RuntimeKey(fast);
        auto binding=g_recordByRuntime.find(runtime);
        if(binding==g_recordByRuntime.end() || binding->second.app!=app ||
           !SameIdentity(binding->second.identity,IdentityOf(fast))) continue;
        const LayoutWin* existing=FindAutoRecord(binding->second.recordId);
        if(!existing){ g_recordByRuntime.erase(binding); continue; }
        const std::string recordId=existing->recordId;
        if(!CommitBoundRecordRefresh(
                *existing,fast,live[index],freshness,nowUtc,runtime,
                g_provisionalRecordByRuntime,[&](const LayoutWin& desired){
                    return UpsertAutoRecord(
                        desired,RecordDeltaKind::ValidatedRuntimeUpsert,
                        nowUtc,snapshot.generation);
                })) continue;
        binding->second.causalGeneration=snapshot.generation;
        reserved.insert(recordId);
        g_pendingRecordByRuntime.erase(runtime);
    }
    return reserved;
}

static bool SaveObservedApp(const std::string& app,
                            const AppFastSnapshot& snapshot,
                            UnixSeconds nowUtc,bool& needsReconcile){
    needsReconcile=false;
    if(!FastSnapshotCanPersistAll(snapshot) || snapshot.generation==0 ||
       nowUtc<=0) return false;

    std::vector<DeskRec> desktops;
    std::string desktopError;
    if(!CurrentDesktops(desktops,&desktopError)) return false;

    std::vector<BoundSaveObservation> observations;
    try {
        observations.reserve(snapshot.windows.size());
        for(const FastWin& fast : snapshot.windows){
            BoundSaveObservation observed;
            observed.window=fast;
            observed.causalGeneration=snapshot.generation;
            for(const DeskRec& desktop : desktops)
                if(GuidEq(desktop.guid,fast.desktop)){
                    observed.deskIndex=desktop.index;
                    break;
                }
            const std::string runtime=RuntimeKey(fast);
            auto binding=g_recordByRuntime.find(runtime);
            auto reservation=g_reservedAutoIdentities.find(runtime);
            const bool reserved=reservation!=g_reservedAutoIdentities.end() &&
                SameIdentity(reservation->second.identity,IdentityOf(fast));
            if(!reserved && binding!=g_recordByRuntime.end() &&
               binding->second.app==app &&
               SameIdentity(binding->second.identity,IdentityOf(fast))){
                observed.hasBinding=true;
                observed.expectedIdentity=binding->second.identity;
                observed.recordId=binding->second.recordId;
            }
            observations.push_back(std::move(observed));
        }
    } catch(...) { return false; }

    SaveObservedAppResult saved=ApplyObservedBoundRecords(
        g_autoRecords,app,observations,true,nowUtc);
    if(!saved.valid) return false;

    bool semanticChanged=false;
    try {
        std::vector<LayoutWin> nextRecords=g_autoRecords;
        std::map<std::string,RecordDelta> nextDeltas=g_dirtyRecordDeltas;
        std::map<std::string,ValidatedRecordTouch> nextTouches=
            g_validatedTouches;
        std::map<std::string,DeferredRecordConflict> nextConflicts=
            g_deferredRecordConflicts;
        for(const BoundSaveUpdate& update : saved.updates){
            if(update.semanticChanged){
                RecordDelta next;
                next.kind=RecordDeltaKind::ValidatedRuntimeUpsert;
                next.record=update.after;
                next.baseRevision=g_autoRevision;
                next.baseRecordPresent=true;
                next.baseRecord=update.before;
                next.changedUtc=nowUtc;
                next.causalGeneration=update.causalGeneration;
                std::vector<LayoutWin> stagedRecords;
                std::map<std::string,RecordDelta> stagedDeltas;
                std::map<std::string,DeferredRecordConflict> stagedConflicts;
                if(StageRecordDeltaMutation(
                        nextRecords,nextDeltas,nextConflicts,next,true,
                        stagedRecords,stagedDeltas,stagedConflicts)!=
                   RecordDeltaStageResult::Accepted)
                    return false;
                nextRecords.swap(stagedRecords);
                nextDeltas.swap(stagedDeltas);
                nextConflicts.swap(stagedConflicts);
                semanticChanged=true;
            } else {
                for(LayoutWin& record : nextRecords)
                    if(record.recordId==update.after.recordId){
                        record=update.after;
                        break;
                    }
            }
            ValidatedRecordTouch touch;
            touch.recordId=update.after.recordId;
            touch.lastSeenUtc=update.after.lastSeenUtc;
            touch.causalGeneration=update.causalGeneration;
            auto priorTouch=nextTouches.find(touch.recordId);
            if(priorTouch==nextTouches.end() ||
               priorTouch->second.lastSeenUtc<touch.lastSeenUtc)
                nextTouches[touch.recordId]=touch;
        }
        g_autoRecords.swap(nextRecords);
        g_autoDesktops.swap(desktops);
        g_dirtyRecordDeltas.swap(nextDeltas);
        g_validatedTouches.swap(nextTouches);
        g_deferredRecordConflicts.swap(nextConflicts);
    } catch(...) { return false; }

    for(const BoundSaveObservation& observed : observations){
        if(!observed.hasBinding) continue;
        auto binding=g_recordByRuntime.find(RuntimeKey(observed.window));
        if(binding!=g_recordByRuntime.end() &&
           SameIdentity(binding->second.identity,IdentityOf(observed.window))){
            binding->second.causalGeneration=snapshot.generation;
            g_pendingRecordByRuntime.erase(RuntimeKey(observed.window));
        }
    }
    needsReconcile=saved.needsReconcile;
    g_dirtyFlush.setConflict(!g_deferredRecordConflicts.empty(),
                             MonotonicNowMs());
    if(semanticChanged) MarkAutoDirty();
    return true;
}

static bool SessionPurposeForOwner(AsyncOperationOwner owner,
                                   SessionPurpose& purpose){
    switch(owner){
    case AsyncOperationOwner::AutoReconcile:
        purpose=SessionPurpose::AutoReconcile; return true;
    case AsyncOperationOwner::ManualSave:
        purpose=SessionPurpose::ManualSave; return true;
    case AsyncOperationOwner::ManualRestore:
        purpose=SessionPurpose::ManualRestore; return true;
    case AsyncOperationOwner::Search:
        purpose=SessionPurpose::Search; return true;
    case AsyncOperationOwner::MetadataProbe:
        purpose=SessionPurpose::MetadataProbe; return true;
    }
    return false;
}

static bool SessionOwnerForPurpose(SessionPurpose purpose,
                                   AsyncOperationOwner& owner){
    switch(purpose){
    case SessionPurpose::AutoReconcile:
        owner=AsyncOperationOwner::AutoReconcile; return true;
    case SessionPurpose::ManualSave:
        owner=AsyncOperationOwner::ManualSave; return true;
    case SessionPurpose::ManualRestore:
        owner=AsyncOperationOwner::ManualRestore; return true;
    case SessionPurpose::Search:
        owner=AsyncOperationOwner::Search; return true;
    case SessionPurpose::MetadataProbe:
        owner=AsyncOperationOwner::MetadataProbe; return true;
    case SessionPurpose::HeartbeatSave:
        return false;
    }
    return false;
}

static void CancelAutoOperation(uint64_t operationId,bool rearm);
static void CancelManualSaveOperation(uint64_t operationId);
static void CancelManualMoveOperation(uint64_t operationId);
static void FinishManualSave(uint64_t operationId);
static void FinishManualMove(uint64_t operationId);
static void RetireSessionRoutesForOperation(AsyncOperationOwner owner,
                                            uint64_t operationId);
static void RetireAsyncSessionOperation(const SessionRoute& route,
                                        AsyncRetirementReason reason);
static void ProcessSessionRetirements(
    const std::vector<AsyncSessionRetirement>& retired);
static void RetireReconcileOperation(uint64_t operationId);
static void CancelExpiredReconcileOperations(uint64_t nowMs);

static uint64_t RequestSessionWork(AsyncOperationOwner owner,uint64_t operationId,
        const AppProfile& profile,const AppFastSnapshot& snapshot,
        SessionPurpose purpose){
    SessionPurpose ownerPurpose=SessionPurpose::MetadataProbe;
    if(!g_sessionWorker || !g_main || operationId==0 ||
       snapshot.identityGeneration==0 ||
       !SessionPurposeForOwner(owner,ownerPurpose) || ownerPurpose!=purpose)
        return 0;
    const uint64_t requestId=TakeNonzeroId(g_nextSessionRequestId);
    SessionRequest request;
    request.requestId=requestId;
    request.app=profile.id;
    request.profile=profile;
    request.purpose=purpose;
    request.identityGeneration=snapshot.identityGeneration;
    SessionRoute route;
    route.owner=owner;
    route.operationId=operationId;
    route.app=profile.id;
    route.purpose=purpose;
    route.identityGeneration=snapshot.identityGeneration;
    route.contentGeneration=snapshot.generation;
    const uint64_t now=MonotonicNowMs();
    const uint64_t lifetime=AsyncSessionRouteGate::maxLifetimeMs();
    route.deadlineMs=now>(std::numeric_limits<uint64_t>::max)()-lifetime
        ? (std::numeric_limits<uint64_t>::max)() : now+lifetime;

    AsyncSessionRoute gated;
    gated.requestId=requestId;
    gated.operationId=operationId;
    gated.app=profile.id;
    gated.purpose=purpose;
    gated.identityGeneration=snapshot.identityGeneration;
    gated.deadlineMs=route.deadlineMs;
    std::vector<AsyncSessionRetirement> retired;
    if(g_sessionRouteGate.submit(gated,now,retired)!=
       AsyncRouteAdmission::Accepted) return 0;
    ProcessSessionRetirements(retired);

    try {
        if(!g_sessionRoutes.emplace(requestId,route).second){
            std::vector<AsyncSessionRetirement> ignored;
            g_sessionRouteGate.retire(requestId,operationId,
                snapshot.identityGeneration,AsyncRetirementReason::Cancelled,
                ignored);
            return 0;
        }
    } catch(...) {
        std::vector<AsyncSessionRetirement> ignored;
        g_sessionRouteGate.retire(requestId,operationId,
            snapshot.identityGeneration,AsyncRetirementReason::Cancelled,
            ignored);
        return 0;
    }
    if(!SetTimer(g_main,TIMER_MONITOR,MONITOR_INTERVAL_MS,nullptr)){
        std::vector<AsyncSessionRetirement> ignored;
        g_sessionRouteGate.retire(requestId,operationId,
            snapshot.identityGeneration,AsyncRetirementReason::Failed,ignored);
        g_sessionRoutes.erase(requestId);
        return 0;
    }
    bool requested=false;
    try { requested=g_sessionWorker->Request(request); }
    catch(...) { requested=false; }
    if(!requested){
        std::vector<AsyncSessionRetirement> ignored;
        g_sessionRouteGate.retire(requestId,operationId,
            snapshot.identityGeneration,AsyncRetirementReason::Failed,ignored);
        g_sessionRoutes.erase(requestId);
        return 0;
    }
    return requestId;
}

static bool RequestReconcileWork(const ReconcileRequest& request){
    if(!g_reconcileWorker || !g_main || request.operationId==0) return false;
    if(!SetTimer(g_main,TIMER_MONITOR,MONITOR_INTERVAL_MS,nullptr)) return false;
    if(!g_reconcileDeadlines.begin(
            request.operationId,MonotonicNowMs())) return false;
    bool accepted=false;
    try { accepted=g_reconcileWorker->Request(request); }
    catch(...) { accepted=false; }
    if(!accepted) g_reconcileDeadlines.complete(request.operationId);
    return accepted;
}

static void DispatchMoveResult(const MoveResult& result);
static void DispatchMoveResult(const MoveResult& result,
                               bool consumeCheckpointWhileProtected);
static void AdvanceMoveQueue();
static void FinishAutoOperation(uint64_t operationId);
static void FinishManualMove(uint64_t operationId);
static bool ExecuteCheckpoint(CheckpointReason reason);

static bool TryCancelQueuedMove(uint64_t jobId,MoveResult& result) noexcept {
    try {
        result=g_moveQueue.cancelJob(jobId);
        return result.completed;
    } catch(...) {
        result=MoveResult();
        return false;
    }
}

static void RefreshMoveCancellationPending() noexcept {
    g_moveCancellationPending=false;
    for(const auto& runtime : g_moveRuntime)
        if(runtime.second.cancelRequested){
            g_moveCancellationPending=true;
            break;
        }
    if(!g_moveCancellationPending) g_moveCancellationRetry.clear();
}


static size_t PendingMoveCancellationCount() noexcept {
    size_t count=0;
    for(const auto& runtime : g_moveRuntime)
        if(runtime.second.cancelRequested && count!=(std::numeric_limits<size_t>::max)())
            ++count;
    return count;
}

static bool ScheduleMoveCancellationRetry(){
    if(!g_main) return !g_moveCancellationPending;
    return g_moveCancellationRetry.request(g_moveCancellationPending,
        [](){
            return SetTimer(g_main,TIMER_MOVE_VERIFY,
                            MOVE_VERIFY_INTERVAL_MS,nullptr)!=0;
        },[](){
            return PostMessageW(g_main,WM_MOVE_CANCEL_RETRY,0,0)!=FALSE;
        });
}

static bool MoveCancellationAwaitsTerminal(uint64_t jobId) noexcept {
    try {
        auto runtime=g_moveRuntime.find(jobId);
        if(runtime==g_moveRuntime.end()) return false;
        return MoveCancellationDispositionFor(
            runtime->second.issueAwaitingVerify,
            RecaptureGenericWindowIdentity(IdentityOf(runtime->second.window)))==
            MoveCancellationDisposition::AwaitTerminalAcknowledgement;
    } catch(...) { return false; }
}

static bool MarkMoveForTerminalRetirement(uint64_t jobId) noexcept {
    if(!MoveCancellationAwaitsTerminal(jobId)) return false;
    auto runtime=g_moveRuntime.find(jobId);
    if(runtime==g_moveRuntime.end()) return false;
    runtime->second.retireAfterVerify=true;
    return true;
}

static bool CancelMoveJobOrDefer(uint64_t jobId){
    if(MarkMoveForTerminalRetirement(jobId)) return true;
    auto runtime=g_moveRuntime.find(jobId);
    if(runtime!=g_moveRuntime.end()) runtime->second.cancelRequested=true;
    MoveResult cancelled;
    try { cancelled=g_moveQueue.cancelJob(jobId); }
    catch(...) {
        RefreshMoveCancellationPending();
        if(!ScheduleMoveCancellationRetry())
            ReportStorageError(L"A cancelled window move is still protected and will be retried when move scheduling becomes available.");
        return false;
    }
    if(cancelled.completed){
        try { DispatchMoveResult(cancelled); }
        catch(...) {}
    } else {
        RefreshMoveCancellationPending();
        if(!ScheduleMoveCancellationRetry())
            ReportStorageError(L"A cancelled window move remains protected because its queue entry could not be retired.");
        return false;
    }
    RefreshMoveCancellationPending();
    return cancelled.completed;
}

static void CancelAutoOperation(uint64_t operationId,bool rearm){
    g_reconcileDeadlines.cancel(operationId);
    auto found=g_pendingAutoOperations.find(operationId);
    if(found==g_pendingAutoOperations.end()) return;
    if(found->second.cancellationPending){
        RefreshMoveCancellationPending();
        ScheduleMoveCancellationRetry();
        return;
    }
    const uint64_t lifecycleGeneration=found->second.lifecycleGeneration;
    auto state=g_lifecycleByApp.find(found->second.app);
    uint64_t jobIds[MAX_LAYOUT_RECORDS]={0};
    size_t jobCount=0;
    for(uint64_t jobId : found->second.liveJobIds)
        if(jobCount<MAX_LAYOUT_RECORDS) jobIds[jobCount++]=jobId;
    PublishMoveCancellationIntent(found->second.cancellationPending,
        jobIds,jobCount,[&](uint64_t jobId) noexcept {
            if(MarkMoveForTerminalRetirement(jobId)) return;
            auto runtime=g_moveRuntime.find(jobId);
            if(runtime!=g_moveRuntime.end()) runtime->second.cancelRequested=true;
        });
    RefreshMoveCancellationPending();
    found->second.reconcile.reset();
    found->second.reconcileFast.clear();
    RetireSessionRoutesForOperation(
        AsyncOperationOwner::AutoReconcile,operationId);
    if(state!=g_lifecycleByApp.end())
        LcCancelRestore(state->second,lifecycleGeneration,MonotonicNowMs(),rearm);
    for(size_t index=0;index<jobCount;++index)
        CancelMoveJobOrDefer(jobIds[index]);
    found=g_pendingAutoOperations.find(operationId);
    if(found!=g_pendingAutoOperations.end() && found->second.liveJobIds.empty())
        g_pendingAutoOperations.erase(found);
}

static void CancelExpiredSessionRoutes(uint64_t nowMs){
    std::vector<AsyncSessionRetirement> retired;
    g_sessionRouteGate.expire(nowMs,retired);
    ProcessSessionRetirements(retired);
}

static bool BeginAutoRestore(const AppProfile& profile,
                             const AppFastSnapshot& snapshot,
                             uint64_t lifecycleGeneration){
    AutoRestoreOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    operation.app=profile.id;
    operation.lifecycleGeneration=lifecycleGeneration;
    operation.identityGeneration=snapshot.identityGeneration;
    operation.contentGeneration=snapshot.generation;
    operation.windowSetSignature=snapshot.windowSetSignature;
    operation.layoutSignature=snapshot.layoutSignature;
    uint64_t operationId=operation.operationId;
    try {
        if(!g_pendingAutoOperations.emplace(operationId,std::move(operation)).second)
            return false;
    } catch(...) { return false; }
    uint64_t requestId=RequestSessionWork(
        AsyncOperationOwner::AutoReconcile,operationId,profile,snapshot,
        SessionPurpose::AutoReconcile);
    if(requestId==0){
        g_pendingAutoOperations.erase(operationId);
        return false;
    }
    g_pendingAutoOperations[operationId].sessionRequestId=requestId;
    return true;
}

static bool BeginMetadataProbe(const AppProfile& profile,
                               const AppFastSnapshot& snapshot){
    if(snapshot.windows.empty() || !FastSnapshotCanObserve(snapshot) ||
       snapshot.identityGeneration==0 || snapshot.generation==0) return false;
    for(const auto& route : g_sessionRoutes)
        if(route.second.app==profile.id) return false;
    for(const auto& operation : g_metadataProbeOperations)
        if(operation.second.app==profile.id) return false;
    MetadataProbeOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    operation.app=profile.id;
    operation.identityGeneration=snapshot.identityGeneration;
    operation.contentGeneration=snapshot.generation;
    const uint64_t operationId=operation.operationId;
    try {
        if(!g_metadataProbeOperations.emplace(operationId,operation).second)
            return false;
    } catch(...) { return false; }
    if(RequestSessionWork(AsyncOperationOwner::MetadataProbe,operationId,
                          profile,snapshot,SessionPurpose::MetadataProbe)==0){
        g_metadataProbeOperations.erase(operationId);
        return false;
    }
    return true;
}

static void ObserveFastSnapshots(
        const std::map<std::string,AppFastSnapshot>& snapshots){
    const uint64_t nowMs=MonotonicNowMs();
    const UnixSeconds nowUtc=UtcNowSeconds();
    CancelExpiredSessionRoutes(nowMs);
    PruneStaleRuntimeState(snapshots);
    std::vector<AppProfile> profiles=ActiveProfiles();
    for(const AppProfile& profile : profiles){
        auto found=snapshots.find(profile.id);
        if(found==snapshots.end() || !FastSnapshotCanObserve(found->second)) continue;
        const AppFastSnapshot& snapshot=found->second;
        LcState& state=g_lifecycleByApp[profile.id];
        uint64_t sessionSignature=g_lastFreshSessionSignature[profile.id];
        LcDecision decision=LcObserve(state,!snapshot.windows.empty(),
            snapshot.windowSetSignature,snapshot.settleSignature,
            snapshot.layoutSignature,sessionSignature,nowMs);
        if(decision.action==LcAction::MarkMissingFromLastSeen){
            MarkAppMissingFromLastSeen(
                profile.id,nowUtc,snapshot.generation);
        } else if(decision.action==LcAction::BeginRestore){
            if(!FastSnapshotCanPersistAll(snapshot) ||
               !BeginAutoRestore(profile,snapshot,decision.generation))
                LcCancelRestore(state,decision.generation,nowMs,true);
        } else if(decision.action==LcAction::SaveLayout){
            bool needsReconcile=true;
            if(SaveObservedApp(profile.id,snapshot,nowUtc,needsReconcile) &&
               !needsReconcile){
                LcExplicitSaveCompleted(state,decision.generation,
                    snapshot.layoutSignature,sessionSignature,nowMs);
            } else {
                LcExplicitSaveNeedsReconcile(state,decision.generation,
                    snapshot.layoutSignature,sessionSignature,nowMs);
            }
        }
        if(!state.restorePending && !state.restoreInFlight &&
           !state.saveInFlight)
            BeginMetadataProbe(profile,snapshot);
    }
}

static HRESULT IssueWindowMove(const MoveRuntimeBinding& binding,
                               WindowIdentityRecapture& identity){
    identity=WindowIdentityRecapture::Match;
    const HWND hwnd=binding.window.hwnd;
    const GUID& destinationGuid=binding.destination;
    if(GuidIsZero(destinationGuid) || !g_vdmi || !g_avc) return E_INVALIDARG;
    ScopedComPtr<IVirtualDesktop> destination(GetDesktopByGuid(destinationGuid));
    if(!destination) return E_INVALIDARG;
    identity=RecaptureGenericWindowIdentity(IdentityOf(binding.window));
    if(identity!=WindowIdentityRecapture::Match)
        return identity==WindowIdentityRecapture::Lost ? E_ABORT : E_PENDING;
    HRESULT issued=E_FAIL;
    try {
        issued=[&]()->HRESULT{
            if(binding.window.pid==GetCurrentProcessId()){
                if(!g_vdmDoc) return E_NOINTERFACE;
                return g_vdmDoc->MoveWindowToDesktop(hwnd,destinationGuid);
            }
            IApplicationView* rawView=nullptr;
            HRESULT result=g_avc->GetViewForHwnd(hwnd,&rawView);
            ScopedComPtr<IApplicationView> view(rawView);
            if(FAILED(result)) return result;
            if(!view) return E_FAIL;
            return g_vdmi->MoveViewToDesktop(view.get(),destination.get());
        }();
    } catch(...) { issued=E_FAIL; }
    return issued;
}

static bool RetryableMoveHresult(HRESULT result){
    return result==E_FAIL || result==E_PENDING || result==RPC_E_CALL_REJECTED ||
           result==HRESULT_FROM_WIN32(ERROR_BUSY) ||
           result==HRESULT_FROM_WIN32(ERROR_RETRY) ||
           result==HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

static MoveAttemptOutcome ReadMoveDestination(const MoveRuntimeBinding& binding,
                                              WindowIdentityRecapture& identity){
    identity=WindowIdentityRecapture::Match;
    if(GetDesktopIndexByGuid(binding.destination)<0)
        return MoveAttemptOutcome::PermanentFailure;
    if(!g_vdmDoc) return MoveAttemptOutcome::PermanentFailure;
    GUID current={0};
    identity=RecaptureGenericWindowIdentity(IdentityOf(binding.window));
    if(identity!=WindowIdentityRecapture::Match)
        return MoveAttemptOutcome::TransientFailure;
    HRESULT result=E_FAIL;
    try { result=g_vdmDoc->GetWindowDesktopId(binding.window.hwnd,&current); }
    catch(...) { result=E_FAIL; }
    if(SUCCEEDED(result) && !GuidIsZero(current))
        return GuidEq(current,binding.destination)
            ? MoveAttemptOutcome::OnDestination
            : MoveAttemptOutcome::TransientFailure;
    return RetryableMoveHresult(result)
        ? MoveAttemptOutcome::TransientFailure
        : MoveAttemptOutcome::PermanentFailure;
}

static bool CancelNextMoveAfterArmFailure(){
    const MoveJob* front=g_moveQueue.front();
    if(!front){ RefreshMoveCancellationPending(); return true; }
    const uint64_t jobId=front->token.jobId;
    auto runtime=g_moveRuntime.find(jobId);
    if(runtime!=g_moveRuntime.end() && !runtime->second.cancelRequested)
        return false;
    MoveResult cancelled;
    if(!TryCancelQueuedMove(jobId,cancelled)){
        RefreshMoveCancellationPending();
        return false;
    }
    try { DispatchMoveResult(cancelled); }
    catch(...) {
        // DispatchMoveResult settles runtime/reservation ownership before an
        // owner callback is allowed to escape.
    }
    RefreshMoveCancellationPending();
    return g_moveQueue.empty();
}

static bool ArmMoveTimer(){
    if(g_moveQueue.empty()) return true;
    try {
        if(g_main && SetTimer(g_main,TIMER_MOVE_VERIFY,
                MOVE_VERIFY_INTERVAL_MS,nullptr)!=0) return true;
    } catch(...) {}

    // No timer means no queued job may Issue.  Mark every retained runtime
    // first, cancel at most one synchronously, then rearm/post bounded retry
    // work so allocation failure preserves coherent guarded ownership.
    for(auto& runtime : g_moveRuntime) runtime.second.cancelRequested=true;
    RefreshMoveCancellationPending();
    const MoveArmFailureCleanup cleanup=RecoverMoveArmFailure(
        [](){ return CancelNextMoveAfterArmFailure(); },
        [](){ return ScheduleMoveCancellationRetry(); });
    if(cleanup==MoveArmFailureCleanup::Completed)
        ReportStorageError(L"Window-move verification could not be started; queued moves were cancelled safely.");
    else if(cleanup==MoveArmFailureCleanup::Rearmed)
        ReportStorageError(L"Window-move verification could not be started; queued moves are being cancelled safely.");
    else
        ReportStorageError(L"Window-move verification could not be started; queued moves remain protected and cancellation will retry on the next move request.");
    return false;
}

static bool HandleIndeterminateMoveIdentity(
        uint64_t jobId,MoveRuntimeBinding& runtime,
        WindowIdentityRecapture recapture){
    const IdentityRecaptureRetryAction action=
        runtime.identityRecaptureBudget.observe(recapture);
    if(recapture!=WindowIdentityRecapture::Indeterminate) return false;
    if(action==IdentityRecaptureRetryAction::Retry) return true;
    runtime.cancelRequested=true;
    MoveResult cancelled;
    if(TryCancelQueuedMove(jobId,cancelled)){
        try { DispatchMoveResult(cancelled); } catch(...) {}
    }
    RefreshMoveCancellationPending();
    return true;
}

static void AdvanceMoveQueue(){
    const MoveJob* front=g_moveQueue.front();
    if(!front){
        if(g_main) KillTimer(g_main,TIMER_MOVE_VERIFY);
        return;
    }
    const uint64_t jobId=front->token.jobId;
    auto runtime=g_moveRuntime.find(jobId);
    if(runtime==g_moveRuntime.end()){
        MoveResult cancelled;
        if(TryCancelQueuedMove(jobId,cancelled)) DispatchMoveResult(cancelled);
        return;
    }
    if(runtime->second.cancelRequested){
        MoveResult cancelled;
        if(TryCancelQueuedMove(jobId,cancelled)){
            try { DispatchMoveResult(cancelled); }
            catch(...) {}
        }
        RefreshMoveCancellationPending();
        if(g_moveQueue.empty() && g_main) KillTimer(g_main,TIMER_MOVE_VERIFY);
        return;
    }
    MoveResult result;
    if(g_moveQueue.nextAction()==MoveAction::Issue){
        WindowIdentityRecapture identity=WindowIdentityRecapture::Match;
        MoveAttemptOutcome current=ReadMoveDestination(
            runtime->second,identity);
        if(HandleIndeterminateMoveIdentity(jobId,runtime->second,identity)) return;
        if(identity==WindowIdentityRecapture::Lost){
            TryCancelQueuedMove(jobId,result);
        } else if(current==MoveAttemptOutcome::OnDestination ||
           current==MoveAttemptOutcome::PermanentFailure){
            result=g_moveQueue.onIssued(current);
        } else {
            HRESULT issued=IssueWindowMove(runtime->second,identity);
            if(HandleIndeterminateMoveIdentity(
                    jobId,runtime->second,identity)) return;
            if(identity==WindowIdentityRecapture::Lost)
                TryCancelQueuedMove(jobId,result);
            else {
                runtime->second.identityRecaptureBudget.reset();
                MoveAttemptOutcome outcome=SUCCEEDED(issued)
                    ? MoveAttemptOutcome::Accepted
                    : (RetryableMoveHresult(issued)
                        ? MoveAttemptOutcome::TransientFailure
                        : MoveAttemptOutcome::PermanentFailure);
                result=g_moveQueue.onIssued(outcome);
            }
        }
        if(!result.completed && g_moveQueue.nextAction()==MoveAction::Verify)
            runtime->second.issueAwaitingVerify=true;
    } else if(g_moveQueue.nextAction()==MoveAction::Verify){
        WindowIdentityRecapture identity=WindowIdentityRecapture::Match;
        MoveAttemptOutcome verified=ReadMoveDestination(
            runtime->second,identity);
        if(HandleIndeterminateMoveIdentity(jobId,runtime->second,identity)) return;
        if(identity==WindowIdentityRecapture::Match)
            runtime->second.identityRecaptureBudget.reset();
        if(runtime->second.retireAfterVerify){
            const IssuedMoveRetirementAction retirement=
                runtime->second.retirement.observe(
                    identity!=WindowIdentityRecapture::Lost,verified);
            if(retirement==IssuedMoveRetirementAction::WaitForReadback) return;
            const bool protectCheckpoint=retirement==
                IssuedMoveRetirementAction::ConsumeProtectedCheckpointAndCancel;
            if(!protectCheckpoint){
                runtime->second.issueAwaitingVerify=false;
            }
            if(!TryCancelQueuedMove(jobId,result)) return;
            if(result.completed)
                DispatchMoveResult(result,protectCheckpoint);
            if(g_moveQueue.empty() && g_main)
                KillTimer(g_main,TIMER_MOVE_VERIFY);
            return;
        }
        runtime->second.issueAwaitingVerify=false;
        if(identity==WindowIdentityRecapture::Lost)
            TryCancelQueuedMove(jobId,result);
        else result=g_moveQueue.onVerified(verified);
    }
    if(result.completed) DispatchMoveResult(result);
    if(g_moveQueue.empty() && g_main) KillTimer(g_main,TIMER_MOVE_VERIFY);
}

static bool ReleaseMoveReservation(const MoveResult& result){
    auto reserved=g_reservedAutoIdentities.find(result.runtimeKey);
    if(reserved!=g_reservedAutoIdentities.end() &&
       SameMoveToken(reserved->second.token,result.token)){
        g_reservedAutoIdentities.erase(reserved);
        return true;
    }
    return false;
}

static bool ConsumeCheckpointAndReleaseMoveReservation(
        const MoveResult& result){
    auto reserved=g_reservedAutoIdentities.find(result.runtimeKey);
    if(reserved==g_reservedAutoIdentities.end() ||
       !SameMoveToken(reserved->second.token,result.token)) return false;
    const bool lastReservation=g_reservedAutoIdentities.size()==1;
    g_checkpointController.acknowledgeReservationBeforeRelease(
        true,lastReservation,g_autoFix && !g_degraded,g_autoLoaded,
        [](CheckpointReason reason){ return ExecuteCheckpoint(reason); });
    // The checkpoint callback must observe the guard.  Re-find afterward so
    // cleanup stays safe even if injected code altered the reservation map.
    reserved=g_reservedAutoIdentities.find(result.runtimeKey);
    if(reserved!=g_reservedAutoIdentities.end() &&
       SameMoveToken(reserved->second.token,result.token)){
        g_reservedAutoIdentities.erase(reserved);
        return true;
    }
    return false;
}

static void FinishAutoOperation(uint64_t operationId){
    auto found=g_pendingAutoOperations.find(operationId);
    if(found!=g_pendingAutoOperations.end() &&
       found->second.cancellationPending){
        if(found->second.liveJobIds.empty())
            g_pendingAutoOperations.erase(found);
        return;
    }
    if(found==g_pendingAutoOperations.end() || found->second.outstanding!=0) return;
    AutoRestoreOperation& operation=found->second;
    g_reconcileDeadlines.cancel(operationId);
    auto lifecycle=g_lifecycleByApp.find(operation.app);
    RunTerminalCompletionOrFail([&](){
        if(!operation.reconcile){
            if(lifecycle!=g_lifecycleByApp.end())
                LcCancelRestore(lifecycle->second,operation.lifecycleGeneration,
                                MonotonicNowMs(),true);
            return;
        }
        const ReconcileResult& result=*operation.reconcile;
        if(result.plan.deferred){
            if(lifecycle!=g_lifecycleByApp.end())
                LcRestoreCompleted(lifecycle->second,operation.lifecycleGeneration,
                    LcRestoreOutcome::Deferred,operation.layoutSignature,
                    operation.sourceSignature,MonotonicNowMs());
            return;
        }

        std::set<size_t> successfulLiveIndices;
        for(size_t index=0;index<operation.successfulLive.size();++index)
            if(operation.successfulLive.succeeded(index))
                successfulLiveIndices.insert(index);
        const UnixSeconds nowUtc=UtcNowSeconds();
        std::vector<LayoutWin> committed=CommitAppReconcile(
            result.saved,result.live,result.plan,
            successfulLiveIndices,result.plan.nowUtc);
    std::map<std::string,LayoutWin> baseById,committedById;
    for(const LayoutWin& record : result.saved)
        if(record.app==operation.app) baseById[record.recordId]=record;
    for(const LayoutWin& record : committed)
        if(record.app==operation.app) committedById[record.recordId]=record;

    for(const auto& entry : baseById){
        if(committedById.count(entry.first)!=0) continue;
        const LayoutWin* current=FindAutoRecord(entry.first);
        if(current && SameRecordForDelta(*current,entry.second))
            EraseAutoRecord(*current,RecordDeltaKind::ExpireDelete,
                            nowUtc,operation.contentGeneration);
    }
    for(const auto& entry : committedById){
        auto base=baseById.find(entry.first);
        if(base!=baseById.end() &&
           SameRecordForDelta(base->second,entry.second)) continue;
        RecordDeltaKind kind=RecordDeltaKind::ValidatedRuntimeUpsert;
        if(base!=baseById.end() && base->second.missingSinceUtc==0 &&
           entry.second.missingSinceUtc!=0)
            kind=RecordDeltaKind::MissingMark;
        UpsertAutoRecord(entry.second,kind,nowUtc,operation.contentGeneration);
    }

    std::set<size_t> restoreIndices;
    for(const RestoreRequest& restore : result.plan.restores)
        restoreIndices.insert(restore.liveIndex);
    for(const LayoutMatch& match : result.plan.matches){
        if(match.savedIndex>=result.saved.size() ||
           match.liveIndex>=operation.reconcileFast.size()) continue;
        const FastWin& fast=operation.reconcileFast[match.liveIndex];
        const std::string runtime=RuntimeKey(fast);
        const bool required=restoreIndices.count(match.liveIndex)!=0;
        const bool succeeded=!required ||
            operation.successfulLive.succeeded(match.liveIndex);
        if(succeeded){
            RuntimeRecordBinding binding;
            binding.app=operation.app;
            binding.recordId=result.saved[match.savedIndex].recordId;
            binding.identity=IdentityOf(fast);
            binding.causalGeneration=operation.contentGeneration;
            g_recordByRuntime[runtime]=binding;
            g_pendingRecordByRuntime.erase(runtime);
        } else {
            g_pendingRecordByRuntime[runtime]=
                result.saved[match.savedIndex].recordId;
        }
    }
    for(const NewRecordRequest& created : result.plan.newRecords){
        if(created.liveIndex>=operation.reconcileFast.size()) continue;
        const FastWin& fast=operation.reconcileFast[created.liveIndex];
        RuntimeRecordBinding binding;
        binding.app=operation.app;
        binding.recordId=created.recordId;
        binding.identity=IdentityOf(fast);
        binding.causalGeneration=operation.contentGeneration;
        g_recordByRuntime[RuntimeKey(fast)]=binding;
        g_pendingRecordByRuntime.erase(RuntimeKey(fast));
    }

        if(operation.hadFailure)
            ReportStorageError(L"Some browser windows could not be restored automatically; their saved destinations were retained.");
        if(lifecycle!=g_lifecycleByApp.end())
            LcRestoreCompleted(lifecycle->second,operation.lifecycleGeneration,
                AutoRestoreCompletionOutcome(
                    operation.hadExhausted,operation.hadFailure),
                operation.layoutSignature,operation.sourceSignature,MonotonicNowMs());
    },[&]() noexcept {
        if(lifecycle==g_lifecycleByApp.end()) return;
        try {
            LcRestoreCompleted(lifecycle->second,operation.lifecycleGeneration,
                LcRestoreOutcome::Exhausted,operation.layoutSignature,
                operation.sourceSignature,MonotonicNowMs());
        } catch(...) {
            try {
                LcCancelRestore(lifecycle->second,operation.lifecycleGeneration,
                                MonotonicNowMs(),true);
            } catch(...) {}
        }
    });
    g_pendingAutoOperations.erase(found);
}

static void DispatchMoveOwnerResult(const MoveResult& result,bool hadRuntime,
                                    MoveRuntimeBinding& runtime){
    if(result.token.owner==MoveOwner::AutoReconcile){
        auto operation=g_pendingAutoOperations.find(result.token.operationId);
        if(operation==g_pendingAutoOperations.end() ||
           operation->second.liveJobIds.erase(result.token.jobId)==0) return;
        if(operation->second.outstanding>0) --operation->second.outstanding;
        if(operation->second.cancellationPending){
            if(operation->second.liveJobIds.empty())
                g_pendingAutoOperations.erase(operation);
            return;
        }
        if(result.terminal==MoveTerminal::Succeeded){
            if(!operation->second.successfulLive.markSucceeded(
                    result.token.itemIndex)) operation->second.hadFailure=true;
            if(hadRuntime && runtime.hasBudgetKey)
                g_restoreBudgets.clearExact(runtime.budgetKey);
        } else if(result.terminal==MoveTerminal::Exhausted){
            operation->second.hadExhausted=true;
            operation->second.hadFailure=true;
            if(hadRuntime && runtime.hasBudgetKey &&
               !g_restoreBudgets.markExhaustedPrepared(
                    std::move(runtime.budgetKey)))
                operation->second.hadFailure=true;
        } else if(result.terminal!=MoveTerminal::Cancelled){
            operation->second.hadFailure=true;
        }
        FinishAutoOperation(result.token.operationId);
        return;
    }

    if(result.token.owner==MoveOwner::ManualTray){
        auto operation=g_manualMoveOperations.find(result.token.operationId);
        if(operation==g_manualMoveOperations.end() ||
           operation->second.liveJobIds.erase(result.token.jobId)==0) return;
        if(operation->second.outstanding>0) --operation->second.outstanding;
        if(operation->second.cancellationPending){
            if(operation->second.liveJobIds.empty())
                g_manualMoveOperations.erase(operation);
            return;
        }
        if(result.terminal==MoveTerminal::Succeeded) ++operation->second.succeeded;
        else ++operation->second.failed;
        FinishManualMove(result.token.operationId);
        return;
    }

    if(result.token.owner==MoveOwner::Picker){
        auto operation=g_pickerOperations.find(result.token.operationId);
        if(operation==g_pickerOperations.end() ||
           operation->second.liveJobIds.erase(result.token.jobId)==0) return;
        PopupPersistenceResult persistence=PopupPersistenceResult::NotTracked;
        std::string persistedApp;
        if(result.terminal==MoveTerminal::Succeeded){
            if(!hadRuntime){
                persistence=PopupPersistenceResult::IdentityIndeterminate;
            } else {
                const WindowIdentityKey expected=IdentityOf(runtime.window);
                persistence=CompletePopupMovePersistence(
                    expected,
                    [](const WindowIdentityKey& identity){
                        return RecaptureGenericWindowIdentity(identity);
                    },
                    [&](const WindowIdentityKey& identity){
                        return ClassifyTrackedBrowserWindow(identity,persistedApp);
                    },
                    [](){
                        if(!g_autoLoaded)
                            return PopupPersistenceReadiness::Unavailable;
                        if(!g_autoWritesAllowed || !g_autoFix || g_degraded)
                            return PopupPersistenceReadiness::ReadOnly;
                        return PopupPersistenceReadiness::Ready;
                    },
                    [&](){
                        return PersistPickerMovedWindow(
                            result,runtime,persistedApp);
                    });
            }
        }
        if(persistence==PopupPersistenceResult::Saved &&
           operation->second.app==persistedApp){
            CompletePopupLifecycleAfterPersistence(persistence,[&]{
                auto lifecycle=g_lifecycleByApp.find(operation->second.app);
                if(lifecycle!=g_lifecycleByApp.end())
                    LcExplicitSaveCompleted(
                        lifecycle->second,
                        operation->second.lifecycleSaveGeneration,
                        operation->second.lifecycleLayoutSignature,
                        operation->second.lifecycleSessionSignature,
                        MonotonicNowMs());
            });
        }
        if(operation->second.liveJobIds.empty()){
            const bool mayReport=!operation->second.completionReported;
            g_pickerOperations.erase(operation);
            if(!mayReport) return;
            if(result.terminal!=MoveTerminal::Succeeded)
                Balloon(L"The window could not be moved to that desktop.");
            else if(persistence==PopupPersistenceResult::IdentityLost ||
                    persistence==PopupPersistenceResult::IdentityIndeterminate)
                Balloon(L"The window moved, but its identity changed before the browser layout could be saved.");
            else if(persistence==PopupPersistenceResult::ClassificationFailed)
                Balloon(L"The window moved, but browser classification failed and its layout was not saved.");
            else if(persistence==PopupPersistenceResult::StorageUnavailable)
                Balloon(L"The window moved, but automatic layout storage is unavailable and the destination was not saved.");
            else if(persistence==PopupPersistenceResult::StorageReadOnly)
                Balloon(L"The window moved, but automatic layout storage is read-only or disabled and the destination was not saved.");
            else if(persistence==PopupPersistenceResult::SaveFailed)
                Balloon(L"The window moved, but its browser destination could not be saved; the change remains pending for retry.");
        }
    }
}

static void FinalizeMoveReservation(const MoveResult& result,
                                    bool consumeCheckpointWhileProtected){
    if(consumeCheckpointWhileProtected){
        ConsumeCheckpointAndReleaseMoveReservation(result);
        return;
    }
    const bool reservationReleased=ReleaseMoveReservation(result);
    try {
        g_checkpointController.reservationTerminated(
            reservationReleased,!g_reservedAutoIdentities.empty(),
            g_autoFix && !g_degraded,g_autoLoaded,
            [](CheckpointReason reason){ return ExecuteCheckpoint(reason); });
    } catch(...) {
        // Reservation ownership is already settled.  Checkpoint persistence
        // retains its own dirty/retry state and must not unwind WndProc.
    }
}

static void RetireMoveOwnerDispatchFailure(const MoveResult& result) noexcept {
    try {
        if(result.token.owner==MoveOwner::AutoReconcile){
            auto operation=g_pendingAutoOperations.find(result.token.operationId);
            if(operation==g_pendingAutoOperations.end()) return;
            if(operation->second.liveJobIds.erase(result.token.jobId)!=0 &&
               operation->second.outstanding>0)
                --operation->second.outstanding;
            operation->second.hadFailure=true;
            if(operation->second.cancellationPending){
                if(operation->second.liveJobIds.empty())
                    g_pendingAutoOperations.erase(operation);
                return;
            }
            if(operation->second.outstanding==0)
                FinishAutoOperation(result.token.operationId);
            return;
        }
        if(result.token.owner==MoveOwner::ManualTray){
            auto operation=g_manualMoveOperations.find(result.token.operationId);
            if(operation==g_manualMoveOperations.end()) return;
            if(operation->second.liveJobIds.erase(result.token.jobId)!=0){
                if(operation->second.outstanding>0) --operation->second.outstanding;
                ++operation->second.failed;
            }
            if(operation->second.cancellationPending){
                if(operation->second.liveJobIds.empty())
                    g_manualMoveOperations.erase(operation);
                return;
            }
            if(operation->second.outstanding==0)
                FinishManualMove(result.token.operationId);
            return;
        }
        auto operation=g_pickerOperations.find(result.token.operationId);
        if(operation==g_pickerOperations.end()) return;
        operation->second.liveJobIds.erase(result.token.jobId);
        if(operation->second.liveJobIds.empty()){
            const bool report=!operation->second.completionReported;
            g_pickerOperations.erase(operation);
            if(report) Balloon(L"The window move completed, but its result could not be processed safely.");
        }
    } catch(...) {}
}

static void DispatchMoveResult(const MoveResult& result,
                               bool consumeCheckpointWhileProtected){
    if(!result.completed || result.token.jobId==0) return;
    static_assert(std::is_nothrow_move_assignable<MoveRuntimeBinding>::value,
        "terminal dispatch must extract runtime ownership without allocation");
    MoveRuntimeBinding runtime;
    bool hadRuntime=false;
    auto runtimeFound=g_moveRuntime.find(result.token.jobId);
    if(runtimeFound!=g_moveRuntime.end()){
        runtime=std::move(runtimeFound->second);
        hadRuntime=true;
        g_moveRuntime.erase(runtimeFound);
    }
    const bool protect=consumeCheckpointWhileProtected ||
        result.terminal!=MoveTerminal::Succeeded;
    try {
        DispatchMoveOwnerResult(result,hadRuntime,runtime);
    } catch(...) {
        RetireMoveOwnerDispatchFailure(result);
        FinalizeMoveReservation(result,protect);
        return;
    }
    FinalizeMoveReservation(result,protect);
}

static void DispatchMoveResult(const MoveResult& result){
    DispatchMoveResult(result,false);
}

static void CompleteAutoDeferred(uint64_t operationId,uint64_t sourceSignature){
    auto found=g_pendingAutoOperations.find(operationId);
    if(found==g_pendingAutoOperations.end()) return;
    const std::string app=found->second.app;
    const uint64_t generation=found->second.lifecycleGeneration;
    const uint64_t layoutSignature=found->second.layoutSignature;
    g_pendingAutoOperations.erase(found);
    g_reconcileDeadlines.cancel(operationId);
    auto lifecycle=g_lifecycleByApp.find(app);
    if(lifecycle!=g_lifecycleByApp.end())
        LcRestoreCompleted(lifecycle->second,generation,
            LcRestoreOutcome::Deferred,layoutSignature,sourceSignature,
            MonotonicNowMs());
}

static void HandleAutoSessionResult(const SessionRoute& route,
                                    const SessionResult& result){
    auto operation=g_pendingAutoOperations.find(route.operationId);
    if(operation==g_pendingAutoOperations.end() ||
       operation->second.sessionRequestId!=result.requestId) return;
    if(result.status==SessionDataStatus::Superseded){
        CancelAutoOperation(route.operationId,true);
        return;
    }
    const uint64_t sourceSignature=SessionSourceSignature(result);
    if(result.status==SessionDataStatus::Unavailable){
        CompleteAutoDeferred(route.operationId,sourceSignature);
        return;
    }
    if(!SessionDataUsable(result.status) || !result.windows ||
       result.dataGeneration==0){
        CancelAutoOperation(route.operationId,true);
        return;
    }

    std::vector<AppProfile> profiles;
    const AppProfile* profile=FindActiveProfile(route.app,profiles);
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    auto current=snapshots.find(route.app);
    if(!profile || current==snapshots.end() ||
       !FastSnapshotCanObserve(current->second) ||
       !FastSnapshotCanPersistAll(current->second) ||
       current->second.identityGeneration!=result.identityGeneration){
        CancelAutoOperation(route.operationId,true);
        return;
    }

    std::vector<DeskRec> currentDesktops;
    std::string desktopError;
    if(!CurrentDesktops(currentDesktops,&desktopError)){
        CancelAutoOperation(route.operationId,true);
        return;
    }
    g_autoDesktops.swap(currentDesktops);
    const ReconcileFreshness freshness=
        result.status==SessionDataStatus::Fresh
            ? ReconcileFreshness::Fresh : ReconcileFreshness::CachedStale;
    operation=g_pendingAutoOperations.find(route.operationId);
    if(operation==g_pendingAutoOperations.end()) return;
    operation->second.identityGeneration=current->second.identityGeneration;
    operation->second.contentGeneration=current->second.generation;
    operation->second.windowSetSignature=current->second.windowSetSignature;
    operation->second.layoutSignature=current->second.layoutSignature;
    operation->second.sourceSignature=sourceSignature;
    operation->second.sessionDataGeneration=result.dataGeneration;
    operation->second.freshness=freshness;
    if(result.status==SessionDataStatus::Fresh){
        g_lastFreshSessionSignature[route.app]=sourceSignature;
    }

    ReconcileRequest request;
    request.operationId=route.operationId;
    request.app=route.app;
    request.identityGeneration=current->second.identityGeneration;
    request.contentGeneration=current->second.generation;
    request.sessionRequestId=result.requestId;
    request.sessionDataGeneration=result.dataGeneration;
    request.nowUtc=UtcNowSeconds();
    request.freshness=freshness;
    try { ConfigureWorkerLiveBuild(request,ReconcileWorkMode::PrepareLiveOnly,
                                   *profile,current->second,result.windows,
                                   g_autoDesktops); }
    catch(...) { CancelAutoOperation(route.operationId,true); return; }
    operation->second.reconcilePending=true;
    operation->second.reconcileMode=ReconcileWorkMode::PrepareLiveOnly;
    if(!RequestReconcileWork(request)){
        operation->second.reconcilePending=false;
        CancelAutoOperation(route.operationId,true);
    }
}

static void HandleSearchSessionResult(const SessionRoute& route,
                                      const SessionResult& result);
static void HandleManualSaveSessionResult(const SessionRoute& route,
                                          const SessionResult& result);
static void HandleManualRestoreSessionResult(const SessionRoute& route,
                                              const SessionResult& result);
static void HandleMetadataProbeSessionResult(const SessionRoute& route,
                                             const SessionResult& result){
    auto operation=g_metadataProbeOperations.find(route.operationId);
    if(operation==g_metadataProbeOperations.end() ||
       operation->second.app!=route.app) return;
    const MetadataProbeOperation expected=operation->second;
    g_metadataProbeOperations.erase(operation);
    if(result.status!=SessionDataStatus::Fresh || !result.windows ||
       result.dataGeneration==0 || !result.sourceStampKnown) return;
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    auto current=snapshots.find(route.app);
    if(current==snapshots.end() || !FastSnapshotCanObserve(current->second) ||
       current->second.identityGeneration!=expected.identityGeneration ||
       current->second.generation!=expected.contentGeneration ||
       route.identityGeneration!=expected.identityGeneration ||
       route.contentGeneration!=expected.contentGeneration) return;
    const uint64_t sourceSignature=SessionSourceSignature(result);
    if(g_lastFreshSessionSignature[route.app]!=sourceSignature)
        g_lastFreshSessionSignature[route.app]=sourceSignature;
}

static void HandleSessionResult(std::unique_ptr<SessionResult> result){
    if(!result) return;
    auto route=g_sessionRoutes.find(result->requestId);
    if(route==g_sessionRoutes.end()) return;
    SessionRoute expected=route->second;
    std::vector<AsyncSessionRetirement> retired;
    const AsyncRetirementReason reason=
        result->status==SessionDataStatus::Superseded
            ? AsyncRetirementReason::Superseded
            : AsyncRetirementReason::Completed;
    if(!g_sessionRouteGate.retire(result->requestId,expected.operationId,
            expected.identityGeneration,reason,retired)){
        g_sessionRoutes.erase(route);
        RetireAsyncSessionOperation(expected,AsyncRetirementReason::Failed);
        return;
    }
    g_sessionRoutes.erase(route);
    if(result->app!=expected.app || result->purpose!=expected.purpose ||
       result->identityGeneration!=expected.identityGeneration){
        RetireAsyncSessionOperation(expected,AsyncRetirementReason::Failed);
        return;
    }
    try {
        if(expected.owner==AsyncOperationOwner::AutoReconcile)
            HandleAutoSessionResult(expected,*result);
        else if(expected.owner==AsyncOperationOwner::Search)
            HandleSearchSessionResult(expected,*result);
        else if(expected.owner==AsyncOperationOwner::ManualSave)
            HandleManualSaveSessionResult(expected,*result);
        else if(expected.owner==AsyncOperationOwner::ManualRestore)
            HandleManualRestoreSessionResult(expected,*result);
        else if(expected.owner==AsyncOperationOwner::MetadataProbe)
            HandleMetadataProbeSessionResult(expected,*result);
    } catch(...) {
        RetireAsyncSessionOperation(expected,AsyncRetirementReason::Failed);
    }
}

static bool QueueAutoMove(AutoRestoreOperation& operation,
                          const ReconcileResult& result,
                          const RestoreRequest& restore){
    if(restore.savedIndex>=result.saved.size() ||
       restore.liveIndex>=operation.reconcileFast.size()) return false;
    const FastWin& fast=operation.reconcileFast[restore.liveIndex];
    const std::string runtimeKey=RuntimeKey(fast);
    const LayoutWin& saved=result.saved[restore.savedIndex];
    g_pendingRecordByRuntime[runtimeKey]=saved.recordId;
    if(g_reservedAutoIdentities.count(runtimeKey)) return false;
    if(GetDesktopIndexByGuid(restore.destination)<0) return false;

    RestoreBudgetKey budget;
    budget.recordId=saved.recordId;
    budget.fullRuntimeIdentity=runtimeKey;
    budget.destinationGuid=W2U8(GuidToString(restore.destination));
    if(!g_restoreBudgets.mayAttempt(budget)){
        operation.hadExhausted=true;
        return false;
    }
    if(!g_restoreBudgets.prepareTerminalInsert()) return false;
    MoveJob job;
    job.token.owner=MoveOwner::AutoReconcile;
    job.token.operationId=operation.operationId;
    job.token.jobId=TakeNonzeroId(g_nextMoveJobId);
    job.token.itemIndex=restore.liveIndex;
    job.runtimeKey=runtimeKey;
    job.recordId=saved.recordId;
    job.destination=restore.destination;
    MoveRuntimeBinding runtime;
    runtime.window=fast;
    runtime.destination=restore.destination;
    runtime.budgetKey=std::move(budget);
    runtime.hasBudgetKey=true;
    ReservedAutoIdentity reservation;
    reservation.token=job.token;
    reservation.identity=IdentityOf(fast);
    reservation.app=fast.app;
    reservation.recordId=saved.recordId;
    reservation.originDesktop=fast.desktop;
    bool insertedRuntime=false,insertedReservation=false;
    bool insertedLiveJob=false,incrementedOutstanding=false;
    const bool published=RunFailureAtomicMoveSetup([&](){
        if(!g_moveRuntime.emplace(job.token.jobId,runtime).second) return false;
        insertedRuntime=true;
        if(!g_reservedAutoIdentities.emplace(runtimeKey,reservation).second)
            return false;
        insertedReservation=true;
        if(!operation.liveJobIds.insert(job.token.jobId).second) return false;
        insertedLiveJob=true;
        ++operation.outstanding;
        incrementedOutstanding=true;
        // Queue publication is deliberately last: no fallible owner update is
        // allowed to leave an enqueued job requiring an allocating cancel.
        return g_moveQueue.enqueue(job);
    },[&](){
        if(incrementedOutstanding && operation.outstanding>0)
            --operation.outstanding;
        if(insertedLiveJob) operation.liveJobIds.erase(job.token.jobId);
        if(insertedReservation){
            auto current=g_reservedAutoIdentities.find(runtimeKey);
            if(current!=g_reservedAutoIdentities.end() &&
               SameMoveToken(current->second.token,job.token))
                g_reservedAutoIdentities.erase(current);
        }
        if(insertedRuntime) g_moveRuntime.erase(job.token.jobId);
    });
    return published;
}

static void HandleManualReconcileResult(std::unique_ptr<ReconcileResult> result);
static void HandleManualSavePreparedResult(std::unique_ptr<ReconcileResult> result);
static void HandleSearchReconcileResult(std::unique_ptr<ReconcileResult> result);

static void HandleReconcileResult(std::unique_ptr<ReconcileResult> result){
    if(!result) return;
    auto operation=g_pendingAutoOperations.find(result->operationId);
    if(operation==g_pendingAutoOperations.end()){
        if(g_manualSaveOperations.count(result->operationId)){
            HandleManualSavePreparedResult(std::move(result));
            return;
        }
        if(g_searchOperations.count(result->operationId)){
            HandleSearchReconcileResult(std::move(result));
            return;
        }
        HandleManualReconcileResult(std::move(result));
        return;
    }
    const uint64_t operationId=result->operationId;
    if(operation->second.cancellationPending) return;
    if(!operation->second.reconcilePending ||
       operation->second.reconcileMode!=result->workMode) return;
    operation->second.reconcilePending=false;
    g_reconcileDeadlines.complete(operationId);
    ReconcileResultConsumerKey expected;
    expected.operationId=operationId;
    expected.app=operation->second.app;
    expected.workMode=operation->second.reconcileMode;
    expected.identityGeneration=operation->second.identityGeneration;
    expected.contentGeneration=operation->second.contentGeneration;
    expected.sessionRequestId=operation->second.sessionRequestId;
    expected.sessionDataGeneration=operation->second.sessionDataGeneration;
    if(!ReconcileResultIsCurrent(*result,expected)){
        CancelAutoOperation(operationId,true);
        return;
    }
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    auto current=snapshots.find(result->app);
    if(current==snapshots.end() || !FastSnapshotCanPersistAll(current->second) ||
       current->second.identityGeneration!=result->identityGeneration ||
       current->second.generation!=result->contentGeneration){
        CancelAutoOperation(operationId,true);
        return;
    }
    if(result->workMode==ReconcileWorkMode::PrepareLiveOnly){
        if(!result->buildLiveFromInputs ||
           result->fastWindows.size()!=result->live.size()){
            CancelAutoOperation(operationId,true);
            return;
        }
        AppFastSnapshot prepared=current->second;
        prepared.windows=result->fastWindows;
        std::set<std::string> reserved=UpdateBoundRecords(
            result->app,prepared,result->live,result->freshness,result->nowUtc);
        for(const auto& entry : g_reservedAutoIdentities)
            if(entry.second.app==result->app && !entry.second.recordId.empty())
                reserved.insert(entry.second.recordId);

        std::vector<LayoutWin> unboundLive;
        std::vector<FastWin> unboundFast;
        try {
            for(size_t index=0;index<result->fastWindows.size();++index){
                const FastWin& fast=result->fastWindows[index];
                auto bound=g_recordByRuntime.find(RuntimeKey(fast));
                if(bound!=g_recordByRuntime.end() &&
                   SameIdentity(bound->second.identity,IdentityOf(fast))) continue;
                unboundFast.push_back(fast);
                unboundLive.push_back(result->live[index]);
            }
        } catch(...) {
            CancelAutoOperation(operationId,true);
            return;
        }
        operation=g_pendingAutoOperations.find(operationId);
        if(operation==g_pendingAutoOperations.end()) return;
        ReconcileRequest plan;
        plan.operationId=operationId;
        plan.app=result->app;
        plan.identityGeneration=result->identityGeneration;
        plan.contentGeneration=result->contentGeneration;
        plan.sessionRequestId=result->sessionRequestId;
        plan.sessionDataGeneration=result->sessionDataGeneration;
        plan.nowUtc=result->nowUtc;
        plan.freshness=result->freshness;
        try {
            operation->second.reconcileFast=unboundFast;
            plan.saved=g_autoRecords;
            plan.live.swap(unboundLive);
            plan.reservedRecordIds.swap(reserved);
        } catch(...) {
            CancelAutoOperation(operationId,true);
            return;
        }
        operation->second.reconcilePending=true;
        operation->second.reconcileMode=ReconcileWorkMode::Plan;
        if(!RequestReconcileWork(plan)){
            operation->second.reconcilePending=false;
            CancelAutoOperation(operationId,true);
        }
        return;
    }
    if(result->workMode!=ReconcileWorkMode::Plan || result->buildLiveFromInputs){
        CancelAutoOperation(operationId,true);
        return;
    }
    if(result->plan.deferred){
        const bool tooComplex=result->plan.tooComplex;
        operation->second.reconcile=std::move(result);
        if(tooComplex)
            ReportStorageError(L"Window reconciliation was deferred because the candidate set is too complex.");
        FinishAutoOperation(operationId);
        return;
    }

    if(!operation->second.successfulLive.initialize(
            operation->second.reconcileFast.size())){
        CancelAutoOperation(operationId,true);
        return;
    }
    operation->second.reconcile=std::move(result);
    ReconcileResult& accepted=*operation->second.reconcile;
    for(const RestoreRequest& restore : accepted.plan.restores){
        if(restore.liveIndex>=operation->second.reconcileFast.size()){
            operation->second.hadFailure=true;
            continue;
        }
        const FastWin& fast=operation->second.reconcileFast[restore.liveIndex];
        if(GuidEq(fast.desktop,restore.destination)){
            if(!operation->second.successfulLive.markSucceeded(restore.liveIndex))
                operation->second.hadFailure=true;
            continue;
        }
        if(!QueueAutoMove(operation->second,accepted,restore))
            operation->second.hadFailure=true;
    }
    if(operation->second.outstanding==0) FinishAutoOperation(operationId);
    else ArmMoveTimer();
}

static std::vector<FinalAppObservation> BuildFinalObservations(
        const std::map<std::string,AppFastSnapshot>& snapshots,
        UnixSeconds nowUtc,
        std::map<std::string,std::string>& provisionalRecordByRuntime){
    std::vector<FinalAppObservation> observations;
    const std::vector<AppProfile> allProfiles=BuiltinProfiles(true,true,true);
    observations.reserve(allProfiles.size());
    for(const AppProfile& profile : allProfiles){
        FinalAppObservation app;
        app.app=profile.id;
        auto snapshot=snapshots.find(profile.id);
        if(snapshot==snapshots.end()){
            app.quality=FinalProfileQuality::Disabled;
            observations.push_back(std::move(app));
            continue;
        }
        if(!snapshot->second.enumerationComplete){
            app.quality=FinalProfileQuality::Incomplete;
            observations.push_back(std::move(app));
            continue;
        }
        app.quality=FinalProfileQuality::Complete;
        for(size_t index=0;index<snapshot->second.windows.size();++index){
            const FastWin& fast=snapshot->second.windows[index];
            const std::string runtime=RuntimeKey(fast);
            FinalWindowObservation window;
            window.observed.app=profile.id;
            window.observed.desktop=fast.desktop;
            window.observed.deskIndex=SnapshotDesktopIndex(fast.desktop);
            window.observed.activeTitle=W2U8(
                StripSuffixes(fast.title,profile.titleSuffixes));
            window.desktopValid=!GuidIsZero(fast.desktop);
            window.fingerprintFresh=false;
            auto bound=g_recordByRuntime.find(runtime);
            if(bound!=g_recordByRuntime.end() &&
               SameIdentity(bound->second.identity,IdentityOf(fast)))
                window.boundRecordId=bound->second.recordId;
            auto pending=g_pendingRecordByRuntime.find(runtime);
            if(pending!=g_pendingRecordByRuntime.end())
                window.pendingRecordId=pending->second;
            auto provisional=provisionalRecordByRuntime.find(runtime);
            if(provisional==provisionalRecordByRuntime.end()){
                std::string id=NewRecordId();
                GUID parsed{};
                if(ParseNonzeroLayoutGuid(id,parsed)){
                    provisionalRecordByRuntime[runtime]=id;
                    window.provisionalRecordId=id;
                }
            } else window.provisionalRecordId=provisional->second;
            auto reservation=g_reservedAutoIdentities.find(runtime);
            if(reservation!=g_reservedAutoIdentities.end() &&
               SameIdentity(reservation->second.identity,IdentityOf(fast))){
                window.reserved=true;
                if(window.boundRecordId.empty())
                    window.boundRecordId=reservation->second.recordId;
                window.hasProvisionalOriginRecord=
                    reservation->second.hasProvisionalOriginRecord;
                window.provisionalOriginRecord=
                    reservation->second.provisionalOriginRecord;
            }
            app.windows.push_back(std::move(window));
        }
        observations.push_back(std::move(app));
    }
    (void)nowUtc;
    return observations;
}

static bool CommitFinalSnapshots(
        const std::map<std::string,AppFastSnapshot>& snapshots){
    const UnixSeconds nowUtc=UtcNowSeconds();
    const uint64_t checkpointGeneration=TakeNonzeroId(g_nextOperationId);
    std::vector<FinalAppObservation> observations;
    std::map<std::string,std::string> stagedProvisional;
    if(!StageFinalObservationsAndProvisionals(
            g_provisionalRecordByRuntime,observations,stagedProvisional,
            [&](std::map<std::string,std::string>& provisionals,
                std::vector<FinalAppObservation>& stagedObservations){
                stagedObservations=BuildFinalObservations(
                    snapshots,nowUtc,provisionals);
                return true;
            })) return false;
    FinalSnapshotResult finalResult;
    try {
        finalResult=CommitFinalSnapshotRecords(
            g_autoRecords,observations,nowUtc);
    } catch(...) { return false; }
    if(!finalResult.valid) return false;

    FinalCheckpointMutationState staged;
    std::map<std::string,RuntimeRecordBinding> stagedBindings;
    try {
        std::vector<ValidatedRecordTouch> touches;
        touches.reserve(g_recordByRuntime.size());
        for(const auto& entry : g_recordByRuntime){
            auto record=std::find_if(finalResult.records.begin(),
                finalResult.records.end(),[&](const LayoutWin& candidate){
                    return candidate.recordId==entry.second.recordId;
                });
            if(record==finalResult.records.end()) continue;
            ValidatedRecordTouch touch;
            touch.recordId=record->recordId;
            touch.lastSeenUtc=record->lastSeenUtc;
            touch.causalGeneration=entry.second.causalGeneration;
            if(touch.causalGeneration!=0) touches.push_back(std::move(touch));
        }
        if(!BuildFinalCheckpointMutation(
                g_autoRecords,finalResult.records,g_dirtyRecordDeltas,
                g_validatedTouches,g_deferredRecordConflicts,touches,
                stagedProvisional,g_autoRevision,nowUtc,
                checkpointGeneration,staged))
            return false;

        stagedBindings=g_recordByRuntime;
        for(const auto& provisional : staged.provisionalRecordByRuntime){
            auto record=std::find_if(staged.records.begin(),staged.records.end(),
                [&](const LayoutWin& candidate){
                    return candidate.recordId==provisional.second;
                });
            if(record==staged.records.end()) continue;
            auto observationsByApp=snapshots.find(record->app);
            if(observationsByApp==snapshots.end()) continue;
            for(const FastWin& fast : observationsByApp->second.windows)
                if(RuntimeKey(fast)==provisional.first){
                    RuntimeRecordBinding binding;
                    binding.app=record->app;
                    binding.recordId=record->recordId;
                    binding.identity=IdentityOf(fast);
                    binding.causalGeneration=observationsByApp->second.generation;
                    stagedBindings[provisional.first]=std::move(binding);
                    break;
                }
        }
    } catch(...) { return false; }

    g_autoRecords.swap(staged.records);
    g_dirtyRecordDeltas.swap(staged.deltas);
    g_validatedTouches.swap(staged.touches);
    g_deferredRecordConflicts.swap(staged.conflicts);
    g_recordByRuntime.swap(stagedBindings);
    g_provisionalRecordByRuntime.swap(
        staged.provisionalRecordByRuntime);
    MarkAutoDirty(false);
    PruneStaleRuntimeState(snapshots);
    return true;
}

static bool ExecuteCheckpoint(CheckpointReason){
    if(!g_autoFix || g_degraded || !g_autoLoaded || !g_autoWritesAllowed)
        return !g_autoFix || g_degraded;
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    if(!CommitFinalSnapshots(snapshots)) return false;
    return FlushAutoLayout(true);
}

static bool CheckpointAutoLayout(CheckpointReason reason){
    try {
        return g_checkpointController.dispatch(reason,g_autoFix && !g_degraded,
            g_autoLoaded,!g_reservedAutoIdentities.empty(),
            [](CheckpointReason current){ return ExecuteCheckpoint(current); });
    } catch(...) { return false; }
}

static bool FinalizeAutoLayout(){
    return CheckpointAutoLayout(CheckpointReason::Finalize);
}

static bool TryLoadAutoLayoutAndInitialize(){
    const uint64_t nowMs=MonotonicNowMs();
    const AutoRuntimeStartResult result=AdvanceAutoRuntimeStart(
        g_autoLoadRetry,nowMs,
        [](){
            return g_main &&
                SetTimer(g_main,TIMER_MONITOR,MONITOR_INTERVAL_MS,nullptr)!=0;
        },
        [](){ return MigrateLegacyLayout() && LoadAutoLayout(); },
        [](){
            try {
                const std::vector<AppProfile> profiles=ActiveProfiles();
                const std::map<std::string,AppFastSnapshot> snapshots=
                    CollectFastSnapshots(profiles);
                InitialLifecyclePreparation prepared;
                if(!PrepareInitialLifecycleStates(
                        profiles,snapshots,MonotonicNowMs(),prepared))
                    return false;
                std::map<std::string,uint64_t> missingGenerations;
                for(const InitialMissingApp& missing : prepared.missingApps)
                    if(!missingGenerations.emplace(
                            missing.app,missing.generation).second)
                        return false;
                FinalCheckpointMutationState mutation;
                const UnixSeconds nowUtc=UtcNowSeconds();
                if(!BuildInitialMissingMutation(
                        g_autoRecords,g_dirtyRecordDeltas,g_validatedTouches,
                        g_deferredRecordConflicts,
                        g_provisionalRecordByRuntime,g_autoRevision,
                        missingGenerations,nowUtc,mutation)) return false;
                bool changed=mutation.records.size()!=g_autoRecords.size();
                for(size_t index=0;!changed && index<mutation.records.size();++index)
                    changed=!SameRecordForDelta(
                        mutation.records[index],g_autoRecords[index]);
                g_autoRecords.swap(mutation.records);
                g_dirtyRecordDeltas.swap(mutation.deltas);
                g_validatedTouches.swap(mutation.touches);
                g_deferredRecordConflicts.swap(mutation.conflicts);
                g_provisionalRecordByRuntime.swap(
                    mutation.provisionalRecordByRuntime);
                g_lifecycleByApp.swap(prepared.states);
                g_lastFreshSessionSignature.swap(prepared.signatures);
                if(changed) MarkAutoDirty(false);
                return true;
            } catch(...) { return false; }
        },
        [](){
            g_heartbeatTimerArmed=g_main &&
                SetTimer(g_main,TIMER_HEARTBEAT,
                         HEARTBEAT_INTERVAL_MS,nullptr)!=0;
            return g_heartbeatTimerArmed;
        },
        [](){
            return g_main &&
                PostMessageW(g_main,WM_AUTO_TIMER_RETRY,0,0)!=FALSE;
        });
    if(result==AutoRuntimeStartResult::Ready){
        g_autoLoaded=true;
        if(g_autoDirty) ScheduleAutoFlush();
        return true;
    }
    if(g_autoLoadRetry.layoutPrepared){
        g_autoLoaded=false;
        KillTimer(g_main,TIMER_AUTO_FLUSH);
        g_flushTimerArmed=false;
        g_flushTimerDueMs=0;
    }
    if(result==AutoRuntimeStartResult::MonitorUnavailable)
        ReportStorageError(L"Automatic monitoring could not be started; startup will retry without publishing loaded state.");
    else if(result==AutoRuntimeStartResult::HeartbeatUnavailable)
        ReportStorageError(L"Automatic heartbeat timer could not be started; startup will retry without publishing loaded state.");
    else if(result==AutoRuntimeStartResult::InitializationUnavailable)
        ReportStorageError(L"Automatic layout initialization could not complete; startup will retry.");
    return false;
}

static void ResetAutoRuntimeState(){
    std::vector<uint64_t> operations;
    operations.reserve(g_pendingAutoOperations.size());
    for(const auto& entry : g_pendingAutoOperations)
        operations.push_back(entry.first);
    for(uint64_t operationId : operations) CancelAutoOperation(operationId,false);
    operations.clear();
    for(const auto& entry : g_metadataProbeOperations)
        operations.push_back(entry.first);
    for(uint64_t operationId : operations)
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::MetadataProbe,operationId);
    g_metadataProbeOperations.clear();
    for(;;){
        auto route=std::find_if(g_sessionRoutes.begin(),g_sessionRoutes.end(),
            [](const std::pair<const uint64_t,SessionRoute>& item){
                return item.second.owner==AsyncOperationOwner::AutoReconcile;
            });
        if(route==g_sessionRoutes.end()) break;
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::AutoReconcile,route->second.operationId);
    }
    g_recordByRuntime.clear();
    g_pendingRecordByRuntime.clear();
    g_provisionalRecordByRuntime.clear();
    g_lifecycleByApp.clear();
    g_lastFreshSessionSignature.clear();
    g_dirtyRecordDeltas.clear();
    g_validatedTouches.clear();
    g_deferredRecordConflicts.clear();
    ClearLayoutPublishCandidateNoThrow(g_pendingAutoPublishCandidate);
    g_autoDirty=false;
    g_dirtyFlush.clearDirty();
    g_flushTimerArmed=false;
    g_flushTimerDueMs=0;
    g_autoLoaded=false;
    g_autoWritesAllowed=false;
    g_autoLoadRetry.reset();
}

static bool SameDesktopSnapshot(const std::vector<DeskRec>& left,
                                const std::vector<DeskRec>& right){
    if(left.size()!=right.size()) return false;
    for(size_t index=0;index<left.size();++index)
        if(left[index].index!=right[index].index ||
           !GuidEq(left[index].guid,right[index].guid)) return false;
    return true;
}

struct ReservationHandoff {
    std::string runtimeKey;
    ReservedAutoIdentity installed;
    ReservedAutoIdentity displaced;
    ReservedAutoIdentity publishValue;
    MoveResult rollbackResult;
    ReservedAutoIdentity* guardSlot=nullptr;
    bool hadDisplaced=false;
    bool committed=false;
    bool active=false;
};

static void SwapLayoutWinNoThrow(LayoutWin& left,LayoutWin& right) noexcept {
    left.recordId.swap(right.recordId);
    left.app.swap(right.app);
    std::swap(left.deskIndex,right.deskIndex);
    const GUID desktop=left.desktop;
    left.desktop=right.desktop;
    right.desktop=desktop;
    left.activeTitle.swap(right.activeTitle);
    left.activeDomain.swap(right.activeDomain);
    std::swap(left.tabCount,right.tabCount);
    left.counts.swap(right.counts);
    std::swap(left.lastSeenUtc,right.lastSeenUtc);
    std::swap(left.missingSinceUtc,right.missingSinceUtc);
    std::swap(left.provisional,right.provisional);
}

static void SwapReservedAutoIdentityNoThrow(
        ReservedAutoIdentity& left,ReservedAutoIdentity& right) noexcept {
    const MoveToken token=left.token;
    left.token=right.token;
    right.token=token;
    const WindowIdentityKey identity=left.identity;
    left.identity=right.identity;
    right.identity=identity;
    left.app.swap(right.app);
    left.recordId.swap(right.recordId);
    const GUID origin=left.originDesktop;
    left.originDesktop=right.originDesktop;
    right.originDesktop=origin;
    SwapLayoutWinNoThrow(
        left.provisionalOriginRecord,right.provisionalOriginRecord);
    std::swap(left.hasProvisionalOriginRecord,
              right.hasProvisionalOriginRecord);
}

static bool BeginReservationHandoff(
        const std::string& runtimeKey,
        const ReservedAutoIdentity& replacement,
        ReservationHandoff& handoff){
    try {
        ReservationHandoff next;
        next.runtimeKey=runtimeKey;
        auto prior=g_reservedAutoIdentities.find(runtimeKey);
        ReservedAutoIdentity installed=replacement;
        if(prior!=g_reservedAutoIdentities.end() &&
           SameIdentity(prior->second.identity,replacement.identity)){
            if(!prior->second.recordId.empty())
                installed.recordId=prior->second.recordId;
            if(!GuidIsZero(prior->second.originDesktop))
                installed.originDesktop=prior->second.originDesktop;
            if(prior->second.hasProvisionalOriginRecord){
                installed.provisionalOriginRecord=
                    prior->second.provisionalOriginRecord;
                installed.hasProvisionalOriginRecord=true;
            }
            if(installed.app.empty()) installed.app=prior->second.app;
        }
        next.installed=installed;
        if(prior==g_reservedAutoIdentities.end()){
            if(!PrepareCancelledMoveResult(next.installed.token,runtimeKey,
                                           next.installed.recordId,
                                           next.rollbackResult)) return false;
            handoff=std::move(next);
            auto inserted=g_reservedAutoIdentities.emplace(runtimeKey,installed);
            if(!inserted.second)
                return false;
            handoff.guardSlot=&inserted.first->second;
            handoff.active=true;
            return true;
        }
        next.displaced=prior->second;
        next.publishValue=installed;
        next.guardSlot=&prior->second;
        next.hadDisplaced=true;
        next.active=true;
        handoff=std::move(next);
        return true;
    } catch(...) { return false; }
}

static void PublishReservationHandoff(ReservationHandoff& handoff) noexcept {
    if(!handoff.active || handoff.committed) return;
    if(handoff.hadDisplaced && handoff.guardSlot)
        SwapReservedAutoIdentityNoThrow(
            *handoff.guardSlot,handoff.publishValue);
    handoff.committed=true;
}

static void CancelDisplacedReservationHandoff(ReservationHandoff& handoff){
    if(!handoff.active || !handoff.committed) return;
    handoff.active=false;
    if(handoff.hadDisplaced)
        CancelMoveJobOrDefer(handoff.displaced.token.jobId);
}

static void RollbackReservationHandoff(ReservationHandoff& handoff){
    if(!handoff.active || handoff.committed) return;
    if(handoff.hadDisplaced){
        // Nothing has been published yet: the displaced issued job, owner,
        // runtime, and exact stable-origin guard remain untouched.
        handoff.active=false;
        return;
    }
    auto current=g_reservedAutoIdentities.find(handoff.runtimeKey);
    if(current==g_reservedAutoIdentities.end() ||
       !SameMoveToken(current->second.token,handoff.installed.token)){
        handoff.active=false;
        return;
    }
    // No move was enqueued, but a deferred checkpoint may have observed this
    // new guard.  Consume it once while the captured origin remains visible.
    ConsumeCheckpointAndReleaseMoveReservation(handoff.rollbackResult);
    handoff.active=false;
}

static void RetireSessionRoutesForOperation(AsyncOperationOwner owner,
                                             uint64_t operationId){
    SessionPurpose purpose=SessionPurpose::MetadataProbe;
    if(SessionPurposeForOwner(owner,purpose)){
        std::vector<AsyncSessionRetirement> ignored;
        g_sessionRouteGate.cancelOperation(purpose,operationId,ignored);
    }
    for(auto route=g_sessionRoutes.begin();route!=g_sessionRoutes.end();)
        if(route->second.owner==owner && route->second.operationId==operationId)
            route=g_sessionRoutes.erase(route);
        else ++route;
}

static void FinishManualSave(uint64_t operationId){
    auto found=g_manualSaveOperations.find(operationId);
    if(found==g_manualSaveOperations.end() || found->second.outstanding!=0) return;
    ManualSaveOperation operation=std::move(found->second);
    g_manualSaveOperations.erase(found);
    g_reconcileDeadlines.cancel(operationId);
    if(operation.completionReported) return;
    auto fail=[&](const wchar_t* message){ Balloon(message); };
    if(operation.failed){
        fail(L"Manual layout was not saved because the snapshot changed or browser data was incomplete. Retry.");
        return;
    }
    std::map<std::string,AppFastSnapshot> current=
        CollectFastSnapshots(operation.profiles.all());
    std::vector<DeskRec> currentDesktops;
    std::string error;
    if(!CurrentDesktops(currentDesktops,&error) ||
       !SameDesktopSnapshot(currentDesktops,operation.desktops)){
        fail(L"Manual layout was not saved because the desktop set changed. Retry.");
        return;
    }
    std::vector<LayoutWin> records;
    std::set<std::string> ids;
    for(const AppProfile& profile : operation.profiles.all()){
        auto captured=operation.snapshots.find(profile.id);
        auto now=current.find(profile.id);
        if(captured==operation.snapshots.end() || now==current.end() ||
           captured->second.identityGeneration!=now->second.identityGeneration ||
           captured->second.generation!=now->second.generation ||
           !FastSnapshotCanPersistAll(now->second)){
            fail(L"Manual layout was not saved because the window snapshot changed. Retry.");
            return;
        }
        if(captured->second.windows.empty()) continue;
        auto prepared=operation.preparedLive.find(profile.id);
        if(prepared==operation.preparedLive.end()){
            fail(L"Manual layout was not saved because fresh browser data was unavailable.");
            return;
        }
        for(LayoutWin record : prepared->second){
            record.recordId=NewRecordId();
            GUID id{};
            std::string canonical;
            if(GuidIsZero(record.desktop) ||
               !ParseNonzeroLayoutGuid(record.recordId,id,&canonical) ||
               !ids.insert(canonical).second){
                fail(L"Manual layout could not allocate stable record identities.");
                return;
            }
            record.recordId=canonical;
            MarkSeen(record,UtcNowSeconds());
            record.missingSinceUtc=0;
            records.push_back(std::move(record));
        }
    }
    if(records.empty()){
        fail(L"No browser windows found. Nothing was saved.");
        return;
    }
    std::string bytes;
    std::vector<LayoutWin> checked=records;
    if(!BuildCheckedLayoutSnapshot(
            currentDesktops,checked,UtcNowSeconds(),bytes,&error)){
        fail(L"Manual layout validation failed; the previous checkpoint was kept.");
        return;
    }
    ScopedLayoutLock lock;
    if(!lock.acquired()){
        fail(L"Manual layout storage is busy; the previous checkpoint was kept.");
        return;
    }
    const bool published=PublishManualSnapshotIfCurrent(
        operation.snapshots,current,operation.desktops,currentDesktops,bytes,
        [&](const std::string& checkedBytes){
            LayoutLoadResult prior=LoadLayoutWithBackupLocked(
                LayoutPath(true),UtcNowSeconds());
            return prior.status!=LayoutLoadStatus::Unavailable &&
                AtomicWriteText(LayoutPath(true),checkedBytes,&error,
                                prior.status==LayoutLoadStatus::Recovered);
        });
    if(!published){
        fail(L"Manual layout write failed; the previous checkpoint was kept.");
        return;
    }
    wchar_t message[160]={0};
    swprintf_s(message,L"Saved manual layout: %u window(s).",
               static_cast<unsigned>(records.size()));
    Balloon(message);
}

static void CancelManualSaveOperation(uint64_t operationId){
    g_reconcileDeadlines.cancel(operationId);
    RetireSessionRoutesForOperation(AsyncOperationOwner::ManualSave,operationId);
    g_manualSaveOperations.erase(operationId);
}

static void StartManualSave(){
    if(g_degraded || !g_sessionWorker){
        Balloon(L"Virtual-desktop or browser-session services are unavailable.");
        return;
    }
    std::vector<uint64_t> old;
    for(const auto& entry : g_manualSaveOperations) old.push_back(entry.first);
    for(uint64_t id : old) CancelManualSaveOperation(id);
    ManualSaveOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    operation.profiles=OperationAppProfiles(ActiveProfiles());
    operation.snapshots=CollectFastSnapshots(operation.profiles.all());
    std::string error;
    if(!CurrentDesktops(operation.desktops,&error)){
        Balloon(L"Manual layout was not saved because desktops could not be collected.");
        return;
    }
    bool any=false;
    for(const auto& entry : operation.snapshots){
        if(!FastSnapshotCanPersistAll(entry.second)){
            Balloon(L"Manual layout was not saved because enumeration or desktop lookup was incomplete.");
            return;
        }
        any=any || !entry.second.windows.empty();
    }
    if(!any){ Balloon(L"No browser windows found. Nothing was saved."); return; }
    const uint64_t operationId=operation.operationId;
    try {
        if(!g_manualSaveOperations.emplace(operationId,operation).second) return;
    } catch(...) { Balloon(L"Manual save could not be started."); return; }
    bool failed=false;
    for(const AppProfile& profile : operation.profiles.all()){
        const AppFastSnapshot& snapshot=operation.snapshots[profile.id];
        if(snapshot.windows.empty()) continue;
        uint64_t request=RequestSessionWork(
            AsyncOperationOwner::ManualSave,operationId,profile,snapshot,
            SessionPurpose::ManualSave);
        if(!request){ failed=true; break; }
        ++g_manualSaveOperations[operationId].outstanding;
    }
    if(failed){
        RetireSessionRoutesForOperation(AsyncOperationOwner::ManualSave,operationId);
        g_manualSaveOperations[operationId].failed=true;
        g_manualSaveOperations[operationId].outstanding=0;
    }
    FinishManualSave(operationId);
}

static void HandleManualSaveSessionResult(const SessionRoute& route,
                                           const SessionResult& result){
    auto operation=g_manualSaveOperations.find(route.operationId);
    if(operation==g_manualSaveOperations.end()) return;
    bool accepted=result.status==SessionDataStatus::Fresh && result.windows &&
        result.dataGeneration!=0;
    std::map<std::string,AppFastSnapshot> current=
        CollectFastSnapshots(operation->second.profiles.all());
    auto captured=operation->second.snapshots.find(route.app);
    auto now=current.find(route.app);
    accepted=accepted && captured!=operation->second.snapshots.end() &&
        now!=current.end() && FastSnapshotCanPersistAll(now->second) &&
        captured->second.identityGeneration==now->second.identityGeneration &&
        captured->second.generation==now->second.generation &&
        route.contentGeneration==now->second.generation;
    if(accepted){
        const AppProfile* profile=operation->second.profiles.find(route.app);
        ReconcileRequest request;
        request.operationId=route.operationId;
        request.app=route.app;
        request.identityGeneration=now->second.identityGeneration;
        request.contentGeneration=now->second.generation;
        request.sessionRequestId=result.requestId;
        request.sessionDataGeneration=result.dataGeneration;
        request.nowUtc=UtcNowSeconds();
        request.freshness=ReconcileFreshness::Fresh;
        try {
            if(profile) ConfigureWorkerLiveBuild(
                request,ReconcileWorkMode::PrepareLiveOnly,*profile,
                captured->second,result.windows,operation->second.desktops);
            else accepted=false;
        } catch(...) { accepted=false; }
        if(accepted){
            try {
                accepted=operation->second.waitingReconcileApps.insert(
                    route.app).second;
            } catch(...) { accepted=false; }
        }
        if(accepted && RequestReconcileWork(request)) return;
        operation->second.waitingReconcileApps.erase(route.app);
        accepted=false;
    }
    operation->second.failed=true;
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.failed){
        g_reconcileDeadlines.cancel(route.operationId);
        operation->second.waitingReconcileApps.clear();
        RetireSessionRoutesForOperation(AsyncOperationOwner::ManualSave,route.operationId);
        operation->second.outstanding=0;
    }
    FinishManualSave(route.operationId);
}

static void HandleManualSavePreparedResult(
        std::unique_ptr<ReconcileResult> result){
    if(!result) return;
    auto operation=g_manualSaveOperations.find(result->operationId);
    if(operation==g_manualSaveOperations.end() ||
       operation->second.outstanding==0 ||
       operation->second.waitingReconcileApps.erase(result->app)==0) return;
    g_reconcileDeadlines.complete(result->operationId);
    std::map<std::string,AppFastSnapshot> current=
        CollectFastSnapshots(operation->second.profiles.all());
    auto captured=operation->second.snapshots.find(result->app);
    auto now=current.find(result->app);
    const bool accepted=
        result->status==ReconcileResultStatus::Completed &&
        result->workMode==ReconcileWorkMode::PrepareLiveOnly &&
        result->buildLiveFromInputs &&
        result->freshness==ReconcileFreshness::Fresh &&
        captured!=operation->second.snapshots.end() && now!=current.end() &&
        FastSnapshotCanPersistAll(now->second) &&
        result->identityGeneration==now->second.identityGeneration &&
        result->contentGeneration==now->second.generation &&
        result->fastWindows.size()==result->live.size();
    if(accepted){
        try { operation->second.preparedLive[result->app]=result->live; }
        catch(...) { operation->second.failed=true; }
    } else operation->second.failed=true;
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.failed){
        g_reconcileDeadlines.cancel(result->operationId);
        operation->second.waitingReconcileApps.clear();
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::ManualSave,result->operationId);
        operation->second.outstanding=0;
    }
    FinishManualSave(result->operationId);
}

static void FinishManualMove(uint64_t operationId){
    auto found=g_manualMoveOperations.find(operationId);
    if(found!=g_manualMoveOperations.end() &&
       found->second.cancellationPending){
        if(found->second.liveJobIds.empty())
            g_manualMoveOperations.erase(found);
        return;
    }
    if(found==g_manualMoveOperations.end() || found->second.outstanding!=0 ||
       !found->second.waitingSessionApps.empty() ||
       !found->second.waitingReconcileApps.empty()) return;
    const size_t succeeded=found->second.succeeded;
    const size_t failed=found->second.failed;
    const size_t already=found->second.already;
    g_manualMoveOperations.erase(found);
    g_reconcileDeadlines.cancel(operationId);
    wchar_t message[220]={0};
    if(succeeded==0 && failed==0)
        swprintf_s(message,L"Restore: nothing to move; %u matched window(s) were already in place.",
                   static_cast<unsigned>(already));
    else
        swprintf_s(message,L"Restore: moved %u, %u already in place, %u failed.",
                   static_cast<unsigned>(succeeded),
                   static_cast<unsigned>(already),
                   static_cast<unsigned>(failed));
    Balloon(message);
}

static void CancelManualMoveOperation(uint64_t operationId){
    g_reconcileDeadlines.cancel(operationId);
    auto found=g_manualMoveOperations.find(operationId);
    if(found==g_manualMoveOperations.end()) return;
    if(found->second.cancellationPending){
        RefreshMoveCancellationPending();
        ScheduleMoveCancellationRetry();
        return;
    }
    uint64_t jobIds[MAX_LAYOUT_RECORDS]={0};
    size_t jobCount=0;
    for(uint64_t jobId : found->second.liveJobIds)
        if(jobCount<MAX_LAYOUT_RECORDS) jobIds[jobCount++]=jobId;
    PublishMoveCancellationIntent(found->second.cancellationPending,
        jobIds,jobCount,[&](uint64_t jobId) noexcept {
            if(MarkMoveForTerminalRetirement(jobId)) return;
            auto runtime=g_moveRuntime.find(jobId);
            if(runtime!=g_moveRuntime.end()) runtime->second.cancelRequested=true;
        });
    RefreshMoveCancellationPending();
    found->second.waitingSessionApps.clear();
    found->second.waitingReconcileApps.clear();
    found->second.snapshots.clear();
    found->second.currentDesktops.clear();
    found->second.saved.clear();
    RetireSessionRoutesForOperation(AsyncOperationOwner::ManualRestore,operationId);
    for(size_t index=0;index<jobCount;++index)
        CancelMoveJobOrDefer(jobIds[index]);
    found=g_manualMoveOperations.find(operationId);
    if(found!=g_manualMoveOperations.end() && found->second.liveJobIds.empty())
        g_manualMoveOperations.erase(found);
}

static void RetireAsyncSessionOperation(const SessionRoute& route,
                                        AsyncRetirementReason reason){
    if(route.operationId==0) return;
    if(route.owner==AsyncOperationOwner::AutoReconcile){
        CancelAutoOperation(route.operationId,true);
        return;
    }
    if(route.owner==AsyncOperationOwner::ManualSave){
        auto operation=g_manualSaveOperations.find(route.operationId);
        if(operation==g_manualSaveOperations.end()) return;
        operation->second.failed=true;
        operation->second.outstanding=0;
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::ManualSave,route.operationId);
        FinishManualSave(route.operationId);
        return;
    }
    if(route.owner==AsyncOperationOwner::ManualRestore){
        if(g_manualMoveOperations.count(route.operationId)==0) return;
        CancelManualMoveOperation(route.operationId);
        if(reason!=AsyncRetirementReason::Cancelled)
            Balloon(L"Restore browser-session work was cancelled or timed out. Retry.");
        return;
    }
    if(route.owner==AsyncOperationOwner::MetadataProbe){
        g_metadataProbeOperations.erase(route.operationId);
        return;
    }
    auto operation=g_searchOperations.find(route.operationId);
    if(operation==g_searchOperations.end()) return;
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(route.operationId);
        g_searchOperations.erase(operation);
    }
}

static void ProcessSessionRetirements(
        const std::vector<AsyncSessionRetirement>& retired){
    for(const AsyncSessionRetirement& event : retired){
        SessionRoute route;
        auto owned=g_sessionRoutes.find(event.route.requestId);
        if(owned!=g_sessionRoutes.end()){
            route=owned->second;
            g_sessionRoutes.erase(owned);
        } else {
            AsyncOperationOwner owner=AsyncOperationOwner::Search;
            if(!SessionOwnerForPurpose(event.route.purpose,owner)) continue;
            route.owner=owner;
            route.operationId=event.route.operationId;
            route.app=event.route.app;
            route.purpose=event.route.purpose;
            route.identityGeneration=event.route.identityGeneration;
            route.deadlineMs=event.route.deadlineMs;
        }
        RetireAsyncSessionOperation(route,event.reason);
    }
}

static void RetireReconcileOperation(uint64_t operationId){
    if(operationId==0) return;
    g_reconcileDeadlines.cancel(operationId);
    if(g_pendingAutoOperations.count(operationId)){
        CancelAutoOperation(operationId,true);
        return;
    }
    auto manualSave=g_manualSaveOperations.find(operationId);
    if(manualSave!=g_manualSaveOperations.end()){
        manualSave->second.failed=true;
        manualSave->second.outstanding=0;
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::ManualSave,operationId);
        FinishManualSave(operationId);
        return;
    }
    if(g_manualMoveOperations.count(operationId)){
        CancelManualMoveOperation(operationId);
        Balloon(L"Restore matching failed unexpectedly. Retry.");
        return;
    }
    auto search=g_searchOperations.find(operationId);
    if(search!=g_searchOperations.end()){
        RetireSessionRoutesForOperation(AsyncOperationOwner::Search,operationId);
        g_searchOperations.erase(search);
    }
}

static void ForceCancelAutoOperationNoThrow(uint64_t operationId) noexcept {
    try {
        auto operation=g_pendingAutoOperations.find(operationId);
        if(operation==g_pendingAutoOperations.end()) return;
        operation->second.cancellationPending=true;
        for(uint64_t jobId : operation->second.liveJobIds){
            if(MarkMoveForTerminalRetirement(jobId)) continue;
            auto runtime=g_moveRuntime.find(jobId);
            if(runtime!=g_moveRuntime.end()) runtime->second.cancelRequested=true;
        }
        if(operation->second.liveJobIds.empty()){
            auto lifecycle=g_lifecycleByApp.find(operation->second.app);
            if(lifecycle!=g_lifecycleByApp.end())
                LcCancelRestore(lifecycle->second,
                    operation->second.lifecycleGeneration,
                    MonotonicNowMs(),true);
            g_pendingAutoOperations.erase(operation);
        }
        RefreshMoveCancellationPending();
        ScheduleMoveCancellationRetry();
    } catch(...) {}
}

static void ForceCancelManualOperationNoThrow(uint64_t operationId) noexcept {
    try {
        auto operation=g_manualMoveOperations.find(operationId);
        if(operation==g_manualMoveOperations.end()) return;
        operation->second.cancellationPending=true;
        for(uint64_t jobId : operation->second.liveJobIds){
            if(MarkMoveForTerminalRetirement(jobId)) continue;
            auto runtime=g_moveRuntime.find(jobId);
            if(runtime!=g_moveRuntime.end()) runtime->second.cancelRequested=true;
        }
        operation->second.waitingSessionApps.clear();
        operation->second.waitingReconcileApps.clear();
        if(operation->second.liveJobIds.empty())
            g_manualMoveOperations.erase(operation);
        RefreshMoveCancellationPending();
        ScheduleMoveCancellationRetry();
    } catch(...) {}
}

static void RetireFailedSessionResult(uint64_t requestId) noexcept {
    if(requestId==0) return;
    auto found=g_sessionRoutes.find(requestId);
    if(found==g_sessionRoutes.end()) return;
    SessionRoute route;
    route.owner=found->second.owner;
    route.operationId=found->second.operationId;
    route.purpose=found->second.purpose;
    route.identityGeneration=found->second.identityGeneration;
    route.contentGeneration=found->second.contentGeneration;
    route.deadlineMs=found->second.deadlineMs;
    g_sessionRouteGate.abandon(
        requestId,route.operationId,route.identityGeneration);
    g_sessionRoutes.erase(found);
    try { RetireAsyncSessionOperation(route,AsyncRetirementReason::Failed); }
    catch(...) {
        // The route is no longer replayable.  Best-effort owner retirement is
        // contained so a reporting/allocation failure cannot unwind WndProc.
        try {
            if(route.owner==AsyncOperationOwner::AutoReconcile)
                ForceCancelAutoOperationNoThrow(route.operationId);
            else if(route.owner==AsyncOperationOwner::ManualRestore)
                ForceCancelManualOperationNoThrow(route.operationId);
            else if(route.owner==AsyncOperationOwner::ManualSave)
                g_manualSaveOperations.erase(route.operationId);
            else if(route.owner==AsyncOperationOwner::MetadataProbe)
                g_metadataProbeOperations.erase(route.operationId);
            else g_searchOperations.erase(route.operationId);
        } catch(...) {}
    }
}

static void RetireFailedReconcileResult(uint64_t operationId) noexcept {
    if(operationId==0) return;
    try { RetireReconcileOperation(operationId); }
    catch(...) {
        try {
            g_reconcileDeadlines.cancel(operationId);
            if(g_pendingAutoOperations.count(operationId))
                ForceCancelAutoOperationNoThrow(operationId);
            else if(g_manualMoveOperations.count(operationId))
                ForceCancelManualOperationNoThrow(operationId);
            else {
                g_manualSaveOperations.erase(operationId);
                g_searchOperations.erase(operationId);
            }
        } catch(...) {}
    }
}

static void CancelExpiredReconcileOperations(uint64_t nowMs){
    std::vector<uint64_t> expired;
    g_reconcileDeadlines.expire(nowMs,expired);
    for(uint64_t operationId : expired)
        RetireReconcileOperation(operationId);
}

static void StartManualRestore(bool manualSource){
    if(g_degraded || !g_sessionWorker || !g_reconcileWorker){
        Balloon(L"Virtual-desktop or browser-session services are unavailable.");
        return;
    }
    std::vector<uint64_t> old;
    for(const auto& entry : g_manualMoveOperations) old.push_back(entry.first);
    for(uint64_t id : old) CancelManualMoveOperation(id);
    LayoutLoadResult loaded=LoadLayoutWithBackup(
        LayoutPath(manualSource),UtcNowSeconds());
    if(!loaded.usable() || loaded.sourceVersion!=4 || loaded.wins.empty()){
        Balloon(manualSource
            ? L"No valid v4 manual layout is available. Save one first."
            : L"No valid v4 automatic layout is available.");
        return;
    }
    ManualMoveOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    operation.saved=loaded.wins;
    for(const LayoutWin& record : operation.saved)
        g_restoreBudgets.clearForExplicitRetry(record.recordId);
    operation.profiles=OperationAppProfiles(ActiveProfiles());
    operation.snapshots=CollectFastSnapshots(operation.profiles.all());
    std::string error;
    if(!CurrentDesktops(operation.currentDesktops,&error)){
        Balloon(L"Restore could not collect the current desktop set.");
        return;
    }
    bool any=false;
    for(const auto& entry : operation.snapshots){
        if(!FastSnapshotCanPersistAll(entry.second)){
            Balloon(L"Restore paused because window enumeration or desktop lookup was incomplete.");
            return;
        }
        any=any || !entry.second.windows.empty();
    }
    if(!any){ Balloon(L"No browser windows are open to restore."); return; }
    const uint64_t operationId=operation.operationId;
    try {
        if(!g_manualMoveOperations.emplace(operationId,operation).second) return;
    } catch(...) { Balloon(L"Restore could not be started."); return; }
    bool failed=false;
    for(const AppProfile& profile : operation.profiles.all()){
        const AppFastSnapshot& snapshot=operation.snapshots[profile.id];
        if(snapshot.windows.empty()) continue;
        uint64_t request=RequestSessionWork(
            AsyncOperationOwner::ManualRestore,operationId,profile,snapshot,
            SessionPurpose::ManualRestore);
        if(!request){ failed=true; break; }
        g_manualMoveOperations[operationId].waitingSessionApps.insert(profile.id);
    }
    if(failed){
        CancelManualMoveOperation(operationId);
        Balloon(L"Restore could not acquire browser session data. Retry.");
        return;
    }
    FinishManualMove(operationId);
}

static void HandleManualRestoreSessionResult(const SessionRoute& route,
                                             const SessionResult& result){
    auto operation=g_manualMoveOperations.find(route.operationId);
    if(operation==g_manualMoveOperations.end() ||
       operation->second.waitingSessionApps.erase(route.app)==0) return;
    std::map<std::string,AppFastSnapshot> current=
        CollectFastSnapshots(operation->second.profiles.all());
    auto captured=operation->second.snapshots.find(route.app);
    auto now=current.find(route.app);
    if(!SessionDataUsable(result.status) || !result.windows ||
       captured==operation->second.snapshots.end() || now==current.end() ||
       !FastSnapshotCanPersistAll(now->second) ||
       captured->second.identityGeneration!=now->second.identityGeneration ||
       captured->second.generation!=now->second.generation ||
       route.contentGeneration!=now->second.generation){
        CancelManualMoveOperation(route.operationId);
        Balloon(L"Restore was cancelled because its window snapshot became stale.");
        return;
    }
    const AppProfile* profile=operation->second.profiles.find(route.app);
    if(!profile){ CancelManualMoveOperation(route.operationId); return; }
    ReconcileRequest request;
    request.operationId=route.operationId;
    request.app=route.app;
    request.identityGeneration=now->second.identityGeneration;
    request.contentGeneration=now->second.generation;
    request.sessionRequestId=result.requestId;
    request.sessionDataGeneration=result.dataGeneration;
    request.nowUtc=UtcNowSeconds();
    request.freshness=result.status==SessionDataStatus::Fresh
        ? ReconcileFreshness::Fresh : ReconcileFreshness::CachedStale;
    request.saved=operation->second.saved;
    try { ConfigureWorkerLiveBuild(request,ReconcileWorkMode::Plan,*profile,
                                   captured->second,result.windows,
                                   operation->second.currentDesktops); }
    catch(...) { CancelManualMoveOperation(route.operationId); return; }
    bool tracked=false;
    try {
        tracked=operation->second.waitingReconcileApps.insert(route.app).second;
    } catch(...) { tracked=false; }
    if(!tracked || !RequestReconcileWork(request)){
        operation->second.waitingReconcileApps.erase(route.app);
        CancelManualMoveOperation(route.operationId);
        Balloon(L"Restore matching could not be queued.");
    }
}

static ReservedAutoIdentity ReservationForManualMove(
        const FastWin& fast,const MoveToken& token,const std::string& recordId,
        bool& provisionalNeedsInsert,bool& reservationReady){
    provisionalNeedsInsert=false;
    reservationReady=false;
    ReservedAutoIdentity reservation;
    reservation.token=token;
    reservation.identity=IdentityOf(fast);
    reservation.app=fast.app;
    reservation.recordId=recordId;
    reservation.originDesktop=fast.desktop;
    auto bound=g_recordByRuntime.find(RuntimeKey(fast));
    if(bound!=g_recordByRuntime.end() &&
       SameIdentity(bound->second.identity,IdentityOf(fast))){
        reservation.recordId=bound->second.recordId;
        reservationReady=true;
    } else if(!fast.app.empty() && !GuidIsZero(fast.desktop)){
        std::string id;
        auto existing=g_provisionalRecordByRuntime.find(RuntimeKey(fast));
        if(existing==g_provisionalRecordByRuntime.end()){
            id=NewRecordId();
            GUID parsed{};
            if(ParseNonzeroLayoutGuid(id,parsed)) provisionalNeedsInsert=true;
            else id.clear();
        } else id=existing->second;
        if(!id.empty()){
            LayoutWin origin;
            origin.recordId=id;
            origin.app=fast.app;
            origin.desktop=fast.desktop;
            origin.deskIndex=SnapshotDesktopIndex(fast.desktop);
            origin.activeTitle=W2U8(fast.title);
            MarkSeen(origin,UtcNowSeconds());
            if(!BindReservationToProvisionalOrigin(
                    origin,reservation.recordId)){
                provisionalNeedsInsert=false;
                return reservation;
            }
            origin.recordId=reservation.recordId;
            reservation.provisionalOriginRecord=origin;
            reservation.hasProvisionalOriginRecord=true;
            reservationReady=true;
        }
    }
    return reservation;
}

static bool QueueManualMove(ManualMoveOperation& operation,
                            const FastWin& fast,const LayoutWin& saved,
                            size_t itemIndex){
    const std::string runtimeKey=RuntimeKey(fast);
    MoveJob job;
    job.token.owner=MoveOwner::ManualTray;
    job.token.operationId=operation.operationId;
    job.token.jobId=TakeNonzeroId(g_nextMoveJobId);
    job.token.itemIndex=itemIndex;
    job.runtimeKey=runtimeKey;
    job.recordId=saved.recordId;
    job.destination=saved.desktop;
    MoveRuntimeBinding runtime;
    runtime.window=fast;
    runtime.destination=saved.desktop;
    bool provisionalNeedsInsert=false;
    bool reservationReady=false;
    ReservedAutoIdentity reservation=
        ReservationForManualMove(fast,job.token,saved.recordId,
                                 provisionalNeedsInsert,reservationReady);
    if(!reservationReady) return false;
    ReservationHandoff handoff;
    bool insertedProvisional=false;
    bool insertedRuntime=false;
    bool insertedLiveJob=false;
    bool incrementedOutstanding=false;
    const bool completed=RunSuccessorFirstReservationHandoff([&]{
        if(!BeginReservationHandoff(runtimeKey,reservation,handoff))
            return false;
        job.recordId=handoff.installed.recordId;
        if(provisionalNeedsInsert){
            if(!g_provisionalRecordByRuntime.emplace(
                    runtimeKey,handoff.installed.recordId).second) return false;
            insertedProvisional=true;
        }
        if(!g_moveRuntime.emplace(job.token.jobId,runtime).second) return false;
        insertedRuntime=true;
        if(!operation.liveJobIds.insert(job.token.jobId).second) return false;
        insertedLiveJob=true;
        ++operation.outstanding;
        incrementedOutstanding=true;
        if(!g_moveQueue.enqueue(job)) return false;
        return true;
    },[&]() noexcept {
        PublishReservationHandoff(handoff);
    },[&]{
        CancelDisplacedReservationHandoff(handoff);
    },[&]{
        if(insertedProvisional){
            auto provisional=g_provisionalRecordByRuntime.find(runtimeKey);
            if(provisional!=g_provisionalRecordByRuntime.end() &&
               provisional->second==handoff.installed.recordId)
                g_provisionalRecordByRuntime.erase(provisional);
        }
        if(insertedLiveJob) operation.liveJobIds.erase(job.token.jobId);
        if(incrementedOutstanding && operation.outstanding>0)
            --operation.outstanding;
        if(insertedRuntime) g_moveRuntime.erase(job.token.jobId);
        RollbackReservationHandoff(handoff);
    });
    if(!completed) return false;
    return true;
}

static void HandleManualReconcileResult(std::unique_ptr<ReconcileResult> result){
    if(!result) return;
    auto operation=g_manualMoveOperations.find(result->operationId);
    if(operation==g_manualMoveOperations.end() ||
       operation->second.cancellationPending ||
       operation->second.waitingReconcileApps.erase(result->app)==0) return;
    g_reconcileDeadlines.complete(result->operationId);
    auto captured=operation->second.snapshots.find(result->app);
    std::map<std::string,AppFastSnapshot> current=
        CollectFastSnapshots(operation->second.profiles.all());
    auto now=current.find(result->app);
    if(result->status!=ReconcileResultStatus::Completed ||
       result->workMode!=ReconcileWorkMode::Plan ||
       !result->buildLiveFromInputs || result->plan.deferred ||
       captured==operation->second.snapshots.end() || now==current.end() ||
       !FastSnapshotCanPersistAll(now->second) ||
       result->identityGeneration!=now->second.identityGeneration ||
       result->contentGeneration!=now->second.generation){
        CancelManualMoveOperation(result->operationId);
        Balloon(L"Restore matching became stale or too complex. Retry.");
        return;
    }
    std::set<size_t> restoredIndices;
    for(const RestoreRequest& restore : result->plan.restores)
        restoredIndices.insert(restore.liveIndex);
    for(const LayoutMatch& match : result->plan.matches){
        if(match.savedIndex>=result->saved.size() ||
           match.liveIndex>=result->fastWindows.size()) continue;
        const LayoutWin& saved=result->saved[match.savedIndex];
        const FastWin& fast=result->fastWindows[match.liveIndex];
        if(ResolveSavedDesktop(saved,operation->second.currentDesktops)<0){
            ++operation->second.failed;
            continue;
        }
        if(GuidEq(fast.desktop,saved.desktop)){
            ++operation->second.already;
            continue;
        }
        if(restoredIndices.count(match.liveIndex)==0 ||
           !QueueManualMove(operation->second,fast,saved,match.liveIndex))
            ++operation->second.failed;
    }
    if(operation->second.outstanding) ArmMoveTimer();
    FinishManualMove(result->operationId);
}
// ================================ CLI mode ===================================
static bool AcquireCliSession(const AppProfile& profile,
                              std::shared_ptr<const std::vector<WinFp> >& output){
    output.reset();
    std::wstring before=ResolveBrowserSessionPath(profile);
    SessionStamp beforeStamp;
    if(before.empty() || !GetSessionStamp(before,beforeStamp)) return false;
    SessionFileReadResult read=ReadBrowserSessionFileBounded(
        before,MAX_BROWSER_SESSION_BYTES);
    if(read.status!=FileReadStatus::Ok || !read.readStampKnown ||
       read.readStamp!=beforeStamp) return false;
    std::unique_ptr<std::vector<WinFp> > parsed(new(std::nothrow) std::vector<WinFp>());
    if(!parsed || !ParseBrowserSessionData(profile,read.bytes,*parsed)) return false;
    std::wstring after=ResolveBrowserSessionPath(profile);
    SessionStamp afterStamp;
    if(after!=before || !GetSessionStamp(after,afterStamp) ||
       beforeStamp!=read.readStamp || read.readStamp!=afterStamp) return false;
    try { output=std::shared_ptr<const std::vector<WinFp> >(parsed.release()); }
    catch(...) { return false; }
    return true;
}

static bool CliSaveCheckpoint(std::string& summary){
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    std::vector<DeskRec> desktops;
    std::string error;
    if(!CurrentDesktops(desktops,&error)){
        summary="Failed to collect desktops: "+error;
        return false;
    }
    std::vector<ReconcileRequest> requests;
    const std::vector<AppProfile> profiles=ActiveProfiles();
    requests.reserve(profiles.size());
    for(const AppProfile& profile : profiles){
        auto snapshot=snapshots.find(profile.id);
        if(snapshot==snapshots.end() || !FastSnapshotCanPersistAll(snapshot->second)){
            summary="Window enumeration or desktop lookup was incomplete; no file was changed.";
            return false;
        }
        if(snapshot->second.windows.empty()) continue;
        std::shared_ptr<const std::vector<WinFp> > session;
        if(!AcquireCliSession(profile,session)){
            summary="Fresh browser session data was unavailable; no file was changed.";
            return false;
        }
        requests.push_back(MakeCliLiveRequest(
            profile,snapshot->second,session,desktops));
    }
    PreparedCliProfileBatch prepared;
    if(!BuildCliProfileBatch(requests,prepared)){
        summary="Browser window preparation failed; no file was changed.";
        return false;
    }
    std::vector<LayoutWin> records;
    std::set<std::string> recordIds;
    records.reserve(prepared.live.size());
    for(LayoutWin& record : prepared.live){
        record.recordId=NewRecordId();
        GUID id{};
        std::string canonical;
        if(GuidIsZero(record.desktop) ||
           !ParseNonzeroLayoutGuid(record.recordId,id,&canonical) ||
           !recordIds.insert(canonical).second){
            summary="Could not allocate valid record IDs; no file was changed.";
            return false;
        }
        record.recordId=canonical;
        MarkSeen(record,UtcNowSeconds());
        record.missingSinceUtc=0;
        records.push_back(std::move(record));
    }
    if(records.empty()){
        summary="No browser windows found. Nothing to save.";
        return false;
    }
    std::string bytes;
    std::vector<LayoutWin> checked=records;
    if(!BuildCheckedLayoutSnapshot(
            desktops,checked,UtcNowSeconds(),bytes,&error)){
        summary="Failed to validate manual layout: "+error;
        return false;
    }
    std::map<std::string,AppFastSnapshot> currentSnapshots=
        CollectFastSnapshots();
    std::vector<DeskRec> currentDesktops;
    if(!CurrentDesktops(currentDesktops,&error) ||
       !CliCheckpointInputsStillCurrent(
           snapshots,currentSnapshots,desktops,currentDesktops)){
        summary="Window or desktop state changed while saving; no file was changed.";
        return false;
    }
    ScopedLayoutLock lock;
    if(!lock.acquired()){
        summary="Layout storage is busy; no file was changed.";
        return false;
    }
    LayoutLoadResult prior=LoadLayoutWithBackupLocked(
        LayoutPath(true),UtcNowSeconds());
    if(prior.status==LayoutLoadStatus::Unavailable ||
       !AtomicWriteText(LayoutPath(true),bytes,&error,
                        prior.status==LayoutLoadStatus::Recovered)){
        summary="Failed to atomically write manual layout: "+error;
        return false;
    }
    summary="Saved layout: "+std::to_string(records.size())+" window(s).";
    return true;
}

static bool WaitForCliVerify(HANDLE timer,DWORD delayMs){
    LARGE_INTEGER due{};
    due.QuadPart=-static_cast<LONGLONG>(delayMs)*10000LL;
    return timer && SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) &&
        WaitForSingleObject(timer,delayMs+1000)==WAIT_OBJECT_0;
}

static bool CliRestoreCheckpoint(bool manual,std::string& summary,
                                 std::vector<std::string>& lines){
    LayoutLoadResult loaded=LoadLayoutWithBackup(
        LayoutPath(manual),UtcNowSeconds());
    if(!loaded.usable() || loaded.sourceVersion!=4 || loaded.wins.empty()){
        summary=manual ? "No valid v4 manual layout." : "No valid v4 automatic layout.";
        return false;
    }
    std::vector<DeskRec> desktops;
    std::string error;
    if(!CurrentDesktops(desktops,&error)){
        summary="Failed to collect desktops: "+error;
        return false;
    }
    std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
    std::vector<ReconcileRequest> requests;
    const std::vector<AppProfile> profiles=ActiveProfiles();
    requests.reserve(profiles.size());
    for(const AppProfile& profile : profiles){
        auto snapshot=snapshots.find(profile.id);
        if(snapshot==snapshots.end() || !FastSnapshotCanPersistAll(snapshot->second)){
            summary="Window enumeration or desktop lookup was incomplete.";
            return false;
        }
        if(snapshot->second.windows.empty()) continue;
        std::shared_ptr<const std::vector<WinFp> > session;
        if(!AcquireCliSession(profile,session)){
            summary="Fresh browser session data was unavailable.";
            return false;
        }
        requests.push_back(MakeCliLiveRequest(
            profile,snapshot->second,session,desktops));
    }
    PreparedCliProfileBatch prepared;
    if(!BuildCliProfileBatch(requests,prepared)){
        summary="Browser window preparation failed; no windows were moved.";
        return false;
    }
    std::vector<LayoutWin> live=std::move(prepared.live);
    std::vector<FastWin> fast=std::move(prepared.fastWindows);
    if(live.empty()){
        summary="No browser windows are open to restore.";
        return false;
    }
    bool tooComplex=false;
    std::vector<LayoutMatch> matches=MatchOneToOne(
        loaded.wins,live,0.55,&tooComplex);
    if(tooComplex){ summary="Restore candidate set is too complex."; return false; }

    MoveQueue queue;
    std::map<uint64_t,MoveRuntimeBinding> runtimes;
    uint64_t nextJob=0;
    size_t already=0,failed=0,moved=0;
    for(const LayoutMatch& match : matches){
        if(match.savedIndex>=loaded.wins.size() || match.liveIndex>=fast.size()) continue;
        const LayoutWin& saved=loaded.wins[match.savedIndex];
        if(ResolveSavedDesktop(saved,desktops)<0){
            ++failed;
            lines.push_back("[no target] "+live[match.liveIndex].activeTitle);
            continue;
        }
        if(GuidEq(fast[match.liveIndex].desktop,saved.desktop)){
            ++already;
            lines.push_back("[already there] "+live[match.liveIndex].activeTitle);
            continue;
        }
        MoveJob job;
        job.token.owner=MoveOwner::ManualTray;
        job.token.operationId=1;
        job.token.jobId=++nextJob;
        job.token.itemIndex=match.liveIndex;
        job.runtimeKey=RuntimeKey(fast[match.liveIndex]);
        job.recordId=saved.recordId;
        job.destination=saved.desktop;
        MoveRuntimeBinding runtime;
        runtime.window=fast[match.liveIndex];
        runtime.destination=saved.desktop;
        bool queued=false;
        try {
            runtimes.emplace(job.token.jobId,runtime);
            queued=queue.enqueue(job);
        } catch(...) { queued=false; }
        if(!queued){
            runtimes.erase(job.token.jobId);
            ++failed;
        }
    }
    UniqueWinHandle timer(CreateWaitableTimerW(nullptr,FALSE,nullptr));
    if(!queue.empty() && !timer){
        summary="Could not create the restore verification timer.";
        return false;
    }
    while(!queue.empty()){
        const MoveJob* front=queue.front();
        if(!front) break;
        auto runtime=runtimes.find(front->token.jobId);
        MoveResult result;
        if(runtime==runtimes.end()){
            result=queue.cancelJob(front->token.jobId);
        } else if(queue.nextAction()==MoveAction::Issue){
            WindowIdentityRecapture identity=WindowIdentityRecapture::Match;
            MoveAttemptOutcome current=ReadMoveDestination(
                runtime->second,identity);
            if(identity==WindowIdentityRecapture::Lost){
                result=queue.cancelJob(front->token.jobId);
            } else if(identity==WindowIdentityRecapture::Indeterminate){
                const IdentityRecaptureRetryAction unknown=
                    runtime->second.identityRecaptureBudget.observe(identity);
                if(unknown==IdentityRecaptureRetryAction::RetireCancelled ||
                   !WaitForCliVerify(timer.get(),MOVE_VERIFY_INTERVAL_MS))
                    result=queue.cancelJob(front->token.jobId);
            } else if(current==MoveAttemptOutcome::OnDestination ||
               current==MoveAttemptOutcome::PermanentFailure)
                result=queue.onIssued(current);
            else {
                HRESULT issued=IssueWindowMove(runtime->second,identity);
                if(identity==WindowIdentityRecapture::Lost)
                    result=queue.cancelJob(front->token.jobId);
                else if(identity==WindowIdentityRecapture::Indeterminate){
                    const IdentityRecaptureRetryAction unknown=
                        runtime->second.identityRecaptureBudget.observe(identity);
                    if(unknown==IdentityRecaptureRetryAction::RetireCancelled ||
                       !WaitForCliVerify(timer.get(),MOVE_VERIFY_INTERVAL_MS))
                        result=queue.cancelJob(front->token.jobId);
                } else {
                    runtime->second.identityRecaptureBudget.reset();
                    result=queue.onIssued(SUCCEEDED(issued)
                        ? MoveAttemptOutcome::Accepted
                        : (RetryableMoveHresult(issued)
                            ? MoveAttemptOutcome::TransientFailure
                            : MoveAttemptOutcome::PermanentFailure));
                }
            }
        } else {
            if(!WaitForCliVerify(timer.get(),MOVE_VERIFY_INTERVAL_MS)){
                result=queue.onVerified(MoveAttemptOutcome::PermanentFailure);
            } else {
                WindowIdentityRecapture identity=WindowIdentityRecapture::Match;
                MoveAttemptOutcome verified=ReadMoveDestination(
                    runtime->second,identity);
                if(identity==WindowIdentityRecapture::Lost)
                    result=queue.cancelJob(front->token.jobId);
                else if(identity==WindowIdentityRecapture::Indeterminate){
                    if(runtime->second.identityRecaptureBudget.observe(identity)==
                       IdentityRecaptureRetryAction::RetireCancelled)
                        result=queue.cancelJob(front->token.jobId);
                } else {
                    runtime->second.identityRecaptureBudget.reset();
                    result=queue.onVerified(verified);
                }
            }
        }
        if(result.completed){
            runtimes.erase(result.token.jobId);
            if(result.terminal==MoveTerminal::Succeeded){
                ++moved;
                lines.push_back("[moved] "+result.recordId);
            } else {
                ++failed;
                lines.push_back("[FAILED] "+result.recordId);
            }
        }
    }
    summary="Restore: moved "+std::to_string(moved)+", "+
        std::to_string(already)+" already in place, "+
        std::to_string(failed)+" failed.";
    return failed==0;
}

static int CliRun(const std::wstring& cmd){
    if(cmd==L"list"||cmd==L"status"){
        UINT count=0; g_vdmi->GetCount(&count);
        printf("Virtual desktops: %u\n",count);
        for(UINT i=0;i<count;++i){ ScopedComPtr<IVirtualDesktop> d(GetDesktopByIndex(i)); if(!d)continue; GUID g={0};d->GetID(&g);
            std::wstring nm=DesktopNameFromRegistry(g); std::string nmU8=nm.empty()?std::string():("("+W2U8(nm)+")");
            printf("  [%u] %s  %s\n",i,W2U8(GuidToString(g)).c_str(),nmU8.c_str()); }
        if(cmd==L"status"){
            std::vector<DeskRec> desks; std::string error;
            const bool desktopsReady=CurrentDesktops(desks,&error);
            std::map<std::string,AppFastSnapshot> snapshots=CollectFastSnapshots();
            for(const AppProfile& profile : ActiveProfiles()){
                auto snapshot=snapshots.find(profile.id);
                if(snapshot==snapshots.end() || snapshot->second.windows.empty()) continue;
                std::shared_ptr<const std::vector<WinFp> > session;
                bool fresh=AcquireCliSession(profile,session);
                PreparedCliProfileBatch prepared;
                std::vector<ReconcileRequest> requests;
                requests.push_back(MakeCliLiveRequest(
                    profile,snapshot->second,session,desks));
                 const bool preparationReady=
                     desktopsReady && fresh &&
                     BuildCliProfileBatch(requests,prepared);
                 std::vector<CliStatusRow> rows;
                 bool rowsReady=BuildCliStatusRows(
                     snapshot->second,desks,
                     preparationReady ? &prepared : nullptr,
                     preparationReady,rows);
                 if(!rowsReady){
                     rows.clear();
                     rowsReady=BuildCliStatusRows(
                         snapshot->second,desks,nullptr,false,rows);
                 }
                 printf("\n%s: %d live window(s) (session data: %s)\n",
                        profile.id.c_str(),
                        rowsReady ? static_cast<int>(rows.size()) : 0,
                        preparationReady?"fresh":"unavailable");
                 for(const CliStatusRow& row : rows){
                     if(row.fingerprintAvailable)
                         printf("  hwnd=0x%p desktop=[%d] tabs=%d active=\"%s\"\n",
                                (void*)row.window.hwnd,row.deskIndex,row.tabCount,
                                row.activeTitle.c_str());
                     else
                         printf("  hwnd=0x%p desktop=[%d] tabs=? active=\"%s\" fingerprint=unavailable\n",
                                (void*)row.window.hwnd,row.deskIndex,
                                row.activeTitle.c_str());
                 }
            }
        }
        return 0;
    }
    if(cmd==L"save"){
        std::string summary;
        bool ok=CliSaveCheckpoint(summary);
        printf("%s\nManual layout: %s\n",summary.c_str(),
               W2U8(LayoutPath(true)).c_str());
        return ok?0:1;
    }
    if(cmd==L"restore" || cmd==L"restore-auto"){
        std::vector<std::string> lines; std::string summary;
        bool ok=CliRestoreCheckpoint(cmd==L"restore",summary,lines);
        for(const std::string& line : lines) printf("  %s\n",line.c_str());
        printf("\n%s\n",summary.c_str());
        return ok?0:1;
    }
    printf("Usage: vde <save|restore|restore-auto|status|list>\n");
    printf("  save          save current window layout to layout-manual.txt\n");
    printf("  restore       restore from layout-manual.txt\n");
    printf("  restore-auto  restore from the last auto-saved layout\n");
    printf("  (no args) -> run resident in tray; Ctrl+Alt+D opens the desktop picker\n");
    return 2;
}

// ================================ GUI: picker ================================
struct WinItem { HWND hwnd; WindowIdentityKey identity; std::wstring title; std::wstring titleLC; std::wstring search; HICON icon; };   // search = titleLC (+ all-tab text for browser windows)
struct Tile { GUID guid; std::string guidKey; std::wstring name; std::wstring displayName; int index; std::vector<WinItem> windows; std::vector<size_t> filtered; RECT rc; int scroll=0; };
static std::vector<Tile> g_tiles;
static PickerState g_picker;
static int  g_sel=-1; // legacy index kept synchronized with g_picker
static HWND g_target=nullptr; static std::wstring g_targetTitle;
static HWND g_settings=nullptr;
static HINSTANCE g_inst=nullptr;
static HFONT g_uiFont=nullptr;
static const UINT WM_TRAY=WM_APP+1;
static NOTIFYICONDATAW g_nid={0};
static int g_dpi=96;
static int S(int v){ return MulDiv(v,g_dpi,96); }   // px@96dpi -> px@текущий DPI

static void StopWorkers(HWND messageWindow){
    if(g_sessionWorker){ g_sessionWorker->Stop(); g_sessionWorker.reset(); }
    if(g_reconcileWorker){ g_reconcileWorker->Stop(); g_reconcileWorker.reset(); }
    DrainPostedSessionResults(messageWindow);
    DrainPostedReconcileResults(messageWindow);
}

static bool QuiesceRuntime(HWND messageWindow) noexcept {
    return RunRuntimeQuiescence(g_runtimeQuiescence,[&](){
        KillTimer(messageWindow,TIMER_MONITOR);
        KillTimer(messageWindow,TIMER_MOVE_VERIFY);
        KillTimer(messageWindow,TIMER_HEARTBEAT);
        KillTimer(messageWindow,TIMER_AUTO_FLUSH);
        g_flushTimerArmed=false;
        g_flushTimerDueMs=0;
        g_heartbeatTimerArmed=false;
        g_moveCancellationRetry.clear();
        StopWorkers(messageWindow);
    });
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
static HWND g_search=nullptr; static WNDPROC g_searchOrigProc=nullptr;
static HWND g_tip=nullptr;
struct RowRec { RECT rc; std::wstring full; bool trunc; };
struct PickerPaintCache {
    std::vector<RowRec> hoverRows;
    std::wstring switchHeader,moveHeader;
    int hintWidth=0;
    RECT clearButton={0,0,0,0};
    void swap(PickerPaintCache& other) noexcept {
        hoverRows.swap(other.hoverRows);
        switchHeader.swap(other.switchHeader);
        moveHeader.swap(other.moveHeader);
        const int width=hintWidth;
        hintWidth=other.hintWidth;
        other.hintWidth=width;
        const RECT button=clearButton;
        clearButton=other.clearButton;
        other.clearButton=button;
    }
    void clear() noexcept {
        PickerPaintCache empty;
        swap(empty);
    }
};
static PickerPaintCache g_pickerPaintCache;
static std::wstring LowerW(std::wstring s){ if(!s.empty()) CharLowerW(&s[0]); return s; }
static bool MatchesSearch(const std::wstring& title){ return g_picker.searchText.empty() || LowerW(title).find(g_picker.searchText)!=std::wstring::npos; }
// ---- picker palette (per mockup) ----
static const COLORREF CLR_BG=RGB(20,20,24), CLR_TILE=RGB(28,28,33), CLR_TILE_DIM=RGB(22,22,26), CLR_SEARCH=RGB(34,33,38),
    CLR_ACTIVE=RGB(0xF2,0x96,0x05) /*#f29605*/, CLR_PASSIVE=RGB(0x6B,0x60,0x4F) /*#6b604f*/, CLR_BORDER=RGB(58,55,52),
    CLR_TEXT=RGB(208,206,210), CLR_HEAD=RGB(238,238,242), CLR_HINT=RGB(150,145,135), CLR_DIM=RGB(110,108,112),
    CLR_SCROLL_TRK=RGB(40,40,46), CLR_SCROLL_THB=RGB(96,92,86);
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

static GUID CurrentDesktopGuid() noexcept {
    GUID guid={0};
    if(!g_vdmi) return guid;
    IVirtualDesktop* raw=nullptr;
    const HRESULT currentResult=g_vdmi->GetCurrentDesktop(&raw);
    ScopedComPtr<IVirtualDesktop> desktop(raw);
    if(FAILED(currentResult) || !desktop) return guid;
    GUID candidate={0};
    if(FAILED(desktop->GetID(&candidate)) || GuidIsZero(candidate))
        return guid;
    return candidate;
}

static WindowIdentityKey CapturePickerWindowIdentity(HWND hwnd) noexcept {
    WindowIdentityKey identity;
    if(!hwnd || !IsWindow(hwnd)) return identity;
    DWORD pid=0;
    if(!GetWindowThreadProcessId(hwnd,&pid) || pid==0) return identity;
    uint64_t started=0;
    if(!TryReadProcessStart(pid,started)) return identity;
    identity.hwnd=reinterpret_cast<uintptr_t>(hwnd);
    identity.pid=pid;
    identity.processStart=started;
    if(RecaptureGenericWindowIdentity(identity)!=WindowIdentityRecapture::Match)
        return WindowIdentityKey{};
    return identity;
}

struct PickerEnumContext {
    std::vector<Tile>* tiles=nullptr;
    std::map<DWORD,uint64_t> processStarts;
    bool failed=false;
};

static BOOL CALLBACK EnumAll(HWND hwnd,LPARAM parameter){
    PickerEnumContext& context=
        *reinterpret_cast<PickerEnumContext*>(parameter);
    try {
        if(!IsAltTabWindow(hwnd)) return TRUE;

        DWORD pid=0;
        if(!GetWindowThreadProcessId(hwnd,&pid) || pid==0){
            context.failed=true;
            return FALSE;
        }
        auto process=context.processStarts.find(pid);
        if(process==context.processStarts.end()){
            uint64_t started=0;
            if(!TryReadProcessStart(pid,started)){
                context.failed=true;
                return FALSE;
            }
            process=context.processStarts.emplace(pid,started).first;
        }
        WindowIdentityKey identity;
        identity.hwnd=reinterpret_cast<uintptr_t>(hwnd);
        identity.pid=pid;
        identity.processStart=process->second;
        if(!AcceptPickerRowIdentity(
                identity,WindowIdentityRecapture::Match)){
            context.failed=true;
            return FALSE;
        }

        GUID desktop={0};
        if(!g_vdmDoc ||
           FAILED(g_vdmDoc->GetWindowDesktopId(hwnd,&desktop)) ||
           GuidIsZero(desktop)){
            context.failed=true;
            return FALSE;
        }
        Tile* tile=nullptr;
        for(Tile& candidate : *context.tiles)
            if(GuidEq(candidate.guid,desktop)){
                tile=&candidate;
                break;
            }
        if(!tile){
            context.failed=true;
            return FALSE;
        }

        const int length=GetWindowTextLengthW(hwnd);
        if(length<=0){
            context.failed=true;
            return FALSE;
        }
        std::wstring title(static_cast<size_t>(length)+1,L'\0');
        const int copied=GetWindowTextW(hwnd,&title[0],length+1);
        if(copied<=0){
            context.failed=true;
            return FALSE;
        }
        title.resize(static_cast<size_t>(copied));

        WinItem item;
        item.hwnd=hwnd;
        item.identity=identity;
        item.title=title;
        item.titleLC=title;
        if(!item.titleLC.empty()) CharLowerW(&item.titleLC[0]);
        item.search=item.titleLC;
        item.icon=WindowIcon(hwnd);
        if(!AcceptPickerRowIdentity(
                item.identity,RecaptureGenericWindowIdentity(item.identity))){
            context.failed=true;
            return FALSE;
        }
        tile->windows.push_back(std::move(item));
        return TRUE;
    } catch(...) {
        context.failed=true;
        return FALSE;
    }
}

static bool PopulatePickerFilteredRows(std::vector<Tile>& tiles,
                                       const std::wstring& searchText) noexcept {
    for(Tile& tile : tiles)
        if(!BuildPickerFilteredIndices(tile.windows,searchText,tile.filtered,
                [](const WinItem& item)->const std::wstring& {
                    return item.search;
                })) return false;
    return true;
}

static bool BuildModel(const WindowIdentityKey& activeWindow,bool resetUi){
    const GUID observedCurrent=CurrentDesktopGuid();
    const bool published=RunPickerRefreshWithCurrent(
        g_tiles,g_picker,observedCurrent,
        [&](std::vector<Tile>& tiles,PickerState& state){
            if(!g_vdmi) return false;
            if(resetUi){
                state.searchText.clear();
                state.searchActive=false;
                state.scrollByDesktop.clear();
            }
            state.activeWindow=activeWindow;

            IObjectArray* rawDesktops=nullptr;
            const HRESULT desktopsResult=
                g_vdmi->GetDesktops(&rawDesktops);
            ScopedComPtr<IObjectArray> desktopArray(rawDesktops);
            if(FAILED(desktopsResult) || !desktopArray) return false;
            UINT count=0;
            if(FAILED(desktopArray->GetCount(&count)) ||
               count>MAX_LAYOUT_RECORDS) return false;
            tiles.reserve(count);
            std::vector<GUID> desktopGuids;
            desktopGuids.reserve(count);
            for(UINT index=0;index<count;++index){
                PickerScopedComOutput<IVirtualDesktop> desktop;
                const HRESULT desktopResult=desktopArray->GetAt(
                        index,kIID_IVirtualDesktop,
                        reinterpret_cast<void**>(desktop.put()));
                if(FAILED(desktopResult) || !desktop) return false;
                GUID guid={0};
                if(FAILED(desktop.get()->GetID(&guid)) || GuidIsZero(guid))
                    return false;
                if(!AppendUniquePickerDesktop(desktopGuids,guid))
                    return false;
                Tile tile;
                tile.guid=guid;
                tile.guidKey=GuidKey(guid);
                tile.index=static_cast<int>(index);
                tile.name=DesktopNameFromRegistry(guid);
                if(tile.name.empty())
                    tile.name=L"Desktop "+std::to_wstring(index+1);
                tile.displayName=
                    std::to_wstring(index+1)+L". "+tile.name;
                const auto scroll=state.scrollByDesktop.find(tile.guidKey);
                if(scroll!=state.scrollByDesktop.end())
                    tile.scroll=std::max(0,scroll->second);
                tiles.push_back(std::move(tile));
            }

            PickerEnumContext context;
            context.tiles=&tiles;
            if(!EnumWindows(EnumAll,reinterpret_cast<LPARAM>(&context)) ||
               context.failed) return false;
            if(!PopulatePickerFilteredRows(tiles,state.searchText))
                return false;
            ResolvePickerSelection(state,desktopGuids);
            return true;
        });
    if(published) g_sel=g_picker.selectedIndex;
    return published;
}

static bool SetPickerSelectionWithLegacy(int index) noexcept {
    if(index<0 || index>=static_cast<int>(g_tiles.size())) return false;
    if(!SetPickerSelection(g_picker,index,g_tiles[index].guid)) return false;
    g_sel=index;
    return true;
}

static bool RememberPickerScroll(Tile& tile,int scroll) noexcept {
    const int value=std::max(0,scroll);
    try {
        auto saved=g_picker.scrollByDesktop.find(tile.guidKey);
        if(saved==g_picker.scrollByDesktop.end())
            g_picker.scrollByDesktop.emplace(tile.guidKey,value);
        else
            saved->second=value;
        tile.scroll=value;
        return true;
    } catch(...) {
        return false;
    }
}
static bool RebuildPickerFilteredRows() noexcept {
    try {
        std::vector<std::vector<size_t>> staged(g_tiles.size());
        for(size_t index=0;index<g_tiles.size();++index)
            if(!BuildPickerFilteredIndices(
                    g_tiles[index].windows,g_picker.searchText,staged[index],
                    [](const WinItem& item)->const std::wstring& {
                        return item.search;
                    })) return false;
        for(size_t index=0;index<g_tiles.size();++index)
            g_tiles[index].filtered.swap(staged[index]);
        return true;
    } catch(...) {
        return false;
    }
}

static bool ApplyPickerSearchText(std::wstring searchText) noexcept {
    try {
        std::vector<std::vector<size_t>> staged(g_tiles.size());
        for(size_t index=0;index<g_tiles.size();++index)
            if(!BuildPickerFilteredIndices(
                    g_tiles[index].windows,searchText,staged[index],
                    [](const WinItem& item)->const std::wstring& {
                        return item.search;
                    })) return false;
        for(size_t index=0;index<g_tiles.size();++index)
            g_tiles[index].filtered.swap(staged[index]);
        g_picker.searchText.swap(searchText);
        return true;
    } catch(...) {
        return false;
    }
}

static bool RefreshPickerPaintCache() noexcept;
static bool g_tabBlobsBuilt=false;
static void HandleSearchSessionResult(const SessionRoute& route,
                                       const SessionResult& result){
    auto operation=g_searchOperations.find(route.operationId);
    if(operation==g_searchOperations.end()) return;
    auto captured=operation->second.snapshots.find(route.app);
    bool queued=false;
    if(captured!=operation->second.snapshots.end() &&
       SessionDataUsable(result.status) && result.windows){
        std::map<std::string,AppFastSnapshot> current=CollectFastSnapshots();
        auto now=current.find(route.app);
        if(now!=current.end() && now->second.identityGeneration==route.identityGeneration &&
           now->second.generation==route.contentGeneration &&
           now->second.generation==captured->second.generation &&
           FastSnapshotCanPersistAll(now->second)){
            std::vector<AppProfile> profiles;
            const AppProfile* profile=FindActiveProfile(route.app,profiles);
            if(profile){
                ReconcileRequest request;
                request.operationId=route.operationId;
                request.app=route.app;
                request.identityGeneration=route.identityGeneration;
                request.contentGeneration=route.contentGeneration;
                request.sessionRequestId=result.requestId;
                request.sessionDataGeneration=result.dataGeneration;
                request.nowUtc=UtcNowSeconds();
                request.freshness=result.status==SessionDataStatus::Fresh
                    ? ReconcileFreshness::Fresh : ReconcileFreshness::CachedStale;
                try {
                    const std::vector<DeskRec> noDesktopLookup;
                    ConfigureWorkerLiveBuild(request,
                        ReconcileWorkMode::PrepareLiveOnly,*profile,
                        captured->second,result.windows,noDesktopLookup);
                    queued=operation->second.waitingReconcileApps.insert(
                        route.app).second;
                    if(queued && !RequestReconcileWork(request)){
                        operation->second.waitingReconcileApps.erase(route.app);
                        queued=false;
                    }
                } catch(...) { queued=false; }
            }
        }
    }
    if(queued) return;
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(route.operationId);
        g_searchOperations.erase(operation);
    }
}

static void HandleSearchReconcileResult(std::unique_ptr<ReconcileResult> result){
    if(!result) return;
    auto operation=g_searchOperations.find(result->operationId);
    if(operation==g_searchOperations.end() || operation->second.outstanding==0 ||
       operation->second.waitingReconcileApps.erase(result->app)==0)
        return;
    g_reconcileDeadlines.complete(result->operationId);
    bool accepted=false;
    auto captured=operation->second.snapshots.find(result->app);
    std::map<std::string,AppFastSnapshot> current=CollectFastSnapshots();
    auto now=current.find(result->app);
    if(result->status==ReconcileResultStatus::Completed &&
       result->workMode==ReconcileWorkMode::PrepareLiveOnly &&
       result->buildLiveFromInputs &&
       captured!=operation->second.snapshots.end() && now!=current.end() &&
       FastSnapshotCanPersistAll(now->second) &&
       result->identityGeneration==now->second.identityGeneration &&
       result->contentGeneration==now->second.generation &&
       result->fastWindows.size()==result->sessionIndexByFast.size()){
        for(size_t index=0;index<result->fastWindows.size();++index){
            const WinFp* session=ReconcileSessionForFast(*result,index);
            if(!session || session->tabsBlob.empty()) continue;
            std::wstring blob=U82W(session->tabsBlob);
            if(!blob.empty()) CharLowerW(&blob[0]);
            for(Tile& tile : g_tiles)
                for(WinItem& item : tile.windows)
                    if(item.hwnd==result->fastWindows[index].hwnd)
                        item.search=item.titleLC+L" "+blob;
        }
        accepted=true;
    }
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(result->operationId);
        g_searchOperations.erase(operation);
    }
    if(accepted && g_main){
        RebuildPickerFilteredRows();
        RefreshPickerPaintCache();
        InvalidateRect(g_main,nullptr,FALSE);
    }
}

// Lazily request all-tab text without reading browser files on the UI thread.
static void EnsureTabSearch(){
    if(g_tabBlobsBuilt) return;
    g_tabBlobsBuilt=true;
    SearchOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    operation.snapshots=CollectFastSnapshots();
    const uint64_t operationId=operation.operationId;
    try {
        if(!g_searchOperations.emplace(operationId,operation).second) return;
    } catch(...) { return; }
    std::vector<AppProfile> profiles=ActiveProfiles();
    for(const AppProfile& profile : profiles){
        auto snapshot=operation.snapshots.find(profile.id);
        if(snapshot==operation.snapshots.end() || snapshot->second.windows.empty() ||
           !FastSnapshotCanObserve(snapshot->second)) continue;
        uint64_t request=RequestSessionWork(
            AsyncOperationOwner::Search,operationId,profile,snapshot->second,
            SessionPurpose::Search);
        if(request) ++g_searchOperations[operationId].outstanding;
    }
    if(g_searchOperations[operationId].outstanding==0){
        g_reconcileDeadlines.cancel(operationId);
        g_searchOperations.erase(operationId);
    }
}
static void LayoutTiles(int clientW){
    int n=(int)g_tiles.size();
    g_cols=std::max(1,std::min(n,std::max(1,(clientW-PAD)/(TILE_W+PAD)))); g_cols=std::min(g_cols,5);
    g_rows=(n+g_cols-1)/g_cols;
    for(int i=0;i<n;++i){ int r=i/g_cols,c=i%g_cols; RECT rc; rc.left=PAD+c*(TILE_W+PAD); rc.top=SEARCH_H+HEADER+PAD+r*(TILE_H+PAD); rc.right=rc.left+TILE_W; rc.bottom=rc.top+TILE_H; g_tiles[i].rc=rc; }
}
static SIZE DesiredClientSize(){ int n=(int)g_tiles.size(); int cols=std::min(std::max(1,n),5); int rows=(n+cols-1)/cols; SIZE s; s.cx=PAD+cols*(TILE_W+PAD); s.cy=SEARCH_H+HEADER+PAD+rows*(TILE_H+PAD); return s; }

class ScopedPickerMeasureDc {
    HWND window_=nullptr;
    HDC dc_=nullptr;
    HGDIOBJ previousFont_=nullptr;
public:
    ScopedPickerMeasureDc(HWND window,HFONT font) noexcept :window_(window){
        dc_=GetDC(window_);
        if(dc_ && font){
            HGDIOBJ selected=SelectObject(dc_,font);
            if(selected && selected!=HGDI_ERROR) previousFont_=selected;
        }
    }
    ~ScopedPickerMeasureDc(){
        if(dc_ && previousFont_) SelectObject(dc_,previousFont_);
        if(dc_) ReleaseDC(window_,dc_);
    }
    HDC get() const noexcept { return dc_; }
};

static bool RebuildPickerPaintCache(int clientWidth) noexcept {
    return RunPickerPaintCacheTransaction(
        g_pickerPaintCache,[&](PickerPaintCache& cache){
        cache.switchHeader=L"Switch to: ";
        const int selected=g_picker.selectedIndex;
        if(selected>=0 && selected<static_cast<int>(g_tiles.size()))
            cache.switchHeader+=g_tiles[selected].name;
        cache.moveHeader=L"Move window:  ";
        cache.moveHeader+=
            g_targetTitle.empty()?L"(no window)":g_targetTitle;

        if(g_picker.searchActive){
            const RECT searchBox=SearchBoxRect(clientWidth);
            const int size=S(30);
            const int left=searchBox.right-S(10)-size;
            const int top=searchBox.top+
                ((searchBox.bottom-searchBox.top)-size)/2;
            cache.clearButton={left,top,left+size,top+size};
        }

        ScopedPickerMeasureDc measure(g_main,g_fPI);
        if(!measure.get()) return false;
        const wchar_t* hint=
            L"Ctrl+Click - Move current window to selected desktop";
        SIZE hintSize={0,0};
        if(!GetTextExtentPoint32W(measure.get(),hint,
                static_cast<int>(wcslen(hint)),&hintSize)) return false;
        cache.hintWidth=hintSize.cx;
        for(const Tile& tile : g_tiles){
            RECT name=tile.rc;
            name.left+=S(14);
            name.top+=S(10);
            name.right-=S(12);
            name.bottom=name.top+S(22);
            const int rowHeight=S(22);
            const int listTop=name.bottom+S(6);
            const int listBottom=tile.rc.bottom-S(10);
            const int visibleRows=std::max(
                0,(listBottom-listTop)/rowHeight);
            const int maximumScroll=std::max(
                0,static_cast<int>(tile.filtered.size())-visibleRows);
            const int visibleScroll=PickerVisibleScroll(
                tile.scroll,maximumScroll);
            const bool hasScroll=
                static_cast<int>(tile.filtered.size())>visibleRows;
            const int rowRight=tile.rc.right-
                (hasScroll?S(18):S(14));
            int y=listTop;
            for(size_t position=static_cast<size_t>(visibleScroll);
                position<tile.filtered.size();++position){
                if(y+rowHeight>listBottom+S(3)) break;
                const size_t windowIndex=tile.filtered[position];
                if(windowIndex>=tile.windows.size()) return false;
                const WinItem& window=tile.windows[windowIndex];
                RECT text={tile.rc.left+S(38),y,rowRight,y+S(18)};
                SIZE extent={0,0};
                if(!GetTextExtentPoint32W(
                        measure.get(),window.title.c_str(),
                        static_cast<int>(window.title.size()),&extent))
                    return false;
                RowRec row;
                row.rc=text;
                row.full=window.title;
                row.trunc=extent.cx>(text.right-text.left);
                cache.hoverRows.push_back(std::move(row));
                y+=rowHeight;
            }
        }
        return true;
    });
}

static bool RefreshPickerPaintCache() noexcept {
    if(!g_main) return false;
    RECT client={0,0,0,0};
    return GetClientRect(g_main,&client) &&
           RebuildPickerPaintCache(client.right);
}

class PickerBackBuffer {
    HDC dc_=nullptr;
    PickerBitmapSelection selection_;
    int width_=0,height_=0;
public:
    bool ensure(HDC reference,int width,int height) noexcept {
        if(!reference || width<=0 || height<=0) return false;
        if(!dc_){
            dc_=CreateCompatibleDC(reference);
            if(!dc_) return false;
            HGDIOBJ original=GetCurrentObject(dc_,OBJ_BITMAP);
            if(!original){
                DeleteDC(dc_);
                dc_=nullptr;
                return false;
            }
            selection_.original=reinterpret_cast<uintptr_t>(original);
            selection_.selected=selection_.original;
        }
        if(selection_.owned && width_==width && height_==height)
            return true;
        HBITMAP replacement=CreateCompatibleBitmap(reference,width,height);
        if(!replacement) return false;
        HGDIOBJ previous=SelectObject(dc_,replacement);
        if(!previous || previous==HGDI_ERROR){
            DeleteObject(replacement);
            return false;
        }
        PickerBitmapSelection staged=selection_;
        uintptr_t release=0;
        if(!PublishPickerBitmapReplacement(staged,
                reinterpret_cast<uintptr_t>(replacement),
                reinterpret_cast<uintptr_t>(previous),release)){
            SelectObject(dc_,previous);
            DeleteObject(replacement);
            return false;
        }
        selection_=staged;
        width_=width;
        height_=height;
        if(release) DeleteObject(reinterpret_cast<HGDIOBJ>(release));
        return true;
    }
    void reset() noexcept {
        if(dc_ && selection_.original)
            SelectObject(dc_,reinterpret_cast<HGDIOBJ>(selection_.original));
        if(selection_.owned)
            DeleteObject(reinterpret_cast<HGDIOBJ>(selection_.owned));
        if(dc_) DeleteDC(dc_);
        dc_=nullptr;
        selection_=PickerBitmapSelection{};
        width_=height_=0;
    }
    HDC get() const noexcept { return dc_; }
};

class ScopedPickerPaint {
    HWND window_=nullptr;
    PAINTSTRUCT paint_={};
    HDC dc_=nullptr;
public:
    explicit ScopedPickerPaint(HWND window) noexcept :window_(window){
        dc_=BeginPaint(window_,&paint_);
    }
    ~ScopedPickerPaint(){ if(dc_) EndPaint(window_,&paint_); }
    HDC get() const noexcept { return dc_; }
};

static PickerBackBuffer g_pickerBuffer;

static void Paint(HDC hdcReal,HDC hdc,RECT client){
    HBRUSH bg=CreateSolidBrush(CLR_BG); FillRect(hdc,&client,bg); DeleteObject(bg); SetBkMode(hdc,TRANSPARENT);
    HFONT fT=g_fPT, fN=g_fPN, fI=g_fPI, fX=g_fPX;   // cached (created in InitMetrics)
    bool ctrlHeld=(GetKeyState(VK_CONTROL)&0x8000)!=0;

    // subtle rounded outer border
    { HPEN p=CreatePen(PS_SOLID,1,CLR_BORDER); HPEN op=(HPEN)SelectObject(hdc,p); HBRUSH ob=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
      RoundRect(hdc,0,0,client.right-1,client.bottom-1,S(18),S(18)); SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(p); }

    // ---- search box (rounded) + clear (x) button ----
    RECT sb=SearchBoxRect(client.right);
    FillRoundRect(hdc, sb, S(12), CLR_SEARCH, g_picker.searchActive?CLR_ACTIVE:CLR_PASSIVE, g_picker.searchActive?S(2):S(1));   // active only after user clicks/types
    if(g_picker.searchActive && g_pickerPaintCache.clearButton.right>g_pickerPaintCache.clearButton.left){ // big × (no circle) — shown whenever the field is active
        SelectObject(hdc,fX); SetTextColor(hdc,CLR_HINT); RECT xr=g_pickerPaintCache.clearButton; DrawTextW(hdc,L"\x2715",-1,&xr,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }

    // ---- header: left title + right Ctrl+Click hint ----
    int headTop=SEARCH_H, headBot=SEARCH_H+HEADER;
    SelectObject(hdc,fI); SetTextColor(hdc,CLR_HINT);
    const wchar_t* hint=L"Ctrl+Click - Move current window to selected desktop";
    RECT hr={PAD,headTop,client.right-PAD,headBot}; DrawTextW(hdc,hint,-1,&hr,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(hdc,fT); SetTextColor(hdc,CLR_HEAD);
    const std::wstring& head=ctrlHeld?
        g_pickerPaintCache.moveHeader:g_pickerPaintCache.switchHeader;
    RECT h2={PAD,headTop,client.right-PAD-g_pickerPaintCache.hintWidth-S(24),headBot}; DrawTextW(hdc,head.c_str(),-1,&h2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_VCENTER);

    // ---- tiles ----
    const COLORREF currentTile=BlendColor(CLR_TILE,CLR_ACTIVE,48);
    const COLORREF activeRow=BlendColor(CLR_TILE,CLR_ACTIVE,72);
    const bool searching=!g_picker.searchText.empty();
    for(const Tile& t : g_tiles){
        const bool isCurrent=IsCurrentDesktop(g_picker,t.guid);
        const bool isSelected=IsSelectedDesktop(g_picker,t.guid);
        const bool dim=searching && t.filtered.empty();
        const COLORREF fill=PickerTileFill(
            CLR_TILE,currentTile,CLR_TILE_DIM,isCurrent,dim);
        FillRoundRect(hdc,t.rc,S(10),fill,
            PickerTileBorder(isSelected,CLR_ACTIVE,CLR_PASSIVE),
            isSelected?S(2):S(1));
        SelectObject(hdc,fN); SetTextColor(hdc,dim?CLR_DIM:CLR_HEAD);
        RECT nr=t.rc; nr.left+=S(14); nr.top+=S(10); nr.right-=S(12); nr.bottom=nr.top+S(22);
        DrawTextW(hdc,t.displayName.c_str(),-1,&nr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,fI); SetTextColor(hdc,CLR_TEXT);
        int rowH=S(22), listTop=nr.bottom+S(6), listBot=t.rc.bottom-S(10);
        int visRows=std::max(0,(listBot-listTop)/rowH); int maxScroll=std::max(0,(int)t.filtered.size()-visRows);
        const int visibleScroll=PickerVisibleScroll(t.scroll,maxScroll);
        bool hasScroll=(int)t.filtered.size()>visRows; int rowRight=t.rc.right-(hasScroll?S(18):S(14));
        int y=listTop;
        for(size_t position=static_cast<size_t>(visibleScroll);
            position<t.filtered.size();++position){
            if(y+rowH>listBot+S(3)) break;
            const size_t windowIndex=t.filtered[position];
            if(windowIndex>=t.windows.size()) break;
            const WinItem& window=t.windows[windowIndex];
            if(IsActiveWindow(g_picker,window.identity)){
                RECT activeRect={t.rc.left+S(8),y-S(2),rowRight,y+S(19)};
                FillRoundRect(hdc,activeRect,S(5),activeRow,activeRow,S(1));
                RECT activeBar={activeRect.left+S(2),activeRect.top+S(3),
                                activeRect.left+S(5),activeRect.bottom-S(3)};
                FillRoundRect(hdc,activeBar,S(2),CLR_ACTIVE,CLR_ACTIVE,S(1));
            }
            if(window.icon)DrawIconEx(hdc,t.rc.left+S(14),y,window.icon,S(16),S(16),0,nullptr,DI_NORMAL);
            RECT ir; ir.left=t.rc.left+S(38); ir.top=y; ir.right=rowRight; ir.bottom=y+S(18);
            DrawTextW(hdc,window.title.c_str(),-1,&ir,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            y+=rowH;
        }
        if(hasScroll){                                   // custom rounded scrollbar
            RECT trk={t.rc.right-S(9),listTop,t.rc.right-S(5),listBot}; FillRoundRect(hdc,trk,S(2),CLR_SCROLL_TRK,CLR_SCROLL_TRK,1);
            int trkH=listBot-listTop, thbH=std::max(S(24),trkH*visRows/std::max(1,(int)t.filtered.size()));
            int thbY=listTop+(maxScroll>0?(trkH-thbH)*visibleScroll/maxScroll:0);
            RECT thb={t.rc.right-S(9),thbY,t.rc.right-S(5),thbY+thbH}; FillRoundRect(hdc,thb,S(2),CLR_SCROLL_THB,CLR_SCROLL_THB,1);
        }
    }
    if(hdc!=hdcReal)
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
        if(!g_picker.searchActive){ g_picker.searchActive=true; RefreshPickerPaintCache(); InvalidateRect(g_main,nullptr,FALSE); } }   // typing activates the field
    if(m==WM_LBUTTONDOWN && !g_picker.searchActive){ g_picker.searchActive=true; RefreshPickerPaintCache(); InvalidateRect(g_main,nullptr,FALSE); }   // click activates
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
static bool CaptureFastWindowForMove(HWND hwnd,FastWin& output,bool& tracked,
                                     bool& titleComplete){
    tracked=false;
    titleComplete=true;
    if(!IsWindow(hwnd)) return false;
    DWORD pid=0;
    if(!GetWindowThreadProcessId(hwnd,&pid) || pid==0) return false;
    ProcessSnapshot process=ReadProcessSnapshot(pid);
    if(process.started==0) return false;
    FastWin captured;
    captured.hwnd=hwnd;
    captured.pid=pid;
    captured.processStart=process.started;
    wchar_t className[64]={0};
    if(GetClassNameW(hwnd,className,64)>0){
        std::vector<AppProfile> profiles=ActiveProfiles();
        const AppProfile* profile=ClassifyBrowserCandidate(
            className,process.image,profiles);
        if(profile){ captured.app=profile->id; tracked=true; }
    }
    int length=GetWindowTextLengthW(hwnd);
    if(length>0){
        try { captured.title.resize(static_cast<size_t>(length)+1,L'\0'); }
        catch(...) { titleComplete=false; }
        if(titleComplete){
            int copied=GetWindowTextW(hwnd,&captured.title[0],length+1);
            if(copied<=0){ captured.title.clear(); titleComplete=false; }
            else captured.title.resize(static_cast<size_t>(copied));
        }
    }
    if(g_vdmDoc){
        HRESULT read=g_vdmDoc->GetWindowDesktopId(hwnd,&captured.desktop);
        if(FAILED(read)) captured.desktop=GUID{};
    }
    output=std::move(captured);
    return true;
}

static void Commit(int idx){
    if(idx<0 || idx>=(int)g_tiles.size()) return;
    HidePicker();
    if(!g_target || !IsWindow(g_target)) return;
    const GUID destination=g_tiles[idx].guid;
    if(GuidIsZero(destination) || GetDesktopIndexByGuid(destination)<0) return;
    FastWin fast;
    bool tracked=false,titleComplete=true;
    if(!CaptureFastWindowForMove(g_target,fast,tracked,titleComplete)) return;
    const std::string runtimeKey=RuntimeKey(fast);

    PickerMoveOperation operation;
    operation.operationId=TakeNonzeroId(g_nextOperationId);
    if(tracked){
        operation.app=fast.app;
        auto lifecycle=g_lifecycleByApp.find(fast.app);
        if(lifecycle!=g_lifecycleByApp.end() &&
           lifecycle->second.saveInFlight){
            operation.lifecycleSaveGeneration=
                lifecycle->second.saveGeneration;
            operation.lifecycleLayoutSignature=
                lifecycle->second.layoutSignature;
            operation.lifecycleSessionSignature=
                lifecycle->second.sessionStampSignature;
        }
    }
    MoveJob job;
    job.token.owner=MoveOwner::Picker;
    job.token.operationId=operation.operationId;
    job.token.jobId=TakeNonzeroId(g_nextMoveJobId);
    job.token.itemIndex=0;
    job.runtimeKey=runtimeKey;
    job.destination=destination;
    ReservedAutoIdentity reservation;
    reservation.token=job.token;
    reservation.identity=IdentityOf(fast);
    reservation.app=fast.app;
    reservation.originDesktop=fast.desktop;
    bool provisionalNeedsInsert=false;
    auto bound=g_recordByRuntime.find(runtimeKey);
    if(bound!=g_recordByRuntime.end() &&
       SameIdentity(bound->second.identity,IdentityOf(fast))){
        reservation.recordId=bound->second.recordId;
        job.recordId=bound->second.recordId;
    } else {
        std::string recordId;
        const PopupReservationRecordSource recordSource=
            SelectPopupReservationRecord(
                tracked,g_autoLoaded && g_autoWritesAllowed,titleComplete,
                !GuidIsZero(fast.desktop),
                [&](std::string& selected){
                    return SelectPendingPopupRecordId(
                        IdentityOf(fast),fast.app,g_pendingRecordByRuntime,
                        [&](const std::string& candidate,
                            const std::string& app,std::string& canonical){
                            GUID parsed{};
                            if(!ParseNonzeroLayoutGuid(
                                    candidate,parsed,&canonical)) return false;
                            for(const LayoutWin& saved : g_autoRecords){
                                GUID savedGuid{};
                                std::string savedCanonical;
                                if(ParseNonzeroLayoutGuid(
                                        saved.recordId,savedGuid,
                                        &savedCanonical) &&
                                   savedCanonical==canonical &&
                                   saved.app==app) return true;
                            }
                            return false;
                        },selected);
                },
                [&](std::string& selected){
                    auto provisional=
                        g_provisionalRecordByRuntime.find(runtimeKey);
                    if(provisional!=g_provisionalRecordByRuntime.end()){
                        selected=provisional->second;
                        return true;
                    }
                    selected=NewRecordId();
                    GUID parsed{};
                    if(!ParseNonzeroLayoutGuid(selected,parsed)){
                        selected.clear();
                        return false;
                    }
                    provisionalNeedsInsert=true;
                    return true;
                },recordId);
        if(recordSource!=PopupReservationRecordSource::None){
            reservation.recordId=recordId;
            job.recordId=recordId;
            if(recordSource==PopupReservationRecordSource::Provisional){
                LayoutWin origin;
                origin.recordId=recordId;
                origin.app=fast.app;
                origin.desktop=fast.desktop;
                origin.deskIndex=SnapshotDesktopIndex(fast.desktop);
                origin.activeTitle=W2U8(fast.title);
                MarkSeen(origin,UtcNowSeconds());
                reservation.provisionalOriginRecord=origin;
                reservation.hasProvisionalOriginRecord=true;
            }
        }
    }
    if(!reservation.recordId.empty())
        g_restoreBudgets.clearForExplicitRetry(reservation.recordId);
    MoveRuntimeBinding runtime;
    runtime.window=fast;
    runtime.destination=destination;
    ReservationHandoff handoff;
    bool insertedProvisional=false;
    bool insertedPickerOperation=false;
    bool insertedRuntime=false;
    const bool queued=RunSuccessorFirstReservationHandoff([&]{
        if(!BeginReservationHandoff(runtimeKey,reservation,handoff))
            return false;
        job.recordId=handoff.installed.recordId;
        if(provisionalNeedsInsert){
            if(!g_provisionalRecordByRuntime.emplace(
                    runtimeKey,handoff.installed.recordId).second) return false;
            insertedProvisional=true;
        }
        if(!operation.liveJobIds.insert(job.token.jobId).second) return false;
        if(!g_pickerOperations.emplace(operation.operationId,operation).second)
            return false;
        insertedPickerOperation=true;
        if(!g_moveRuntime.emplace(job.token.jobId,runtime).second) return false;
        insertedRuntime=true;
        if(!g_moveQueue.enqueue(job)) return false;
        return true;
    },[&]() noexcept {
        PublishReservationHandoff(handoff);
    },[&]{
        CancelDisplacedReservationHandoff(handoff);
    },[&]{
        if(insertedProvisional){
            auto provisional=g_provisionalRecordByRuntime.find(runtimeKey);
            if(provisional!=g_provisionalRecordByRuntime.end() &&
               provisional->second==handoff.installed.recordId)
                g_provisionalRecordByRuntime.erase(provisional);
        }
        if(insertedRuntime) g_moveRuntime.erase(job.token.jobId);
        if(insertedPickerOperation) g_pickerOperations.erase(operation.operationId);
        RollbackReservationHandoff(handoff);
    });
    if(!queued){
        return;
    }
    ArmMoveTimer();
    if(SWITCH_AFTER_MOVE){
        ScopedComPtr<IVirtualDesktop> desktop(GetDesktopByGuid(destination));
        if(desktop) g_vdmi->SwitchDesktop(desktop.get());
    }
}
static PickerTargetCaptureState CapturePickerTarget() noexcept {
    PickerTargetCaptureState capture;
    HWND window=GetForegroundWindow();
    if(window==g_main) window=nullptr;
    if(!window || !IsWindow(window)) return capture;
    capture.hwnd=reinterpret_cast<uintptr_t>(window);
    capture.identity=CapturePickerWindowIdentity(window);
    if(!SameIdentity(capture.identity,capture.identity)){
        CompletePickerTargetRecapture(
            capture,WindowIdentityRecapture::Lost);
        return capture;
    }
    try {
        const int length=GetWindowTextLengthW(window);
        if(length>0){
            std::wstring title(static_cast<size_t>(length)+1,L'\0');
            const int copied=GetWindowTextW(
                window,&title[0],length+1);
            if(copied>0){
                title.resize(static_cast<size_t>(copied));
                capture.title.swap(title);
            }
        }
    } catch(...) {
        capture.title.clear();
    }
    CompletePickerTargetRecapture(
        capture,RecaptureGenericWindowIdentity(capture.identity));
    return capture;
}

static void ShowPicker(PickerTargetCaptureState capture){
    if(g_degraded) return;   // desktop COM unavailable; startup dialog + tray tip already explain
    const bool modelReady=BuildModel(capture.identity,true);
    if(!modelReady) return;
    g_target=reinterpret_cast<HWND>(capture.hwnd);
    g_targetTitle.swap(capture.title);
    g_tabBlobsBuilt=false;
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
    const bool paintCacheReady=RefreshPickerPaintCache();
    if(!PickerShowPreparationComplete(modelReady,paintCacheReady)){
        HidePicker();
        g_pickerPaintCache.clear();
        g_target=nullptr;
        g_targetTitle.clear();
        return;
    }
    ShowWindow(g_main,SW_SHOW); SetForegroundWindow(g_main);
    if(g_search) SetFocus(g_search);
    InvalidateRect(g_main,nullptr,FALSE);
}
static void MoveSel(int dx,int dy){ if(g_tiles.empty())return; int r=g_sel/g_cols,c=g_sel%g_cols; c+=dx;r+=dy; int n=(int)g_tiles.size();
    if(c<0)c=0; if(c>=g_cols)c=g_cols-1; if(r<0)r=0; int idx=r*g_cols+c; if(idx>=n)idx=n-1; if(idx<0)idx=0; SetPickerSelectionWithLegacy(idx); RefreshPickerPaintCache(); InvalidateRect(g_main,nullptr,FALSE); }

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
static void ApplyAutoFix(){
    if(g_autoFix && !g_degraded){
        TryLoadAutoLayoutAndInitialize();
        return;
    }
    KillTimer(g_main,TIMER_HEARTBEAT);
    KillTimer(g_main,TIMER_AUTO_FLUSH);
    g_flushTimerArmed=false;
    g_flushTimerDueMs=0;
    g_heartbeatTimerArmed=false;
    ResetAutoRuntimeState();
    if(g_sessionRoutes.empty() && g_reconcileDeadlines.empty())
        KillTimer(g_main,TIMER_MONITOR);
    if(g_moveQueue.empty()) KillTimer(g_main,TIMER_MOVE_VERIFY);
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
    SetPickerSelectionWithLegacy(idx);
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
            UINT newHotVk=g_hotVk,newHotMods=g_hotMods;
            if(vk!=0){
                UINT mods=0;
                if(hf&HOTKEYF_SHIFT)mods|=MOD_SHIFT;
                if(hf&HOTKEYF_CONTROL)mods|=MOD_CONTROL;
                if(hf&HOTKEYF_ALT)mods|=MOD_ALT;
                newHotVk=vk; newHotMods=mods;
            }
            const bool newAutoFix=IsDlgButtonChecked(hwnd,IDC_AUTOFIX)==BST_CHECKED;
            const bool newRunAtLogon=IsDlgButtonChecked(hwnd,IDC_AUTOSTART)==BST_CHECKED;
            const bool newFirefox=IsDlgButtonChecked(hwnd,IDC_APP_FF)==BST_CHECKED;
            const bool newChrome=IsDlgButtonChecked(hwnd,IDC_APP_CR)==BST_CHECKED;
            const bool newEdge=IsDlgButtonChecked(hwnd,IDC_APP_ED)==BST_CHECKED;
            SettingsRuntimeSnapshot currentSettings;
            currentSettings.hotkeyVk=g_hotVk;
            currentSettings.hotkeyMods=g_hotMods;
            currentSettings.autoFix=g_autoFix;
            currentSettings.runAtLogon=GetRunAtLogon();
            currentSettings.firefox=g_appFirefox;
            currentSettings.chrome=g_appChrome;
            currentSettings.edge=g_appEdge;
            SettingsRuntimeSnapshot requestedSettings=currentSettings;
            requestedSettings.hotkeyVk=newHotVk;
            requestedSettings.hotkeyMods=newHotMods;
            requestedSettings.autoFix=newAutoFix;
            requestedSettings.runAtLogon=newRunAtLogon;
            requestedSettings.firefox=newFirefox;
            requestedSettings.chrome=newChrome;
            requestedSettings.edge=newEdge;
            if(!ApplySettingsRuntimeTransaction(
                    currentSettings,requestedSettings,
                    [](){
                        return CheckpointAutoLayout(
                            CheckpointReason::SettingsChange);
                    },
                    [](){
                        try { ResetAutoRuntimeState(); return true; }
                        catch(...) { return false; }
                    })){
                MessageBoxW(hwnd,L"The current automatic layout could not be saved. Settings were not changed; retry after storage becomes available.",APP_NAME,MB_ICONWARNING);
                return 0;
            }
            g_hotVk=currentSettings.hotkeyVk;
            g_hotMods=currentSettings.hotkeyMods;
            g_autoFix=currentSettings.autoFix;
            g_appFirefox=currentSettings.firefox;
            g_appChrome=currentSettings.chrome;
            g_appEdge=currentSettings.edge;
            SetRunAtLogon(currentSettings.runAtLogon);
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

static LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_HOTKEY: ShowPicker(CapturePickerTarget()); return 0;
    case WM_PAINT:{ ScopedPickerPaint paint(hwnd); HDC target=paint.get(); if(!target)return 0; RECT cr; GetClientRect(hwnd,&cr); HDC canvas=target; if(g_pickerBuffer.ensure(target,cr.right,cr.bottom))canvas=g_pickerBuffer.get(); Paint(target,canvas,cr); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: if((HWND)lp==g_search){ HDC dc=(HDC)wp; SetTextColor(dc,CLR_TEXT); SetBkColor(dc,CLR_SEARCH);
        static HBRUSH sbr=nullptr; if(!sbr)sbr=CreateSolidBrush(CLR_SEARCH); return (LRESULT)sbr; }
        return DefWindowProcW(hwnd,msg,wp,lp);
    case WM_KEYDOWN:{
        bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(wp==VK_CONTROL){ InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(wp==VK_ESCAPE){HidePicker();return 0;}
        if(wp==VK_RETURN||wp==VK_SPACE){Activate(g_picker.selectedIndex,ctrl);return 0;}
        if(wp==VK_LEFT){MoveSel(-1,0);return 0;} if(wp==VK_RIGHT){MoveSel(1,0);return 0;}
        if(wp==VK_UP){MoveSel(0,-1);return 0;} if(wp==VK_DOWN){MoveSel(0,1);return 0;}
        if(wp==VK_TAB){ bool sh=(GetKeyState(VK_SHIFT)&0x8000)!=0; int n=(int)g_tiles.size(); if(n){int selected=g_picker.selectedIndex; if(selected<0||selected>=n)selected=0; SetPickerSelectionWithLegacy((selected+(sh?-1:1)+n)%n); RefreshPickerPaintCache(); InvalidateRect(hwnd,nullptr,FALSE);} return 0; }
        if(wp>='1'&&wp<='9'){Activate((int)(wp-'1'),ctrl);return 0;} if(wp=='0'){Activate(9,ctrl);return 0;}
        return 0; }
    case WM_KEYUP:
        if(wp==VK_CONTROL) InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    case WM_LBUTTONDOWN:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(g_pickerPaintCache.clearButton.right>g_pickerPaintCache.clearButton.left && PtInRect(&g_pickerPaintCache.clearButton,pt)){ SetWindowTextW(g_search,L""); g_picker.searchActive=false; RefreshPickerPaintCache(); SetFocus(g_search); InvalidateRect(hwnd,nullptr,FALSE); return 0; }   // clear + deactivate border, keep caret
        { RECT cr; GetClientRect(hwnd,&cr); RECT sb=SearchBoxRect(cr.right); if(PtInRect(&sb,pt)){ g_picker.searchActive=true; RefreshPickerPaintCache(); SetFocus(g_search); InvalidateRect(hwnd,nullptr,FALSE); return 0; } }   // clicked the search field -> activate
        if(g_picker.searchActive){ g_picker.searchActive=false; RefreshPickerPaintCache(); InvalidateRect(hwnd,nullptr,FALSE); }   // clicked outside the field -> deactivate border
        for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ Activate((int)i,ctrl); return 0; } return 0; }
    case WM_MOUSEMOVE:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        int hovRow=-1; for(size_t i=0;i<g_pickerPaintCache.hoverRows.size();++i) if(PtInRect(&g_pickerPaintCache.hoverRows[i].rc,pt)){ hovRow=(int)i; break; }
        if(hovRow>=0 && g_pickerPaintCache.hoverRows[hovRow].trunc && g_tip){    // R9: full name on hover when truncated
            if(hovRow!=g_lastHoverRow){                        // update text/activate only on row change (avoid churn/lag)
                TOOLINFOW ti={0}; ti.cbSize=sizeof(ti); ti.hwnd=hwnd; ti.uId=1; ti.lpszText=(LPWSTR)g_pickerPaintCache.hoverRows[hovRow].full.c_str();
                SendMessageW(g_tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&ti);
                SendMessageW(g_tip,TTM_TRACKACTIVATE,TRUE,(LPARAM)&ti);
                g_lastHoverRow=hovRow;
            }
            POINT sp=pt; ClientToScreen(hwnd,&sp); SendMessageW(g_tip,TTM_TRACKPOSITION,0,(LPARAM)MAKELONG(sp.x+S(16),sp.y+S(20)));
        } else if(g_lastHoverRow!=-1){ TipDeactivate(); g_lastHoverRow=-1; }
        for(size_t i=0;i<g_tiles.size();++i) if(PtInRect(&g_tiles[i].rc,pt)){ if(g_picker.selectedIndex!=(int)i){SetPickerSelectionWithLegacy((int)i); RefreshPickerPaintCache(); InvalidateRect(hwnd,nullptr,FALSE);} break; }
        TRACKMOUSEEVENT tme={sizeof(tme)}; tme.dwFlags=TME_LEAVE; tme.hwndTrack=hwnd; TrackMouseEvent(&tme);
        return 0; }
    case WM_MOUSELEAVE: TipDeactivate(); g_lastHoverRow=-1; return 0;
    case WM_MOUSEWHEEL:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&pt); int delta=GET_WHEEL_DELTA_WPARAM(wp);   // R8: scroll a tile's window list
        for(auto& t:g_tiles) if(PtInRect(&t.rc,pt)){ RememberPickerScroll(t,AdvancePickerScroll(t.scroll,delta)); RefreshPickerPaintCache(); InvalidateRect(hwnd,nullptr,FALSE); break; }
        return 0; }
    case WM_COMMAND:                                            // R7: live-filter as the search text changes
        if(g_search && (HWND)lp==g_search && HIWORD(wp)==EN_CHANGE){
            int n=GetWindowTextLengthW(g_search); std::wstring s(n+1,0); GetWindowTextW(g_search,&s[0],n+1); s.resize(wcslen(s.c_str()));
            std::wstring searchText=LowerW(s);
            if(ApplyPickerSearchText(std::move(searchText))){
                if(!g_picker.searchText.empty()) EnsureTabSearch();
                RefreshPickerPaintCache();
            }
            InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    case WM_MOVE_CANCEL_RETRY:
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        if(g_moveCancellationPending){
            const size_t before=PendingMoveCancellationCount();
            RunMessageRouteNoThrow(
                [](){ AdvanceMoveQueue(); },[](){},[&](){
                    RefreshMoveCancellationPending();
                    const size_t after=PendingMoveCancellationCount();
                    g_moveCancellationRetry.completePostedAttempt(before,after);
                    if(g_moveCancellationPending &&
                       !ScheduleMoveCancellationRetry())
                        ReportStorageError(L"Cancelled window moves remain protected; cancellation will retry on the next move request.");
                });
        } else {
            g_moveCancellationRetry.clear();
        }
        return 0;
    case WM_AUTO_TIMER_RETRY:
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        RunMessageRouteNoThrow([](){
            if(g_autoFix && !g_degraded && !g_autoLoadRetry.loaded)
                TryLoadAutoLayoutAndInitialize();
        },[](){},[](){});
        return 0;
    case WM_TIMER:
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        if(wp==TIMER_HEARTBEAT){
            RunMessageRouteNoThrow([](){
                CheckpointAutoLayout(CheckpointReason::Heartbeat);
            },[](){},[](){});
            return 0;
        }
        if(wp==TIMER_AUTO_FLUSH){
            KillTimer(hwnd,TIMER_AUTO_FLUSH);
            g_flushTimerArmed=false;
            g_flushTimerDueMs=0;
            RunMessageRouteNoThrow([](){
                if(g_autoDirty) FlushAutoLayout(g_dirtyFlush.conflicted());
            },[](){},[](){ MaintainAutoFlushTimer(); });
            return 0;
        }
        if(wp==TIMER_MOVE_VERIFY){
            RunMessageRouteNoThrow([](){ AdvanceMoveQueue(); },[](){
                try {
                    const MoveJob* front=g_moveQueue.front();
                    if(front){
                        const uint64_t jobId=front->token.jobId;
                        if(!MarkMoveForTerminalRetirement(jobId)){
                            auto runtime=g_moveRuntime.find(jobId);
                            if(runtime!=g_moveRuntime.end())
                                runtime->second.cancelRequested=true;
                        }
                    }
                    RefreshMoveCancellationPending();
                    ScheduleMoveCancellationRetry();
                } catch(...) {}
            },[](){});
            return 0;
        }
        if(wp==TIMER_MONITOR){
            RunMessageRouteNoThrow([&](){
                const uint64_t nowMs=MonotonicNowMs();
                CancelExpiredSessionRoutes(nowMs);
                CancelExpiredReconcileOperations(nowMs);
                if(!g_autoFix || g_degraded){
                    if(g_sessionRoutes.empty() && g_reconcileDeadlines.empty())
                        KillTimer(hwnd,TIMER_MONITOR);
                    return;
                }
                if(!g_autoLoadRetry.loaded){
                    if(!g_autoLoadRetry.monitorStarted ||
                       g_autoLoadRetry.due(nowMs))
                        TryLoadAutoLayoutAndInitialize();
                    return;
                }
                ObserveFastSnapshots(CollectFastSnapshots());
                CancelExpiredSessionRoutes(MonotonicNowMs());
            },[](){},[](){ MaintainAutoFlushTimer(); });
        }
        return 0;
    case WM_ACTIVATE: if(LOWORD(wp)==WA_INACTIVE)HidePicker(); return 0;
    case WM_SESSION_RESULT: {
        std::unique_ptr<SessionResult> result((SessionResult*)lp);
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        const uint64_t requestId=result ? result->requestId : 0;
        RunMessageRouteNoThrow(
            [&](){ HandleSessionResult(std::move(result)); },
            [&](){ RetireFailedSessionResult(requestId); },[&](){
                if((!g_autoFix || g_degraded) && g_sessionRoutes.empty() &&
                   g_reconcileDeadlines.empty())
                    KillTimer(hwnd,TIMER_MONITOR);
            });
        return 0;
    }
    case WM_RECONCILE_RESULT: {
        std::unique_ptr<ReconcileResult> result((ReconcileResult*)lp);
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        const uint64_t operationId=result ? result->operationId : 0;
        RunMessageRouteNoThrow(
            [&](){ HandleReconcileResult(std::move(result)); },
            [&](){ RetireFailedReconcileResult(operationId); },[&](){
                if((!g_autoFix || g_degraded) && g_sessionRoutes.empty() &&
                   g_reconcileDeadlines.empty())
                    KillTimer(hwnd,TIMER_MONITOR);
            });
        return 0;
    }
    case WM_TRAY:
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        if(LOWORD(lp)==WM_RBUTTONUP){
            PickerTargetCaptureState pickerTarget=CapturePickerTarget();
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
            if(cmd==200)ShowPicker(std::move(pickerTarget));
            else if(cmd==201)StartManualSave();
            else if(cmd==202)StartManualRestore(true);
            else if(cmd==204)StartManualRestore(false);
            else if(cmd==203)OpenSettings();
            else if(cmd==206)OpenHelp();
            else if(cmd==205)OpenAbout();
            else if(cmd==209) RunTrayExit(
                [](){ return FinalizeAutoLayout(); },
                [=](){ DestroyWindow(hwnd); });
        } else if(LOWORD(lp)==WM_LBUTTONDBLCLK)
            ShowPicker(CapturePickerTarget());
        return 0;
    case WM_QUERYENDSESSION:
        CheckpointAutoLayout(CheckpointReason::QueryEndSession);
        return TRUE;
    case WM_ENDSESSION:
        FinalizeSessionAndQuiesce(wp!=0,
            [](){ return FinalizeAutoLayout(); },
            [=](){ return QuiesceRuntime(hwnd); });
        return 0;
    case WM_DESTROY:
        FinalizeAutoLayout();
        QuiesceRuntime(hwnd);
        g_pickerBuffer.reset();
        TrayRemove(); UnregisterHotKey(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        noexcept {
    try { return WndProcImpl(hwnd,msg,wp,lp); }
    catch(...) { return 0; }
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
    try { g_reconcileWorker.reset(new ReconcileWorker(g_main)); }
    catch(...) { g_reconcileWorker.reset(); }
    { DWORD pref=2; DwmSetWindowAttribute(g_main,33,&pref,sizeof(pref)); }   // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (Win11)
    TrayAdd(g_main);
    bool hk=ApplyHotkey();
    if(!hk) MessageBoxW(nullptr,L"Could not register the global hotkey.\n"
                               L"Another app may be using it. Change it in Settings,\n"
                               L"or open the picker via double-click on the tray icon.",APP_NAME,MB_ICONWARNING);
    ApplyAutoFix();   // start the lifecycle monitor timer if enabled
    std::wstring hkStr=HotkeyString(), tip;
    if(g_degraded)     tip=L"Running in limited mode (compatibility issue). See About for details.";
    else if(g_autoFix) tip=L"Running. Auto-restore is on. Press "+hkStr+L" to open the desktop picker.";
    else               tip=L"Running. Press "+hkStr+L" to move the active window to a desktop.";
    Balloon(tip);
    MSG msg;
    for(;;){
        DWORD waitMs=INFINITE;
        uint32_t retryDelay=0;
        if(g_runtimeQuiescence.acceptsDispatch() && g_autoFix && !g_degraded &&
           g_autoLoadRetry.monitorRetryDelayMs(
               MonotonicNowMs(),retryDelay))
            waitMs=static_cast<DWORD>(retryDelay);
        const DWORD wait=MsgWaitForMultipleObjectsEx(
            0,nullptr,waitMs,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(wait==WAIT_TIMEOUT){
            if(g_runtimeQuiescence.acceptsDispatch())
                TryLoadAutoLayoutAndInitialize();
            continue;
        }
        if(wait==WAIT_FAILED) break;
        if(!PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) continue;
        if(msg.message==WM_QUIT) break;
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
    UniqueWinHandle trayMutex;
    bool trayMutexOwned=false;
    auto acquireTrayInstance=[&](){
        HANDLE raw=CreateMutexW(nullptr,TRUE,
            L"Local\\VirtualDesktopExtension.Tray.Instance");
        const DWORD error=GetLastError();
        if(!raw){
            MessageBoxW(nullptr,L"Could not create the tray instance guard.",
                        APP_NAME,MB_OK|MB_ICONERROR);
            return TrayInstanceAcquireStatus::Failed;
        }
        trayMutex.reset(raw);
        if(error==ERROR_ALREADY_EXISTS){
            trayMutex.reset();
            MessageBoxW(nullptr,L"Virtual Desktop Extension is already running.",
                        APP_NAME,MB_OK|MB_ICONINFORMATION);
            return TrayInstanceAcquireStatus::AlreadyRunning;
        }
        trayMutexOwned=true;
        return TrayInstanceAcquireStatus::Acquired;
    };
    auto releaseTrayInstance=[&](){
        if(trayMutexOwned && trayMutex) ReleaseMutex(trayMutex.get());
        trayMutexOwned=false;
        trayMutex.reset();
    };
    auto dispatch=[&](){
        int dispatchResult=1;
        if(cli){
            if(AttachConsole(ATTACH_PARENT_PROCESS)){ FILE* f; freopen_s(&f,"CONOUT$","w",stdout); freopen_s(&f,"CONOUT$","w",stderr); }
            SetConsoleOutputCP(CP_UTF8);
            dispatchResult=RunCliWithLoadedSettings(
                []{ LoadSettings(); },
                [&](){
                    if(!InitializeServicesWithRollback(
                            []{ return InitServices(); },
                            []{ return SanityCheckServices(); },
                            []{ ReleaseServices(); })){
                        printf("Virtual-desktop services unavailable: a Windows update may have changed the undocumented interfaces. Please report your build (%lu) to info@conus.vision or https://github.com/conus-vision/win-vde\n",(unsigned long)GetWindowsBuild());
                        return 3;
                    }
                    WriteLastGoodBuild(GetWindowsBuild());
                    return CliRun(cmd);
                });
        } else {
            g_inst=hInst;
            InitMetrics();
            bool good=InitializeServicesWithRollback(
                []{ return InitServices(); },
                []{ return SanityCheckServices(); },
                []{ ReleaseServices(); });
            if(good){
                WriteLastGoodBuild(GetWindowsBuild());
                dispatchResult=RunGui(hInst);
            } else { // explain the degraded mode instead of exiting silently
                g_degraded=true;
                DWORD last=ReadLastGoodBuild();
                ShowCompatIssue(last!=0 && last!=GetWindowsBuild());
                dispatchResult=RunGui(hInst);
            }
        }
        ReleaseServices();
        return dispatchResult;
    };

    int rc=1;
    try { rc=RunWithTrayInstanceScope(
        cli,acquireTrayInstance,dispatch,releaseTrayInstance); }
    catch(...) {
        ReleaseServices();
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return rc;
}
