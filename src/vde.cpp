// vde.cpp — Virtual Desktop Extension for Windows 11
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
#include <inspectable.h>
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
#include "gdi_buffer.hpp"
#include "icon_cache.hpp"
#include "picker_state.hpp"
#include "picker_trace.hpp"
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
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

// =============================== app config ==================================
static const wchar_t* APP_NAME  = L"Virtual Desktop Extension for Windows 11";
static const wchar_t* APP_SHORT = L"Virtual Desktop Extension"; // <=63 chars for balloon title
static const wchar_t* REPO_URL = FooterRepoUrl();
static const wchar_t* CONUS_URL = FooterConusUrl();

static UINT g_hotMods = MOD_CONTROL | MOD_ALT;  // Ctrl+Alt+D по умолчанию
static UINT g_hotVk   = 'D';
static bool g_autoFix = true;                   // монитор: авто-сохранение и авто-восстановление раскладки
static bool g_degraded = false;                 // недокументированный COM не работает (обновление Windows) -> урезанный режим
static bool g_appFirefox = true, g_appChrome = true, g_appEdge = true;  // какие приложения отслеживать
#define IDI_APPICON 101    // должен совпадать с ID в vde.rc
#define TIMER_MONITOR 1
#define TIMER_MOVE_VERIFY 3
#define TIMER_HEARTBEAT 4
#define TIMER_AUTO_FLUSH 5
#define TIMER_PICKER_TRANSITION 6
#define TIMER_PICKER_SEARCH_RETRY 7
#define TIMER_PICKER_ICON_PRELOAD 8
#define WM_PICKER_TRANSITION (WM_APP + 16)
#define WM_PICKER_SEARCH_RETRY (WM_APP + 17)
#define WM_MOVE_CANCEL_RETRY (WM_APP + 14)
#define WM_AUTO_TIMER_RETRY (WM_APP + 15)
#define MONITOR_INTERVAL_MS 5000
#define MOVE_VERIFY_INTERVAL_MS 150
#define PICKER_IDLE_REFRESH_MS 1000
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
static const wchar_t* APP_VERSION = L"1.1.0";

static HWND g_main=nullptr;
static void Balloon(const std::wstring& text);
static PickerState g_picker;
static PickerPointerGesture g_pickerGesture;
struct PickerDragPreviewState {
    bool captured=false;
    std::wstring fullTitle;
    std::string runtimeKey;
    WindowIdentityKey identity;
    SIZE size={0,0};
    POINT grabOffset={0,0};
    POINT pointer={0,0};
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;

    void swap(PickerDragPreviewState& other) noexcept {
        std::swap(captured,other.captured);
        fullTitle.swap(other.fullTitle);
        runtimeKey.swap(other.runtimeKey);
        std::swap(identity,other.identity);
        std::swap(size,other.size);
        std::swap(grabOffset,other.grabOffset);
        std::swap(pointer,other.pointer);
        std::swap(modelGeneration,other.modelGeneration);
        std::swap(rowLayoutEpoch,other.rowLayoutEpoch);
    }
};
static PickerDragPreviewState g_pickerDragPreview;
static PickerTraceSession g_pickerTrace;
static PickerTabSearchCacheState g_pickerTabSearchCache;
static bool g_suppressPickerCtrlSpaceChar=false;

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
    bool timerArmFailureCancellation=false;
    IdentityRecaptureRetryBudget identityRecaptureBudget;
    IssuedMoveRetirementTracker retirement;
};

struct ReservedAutoIdentity {
    MoveToken token;
    WindowIdentityKey identity;
    std::string app;
    std::string recordId;
    GUID originDesktop={0};
    uint64_t identityGeneration=0;
    LayoutWin acceptedFreshRecord;
    bool hasAcceptedFreshRecord=false;
    LayoutWin provisionalOriginRecord;
    bool hasProvisionalOriginRecord=false;
};

struct AcceptedFreshRuntime {
    WindowIdentityKey identity;
    uint64_t identityGeneration=0;
    LayoutWin record;
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
    bool reconcileFastKnown=false;
    ReconcileWorkMode reconcileMode=ReconcileWorkMode::Plan;
    std::vector<FastWin> reconcileFast;
    std::vector<DeskRec> currentDesktops;
    std::unique_ptr<ReconcileResult> reconcile;
    std::map<std::string,PickerOperationLifetimeClaim>
        pickerClaimedRecordByRuntime;
    MoveTerminalOutcomes successfulLive;
    std::set<uint64_t> liveJobIds;
    size_t outstanding=0;
    bool hadExhausted=false;
    bool hadFailure=false;
    bool cancellationPending=false;
    PickerOperationClaimRearmControl pickerClaimRearm;
};

struct SearchOperation {
    uint64_t operationId=0;
    uint64_t pickerModelGeneration=0;
    std::wstring pickerQuery;
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
static std::map<std::string,AcceptedFreshRuntime> g_acceptedFreshByRuntime;
static std::map<uint64_t,MoveRuntimeBinding> g_moveRuntime;
static std::map<uint64_t,SessionRoute> g_sessionRoutes;
static std::map<uint64_t,AutoRestoreOperation> g_pendingAutoOperations;
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
static const GUID kCLSID_VirtualDesktopPinnedApps=
    {0xB5A399E7,0x1C87,0x46B8,
     {0x88,0xE9,0xFC,0x57,0x47,0xB1,0x71,0xBD}};
static const GUID kIID_IVirtualDesktopPinnedApps=
    {0x4CE81583,0x1E4C,0x4632,
     {0xA6,0x21,0x07,0xA5,0x35,0x43,0x14,0x8F}};

// ============================ Undocumented interfaces =========================
struct __declspec(uuid("372E1D3B-38D3-42E4-A15B-8AB2B178F513"))
IApplicationView : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetFocus()=0;
    virtual HRESULT STDMETHODCALLTYPE SwitchTo()=0;
    virtual HRESULT STDMETHODCALLTYPE TryInvokeBack(void*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetThumbnailWindow(HWND*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitor(void**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetVisibility(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE SetCloak(int,int)=0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(REFIID,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE SetPosition(void*)=0;
    virtual HRESULT STDMETHODCALLTYPE InsertAfterWindow(HWND)=0;
    virtual HRESULT STDMETHODCALLTYPE GetExtendedFramePosition(RECT*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppUserModelId(PWSTR*)=0;
};
struct __declspec(uuid("4CE81583-1E4C-4632-A621-07A53543148F"))
IVirtualDesktopPinnedApps : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsAppIdPinned(
        PCWSTR,BOOL*)=0;
    virtual HRESULT STDMETHODCALLTYPE PinAppID(PCWSTR)=0;
    virtual HRESULT STDMETHODCALLTYPE UnpinAppID(PCWSTR)=0;
    virtual HRESULT STDMETHODCALLTYPE IsViewPinned(
        IApplicationView*,BOOL*)=0;
    virtual HRESULT STDMETHODCALLTYPE PinView(IApplicationView*)=0;
    virtual HRESULT STDMETHODCALLTYPE UnpinView(IApplicationView*)=0;
};
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
static IVirtualDesktopPinnedApps*      g_pinnedApps=nullptr;

static void TryInitializePinnedApps() noexcept {
    if(!g_shell || g_pinnedApps) return;
    IVirtualDesktopPinnedApps* candidate=nullptr;
    HRESULT result=E_NOINTERFACE;
    try {
        result=g_shell->QueryService(
            kCLSID_VirtualDesktopPinnedApps,
            kIID_IVirtualDesktopPinnedApps,
            reinterpret_cast<void**>(&candidate));
    } catch(...) {
        result=E_FAIL;
    }
    if(SUCCEEDED(result) && candidate)
        g_pinnedApps=candidate;
    else if(candidate)
        candidate->Release();
}

static void ReleasePinnedApps() noexcept {
    IVirtualDesktopPinnedApps* prior=g_pinnedApps;
    g_pinnedApps=nullptr;
    if(prior) prior->Release();
}

static void ReleaseServices();
static bool InitServices(){
    HRESULT result=CoCreateInstance(
        kCLSID_ImmersiveShell,nullptr,CLSCTX_LOCAL_SERVER,
        __uuidof(IServiceProvider),reinterpret_cast<void**>(&g_shell));
    if(SUCCEEDED(result) && g_shell) result=g_shell->QueryService(
        kCLSID_VirtualDesktopManagerInternal,kIID_IVirtualDesktopManagerInternal,
        reinterpret_cast<void**>(&g_vdmi));
    else if(SUCCEEDED(result)) result=E_NOINTERFACE;
    if(SUCCEEDED(result)) result=g_shell->QueryService(
        kIID_IApplicationViewCollection,kIID_IApplicationViewCollection,
        reinterpret_cast<void**>(&g_avc));
    if(SUCCEEDED(result)) result=CoCreateInstance(
        CLSID_VirtualDesktopManager,nullptr,CLSCTX_INPROC_SERVER,
        IID_IVirtualDesktopManager,reinterpret_cast<void**>(&g_vdmDoc));
    if(FAILED(result) ||
       !DesktopServicesReady(g_vdmi!=nullptr,g_avc!=nullptr,g_vdmDoc!=nullptr)){
        ReleaseServices();
        return false;
    }
    TryInitializePinnedApps();
    return true;
}
static void ReleaseServices(){
    ReleasePinnedApps();
    if(g_vdmDoc){ g_vdmDoc->Release(); g_vdmDoc=nullptr; }
    if(g_avc){ g_avc->Release(); g_avc=nullptr; }
    if(g_vdmi){ g_vdmi->Release(); g_vdmi=nullptr; }
    if(g_shell){ g_shell->Release(); g_shell=nullptr; }
}

template<class T>
class ScopedComPtr {
public:
    ScopedComPtr()=default;
    explicit ScopedComPtr(T* value):value_(value){}
    ~ScopedComPtr() noexcept { reset(); }
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
    T* release() noexcept { T* value=value_; value_=nullptr; return value; }
    void reset(T* value=nullptr) noexcept {
        if(value_) value_->Release();
        value_=value;
    }
private:
    T* value_=nullptr;
};

class ScopedCoTaskMemString {
public:
    ScopedCoTaskMemString() noexcept=default;
    ~ScopedCoTaskMemString() noexcept {
        if(value_) CoTaskMemFree(value_);
    }
    ScopedCoTaskMemString(const ScopedCoTaskMemString&)=delete;
    ScopedCoTaskMemString& operator=(
        const ScopedCoTaskMemString&)=delete;
    ScopedCoTaskMemString(ScopedCoTaskMemString&&)=delete;
    ScopedCoTaskMemString& operator=(
        ScopedCoTaskMemString&&)=delete;

    PWSTR* out() noexcept { return &value_; }
    PCWSTR get() const noexcept { return value_; }
    bool usable() const noexcept {
        return value_ && value_[0]!=L'\0';
    }

private:
    PWSTR value_=nullptr;
};

// After a Windows update the undocumented vtable/IIDs can shift: QueryService may
// still succeed but calls return garbage. Verify a couple of calls make sense.
static bool SanityCheckServices(){
    if(!DesktopServicesReady(g_vdmi!=nullptr,g_avc!=nullptr,g_vdmDoc!=nullptr))
        return false;
    UINT n=0; if(FAILED(g_vdmi->GetCount(&n))) return false;
    if(n<1 || n>MAX_VIRTUAL_DESKTOPS) return false;
    IVirtualDesktop* d=nullptr;
    const HRESULT currentResult=g_vdmi->GetCurrentDesktop(&d);
    if(!ValidateComOutPointerOrRelease(
            currentResult,d!=nullptr,[&](){ d->Release(); d=nullptr; }))
        return false;
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

struct DesktopCollectionComOps {
    HRESULT getDesktops(IObjectArray** output) const noexcept {
        return g_vdmi ? g_vdmi->GetDesktops(output) : E_NOINTERFACE;
    }
    HRESULT getCount(IObjectArray* array,UINT* output) const noexcept {
        return array->GetCount(output);
    }
    HRESULT getAt(IObjectArray* array,UINT index,
                  IVirtualDesktop** output) const noexcept {
        return array->GetAt(index,kIID_IVirtualDesktop,
                            reinterpret_cast<void**>(output));
    }
    HRESULT getId(IVirtualDesktop* desktop,GUID* output) const noexcept {
        return desktop->GetID(output);
    }
    void releaseArray(IObjectArray* value) const noexcept { value->Release(); }
    void releaseDesktop(IVirtualDesktop* value) const noexcept {
        value->Release();
    }
};

static PickerTraceDesktopLookupStage PickerTraceLookupStage(
        DesktopCollectionLookupObservationStage stage) noexcept {
    switch(stage){
    case DesktopCollectionLookupObservationStage::ValidateRequest:
        return PickerTraceDesktopLookupStage::ValidateRequest;
    case DesktopCollectionLookupObservationStage::GetDesktops:
        return PickerTraceDesktopLookupStage::GetDesktops;
    case DesktopCollectionLookupObservationStage::GetCount:
        return PickerTraceDesktopLookupStage::GetCount;
    case DesktopCollectionLookupObservationStage::GetAt:
        return PickerTraceDesktopLookupStage::GetAt;
    case DesktopCollectionLookupObservationStage::GetId:
        return PickerTraceDesktopLookupStage::GetId;
    case DesktopCollectionLookupObservationStage::Match:
        return PickerTraceDesktopLookupStage::Match;
    case DesktopCollectionLookupObservationStage::NotFound:
        return PickerTraceDesktopLookupStage::NotFound;
    case DesktopCollectionLookupObservationStage::Exception:
        return PickerTraceDesktopLookupStage::Exception;
    }
    return PickerTraceDesktopLookupStage::Exception;
}

template<class DesktopOwner>
static bool LookupPickerDesktopByGuid(
        const GUID& target,DesktopCollectionComOps& ops,
        DesktopOwner& desktop,int& matchedIndex,
        const PickerTraceDesktopLookupContext* context) noexcept {
    if(!context || !context->trace || !context->trace->active())
        return LookupDesktopCollectionOwned<IObjectArray,IVirtualDesktop>(
            DesktopCollectionLookupRequest::ByGuid(target),
            ops,desktop,matchedIndex);
    std::function<void(const DesktopCollectionLookupObservation&)>
        observer;
    try {
        observer=[context](
                const DesktopCollectionLookupObservation& observation){
            EmitPickerTraceDesktopLookupStage(
                context,PickerTraceLookupStage(observation.stage),
                observation.index,observation.result,
                observation.actual,observation.matched);
        };
    } catch(...) {
        return LookupDesktopCollectionOwned<IObjectArray,IVirtualDesktop>(
            DesktopCollectionLookupRequest::ByGuid(target),
            ops,desktop,matchedIndex);
    }
    return LookupDesktopCollectionOwned<IObjectArray,IVirtualDesktop>(
        DesktopCollectionLookupRequest::ByGuid(target),
        ops,desktop,matchedIndex,&observer);
}

static ScopedComPtr<IVirtualDesktop> GetDesktopByIndex(UINT index){
    DesktopCollectionComOps ops;
    ScopedComPtr<IVirtualDesktop> desktop;
    int matchedIndex=-1;
    LookupDesktopCollectionOwned<IObjectArray,IVirtualDesktop>(
        DesktopCollectionLookupRequest::ByIndex(index),
        ops,desktop,matchedIndex);
    return desktop;
}

static ScopedComPtr<IVirtualDesktop> GetDesktopByGuid(
        const GUID& target,
        const PickerTraceDesktopLookupContext* traceContext=nullptr){
    DesktopCollectionComOps ops;
    ScopedComPtr<IVirtualDesktop> desktop;
    int matchedIndex=-1;
    LookupPickerDesktopByGuid(
        target,ops,desktop,matchedIndex,traceContext);
    return desktop;
}

static int GetDesktopIndexByGuid(
        const GUID& target,
        const PickerTraceDesktopLookupContext* traceContext=nullptr){
    DesktopCollectionComOps ops;
    ScopedComPtr<IVirtualDesktop> desktop;
    int matchedIndex=-1;
    if(!LookupPickerDesktopByGuid(
            target,ops,desktop,matchedIndex,traceContext)) return -1;
    return matchedIndex;
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

static WindowIdentityRecapture RecaptureGenericWindowIdentity(
    const WindowIdentityKey& expected) noexcept;

static void MarkAllFastProfilesIncomplete(FastEnumContext& context) noexcept {
    MarkFastSnapshotCaptureIncomplete(
        *context.profiles,*context.snapshots);
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
            MarkAllFastProfilesIncomplete(context);
            return TRUE;
        }
        auto process=context.processes.find(pid);
        if(process==context.processes.end())
            process=context.processes.emplace(pid,ReadProcessSnapshot(pid)).first;
        if(process->second.image.empty() || process->second.started==0){
            MarkAllFastProfilesIncomplete(context);
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
                MarkAllFastProfilesIncomplete(context);
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
        const WindowIdentityRecapture finalIdentity=
            RecaptureGenericWindowIdentity(IdentityOf(window));
        if(FinalFastWindowIdentityFailureInvalidatesEveryProfile(
                finalIdentity)){
            MarkAllFastProfilesIncomplete(context);
            return TRUE;
        }
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

struct PickerProcessStartTraceFacts {
    bool openAttempted=false;
    bool processOpened=false;
    DWORD openError=ERROR_SUCCESS;
    bool timesAttempted=false;
    bool timesRead=false;
    DWORD timesError=ERROR_SUCCESS;
};

static bool TryReadProcessStart(
        DWORD pid,uint64_t& started,
        PickerProcessStartTraceFacts* facts=nullptr) noexcept {
    if(facts) *facts=PickerProcessStartTraceFacts{};
    if(pid==0) return false;
    if(facts) facts->openAttempted=true;
    if(facts) SetLastError(ERROR_SUCCESS);
    UniqueWinHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if(facts){
        facts->processOpened=static_cast<bool>(process);
        facts->openError=GetLastError();
    }
    if(!process) return false;
    FILETIME created{},exited{},kernel{},user{};
    if(facts) facts->timesAttempted=true;
    if(facts) SetLastError(ERROR_SUCCESS);
    const BOOL timesRead=GetProcessTimes(
        process.get(),&created,&exited,&kernel,&user);
    if(facts){
        facts->timesRead=timesRead!=FALSE;
        facts->timesError=GetLastError();
    }
    if(!timesRead)
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

struct TargetMobilityProbeFacts {
    WindowIdentityRecapture identity=
        WindowIdentityRecapture::Indeterminate;
    TargetDesktopRoute route=TargetDesktopRoute::Indeterminate;
    HRESULT viewPinnedResult=E_NOINTERFACE;
    HRESULT appIdResult=E_NOINTERFACE;
    HRESULT appPinnedResult=E_NOINTERFACE;
    HRESULT canMoveResult=E_NOINTERFACE;
    BOOL viewPinned=FALSE;
    BOOL appPinned=FALSE;
    int canMove=0;
    bool viewPinnedInvoked=false;
    bool appIdInvoked=false;
    bool appPinnedInvoked=false;
    bool canMoveInvoked=false;
    TargetMobilityDecision decision;
};

static void EmitTargetMobilityProbeEvidence(
        PickerTraceApiKind api,const WindowIdentityKey& expected,
        HRESULT result,bool invoked,BOOL value=FALSE) noexcept {
    PickerTraceApiResultEvent event;
    event.api=api;
    event.resultKind=PickerTraceRawResultKind::HResult;
    event.hwnd=expected.hwnd;
    event.hresult=result;
    event.boolResult=value!=FALSE;
    event.invoked=invoked;
    g_pickerTrace.emit(event);
}

static TargetMobilityDecision QueryTargetWindowMobility(
        const WindowIdentityKey& expected,
        TargetDesktopRoute knownRoute,IApplicationView* view,
        TargetMobilityProbeFacts& facts) noexcept {
    facts=TargetMobilityProbeFacts{};
    facts.route=knownRoute;
    facts.identity=RecaptureGenericWindowIdentity(expected);
    if(facts.identity!=WindowIdentityRecapture::Match)
        return facts.decision;

    TargetMobilityEvidence evidence;
    evidence.desktopRoute=knownRoute;
    if(!view){
        facts.decision=DecideTargetMobility(evidence);
        return facts.decision;
    }
    if(g_pinnedApps){
        facts.viewPinnedInvoked=true;
        try {
            facts.viewPinnedResult=g_pinnedApps->IsViewPinned(
                view,&facts.viewPinned);
        } catch(...) {
            facts.viewPinnedResult=E_FAIL;
        }
    }
    EmitTargetMobilityProbeEvidence(
        PickerTraceApiKind::IsViewPinned,expected,
        facts.viewPinnedResult,facts.viewPinnedInvoked,
        facts.viewPinned);
    evidence.viewPinned=MobilityEvidenceFromBoolean(
        facts.viewPinnedResult,facts.viewPinned);

    ScopedCoTaskMemString appId;
    facts.appIdInvoked=true;
    try {
        facts.appIdResult=view->GetAppUserModelId(appId.out());
    } catch(...) {
        facts.appIdResult=E_FAIL;
    }
    EmitTargetMobilityProbeEvidence(
        PickerTraceApiKind::GetAppUserModelId,expected,
        facts.appIdResult,facts.appIdInvoked);
    if(SUCCEEDED(facts.appIdResult) && appId.usable() &&
       g_pinnedApps){
        facts.appPinnedInvoked=true;
        try {
            facts.appPinnedResult=g_pinnedApps->IsAppIdPinned(
                appId.get(),&facts.appPinned);
        } catch(...) {
            facts.appPinnedResult=E_FAIL;
        }
    }
    EmitTargetMobilityProbeEvidence(
        PickerTraceApiKind::IsAppIdPinned,expected,
        facts.appPinnedResult,facts.appPinnedInvoked,
        facts.appPinned);
    evidence.appPinned=MobilityEvidenceFromBoolean(
        facts.appPinnedResult,facts.appPinned);

    if(g_vdmi){
        facts.canMoveInvoked=true;
        try {
            facts.canMoveResult=g_vdmi->CanViewMoveDesktops(
                view,&facts.canMove);
        } catch(...) {
            facts.canMoveResult=E_FAIL;
        }
    }
    EmitTargetMobilityProbeEvidence(
        PickerTraceApiKind::CanViewMoveDesktops,expected,
        facts.canMoveResult,facts.canMoveInvoked,
        facts.canMove!=0 ? TRUE : FALSE);
    evidence.canMove=MobilityEvidenceFromBoolean(
        facts.canMoveResult,facts.canMove!=0);
    facts.decision=DecideTargetMobility(evidence);
    return facts.decision;
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
    DesktopCollectionComOps ops;
    std::vector<DesktopCollectionEntry> snapshot;
    if(!SnapshotDesktopCollectionOwned<IObjectArray,IVirtualDesktop>(
            ops,snapshot))
        return fail("failed to enumerate virtual desktops");
    std::vector<DeskRec> desks;
    try { desks.reserve(snapshot.size()); }
    catch(...) { return fail("out of memory collecting virtual desktops"); }
    for(const DesktopCollectionEntry& entry : snapshot){
        try {
            DeskRec record;
            record.index=static_cast<int>(entry.index);
            record.guid=entry.guid;
            record.name=DesktopNameFromRegistry(entry.guid);
            desks.push_back(std::move(record));
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

static const GUID& DeskGuid(const DeskRec& desktop) noexcept {
    return desktop.guid;
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

static bool BuildPickerCapturedTitleOnlyProvisional(
        const std::string& app,const std::wstring& capturedTitle,
        LayoutWin& output) noexcept {
    try {
        std::vector<AppProfile> profiles;
        const AppProfile* profile=FindActiveProfile(app,profiles);
        if(!profile || capturedTitle.empty()) return false;
        const std::wstring normalized=StripReconcileTitleSuffix(
            capturedTitle,profile->titleSuffixes);
        const std::string title=W2U8(normalized);
        if(!PickerTitleOnlyProvisionalFieldsUsable(app,title)) return false;
        LayoutWin built;
        built.app=app;
        built.activeTitle=title;
        built.provisional=true;
        output=std::move(built);
        return true;
    } catch(...) { return false; }
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

static void MarkPickerOperationClaimsPublished(
        const std::string& runtimeKey,
        const std::string& recordId) noexcept {
    if(runtimeKey.empty() || recordId.empty()) return;
    try {
        for(auto& operation : g_pendingAutoOperations)
            MarkPickerOperationLifetimeClaimPublished(
                operation.second.pickerClaimedRecordByRuntime,
                runtimeKey,recordId,PopupSaveStatus::Saved,
                PopupSaveFailure::None);
    } catch(...) {}
}

static void MarkPickerOperationClaimsTerminalOutcome(
        const std::string& runtimeKey,const std::string& recordId,
        bool targetRestored) noexcept {
    if(runtimeKey.empty() || recordId.empty()) return;
    try {
        for(auto& operation : g_pendingAutoOperations)
            MarkPickerOperationLifetimeClaimTerminalOutcome(
                operation.second.pickerClaimedRecordByRuntime,
                runtimeKey,recordId,targetRestored);
    } catch(...) {}
}

static PopupSaveResult SavePopupMovedWindow(
        const PickerTransition& transition) noexcept {
    PopupSaveResult result;
    result.status=PopupSaveStatus::Failed;
    try {
        const WindowIdentityKey identity=transition.target;
        const WindowIdentityRecapture recapture=
            RecaptureGenericWindowIdentity(identity);
        if(recapture!=WindowIdentityRecapture::Match){
            result.failure=recapture==WindowIdentityRecapture::Lost
                ? PopupSaveFailure::IdentityLost
                : PopupSaveFailure::IdentityIndeterminate;
            return result;
        }

        std::string app;
        const PopupBrowserClassification classification=
            ClassifyTrackedBrowserWindow(identity,app);
        const PopupPersistenceReadiness readiness=!g_autoLoaded
            ? PopupPersistenceReadiness::Unavailable
            : (!g_autoWritesAllowed || !g_autoFix || g_degraded)
                ? PopupPersistenceReadiness::ReadOnly
                : PopupPersistenceReadiness::Ready;
        const std::string& terminalApp=app.empty() ? transition.app : app;
        return RunPickerPersistenceTransaction(
            classification,terminalApp,readiness,
            [&](const std::string& exactApp)->PopupSaveResult {
        PopupSaveResult result;
        if(!TryStagePickerPersistenceAppNoThrow(result,exactApp)){
            result.failure=PopupSaveFailure::Unexpected;
            return result;
        }

        const std::string runtimeKey=RuntimeKey(identity);
        auto reserved=g_reservedAutoIdentities.find(runtimeKey);
        if(reserved==g_reservedAutoIdentities.end() ||
           !SameMoveToken(reserved->second.token,
                          transition.reservationToken) ||
           !SameIdentity(reserved->second.identity,identity) ||
           !PickerTerminalReservationAppAllowed(
               reserved->second.app,app) ||
           GuidIsZero(transition.destination)){
            result.failure=PopupSaveFailure::ReservationUnavailable;
            return result;
        }
        GUID parsedId{};
        std::string recordId;
        LayoutWin terminalProvisional;
        bool hasTerminalProvisional=false;
        PopupSaveFailure recordFailure=PopupSaveFailure::InvalidRecordId;
        if(!SelectPopupPersistRecordId(
                reserved->second.recordId,
                [&](std::string& selected){
                    return SelectPendingPopupRecordId(
                        identity,app,g_pendingRecordByRuntime,
                        [&](const std::string& candidate,
                            const std::string& expectedApp,
                            std::string& canonical){
                            GUID parsed{};
                            if(!ParseNonzeroLayoutGuid(
                                    candidate,parsed,&canonical))
                                return false;
                            for(const LayoutWin& saved : g_autoRecords)
                                if(saved.recordId==canonical &&
                                   saved.app==expectedApp) return true;
                            return false;
                        },selected);
                },
                [&](std::string& selected){
                    recordFailure=PopupSaveFailure::IncompleteTitle;
                    if(!transition.capturedTitleComplete ||
                       transition.capturedTitle.empty()) return false;
                    std::string generated=NewRecordId();
                    GUID parsed{};
                    std::string canonical;
                    if(!ParseNonzeroLayoutGuid(
                            generated,parsed,&canonical)){
                        recordFailure=PopupSaveFailure::InvalidRecordId;
                        return false;
                    }
                    if(!BuildPickerCapturedTitleOnlyProvisional(
                            app,transition.capturedTitle,
                            terminalProvisional)) return false;
                    selected.swap(canonical);
                    hasTerminalProvisional=true;
                    return true;
                },recordId) ||
           !ParseNonzeroLayoutGuid(recordId,parsedId,&recordId)){
            result.failure=recordFailure;
            return result;
        }
        const int deskIndex=GetDesktopIndexByGuid(transition.destination);
        const UnixSeconds nowUtc=UtcNowSeconds();
        if(deskIndex<0 || nowUtc<=0 ||
           transition.reservationToken.operationId==0){
            result.failure=PopupSaveFailure::DesktopUnavailable;
            return result;
        }

        const LayoutWin* before=nullptr;
        for(const LayoutWin& current : g_autoRecords)
            if(current.recordId==recordId){ before=&current; break; }
        if(before && before->app!=app){
            result.failure=PopupSaveFailure::WrongProfile;
            return result;
        }
        auto acceptedAfterEntry=g_acceptedFreshByRuntime.find(runtimeKey);
        const bool hasAcceptedAfterEntry=
            acceptedAfterEntry!=g_acceptedFreshByRuntime.end() &&
            acceptedAfterEntry->second.record.app==app;
        const PickerFreshRecordSource freshSource=
            SelectPickerFreshRecordSource(
                identity,transition.identityGeneration,
                reserved->second.hasAcceptedFreshRecord,
                reserved->second.identity,
                reserved->second.identityGeneration,
                hasAcceptedAfterEntry,
                hasAcceptedAfterEntry
                    ? acceptedAfterEntry->second.identity
                    : WindowIdentityKey{},
                hasAcceptedAfterEntry
                    ? acceptedAfterEntry->second.identityGeneration
                    : 0);
        const bool fresh=freshSource!=PickerFreshRecordSource::None;

        LayoutWin desired;
        if(freshSource==PickerFreshRecordSource::AcceptedAfterEntry){
            desired=acceptedAfterEntry->second.record;
            desired.provisional=false;
        } else if(freshSource==PickerFreshRecordSource::Reserved){
            desired=reserved->second.acceptedFreshRecord;
            desired.provisional=false;
        } else if(before){
            desired=*before;
        } else if(reserved->second.hasProvisionalOriginRecord){
            desired=reserved->second.provisionalOriginRecord;
            desired.provisional=true;
        } else if(hasTerminalProvisional){
            desired=terminalProvisional;
            desired.provisional=true;
        } else {
            result.failure=!transition.capturedTitleComplete
                ? PopupSaveFailure::IncompleteTitle
                : PopupSaveFailure::FingerprintUnavailable;
            return result;
        }
        if(!before && !fresh && desired.activeTitle.empty()){
            result.failure=PopupSaveFailure::IncompleteTitle;
            return result;
        }
        desired.recordId=recordId;
        desired.app=app;
        desired.desktop=transition.destination;
        desired.deskIndex=deskIndex;
        MarkSeen(desired,nowUtc);

        RecordDelta delta;
        delta.kind=RecordDeltaKind::ExplicitUpsert;
        delta.record=desired;
        delta.baseRevision=g_autoRevision;
        delta.baseRecordPresent=before!=nullptr;
        if(before) delta.baseRecord=*before;
        delta.changedUtc=nowUtc;
        delta.causalGeneration=transition.reservationToken.operationId;
        std::vector<LayoutWin> stagedRecords;
        std::map<std::string,RecordDelta> stagedDeltas;
        std::map<std::string,DeferredRecordConflict> stagedConflicts;
        if(StageRecordDeltaMutation(
                g_autoRecords,g_dirtyRecordDeltas,
                g_deferredRecordConflicts,delta,true,stagedRecords,
                stagedDeltas,stagedConflicts)!=
           RecordDeltaStageResult::Accepted){
            result.failure=PopupSaveFailure::StageRejected;
            return result;
        }

        std::map<std::string,ValidatedRecordTouch> stagedTouches=
            g_validatedTouches;
        ValidatedRecordTouch touch;
        touch.recordId=recordId;
        touch.lastSeenUtc=desired.lastSeenUtc;
        touch.causalGeneration=transition.reservationToken.operationId;
        stagedTouches[recordId]=touch;
        std::map<std::string,RuntimeRecordBinding> stagedBindings=
            g_recordByRuntime;
        RuntimeRecordBinding binding;
        binding.app=app;
        binding.recordId=recordId;
        binding.identity=identity;
        binding.causalGeneration=transition.reservationToken.operationId;
        stagedBindings[runtimeKey]=binding;
        std::map<std::string,std::string> stagedPending=
            g_pendingRecordByRuntime;
        std::map<std::string,std::string> stagedProvisional=
            g_provisionalRecordByRuntime;
        stagedPending.erase(runtimeKey);
        stagedProvisional.erase(runtimeKey);

        if(RecaptureGenericWindowIdentity(identity)!=
           WindowIdentityRecapture::Match){
            result.failure=PopupSaveFailure::IdentityChanged;
            return result;
        }
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
        // The exact record/delta/binding publication is already durable in
        // memory here.  Mark the accepted-plan claim before the flush so a
        // write failure remains queued ownership and cannot be mistaken for
        // a pre-Save cancellation.  This uses the already captured runtime
        // key and record ID, so the terminal outcome does not depend on a
        // later allocating identity lookup.
        MarkPickerOperationClaimsPublished(runtimeKey,recordId);
        if(!FlushAutoLayout(true)){
            result.failure=PopupSaveFailure::FlushFailed;
            return result;
        }
        result.status=PopupSaveStatus::Saved;
        result.failure=PopupSaveFailure::None;
        return result;
            });
    } catch(...) {
        result.status=PopupSaveStatus::Failed;
        if(result.failure==PopupSaveFailure::None)
            result.failure=PopupSaveFailure::Unexpected;
        return result;
    }
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

static bool ReconcileFastHasUsableAcceptedFresh(
        const ReconcileResult& result,size_t fastIndex) noexcept;

static std::set<std::string> UpdateBoundRecords(
        const std::string& app,const AppFastSnapshot& snapshot,
        const ReconcileResult& result,UnixSeconds nowUtc){
    std::set<std::string> reserved;
    for(size_t index=0;
        index<snapshot.windows.size() && index<result.live.size();++index){
        const FastWin& fast=snapshot.windows[index];
        const std::string runtime=RuntimeKey(fast);
        auto pickerReservation=g_reservedAutoIdentities.find(runtime);
        if(pickerReservation!=g_reservedAutoIdentities.end() &&
           pickerReservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(pickerReservation->second.identity,
                        IdentityOf(fast))){
            if(!pickerReservation->second.recordId.empty())
                reserved.insert(pickerReservation->second.recordId);
            continue;
        }
        auto binding=g_recordByRuntime.find(runtime);
        if(binding==g_recordByRuntime.end() || binding->second.app!=app ||
           !SameIdentity(binding->second.identity,IdentityOf(fast))) continue;
        const LayoutWin* existing=FindAutoRecord(binding->second.recordId);
        if(!existing){ g_recordByRuntime.erase(binding); continue; }
        const std::string recordId=existing->recordId;
        const ReconcileFreshness rowFreshness=
            PickerRowUsesFreshFingerprint(
                result.freshness==ReconcileFreshness::Fresh,
                ReconcileFastHasUsableAcceptedFresh(result,index))
                ? ReconcileFreshness::Fresh
                : ReconcileFreshness::CachedStale;
        if(!CommitBoundRecordRefresh(
                *existing,fast,result.live[index],rowFreshness,
                nowUtc,runtime,
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

static bool ReconcileFastHasUsableAcceptedFresh(
        const ReconcileResult& result,size_t fastIndex) noexcept {
    if(result.status!=ReconcileResultStatus::Completed ||
       result.workMode!=ReconcileWorkMode::PrepareLiveOnly ||
       !result.buildLiveFromInputs || result.app.empty() ||
       result.identityGeneration==0 ||
       fastIndex>=result.fastWindows.size() ||
       fastIndex>=result.live.size()) return false;
    const WindowIdentityKey identity=IdentityOf(result.fastWindows[fastIndex]);
    const WinFp* session=ReconcileSessionForFast(result,fastIndex);
    const LayoutWin& live=result.live[fastIndex];
    const bool associationMatches=session && live.tabCount>=0 &&
        live.activeDomain==session->activeDomain &&
        live.tabCount==session->tabCount && live.counts==session->counts &&
        (session->activeTitle.empty() ||
         live.activeTitle==session->activeTitle);
    return PickerAcceptedFreshRowUsable(
        result.freshness==ReconcileFreshness::Fresh,
        associationMatches,identity,live.app==result.app,
        live.activeTitle,live.counts);
}

static bool RememberAcceptedFreshRuntimeRecords(
        const ReconcileResult& result) noexcept {
    if(result.app.empty() || result.identityGeneration==0 ||
       result.fastWindows.size()!=result.live.size() ||
       result.fastWindows.size()!=result.sessionIndexByFast.size())
        return false;
    try {
        std::map<std::string,AcceptedFreshRuntime> staged=
            g_acceptedFreshByRuntime;
        for(auto it=staged.begin();it!=staged.end();){
            if(it->second.record.app==result.app) it=staged.erase(it);
            else ++it;
        }
        for(size_t index=0;index<result.fastWindows.size();++index){
            if(!ReconcileFastHasUsableAcceptedFresh(result,index)) continue;
            const WindowIdentityKey identity=
                IdentityOf(result.fastWindows[index]);
            AcceptedFreshRuntime accepted;
            accepted.identity=identity;
            accepted.identityGeneration=result.identityGeneration;
            accepted.record=result.live[index];
            accepted.record.app=result.app;
            staged[RuntimeKey(identity)]=std::move(accepted);
        }
        g_acceptedFreshByRuntime.swap(staged);
        return true;
    } catch(...) { return false; }
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
            if(ConcreteDesktopExists(fast.desktop,desktops,DeskGuid))
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

static void CancelAutoOperation(uint64_t operationId,bool rearm) noexcept;
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

static void SchedulePickerTabSearchRetry(
        const std::string* freedApp=nullptr) noexcept {
    if(freedApp)
        NotePickerTabSearchRouteFreed(
            g_pickerTabSearchCache,*freedApp);
    if(!g_main || !PickerTabSearchRetryPostNeeded(
            g_pickerTabSearchCache,g_picker.modelGeneration,
            g_picker.searchText)) return;
    if(PostMessageW(g_main,WM_PICKER_SEARCH_RETRY,0,0)){
        KillTimer(g_main,TIMER_PICKER_SEARCH_RETRY);
        MarkPickerTabSearchRetryPosted(
            g_pickerTabSearchCache,g_picker.modelGeneration,
            g_picker.searchText);
        return;
    }
    if(MarkPickerTabSearchRetryDeliveryFailed(
            g_pickerTabSearchCache,g_picker.modelGeneration,
            g_picker.searchText))
        SetTimer(g_main,TIMER_PICKER_SEARCH_RETRY,1,nullptr);
}

static uint64_t RequestSessionWork(AsyncOperationOwner owner,uint64_t operationId,
        const AppProfile& profile,const AppFastSnapshot& snapshot,
        SessionPurpose purpose,
        PickerTabSearchRetryTrigger* searchRetryTrigger=nullptr){
    if(searchRetryTrigger)
        *searchRetryTrigger=PickerTabSearchRetryTrigger::Immediate;
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
    const AsyncRouteAdmission admission=
        g_sessionRouteGate.submit(gated,now,retired);
    if(admission!=AsyncRouteAdmission::Accepted){
        if(searchRetryTrigger){
            if(admission==AsyncRouteAdmission::RejectedProtected)
                *searchRetryTrigger=
                    PickerTabSearchRetryTrigger::ExactAppRoute;
            else if(admission==AsyncRouteAdmission::RejectedCapacity)
                *searchRetryTrigger=PickerTabSearchRetryTrigger::AnyRoute;
        }
        return 0;
    }
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
static bool QueueAutoMove(AutoRestoreOperation& operation,
                          const ReconcileResult& result,
                          const RestoreRequest& restore);
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

static void CancelAutoOperation(uint64_t operationId,bool rearm) noexcept {
    try { g_reconcileDeadlines.cancel(operationId); } catch(...) {}
    auto found=g_pendingAutoOperations.find(operationId);
    if(found==g_pendingAutoOperations.end()) return;
    if(found->second.cancellationPending){
        RefreshMoveCancellationPending();
        try { ScheduleMoveCancellationRetry(); } catch(...) {}
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
    if(state!=g_lifecycleByApp.end()){
        try {
            LcCancelRestore(state->second,lifecycleGeneration,
                            MonotonicNowMs(),rearm);
        } catch(...) {}
    }
    found->second.reconcile.reset();
    found->second.reconcileFast.clear();
    try {
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::AutoReconcile,operationId);
    } catch(...) {}
    for(size_t index=0;index<jobCount;++index)
        try { CancelMoveJobOrDefer(jobIds[index]); } catch(...) {}
    found=g_pendingAutoOperations.find(operationId);
    if(found!=g_pendingAutoOperations.end() && found->second.liveJobIds.empty())
        g_pendingAutoOperations.erase(found);
}

static_assert(noexcept(CancelAutoOperation(uint64_t{},true)),
    "automatic cancellation must not escape WndProc");

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

struct TargetMoveIssueResult {
    HRESULT result=E_FAIL;
    bool invoked=false;
    WindowIdentityRecapture identity=
        WindowIdentityRecapture::Indeterminate;
    TargetDesktopRoute desktopRoute=
        TargetDesktopRoute::Indeterminate;
    TargetMobilityDecision mobility;
};

static void EmitPickerTraceHResult(
    PickerTraceApiKind api,uint64_t generation,uint64_t effectSerial,
    HRESULT result,bool invoked,HWND hwnd=nullptr,
    const GUID& requested=GUID{},const GUID& actual=GUID{}) noexcept;

static TargetMoveIssueResult IssueGuardedTargetMove(
        const WindowIdentityKey& expected,
        TargetDesktopRoute observedRoute,
        const GUID& destinationGuid,
        PickerTraceDesktopLookupUse lookupUse,
        uint64_t generation,uint64_t effectSerial) noexcept {
    TargetMoveIssueResult issued;
    issued.desktopRoute=observedRoute;
    const HWND hwnd=reinterpret_cast<HWND>(expected.hwnd);
    if(!hwnd || GuidIsZero(destinationGuid) || !g_vdmDoc ||
       !g_avc || !g_vdmi){
        issued.result=E_INVALIDARG;
        return issued;
    }

    std::vector<DeskRec> desktops;
    std::string desktopError;
    if(!CurrentDesktops(desktops,&desktopError)){
        issued.result=E_FAIL;
        return issued;
    }
    const bool destinationExists=ConcreteDesktopExists(
        destinationGuid,desktops,DeskGuid);
    if(!destinationExists){
        issued.result=E_INVALIDARG;
        return issued;
    }

    issued.identity=RecaptureGenericWindowIdentity(expected);
    if(issued.identity!=WindowIdentityRecapture::Match){
        issued.result=issued.identity==WindowIdentityRecapture::Lost
            ? E_ABORT : E_PENDING;
        return issued;
    }

    GUID sourceDesktop{};
    HRESULT sourceResult=E_FAIL;
    try {
        sourceResult=g_vdmDoc->GetWindowDesktopId(
            hwnd,&sourceDesktop);
    } catch(...) {
        sourceResult=E_FAIL;
    }
    EmitPickerTraceHResult(
        PickerTraceApiKind::GetWindowDesktopIdTarget,
        generation,effectSerial,sourceResult,true,hwnd,
        GUID{},sourceDesktop);
    BOOL onCurrentDesktop=FALSE;
    HRESULT membershipResult=E_FAIL;
    try {
        membershipResult=
            g_vdmDoc->IsWindowOnCurrentVirtualDesktop(
                hwnd,&onCurrentDesktop);
    } catch(...) {
        membershipResult=E_FAIL;
    }
    const bool sourceExists=ConcreteDesktopExists(
        sourceDesktop,desktops,DeskGuid);
    const TargetDesktopRoute freshRoute=DecideTargetDesktopRoute(
        sourceResult,!GuidIsZero(sourceDesktop),sourceExists,
        membershipResult,onCurrentDesktop!=FALSE);
    issued.desktopRoute=
        observedRoute==TargetDesktopRoute::GloballyVisible
            ? TargetDesktopRoute::GloballyVisible : freshRoute;
    const bool sameSource=sourceExists &&
        GuidEq(sourceDesktop,destinationGuid);
    if(!sourceExists || sameSource){
        issued.result=E_ACCESSDENIED;
        return issued;
    }

    IApplicationView* rawView=nullptr;
    HRESULT viewResult=E_FAIL;
    try {
        viewResult=g_avc->GetViewForHwnd(hwnd,&rawView);
    } catch(...) {
        viewResult=E_FAIL;
    }
    EmitPickerTraceHResult(
        PickerTraceApiKind::GetViewForHwnd,
        generation,effectSerial,viewResult,true,hwnd);
    ScopedComPtr<IApplicationView> view(rawView);
    if(FAILED(viewResult) || !view){
        issued.result=FAILED(viewResult) ? viewResult : E_FAIL;
        return issued;
    }

    TargetMobilityProbeFacts probeFacts;
    issued.mobility=QueryTargetWindowMobility(
        expected,issued.desktopRoute,view.get(),probeFacts);
    issued.identity=probeFacts.identity;
    if(issued.identity!=WindowIdentityRecapture::Match){
        issued.result=issued.identity==WindowIdentityRecapture::Lost
            ? E_ABORT : E_PENDING;
        return issued;
    }

    PickerTraceDesktopLookupContext lookupContext;
    lookupContext.trace=&g_pickerTrace;
    lookupContext.use=lookupUse;
    lookupContext.generation=generation;
    lookupContext.effectSerial=effectSerial;
    lookupContext.requested=destinationGuid;
    ScopedComPtr<IVirtualDesktop> destination=
        GetDesktopByGuid(destinationGuid,&lookupContext);
    if(!destination){
        issued.result=E_INVALIDARG;
        return issued;
    }

    issued.identity=RecaptureGenericWindowIdentity(expected);
    const PickerTraceApiKind selectedApi=
        expected.pid==GetCurrentProcessId()
            ? PickerTraceApiKind::MoveWindowToDesktop
            : PickerTraceApiKind::MoveViewToDesktop;
    issued.result=ExecutePhysicalTargetMoveDecision(
        issued.identity,issued.desktopRoute,issued.mobility,
        sourceExists,destinationExists,sameSource,issued.invoked,
        [&]()->HRESULT {
            if(expected.pid==GetCurrentProcessId())
                return g_vdmDoc->MoveWindowToDesktop(
                    hwnd,destinationGuid);
            return g_vdmi->MoveViewToDesktop(
                view.get(),destination.get());
        });
    EmitPickerTraceHResult(
        selectedApi,generation,effectSerial,issued.result,
        issued.invoked,hwnd,destinationGuid);
    return issued;
}

static HRESULT IssuePickerPopupMove(
        HWND popup,const GUID& destinationGuid,bool& invoked,
        PickerTraceDesktopLookupUse lookupUse,
        uint64_t generation,uint64_t effectSerial) noexcept {
    invoked=false;
    if(!popup || popup!=g_main || GuidIsZero(destinationGuid) ||
       !g_vdmDoc)
        return E_INVALIDARG;
    PickerTraceDesktopLookupContext lookup;
    lookup.trace=&g_pickerTrace;
    lookup.use=lookupUse;
    lookup.generation=generation;
    lookup.effectSerial=effectSerial;
    lookup.requested=destinationGuid;
    if(GetDesktopIndexByGuid(destinationGuid,&lookup)<0)
        return E_INVALIDARG;
    HRESULT result=E_FAIL;
    try {
        invoked=true;
        result=g_vdmDoc->MoveWindowToDesktop(
            popup,destinationGuid);
    } catch(...) {
        result=E_FAIL;
    }
    EmitPickerTraceHResult(
        PickerTraceApiKind::MoveWindowToDesktop,
        generation,effectSerial,result,invoked,popup,
        destinationGuid);
    return result;
}

static HRESULT IssueWindowMove(
        const MoveRuntimeBinding& binding,
        WindowIdentityRecapture& identity){
    TargetMoveIssueResult issued=IssueGuardedTargetMove(
        IdentityOf(binding.window),
        TargetDesktopRoute::Indeterminate,
        binding.destination,
        PickerTraceDesktopLookupUse::MoveEntryDestination,
        0,0);
    identity=issued.identity;
    return issued.result;
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

static bool CancelNextMoveAfterArmFailure() noexcept {
    const MoveJob* front=g_moveQueue.front();
    if(!front){ RefreshMoveCancellationPending(); return true; }
    const uint64_t jobId=front->token.jobId;
    auto runtime=g_moveRuntime.find(jobId);
    if(runtime!=g_moveRuntime.end() && !runtime->second.cancelRequested)
        return false;
    bool cancellationPublished=false;
    try { cancellationPublished=CancelMoveJobOrDefer(jobId); }
    catch(...) { cancellationPublished=false; }
    if(!cancellationPublished){
        RefreshMoveCancellationPending();
        return false;
    }
    RefreshMoveCancellationPending();
    return g_moveQueue.empty();
}

static bool ArmMoveTimer() noexcept {
    if(g_moveQueue.empty()) return true;
    try {
        if(g_main && SetTimer(g_main,TIMER_MOVE_VERIFY,
                MOVE_VERIFY_INTERVAL_MS,nullptr)!=0) return true;
    } catch(...) {}

    // No timer means no queued job may Issue.  Mark every retained runtime
    // first, cancel at most one synchronously, then rearm/post bounded retry
    // work so allocation failure preserves coherent guarded ownership.
    for(auto& runtime : g_moveRuntime){
        runtime.second.cancelRequested=true;
        runtime.second.timerArmFailureCancellation=true;
    }
    RefreshMoveCancellationPending();
    MoveArmFailureCleanup cleanup=MoveArmFailureCleanup::Unresolved;
    try {
        if(CancelNextMoveAfterArmFailure())
            cleanup=MoveArmFailureCleanup::Completed;
    } catch(...) {}
    if(cleanup==MoveArmFailureCleanup::Unresolved){
        try {
            if(ScheduleMoveCancellationRetry())
                cleanup=MoveArmFailureCleanup::Rearmed;
        } catch(...) {}
    }
    try {
        if(cleanup==MoveArmFailureCleanup::Completed)
            ReportStorageError(L"Window-move verification could not be started; queued moves were cancelled safely.");
        else if(cleanup==MoveArmFailureCleanup::Rearmed)
            ReportStorageError(L"Window-move verification could not be started; queued moves are being cancelled safely.");
        else
            ReportStorageError(L"Window-move verification could not be started; queued moves remain protected and cancellation will retry on the next move request.");
    } catch(...) {}
    return false;
}

static_assert(noexcept(ArmMoveTimer()),
    "move-timer arm failure recovery must not escape WndProc");

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
    if(ShouldCancelMoveBeforeIssuedReadback(
            runtime->second.cancelRequested,
            runtime->second.retireAfterVerify,
            g_moveQueue.nextAction()==MoveAction::Verify)){
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
        const MoveToken& token,const std::string& runtimeKey,
        PickerTraceReservationReleaseFacts* facts=nullptr){
    const auto tokenStillReserved=[&]() noexcept {
        for(const auto& reservation : g_reservedAutoIdentities)
            if(SameMoveToken(reservation.second.token,token)) return true;
        return false;
    };
    PickerTraceReservationExceptionStage currentStage=
        PickerTraceReservationExceptionStage::FirstDecision;
    try {
        currentStage=PickerTraceReservationExceptionStage::FirstDecision;
        auto reserved=g_reservedAutoIdentities.find(runtimeKey);
        PickerTerminalGuardReleaseAction release=
            DecidePickerTerminalGuardRelease(
                reserved!=g_reservedAutoIdentities.end(),
                reserved!=g_reservedAutoIdentities.end() &&
                    SameMoveToken(reserved->second.token,token),
                tokenStillReserved());
        if(facts){
            facts->firstAction=release;
            facts->firstActionAvailable=true;
        }
        if(release==PickerTerminalGuardReleaseAction::ResolvedAbsent)
            return true;
        if(release!=PickerTerminalGuardReleaseAction::ConsumeExact)
            return false;
        const bool lastReservation=g_reservedAutoIdentities.size()==1;
        currentStage=
            PickerTraceReservationExceptionStage::CheckpointCallback;
        g_checkpointController.acknowledgeReservationBeforeRelease(
            true,lastReservation,g_autoFix && !g_degraded,g_autoLoaded,
            [](CheckpointReason reason){ return ExecuteCheckpoint(reason); });
        // The checkpoint callback must observe the guard.  Re-find afterward
        // so cleanup stays safe even if injected code altered the map.
        currentStage=PickerTraceReservationExceptionStage::Refind;
        reserved=g_reservedAutoIdentities.find(runtimeKey);
        currentStage=PickerTraceReservationExceptionStage::SecondDecision;
        if(facts) facts->retried=true;
        release=DecidePickerTerminalGuardRelease(
            reserved!=g_reservedAutoIdentities.end(),
            reserved!=g_reservedAutoIdentities.end() &&
                SameMoveToken(reserved->second.token,token),
            tokenStillReserved());
        if(facts){
            facts->retryAction=release;
            facts->retryActionAvailable=true;
        }
        if(release==PickerTerminalGuardReleaseAction::ResolvedAbsent)
            return true;
        if(release==PickerTerminalGuardReleaseAction::ConsumeExact){
            currentStage=PickerTraceReservationExceptionStage::Erase;
            g_reservedAutoIdentities.erase(reserved);
            return true;
        }
        return false;
    } catch(...) {
        if(facts){
            facts->exceptionStage=currentStage;
            facts->exceptionStageAvailable=true;
        }
        throw;
    }
}

static bool ConsumeCheckpointAndReleaseMoveReservation(
        const MoveResult& result){
    return ConsumeCheckpointAndReleaseMoveReservation(
        result.token,result.runtimeKey);
}

enum class PickerClaimRearmAttempt {
    None, MoveQueued, RetryOperation
};

static PickerClaimRearmAttempt RearmUnpublishedPickerClaimRestores(
        AutoRestoreOperation& operation) noexcept {
    if(!operation.reconcile ||
       operation.pickerClaimedRecordByRuntime.empty())
        return PickerClaimRearmAttempt::None;
    bool rearmed=false;
    try {
        const ReconcileResult& result=*operation.reconcile;
        for(auto claim=operation.pickerClaimedRecordByRuntime.begin();
            claim!=operation.pickerClaimedRecordByRuntime.end();){
            const bool recordCurrentlyExists=
                FindAutoRecord(claim->second.recordId)!=nullptr;
            const RestoreRequest* selected=nullptr;
            bool ambiguous=false;
            for(const RestoreRequest& restore : result.plan.restores){
                if(restore.savedIndex>=result.saved.size() ||
                   restore.liveIndex>=operation.reconcileFast.size() ||
                   operation.successfulLive.succeeded(restore.liveIndex) ||
                   result.saved[restore.savedIndex].recordId!=
                       claim->second.recordId ||
                   RuntimeKey(operation.reconcileFast[restore.liveIndex])!=
                       claim->first) continue;
                if(selected){
                    selected=nullptr;
                    ambiguous=true;
                    break;
                }
                selected=&restore;
            }
            if(ambiguous) return PickerClaimRearmAttempt::RetryOperation;
            const PickerOperationLifetimeClaimReleaseAction action=
                DecidePickerOperationLifetimeClaimRelease(
                    claim->second,recordCurrentlyExists,
                    selected!=nullptr && !ambiguous);
            if(action==
                    PickerOperationLifetimeClaimReleaseAction::ProtectPublished){
                ++claim;
                continue;
            }
            if(action==
                    PickerOperationLifetimeClaimReleaseAction::RearmOperation){
                ObservePickerOperationClaimQueuePublication(
                    operation.pickerClaimRearm,false);
                return PickerClaimRearmAttempt::RetryOperation;
            }
            if(action!=
                    PickerOperationLifetimeClaimReleaseAction::RearmRestore){
                claim=operation.pickerClaimedRecordByRuntime.erase(claim);
                continue;
            }
            const bool queuePublished=selected &&
                QueueAutoMove(operation,result,*selected);
            if(ObservePickerOperationClaimQueuePublication(
                    operation.pickerClaimRearm,queuePublished)==
                    PickerOperationClaimQueueAction::
                        RetainClaimAndRearmOperation)
                return PickerClaimRearmAttempt::RetryOperation;
            claim=operation.pickerClaimedRecordByRuntime.erase(claim);
            rearmed=true;
        }
    } catch(...) {
        ObservePickerOperationClaimQueuePublication(
            operation.pickerClaimRearm,false);
        return PickerClaimRearmAttempt::RetryOperation;
    }
    if(rearmed){
        // Arm failure can synchronously cancel a queued restore.  Publish the
        // retry-wave intent before that reentrant boundary so Cancelled can
        // never complete the accepted operation as Success.
        const uint64_t operationId=operation.operationId;
        BeginPickerOperationClaimMoveArm(operation.pickerClaimRearm);
        const bool armed=ArmMoveTimer();
        auto current=g_pendingAutoOperations.find(operationId);
        const PickerRearmedMoveArmAction armAction=
            current!=g_pendingAutoOperations.end()
                ? CompletePickerOperationClaimMoveArm(
                    current->second.pickerClaimRearm,armed)
                : DecidePickerRearmedMoveArmResult(armed);
        if(armAction==
                PickerRearmedMoveArmAction::RearmOperation){
            CancelAutoOperation(operationId,true);
            return PickerClaimRearmAttempt::RetryOperation;
        }
    }
    return rearmed ? PickerClaimRearmAttempt::MoveQueued
                   : PickerClaimRearmAttempt::None;
}

static void FinishAutoOperation(uint64_t operationId){
    auto found=g_pendingAutoOperations.find(operationId);
    if(found!=g_pendingAutoOperations.end() &&
       found->second.cancellationPending){
        if(found->second.liveJobIds.empty())
            g_pendingAutoOperations.erase(found);
        return;
    }
    if(found==g_pendingAutoOperations.end()) return;
    if(PickerOperationClaimRearmRequiresRetry(
            found->second.pickerClaimRearm)){
        CancelAutoOperation(operationId,true);
        return;
    }
    if(found->second.outstanding!=0) return;
    AutoRestoreOperation& operation=found->second;
    for(const auto& claim : operation.pickerClaimedRecordByRuntime){
        auto reservation=g_reservedAutoIdentities.find(claim.first);
        if(reservation!=g_reservedAutoIdentities.end() &&
           reservation->second.token.owner==MoveOwner::Picker)
            return;
    }
    const PickerClaimRearmAttempt pickerRearm=
        RearmUnpublishedPickerClaimRestores(operation);
    if(pickerRearm==PickerClaimRearmAttempt::RetryOperation){
        CancelAutoOperation(operationId,true);
        return;
    }
    if(pickerRearm==PickerClaimRearmAttempt::MoveQueued) return;
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
    std::set<std::string> pickerProtectedRecordIds;
    std::set<std::string> pickerProtectedClaimRuntimes;
    for(const auto& claim : operation.pickerClaimedRecordByRuntime){
        const bool recordCurrentlyExists=
            FindAutoRecord(claim.second.recordId)!=nullptr;
        if(PickerOperationLifetimeClaimMustProtect(
                claim.second,recordCurrentlyExists)){
            pickerProtectedRecordIds.insert(claim.second.recordId);
            pickerProtectedClaimRuntimes.insert(claim.first);
        }
    }
    for(const auto& reservation : g_reservedAutoIdentities)
        if(reservation.second.token.owner==MoveOwner::Picker &&
           reservation.second.app==operation.app &&
           !reservation.second.recordId.empty())
            pickerProtectedRecordIds.insert(reservation.second.recordId);
    for(const LayoutMatch& match : result.plan.matches){
        if(match.savedIndex>=result.saved.size() ||
           match.liveIndex>=operation.reconcileFast.size()) continue;
        const FastWin& fast=operation.reconcileFast[match.liveIndex];
        auto reservation=g_reservedAutoIdentities.find(RuntimeKey(fast));
        if(reservation!=g_reservedAutoIdentities.end() &&
           reservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(reservation->second.identity,IdentityOf(fast)) &&
           !result.saved[match.savedIndex].recordId.empty())
            pickerProtectedRecordIds.insert(
                result.saved[match.savedIndex].recordId);
    }
    for(const NewRecordRequest& created : result.plan.newRecords){
        if(created.liveIndex>=operation.reconcileFast.size() ||
           created.recordId.empty()) continue;
        const FastWin& fast=operation.reconcileFast[created.liveIndex];
        auto reservation=g_reservedAutoIdentities.find(RuntimeKey(fast));
        if(reservation!=g_reservedAutoIdentities.end() &&
           reservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(reservation->second.identity,IdentityOf(fast)))
            pickerProtectedRecordIds.insert(created.recordId);
    }
    for(const LayoutWin& record : result.saved)
        if(record.app==operation.app) baseById[record.recordId]=record;
    for(const LayoutWin& record : committed)
        if(record.app==operation.app) committedById[record.recordId]=record;

    for(const auto& entry : baseById){
        if(pickerProtectedRecordIds.count(entry.first)!=0) continue;
        if(committedById.count(entry.first)!=0) continue;
        const LayoutWin* current=FindAutoRecord(entry.first);
        if(current && SameRecordForDelta(*current,entry.second))
            EraseAutoRecord(*current,RecordDeltaKind::ExpireDelete,
                            nowUtc,operation.contentGeneration);
    }
    for(const auto& entry : committedById){
        if(pickerProtectedRecordIds.count(entry.first)!=0) continue;
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
        if(pickerProtectedClaimRuntimes.count(runtime)!=0 &&
           PickerOperationLifetimeClaimMatches(
               operation.pickerClaimedRecordByRuntime,IdentityOf(fast),
               result.saved[match.savedIndex].recordId)) continue;
        auto pickerReservation=g_reservedAutoIdentities.find(runtime);
        if(pickerReservation!=g_reservedAutoIdentities.end() &&
           pickerReservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(pickerReservation->second.identity,
                        IdentityOf(fast))) continue;
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
        if(pickerProtectedClaimRuntimes.count(RuntimeKey(fast))!=0 &&
           PickerOperationLifetimeClaimMatches(
               operation.pickerClaimedRecordByRuntime,IdentityOf(fast),
               created.recordId)) continue;
        auto pickerReservation=g_reservedAutoIdentities.find(
            RuntimeKey(fast));
        if(pickerReservation!=g_reservedAutoIdentities.end() &&
           pickerReservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(pickerReservation->second.identity,
                        IdentityOf(fast))) continue;
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

static void FinishAutoOperationsClaimedByPicker(
        const std::string& runtimeKey) noexcept {
    if(runtimeKey.empty()) return;
    try {
        bool haveCursor=false;
        uint64_t cursor=0;
        for(;;){
            auto operation=PickerOperationCursorNext(
                g_pendingAutoOperations,haveCursor,cursor);
            if(operation==g_pendingAutoOperations.end()) break;
            const uint64_t operationId=operation->first;
            const bool ready=operation->second.outstanding==0 &&
                operation->second.pickerClaimedRecordByRuntime.count(
                    runtimeKey)!=0;
            cursor=operationId;
            haveCursor=true;
            if(ready) FinishAutoOperation(operationId);
        }
    } catch(...) {}
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
        if(result.terminal==MoveTerminal::Cancelled && hadRuntime &&
           DecidePickerAutoCancelledMoveOwnerAction(
               runtime.timerArmFailureCancellation)==
               PickerAutoCancelledMoveOwnerAction::RearmOperation){
            CancelAutoOperation(result.token.operationId,true);
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
    try {
        ConfigureWorkerLiveBuild(request,ReconcileWorkMode::PrepareLiveOnly,
                                 *profile,current->second,result.windows,
                                 g_autoDesktops);
        operation->second.currentDesktops=request.desktops;
    }
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
        SchedulePickerTabSearchRetry(&expected.app);
        return;
    }
    g_sessionRoutes.erase(route);
    if(result->app!=expected.app || result->purpose!=expected.purpose ||
       result->identityGeneration!=expected.identityGeneration){
        RetireAsyncSessionOperation(expected,AsyncRetirementReason::Failed);
        SchedulePickerTabSearchRetry(&expected.app);
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
    SchedulePickerTabSearchRetry(&expected.app);
}

static bool QueueAutoMove(AutoRestoreOperation& operation,
                          const ReconcileResult& result,
                          const RestoreRequest& restore){
    if(restore.savedIndex>=result.saved.size() ||
       restore.liveIndex>=operation.reconcileFast.size()) return false;
    const FastWin& fast=operation.reconcileFast[restore.liveIndex];
    const std::string runtimeKey=RuntimeKey(fast);
    const LayoutWin& saved=result.saved[restore.savedIndex];
    if(!SavedRestoreDestinationAvailable(
            saved,restore.destination,operation.currentDesktops)) return false;
    if(g_reservedAutoIdentities.count(runtimeKey)) return false;
    g_pendingRecordByRuntime[runtimeKey]=saved.recordId;

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

static void SwapLayoutWinNoThrow(LayoutWin& left,LayoutWin& right) noexcept;

static PickerLatePlanHandoffAction TransferLateAcceptedPlanRecordToPicker(
        AutoRestoreOperation& operation,
        const ReconcileResult& accepted) noexcept {
    try {
        bool found=false;
        bool selectedRecordExists=false;
        std::string selected,runtimeKey;
        MoveToken pickerToken;
        auto acceptExact=[&](const FastWin& candidate,
                             const std::string& candidateRecordId,
                             bool requireExisting)->bool {
            const std::string runtime=RuntimeKey(candidate);
            auto reservation=g_reservedAutoIdentities.find(runtime);
            const bool exactPickerReservation=
                reservation!=g_reservedAutoIdentities.end() &&
                reservation->second.token.owner==MoveOwner::Picker &&
                SameIdentity(reservation->second.identity,
                             IdentityOf(candidate));
            if(!exactPickerReservation) return true;
            const bool sameTransition=
                g_picker.controlledTransition() &&
                SameMoveToken(reservation->second.token,
                              g_picker.transition.reservationToken) &&
                SameIdentity(g_picker.transition.target,
                             IdentityOf(candidate));
            const PickerLatePlanHandoffAction action=
                DecidePickerLatePlanHandoff(
                    true,sameTransition,
                    sameTransition &&
                        g_picker.transition.commitCutoffReached);
            if(action!=PickerLatePlanHandoffAction::TransferBeforeSave)
                return false;
            GUID parsed{};
            std::string canonical;
            const LayoutWin* current=nullptr;
            if(!ParseNonzeroLayoutGuid(
                    candidateRecordId,parsed,&canonical) ||
               (requireExisting &&
                (!(current=FindAutoRecord(canonical)) ||
                 current->app!=operation.app))) return false;
            const PickerAcceptedPlanRecordResult accumulated=
                [&](){
                    const bool previouslyFound=found;
                    const PickerAcceptedPlanRecordResult value=
                AccumulatePickerAcceptedPlanRecord(
                    g_picker.transition.target,operation.app,
                    IdentityOf(candidate),operation.app,canonical,
                    selected,found);
                    if(value==PickerAcceptedPlanRecordResult::Selected){
                        if(previouslyFound &&
                           selectedRecordExists!=requireExisting)
                            return PickerAcceptedPlanRecordResult::Rejected;
                        if(!previouslyFound)
                            selectedRecordExists=requireExisting;
                    }
                    return value;
                }();
            if(accumulated==PickerAcceptedPlanRecordResult::Rejected)
                return false;
            if(runtimeKey.empty()){
                runtimeKey=runtime;
                pickerToken=reservation->second.token;
            } else if(runtimeKey!=runtime ||
                      !SameMoveToken(pickerToken,
                                     reservation->second.token)){
                return false;
            }
            return true;
        };
        for(const LayoutMatch& match : accepted.plan.matches){
            if(match.savedIndex>=accepted.saved.size() ||
               match.liveIndex>=operation.reconcileFast.size()) continue;
            const LayoutWin& saved=accepted.saved[match.savedIndex];
            if(saved.app!=operation.app ||
               !acceptExact(operation.reconcileFast[match.liveIndex],
                            saved.recordId,true))
                return PickerLatePlanHandoffAction::RejectPlan;
        }
        for(const NewRecordRequest& created : accepted.plan.newRecords){
            if(created.liveIndex>=operation.reconcileFast.size() ||
               created.liveIndex>=accepted.live.size()) continue;
            if(accepted.live[created.liveIndex].app!=operation.app ||
               !acceptExact(operation.reconcileFast[created.liveIndex],
                            created.recordId,false))
                return PickerLatePlanHandoffAction::RejectPlan;
        }
        if(!found) return PickerLatePlanHandoffAction::Ignore;

        auto reservation=g_reservedAutoIdentities.find(runtimeKey);
        if(reservation==g_reservedAutoIdentities.end() ||
           !SameMoveToken(reservation->second.token,pickerToken) ||
           !SameMoveToken(g_picker.transition.reservationToken,pickerToken) ||
           g_picker.transition.commitCutoffReached ||
           (!reservation->second.app.empty() &&
            reservation->second.app!=operation.app) ||
           (!g_picker.transition.app.empty() &&
            g_picker.transition.app!=operation.app))
            return PickerLatePlanHandoffAction::RejectPlan;
        std::string reservedRecord=selected;
        std::string transitionRecord=selected;
        std::string reservedApp=operation.app;
        std::string transitionApp=operation.app;
        std::map<std::string,std::string> stagedPending;
        std::map<std::string,PickerOperationLifetimeClaim> stagedClaims;
        if(!StagePickerAcceptedPlanPendingAssociation(
                g_pendingRecordByRuntime,runtimeKey,selected,
                stagedPending) ||
           !StagePickerOperationLifetimeClaim(
                operation.pickerClaimedRecordByRuntime,runtimeKey,
                selected,selectedRecordExists,stagedClaims))
            return PickerLatePlanHandoffAction::RejectPlan;
        LayoutWin acceptedFreshRecord;
        bool hasAcceptedFresh=false;
        auto fresh=g_acceptedFreshByRuntime.find(runtimeKey);
        if(fresh!=g_acceptedFreshByRuntime.end() &&
           fresh->second.record.app==operation.app &&
           PickerFreshRuntimeMatches(
               g_picker.transition.target,operation.identityGeneration,
               fresh->second.identity,
               fresh->second.identityGeneration)){
            acceptedFreshRecord=fresh->second.record;
            hasAcceptedFresh=true;
        }
        LayoutWin transferredProvisional;
        bool hasTransferredProvisional=false;
        if(!selectedRecordExists && !hasAcceptedFresh){
            if(!g_picker.transition.capturedTitleComplete ||
               !BuildPickerCapturedTitleOnlyProvisional(
                   operation.app,g_picker.transition.capturedTitle,
                   transferredProvisional))
                return PickerLatePlanHandoffAction::RejectPlan;
            transferredProvisional.recordId=selected;
            transferredProvisional.desktop=
                g_picker.transition.targetOrigin;
            transferredProvisional.deskIndex=SnapshotDesktopIndex(
                g_picker.transition.targetOrigin);
            MarkSeen(transferredProvisional,UtcNowSeconds());
            hasTransferredProvisional=true;
        }
        if(!CommitPickerAcceptedPlanRecordTransfer(
                [&](){
                    g_restoreBudgets.clearForExplicitRetry(selected);
                    return true;
                },[&]() noexcept {
                    g_pendingRecordByRuntime.swap(stagedPending);
                    operation.pickerClaimedRecordByRuntime.swap(
                        stagedClaims);
                    reservation->second.recordId.swap(reservedRecord);
                    g_picker.transition.pendingRecordId.swap(
                        transitionRecord);
                    reservation->second.app.swap(reservedApp);
                    g_picker.transition.app.swap(transitionApp);
                    reservation->second.identityGeneration=
                        operation.identityGeneration;
                    g_picker.transition.identityGeneration=
                        operation.identityGeneration;
                    if(hasAcceptedFresh){
                        SwapLayoutWinNoThrow(
                            reservation->second.acceptedFreshRecord,
                            acceptedFreshRecord);
                        reservation->second.hasAcceptedFreshRecord=true;
                    }
                    if(hasTransferredProvisional){
                        SwapLayoutWinNoThrow(
                            reservation->second.provisionalOriginRecord,
                            transferredProvisional);
                        reservation->second.hasProvisionalOriginRecord=true;
                    }
                }))
            return PickerLatePlanHandoffAction::RejectPlan;
        try { g_provisionalRecordByRuntime.erase(runtimeKey); }
        catch(...) {}
        return PickerLatePlanHandoffAction::TransferBeforeSave;
    } catch(...) {
        return PickerLatePlanHandoffAction::RejectPlan;
    }
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
           result->fastWindows.size()!=result->live.size() ||
           result->fastWindows.size()!=result->sessionIndexByFast.size()){
            CancelAutoOperation(operationId,true);
            return;
        }
        AppFastSnapshot prepared=current->second;
        prepared.windows=result->fastWindows;
        RememberAcceptedFreshRuntimeRecords(*result);
        std::set<std::string> reserved=UpdateBoundRecords(
            result->app,prepared,*result,result->nowUtc);
        for(const auto& entry : g_reservedAutoIdentities)
            if(entry.second.app==result->app && !entry.second.recordId.empty())
                reserved.insert(entry.second.recordId);

        std::vector<LayoutWin> unboundLive;
        std::vector<FastWin> unboundFast;
        bool omittedUnusableUnboundRow=false;
        try {
            for(size_t index=0;index<result->fastWindows.size();++index){
                const FastWin& fast=result->fastWindows[index];
                auto reservation=g_reservedAutoIdentities.find(
                    RuntimeKey(fast));
                if(reservation!=g_reservedAutoIdentities.end() &&
                   reservation->second.token.owner==MoveOwner::Picker &&
                   SameIdentity(reservation->second.identity,
                                IdentityOf(fast))) continue;
                auto bound=g_recordByRuntime.find(RuntimeKey(fast));
                if(bound!=g_recordByRuntime.end() &&
                   SameIdentity(bound->second.identity,IdentityOf(fast))) continue;
                if(!PickerUnboundRowEligibleForReconcilePlan(
                        result->freshness==ReconcileFreshness::Fresh,
                        ReconcileFastHasUsableAcceptedFresh(
                            *result,index))){
                    omittedUnusableUnboundRow=true;
                    continue;
                }
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
        plan.freshness=PickerUnboundPlanUsesFreshness(
            result->freshness==ReconcileFreshness::Fresh,
            omittedUnusableUnboundRow)
                ? ReconcileFreshness::Fresh
                : ReconcileFreshness::CachedStale;
        try {
            operation->second.reconcileFast=unboundFast;
            operation->second.reconcileFastKnown=true;
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

    if(TransferLateAcceptedPlanRecordToPicker(
            operation->second,*result)==
       PickerLatePlanHandoffAction::RejectPlan){
        CancelAutoOperation(operationId,true);
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
        if(restore.savedIndex>=accepted.saved.size() ||
           restore.liveIndex>=operation->second.reconcileFast.size() ||
           !SavedRestoreDestinationAvailable(
               accepted.saved[restore.savedIndex],restore.destination,
               operation->second.currentDesktops)){
            operation->second.hadFailure=true;
            continue;
        }
        const FastWin& fast=operation->second.reconcileFast[restore.liveIndex];
        auto pickerReservation=g_reservedAutoIdentities.find(
            RuntimeKey(fast));
        if(pickerReservation!=g_reservedAutoIdentities.end() &&
           pickerReservation->second.token.owner==MoveOwner::Picker &&
           SameIdentity(pickerReservation->second.identity,
                        IdentityOf(fast)))
            continue;
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
        const std::vector<DeskRec>& desktops,
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
            window.observed.deskIndex=-1;
            window.desktopValid=
                ConcreteDesktopExists(fast.desktop,desktops,DeskGuid);
            if(window.desktopValid)
                for(const DeskRec& desktop : desktops)
                    if(GuidEq(desktop.guid,fast.desktop)){
                        window.observed.deskIndex=desktop.index;
                        break;
                    }
            window.observed.activeTitle=W2U8(
                StripReconcileTitleSuffix(
                    fast.title,profile.titleSuffixes));
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
    std::vector<DeskRec> desktops;
    std::string desktopError;
    if(!CurrentDesktops(desktops,&desktopError)) return false;
    const uint64_t checkpointGeneration=TakeNonzeroId(g_nextOperationId);
    std::vector<FinalAppObservation> observations;
    std::map<std::string,std::string> stagedProvisional;
    if(!StageFinalObservationsAndProvisionals(
            g_provisionalRecordByRuntime,observations,stagedProvisional,
            [&](std::map<std::string,std::string>& provisionals,
                std::vector<FinalAppObservation>& stagedObservations){
                stagedObservations=BuildFinalObservations(
                    snapshots,desktops,nowUtc,provisionals);
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
        std::set<std::string> claimedRecordIds;
        for(const FinalAppObservation& app : observations)
            for(const FinalWindowObservation& window : app.windows){
                if(!window.boundRecordId.empty())
                    claimedRecordIds.insert(window.boundRecordId);
                if(!window.pendingRecordId.empty())
                    claimedRecordIds.insert(window.pendingRecordId);
            }
        for(const auto& provisional : staged.provisionalRecordByRuntime){
            auto record=std::find_if(staged.records.begin(),staged.records.end(),
                [&](const LayoutWin& candidate){
                    return candidate.recordId==provisional.second;
                });
            if(record==staged.records.end()) continue;
            if(!CanBindFinalProvisional(
                    staged.records,claimedRecordIds,*record,
                    g_pendingRecordByRuntime.count(provisional.first)!=0,
                    stagedBindings.count(provisional.first)!=0,nowUtc)) continue;
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
                    claimedRecordIds.insert(record->recordId);
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
    g_acceptedFreshByRuntime.clear();
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
    std::swap(left.identityGeneration,right.identityGeneration);
    SwapLayoutWinNoThrow(
        left.acceptedFreshRecord,right.acceptedFreshRecord);
    std::swap(left.hasAcceptedFreshRecord,
              right.hasAcceptedFreshRecord);
    SwapLayoutWinNoThrow(
        left.provisionalOriginRecord,right.provisionalOriginRecord);
    std::swap(left.hasProvisionalOriginRecord,
              right.hasProvisionalOriginRecord);
}

static bool PickerCanSupersedeAutoReservation(
        const ReservedAutoIdentity& prior) noexcept {
    if(prior.token.owner!=MoveOwner::AutoReconcile ||
       prior.token.jobId==0) return false;
    auto runtime=g_moveRuntime.find(prior.token.jobId);
    if(runtime==g_moveRuntime.end() || runtime->second.cancelRequested ||
       runtime->second.retireAfterVerify ||
       runtime->second.issueAwaitingVerify) return false;
    const MoveJob* front=g_moveQueue.front();
    if(front && front->token.jobId==prior.token.jobId &&
       g_moveQueue.nextAction()==MoveAction::Verify) return false;
    return true;
}

static bool BeginReservationHandoff(
        const std::string& runtimeKey,
        const ReservedAutoIdentity& replacement,
        ReservationHandoff& handoff){
    try {
        ReservationHandoff next;
        next.runtimeKey=runtimeKey;
        auto prior=g_reservedAutoIdentities.find(runtimeKey);
        if(prior!=g_reservedAutoIdentities.end() &&
           !PickerReservationReplacementAllowed(
               prior->second.token.owner,replacement.token.owner))
            return false;
        if(prior!=g_reservedAutoIdentities.end() &&
           replacement.token.owner==MoveOwner::Picker &&
           !PickerCanSupersedeAutoReservation(prior->second))
            return false;
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
        if(route->second.owner==owner && route->second.operationId==operationId){
            if(owner==AsyncOperationOwner::Search){
                auto operation=g_searchOperations.find(operationId);
                if(operation!=g_searchOperations.end())
                    MarkPickerTabSearchRetryNeeded(
                        g_pickerTabSearchCache,operationId,
                        operation->second.pickerModelGeneration,
                        operation->second.pickerQuery,route->second.app);
            }
            SchedulePickerTabSearchRetry(&route->second.app);
            route=g_sessionRoutes.erase(route);
        } else ++route;
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
            if(!ConcreteDesktopExists(record.desktop,currentDesktops,DeskGuid) ||
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
    MarkPickerTabSearchRetryNeeded(
        g_pickerTabSearchCache,
        route.operationId,
        operation->second.pickerModelGeneration,
        operation->second.pickerQuery,route.app);
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(route.operationId);
        CompletePickerTabSearchAttempt(
            g_pickerTabSearchCache,
            route.operationId,
            operation->second.pickerModelGeneration,
            operation->second.pickerQuery);
        g_searchOperations.erase(operation);
        SchedulePickerTabSearchRetry(&route.app);
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
        SchedulePickerTabSearchRetry(&route.app);
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
        for(const std::string& app : search->second.waitingReconcileApps)
            MarkPickerTabSearchRetryNeeded(
                g_pickerTabSearchCache,operationId,
                search->second.pickerModelGeneration,
                search->second.pickerQuery,app,
                PickerTabSearchRetryTrigger::Immediate);
        CompletePickerTabSearchAttempt(
            g_pickerTabSearchCache,operationId,
            search->second.pickerModelGeneration,
            search->second.pickerQuery);
        g_searchOperations.erase(search);
        SchedulePickerTabSearchRetry();
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
    route.app=found->second.app;
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
    SchedulePickerTabSearchRetry(&route.app);
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
        const std::vector<DeskRec>& currentDesktops,
        bool& provisionalNeedsInsert,bool& reservationReady){
    provisionalNeedsInsert=false;
    reservationReady=false;
    ReservedAutoIdentity reservation;
    reservation.token=token;
    reservation.identity=IdentityOf(fast);
    reservation.app=fast.app;
    reservation.recordId=recordId;
    const bool concreteOrigin=ConcreteDesktopExists(
        fast.desktop,currentDesktops,DeskGuid);
    reservation.originDesktop=concreteOrigin ? fast.desktop : GUID{};
    auto bound=g_recordByRuntime.find(RuntimeKey(fast));
    if(bound!=g_recordByRuntime.end() &&
       SameIdentity(bound->second.identity,IdentityOf(fast))){
        reservation.recordId=bound->second.recordId;
        reservationReady=true;
    } else if(!fast.app.empty() &&
              ConcreteDesktopExists(fast.desktop,currentDesktops,DeskGuid)){
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
                                 operation.currentDesktops,
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
        if(!ConcreteDesktopExists(record.desktop,desktops,DeskGuid) ||
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
    std::vector<std::string> enabledApps;
    enabledApps.reserve(profiles.size());
    for(const AppProfile& profile : profiles) enabledApps.push_back(profile.id);
    const CliRestoreMatchPlan matchPlan=PlanCliCheckpointRestoreMatches(
        manual,loaded.wins,live,enabledApps,UtcNowSeconds());
    if(matchPlan.status==CliRestoreMatchStatus::TooComplex){
        summary="Restore candidate set is too complex.";
        return false;
    }
    if(matchPlan.status!=CliRestoreMatchStatus::Ready){
        summary="Restore matching was ambiguous; no windows were moved.";
        return false;
    }
    const std::vector<LayoutMatch>& matches=matchPlan.matches;

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
        std::vector<DeskRec> desks;
        std::string desktopError;
        if(!CurrentDesktops(desks,&desktopError)){
            fprintf(stderr,"Could not enumerate virtual desktops: %s\n",
                    desktopError.c_str());
            return 1;
        }
        printf("Virtual desktops: %u\n",
               static_cast<unsigned>(desks.size()));
        for(const DeskRec& desk : desks){
            const std::string name=desk.name.empty()
                ? std::string() : "("+W2U8(desk.name)+")";
            printf("  [%d] %s  %s\n",desk.index,
                   W2U8(GuidToString(desk.guid)).c_str(),name.c_str());
        }
        if(cmd==L"status"){
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
                     fresh && BuildCliProfileBatch(requests,prepared);
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
struct WinItem {
    HWND hwnd=nullptr;
    WindowIdentityKey identity={};
    std::string runtimeKey;
    std::wstring title;
    std::wstring titleLC;
    std::wstring search;
    GUID observedDesktop={0};
    GUID baseDesktop={0};
    GUID displayedDesktop={0};
    TargetDesktopRoute desktopRoute=TargetDesktopRoute::Indeterminate;
    TargetMobility mobility=TargetMobility::Indeterminate;
    bool visuallyAssigned=false;
    PickerRowAdmission admission=PickerRowAdmission::DisplayOnly;
};   // search = titleLC (+ all-tab text for browser windows)
struct Tile { GUID guid; std::string guidKey; std::wstring name; std::wstring displayName; int index; std::vector<WinItem> windows; std::vector<size_t> filtered; RECT rc; int scroll=0; };
static std::vector<Tile> g_tiles;
static PickerEffect g_pickerScheduledEffect;
static bool g_pickerEffectScheduled=false;
static uint64_t g_pickerEffectNotBeforeMs=0;
static PickerKickState g_pickerObservationKick;
static bool g_pickerTerminalizationPending=false;
static bool g_pickerPumpActive=false;
static bool g_pickerShutdownDrain=false;
static bool g_pickerDurableKickPending=false;
static PickerTraceTerminalMetadata g_pickerTraceTerminalMetadata;
static PickerTracePendingTerminalDelivery
    g_pickerTracePendingTerminalDelivery;
static uint64_t g_pickerTraceTerminalizationGeneration=0;
static uint64_t g_pickerTraceTerminalizationAttempt=0;
static uint64_t g_pickerTraceDeliveryGeneration=0;
static uint64_t g_pickerTraceDeliverySerial=0;
static uint32_t g_pickerTraceDeliveryAttempt=0;
static HWND g_target=nullptr; static std::wstring g_targetTitle;
static HWND g_settings=nullptr;
static HINSTANCE g_inst=nullptr;
static HFONT g_uiFont=nullptr;
static const UINT WM_TRAY=WM_APP+1;
static NOTIFYICONDATAW g_nid={0};
static const size_t MAX_OWNED_APP_ICONS=7;
static std::vector<HICON> g_ownedIcons;
static FixedIconRetirement<MAX_OWNED_APP_ICONS>
    g_failedOwnedIconReleases;
static bool g_appIconOwnershipReady=false;
static bool g_uiShutdownComplete=false;
static OrderedTeardownGate g_uiTeardown;
struct PickerIconPreloadRef { size_t tile=0; size_t window=0; };
static IconPreloadGate g_pickerIconPreloadGate;
static std::vector<PickerIconPreloadRef> g_pickerIconPreloadQueue;
static bool g_pickerIconPreloadQueueDirty=false;
static bool g_pickerIconPreloadTimerArmed=false;
static int g_pickerIconPreloadPriority=-1;
static int g_dpi=96;
static int S(int v){ return PickerScaleForDpi(v,g_dpi); }

static void CancelPickerIconPreload(HWND window) noexcept {
    if(window) KillTimer(window,TIMER_PICKER_ICON_PRELOAD);
    g_pickerIconPreloadTimerArmed=false;
    g_pickerIconPreloadQueueDirty=false;
    g_pickerIconPreloadPriority=-1;
    g_pickerIconPreloadQueue.clear();
    g_pickerIconPreloadGate.cancel();
}

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
        KillTimer(messageWindow,TIMER_PICKER_TRANSITION);
        KillTimer(messageWindow,TIMER_PICKER_SEARCH_RETRY);
        CancelPickerIconPreload(messageWindow);
        g_flushTimerArmed=false;
        g_flushTimerDueMs=0;
        g_heartbeatTimerArmed=false;
        g_moveCancellationRetry.clear();
        g_pickerEffectScheduled=false;
        g_pickerEffectNotBeforeMs=0;
        g_pickerObservationKick.pending=false;
        g_pickerTerminalizationPending=false;
        g_pickerShutdownDrain=false;
        g_pickerDurableKickPending=false;
        g_pickerTraceTerminalMetadata=PickerTraceTerminalMetadata{};
        ResetPickerTracePendingTerminalDelivery(
            g_pickerTracePendingTerminalDelivery);
        g_pickerTraceTerminalizationGeneration=0;
        g_pickerTraceTerminalizationAttempt=0;
        g_pickerTraceDeliveryGeneration=0;
        g_pickerTraceDeliverySerial=0;
        g_pickerTraceDeliveryAttempt=0;
        StopWorkers(messageWindow);
    });
}
static int TILE_W=240,TILE_H=150,PAD=16,HEADER=44,SEARCH_H=40;
static int FOOTER_H=34,FOOTER_MIN_W=720,FOOTER_LINK_H=22;
static int g_cols=1,g_rows=1;
static HFONT g_fPT=nullptr,g_fPN=nullptr,g_fPI=nullptr,g_fPX=nullptr;   // cached picker fonts (avoid re-create per repaint)
static HBRUSH g_searchBrush=nullptr;
static int g_lastHoverRow=-1;                                          // last tooltip row (avoid redundant TTM churn)
static uint64_t g_lastHoverGeneration=0;
static std::wstring g_pickerTooltipText;
static void InitMetrics(){
    g_uiShutdownComplete=false;
    g_uiTeardown.reset();
    try {
        g_ownedIcons.reserve(MAX_OWNED_APP_ICONS);
        g_appIconOwnershipReady=true;
    } catch(...) { g_appIconOwnershipReady=false; }
    HDC dc=GetDC(nullptr); g_dpi=GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(nullptr,dc);
    TILE_W=S(240); TILE_H=S(150); PAD=S(16); HEADER=S(38); SEARCH_H=S(58);
    FOOTER_H=S(34); FOOTER_MIN_W=S(720); FOOTER_LINK_H=S(22);
    if(g_fPT)DeleteObject(g_fPT); if(g_fPN)DeleteObject(g_fPN); if(g_fPI)DeleteObject(g_fPI); if(g_fPX)DeleteObject(g_fPX);
    g_fPT=CreateFontW(S(20),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPN=CreateFontW(S(17),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPI=CreateFontW(S(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    g_fPX=CreateFontW(S(30),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    if(!g_searchBrush) g_searchBrush=CreateSolidBrush(RGB(34,33,38));
}
// ---- picker search / scroll / tooltip state ----
static HWND g_search=nullptr; static WNDPROC g_searchOrigProc=nullptr;
static HWND g_tip=nullptr;
struct RowRec { PickerRowHitSnapshot snapshot; };
using PickerPaintCache=PickerPaintCacheState<RowRec>;
static PickerPaintCache g_pickerPaintCache;
static PickerHoverEventState g_pickerHoverState;

static void ClearPickerDragPreview() noexcept {
    PickerDragPreviewState empty;
    g_pickerDragPreview.swap(empty);
}

static bool CapturePickerDragPreview(
        const PickerRowHitSnapshot& snapshot,POINT pointer) noexcept {
    try {
        PickerDragPreviewState staged;
        const long long width=
            static_cast<long long>(snapshot.hitRect.right)-
            snapshot.hitRect.left;
        const long long height=
            static_cast<long long>(snapshot.hitRect.bottom)-
            snapshot.hitRect.top;
        const long long grabX=
            static_cast<long long>(pointer.x)-snapshot.hitRect.left;
        const long long grabY=
            static_cast<long long>(pointer.y)-snapshot.hitRect.top;
        if(width<=0 || height<=0 ||
           width>(std::numeric_limits<LONG>::max)() ||
           height>(std::numeric_limits<LONG>::max)() ||
           grabX<(std::numeric_limits<LONG>::min)() ||
           grabX>(std::numeric_limits<LONG>::max)() ||
           grabY<(std::numeric_limits<LONG>::min)() ||
           grabY>(std::numeric_limits<LONG>::max)()){
            ClearPickerDragPreview();
            return false;
        }
        staged.size={
            static_cast<LONG>(width),static_cast<LONG>(height)};
        staged.grabOffset={
            static_cast<LONG>(grabX),static_cast<LONG>(grabY)};
        if(!PickerDragPreviewGeometryValid(
                staged.size,staged.grabOffset)){
            ClearPickerDragPreview();
            return false;
        }
        staged.fullTitle=snapshot.fullTitle;
        staged.runtimeKey=snapshot.runtimeKey;
        staged.identity=snapshot.action.identity;
        staged.pointer=pointer;
        staged.modelGeneration=snapshot.action.modelGeneration;
        staged.rowLayoutEpoch=snapshot.action.rowLayoutEpoch;
        staged.captured=true;
        g_pickerDragPreview.swap(staged);
        return true;
    } catch(...) {
        ClearPickerDragPreview();
        return false;
    }
}

static bool CurrentPickerDragPreviewBounds(
        HWND owner,RECT& bounds) noexcept {
    bounds=RECT{0,0,0,0};
    const bool identityMatches=
        g_pickerDragPreview.captured &&
        SameIdentity(
            g_pickerDragPreview.identity,
            g_pickerGesture.row.identity);
    if(!PickerDragPreviewPaintable(
            g_pickerGesture.phase,
            owner && GetCapture()==owner,
            identityMatches,
            g_pickerDragPreview.modelGeneration,
            g_picker.modelGeneration,
            g_pickerDragPreview.rowLayoutEpoch,
            g_picker.rowLayoutEpoch) ||
       !PickerDragPreviewGeometryValid(
            g_pickerDragPreview.size,
            g_pickerDragPreview.grabOffset))
        return false;
    bounds=PickerDragPreviewBounds(
        g_pickerDragPreview.pointer,
        g_pickerDragPreview.size,
        g_pickerDragPreview.grabOffset);
    return bounds.right>bounds.left && bounds.bottom>bounds.top;
}

static bool CurrentPickerDragPreviewClientBounds(
        HWND owner,RECT& bounds) noexcept {
    RECT raw={0,0,0,0};
    RECT client={0,0,0,0};
    if(!CurrentPickerDragPreviewBounds(owner,raw) || !owner ||
       !GetClientRect(owner,&client)){
        bounds=RECT{0,0,0,0};
        return false;
    }
    const PickerDragPreviewBlit clipped=
        ResolvePickerDragPreviewBlit(raw,client);
    bounds=clipped.destination;
    return clipped.visible;
}

static int HitPickerTile(POINT point) noexcept {
    for(size_t index=0;index<g_tiles.size();++index)
        if(PtInRect(&g_tiles[index].rc,point))
            return static_cast<int>(index);
    return -1;
}

static int HitPickerRow(POINT point) noexcept {
    if(!PickerPaintCacheMatches(
            g_picker,g_pickerPaintCache.generation)) return -1;
    for(size_t index=0;index<g_pickerPaintCache.hoverRows.size();++index)
        if(PtInRect(
                &g_pickerPaintCache.hoverRows[index].snapshot.hitRect,
                point)) return static_cast<int>(index);
    return -1;
}

static PickerActionIntent PickerGestureTraceIntent(
        PickerPointerPhase phase,PickerGestureAction action,
        bool ctrlAtDown) noexcept {
    if(phase==PickerPointerPhase::Dragging ||
       action==PickerGestureAction::DragStarted ||
       action==PickerGestureAction::Drop ||
       action==PickerGestureAction::NoOp)
        return PickerActionIntent::RowMoveOnly;
    if(action==PickerGestureAction::SwitchOnly)
        return PickerActionIntent::TileSwitch;
    return ctrlAtDown
        ?PickerActionIntent::MoveAndFollow
        :PickerActionIntent::ActivateExact;
}

static void EmitPickerGestureTrace(
        const PickerPointerGesture& facts,
        PickerPointerPhase before,PickerPointerPhase after,
        PickerGestureAction action,bool thresholdCrossed,
        int destinationTileIndex) noexcept {
    PickerTraceGestureEvent event;
    event.phaseBefore=before;
    event.phaseAfter=after;
    event.action=action;
    event.intent=PickerGestureTraceIntent(
        before,action,facts.ctrlAtDown);
    event.sourceTileIndex=facts.row.tileIndex;
    event.destinationTileIndex=destinationTileIndex;
    event.ctrlAtDown=facts.ctrlAtDown;
    event.thresholdCrossed=thresholdCrossed;
    event.modelGenerationValid=
        facts.row.modelGeneration==g_picker.modelGeneration;
    event.rowLayoutEpochValid=
        facts.rowLayoutEpoch==g_picker.rowLayoutEpoch;
    g_pickerTrace.emit(event);
}

static bool ResetPickerPointerGesture(
        HWND owner,bool releaseCapture,
        bool emitCancellation=true) noexcept {
    const bool changed=
        g_pickerGesture.phase!=PickerPointerPhase::Idle;
    const PickerPointerGesture facts=g_pickerGesture;
    ClearPickerDragPreview();
    CancelPickerRowGesture(g_pickerGesture);
    if(changed && emitCancellation)
        EmitPickerGestureTrace(
            facts,facts.phase,PickerPointerPhase::Idle,
            PickerGestureAction::Cancel,false,
            facts.dropTileIndex);
    if(releaseCapture && owner && GetCapture()==owner)
        ReleaseCapture();
    if(changed && owner) InvalidateRect(owner,nullptr,FALSE);
    return changed;
}

static std::wstring LowerW(std::wstring s){ if(!s.empty()) CharLowerW(&s[0]); return s; }
static bool MatchesSearch(const std::wstring& title){ return g_picker.searchText.empty() || LowerW(title).find(g_picker.searchText)!=std::wstring::npos; }
// ---- picker palette (per mockup) ----
static const COLORREF CLR_BG=RGB(20,20,24), CLR_TILE=RGB(28,28,33), CLR_TILE_DIM=RGB(22,22,26), CLR_SEARCH=RGB(34,33,38),
    CLR_ACTIVE=RGB(0xF2,0x96,0x05) /*#f29605*/, CLR_PASSIVE=RGB(0x6B,0x60,0x4F) /*#6b604f*/, CLR_BORDER=RGB(58,55,52),
    CLR_TEXT=RGB(208,206,210), CLR_HEAD=RGB(238,238,242), CLR_HINT=RGB(150,145,135), CLR_DIM=RGB(110,108,112),
    CLR_SCROLL_TRK=RGB(40,40,46), CLR_SCROLL_THB=RGB(96,92,86);
static HFONT g_searchFont=nullptr;
static uint64_t g_iconTouch=0;
static HICON g_sharedFallbackIcon=LoadIconW(nullptr,IDI_APPLICATION);
static const size_t PICKER_ICON_CACHE_LIMIT=256;
static const size_t PICKER_ICON_PRELOAD_MISS_BUDGET=4;
static OwnedIconCache g_windowIconCache(256,{
    [](HICON icon){ return icon?CopyIcon(icon):nullptr; },
    [](HICON icon)->bool { return !icon || DestroyIcon(icon)!=FALSE; }
});

static void MarkPickerIconPreloadDirty(int priority=-1) noexcept {
    g_pickerIconPreloadQueueDirty=true;
    g_pickerIconPreloadPriority=priority;
    g_pickerIconPreloadGate.markDirty();
}
static RECT SearchBoxRect(int clientW){ RECT r; r.left=PAD; r.top=S(12); r.right=clientW-PAD; r.bottom=S(12)+S(40); return r; }
static void FillRoundRect(HDC hdc, RECT r, int rad, COLORREF fill, COLORREF border, int bw){
    HBRUSH b=CreateSolidBrush(fill); HPEN p=CreatePen(PS_SOLID,bw,border);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,b); HPEN op=(HPEN)SelectObject(hdc,p);
    RoundRect(hdc,r.left,r.top,r.right,r.bottom,rad,rad);
    SelectObject(hdc,ob); SelectObject(hdc,op); DeleteObject(b); DeleteObject(p);
}

static BOOL PickerTraceIsWindowVisible(void*,HWND hwnd) noexcept {
    return IsWindowVisible(hwnd);
}

static int PickerTraceWindowTitleLength(
        void*,HWND hwnd,DWORD& error) noexcept {
    SetLastError(ERROR_SUCCESS);
    const int length=GetWindowTextLengthW(hwnd);
    error=GetLastError();
    return length;
}

static LONG_PTR PickerTraceWindowExtendedStyle(
        void*,HWND hwnd,DWORD& error) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    error=GetLastError();
    return style;
}

static HWND PickerTraceWindowRootOwner(void*,HWND hwnd) noexcept {
    return GetAncestor(hwnd,GA_ROOTOWNER);
}

static PickerTraceAltTabOps PickerTraceProductAltTabOps() noexcept {
    PickerTraceAltTabOps ops;
    ops.isVisible=PickerTraceIsWindowVisible;
    ops.titleLength=PickerTraceWindowTitleLength;
    ops.extendedStyle=PickerTraceWindowExtendedStyle;
    ops.rootOwner=PickerTraceWindowRootOwner;
    return ops;
}

static PickerTraceSafeImageBasename ReadPickerTraceImageBasename(
        DWORD pid) noexcept {
    try {
        std::vector<wchar_t> path(32768,L'\0');
        UniqueWinHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if(!process) return PickerTraceSafeImageBasename{};
        DWORD length=static_cast<DWORD>(path.size());
        if(!QueryFullProcessImageNameW(
                process.get(),0,path.data(),&length) || length==0)
            return PickerTraceSafeImageBasename{};
        size_t begin=static_cast<size_t>(length);
        while(begin>0 && path[begin-1]!=L'\\' && path[begin-1]!=L'/')
            --begin;
        const size_t basenameLength=static_cast<size_t>(length)-begin;
        if(basenameLength==0 || basenameLength>260)
            return PickerTraceSafeImageBasename{};
        return MakePickerTraceSafeImageBasename(
            path.data()+begin,static_cast<int>(basenameLength));
    } catch(...) {
        return PickerTraceSafeImageBasename{};
    }
}

static void CollectPickerTraceEnumNonDrivingFacts(
        HWND hwnd,PickerTraceEnumWindowEvent& event) noexcept {
    if(!g_pickerTrace.active()) return;
    try {
        event.owner=reinterpret_cast<uintptr_t>(GetWindow(hwnd,GW_OWNER));
        event.lastActivePopup=
            reinterpret_cast<uintptr_t>(GetLastActivePopup(hwnd));
        wchar_t className[129]={0};
        const int classLength=GetClassNameW(
            hwnd,className,static_cast<int>(_countof(className)));
        if(classLength>0)
            event.className=MakePickerTraceSafeClassName(
                className,classLength);
        // Do not retry the product PID read after that branch was reached:
        // a later success would make a pid-unavailable decision look false.
        if(event.pid==0 && event.secondTitleCopied<=0){
            DWORD pid=0;
            event.tid=GetWindowThreadProcessId(hwnd,&pid);
            event.pid=pid;
        }
        if(event.pid!=0)
            event.imageBasename=ReadPickerTraceImageBasename(event.pid);
        event.cloakedObserved=true;
        event.cloakedResult=DwmGetWindowAttribute(
            hwnd,DWMWA_CLOAKED,&event.cloaked,sizeof(event.cloaked));
    } catch(...) {}
}
static uint64_t NextIconTouch() noexcept {
    if(g_iconTouch!=(std::numeric_limits<uint64_t>::max)()) ++g_iconTouch;
    return g_iconTouch;
}

static HICON LoadWindowIconOutsidePaint(const WinItem& window) noexcept {
    if(!PickerRowUsesStableIdentity(window.admission))
        return g_sharedFallbackIcon;
    HICON cached=g_windowIconCache.getAndTouch(
        window.runtimeKey,NextIconTouch());
    if(cached) return cached;
    if(RecaptureGenericWindowIdentity(window.identity)!=
       WindowIdentityRecapture::Match) return g_sharedFallbackIcon;
    const HWND hwnd=reinterpret_cast<HWND>(window.identity.hwnd);
    HICON borrowed=reinterpret_cast<HICON>(
        GetClassLongPtrW(hwnd,GCLP_HICONSM));
    if(!borrowed) borrowed=reinterpret_cast<HICON>(
        GetClassLongPtrW(hwnd,GCLP_HICON));
    if(!borrowed){
        DWORD_PTR response=0;
        SendMessageTimeoutW(hwnd,WM_GETICON,ICON_SMALL2,0,
            SMTO_ABORTIFHUNG|SMTO_BLOCK,25,&response);
        borrowed=reinterpret_cast<HICON>(response);
    }
    if(RecaptureGenericWindowIdentity(window.identity)!=
       WindowIdentityRecapture::Match) return g_sharedFallbackIcon;
    if(!borrowed) borrowed=g_sharedFallbackIcon;
    HICON owned=borrowed?g_windowIconCache.insertBorrowed(
        window.runtimeKey,borrowed,NextIconTouch()):nullptr;
    return owned?owned:g_sharedFallbackIcon;
}

static HICON CachedWindowIcon(const std::string& runtimeKey) noexcept {
    HICON cached=g_windowIconCache.peek(runtimeKey);
    return cached?cached:g_sharedFallbackIcon;
}

static void PruneIconCache(const std::set<std::string>& liveKeys) noexcept {
    g_windowIconCache.pruneTo(liveKeys);
}

static bool ClearWindowIconCache() noexcept {
    return g_windowIconCache.clear();
}

struct PickerTraceCurrentDesktopFacts {
    bool currentInvoked=false;
    HRESULT currentResult=E_NOINTERFACE;
    bool idInvoked=false;
    HRESULT idResult=E_NOTIMPL;
    GUID actual{};
    PickerReadValidity validity=PickerReadValidity::Unavailable;
};

static GUID CurrentDesktopGuid(
        PickerTraceCurrentDesktopFacts* facts=nullptr) noexcept {
    if(facts) *facts=PickerTraceCurrentDesktopFacts{};
    GUID guid={0};
    if(!g_vdmi) return guid;
    IVirtualDesktop* raw=nullptr;
    if(facts) facts->currentInvoked=true;
    const HRESULT currentResult=g_vdmi->GetCurrentDesktop(&raw);
    if(facts) facts->currentResult=currentResult;
    ScopedComPtr<IVirtualDesktop> desktop(raw);
    if(FAILED(currentResult) || !desktop) return guid;
    GUID candidate={0};
    if(facts) facts->idInvoked=true;
    const HRESULT idResult=desktop->GetID(&candidate);
    if(facts){
        facts->idResult=idResult;
        facts->actual=candidate;
    }
    if(FAILED(idResult) || GuidIsZero(candidate))
        return guid;
    if(facts) facts->validity=PickerReadValidity::Valid;
    return candidate;
}

struct PickerWindowIdentityCaptureTraceFacts {
    DWORD pid=0;
    DWORD tid=0;
    bool identityComplete=false;
    WindowIdentityRecapture recapture=
        WindowIdentityRecapture::Indeterminate;
};

static WindowIdentityKey CapturePickerWindowIdentity(
        HWND hwnd,
        PickerWindowIdentityCaptureTraceFacts* facts=nullptr) noexcept {
    if(facts) *facts=PickerWindowIdentityCaptureTraceFacts{};
    WindowIdentityKey identity;
    if(!hwnd || !IsWindow(hwnd)){
        if(facts) facts->recapture=WindowIdentityRecapture::Lost;
        return identity;
    }
    DWORD pid=0;
    const DWORD tid=GetWindowThreadProcessId(hwnd,&pid);
    if(facts){
        facts->pid=pid;
        facts->tid=tid;
    }
    if(!tid || pid==0) return identity;
    uint64_t started=0;
    if(!TryReadProcessStart(pid,started)) return identity;
    identity.hwnd=reinterpret_cast<uintptr_t>(hwnd);
    identity.pid=pid;
    identity.processStart=started;
    const bool complete=SameIdentity(identity,identity);
    const WindowIdentityRecapture recapture=complete
        ? RecaptureGenericWindowIdentity(identity)
        : WindowIdentityRecapture::Lost;
    if(facts){
        facts->identityComplete=complete;
        facts->recapture=recapture;
    }
    if(recapture!=WindowIdentityRecapture::Match)
        return WindowIdentityKey{};
    return identity;
}

struct PickerEnumContext {
    std::vector<Tile>* tiles=nullptr;
    std::set<std::string>* liveKeys=nullptr;
    GUID currentDesktop{};
    std::map<DWORD,uint64_t> processStarts;
    uint64_t modelGeneration=0;
    uint64_t enumSequence=0;
    uint64_t candidates=0;
    std::array<uint64_t,static_cast<size_t>(
        PickerTraceEnumDecision::Count)> decisionCounts{};
    bool traceActive=false;
    bool failed=false;
};

static BOOL HandlePickerRowReadResult(
        PickerEnumContext& context,PickerRowReadResult result) noexcept {
    return ContinuePickerRowEnumeration(result,context.failed)?TRUE:FALSE;
}

static BOOL FinishPickerEnumWindow(
        PickerEnumContext& context,HWND hwnd,
        PickerTraceEnumWindowEvent& event,
        PickerTraceEnumDecision decision,BOOL productResult) noexcept {
    if(!context.traceActive) return productResult;
    const DWORD productLastError=GetLastError();
    event.decision=decision;
    if(context.enumSequence!=(std::numeric_limits<uint64_t>::max)())
        ++context.enumSequence;
    event.enumSequence=context.enumSequence;
    if(context.candidates!=(std::numeric_limits<uint64_t>::max)())
        ++context.candidates;
    const size_t decisionIndex=static_cast<size_t>(decision);
    if(decisionIndex<context.decisionCounts.size() &&
       context.decisionCounts[decisionIndex]!=
           (std::numeric_limits<uint64_t>::max)())
        ++context.decisionCounts[decisionIndex];
    CollectPickerTraceEnumNonDrivingFacts(hwnd,event);
    g_pickerTrace.emit(event);
    SetLastError(productLastError);
    return productResult;
}

static BOOL CALLBACK EnumAll(HWND hwnd,LPARAM parameter){
    PickerEnumContext& context=
        *reinterpret_cast<PickerEnumContext*>(parameter);
    PickerTraceEnumWindowEvent traceEvent;
    traceEvent.modelGeneration=context.modelGeneration;
    traceEvent.hwnd=reinterpret_cast<uintptr_t>(hwnd);
    const auto finish=[&](PickerTraceEnumDecision decision,
                          BOOL productResult) noexcept {
        return FinishPickerEnumWindow(
            context,hwnd,traceEvent,decision,productResult);
    };
    try {
        const PickerTraceAltTabFacts altTab=
            ObservePickerTraceAltTabWindow(hwnd,PickerTraceProductAltTabOps());
        traceEvent.visibleObserved=altTab.visibleObserved;
        traceEvent.visible=altTab.visible;
        traceEvent.firstTitleObserved=altTab.firstTitleObserved;
        traceEvent.firstTitleLength=altTab.firstTitleLength;
        traceEvent.firstTitleError=altTab.firstTitleError;
        traceEvent.exStyleObserved=altTab.exStyleObserved;
        traceEvent.exStyle=static_cast<uint64_t>(altTab.exStyle);
        traceEvent.exStyleError=altTab.exStyleError;
        traceEvent.toolWindow=altTab.exStyleObserved &&
            (altTab.exStyle&WS_EX_TOOLWINDOW)!=0;
        traceEvent.rootOwnerObserved=altTab.rootOwnerObserved;
        traceEvent.rootOwner=
            reinterpret_cast<uintptr_t>(altTab.rootOwner);
        traceEvent.rootOwnerSelf=altTab.rootOwnerObserved &&
            altTab.rootOwner==hwnd;
        traceEvent.altTabReason=altTab.reason;
        if(altTab.reason!=PickerTraceAltTabReason::Eligible)
            return finish(DecidePickerTraceEnumDecision(
                altTab.reason,true,S_OK,true,0,1,1,true,true,
                WindowIdentityRecapture::Match),TRUE);

        GUID desktop={0};
        if(!g_vdmDoc){
            traceEvent.desktopResult=E_NOINTERFACE;
            const BOOL productResult=HandlePickerRowReadResult(
                context,PickerRowReadResult::GlobalSnapshotFailure);
            return finish(
                PickerTraceEnumDecision::SkipDesktopServiceMissing,
                productResult);
        }
        traceEvent.desktopResult=
            g_vdmDoc->GetWindowDesktopId(hwnd,&desktop);
        traceEvent.desktop=desktop;
        Tile* tile=nullptr;
        if(SUCCEEDED(traceEvent.desktopResult) && !GuidIsZero(desktop))
            for(Tile& candidate : *context.tiles)
                if(GuidEq(candidate.guid,desktop)){
                    tile=&candidate;
                    traceEvent.tileIndex=candidate.index;
                    break;
                }
        Tile* currentTile=nullptr;
        if(!tile && !GuidIsZero(context.currentDesktop))
            for(Tile& candidate : *context.tiles)
                if(GuidEq(candidate.guid,context.currentDesktop)){
                    currentTile=&candidate;
                    break;
                }
        BOOL onCurrentDesktop=FALSE;
        HRESULT currentMembershipResult=E_NOTIMPL;
        if(!tile && currentTile){
            try {
                currentMembershipResult=g_vdmDoc->
                    IsWindowOnCurrentVirtualDesktop(hwnd,&onCurrentDesktop);
            } catch(...) {
                currentMembershipResult=E_FAIL;
            }
        }
        const PickerDesktopTileRoute desktopRoute=
            DecidePickerDesktopTileRoute(
                tile!=nullptr,currentTile!=nullptr,
                traceEvent.desktopResult,currentMembershipResult,
                onCurrentDesktop!=FALSE);
        const bool currentDesktopFallback=
            desktopRoute==PickerDesktopTileRoute::CurrentDesktopFallback ||
            desktopRoute==PickerDesktopTileRoute::
                GloballyVisibleCurrentDesktopFallback;
        if(currentDesktopFallback){
            tile=currentTile;
            traceEvent.tileIndex=tile->index;
        }
        if(desktopRoute==PickerDesktopTileRoute::Skip)
            return finish(
                PickerTraceEnumDecision::SkipDesktopTileMissing,
                HandlePickerRowReadResult(
                    context,PickerRowReadResult::DesktopUnavailable));

        traceEvent.secondTitleObserved=true;
        SetLastError(ERROR_SUCCESS);
        const int length=GetWindowTextLengthW(hwnd);
        traceEvent.secondTitleLength=length;
        traceEvent.secondTitleError=GetLastError();
        if(length<=0)
            return finish(
                PickerTraceEnumDecision::SkipSecondTitleUnavailable,
                HandlePickerRowReadResult(
                    context,PickerRowReadResult::TitleUnavailable));
        std::wstring title(static_cast<size_t>(length)+1,L'\0');
        SetLastError(ERROR_SUCCESS);
        const int copied=GetWindowTextW(hwnd,&title[0],length+1);
        traceEvent.secondTitleCopied=copied;
        traceEvent.secondTitleError=GetLastError();
        if(copied<=0)
            return finish(
                PickerTraceEnumDecision::SkipSecondTitleReadFailed,
                HandlePickerRowReadResult(
                    context,PickerRowReadResult::TitleUnavailable));
        title.resize(static_cast<size_t>(copied));

        WindowIdentityKey identity;
        WindowIdentityRecapture recapture=
            WindowIdentityRecapture::Indeterminate;
        bool identityComplete=false;
        DWORD pid=0;
        traceEvent.tid=GetWindowThreadProcessId(hwnd,&pid);
        traceEvent.pid=pid;
        if(traceEvent.tid && pid!=0){
            auto process=context.processStarts.find(pid);
            if(process==context.processStarts.end()){
                uint64_t started=0;
                PickerProcessStartTraceFacts processFacts;
                if(TryReadProcessStart(pid,started,&processFacts))
                    process=context.processStarts.emplace(pid,started).first;
                traceEvent.processStartError=processFacts.timesAttempted
                    ? processFacts.timesError : processFacts.openError;
            } else {
                traceEvent.processStartCacheHit=true;
            }
            if(process!=context.processStarts.end()){
                traceEvent.processStartAvailable=true;
                identity.hwnd=reinterpret_cast<uintptr_t>(hwnd);
                identity.pid=pid;
                identity.processStart=process->second;
                identityComplete=SameIdentity(identity,identity);
                if(identityComplete)
                    recapture=RecaptureGenericWindowIdentity(identity);
            }
        }
        traceEvent.identityComplete=identityComplete;
        traceEvent.recapture=recapture;
        const PickerRowAdmission baseAdmission=DecidePickerRowAdmission(
            true,desktopRoute!=PickerDesktopTileRoute::Skip,true,
            identityComplete,recapture);
        const PickerTraceEnumDecision baseDecision=
            DecidePickerTraceEnumDecision(
            altTab.reason,true,traceEvent.desktopResult,
            !GuidIsZero(desktop),
            traceEvent.tileIndex,length,copied,
            traceEvent.tid!=0 && pid!=0,
            traceEvent.processStartAvailable,recapture);
        const PickerFinalRowRoute<PickerTraceEnumDecision> finalRoute=
            FinalizePickerRowRoute(
                baseAdmission,desktopRoute,baseDecision,
                PickerTraceEnumDecision::
                    DisplayOnlyCurrentDesktopFallback,
                PickerTraceEnumDecision::
                    VerifiedCurrentDesktopFallback);
        const PickerRowAdmission admission=finalRoute.admission;
        const PickerTraceEnumDecision decision=finalRoute.decision;
        if(admission==PickerRowAdmission::Skip)
            return finish(decision,TRUE);

        WinItem item;
        item.hwnd=hwnd;
        item.admission=admission;
        item.observedDesktop=desktop;
        item.baseDesktop=tile->guid;
        item.displayedDesktop=tile->guid;
        item.desktopRoute=TargetRouteFromPickerTileRoute(desktopRoute);
        item.title=title;
        item.titleLC=title;
        if(!item.titleLC.empty()) CharLowerW(&item.titleLC[0]);
        item.search=item.titleLC;
        if(PickerRowUsesStableIdentity(admission)){
            item.identity=identity;
            item.runtimeKey=RuntimeKey(identity);
            IApplicationView* rawView=nullptr;
            HRESULT viewResult=E_NOINTERFACE;
            if(g_avc){
                try { viewResult=g_avc->GetViewForHwnd(hwnd,&rawView); }
                catch(...) { viewResult=E_FAIL; }
            }
            ScopedComPtr<IApplicationView> view(rawView);
            if(SUCCEEDED(viewResult) && view){
                TargetMobilityProbeFacts mobilityFacts;
                item.mobility=QueryTargetWindowMobility(
                    identity,item.desktopRoute,view.get(),
                    mobilityFacts).mobility;
            }
            if(!context.liveKeys){
                const BOOL productResult=HandlePickerRowReadResult(
                    context,PickerRowReadResult::GlobalSnapshotFailure);
                return finish(
                    PickerTraceEnumDecision::GlobalSnapshotFailure,
                    productResult);
            }
        }
        tile->windows.push_back(std::move(item));
        if(PickerRowUsesStableIdentity(admission))
            context.liveKeys->insert(tile->windows.back().runtimeKey);
        return finish(decision,TRUE);
    } catch(...) {
        const BOOL productResult=HandlePickerRowReadResult(
            context,PickerRowReadResult::AllocationFailure);
        return finish(
            PickerTraceEnumDecision::AllocationFailure,productResult);
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

struct PickerModelAttemptTraceFacts {
    GUID currentDesktop{};
    uint64_t modelGeneration=0;
    bool currentDesktopAvailable=false;
};

static uint64_t NextPickerModelGeneration(uint64_t current) noexcept {
    const uint64_t next=current==(std::numeric_limits<uint64_t>::max)()
        ? 1 : current+1;
    return next==0 ? 1 : next;
}

static void RecordPickerDesktopSnapshotObservation(
        PickerTraceDesktopSnapshotFacts& facts,
        const DesktopCollectionSnapshotObservation& observation) noexcept {
    facts.result=observation.result;
    facts.index=observation.index;
    facts.count=observation.count;
    switch(observation.stage){
    case DesktopCollectionSnapshotObservationStage::GetDesktops:
        facts.status=PickerTraceDesktopSnapshotStatus::GetDesktopsFailed;
        return;
    case DesktopCollectionSnapshotObservationStage::GetCount:
        facts.status=PickerTraceDesktopSnapshotStatus::GetCountFailed;
        return;
    case DesktopCollectionSnapshotObservationStage::InvalidCount:
        facts.status=PickerTraceDesktopSnapshotStatus::InvalidCount;
        return;
    case DesktopCollectionSnapshotObservationStage::GetAt:
        facts.status=PickerTraceDesktopSnapshotStatus::GetAtFailed;
        return;
    case DesktopCollectionSnapshotObservationStage::GetId:
        facts.status=PickerTraceDesktopSnapshotStatus::GetIdFailed;
        return;
    case DesktopCollectionSnapshotObservationStage::InvalidGuid:
        facts.status=PickerTraceDesktopSnapshotStatus::InvalidGuid;
        return;
    case DesktopCollectionSnapshotObservationStage::Complete:
        facts.status=PickerTraceDesktopSnapshotStatus::Complete;
        return;
    case DesktopCollectionSnapshotObservationStage::AllocationFailure:
        facts.status=PickerTraceDesktopSnapshotStatus::AllocationFailure;
        return;
    case DesktopCollectionSnapshotObservationStage::Exception:
        facts.status=PickerTraceDesktopSnapshotStatus::Exception;
        return;
    }
}

static bool ResolvePickerModelClientSize(
        bool resetUi,size_t tileCount,int& width,int& height) noexcept;
static bool BuildPickerPaintCache(
        PickerPaintCache& cache,std::vector<Tile>& tiles,
        PickerState& state,int clientWidth,int clientHeight,
        uint64_t generation) noexcept;
static void PublishPickerGridMetrics(
        size_t tileCount,int clientWidth) noexcept;
static void ResetPickerHoverState(
        PickerHoverResetReason reason) noexcept;

static bool BuildModel(const WindowIdentityKey& activeWindow,bool resetUi,
                       bool selectionFromActual=false,
                       PickerTraceDesktopSnapshotFacts* snapshotFacts=nullptr,
                       PickerModelAttemptTraceFacts* attemptFacts=nullptr,
                       PickerVisualAssignmentMutation visualMutation={}){
    if(snapshotFacts) *snapshotFacts=PickerTraceDesktopSnapshotFacts{};
    const GUID observedCurrent=CurrentDesktopGuid();
    const uint64_t attemptedGeneration=
        NextPickerModelGeneration(g_picker.modelGeneration);
    if(attemptFacts){
        attemptFacts->currentDesktop=observedCurrent;
        attemptFacts->currentDesktopAvailable=!GuidIsZero(observedCurrent);
        attemptFacts->modelGeneration=attemptedGeneration;
    }
    std::set<std::string> liveKeys;
    PickerEnumContext enumContext;
    bool enumAttempted=false;
    BOOL enumWindowsResult=FALSE;
    DWORD enumWindowsError=ERROR_SUCCESS;
    int publishedClientWidth=0;
    if(GuidIsZero(observedCurrent))
        SetPickerCurrentDesktop(g_picker,GUID{});
    const bool published=RunPickerVisualRefreshTransaction(
        g_tiles,g_pickerPaintCache,g_picker,visualMutation,
        [&](std::vector<Tile>& tiles,PickerPaintCache& paintCache,
            PickerState& state){
            if(!g_vdmi){
                if(snapshotFacts){
                    snapshotFacts->status=
                        PickerTraceDesktopSnapshotStatus::DesktopServiceMissing;
                    snapshotFacts->result=E_NOINTERFACE;
                }
                return false;
            }
            if(resetUi){
                state.searchEditText.clear();
                state.searchText.clear();
                state.searchActive=false;
                state.scrollByDesktop.clear();
                EndPickerVisualSession(state);
            }
            if(resetUi) state.activeWindow=activeWindow;
            SetPickerCurrentDesktop(state,observedCurrent);

            DesktopCollectionComOps desktopOps;
            std::vector<DesktopCollectionEntry> desktopSnapshot;
            std::function<void(
                const DesktopCollectionSnapshotObservation&)> observer;
            bool observerReady=false;
            if(snapshotFacts){
                try {
                    observer=[snapshotFacts](
                            const DesktopCollectionSnapshotObservation& value)
                            noexcept {
                        RecordPickerDesktopSnapshotObservation(
                            *snapshotFacts,value);
                    };
                    observerReady=true;
                } catch(...) {}
            }
            const bool snapshotReady=observerReady
                ? SnapshotDesktopCollectionOwned<
                    IObjectArray,IVirtualDesktop>(
                        desktopOps,desktopSnapshot,&observer)
                : SnapshotDesktopCollectionOwned<
                    IObjectArray,IVirtualDesktop>(
                        desktopOps,desktopSnapshot);
            if(!snapshotReady) return false;
            tiles.reserve(desktopSnapshot.size());
            std::vector<GUID> desktopGuids;
            desktopGuids.reserve(desktopSnapshot.size());
            for(const DesktopCollectionEntry& entry : desktopSnapshot){
                const UINT index=entry.index;
                const GUID guid=entry.guid;
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

            enumContext.tiles=&tiles;
            enumContext.liveKeys=&liveKeys;
            enumContext.currentDesktop=observedCurrent;
            enumContext.modelGeneration=attemptedGeneration;
            enumContext.traceActive=g_pickerTrace.active();
            if(enumContext.traceActive){
                try {
                    PickerTraceEnumBeginEvent event;
                    event.modelGeneration=attemptedGeneration;
                    event.desktops=desktopGuids;
                    g_pickerTrace.emit(event);
                } catch(...) {}
            }
            enumAttempted=true;
            SetLastError(ERROR_SUCCESS);
            const PickerTraceBoolCallResult enumResult=
                CallPickerTraceBoolWithImmediateError(
                    [&]() noexcept {
                        return EnumWindows(
                            EnumAll,
                            reinterpret_cast<LPARAM>(&enumContext));
                    },[]() noexcept { return GetLastError(); });
            enumWindowsResult=enumResult.value;
            enumWindowsError=enumResult.immediateError;
            if(!enumWindowsResult || enumContext.failed) return false;
            if(!ApplyPickerVisualAssignmentsToModel(
                    tiles,state.visualAssignments,
                    [](const Tile& tile)->const GUID& {
                        return tile.guid;
                    },
                    [](Tile& tile)->std::vector<WinItem>& {
                        return tile.windows;
                    },
                    [](const WinItem& item)->const std::string& {
                        return item.runtimeKey;
                    },
                    [](WinItem& item,const GUID& base,
                       const GUID& displayed,bool assigned) noexcept {
                        item.baseDesktop=base;
                        item.displayedDesktop=displayed;
                        item.visuallyAssigned=assigned;
                    })) return false;
            if(visualMutation.kind==
                    PickerVisualMutationKind::Erase){
                size_t destinationIndex=tiles.size();
                size_t sourceIndex=tiles.size();
                size_t windowIndex=0;
                bool duplicate=false;
                for(size_t tileIndex=0;tileIndex<tiles.size();++tileIndex){
                    if(GuidEq(tiles[tileIndex].guid,
                              visualMutation.baseDesktop))
                        destinationIndex=tileIndex;
                    for(size_t itemIndex=0;
                        itemIndex<tiles[tileIndex].windows.size();
                        ++itemIndex){
                        if(tiles[tileIndex].windows[itemIndex].runtimeKey!=
                           visualMutation.runtimeKey) continue;
                        if(sourceIndex!=tiles.size()){
                            duplicate=true;
                            break;
                        }
                        sourceIndex=tileIndex;
                        windowIndex=itemIndex;
                    }
                }
                if(duplicate || sourceIndex==tiles.size() ||
                   destinationIndex==tiles.size()) return false;
                if(sourceIndex==destinationIndex){
                    WinItem& item=
                        tiles[sourceIndex].windows[windowIndex];
                    item.baseDesktop=visualMutation.baseDesktop;
                    item.displayedDesktop=visualMutation.baseDesktop;
                    item.visuallyAssigned=false;
                } else {
                    std::vector<WinItem>& source=
                        tiles[sourceIndex].windows;
                    WinItem moved=std::move(source[windowIndex]);
                    moved.baseDesktop=visualMutation.baseDesktop;
                    moved.displayedDesktop=visualMutation.baseDesktop;
                    moved.visuallyAssigned=false;
                    tiles[destinationIndex].windows.push_back(
                        std::move(moved));
                    source.erase(source.begin()+windowIndex);
                }
            }
            if(!PopulatePickerFilteredRows(tiles,state.searchText))
                return false;
            if(selectionFromActual)
                PreparePickerRefreshSelectionFromActual(state);
            ResolvePickerSelection(state,desktopGuids);
            if(!PrunePickerScrollState(state,desktopGuids)) return false;
            state.modelGeneration=attemptedGeneration;
            AdvancePickerRowLayoutEpoch(state);
            const uint64_t paintGeneration=
                BeginPickerPaintRefresh(state);
            int clientWidth=0,clientHeight=0;
            if(!ResolvePickerModelClientSize(
                    resetUi,tiles.size(),clientWidth,clientHeight))
                return false;
            if(!BuildPickerPaintCache(
                    paintCache,tiles,state,clientWidth,clientHeight,
                    paintGeneration)) return false;
            publishedClientWidth=clientWidth;
            return true;
        });
    if(enumAttempted && enumContext.traceActive){
        PickerTraceEnumEndEvent event;
        event.modelGeneration=attemptedGeneration;
        event.candidates=enumContext.candidates;
        event.counts=enumContext.decisionCounts;
        event.enumWindowsReturned=enumWindowsResult!=FALSE;
        event.enumWindowsError=enumWindowsError;
        event.modelPublished=published;
        g_pickerTrace.emit(event);
        g_pickerTrace.flushBoundary();
    }
    if(published){
        PublishPickerGridMetrics(g_tiles.size(),publishedClientWidth);
        ResetPickerHoverState(PickerHoverResetReason::CachePublication);
        PruneIconCache(liveKeys);
        MarkPickerIconPreloadDirty();
    }
    return published;
}

static bool SetPickerSelectionCurrent(int index) noexcept {
    if(index<0 || index>=static_cast<int>(g_tiles.size())) return false;
    if(!SetPickerSelection(g_picker,index,g_tiles[index].guid)) return false;
    MarkPickerIconPreloadDirty(index);
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

template<class Mutate>
static bool PublishPickerModelPaintUpdate(
        Mutate&& mutate,bool rowLayoutChanges) noexcept {
    if(PickerInteractionBusy(g_picker,g_pickerGesture)) return false;
    try {
        std::vector<Tile> stagedTiles=g_tiles;
        PickerPaintCache stagedCache;
        PickerState stagedState=PreservePickerUi(g_picker);
        if(!mutate(stagedTiles,stagedState)) return false;
        if(rowLayoutChanges) AdvancePickerRowLayoutEpoch(stagedState);
        const uint64_t generation=BeginPickerPaintRefresh(stagedState);
        int clientWidth=0,clientHeight=0;
        if(!ResolvePickerModelClientSize(
                false,stagedTiles.size(),clientWidth,clientHeight))
            return false;
        if(!BuildPickerPaintCache(
                stagedCache,stagedTiles,stagedState,
                clientWidth,clientHeight,generation))
            return false;
        g_tiles.swap(stagedTiles);
        g_pickerPaintCache.swap(stagedCache);
        SwapPickerState(g_picker,stagedState);
        PublishPickerGridMetrics(g_tiles.size(),clientWidth);
        ResetPickerHoverState(PickerHoverResetReason::CachePublication);
        MarkPickerIconPreloadDirty();
        return true;
    } catch(...) {
        return false;
    }
}

static bool RebuildPickerFilteredRows() noexcept {
    return PublishPickerModelPaintUpdate(
        [](std::vector<Tile>& tiles,PickerState& state){
        for(size_t index=0;index<tiles.size();++index)
            if(!BuildPickerFilteredIndices(
                    tiles[index].windows,state.searchText,
                    tiles[index].filtered,
                    [](const WinItem& item)->const std::wstring& {
                        return item.search;
                    })) return false;
        return true;
    },true);
}

static bool ApplyPickerSearchText(const std::wstring& editText,
                                  const std::wstring& searchText) noexcept {
    const bool published=PublishPickerModelPaintUpdate(
        [&](std::vector<Tile>& tiles,PickerState& state){
        for(size_t index=0;index<tiles.size();++index)
            if(!BuildPickerFilteredIndices(
                    tiles[index].windows,searchText,
                    tiles[index].filtered,
                    [](const WinItem& item)->const std::wstring& {
                        return item.search;
                    })) return false;
        return SetPickerSearchText(state,editText,searchText);
    },true);
    if(published && searchText.empty())
        InvalidatePickerTabSearchCache(
            g_pickerTabSearchCache,g_picker.modelGeneration);
    return published;
}

static void ResetPickerHoverTooltip() noexcept;
static void ResetPickerHoverState(
        PickerHoverResetReason reason) noexcept {
    ResetPickerHoverTooltip();
    ResetPickerHoverEventState(g_pickerHoverState,reason);
}
static bool RefreshPickerPaintCache(
        bool allowHiddenPreparation=false) noexcept;
static void HandleSearchSessionResult(const SessionRoute& route,
                                       const SessionResult& result){
    auto operation=g_searchOperations.find(route.operationId);
    if(operation==g_searchOperations.end()) return;
    const bool currentPickerSearch=PickerTabSearchAttemptMatches(
        g_pickerTabSearchCache,route.operationId,
        g_picker.modelGeneration,g_picker.searchText) &&
        operation->second.pickerModelGeneration==g_picker.modelGeneration &&
        operation->second.pickerQuery==g_picker.searchText;
    auto captured=operation->second.snapshots.find(route.app);
    bool queued=false;
    if(currentPickerSearch && captured!=operation->second.snapshots.end() &&
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
    MarkPickerTabSearchRetryNeeded(
        g_pickerTabSearchCache,
        route.operationId,
        operation->second.pickerModelGeneration,
        operation->second.pickerQuery,route.app);
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(route.operationId);
        CompletePickerTabSearchAttempt(
            g_pickerTabSearchCache,
            route.operationId,
            operation->second.pickerModelGeneration,
            operation->second.pickerQuery);
        g_searchOperations.erase(operation);
        SchedulePickerTabSearchRetry(&route.app);
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
    const bool currentPickerSearch=PickerTabSearchAttemptMatches(
        g_pickerTabSearchCache,result->operationId,
        g_picker.modelGeneration,g_picker.searchText) &&
        !PickerInteractionBusy(g_picker,g_pickerGesture) &&
        operation->second.pickerModelGeneration==g_picker.modelGeneration &&
        operation->second.pickerQuery==g_picker.searchText;
    auto captured=operation->second.snapshots.find(result->app);
    std::map<std::string,AppFastSnapshot> current=CollectFastSnapshots();
    auto now=current.find(result->app);
    if(currentPickerSearch &&
       result->status==ReconcileResultStatus::Completed &&
       result->workMode==ReconcileWorkMode::PrepareLiveOnly &&
       result->buildLiveFromInputs &&
       captured!=operation->second.snapshots.end() && now!=current.end() &&
       FastSnapshotCanPersistAll(now->second) &&
       result->identityGeneration==now->second.identityGeneration &&
       result->contentGeneration==now->second.generation &&
       result->fastWindows.size()==result->sessionIndexByFast.size()){
        RememberAcceptedFreshRuntimeRecords(*result);
        for(size_t index=0;index<result->fastWindows.size();++index){
            const WinFp* session=ReconcileSessionForFast(*result,index);
            if(!session || session->tabsBlob.empty()) continue;
            const WindowIdentityKey resultIdentity=
                IdentityOf(result->fastWindows[index]);
            std::wstring blob=U82W(session->tabsBlob);
            if(!blob.empty()) CharLowerW(&blob[0]);
            for(Tile& tile : g_tiles)
                for(WinItem& item : tile.windows)
                    if(PickerSearchResultMatches(
                            item.identity,resultIdentity))
                        item.search=item.titleLC+L" "+blob;
        }
        accepted=true;
    }
    if(!accepted)
        MarkPickerTabSearchRetryNeeded(
            g_pickerTabSearchCache,
            result->operationId,
            operation->second.pickerModelGeneration,
            operation->second.pickerQuery,result->app,
            PickerTabSearchRetryTrigger::Immediate);
    if(operation->second.outstanding>0) --operation->second.outstanding;
    if(operation->second.outstanding==0){
        g_reconcileDeadlines.cancel(result->operationId);
        CompletePickerTabSearchAttempt(
            g_pickerTabSearchCache,
            result->operationId,
            operation->second.pickerModelGeneration,
            operation->second.pickerQuery);
        g_searchOperations.erase(operation);
        SchedulePickerTabSearchRetry();
    }
    if(accepted && g_main){
        RebuildPickerFilteredRows();
        InvalidateRect(g_main,nullptr,FALSE);
    }
}

static void CleanupPickerTabSearchEnsureOperation(
        uint64_t operationId) noexcept {
    if(operationId==0) return;
    try {
        RetireSessionRoutesForOperation(
            AsyncOperationOwner::Search,operationId);
    } catch(...) {}
    try { g_reconcileDeadlines.cancel(operationId); } catch(...) {}
    g_searchOperations.erase(operationId);
}

// Lazily request all-tab text without reading browser files on the UI thread.
static PickerTabSearchEnsureOutcome EnsureTabSearch() noexcept {
    try {
        if(g_picker.searchText.empty()){
            InvalidatePickerTabSearchCache(
                g_pickerTabSearchCache,g_picker.modelGeneration);
            return PickerTabSearchEnsureOutcome::AttemptCommitted;
        }
        if(PickerTabSearchCacheUsable(
                g_pickerTabSearchCache,g_picker.modelGeneration,
                g_picker.searchText) ||
           (g_pickerTabSearchCache.pending &&
            PickerTabSearchKeyMatches(
                g_pickerTabSearchCache,g_picker.modelGeneration,
                g_picker.searchText)))
            return PickerTabSearchEnsureOutcome::AttemptCommitted;

        SearchOperation operation;
        operation.operationId=TakeNonzeroId(g_nextOperationId);
        operation.pickerModelGeneration=g_picker.modelGeneration;
        operation.pickerQuery=g_picker.searchText;
        operation.snapshots=CollectFastSnapshots();
        std::vector<AppProfile> profiles=ActiveProfiles();
        const uint64_t operationId=operation.operationId;
        const uint64_t modelGeneration=operation.pickerModelGeneration;
        std::wstring stagedQuery=operation.pickerQuery;
        bool operationPublished=false;
        const PickerTabSearchEnsureOutcome outcome=
            RunPickerTabSearchEnsureAttempt(
                g_pickerTabSearchCache,operationId,modelGeneration,
                stagedQuery,
                [&](){
                    operationPublished=g_searchOperations.emplace(
                        operationId,std::move(operation)).second;
                    return operationPublished;
                },
                [&](){
                    for(const AppProfile& profile : profiles){
                        auto stored=g_searchOperations.find(operationId);
                        if(stored==g_searchOperations.end()) return false;
                        auto snapshot=
                            stored->second.snapshots.find(profile.id);
                        if(snapshot==stored->second.snapshots.end() ||
                           snapshot->second.windows.empty() ||
                           !FastSnapshotCanObserve(snapshot->second))
                            continue;
                        PickerTabSearchRetryTrigger retryTrigger=
                            PickerTabSearchRetryTrigger::Immediate;
                        const uint64_t request=RequestSessionWork(
                            AsyncOperationOwner::Search,operationId,profile,
                            snapshot->second,SessionPurpose::Search,
                            &retryTrigger);
                        stored=g_searchOperations.find(operationId);
                        if(stored==g_searchOperations.end()) return false;
                        if(request)
                            ++stored->second.outstanding;
                        else
                            MarkPickerTabSearchRetryNeeded(
                                g_pickerTabSearchCache,operationId,
                                modelGeneration,stagedQuery,profile.id,
                                retryTrigger);
                    }
                    auto stored=g_searchOperations.find(operationId);
                    if(stored==g_searchOperations.end()) return false;
                    if(stored->second.outstanding==0){
                        g_reconcileDeadlines.cancel(operationId);
                        CompletePickerTabSearchAttempt(
                            g_pickerTabSearchCache,operationId,
                            modelGeneration,stagedQuery);
                        g_searchOperations.erase(stored);
                        SchedulePickerTabSearchRetry();
                    }
                    return true;
                },
                [&]() noexcept {
                    CleanupPickerTabSearchPublishedOperation(
                        operationPublished,[&](){
                            CleanupPickerTabSearchEnsureOperation(
                                operationId);
                        });
                });
        if(outcome==PickerTabSearchEnsureOutcome::RetryPreserved)
            SchedulePickerTabSearchRetry();
        return outcome;
    } catch(...) {
        SchedulePickerTabSearchRetry();
        return PickerTabSearchEnsureOutcome::RetryPreserved;
    }
}
static int PickerGridColumnCount(size_t tileCount,int clientWidth) noexcept {
    const int count=tileCount>static_cast<size_t>(
        (std::numeric_limits<int>::max)())
        ?(std::numeric_limits<int>::max)()
        :static_cast<int>(tileCount);
    int columns=std::max(
        1,std::min(count,std::max(
            1,(clientWidth-PAD)/(TILE_W+PAD))));
    return std::min(columns,5);
}

static void PublishPickerGridMetrics(
        size_t tileCount,int clientWidth) noexcept {
    g_cols=PickerGridColumnCount(tileCount,clientWidth);
    const int count=tileCount>static_cast<size_t>(
        (std::numeric_limits<int>::max)())
        ?(std::numeric_limits<int>::max)()
        :static_cast<int>(tileCount);
    g_rows=(count+g_cols-1)/g_cols;
}

static void LayoutPickerTiles(
        std::vector<Tile>& tiles,int clientWidth) noexcept {
    const int columns=PickerGridColumnCount(tiles.size(),clientWidth);
    for(size_t index=0;index<tiles.size();++index){
        const int value=static_cast<int>(index);
        const int row=value/columns;
        const int column=value%columns;
        RECT rect;
        rect.left=PAD+column*(TILE_W+PAD);
        rect.top=SEARCH_H+HEADER+PAD+row*(TILE_H+PAD);
        rect.right=rect.left+TILE_W;
        rect.bottom=rect.top+TILE_H;
        tiles[index].rc=rect;
    }
}

static void LayoutTiles(int clientWidth){
    PublishPickerGridMetrics(g_tiles.size(),clientWidth);
    LayoutPickerTiles(g_tiles,clientWidth);
}
static SIZE DesiredClientSize(){
    return PickerDesiredClientSize(
        g_tiles.size(),TILE_W,TILE_H,PAD,SEARCH_H,HEADER,
        FOOTER_H,FOOTER_MIN_W);
}

static bool ResolvePickerModelClientSize(
        bool resetUi,size_t tileCount,int& width,int& height) noexcept {
    width=0;
    height=0;
    if(!resetUi && g_main){
        RECT client={0,0,0,0};
        if(GetClientRect(g_main,&client) &&
           client.right>0 && client.bottom>0){
            width=client.right;
            height=client.bottom;
            return true;
        }
    }
    const SIZE desired=PickerDesiredClientSize(
        tileCount,TILE_W,TILE_H,PAD,SEARCH_H,HEADER,
        FOOTER_H,FOOTER_MIN_W);
    if(desired.cx<=0 || desired.cy<=0) return false;
    width=desired.cx;
    height=desired.cy;
    return true;
}

static int PickerTileVisibleRows(const Tile& tile) noexcept {
    const int listTop=tile.rc.top+S(10)+S(22)+S(6);
    const int listBottom=tile.rc.bottom-S(10);
    return std::max(0,(listBottom-listTop)/S(22));
}

static int PickerTileFilteredCount(const Tile& tile) noexcept {
    const size_t integerMax=static_cast<size_t>(
        (std::numeric_limits<int>::max)());
    return tile.filtered.size()>integerMax
        ?(std::numeric_limits<int>::max)()
        :static_cast<int>(tile.filtered.size());
}

static int PickerTileMaxScroll(const Tile& tile) noexcept {
    return std::max(0,PickerTileFilteredCount(tile)-
                       PickerTileVisibleRows(tile));
}

static void ClampAllPickerScrolls() noexcept {
    int firstChanged=-1;
    for(size_t index=0;index<g_tiles.size();++index){
        Tile& tile=g_tiles[index];
        const int maximum=PickerTileMaxScroll(tile);
        const int clamped=PickerVisibleScroll(tile.scroll,maximum);
        if(tile.scroll==clamped) continue;
        if(!RememberPickerScroll(tile,clamped)) tile.scroll=clamped;
        if(firstChanged<0) firstChanged=static_cast<int>(index);
    }
    if(firstChanged>=0) MarkPickerIconPreloadDirty(firstChanged);
}

static bool BuildPickerIconPreloadQueue() noexcept {
    try {
        std::vector<PickerIconPreloadRef> staged;
        staged.reserve(PICKER_ICON_CACHE_LIMIT);
        auto appendTile=[&](size_t tileIndex){
            if(tileIndex>=g_tiles.size()) return;
            const Tile& tile=g_tiles[tileIndex];
            const int visibleRows=PickerTileVisibleRows(tile);
            const int maximum=PickerTileMaxScroll(tile);
            size_t position=static_cast<size_t>(
                PickerVisibleScroll(tile.scroll,maximum));
            for(int row=0;row<visibleRows &&
                position<tile.filtered.size() &&
                staged.size()<PICKER_ICON_CACHE_LIMIT;
                ++row,++position){
                const size_t windowIndex=tile.filtered[position];
                if(windowIndex>=tile.windows.size()) break;
                staged.push_back(PickerIconPreloadRef{
                    tileIndex,windowIndex});
            }
        };

        const int priority=g_pickerIconPreloadPriority>=0 &&
            g_pickerIconPreloadPriority<static_cast<int>(g_tiles.size())
            ?g_pickerIconPreloadPriority:-1;
        const int selected=g_picker.selectedIndex>=0 &&
            g_picker.selectedIndex<static_cast<int>(g_tiles.size())
            ?g_picker.selectedIndex:-1;
        if(priority>=0) appendTile(static_cast<size_t>(priority));
        if(selected>=0 && selected!=priority)
            appendTile(static_cast<size_t>(selected));
        for(size_t index=0;index<g_tiles.size() &&
            staged.size()<PICKER_ICON_CACHE_LIMIT;++index){
            if(static_cast<int>(index)==priority ||
               static_cast<int>(index)==selected) continue;
            appendTile(index);
        }
        g_pickerIconPreloadQueue.swap(staged);
        g_pickerIconPreloadQueueDirty=false;
        g_pickerIconPreloadPriority=-1;
        return true;
    } catch(...) {
        g_pickerIconPreloadQueue.clear();
        g_pickerIconPreloadQueueDirty=false;
        g_pickerIconPreloadPriority=-1;
        g_pickerIconPreloadGate.cancel();
        return false;
    }
}

static void SchedulePickerIconPreloadContinuation() noexcept {
    if(!g_main || g_pickerIconPreloadTimerArmed ||
       !g_pickerIconPreloadGate.dirty()) return;
    if(SetTimer(g_main,TIMER_PICKER_ICON_PRELOAD,1,nullptr))
        g_pickerIconPreloadTimerArmed=true;
}

static void PreloadVisiblePickerIcons(bool continuation=false) noexcept {
    if(!continuation && !g_pickerIconPreloadQueueDirty) return;
    if(g_pickerIconPreloadQueueDirty &&
       !BuildPickerIconPreloadQueue()) return;
    const IconPreloadTurn turn=g_pickerIconPreloadGate.runTurn(
        PICKER_ICON_PRELOAD_MISS_BUDGET,[&](size_t cursor){
            if(cursor>=g_pickerIconPreloadQueue.size())
                return IconPreloadStep::Exhausted;
            const PickerIconPreloadRef ref=
                g_pickerIconPreloadQueue[cursor];
            if(ref.tile>=g_tiles.size() ||
               ref.window>=g_tiles[ref.tile].windows.size())
                return IconPreloadStep::Cached;
            const WinItem& window=g_tiles[ref.tile].windows[ref.window];
            if(!PickerRowUsesStableIdentity(window.admission))
                return IconPreloadStep::Cached;
            if(g_windowIconCache.getAndTouch(
                    window.runtimeKey,NextIconTouch()))
                return IconPreloadStep::Cached;
            LoadWindowIconOutsidePaint(window);
            return IconPreloadStep::Miss;
        });
    if(!turn.complete) SchedulePickerIconPreloadContinuation();
}

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

static bool BuildPickerPaintCache(
        PickerPaintCache& cache,std::vector<Tile>& tiles,
        PickerState& state,int clientWidth,int clientHeight,
        uint64_t generation) noexcept {
    try {
        LayoutPickerTiles(tiles,clientWidth);
        cache.generation=generation;
        cache.switchHeader=L"Switch to: ";
        const int selected=state.selectedIndex;
        if(selected>=0 && selected<static_cast<int>(tiles.size()))
            cache.switchHeader+=tiles[selected].name;
        cache.moveHeader=L"Move window:  ";
        cache.moveHeader+=
            g_targetTitle.empty()?L"(no window)":g_targetTitle;
        cache.footer.repo=FooterRepoLabel();
        cache.footer.middle=FooterMiddle();
        cache.footer.conus=FooterConusLabel();

        if(state.searchActive){
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
        SIZE repoExtent={0,0},middleExtent={0,0},conusExtent={0,0};
        if(!GetTextExtentPoint32W(
                measure.get(),cache.footer.repo.c_str(),
                static_cast<int>(cache.footer.repo.size()),&repoExtent) ||
           !GetTextExtentPoint32W(
                measure.get(),cache.footer.middle.c_str(),
                static_cast<int>(cache.footer.middle.size()),&middleExtent) ||
           !GetTextExtentPoint32W(
                measure.get(),cache.footer.conus.c_str(),
                static_cast<int>(cache.footer.conus.size()),&conusExtent))
            return false;
        if(!BuildPickerFooterLayout(
            clientWidth,clientHeight,PAD,FOOTER_H,FOOTER_LINK_H,
            repoExtent.cx,middleExtent.cx,conusExtent.cx,
            cache.footer.layout)) return false;
        for(size_t tileIndex=0;tileIndex<tiles.size();++tileIndex){
            Tile& tile=tiles[tileIndex];
            RECT name=tile.rc;
            name.left+=S(14);
            name.top+=S(10);
            name.right-=S(12);
            name.bottom=name.top+S(22);
            const int rowHeight=S(22);
            const int listTop=name.bottom+S(6);
            const int listBottom=tile.rc.bottom-S(10);
            const int visibleRows=PickerTileVisibleRows(tile);
            const int maximumScroll=PickerTileMaxScroll(tile);
            const int visibleScroll=PickerVisibleScroll(
                tile.scroll,maximumScroll);
            if(tile.scroll!=visibleScroll){
                tile.scroll=visibleScroll;
                const auto saved=state.scrollByDesktop.find(tile.guidKey);
                if(saved==state.scrollByDesktop.end())
                    state.scrollByDesktop.emplace(
                        tile.guidKey,visibleScroll);
                else
                    saved->second=visibleScroll;
            }
            const bool hasScroll=
                PickerTileFilteredCount(tile)>visibleRows;
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
                PickerRowHitSnapshot& snapshot=row.snapshot;
                snapshot.hitRect={
                    tile.rc.left+S(8),y-S(2),
                    rowRight,y+S(20)
                };
                snapshot.textRect={
                    tile.rc.left+S(38),y,
                    rowRight,y+S(18)
                };
                snapshot.fullTitle=window.title;
                snapshot.runtimeKey=window.runtimeKey;
                snapshot.truncated=
                    extent.cx>(text.right-text.left);
                snapshot.action.tileIndex=static_cast<int>(tileIndex);
                snapshot.action.windowIndex=windowIndex;
                snapshot.action.hwnd=
                    reinterpret_cast<uintptr_t>(window.hwnd);
                snapshot.action.displayedDesktop=
                    window.displayedDesktop;
                snapshot.action.observedDesktop=window.observedDesktop;
                snapshot.action.baseDesktop=window.baseDesktop;
                snapshot.action.identity=window.identity;
                snapshot.action.admission=window.admission;
                snapshot.action.desktopRoute=window.desktopRoute;
                snapshot.action.mobility=window.mobility;
                snapshot.action.visuallyAssigned=
                    window.visuallyAssigned;
                snapshot.action.modelGeneration=
                    state.modelGeneration;
                snapshot.action.rowLayoutEpoch=
                    state.rowLayoutEpoch;
                snapshot.action.paintGeneration=
                    state.paintGeneration;
                cache.hoverRows.push_back(std::move(row));
                y+=rowHeight;
            }
        }
        return true;
    } catch(...) {
        return false;
    }
}

static bool RebuildPickerPaintCache(int clientWidth,int clientHeight,
                                    uint64_t generation) noexcept {
    try {
        std::vector<Tile> stagedTiles=g_tiles;
        PickerPaintCache stagedCache;
        PickerState stagedState=PreservePickerUi(g_picker);
        stagedState.paintGeneration=generation;
        if(!BuildPickerPaintCache(
                stagedCache,stagedTiles,stagedState,
                clientWidth,clientHeight,generation))
            return false;
        bool geometryChanged=stagedTiles.size()!=g_tiles.size();
        for(size_t index=0;!geometryChanged &&
                index<stagedTiles.size();++index){
            const RECT& left=stagedTiles[index].rc;
            const RECT& right=g_tiles[index].rc;
            geometryChanged=left.left!=right.left ||
                left.top!=right.top || left.right!=right.right ||
                left.bottom!=right.bottom ||
                stagedTiles[index].scroll!=g_tiles[index].scroll;
        }
        if(geometryChanged){
            const uint64_t rowLayoutEpoch=
                AdvancePickerRowLayoutEpoch(stagedState);
            for(RowRec& row : stagedCache.hoverRows)
                row.snapshot.action.rowLayoutEpoch=rowLayoutEpoch;
        }
        g_tiles.swap(stagedTiles);
        g_pickerPaintCache.swap(stagedCache);
        SwapPickerState(g_picker,stagedState);
        PublishPickerGridMetrics(g_tiles.size(),clientWidth);
        ResetPickerHoverState(PickerHoverResetReason::CachePublication);
        return true;
    } catch(...) {
        return false;
    }
}

static void InvalidatePublishedPickerPaintCache() noexcept {
    InvalidatePickerPaintCacheState(
        g_picker,g_pickerPaintCache,
        []() noexcept {
            ResetPickerHoverState(
                PickerHoverResetReason::ExplicitInvalidation);
        });
}

static bool RefreshPickerPaintCache(
        bool allowHiddenPreparation) noexcept {
    if(PickerInteractionBusy(g_picker,g_pickerGesture)) return false;
    const uint64_t generation=
        g_picker.paintGeneration==
            (std::numeric_limits<uint64_t>::max)()
        ?1:g_picker.paintGeneration+1;
    if(!g_main){
        return false;
    }
    RECT client={0,0,0,0};
    if(!GetClientRect(g_main,&client)){
        return false;
    }
    if(IsWindowVisible(g_main) || allowHiddenPreparation)
        PreloadVisiblePickerIcons();
    else
        CancelPickerIconPreload(g_main);
    return RebuildPickerPaintCache(
        client.right,client.bottom,generation);
}

struct PickerLightweightSnapshot {
    GUID currentDesktop={0};
    PickerReadValidity currentValidity=PickerReadValidity::Unavailable;
    PickerForegroundObservation foregroundObservation=
        PickerForegroundObservation::Unavailable;
    WindowIdentityKey foreground;
    PickerIdentityValidity activeIdentity=PickerIdentityValidity::Unknown;
    std::wstring cachedTitle;
    bool popupVisible=false;
};

static bool RefreshPickerHighlightsLightweight() noexcept {
    if(PickerInteractionBusy(g_picker,g_pickerGesture)) return false;
    PickerLightweightSnapshot adopted;
    PickerLightweightActiveUpdate activeUpdate=
        PickerLightweightActiveUpdate::Preserved;
    const bool refreshed=RunPickerLightweightRefresh(
        g_picker,
        [](){
            PickerLightweightSnapshot snapshot;
            snapshot.currentDesktop=CurrentDesktopGuid();
            if(!GuidIsZero(snapshot.currentDesktop))
                snapshot.currentValidity=PickerReadValidity::Valid;
            snapshot.popupVisible=g_main && IsWindowVisible(g_main)!=FALSE;
            if(SameIdentity(
                    g_picker.activeWindow,g_picker.activeWindow)){
                switch(RecaptureGenericWindowIdentity(
                        g_picker.activeWindow)){
                case WindowIdentityRecapture::Match:
                    snapshot.activeIdentity=PickerIdentityValidity::Match;
                    break;
                case WindowIdentityRecapture::Lost:
                    snapshot.activeIdentity=PickerIdentityValidity::Lost;
                    break;
                case WindowIdentityRecapture::Indeterminate:
                    snapshot.activeIdentity=
                        PickerIdentityValidity::Indeterminate;
                    break;
                }
            }
            const HWND foreground=GetForegroundWindow();
            if(foreground==g_main){
                snapshot.foregroundObservation=
                    PickerForegroundObservation::Popup;
            } else if(foreground){
                snapshot.foreground=CapturePickerWindowIdentity(foreground);
                if(SameIdentity(snapshot.foreground,snapshot.foreground)){
                    snapshot.foregroundObservation=
                        PickerForegroundObservation::ValidExternal;
                    for(const Tile& tile : g_tiles){
                        for(const WinItem& item : tile.windows){
                            if(SameIdentity(
                                    item.identity,snapshot.foreground)){
                                snapshot.cachedTitle=item.title;
                                return snapshot;
                            }
                        }
                    }
                } else {
                    snapshot.foregroundObservation=
                        PickerForegroundObservation::UnusableExternal;
                }
            }
            return snapshot;
        },
        [&](const PickerLightweightSnapshot& snapshot,PickerState& staged){
            adopted=snapshot;
            activeUpdate=ApplyPickerLightweightHighlightSnapshot(
                staged,snapshot.currentValidity,snapshot.currentDesktop,
                snapshot.foregroundObservation,snapshot.foreground,
                snapshot.activeIdentity,
                reinterpret_cast<uintptr_t>(g_main),
                reinterpret_cast<uintptr_t>(g_main),snapshot.popupVisible);
            return true;
        });
    if(!refreshed) return false;
    if(activeUpdate==PickerLightweightActiveUpdate::Adopted){
        g_target=reinterpret_cast<HWND>(adopted.foreground.hwnd);
        g_targetTitle.swap(adopted.cachedTitle);
    } else if(activeUpdate==PickerLightweightActiveUpdate::Cleared){
        g_target=nullptr;
        g_targetTitle.clear();
    }
    const bool cacheReady=RefreshPickerPaintCache();
    if(g_main) InvalidateRect(g_main,nullptr,FALSE);
    return cacheReady;
}

static void ArmPickerIdleRefresh() noexcept {
    if(g_main && !PickerInteractionBusy(g_picker,g_pickerGesture) &&
       IsWindowVisible(g_main))
        SetTimer(g_main,TIMER_PICKER_TRANSITION,
                 PICKER_IDLE_REFRESH_MS,nullptr);
}

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

static GdiBuffer g_pickerBuffer;
static GdiBuffer g_pickerDragBuffer;

static bool PaintPickerDragPreview(HDC target,RECT client) noexcept {
    if(!target) return false;
    RECT raw={0,0,0,0};
    if(!CurrentPickerDragPreviewBounds(g_main,raw)) return false;
    const PickerDragPreviewBlit clipped=
        ResolvePickerDragPreviewBlit(raw,client);
    if(!clipped.visible) return false;

    const int width=g_pickerDragPreview.size.cx;
    const int height=g_pickerDragPreview.size.cy;
    if(!g_pickerDragBuffer.ensure(target,width,height)) return false;
    HDC scratch=g_pickerDragBuffer.get();
    if(!scratch) return false;

    const int savedScratch=SaveDC(scratch);
    if(savedScratch==0) return false;
    bool scratchPainted=false;
    do {
        HGDIOBJ brush=GetStockObject(DC_BRUSH);
        HGDIOBJ pen=GetStockObject(DC_PEN);
        if(!brush || !pen) break;
        HGDIOBJ oldBrush=SelectObject(scratch,brush);
        HGDIOBJ oldPen=SelectObject(scratch,pen);
        if(!oldBrush || oldBrush==HGDI_ERROR ||
           !oldPen || oldPen==HGDI_ERROR) break;
        if(SetDCBrushColor(
                scratch,BlendColor(CLR_TILE,CLR_HEAD,20))==CLR_INVALID ||
           SetDCPenColor(scratch,CLR_PASSIVE)==CLR_INVALID)
            break;
        if(!RoundRect(
                scratch,0,0,width,height,S(8),S(8)))
            break;
        if(SetBkMode(scratch,TRANSPARENT)==0 ||
           SetTextColor(scratch,CLR_TEXT)==CLR_INVALID)
            break;
        HGDIOBJ oldFont=SelectObject(scratch,g_fPI);
        if(!oldFont || oldFont==HGDI_ERROR) break;

        HICON icon=
            CachedWindowIcon(g_pickerDragPreview.runtimeKey);
        if(!icon || !DrawIconEx(
                scratch,S(6),S(2),icon,S(16),S(16),
                0,nullptr,DI_NORMAL))
            break;
        RECT text={S(30),S(1),width-S(4),height-S(1)};
        if(g_pickerDragPreview.fullTitle.empty() ||
           DrawTextW(
                scratch,g_pickerDragPreview.fullTitle.c_str(),-1,&text,
                DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_VCENTER)==0)
            break;
        scratchPainted=true;
    } while(false);
    const bool scratchRestored=
        RestoreDC(scratch,savedScratch)!=FALSE;
    if(!scratchPainted || !scratchRestored) return false;

    HRGN roundedClip=CreateRoundRectRgn(
        raw.left,raw.top,raw.right,raw.bottom,S(8),S(8));
    if(!roundedClip) return false;
    const int savedTarget=SaveDC(target);
    if(savedTarget==0){
        DeleteObject(roundedClip);
        return false;
    }
    const bool clipSelected=
        ExtSelectClipRgn(target,roundedClip,RGN_AND)!=ERROR;
    const int blendWidth=
        clipped.destination.right-clipped.destination.left;
    const int blendHeight=
        clipped.destination.bottom-clipped.destination.top;
    BLENDFUNCTION blend={AC_SRC_OVER,0,166,0};
    const bool blended=clipSelected && AlphaBlend(
        target,
        clipped.destination.left,clipped.destination.top,
        blendWidth,blendHeight,
        scratch,clipped.source.x,clipped.source.y,
        blendWidth,blendHeight,blend)!=FALSE;
    const bool targetRestored=
        RestoreDC(target,savedTarget)!=FALSE;
    const bool regionDeleted=DeleteObject(roundedClip)!=FALSE;
    return blended && targetRestored && regionDeleted;
}

static void Paint(HDC hdcReal,HDC hdc,RECT client){
    HBRUSH bg=CreateSolidBrush(CLR_BG); FillRect(hdc,&client,bg); DeleteObject(bg); SetBkMode(hdc,TRANSPARENT);
    HFONT fT=g_fPT, fN=g_fPN, fI=g_fPI, fX=g_fPX;   // cached (created in InitMetrics)
    bool ctrlHeld=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    const bool paintCacheReady=PickerPaintCacheMatches(
        g_picker,g_pickerPaintCache.generation);

    // subtle rounded outer border
    { HPEN p=CreatePen(PS_SOLID,1,CLR_BORDER); HPEN op=(HPEN)SelectObject(hdc,p); HBRUSH ob=(HBRUSH)SelectObject(hdc,(HBRUSH)GetStockObject(NULL_BRUSH));
      RoundRect(hdc,0,0,client.right-1,client.bottom-1,S(18),S(18)); SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(p); }

    // ---- search box (rounded) + clear (x) button ----
    RECT sb=SearchBoxRect(client.right);
    FillRoundRect(hdc, sb, S(12), CLR_SEARCH, g_picker.searchActive?CLR_ACTIVE:CLR_PASSIVE, g_picker.searchActive?S(2):S(1));   // active only after user clicks/types
    if(paintCacheReady && g_picker.searchActive && g_pickerPaintCache.clearButton.right>g_pickerPaintCache.clearButton.left){ // big × (no circle) — shown whenever the field is active
        SelectObject(hdc,fX); SetTextColor(hdc,CLR_HINT); RECT xr=g_pickerPaintCache.clearButton; DrawTextW(hdc,L"\x2715",-1,&xr,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }

    // ---- header: left title + right Ctrl+Click hint ----
    int headTop=SEARCH_H, headBot=SEARCH_H+HEADER;
    SelectObject(hdc,fI); SetTextColor(hdc,CLR_HINT);
    const wchar_t* hint=L"Ctrl+Click - Move current window to selected desktop";
    RECT hr={PAD,headTop,client.right-PAD,headBot}; DrawTextW(hdc,hint,-1,&hr,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(hdc,fT); SetTextColor(hdc,CLR_HEAD);
    const wchar_t* head=paintCacheReady
        ?(ctrlHeld?g_pickerPaintCache.moveHeader.c_str()
                  :g_pickerPaintCache.switchHeader.c_str())
        :L"";
    const int hintWidth=paintCacheReady?g_pickerPaintCache.hintWidth:0;
    RECT h2={PAD,headTop,client.right-PAD-hintWidth-S(24),headBot}; DrawTextW(hdc,head,-1,&h2,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_VCENTER);

    // ---- tiles ----
    const COLORREF currentTile=BlendColor(CLR_TILE,CLR_ACTIVE,48);
    const COLORREF activeRow=BlendColor(CLR_TILE,CLR_ACTIVE,72);
    const bool searching=!g_picker.searchText.empty();
    for(size_t tileIndex=0;tileIndex<g_tiles.size();++tileIndex){
        const Tile& t=g_tiles[tileIndex];
        const bool isCurrent=IsCurrentDesktop(g_picker,t.guid);
        const bool isSelected=IsSelectedDesktop(g_picker,t.guid);
        const bool isDropTarget=
            g_pickerGesture.phase==PickerPointerPhase::Dragging &&
            g_pickerGesture.dropTileIndex==static_cast<int>(tileIndex);
        const bool dim=searching && t.filtered.empty();
        COLORREF fill=PickerTileFill(
            CLR_TILE,currentTile,CLR_TILE_DIM,isCurrent,dim);
        if(isDropTarget) fill=BlendColor(fill,CLR_ACTIVE,64);
        FillRoundRect(hdc,t.rc,S(10),fill,
            PickerTileBorder(
                isSelected || isDropTarget,CLR_ACTIVE,CLR_PASSIVE),
            isDropTarget?S(3):(isSelected?S(2):S(1)));
        SelectObject(hdc,fN); SetTextColor(hdc,dim?CLR_DIM:CLR_HEAD);
        RECT nr=t.rc; nr.left+=S(14); nr.top+=S(10); nr.right-=S(12); nr.bottom=nr.top+S(22);
        DrawTextW(hdc,t.displayName.c_str(),-1,&nr,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,fI); SetTextColor(hdc,CLR_TEXT);
        int listTop=nr.bottom+S(6), listBot=t.rc.bottom-S(10);
        int visRows=PickerTileVisibleRows(t); int maxScroll=PickerTileMaxScroll(t);
        const int visibleScroll=PickerVisibleScroll(t.scroll,maxScroll);
        const int filteredCount=PickerTileFilteredCount(t);
        bool hasScroll=filteredCount>visRows;
        if(paintCacheReady)
        for(size_t rowIndex=0;
                rowIndex<g_pickerPaintCache.hoverRows.size();
                ++rowIndex){
            const RowRec& row=g_pickerPaintCache.hoverRows[rowIndex];
            const PickerRowHitSnapshot& snapshot=row.snapshot;
            if(snapshot.action.tileIndex!=static_cast<int>(tileIndex))
                continue;
            const bool active=PickerRowUsesStableIdentity(
                    snapshot.action.admission) &&
                IsActiveWindow(g_picker,snapshot.action.identity);
            const bool hovered=PickerRowHoverMatches(
                g_pickerHoverState,static_cast<int>(rowIndex),
                g_pickerPaintCache.generation);
            if(hovered && !active){
                const COLORREF hoverRow=
                    BlendColor(fill,CLR_HEAD,24);
                FillRoundRect(
                    hdc,snapshot.hitRect,S(5),
                    hoverRow,hoverRow,S(1));
            }
            if(active){
                RECT activeRect=row.snapshot.hitRect;
                FillRoundRect(hdc,activeRect,S(5),activeRow,activeRow,S(1));
                RECT activeBar={activeRect.left+S(2),activeRect.top+S(3),
                                activeRect.left+S(5),activeRect.bottom-S(3)};
                FillRoundRect(hdc,activeBar,S(2),CLR_ACTIVE,CLR_ACTIVE,S(1));
            }
            HICON icon=PickerRowUsesStableIdentity(
                    snapshot.action.admission)
                ? CachedWindowIcon(snapshot.runtimeKey)
                :g_sharedFallbackIcon;
            if(icon) DrawIconEx(
                hdc,row.snapshot.hitRect.left+S(6),
                row.snapshot.textRect.top,icon,S(16),S(16),
                0,nullptr,DI_NORMAL);
            RECT textRect=row.snapshot.textRect;
            DrawTextW(
                hdc,row.snapshot.fullTitle.c_str(),-1,&textRect,
                DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        if(hasScroll){                                   // custom rounded scrollbar
            RECT trk={t.rc.right-S(9),listTop,t.rc.right-S(5),listBot}; FillRoundRect(hdc,trk,S(2),CLR_SCROLL_TRK,CLR_SCROLL_TRK,1);
            int trkH=listBot-listTop, thbH=std::max(S(24),trkH*visRows/std::max(1,filteredCount));
            int thbY=listTop+(maxScroll>0?(trkH-thbH)*visibleScroll/maxScroll:0);
            RECT thb={t.rc.right-S(9),thbY,t.rc.right-S(5),thbY+thbH}; FillRoundRect(hdc,thb,S(2),CLR_SCROLL_THB,CLR_SCROLL_THB,1);
        }
    }
    if(paintCacheReady){
        const PickerFooterLayout& footer=
            g_pickerPaintCache.footer.layout;
        SelectObject(hdc,fI);
        SetTextColor(hdc,g_pickerHoverState.footerLink==PickerFooterLink::Repository
            ?CLR_HEAD:CLR_ACTIVE);
        TextOutW(hdc,footer.repoText.x,footer.repoText.y,
            g_pickerPaintCache.footer.repo.c_str(),
            static_cast<int>(g_pickerPaintCache.footer.repo.size()));
        SetTextColor(hdc,CLR_HINT);
        TextOutW(hdc,footer.middleText.x,footer.middleText.y,
            g_pickerPaintCache.footer.middle.c_str(),
            static_cast<int>(g_pickerPaintCache.footer.middle.size()));
        SetTextColor(hdc,g_pickerHoverState.footerLink==PickerFooterLink::ConusVision
            ?CLR_HEAD:CLR_ACTIVE);
        TextOutW(hdc,footer.conusText.x,footer.conusText.y,
            g_pickerPaintCache.footer.conus.c_str(),
            static_cast<int>(g_pickerPaintCache.footer.conus.size()));
    }
    (void)PaintPickerDragPreview(hdc,client);
    if(hdc!=hdcReal)
        BitBlt(hdcReal,0,0,client.right,client.bottom,hdc,0,0,SRCCOPY);
}
static void TipDeactivate() noexcept {
    if(!g_tip) return;
    TOOLINFOW ti={0};
    ti.cbSize=sizeof(ti);
    ti.hwnd=g_main;
    ti.uId=1;
    SendMessageW(g_tip,TTM_TRACKACTIVATE,FALSE,(LPARAM)&ti);
    ti.lpszText=const_cast<LPWSTR>(L"");
    SendMessageW(g_tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&ti);
}
static void ResetPickerHoverTooltip() noexcept {
    TipDeactivate();
    g_lastHoverRow=-1;
    g_lastHoverGeneration=0;
    g_pickerTooltipText.clear();
    g_pickerHoverState.rowTooltipActive=false;
}
static void TrackPickerHoverTooltip(HWND hwnd,const RowRec& row,
                                    int rowIndex,uint64_t generation,
                                    POINT clientPoint) noexcept {
    if(!g_tip) return;
    if(!PickerHoverPairMatches(
            g_lastHoverRow,g_lastHoverGeneration,
            rowIndex,generation)){
        ResetPickerHoverTooltip();
        try {
            g_pickerTooltipText=row.snapshot.fullTitle;
        } catch(...) {
            return;
        }
        TOOLINFOW ti={0};
        ti.cbSize=sizeof(ti);
        ti.hwnd=hwnd;
        ti.uId=1;
        ti.lpszText=const_cast<LPWSTR>(
            g_pickerTooltipText.c_str());
        SendMessageW(g_tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&ti);
        SendMessageW(g_tip,TTM_TRACKACTIVATE,TRUE,(LPARAM)&ti);
        g_lastHoverRow=rowIndex;
        g_lastHoverGeneration=generation;
        g_pickerHoverState.rowTooltipActive=true;
    }
    POINT screenPoint=clientPoint;
    ClientToScreen(hwnd,&screenPoint);
    SendMessageW(g_tip,TTM_TRACKPOSITION,0,
        (LPARAM)MAKELONG(screenPoint.x+S(16),screenPoint.y+S(20)));
}
static void EndPickerVisualSessionRuntime() noexcept {
    const bool releaseCapture=
        g_main && GetCapture()==g_main;
    ClearPickerDragPreview();
    CancelPickerRowGesture(g_pickerGesture);
    if(releaseCapture)
        ReleaseCapture();
    EndPickerVisualSession(g_picker);
}

static void HidePicker(PickerHideDisposition disposition) noexcept {
    if(g_picker.controlledTransition()) return;
    if(PickerHideEndsVisualSession(disposition))
        EndPickerVisualSessionRuntime();
    else
        ResetPickerPointerGesture(g_main,true);
    if(g_main) KillTimer(g_main,TIMER_PICKER_TRANSITION);
    CancelPickerIconPreload(g_main);
    ResetPickerHoverState(PickerHoverResetReason::Hide);
    ShowWindow(g_main,SW_HIDE);
}
// Search EDIT subclass: forward navigation keys to the grid; let letters/numbers type.
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp){
    if(PickerInteractionBusy(g_picker,g_pickerGesture)){
        if((m==WM_CHAR && wp==L' ') ||
           (m==WM_KEYUP && wp==VK_SPACE) || m==WM_KILLFOCUS)
            g_suppressPickerCtrlSpaceChar=false;
        if(m==WM_KEYDOWN && wp==VK_ESCAPE)
            SendMessageW(g_main,WM_KEYDOWN,wp,lp);
        if(PickerControlledEditMessageAllowed(m))
            return CallWindowProcW(g_searchOrigProc,h,m,wp,lp);
        return 0;
    }
    if((m==WM_KEYDOWN||m==WM_KEYUP)&&wp==VK_CONTROL){ InvalidateRect(g_main,nullptr,FALSE); return 0; }
    if((m==WM_KEYUP && wp==VK_SPACE) || m==WM_KILLFOCUS)
        g_suppressPickerCtrlSpaceChar=false;
    const bool controlDown=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    const bool suppressSpaceChar=m==WM_CHAR && wp==L' ' &&
        g_suppressPickerCtrlSpaceChar;
    const PickerIdleEditInputRoute inputRoute=RoutePickerIdleEditInput(
        m,wp,controlDown || suppressSpaceChar);
    if(inputRoute==PickerIdleEditInputRoute::Grid){
        if(m==WM_KEYDOWN && wp==VK_SPACE && controlDown)
            g_suppressPickerCtrlSpaceChar=true;
        SendMessageW(g_main,WM_KEYDOWN,wp,lp);
        return 0;
    }
    if(inputRoute==PickerIdleEditInputRoute::Swallow){
        if(m==WM_CHAR && wp==L' ')
            g_suppressPickerCtrlSpaceChar=false;
        return 0;
    }
    if(m==WM_CHAR){
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

static PickerIdentityValidity PickerIdentityObservation(
        WindowIdentityRecapture identity) noexcept {
    switch(identity){
    case WindowIdentityRecapture::Match:
        return PickerIdentityValidity::Match;
    case WindowIdentityRecapture::Lost:
        return PickerIdentityValidity::Lost;
    case WindowIdentityRecapture::Indeterminate:
        return PickerIdentityValidity::Indeterminate;
    }
    return PickerIdentityValidity::Indeterminate;
}

static void EmitPickerTraceHResult(
        PickerTraceApiKind api,uint64_t generation,uint64_t effectSerial,
        HRESULT result,bool invoked,HWND hwnd,
        const GUID& requested,
        const GUID& actual) noexcept {
    PickerTraceApiResultEvent event;
    event.api=api;
    event.resultKind=PickerTraceRawResultKind::HResult;
    event.generation=generation;
    event.effectSerial=effectSerial;
    event.hwnd=reinterpret_cast<uintptr_t>(hwnd);
    event.requestedDesktop=requested;
    event.actualDesktop=actual;
    event.hresult=result;
    event.invoked=invoked;
    g_pickerTrace.emit(event);
}

struct PickerTraceWindowDesktopFacts {
    bool invoked=false;
    HRESULT result=E_NOINTERFACE;
    GUID actual{};
    PickerReadValidity validity=PickerReadValidity::Unavailable;
};

static void ReadPickerWindowDesktop(
        HWND hwnd,PickerReadValidity& validity,GUID& desktop,
        PickerTraceWindowDesktopFacts* facts=nullptr) noexcept {
    if(facts) *facts=PickerTraceWindowDesktopFacts{};
    validity=PickerReadValidity::Unavailable;
    desktop=GUID{};
    if(!hwnd || !g_vdmDoc) return;
    GUID observed={0};
    HRESULT result=E_FAIL;
    if(facts) facts->invoked=true;
    try { result=g_vdmDoc->GetWindowDesktopId(hwnd,&observed); }
    catch(...) { result=E_FAIL; }
    if(facts){
        facts->result=result;
        facts->actual=observed;
    }
    if(SUCCEEDED(result) && !GuidIsZero(observed)){
        desktop=observed;
        validity=PickerReadValidity::Valid;
        if(facts) facts->validity=PickerReadValidity::Valid;
    }
}

static void EmitPickerTraceWindowDesktopFacts(
        PickerTraceApiKind api,HWND hwnd,uint64_t generation,
        uint64_t effectSerial,
        const PickerTraceWindowDesktopFacts& facts) noexcept {
    EmitPickerTraceHResult(
        api,generation,effectSerial,facts.result,
        facts.invoked,hwnd,GUID{},facts.actual);
}

static void EmitPickerTraceCurrentDesktopFacts(
        uint64_t generation,uint64_t effectSerial,
        const PickerTraceCurrentDesktopFacts& facts) noexcept {
    EmitPickerTraceHResult(
        PickerTraceApiKind::GetCurrentDesktop,
        generation,effectSerial,facts.currentResult,
        facts.currentInvoked,nullptr,GUID{},facts.actual);
    if(facts.idInvoked){
        EmitPickerTraceHResult(
            PickerTraceApiKind::GetId,generation,effectSerial,
            facts.idResult,true,nullptr,GUID{},facts.actual);
    }
}

static bool RefreshPickerModelPreservingUi(
        bool selectionFromActual=true) noexcept {
    try {
        std::wstring editText=g_picker.searchEditText;
        if(g_search){
            const int length=GetWindowTextLengthW(g_search);
            if(length>=0){
                std::wstring current(static_cast<size_t>(length)+1,L'\0');
                const int copied=GetWindowTextW(
                    g_search,&current[0],length+1);
                if(copied>=0){
                    current.resize(static_cast<size_t>(copied));
                    editText.swap(current);
                }
            }
        }
        for(Tile& tile : g_tiles)
            RememberPickerScroll(tile,tile.scroll);
        std::wstring normalized=LowerW(editText);
        if(!SetPickerSearchText(g_picker,editText,normalized)) return false;
        if(!BuildModel(g_picker.activeWindow,false,selectionFromActual))
            return false;
        std::vector<GUID> live;
        live.reserve(g_tiles.size());
        for(const Tile& tile : g_tiles) live.push_back(tile.guid);
        if(!PrunePickerScrollState(g_picker,live)) return false;
        if(g_search)
            SetWindowTextW(g_search,g_picker.searchEditText.c_str());
        InvalidatePickerTabSearchCache(
            g_pickerTabSearchCache,g_picker.modelGeneration);
        if(!g_picker.searchText.empty()) EnsureTabSearch();
        return true;
    } catch(...) { return false; }
}

struct PickerForegroundHandoffProductContext {
    IVirtualDesktop* desktop=nullptr;
};

static BOOL PickerProductAttachThreadInput(
        void*,DWORD source,DWORD destination,BOOL attach) noexcept {
    return AttachThreadInput(source,destination,attach);
}

static BOOL PickerProductSetForegroundWindow(
        void*,HWND hwnd) noexcept {
    return SetForegroundWindow(hwnd);
}

static HRESULT PickerProductSwitchDesktop(void* opaque) {
    auto* context=
        static_cast<PickerForegroundHandoffProductContext*>(opaque);
    return g_vdmi && context && context->desktop
        ? g_vdmi->SwitchDesktop(context->desktop) : E_NOINTERFACE;
}

static BOOL PickerProductShowWindow(
        void*,HWND hwnd,int command) noexcept {
    return ShowWindow(hwnd,command);
}

struct PickerProductApiTraceContext {
    uint64_t generation=0;
    uint64_t effectSerial=0;
    HWND progman=nullptr;
    HWND foreground=nullptr;
    GUID requested{};
};

static void EmitPickerProductApiTrace(
        void* opaque,const PickerTraceApiResultEvent& input) noexcept {
    PickerTraceApiResultEvent event=input;
    const auto* context=static_cast<PickerProductApiTraceContext*>(opaque);
    if(context){
        event.generation=context->generation;
        event.effectSerial=context->effectSerial;
        event.requestedDesktop=context->requested;
        switch(event.api){
        case PickerTraceApiKind::AttachForegroundInput:
        case PickerTraceApiKind::DetachForegroundInput:
            event.hwnd=reinterpret_cast<uintptr_t>(context->foreground);
            break;
        case PickerTraceApiKind::AttachDesktopInput:
        case PickerTraceApiKind::DetachDesktopInput:
            event.hwnd=reinterpret_cast<uintptr_t>(context->progman);
            break;
        default:
            break;
        }
    }
    g_pickerTrace.emit(event);
}

static HRESULT SwitchDesktopWithForegroundHandoff(
        const GUID& destinationGuid,bool& invoked,
        uint64_t generation=0,uint64_t effectSerial=0) noexcept {
    invoked=false;
    if(GuidIsZero(destinationGuid) || !g_vdmi) return E_INVALIDARG;
    PickerTraceDesktopLookupContext lookupContext;
    lookupContext.trace=&g_pickerTrace;
    lookupContext.use=
        PickerTraceDesktopLookupUse::SwitchHandoffDestination;
    lookupContext.generation=generation;
    lookupContext.effectSerial=effectSerial;
    lookupContext.requested=destinationGuid;
    ScopedComPtr<IVirtualDesktop> desktop;
    try { desktop=GetDesktopByGuid(destinationGuid,&lookupContext); }
    catch(...) { return E_FAIL; }
    if(!desktop) return E_INVALIDARG;

    HWND prog=FindWindowW(L"Progman",L"Program Manager");
    DWORD ignored=0;
    const DWORD desktopThread=prog
        ?GetWindowThreadProcessId(prog,&ignored):0;
    const HWND foreground=GetForegroundWindow();
    const DWORD foregroundThread=
        GetWindowThreadProcessId(foreground,&ignored);
    const DWORD currentThread=GetCurrentThreadId();
    const PickerForegroundHandoffPlan plan=PlanPickerForegroundHandoff(
        prog!=nullptr,desktopThread,foregroundThread,currentThread);
    PickerForegroundHandoffProductContext productContext;
    productContext.desktop=desktop.get();
    PickerTraceForegroundHandoffOps ops;
    ops.context=&productContext;
    ops.attachThreadInput=PickerProductAttachThreadInput;
    ops.setForegroundWindow=PickerProductSetForegroundWindow;
    ops.switchDesktop=PickerProductSwitchDesktop;
    ops.showWindow=PickerProductShowWindow;
    PickerProductApiTraceContext traceContext;
    traceContext.generation=generation;
    traceContext.effectSerial=effectSerial;
    traceContext.progman=prog;
    traceContext.foreground=foreground;
    traceContext.requested=destinationGuid;
    PickerTraceApiEventObserver observer;
    observer.context=&traceContext;
    observer.emit=EmitPickerProductApiTrace;
    const PickerTraceForegroundHandoffResult result=
        ExecutePickerForegroundHandoffCalls(
            plan,prog,desktopThread,foregroundThread,currentThread,
            ops,g_pickerTrace.active()?&observer:nullptr);
    invoked=result.invoked;
    return result.switchResult;
}

static void InvalidatePickerExactRowHit(
        const WindowIdentityKey& identity) noexcept {
    const uint64_t rowLayoutEpoch=
        AdvancePickerRowLayoutEpoch(g_picker);
    for(RowRec& row : g_pickerPaintCache.hoverRows){
        row.snapshot.action.rowLayoutEpoch=rowLayoutEpoch;
        if(!SameIdentity(row.snapshot.action.identity,identity))
            continue;
        row.snapshot.action.hwnd=0;
        row.snapshot.action.identity=WindowIdentityKey{};
        row.snapshot.action.admission=PickerRowAdmission::DisplayOnly;
        row.snapshot.action.mobility=TargetMobility::Indeterminate;
    }
}

static PickerObservation ExecutePickerEffect(
        const PickerEffect& effect,
        bool* caughtException=nullptr) noexcept {
    if(caughtException) *caughtException=false;
    PickerObservation observation;
    observation.generation=effect.generation;
    observation.effectKind=effect.kind;
    observation.effectSerial=effect.effectSerial;
    observation.event=PickerEvent::EffectCompleted;
    try {
        switch(effect.kind){
        case PickerEffectKind::MoveTarget: {
            observation.event=PickerEvent::ApiCompleted;
            const TargetMoveIssueResult issued=IssueGuardedTargetMove(
                g_picker.transition.target,
                TargetDesktopRoute::Indeterminate,effect.desktop,
                PickerTraceDesktopLookupUse::MoveTargetDestination,
                effect.generation,effect.effectSerial);
            observation.identity=
                PickerIdentityObservation(issued.identity);
            observation.apiInvoked=issued.invoked;
            observation.apiAccepted=SUCCEEDED(issued.result);
            break;
        }
        case PickerEffectKind::ReadTarget: {
            observation.event=PickerEvent::ReadbackCompleted;
            const WindowIdentityRecapture identity=
                RecaptureGenericWindowIdentity(g_picker.transition.target);
            observation.identity=PickerIdentityObservation(identity);
            PickerTraceWindowDesktopFacts facts;
            if(identity==WindowIdentityRecapture::Match)
                ReadPickerWindowDesktop(
                    reinterpret_cast<HWND>(g_picker.transition.target.hwnd),
                    observation.targetRead,
                    observation.actualTargetDesktop,&facts);
            else
                observation.targetRead=PickerReadValidity::Unavailable;
            EmitPickerTraceWindowDesktopFacts(
                PickerTraceApiKind::GetWindowDesktopIdTarget,
                reinterpret_cast<HWND>(g_picker.transition.target.hwnd),
                effect.generation,effect.effectSerial,facts);
            break;
        }
        case PickerEffectKind::ValidateTarget:
            observation.identity=PickerIdentityObservation(
                RecaptureGenericWindowIdentity(g_picker.transition.target));
            break;
        case PickerEffectKind::MovePopup: {
            observation.event=PickerEvent::ApiCompleted;
            if(g_picker.transition.popupRoute!=PickerPopupRoute::Managed)
                break;
            bool invoked=false;
            const HRESULT result=IssuePickerPopupMove(
                g_main,effect.desktop,invoked,
                PickerTraceDesktopLookupUse::MovePopupDestination,
                effect.generation,effect.effectSerial);
            observation.apiInvoked=invoked;
            observation.apiAccepted=SUCCEEDED(result);
            break;
        }
        case PickerEffectKind::ReadPopup: {
            observation.event=PickerEvent::ReadbackCompleted;
            if(g_picker.transition.popupRoute!=PickerPopupRoute::Managed)
                break;
            PickerTraceWindowDesktopFacts facts;
            ReadPickerWindowDesktop(
                g_main,observation.popupRead,
                observation.actualPopupDesktop,&facts);
            EmitPickerTraceWindowDesktopFacts(
                PickerTraceApiKind::GetWindowDesktopIdPopup,g_main,
                effect.generation,effect.effectSerial,facts);
            break;
        }
        case PickerEffectKind::SwitchDesktop: {
            observation.event=PickerEvent::ApiCompleted;
            HRESULT result=E_INVALIDARG;
            PickerTraceDesktopLookupContext lookupContext;
            lookupContext.trace=&g_pickerTrace;
            lookupContext.use=
                PickerTraceDesktopLookupUse::SwitchPrecheckDestination;
            lookupContext.generation=effect.generation;
            lookupContext.effectSerial=effect.effectSerial;
            lookupContext.requested=effect.desktop;
            ScopedComPtr<IVirtualDesktop> desktop=
                GetDesktopByGuid(effect.desktop,&lookupContext);
            const bool rollback=
                g_picker.transition.phase==
                    PickerPhase::RollbackSwitchIssue;
            if(rollback){
                observation.identity=PickerIdentityValidity::Match;
            } else {
                observation.identity=PickerIdentityObservation(
                    RecaptureGenericWindowIdentity(
                        g_picker.transition.target));
            }
            if(PickerForwardSwitchInvocationAllowed(
                    observation.identity,desktop && g_vdmi) ||
               (rollback && desktop && g_vdmi)){
                bool invoked=false;
                result=SwitchDesktopWithForegroundHandoff(
                    effect.desktop,invoked,
                    effect.generation,effect.effectSerial);
                observation.apiInvoked=invoked;
            }
            observation.apiAccepted=SUCCEEDED(result);
            break;
        }
        case PickerEffectKind::ReadCurrent: {
            observation.event=PickerEvent::ReadbackCompleted;
            PickerTraceCurrentDesktopFacts facts;
            const GUID current=CurrentDesktopGuid(&facts);
            EmitPickerTraceCurrentDesktopFacts(
                effect.generation,effect.effectSerial,facts);
            if(!GuidIsZero(current)){
                observation.currentRead=PickerReadValidity::Valid;
                observation.actualCurrentDesktop=current;
            } else {
                observation.currentRead=PickerReadValidity::Unavailable;
            }
            break;
        }
        case PickerEffectKind::SaveExactTarget: {
            const PopupSaveResult saved=
                SavePopupMovedWindow(g_picker.transition);
            observation.saveStatus=saved.status;
            observation.saveFailure=saved.failure;
            observation.identity=PickerIdentityObservation(
                RecaptureGenericWindowIdentity(g_picker.transition.target));
            CompletePickerLifecycleForSave(saved,[&](const std::string& app){
                if(app!=g_picker.transition.app ||
                   g_picker.transition.lifecycleSaveGeneration==0) return;
                auto lifecycle=g_lifecycleByApp.find(app);
                if(lifecycle!=g_lifecycleByApp.end())
                    LcExplicitSaveCompleted(
                        lifecycle->second,
                        g_picker.transition.lifecycleSaveGeneration,
                        g_picker.transition.lifecycleLayoutSignature,
                        g_picker.transition.lifecycleSessionSignature,
                        MonotonicNowMs());
            });
            break;
        }
        case PickerEffectKind::PublishVisualAssignment:
            observation.apiAccepted=BuildModel(
                g_picker.activeWindow,false,true,nullptr,nullptr,
                g_picker.transition.visualMutation);
            if(observation.apiAccepted)
                InvalidateRect(g_main,nullptr,FALSE);
            break;
        case PickerEffectKind::Refresh:
            observation.apiAccepted=RefreshPickerModelPreservingUi(
                g_picker.transition.mode!=
                    PickerTransitionMode::RowMoveOnly);
            if(observation.apiAccepted)
                InvalidateRect(g_main,nullptr,FALSE);
            else if(g_picker.transition.mode==
                        PickerTransitionMode::RowMoveOnly &&
                    g_picker.transition.failed)
                InvalidatePickerExactRowHit(
                    g_picker.transition.target);
            break;
        case PickerEffectKind::ShowAndFocus:
            if(!g_picker.transition.dismissed){
                ShowWindow(g_main,SW_SHOW);
                SetWindowPos(g_main,HWND_TOPMOST,0,0,0,0,
                             SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
                SetForegroundWindow(g_main);
                if(g_search) SetFocus(g_search);
            }
            observation.popupIsForeground=
                GetForegroundWindow()==g_main;
            break;
        case PickerEffectKind::Hide:
            CancelPickerIconPreload(g_main);
            ResetPickerHoverState(PickerHoverResetReason::Hide);
            if(PickerHideEndsVisualSession(effect.hideDisposition))
                EndPickerVisualSessionRuntime();
            ShowWindow(g_main,SW_HIDE);
            observation.apiAccepted=true;
            break;
        case PickerEffectKind::ReportFailure:
            Balloon(g_picker.transition.diagnostic.empty()
                ? L"The picker transition could not be completed safely."
                : g_picker.transition.diagnostic);
            observation.apiAccepted=true;
            break;
        case PickerEffectKind::None:
            break;
        }
    } catch(...) {
        observation.apiAccepted=false;
        if(caughtException) *caughtException=true;
    }
    return observation;
}

static void PumpPickerTransitionWork() noexcept;

static uint32_t NextPickerTraceDeliveryAttempt(
        const PickerEffect& effect) noexcept {
    if(effect.generation!=g_pickerTraceDeliveryGeneration ||
       effect.effectSerial!=g_pickerTraceDeliverySerial){
        g_pickerTraceDeliveryGeneration=effect.generation;
        g_pickerTraceDeliverySerial=effect.effectSerial;
        g_pickerTraceDeliveryAttempt=0;
    }
    if(g_pickerTraceDeliveryAttempt!=
       (std::numeric_limits<uint32_t>::max)())
        ++g_pickerTraceDeliveryAttempt;
    return g_pickerTraceDeliveryAttempt;
}

static void EmitPickerTraceEffectQueue(
        const PickerEffect& effect,
        const PickerTraceScheduleResult& schedule) noexcept {
    if(!g_pickerTrace.active()) return;
    PickerTraceEffectEvent event;
    event.stage=PickerTraceEffectStage::Queue;
    event.generation=effect.generation;
    event.effectSerial=effect.effectSerial;
    event.effect=effect.kind;
    event.delivery=schedule.route;
    event.deliveryAvailable=schedule.routeAvailable;
    event.deliveryAttempt=NextPickerTraceDeliveryAttempt(effect);
    g_pickerTrace.emit(event);
}

static void EmitPickerTraceEffectExecute(
        const PickerEffect& effect,
        PickerEffectExecutionRoute route) noexcept {
    if(!g_pickerTrace.active()) return;
    PickerTraceEffectEvent event;
    event.stage=PickerTraceEffectStage::Execute;
    event.generation=effect.generation;
    event.effectSerial=effect.effectSerial;
    event.effect=effect.kind;
    event.executionRoute=route;
    event.executionRouteAvailable=true;
    g_pickerTrace.emit(event);
}

static void EmitPickerTraceEffectObservation(
        const PickerEffect& effect,
        const PickerObservation& observation) noexcept {
    if(!g_pickerTrace.active()) return;
    PickerTraceEffectEvent event;
    event.stage=PickerTraceEffectStage::Observation;
    event.generation=effect.generation;
    event.effectSerial=effect.effectSerial;
    event.observationEvent=observation.event;
    event.effect=effect.kind;
    event.identity=observation.identity;
    event.targetRead=observation.targetRead;
    event.popupRead=observation.popupRead;
    event.currentRead=observation.currentRead;
    event.apiInvoked=observation.apiInvoked;
    event.apiAccepted=observation.apiAccepted;
    g_pickerTrace.emit(event);
}

static PickerTraceScheduleResult PickerTraceObservationDelivery(
        PickerKickRoute route) noexcept {
    PickerTraceScheduleResult result;
    result.routeAvailable=true;
    switch(route){
    case PickerKickRoute::Posted:
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::Posted;
        break;
    case PickerKickRoute::TimerArmed:
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::TimerArmed;
        break;
    case PickerKickRoute::InlineFallback:
        result.route=PickerTraceDeliveryRoute::InlineFallback;
        break;
    case PickerKickRoute::Teardown:
        result.route=PickerTraceDeliveryRoute::ShutdownDrain;
        break;
    case PickerKickRoute::PendingPreserved:
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::DurableExternalKick;
        break;
    }
    return result;
}

static void StagePickerScheduledEffect(
        const PickerEffect& effect) noexcept {
    g_pickerScheduledEffect=effect;
    g_pickerEffectScheduled=true;
    g_pickerEffectNotBeforeMs=PickerEffectRequiresSettlingDelay(effect.kind)
        ? PickerSettlingNotBeforeMs(
            MonotonicNowMs(),MOVE_VERIFY_INTERVAL_MS)
        : 0;
}

static PickerTraceScheduleResult DeferPickerTransitionWork(
        bool delayed) noexcept {
    uint64_t remaining=0;
    if(g_main && !g_pickerShutdownDrain && delayed)
        remaining=PickerSettlingDelayRemainingMs(
            MonotonicNowMs(),g_pickerEffectNotBeforeMs);
    const PickerTraceScheduleResult result=SchedulePickerTransitionWork(
        g_main!=nullptr,g_pickerShutdownDrain,remaining,
        [](UINT wait) noexcept {
            return SetTimer(
                g_main,TIMER_PICKER_TRANSITION,wait,nullptr)!=0;
        },[]() noexcept {
            return PostMessageW(
                g_main,WM_PICKER_TRANSITION,0,0)!=FALSE;
        });
    switch(result.route){
    case PickerTraceDeliveryRoute::Posted:
    case PickerTraceDeliveryRoute::TimerArmed:
    case PickerTraceDeliveryRoute::DelayedTimer:
        g_pickerDurableKickPending=false;
        break;
    case PickerTraceDeliveryRoute::DurableExternalKick:
        g_pickerDurableKickPending=true;
        break;
    default:
        break;
    }
    return result;
}

static void StorePickerTraceTerminalDelivery(
        const PickerTraceScheduleResult& schedule) noexcept {
    if(!g_pickerTrace.active() || !schedule.routeAvailable) return;
    StorePickerTracePendingTerminalDelivery(
        g_pickerTracePendingTerminalDelivery,
        g_picker.transition.generation,schedule.route);
}

static uint64_t NextPickerTraceTerminalizationAttempt(
        uint64_t generation) noexcept {
    if(generation!=g_pickerTraceTerminalizationGeneration){
        g_pickerTraceTerminalizationGeneration=generation;
        g_pickerTraceTerminalizationAttempt=0;
    }
    if(g_pickerTraceTerminalizationAttempt!=
       (std::numeric_limits<uint64_t>::max)())
        ++g_pickerTraceTerminalizationAttempt;
    return g_pickerTraceTerminalizationAttempt;
}

static void QueuePickerEffect(const PickerEffect& effect) noexcept {
    if(effect.kind==PickerEffectKind::None){
        if(g_picker.transition.terminalAcknowledged){
            g_pickerTerminalizationPending=true;
            const PickerTraceScheduleResult schedule=
                DeferPickerTransitionWork(false);
            StorePickerTraceTerminalDelivery(schedule);
            if(!schedule.deferred && !g_pickerPumpActive)
                PumpPickerTransitionWork();
        }
        return;
    }
    if(g_pickerEffectScheduled){
        g_picker.transition.failed=true;
        g_picker.transition.suppressFocus=true;
        g_picker.transition.terminalAcknowledged=true;
        g_pickerTerminalizationPending=true;
        ObservePickerTraceTerminalMetadata(
            g_pickerTraceTerminalMetadata,
            g_picker.transition.phase,g_picker.transition.phase,
            PickerObservation{},true,false);
        return;
    }
    StagePickerScheduledEffect(effect);
    const bool delayed=PickerEffectRequiresSettlingDelay(effect.kind);
    const PickerTraceScheduleResult schedule=
        DeferPickerTransitionWork(delayed);
    const PickerTraceQueueCallerDecision decision=
        DecidePickerTraceExternalQueue(schedule,g_pickerPumpActive);
    EmitPickerTraceEffectQueue(effect,decision.delivery);
    if(decision.runInlinePump)
        PumpPickerTransitionWork();
}

struct PickerRuntimeTerminalTraceSnapshot {
    PickerTraceTransitionTerminalEvent terminal;
    HWND target=nullptr;
    PickerPopupRoute popupRoute=PickerPopupRoute::Managed;
    uint64_t effectSerial=0;
};

struct PickerRuntimeTerminalizationResult {
    PickerTraceTerminalizationRunResult run;
    PickerRuntimeTerminalTraceSnapshot snapshot;
};

static PickerRuntimeTerminalTraceSnapshot
CapturePickerRuntimeTerminalTraceSnapshot() noexcept {
    PickerRuntimeTerminalTraceSnapshot result;
    const PickerTransition& transition=g_picker.transition;
    result.terminal.generation=transition.generation;
    result.terminal.outcome=transition.cancelRequested
        ? PickerTraceTerminalOutcome::Cancelled
        : transition.failed
            ? PickerTraceTerminalOutcome::Failed
            : PickerTraceTerminalOutcome::Succeeded;
    result.terminal.rollbackTrigger=
        g_pickerTraceTerminalMetadata.rollbackTrigger;
    result.terminal.diagnosticCode=
        g_pickerTraceTerminalMetadata.diagnosticCode;
    result.terminal.forwardTargetAttempts=
        transition.forwardTargetAttempts;
    result.terminal.forwardPopupAttempts=
        transition.forwardPopupAttempts;
    result.terminal.forwardSwitchAttempts=
        transition.forwardSwitchAttempts;
    result.terminal.rollbackTargetAttempts=
        transition.rollbackTargetAttempts;
    result.terminal.rollbackPopupAttempts=
        transition.rollbackPopupAttempts;
    result.terminal.rollbackSwitchAttempts=
        transition.rollbackSwitchAttempts;
    result.terminal.focusAttempts=transition.focusAttempts;
    result.terminal.targetRead=transition.observedTargetValidity;
    result.terminal.popupRead=transition.observedPopupValidity;
    result.terminal.currentRead=transition.observedCurrentValidity;
    result.terminal.targetDesktop=transition.observedTargetDesktop;
    result.terminal.popupDesktop=transition.observedPopupDesktop;
    result.terminal.currentDesktop=transition.observedCurrentDesktop;
    result.target=reinterpret_cast<HWND>(transition.target.hwnd);
    result.popupRoute=transition.popupRoute;
    result.effectSerial=transition.effectSerial;
    return result;
}

static void ReadPickerRuntimeTerminalTraceSnapshot(
        PickerRuntimeTerminalTraceSnapshot& snapshot) noexcept {
    if(!g_pickerTrace.active()) return;
    PickerTraceWindowDesktopFacts targetFacts;
    ReadPickerWindowDesktop(
        snapshot.target,snapshot.terminal.targetRead,
        snapshot.terminal.targetDesktop,&targetFacts);
    EmitPickerTraceWindowDesktopFacts(
        PickerTraceApiKind::GetWindowDesktopIdTarget,snapshot.target,
        snapshot.terminal.generation,snapshot.effectSerial,targetFacts);
    if(snapshot.popupRoute==PickerPopupRoute::Managed){
        PickerTraceWindowDesktopFacts popupFacts;
        ReadPickerWindowDesktop(
            g_main,snapshot.terminal.popupRead,
            snapshot.terminal.popupDesktop,&popupFacts);
        EmitPickerTraceWindowDesktopFacts(
            PickerTraceApiKind::GetWindowDesktopIdPopup,g_main,
            snapshot.terminal.generation,snapshot.effectSerial,popupFacts);
    }
    PickerTraceCurrentDesktopFacts currentFacts;
    snapshot.terminal.currentDesktop=CurrentDesktopGuid(&currentFacts);
    snapshot.terminal.currentRead=currentFacts.validity;
    EmitPickerTraceCurrentDesktopFacts(
        snapshot.terminal.generation,snapshot.effectSerial,currentFacts);
}

static void EmitPickerRuntimeTerminalizationAttempt(
        void* opaque,
        const PickerTraceTerminalizationAttemptEvent& event) noexcept {
    static_cast<PickerTraceSession*>(opaque)->emit(event);
}

static void EmitPickerRuntimeTerminalEvent(
        void* opaque,
        const PickerTraceTransitionTerminalEvent& event) noexcept {
    static_cast<PickerTraceSession*>(opaque)->emit(event);
}

static void FlushPickerRuntimeTerminalBoundary(void* opaque) noexcept {
    static_cast<PickerTraceSession*>(opaque)->flushBoundary();
}

static PickerTraceTerminalizationEventObserver
PickerRuntimeTerminalTraceObserver() noexcept {
    PickerTraceTerminalizationEventObserver observer;
    observer.context=&g_pickerTrace;
    observer.emitAttempt=EmitPickerRuntimeTerminalizationAttempt;
    observer.emitTerminal=EmitPickerRuntimeTerminalEvent;
    observer.flushBoundary=FlushPickerRuntimeTerminalBoundary;
    return observer;
}

static PickerRuntimeTerminalizationResult
FinalizePickerRuntimeTransition() noexcept {
    PickerRuntimeTerminalizationResult result;
    result.snapshot=CapturePickerRuntimeTerminalTraceSnapshot();
    const bool terminalAcknowledged=
        g_picker.transition.terminalAcknowledged;
    const bool pendingEffectNone=
        g_picker.transition.pendingEffect==PickerEffectKind::None;
    const bool runtimeKeyPresent=!g_picker.transition.runtimeKey.empty();
    bool reservationReleased=false;
    std::string claimedRuntime;
    result.run=DrivePickerTraceTerminalization(
        terminalAcknowledged,pendingEffectNone,runtimeKeyPresent,
        [&](PickerTraceReservationReleaseFacts& facts){
            reservationReleased=
                ConsumeCheckpointAndReleaseMoveReservation(
                    g_picker.transition.reservationToken,
                    g_picker.transition.runtimeKey,&facts);
            return reservationReleased;
        },[&](){
            return PickerRuntimeTerminalizationReady(
                g_picker.transition,reservationReleased);
        },[&](){
            MarkPickerOperationClaimsTerminalOutcome(
                g_picker.transition.runtimeKey,
                g_picker.transition.pendingRecordId,
                PickerTransitionTargetRestoredToOrigin(
                    g_picker.transition));
            claimedRuntime.swap(g_picker.transition.runtimeKey);
            if(!FinalizePickerTransition(g_picker)){
                claimedRuntime.swap(g_picker.transition.runtimeKey);
                return false;
            }
            return true;
        });
    if(!result.run.completed) return result;
    FinishAutoOperationsClaimedByPicker(claimedRuntime);
    bool resumeTabSearch=false;
    if(g_pickerShutdownDrain || !g_runtimeQuiescence.acceptsDispatch())
        InvalidatePickerTabSearchCache(
            g_pickerTabSearchCache,g_picker.modelGeneration);
    else
        resumeTabSearch=AcquirePickerTabSearchRetryPostLeaseWhenIdle(
            g_pickerTabSearchCache,false,g_picker.modelGeneration,
            g_picker.searchText) ||
            PickerTabSearchRetryDeliveryReadyWhenIdle(
                g_pickerTabSearchCache,false,g_picker.modelGeneration,
                g_picker.searchText);
    g_pickerTerminalizationPending=false;
    g_pickerDurableKickPending=false;
    KillTimer(g_main,TIMER_PICKER_TRANSITION);
    ArmPickerIdleRefresh();
    try {
        if(resumeTabSearch) EnsureTabSearch();
        else if(!g_pickerShutdownDrain) SchedulePickerTabSearchRetry();
    } catch(...) {
        if(!g_pickerShutdownDrain) SchedulePickerTabSearchRetry();
    }
    return result;
}

static void PumpPickerTransitionWork() noexcept {
    if(g_pickerPumpActive) return;
    g_pickerPumpActive=true;
    g_pickerDurableKickPending=false;
    bool terminalRetryNoProgress=false;
    bool terminalRetryDeferred=false;
    bool effectDeferredUntilDue=false;
    bool terminalAttemptCaptured=false;
    uint64_t terminalAttempt=0;
    PickerTraceTerminalDeliveryFacts terminalDelivery;
    PickerRuntimeTerminalizationResult terminalization;
    if(g_main) KillTimer(g_main,TIMER_PICKER_TRANSITION);
    for(unsigned budget=0;budget<256;++budget){
        if(g_pickerObservationKick.pending){
            PickerObservation observation;
            if(!ConsumePickerObservationKick(
                    g_pickerObservationKick,observation)) break;
            const PickerEffect next=
                AdvancePickerTransitionTraced(g_picker,
                    observation,&g_pickerTrace,
                    &g_pickerTraceTerminalMetadata);
            if(next.kind!=PickerEffectKind::None){
                StagePickerScheduledEffect(next);
                const bool delayed=
                    PickerEffectRequiresSettlingDelay(next.kind);
                const PickerTraceScheduleResult schedule=
                    DeferPickerTransitionWork(delayed);
                EmitPickerTraceEffectQueue(next,schedule);
                if(schedule.deferred){
                    effectDeferredUntilDue=delayed;
                    break;
                }
                continue;
            }
            if(g_picker.transition.terminalAcknowledged){
                g_pickerTerminalizationPending=true;
                const PickerTraceScheduleResult schedule=
                    DeferPickerTransitionWork(false);
                StorePickerTraceTerminalDelivery(schedule);
                if(schedule.deferred) break;
            }
            continue;
        }
        if(g_pickerEffectScheduled){
            if(!g_pickerShutdownDrain &&
               PickerEffectRequiresSettlingDelay(
                   g_pickerScheduledEffect.kind) &&
               PickerSettlingDelayRemainingMs(
                   MonotonicNowMs(),g_pickerEffectNotBeforeMs)!=0){
                const PickerTraceScheduleResult schedule=
                    DeferPickerTransitionWork(true);
                EmitPickerTraceEffectQueue(
                    g_pickerScheduledEffect,schedule);
                effectDeferredUntilDue=schedule.deferred;
                break;
            }
            const PickerEffectExecutionRoute executionRoute=
                RoutePickerEffectExecution(
                    g_pickerScheduledEffect.kind,g_pickerShutdownDrain);
            EmitPickerTraceEffectExecute(
                g_pickerScheduledEffect,executionRoute);
            if(executionRoute==
                    PickerEffectExecutionRoute::DeferUntilDue){
                effectDeferredUntilDue=true;
                break;
            }
            const PickerEffect effect=g_pickerScheduledEffect;
            g_pickerScheduledEffect=PickerEffect{};
            g_pickerEffectScheduled=false;
            g_pickerEffectNotBeforeMs=0;
            PickerObservation observation;
            bool caughtException=false;
            if(executionRoute==
                    PickerEffectExecutionRoute::AcknowledgeWithoutUi){
                observation.generation=effect.generation;
                observation.effectKind=effect.kind;
                observation.effectSerial=effect.effectSerial;
                observation.event=PickerEvent::EffectCompleted;
                observation.apiAccepted=true;
            } else {
                observation=ExecutePickerEffect(
                    effect,&caughtException);
            }
            if(caughtException)
                ObservePickerTraceTerminalMetadata(
                    g_pickerTraceTerminalMetadata,
                    g_picker.transition.phase,
                    g_picker.transition.phase,
                    observation,false,true);
            EmitPickerTraceEffectObservation(effect,observation);
            const bool posted=!g_pickerShutdownDrain && PostMessageW(
                g_main,WM_PICKER_TRANSITION,0,0)!=FALSE;
            const bool timer=posted || g_pickerShutdownDrain ? false :
                SetTimer(g_main,TIMER_PICKER_TRANSITION,1,nullptr)!=0;
            const PickerKickRoute route=StagePickerObservationKick(
                g_pickerObservationKick,observation,
                posted,timer,g_main!=nullptr);
            EmitPickerTraceEffectQueue(
                effect,PickerTraceObservationDelivery(route));
            if(route==PickerKickRoute::Posted ||
               route==PickerKickRoute::TimerArmed) break;
            continue;
        }
        if(g_pickerTerminalizationPending){
            terminalAttemptCaptured=g_pickerTrace.active();
            if(terminalAttemptCaptured){
                const uint64_t generation=
                    g_picker.transition.generation;
                terminalAttempt=
                    NextPickerTraceTerminalizationAttempt(generation);
                terminalDelivery.incomingAvailable=
                    ConsumePickerTracePendingTerminalDelivery(
                        g_pickerTracePendingTerminalDelivery,generation,
                        terminalDelivery.incoming);
            }
            terminalization=FinalizePickerRuntimeTransition();
            if(!terminalization.run.completed){
                terminalRetryNoProgress=true;
                if(!g_pickerShutdownDrain && g_main)
                    terminalRetryDeferred=SetTimer(
                        g_main,TIMER_PICKER_TRANSITION,
                        MOVE_VERIFY_INTERVAL_MS,nullptr)!=0;
                break;
            }
            if(terminalAttemptCaptured){
                ReadPickerRuntimeTerminalTraceSnapshot(
                    terminalization.snapshot);
                const PickerTraceTerminalizationEmission emission=
                    MapPickerTraceTerminalization(
                        terminalization.run,
                        terminalization.snapshot.terminal.generation,
                        terminalAttempt,terminalDelivery);
                const PickerTraceTerminalizationEventObserver observer=
                    PickerRuntimeTerminalTraceObserver();
                PublishPickerTraceTerminalization(
                    emission,&terminalization.snapshot.terminal,&observer);
            }
            ResetPickerTracePendingTerminalDelivery(
                g_pickerTracePendingTerminalDelivery);
            g_pickerTraceTerminalizationGeneration=0;
            g_pickerTraceTerminalizationAttempt=0;
            g_pickerTraceTerminalMetadata=PickerTraceTerminalMetadata{};
            continue;
        }
        break;
    }
    g_pickerPumpActive=false;
    if((g_pickerEffectScheduled || g_pickerObservationKick.pending ||
        g_pickerTerminalizationPending)){
        if(!PickerPumpImmediateKickAllowed(effectDeferredUntilDue)){
            // The settling timer (or its durable retry flag) exclusively owns
            // delivery; shutdown restores that ownership after its bounded pump.
        } else if(terminalRetryNoProgress){
            if(DecidePickerTerminalNoProgressRoute(
                    g_pickerShutdownDrain,terminalRetryDeferred)==
                    PickerTerminalNoProgressRoute::DurableExternalKick)
                g_pickerDurableKickPending=true;
            if(terminalAttemptCaptured){
                terminalDelivery.retry=
                    DecidePickerTraceDeliveryRoute(
                        g_pickerShutdownDrain,terminalRetryDeferred,
                        false,false,false,!terminalRetryDeferred);
                terminalDelivery.retryAvailable=true;
                StorePickerTracePendingTerminalDelivery(
                    g_pickerTracePendingTerminalDelivery,
                    terminalization.snapshot.terminal.generation,
                    terminalDelivery.retry);
                const PickerTraceTerminalizationEmission emission=
                    MapPickerTraceTerminalization(
                        terminalization.run,
                        terminalization.snapshot.terminal.generation,
                        terminalAttempt,terminalDelivery);
                const PickerTraceTerminalizationEventObserver observer=
                    PickerRuntimeTerminalTraceObserver();
                PublishPickerTraceTerminalization(
                    emission,nullptr,&observer);
            }
        } else {
            const PickerTraceScheduleResult schedule=
                DeferPickerTransitionWork(false);
            const PickerTraceQueueCallerDecision decision=
                DecidePickerTracePumpRearm(schedule);
            if(decision.claimDurableKickAtCaller)
                g_pickerDurableKickPending=true;
            if(g_pickerTerminalizationPending)
                StorePickerTraceTerminalDelivery(decision.delivery);
            if(g_pickerEffectScheduled)
                EmitPickerTraceEffectQueue(
                    g_pickerScheduledEffect,decision.delivery);
        }
    }
}

static void RequestPickerCancellation() noexcept {
    if(!g_picker.controlledTransition() ||
       g_picker.transition.dismissed) return;
    const bool unissued=DiscardPickerUnissuedEffectForCancel(
        g_pickerScheduledEffect,g_pickerEffectScheduled,
        g_picker.transition);
    if(unissued) g_pickerEffectNotBeforeMs=0;
    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=g_picker.transition.generation;
    cancel.unissuedEffectCancelled=unissued;
    QueuePickerEffect(AdvancePickerTransitionTraced(g_picker,
        cancel,&g_pickerTrace,
        &g_pickerTraceTerminalMetadata));
}

static bool DrainPickerForShutdown() noexcept {
    if(!g_picker.controlledTransition()) return true;
    g_pickerShutdownDrain=true;
    const bool drained=RunPickerShutdownDrain(
        g_picker,[](){ RequestPickerCancellation(); },
        [](){ PumpPickerTransitionWork(); });
    g_pickerShutdownDrain=false;
    if(!drained && (g_pickerEffectScheduled ||
       g_pickerObservationKick.pending || g_pickerTerminalizationPending)){
        const bool delayed=g_pickerEffectScheduled &&
            PickerEffectRequiresSettlingDelay(g_pickerScheduledEffect.kind);
        PickerTraceScheduleResult schedule=
            DeferPickerTransitionWork(delayed);
        g_pickerDurableKickPending=
            PickerDurableKickRequiredAfterDefer(
                schedule.deferred,g_pickerDurableKickPending);
        if(g_pickerDurableKickPending)
            schedule=MarkPickerTraceDurableKick(schedule);
        if(g_pickerTerminalizationPending)
            StorePickerTraceTerminalDelivery(schedule);
        if(g_pickerEffectScheduled)
            EmitPickerTraceEffectQueue(
                g_pickerScheduledEffect,schedule);
    }
    return drained;
}

static bool CaptureFastWindowForMove(
        HWND hwnd,FastWin& output,bool& tracked,bool& titleComplete,
        PickerTraceWindowDesktopFacts* desktopFacts=nullptr){
    if(desktopFacts) *desktopFacts=PickerTraceWindowDesktopFacts{};
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
    if(length<=0){
        titleComplete=false;
    } else {
        try { captured.title.resize(static_cast<size_t>(length)+1,L'\0'); }
        catch(...) { titleComplete=false; }
        if(titleComplete){
            int copied=GetWindowTextW(hwnd,&captured.title[0],length+1);
            if(copied<=0){ captured.title.clear(); titleComplete=false; }
            else captured.title.resize(static_cast<size_t>(copied));
        }
    }
    if(g_vdmDoc){
        if(desktopFacts) desktopFacts->invoked=true;
        HRESULT read=g_vdmDoc->GetWindowDesktopId(hwnd,&captured.desktop);
        if(desktopFacts){
            desktopFacts->result=read;
            desktopFacts->actual=captured.desktop;
            if(SUCCEEDED(read) && !GuidIsZero(captured.desktop))
                desktopFacts->validity=PickerReadValidity::Valid;
        }
        if(FAILED(read)) captured.desktop=GUID{};
    }
    output=std::move(captured);
    return true;
}

static PickerAcceptedPlanRecordResult SelectPickerAcceptedPlanRecord(
        const WindowIdentityKey& identity,const std::string& capturedApp,
        std::string& selectedApp,std::string& output,
        uint64_t& selectedOperationId,uint64_t& selectedIdentityGeneration,
        LayoutWin& selectedFreshRecord,bool& hasSelectedFreshRecord,
        bool& selectedRecordExists) noexcept {
    if(!SameIdentity(identity,identity))
        return PickerAcceptedPlanRecordResult::Rejected;
    try {
        bool found=false;
        std::string selected,adoptedApp=capturedApp;
        uint64_t acceptedOperationId=0;
        bool acceptedRecordExists=false;
        for(const auto& entry : g_pendingAutoOperations){
            const AutoRestoreOperation& operation=entry.second;
            if(!capturedApp.empty() && operation.app!=capturedApp) continue;
            bool exactInInput=false;
            for(const FastWin& candidate : operation.reconcileFast)
                if(SameIdentity(identity,IdentityOf(candidate))){
                    exactInInput=true;
                    break;
                }
            // A generic capture cannot identify a same-app operation, but an
            // exact full runtime identity can.  Unknown/unrelated apps remain
            // independent; an exact in-flight plan is a persistence barrier.
            if(capturedApp.empty() && !exactInInput) continue;
            if(operation.cancellationPending)
                return PickerAcceptedPlanRecordResult::Rejected;
            bool acceptedExactRecord=false;
            if(operation.reconcile &&
               !operation.reconcile->plan.deferred && exactInInput){
                const ReconcileResult& accepted=*operation.reconcile;
                for(const LayoutMatch& match : accepted.plan.matches){
                    if(match.savedIndex>=accepted.saved.size() ||
                       match.liveIndex>=operation.reconcileFast.size())
                        continue;
                    const FastWin& candidate=
                        operation.reconcileFast[match.liveIndex];
                    if(!SameIdentity(identity,IdentityOf(candidate)))
                        continue;
                    const LayoutWin& saved=accepted.saved[match.savedIndex];
                    GUID parsed{};
                    std::string canonical;
                    const LayoutWin* current=nullptr;
                    if(!ParseNonzeroLayoutGuid(
                            saved.recordId,parsed,&canonical) ||
                       saved.app!=operation.app ||
                       !(current=FindAutoRecord(canonical)) ||
                       current->app!=operation.app)
                        return PickerAcceptedPlanRecordResult::Rejected;
                    const PickerAcceptedPlanRecordResult accumulated=
                        AccumulatePickerAcceptedPlanRecordAdoptingApp(
                            identity,capturedApp,IdentityOf(candidate),
                            saved.app,canonical,adoptedApp,selected,found);
                    if(accumulated==
                       PickerAcceptedPlanRecordResult::Rejected)
                        return accumulated;
                    if(accumulated==
                       PickerAcceptedPlanRecordResult::Selected){
                        if(acceptedOperationId!=0 &&
                           (acceptedOperationId!=entry.first ||
                            !acceptedRecordExists))
                            return PickerAcceptedPlanRecordResult::Rejected;
                        acceptedOperationId=entry.first;
                        acceptedRecordExists=true;
                    }
                    acceptedExactRecord=true;
                }
                for(const NewRecordRequest& created :
                    accepted.plan.newRecords){
                    if(created.liveIndex>=operation.reconcileFast.size() ||
                       created.liveIndex>=accepted.live.size()) continue;
                    const FastWin& candidate=
                        operation.reconcileFast[created.liveIndex];
                    if(!SameIdentity(identity,IdentityOf(candidate))) continue;
                    GUID parsed{};
                    std::string canonical;
                    if(!ParseNonzeroLayoutGuid(
                            created.recordId,parsed,&canonical) ||
                       accepted.live[created.liveIndex].app!=operation.app)
                        return PickerAcceptedPlanRecordResult::Rejected;
                    const PickerAcceptedPlanRecordResult accumulated=
                        AccumulatePickerAcceptedPlanRecordAdoptingApp(
                            identity,capturedApp,IdentityOf(candidate),
                            operation.app,canonical,adoptedApp,selected,found);
                    if(accumulated==
                       PickerAcceptedPlanRecordResult::Rejected)
                        return accumulated;
                    if(accumulated==
                       PickerAcceptedPlanRecordResult::Selected){
                        if(acceptedOperationId!=0 &&
                           (acceptedOperationId!=entry.first ||
                            acceptedRecordExists))
                            return PickerAcceptedPlanRecordResult::Rejected;
                        acceptedOperationId=entry.first;
                        acceptedRecordExists=false;
                    }
                    acceptedExactRecord=true;
                }
            }
            const PickerInFlightPlanEntryAction action=
                DecidePickerInFlightPlanEntry(
                    true,operation.reconcile!=nullptr ||
                         operation.reconcileFastKnown,
                    exactInInput,acceptedExactRecord);
            if(action==PickerInFlightPlanEntryAction::Wait)
                return PickerAcceptedPlanRecordResult::Rejected;
        }
        if(!found) return PickerAcceptedPlanRecordResult::Unrelated;
        if(acceptedOperationId==0 || adoptedApp.empty())
            return PickerAcceptedPlanRecordResult::Rejected;
        auto acceptedOperation=
            g_pendingAutoOperations.find(acceptedOperationId);
        if(acceptedOperation==g_pendingAutoOperations.end() ||
           acceptedOperation->second.identityGeneration==0)
            return PickerAcceptedPlanRecordResult::Rejected;
        LayoutWin freshRecord;
        bool hasFreshRecord=false;
        auto fresh=g_acceptedFreshByRuntime.find(RuntimeKey(identity));
        if(fresh!=g_acceptedFreshByRuntime.end() &&
           fresh->second.record.app==adoptedApp &&
           PickerFreshRuntimeMatches(
               identity,acceptedOperation->second.identityGeneration,
               fresh->second.identity,
               fresh->second.identityGeneration)){
            freshRecord=fresh->second.record;
            hasFreshRecord=true;
        }
        selectedApp.swap(adoptedApp);
        output.swap(selected);
        selectedOperationId=acceptedOperationId;
        selectedIdentityGeneration=
            acceptedOperation->second.identityGeneration;
        if(hasFreshRecord)
            SwapLayoutWinNoThrow(selectedFreshRecord,freshRecord);
        hasSelectedFreshRecord=hasFreshRecord;
        selectedRecordExists=acceptedRecordExists;
        return PickerAcceptedPlanRecordResult::Selected;
    } catch(...) {
        return PickerAcceptedPlanRecordResult::Rejected;
    }
}

static PickerActionDispatchResult BeginPickerAction(
        const PickerActionRequest& request) noexcept {
    PickerTraceMoveBeginEvent traceEvent;
    traceEvent.activationId=request.activationId;
    traceEvent.intent=request.intent;
    switch(request.intent){
    case PickerActionIntent::RowMoveOnly:
        traceEvent.mode=PickerTransitionMode::RowMoveOnly;
        break;
    case PickerActionIntent::VisualAndFollow:
        traceEvent.mode=PickerTransitionMode::VisualAndFollow;
        break;
    case PickerActionIntent::VisualOnly:
        traceEvent.mode=PickerTransitionMode::VisualOnly;
        break;
    case PickerActionIntent::TileSwitch:
    case PickerActionIntent::ActivateExact:
    case PickerActionIntent::MoveAndFollow:
        traceEvent.mode=PickerTransitionMode::MoveAndFollow;
        break;
    }
    int index=-1;
    for(size_t tileIndex=0;tileIndex<g_tiles.size();++tileIndex)
        if(GuidEq(g_tiles[tileIndex].guid,request.destination)){
            index=static_cast<int>(tileIndex);
            break;
        }
    traceEvent.tileIndex=index;
    bool transitionPublishedForTrace=false;
    const auto finish=[&](PickerTraceMoveBeginReason reason,
                          bool flush=true) noexcept {
        traceEvent.reason=reason;
        g_pickerTrace.emit(traceEvent);
        if(flush) g_pickerTrace.flushBoundary();
        return reason==PickerTraceMoveBeginReason::Accepted
            ? PickerActionDispatchResult::TransitionStarted
            : PickerActionDispatchResult::Rejected;
    };
    if(g_picker.controlledTransition())
        return finish(PickerTraceMoveBeginReason::AlreadyControlled);
    const bool rowMove=PickerActionIsRowMove(request.intent);
    const bool followMove=PickerActionUsesPopupActiveTarget(request.intent);
    if(!rowMove && !followMove)
        return finish(PickerTraceMoveBeginReason::InvalidIndex);
    if(rowMove && (!request.hasRow ||
       request.row.admission!=PickerRowAdmission::Verified ||
       !PickerRowActionableForDrag(
           request.row,g_picker.modelGeneration,
           g_picker.rowLayoutEpoch)))
        return finish(PickerTraceMoveBeginReason::IdentityMismatch);
    if(index<0 || index>=static_cast<int>(g_tiles.size()))
        return finish(PickerTraceMoveBeginReason::InvalidIndex);
    if(followMove && g_picker.selectedIndex!=index)
        return finish(PickerTraceMoveBeginReason::SelectionIndexMismatch);
    if(followMove &&
       !GuidEq(g_picker.selectedDesktop,g_tiles[index].guid))
        return finish(PickerTraceMoveBeginReason::SelectionDesktopMismatch);
    if(!g_main)
        return finish(PickerTraceMoveBeginReason::MainWindowMissing);
    if(!g_vdmi)
        return finish(PickerTraceMoveBeginReason::DesktopManagerMissing);
    if(!g_vdmDoc)
        return finish(PickerTraceMoveBeginReason::DesktopDocumentMissing);
    try {
        const WindowIdentityKey actionTarget=followMove
            ? request.popupActiveTarget : request.row.identity;
        const uintptr_t targetHwnd=actionTarget.hwnd;
        HWND targetWindow=reinterpret_cast<HWND>(targetHwnd);
        if(followMove &&
           (!SameIdentity(request.popupActiveTarget,
                          g_picker.activeWindow) ||
            !PickerTargetMatchesActive(
                targetHwnd,g_picker.activeWindow)))
            return finish(PickerTraceMoveBeginReason::TargetMismatch);
        if(!targetWindow)
            return finish(PickerTraceMoveBeginReason::TargetWindowMissing);
        if(!IsWindow(targetWindow))
            return finish(PickerTraceMoveBeginReason::TargetWindowInvalid);
        const GUID destination=request.destination;
        PickerTraceCurrentDesktopFacts currentFacts;
        const GUID currentOrigin=CurrentDesktopGuid(&currentFacts);
        EmitPickerTraceCurrentDesktopFacts(0,0,currentFacts);
        traceEvent.destination=destination;
        traceEvent.currentOrigin=currentOrigin;
        if(GuidIsZero(destination))
            return finish(PickerTraceMoveBeginReason::DestinationZero);
        PickerTraceDesktopLookupContext lookupContext;
        lookupContext.trace=&g_pickerTrace;
        lookupContext.use=
            PickerTraceDesktopLookupUse::MoveEntryDestination;
        lookupContext.requested=destination;
        if(GetDesktopIndexByGuid(destination,&lookupContext)<0)
            return finish(
                PickerTraceMoveBeginReason::DestinationLookupFailed);
        if(followMove && GuidIsZero(currentOrigin))
            return finish(
                PickerTraceMoveBeginReason::CurrentDesktopUnavailable);

        PickerPopupBindingFacts popupBindingFacts;
        PickerPopupBindingResult popupBinding=
            PickerPopupBindingResult::CurrentUnavailable;
        if(followMove) popupBinding=
            DrivePickerPopupBinding(
                currentOrigin,
                [&]() -> PickerPopupDesktopRead {
                    PickerReadValidity validity=
                        PickerReadValidity::Unavailable;
                    GUID observed{};
                    PickerTraceWindowDesktopFacts facts;
                    ReadPickerWindowDesktop(
                        g_main,validity,observed,&facts);
                    EmitPickerTraceWindowDesktopFacts(
                        PickerTraceApiKind::GetWindowDesktopIdPopup,
                        g_main,0,0,facts);
                    return PickerPopupDesktopRead(
                        facts.result,validity,observed);
                },
                [&](const GUID& requested) -> PickerPopupDesktopMove {
                    bool invoked=false;
                    HRESULT result=E_FAIL;
                    try {
                        invoked=true;
                        result=g_vdmDoc->MoveWindowToDesktop(
                            g_main,currentOrigin);
                    } catch(...) {
                        result=E_FAIL;
                    }
                    EmitPickerTraceHResult(
                        PickerTraceApiKind::MoveWindowToDesktop,
                        0,0,result,invoked,g_main,requested);
                    return PickerPopupDesktopMove(invoked,result);
                },popupBindingFacts);
        const bool popupIsToolWindow=
            (GetWindowLongPtrW(g_main,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)!=0;
        const PickerPopupRoute popupRoute=followMove
            ?DecidePickerPopupRoute(
                popupBinding,popupBindingFacts,popupIsToolWindow)
            :PickerPopupRoute::Managed;
        PickerReadValidity popupValidity=PickerReadValidity::Unavailable;
        GUID popupOrigin{};
        if(popupBinding==PickerPopupBindingResult::UseObserved){
            popupValidity=popupBindingFacts.initial.validity;
            popupOrigin=popupBindingFacts.initial.desktop;
        } else if(popupBinding==PickerPopupBindingResult::Repaired){
            popupValidity=popupBindingFacts.verify.validity;
            popupOrigin=popupBindingFacts.verify.desktop;
        }
        traceEvent.popupOrigin=popupOrigin;
        const bool popupRouteAccepted=
            (popupRoute==PickerPopupRoute::Managed &&
             popupValidity==PickerReadValidity::Valid &&
             !GuidIsZero(popupOrigin)) ||
            (popupRoute==PickerPopupRoute::StickyUnmanaged &&
             GuidIsZero(popupOrigin));
        if(followMove && !popupRouteAccepted)
            return finish(
                PickerTraceMoveBeginReason::PopupDesktopUnavailable);

        FastWin fast;
        bool tracked=false,titleComplete=true;
        PickerTraceWindowDesktopFacts targetDesktopFacts;
        if(!CaptureFastWindowForMove(
                targetWindow,fast,tracked,titleComplete,
                &targetDesktopFacts))
            return finish(PickerTraceMoveBeginReason::FastCaptureFailed);
        EmitPickerTraceWindowDesktopFacts(
            PickerTraceApiKind::GetWindowDesktopIdCapture,
            targetWindow,0,0,targetDesktopFacts);
        traceEvent.targetOrigin=fast.desktop;
        std::vector<DeskRec> pickerDesktops;
        std::string pickerDesktopError;
        if(!CurrentDesktops(pickerDesktops,&pickerDesktopError))
            return finish(
                PickerTraceMoveBeginReason::TargetDesktopUnavailable);
        if(!ConcreteDesktopExists(
                destination,pickerDesktops,DeskGuid))
            return finish(
                PickerTraceMoveBeginReason::DestinationLookupFailed);
        const bool targetOriginConcrete=
            ConcreteDesktopExists(fast.desktop,pickerDesktops,DeskGuid);
        const WindowIdentityKey identity=IdentityOf(fast);
        const WindowIdentityRecapture identityRecapture=
            RecaptureGenericWindowIdentity(identity);
        const bool identityAllowed=followMove
            ? PickerCommitIdentityAllowed(
                targetHwnd,g_picker.activeWindow,identity,
                identityRecapture)
            : identityRecapture==WindowIdentityRecapture::Match &&
                SameIdentity(actionTarget,identity) &&
                targetHwnd==identity.hwnd;
        if(!identityAllowed){
            if(!SameIdentity(actionTarget,identity))
                return finish(PickerTraceMoveBeginReason::IdentityMismatch);
            if(identityRecapture==WindowIdentityRecapture::Lost)
                return finish(PickerTraceMoveBeginReason::IdentityLost);
            if(identityRecapture==WindowIdentityRecapture::Indeterminate)
                return finish(
                    PickerTraceMoveBeginReason::IdentityIndeterminate);
            return finish(PickerTraceMoveBeginReason::IdentityMismatch);
        }

        BOOL onCurrentDesktop=FALSE;
        HRESULT membershipResult=E_FAIL;
        try {
            membershipResult=
                g_vdmDoc->IsWindowOnCurrentVirtualDesktop(
                    targetWindow,&onCurrentDesktop);
        } catch(...) {
            membershipResult=E_FAIL;
        }
        const TargetDesktopRoute desktopRoute=DecideTargetDesktopRoute(
            targetDesktopFacts.result,!GuidIsZero(fast.desktop),
            targetOriginConcrete,membershipResult,
            onCurrentDesktop!=FALSE);
        IApplicationView* rawView=nullptr;
        HRESULT viewResult=E_NOINTERFACE;
        if(g_avc){
            try {
                viewResult=g_avc->GetViewForHwnd(
                    targetWindow,&rawView);
            } catch(...) {
                viewResult=E_FAIL;
            }
        }
        ScopedComPtr<IApplicationView> actionView(rawView);
        TargetMobilityProbeFacts mobilityFacts;
        const TargetMobilityDecision mobilityDecision=
            QueryTargetWindowMobility(
                identity,desktopRoute,
                SUCCEEDED(viewResult)?actionView.get():nullptr,
                mobilityFacts);
        traceEvent.mobility=mobilityDecision.mobility;
        const PickerActionIntentDecision intentDecision=
            DecidePickerActionIntent(
                request.hasRow,rowMove,followMove,
                request.hasRow?request.row.admission:
                    PickerRowAdmission::Verified,
                true,mobilityDecision.disposition);
        if(!intentDecision.accepted)
            return finish(
                PickerTraceMoveBeginReason::TargetDesktopUnavailable);
        const PickerActionIntent resolvedIntent=intentDecision.intent;
        PickerTransitionMode transitionMode=
            PickerTransitionMode::MoveAndFollow;
        switch(resolvedIntent){
        case PickerActionIntent::MoveAndFollow:
            transitionMode=PickerTransitionMode::MoveAndFollow;
            break;
        case PickerActionIntent::RowMoveOnly:
            transitionMode=PickerTransitionMode::RowMoveOnly;
            break;
        case PickerActionIntent::VisualAndFollow:
            transitionMode=PickerTransitionMode::VisualAndFollow;
            break;
        case PickerActionIntent::VisualOnly:
            transitionMode=PickerTransitionMode::VisualOnly;
            break;
        case PickerActionIntent::TileSwitch:
        case PickerActionIntent::ActivateExact:
            return finish(PickerTraceMoveBeginReason::IdentityMismatch);
        }
        traceEvent.mode=transitionMode;
        const bool visualMode=
            transitionMode==PickerTransitionMode::VisualAndFollow ||
            transitionMode==PickerTransitionMode::VisualOnly;
        if(!visualMode && (!targetOriginConcrete ||
           desktopRoute!=TargetDesktopRoute::Exact ||
           GuidEq(fast.desktop,destination)))
            return finish(
                PickerTraceMoveBeginReason::TargetDesktopUnavailable);
        const std::string runtimeKey=RuntimeKey(identity);
        if(visualMode){
            const WinItem* exactRow=nullptr;
            bool duplicateRow=false;
            for(const Tile& tile : g_tiles)
                for(const WinItem& item : tile.windows){
                    if(item.runtimeKey!=runtimeKey ||
                       !SameIdentity(item.identity,actionTarget))
                        continue;
                    if(exactRow){
                        duplicateRow=true;
                        break;
                    }
                    exactRow=&item;
                }
            if(!exactRow || duplicateRow ||
               exactRow->admission!=PickerRowAdmission::Verified)
                return finish(
                    PickerTraceMoveBeginReason::IdentityMismatch);

            const auto existing=
                g_picker.visualAssignments.find(runtimeKey);
            GUID baseDesktop=existing!=
                    g_picker.visualAssignments.end()
                ? existing->second.baseDesktop
                : transitionMode==PickerTransitionMode::VisualOnly
                    ? request.row.baseDesktop
                    : exactRow->baseDesktop;
            const GUID displayedDesktop=
                transitionMode==PickerTransitionMode::VisualOnly
                    ? request.row.displayedDesktop
                    : exactRow->displayedDesktop;
            if(!ConcreteDesktopExists(
                    baseDesktop,pickerDesktops,DeskGuid) ||
               GuidEq(displayedDesktop,destination))
                return finish(
                    PickerTraceMoveBeginReason::TargetDesktopUnavailable);

            PickerTransition prepared;
            prepared.mode=transitionMode;
            prepared.generation=TakeNonzeroId(g_nextOperationId);
            traceEvent.generation=prepared.generation;
            prepared.reservationToken.owner=MoveOwner::Picker;
            prepared.reservationToken.operationId=prepared.generation;
            prepared.reservationToken.jobId=
                TakeNonzeroId(g_nextMoveJobId);
            prepared.popupActiveTarget=request.popupActiveTarget;
            prepared.target=actionTarget;
            prepared.runtimeKey=runtimeKey;
            prepared.targetOrigin=fast.desktop;
            prepared.popupOrigin=popupOrigin;
            prepared.popupRoute=popupRoute;
            prepared.currentOrigin=currentOrigin;
            prepared.destination=destination;
            prepared.capturedTitle=fast.title;
            prepared.capturedTitleComplete=titleComplete;
            prepared.visualMutation.runtimeKey=runtimeKey;
            prepared.visualMutation.baseDesktop=baseDesktop;
            prepared.visualMutation.destination=destination;
            prepared.visualMutation.kind=GuidEq(
                    baseDesktop,destination)
                ? PickerVisualMutationKind::Erase
                : PickerVisualMutationKind::Upsert;

            ReservedAutoIdentity reservation;
            reservation.token=prepared.reservationToken;
            reservation.identity=actionTarget;
            ReservationHandoff handoff;
            bool transitionPublished=false;
            const bool staged=RunSuccessorFirstReservationHandoff([&](){
                if(!BeginReservationHandoff(
                        runtimeKey,reservation,handoff))
                    return false;
                g_picker.transition.swap(prepared);
                transitionPublished=true;
                return true;
            },[&]() noexcept {
                PublishReservationHandoff(handoff);
            },[&](){
                CancelDisplacedReservationHandoff(handoff);
            },[&](){
                if(transitionPublished){
                    g_picker.transition.swap(prepared);
                    transitionPublished=false;
                }
                RollbackReservationHandoff(handoff);
            });
            if(!staged)
                return finish(
                    PickerTraceMoveBeginReason::
                        ReservationHandoffFailed);
            transitionPublishedForTrace=true;
            ResetPickerTracePendingTerminalDelivery(
                g_pickerTracePendingTerminalDelivery);
            g_pickerTraceTerminalizationGeneration=
                g_picker.transition.generation;
            g_pickerTraceTerminalizationAttempt=0;
            g_pickerTraceTerminalMetadata=
                PickerTraceTerminalMetadata{};
            g_pickerTraceTerminalMetadata.generation=
                g_picker.transition.generation;
            if(!GuidIsZero(currentOrigin))
                SetPickerCurrentDesktop(g_picker,currentOrigin);

            PickerObservation begin;
            begin.event=PickerEvent::Begin;
            begin.generation=g_picker.transition.generation;
            const PickerEffect effect=AdvancePickerTransitionTraced(
                g_picker,begin,&g_pickerTrace,
                &g_pickerTraceTerminalMetadata);
            if(effect.kind==PickerEffectKind::None){
                MoveResult terminal;
                terminal.completed=true;
                terminal.terminal=MoveTerminal::Cancelled;
                terminal.token=g_picker.transition.reservationToken;
                terminal.runtimeKey=runtimeKey;
                ConsumeCheckpointAndReleaseMoveReservation(terminal);
                g_picker.transition.swap(prepared);
                ResetPickerTracePendingTerminalDelivery(
                    g_pickerTracePendingTerminalDelivery);
                g_pickerTraceTerminalizationGeneration=0;
                g_pickerTraceTerminalizationAttempt=0;
                g_pickerTraceTerminalMetadata=
                    PickerTraceTerminalMetadata{};
                return finish(
                    PickerTraceMoveBeginReason::NoInitialEffect);
            }
            traceEvent.firstEffect=effect.kind;
            (void)finish(PickerTraceMoveBeginReason::Accepted,false);
            QueuePickerEffect(effect);
            g_pickerTrace.flushBoundary();
            return PickerActionDispatchResult::TransitionStarted;
        }
        std::string acceptedPlanApp,acceptedPlanRecord;
        uint64_t acceptedPlanOperationId=0;
        uint64_t acceptedPlanIdentityGeneration=0;
        LayoutWin acceptedPlanFreshRecord;
        bool hasAcceptedPlanFreshRecord=false;
        bool acceptedPlanRecordExists=false;
        const PickerAcceptedPlanRecordResult acceptedPlan=
            SelectPickerAcceptedPlanRecord(
                identity,fast.app,acceptedPlanApp,acceptedPlanRecord,
                acceptedPlanOperationId,acceptedPlanIdentityGeneration,
                acceptedPlanFreshRecord,hasAcceptedPlanFreshRecord,
                acceptedPlanRecordExists);
        if(acceptedPlan==PickerAcceptedPlanRecordResult::Rejected)
            return finish(PickerTraceMoveBeginReason::AcceptedPlanConflict);
        if(acceptedPlan==PickerAcceptedPlanRecordResult::Selected){
            fast.app=acceptedPlanApp;
            tracked=true;
        }

        PickerTransition prepared;
        prepared.mode=transitionMode;
        prepared.generation=TakeNonzeroId(g_nextOperationId);
        traceEvent.generation=prepared.generation;
        prepared.reservationToken.owner=MoveOwner::Picker;
        prepared.reservationToken.operationId=prepared.generation;
        prepared.reservationToken.jobId=TakeNonzeroId(g_nextMoveJobId);
        prepared.popupActiveTarget=request.popupActiveTarget;
        prepared.target=actionTarget;
        prepared.runtimeKey=runtimeKey;
        prepared.app=fast.app;
        prepared.targetOrigin=fast.desktop;
        prepared.popupOrigin=popupOrigin;
        prepared.popupRoute=popupRoute;
        prepared.currentOrigin=currentOrigin;
        prepared.destination=destination;
        prepared.capturedTitle=fast.title;
        prepared.capturedTitleComplete=titleComplete;

        ReservedAutoIdentity reservation;
        reservation.token=prepared.reservationToken;
        reservation.identity=actionTarget;
        reservation.app=fast.app;
        reservation.originDesktop=fast.desktop;
        if(acceptedPlan==PickerAcceptedPlanRecordResult::Selected){
            prepared.identityGeneration=acceptedPlanIdentityGeneration;
            reservation.identityGeneration=acceptedPlanIdentityGeneration;
            if(hasAcceptedPlanFreshRecord){
                reservation.acceptedFreshRecord=acceptedPlanFreshRecord;
                reservation.hasAcceptedFreshRecord=true;
            }
        }
        bool provisionalNeedsInsert=false;
        bool preserveAcceptedPlanPending=false;

        if(tracked){
            std::map<std::string,AppFastSnapshot> snapshots=
                CollectFastSnapshots();
            auto snapshot=snapshots.find(fast.app);
            bool exactPresent=false;
            if(snapshot!=snapshots.end() &&
               FastSnapshotCanObserve(snapshot->second)){
                for(const FastWin& candidate : snapshot->second.windows)
                    if(SameIdentity(IdentityOf(candidate),identity)){
                        exactPresent=true;
                        break;
                    }
            }
            if(exactPresent){
                prepared.identityGeneration=
                    snapshot->second.identityGeneration;
                reservation.identityGeneration=
                    prepared.identityGeneration;
                reservation.acceptedFreshRecord=LayoutWin{};
                reservation.hasAcceptedFreshRecord=false;
                auto fresh=g_acceptedFreshByRuntime.find(runtimeKey);
                if(fresh!=g_acceptedFreshByRuntime.end() &&
                   PickerFreshRuntimeMatches(
                       identity,prepared.identityGeneration,
                       fresh->second.identity,
                       fresh->second.identityGeneration)){
                    reservation.acceptedFreshRecord=fresh->second.record;
                    reservation.hasAcceptedFreshRecord=true;
                }
            }
            auto lifecycle=g_lifecycleByApp.find(fast.app);
            if(lifecycle!=g_lifecycleByApp.end() &&
               lifecycle->second.saveInFlight){
                prepared.lifecycleSaveGeneration=
                    lifecycle->second.saveGeneration;
                prepared.lifecycleLayoutSignature=
                    lifecycle->second.layoutSignature;
                prepared.lifecycleSessionSignature=
                    lifecycle->second.sessionStampSignature;
            }
        }

        auto bound=g_recordByRuntime.find(runtimeKey);
        if(bound!=g_recordByRuntime.end() &&
           SameIdentity(bound->second.identity,identity)){
            if(acceptedPlan==PickerAcceptedPlanRecordResult::Selected &&
               bound->second.recordId!=acceptedPlanRecord)
                return finish(
                    PickerTraceMoveBeginReason::BoundRecordConflict);
            reservation.recordId=bound->second.recordId;
        } else {
            std::string recordId;
            PopupReservationRecordSource source=
                PopupReservationRecordSource::None;
            const bool persistenceReady=
                g_autoLoaded && g_autoWritesAllowed &&
                g_autoFix && !g_degraded;
            LayoutWin capturedProvisional;
            const bool hasCapturedProvisional=titleComplete &&
                BuildPickerCapturedTitleOnlyProvisional(
                    fast.app,fast.title,capturedProvisional);
            const bool acceptedNewNeedsOrigin=
                acceptedPlan==PickerAcceptedPlanRecordResult::Selected &&
                !acceptedPlanRecordExists &&
                !reservation.hasAcceptedFreshRecord;
            if(acceptedNewNeedsOrigin && !hasCapturedProvisional)
                return finish(
                    PickerTraceMoveBeginReason::SafeOriginUnavailable);
            const bool safeOriginRecord=PickerHasSafeOriginRecord(
                hasCapturedProvisional,
                reservation.hasAcceptedFreshRecord);
            if(acceptedPlan==PickerAcceptedPlanRecordResult::Selected){
                recordId=acceptedPlanRecord;
                source=PopupReservationRecordSource::Pending;
                preserveAcceptedPlanPending=true;
            } else {
                source=SelectPopupReservationRecord(
                    tracked,persistenceReady,safeOriginRecord,
                    targetOriginConcrete,
                    [&](std::string& selected){
                        return SelectPendingPopupRecordId(
                            identity,fast.app,g_pendingRecordByRuntime,
                            [&](const std::string& candidate,
                                const std::string& app,
                                std::string& canonical){
                                GUID parsed{};
                                if(!ParseNonzeroLayoutGuid(
                                        candidate,parsed,&canonical))
                                    return false;
                                for(const LayoutWin& saved : g_autoRecords)
                                    if(saved.recordId==canonical &&
                                       saved.app==app) return true;
                                return false;
                            },selected);
                    },
                    [&](std::string& selected){
                        auto existing=g_provisionalRecordByRuntime.find(
                            runtimeKey);
                        if(existing!=g_provisionalRecordByRuntime.end()){
                            selected=existing->second;
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
            }
            if(source==PopupReservationRecordSource::None &&
               !safeOriginRecord &&
               PickerMayReserveStableRecordId(
                   tracked,persistenceReady,
                   targetOriginConcrete)){
                auto existing=g_provisionalRecordByRuntime.find(runtimeKey);
                if(existing!=g_provisionalRecordByRuntime.end()){
                    recordId=existing->second;
                    source=PopupReservationRecordSource::Provisional;
                } else {
                    recordId=NewRecordId();
                    GUID parsed{};
                    if(ParseNonzeroLayoutGuid(recordId,parsed)){
                        provisionalNeedsInsert=true;
                        source=PopupReservationRecordSource::Provisional;
                    } else {
                        recordId.clear();
                    }
                }
            }
            if(source!=PopupReservationRecordSource::None){
                reservation.recordId=recordId;
                if((source==PopupReservationRecordSource::Provisional ||
                    acceptedNewNeedsOrigin) &&
                   safeOriginRecord){
                    LayoutWin origin;
                    if(reservation.hasAcceptedFreshRecord){
                        origin=reservation.acceptedFreshRecord;
                        origin.provisional=false;
                    } else {
                        origin=capturedProvisional;
                    }
                    origin.recordId=recordId;
                    origin.app=fast.app;
                    origin.desktop=fast.desktop;
                    origin.deskIndex=SnapshotDesktopIndex(fast.desktop);
                    MarkSeen(origin,UtcNowSeconds());
                    reservation.provisionalOriginRecord=origin;
                    reservation.hasProvisionalOriginRecord=true;
                }
            }
        }
        ReservationHandoff handoff;
        std::map<std::string,std::string> stagedAcceptedPending;
        std::map<std::string,PickerOperationLifetimeClaim>
            stagedOperationClaims;
        AutoRestoreOperation* claimedOperation=nullptr;
        bool insertedProvisional=false;
        bool acceptedPendingPublished=false;
        bool operationClaimPublished=false;
        bool transitionPublished=false;
        PickerTraceMoveBeginReason stagingFailure=
            PickerTraceMoveBeginReason::ReservationHandoffFailed;
        if(acceptedPlan==PickerAcceptedPlanRecordResult::Selected){
            auto operation=
                g_pendingAutoOperations.find(acceptedPlanOperationId);
            if(operation==g_pendingAutoOperations.end())
                return finish(
                    PickerTraceMoveBeginReason::AcceptedOperationMissing);
            if(!StagePickerOperationLifetimeClaim(
                   operation->second.pickerClaimedRecordByRuntime,
                   runtimeKey,acceptedPlanRecord,
                   acceptedPlanRecordExists,stagedOperationClaims))
                return finish(
                    PickerTraceMoveBeginReason::OperationClaimStageFailed);
            claimedOperation=&operation->second;
        }
        const bool staged=RunSuccessorFirstReservationHandoff([&](){
            stagingFailure=
                PickerTraceMoveBeginReason::ReservationHandoffFailed;
            if(!BeginReservationHandoff(runtimeKey,reservation,handoff))
                return false;
            prepared.pendingRecordId=handoff.installed.recordId;
            if(preserveAcceptedPlanPending){
                stagingFailure=
                    PickerTraceMoveBeginReason::PendingAssociationStageFailed;
                if(!StagePickerAcceptedPlanPendingAssociation(
                        g_pendingRecordByRuntime,runtimeKey,
                        handoff.installed.recordId,stagedAcceptedPending))
                    return false;
                g_pendingRecordByRuntime.swap(stagedAcceptedPending);
                acceptedPendingPublished=true;
            }
            if(claimedOperation){
                claimedOperation->pickerClaimedRecordByRuntime.swap(
                    stagedOperationClaims);
                operationClaimPublished=true;
            }
            if(provisionalNeedsInsert){
                stagingFailure=
                    PickerTraceMoveBeginReason::ProvisionalInsertFailed;
                if(!g_provisionalRecordByRuntime.emplace(
                        runtimeKey,handoff.installed.recordId).second)
                    return false;
                insertedProvisional=true;
            }
            g_picker.transition.swap(prepared);
            transitionPublished=true;
            return true;
        },[&]() noexcept {
            PublishReservationHandoff(handoff);
        },[&](){
            CancelDisplacedReservationHandoff(handoff);
        },[&](){
            if(transitionPublished){
                g_picker.transition.swap(prepared);
                transitionPublished=false;
            }
            if(acceptedPendingPublished){
                g_pendingRecordByRuntime.swap(stagedAcceptedPending);
                acceptedPendingPublished=false;
            }
            if(operationClaimPublished && claimedOperation){
                claimedOperation->pickerClaimedRecordByRuntime.swap(
                    stagedOperationClaims);
                operationClaimPublished=false;
            }
            if(insertedProvisional){
                auto provisional=g_provisionalRecordByRuntime.find(runtimeKey);
                if(provisional!=g_provisionalRecordByRuntime.end() &&
                   provisional->second==handoff.installed.recordId)
                    g_provisionalRecordByRuntime.erase(provisional);
            }
            RollbackReservationHandoff(handoff);
        });
        if(!staged) return finish(stagingFailure);
        transitionPublishedForTrace=true;
        ResetPickerTracePendingTerminalDelivery(
            g_pickerTracePendingTerminalDelivery);
        g_pickerTraceTerminalizationGeneration=
            g_picker.transition.generation;
        g_pickerTraceTerminalizationAttempt=0;
        g_pickerTraceTerminalMetadata=PickerTraceTerminalMetadata{};
        g_pickerTraceTerminalMetadata.generation=
            g_picker.transition.generation;

        if(!GuidIsZero(currentOrigin))
            SetPickerCurrentDesktop(g_picker,currentOrigin);
        if(!g_picker.transition.pendingRecordId.empty()){
            try {
                g_restoreBudgets.clearForExplicitRetry(
                    g_picker.transition.pendingRecordId);
            } catch(...) {}
        }
        PickerObservation begin;
        begin.event=PickerEvent::Begin;
        begin.generation=g_picker.transition.generation;
        const PickerEffect effect=
            AdvancePickerTransitionTraced(g_picker,
                begin,&g_pickerTrace,
                &g_pickerTraceTerminalMetadata);
        if(effect.kind==PickerEffectKind::None){
            MoveResult terminal;
            terminal.completed=true;
            terminal.terminal=MoveTerminal::Cancelled;
            terminal.token=g_picker.transition.reservationToken;
            terminal.runtimeKey=runtimeKey;
            terminal.recordId=g_picker.transition.pendingRecordId;
            ConsumeCheckpointAndReleaseMoveReservation(terminal);
            g_picker.transition.swap(prepared);
            ResetPickerTracePendingTerminalDelivery(
                g_pickerTracePendingTerminalDelivery);
            g_pickerTraceTerminalizationGeneration=0;
            g_pickerTraceTerminalizationAttempt=0;
            g_pickerTraceTerminalMetadata=PickerTraceTerminalMetadata{};
            return finish(PickerTraceMoveBeginReason::NoInitialEffect);
        }
        traceEvent.firstEffect=effect.kind;
        (void)finish(PickerTraceMoveBeginReason::Accepted,false);
        QueuePickerEffect(effect);
        g_pickerTrace.flushBoundary();
        return PickerActionDispatchResult::TransitionStarted;
    } catch(...) {
        // Entry staging is failure-atomic; any published transition owns its
        // exact guard and the durable pump will finish or roll it back.
        PickerTraceMoveBeginExceptionEvent event;
        event.activationId=request.activationId;
        event.transitionPublished=transitionPublishedForTrace;
        g_pickerTrace.emit(event);
        g_pickerTrace.flushBoundary();
        return transitionPublishedForTrace
            ? PickerActionDispatchResult::TransitionStarted
            : PickerActionDispatchResult::Rejected;
    }
}
class PickerTraceCaptureEmitScope {
public:
    explicit PickerTraceCaptureEmitScope(
            PickerTraceCaptureEvent* event) noexcept:event_(event){}
    ~PickerTraceCaptureEmitScope() noexcept {
        if(event_ && g_pickerTrace.active()) g_pickerTrace.emit(*event_);
    }
    PickerTraceCaptureEmitScope(const PickerTraceCaptureEmitScope&)=delete;
    PickerTraceCaptureEmitScope& operator=(
        const PickerTraceCaptureEmitScope&)=delete;
private:
    PickerTraceCaptureEvent* event_=nullptr;
};

static PickerTargetCaptureState CapturePickerTarget() noexcept {
    PickerTargetCaptureState capture;
    PickerTraceCaptureEvent traceEvent;
    PickerTraceCaptureEmitScope traceScope(
        g_pickerTrace.active()?&traceEvent:nullptr);
    HWND window=GetForegroundWindow();
    traceEvent.hwnd=reinterpret_cast<uintptr_t>(window);
    if(window==g_main) window=nullptr;
    if(!window) return capture;
    traceEvent.windowValid=IsWindow(window)!=FALSE;
    if(!traceEvent.windowValid) return capture;
    capture.hwnd=reinterpret_cast<uintptr_t>(window);
    PickerWindowIdentityCaptureTraceFacts identityFacts;
    capture.identity=CapturePickerWindowIdentity(window,&identityFacts);
    traceEvent.pid=identityFacts.pid;
    traceEvent.tid=identityFacts.tid;
    traceEvent.identityComplete=identityFacts.identityComplete;
    traceEvent.recapture=identityFacts.recapture;
    if(!SameIdentity(capture.identity,capture.identity)){
        traceEvent.targetPublished=CompletePickerTargetRecapture(
            capture,identityFacts.recapture);
        return capture;
    }
    try {
        const int length=GetWindowTextLengthW(window);
        traceEvent.titleLength=length;
        if(length>0){
            std::wstring title(static_cast<size_t>(length)+1,L'\0');
            const int copied=GetWindowTextW(
                window,&title[0],length+1);
            if(copied>0){
                traceEvent.titleRead=true;
                title.resize(static_cast<size_t>(copied));
                capture.title.swap(title);
            }
        }
    } catch(...) {
        capture.title.clear();
    }
    traceEvent.recapture=RecaptureGenericWindowIdentity(capture.identity);
    traceEvent.targetPublished=CompletePickerTargetRecapture(
        capture,traceEvent.recapture);
    return capture;
}

static bool GetPrimaryPickerWorkArea(RECT& workArea) noexcept {
    POINT origin={0,0};
    HMONITOR monitor=MonitorFromPoint(
        origin,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info={sizeof(info)};
    if(monitor && GetMonitorInfoW(monitor,&info) &&
       info.rcWork.right>info.rcWork.left &&
       info.rcWork.bottom>info.rcWork.top){
        workArea=info.rcWork;
        return true;
    }

    RECT fallback={0,0,0,0};
    if(SystemParametersInfoW(
            SPI_GETWORKAREA,0,&fallback,0) &&
       fallback.right>fallback.left &&
       fallback.bottom>fallback.top){
        workArea=fallback;
        return true;
    }

    fallback={0,0,GetSystemMetrics(SM_CXSCREEN),
                   GetSystemMetrics(SM_CYSCREEN)};
    if(fallback.right<=fallback.left ||
       fallback.bottom<=fallback.top) return false;
    workArea=fallback;
    return true;
}

static void AbortPickerShowPreparation() noexcept {
    HidePicker(PickerHideDisposition::DismissSession);
    g_pickerPaintCache.clear();
    g_target=nullptr;
    g_targetTitle.clear();
}

class PickerTraceOpenEmitScope {
public:
    explicit PickerTraceOpenEmitScope(
            PickerTraceOpenEvent* event) noexcept:event_(event){}
    ~PickerTraceOpenEmitScope() noexcept {
        if(event_ && g_pickerTrace.active()) g_pickerTrace.emit(*event_);
    }
    PickerTraceOpenEmitScope(const PickerTraceOpenEmitScope&)=delete;
    PickerTraceOpenEmitScope& operator=(
        const PickerTraceOpenEmitScope&)=delete;
private:
    PickerTraceOpenEvent* event_=nullptr;
};

static void ShowPicker(PickerTargetCaptureState capture){
    PickerTraceOpenEvent traceEvent;
    PickerTraceOpenEmitScope traceScope(
        g_pickerTrace.active()?&traceEvent:nullptr);
    traceEvent.targetIdentityPresent=capture.hwnd!=0 &&
        SameIdentity(capture.identity,capture.identity);
    if(g_degraded){
        traceEvent.result=PickerTraceOpenResult::Degraded;
        return;   // desktop COM unavailable; startup dialog + tray tip already explain
    }
    if(PickerInteractionBusy(g_picker,g_pickerGesture)){
        traceEvent.result=PickerTraceOpenResult::ControlledTransition;
        return;
    }
    RECT workArea={0,0,0,0};
    if(!GetPrimaryPickerWorkArea(workArea)){
        traceEvent.result=PickerTraceOpenResult::WorkAreaUnavailable;
        AbortPickerShowPreparation();
        return;
    }
    g_target=reinterpret_cast<HWND>(capture.hwnd);
    g_targetTitle.swap(capture.title);
    PickerTraceDesktopSnapshotFacts snapshotFacts;
    PickerModelAttemptTraceFacts attemptFacts;
    const bool traceActive=g_pickerTrace.active();
    const bool modelReady=BuildModel(
        capture.identity,true,false,
        traceActive?&snapshotFacts:nullptr,
        traceActive?&attemptFacts:nullptr);
    if(traceActive){
        traceEvent.desktopSnapshot=snapshotFacts.status;
        traceEvent.desktopSnapshotResult=snapshotFacts.result;
        traceEvent.desktopSnapshotIndex=snapshotFacts.index;
        traceEvent.desktopSnapshotCount=snapshotFacts.count;
        traceEvent.currentDesktop=attemptFacts.currentDesktop;
        traceEvent.currentDesktopAvailable=
            attemptFacts.currentDesktopAvailable;
        traceEvent.modelGeneration=attemptFacts.modelGeneration;
    }
    if(!modelReady){
        traceEvent.result=PickerTraceOpenResult::ModelUnavailable;
        AbortPickerShowPreparation();
        return;
    }
    InvalidatePickerTabSearchCache(
        g_pickerTabSearchCache,g_picker.modelGeneration);
    SIZE sz=DesiredClientSize();
    RECT windowRect={0,0,sz.cx,sz.cy};
    const BOOL windowAdjusted=AdjustWindowRectEx(
        &windowRect,WS_POPUP,FALSE,WS_EX_TOOLWINDOW|WS_EX_TOPMOST);
    if(!windowAdjusted){
        traceEvent.result=PickerTraceOpenResult::AdjustRectFailed;
        AbortPickerShowPreparation();
        return;
    }
    const SIZE outer={
        windowRect.right-windowRect.left,
        windowRect.bottom-windowRect.top
    };
    if(!PickerOuterSizeValid(outer)){
        traceEvent.result=PickerTraceOpenResult::OuterSizeInvalid;
        AbortPickerShowPreparation();
        return;
    }
    const POINT origin=PickerCenteredOrigin(workArea,outer);
    const BOOL windowPositioned=SetWindowPos(
        g_main,HWND_TOPMOST,origin.x,origin.y,
        outer.cx,outer.cy,SWP_NOACTIVATE);
    if(!windowPositioned){
        traceEvent.result=PickerTraceOpenResult::PositionFailed;
        AbortPickerShowPreparation();
        return;
    }
    RECT cr={0,0,0,0};
    if(!GetClientRect(g_main,&cr)){
        traceEvent.result=PickerTraceOpenResult::ClientRectFailed;
        AbortPickerShowPreparation();
        return;
    }
    EnsurePickerChildren();
    if(g_search){ SetWindowTextW(g_search,L""); RECT sb=SearchBoxRect(cr.right);
        int eLeft=sb.left+S(14), eRight=sb.right-S(44), eH=S(22), eTop=sb.top+((sb.bottom-sb.top)-eH)/2;
        MoveWindow(g_search,eLeft,eTop,eRight-eLeft,eH,TRUE); ShowWindow(g_search,SW_SHOW); }
    MarkPickerIconPreloadDirty(g_picker.selectedIndex);
    PreloadVisiblePickerIcons();
    const bool paintCacheReady=
        cr.right==sz.cx && cr.bottom==sz.cy &&
        PickerPaintCacheMatches(
            g_picker,g_pickerPaintCache.generation);
    if(!PickerShowPreparationComplete(modelReady,paintCacheReady)){
        traceEvent.result=PickerTraceOpenResult::PaintCacheFailed;
        AbortPickerShowPreparation();
        return;
    }
    ShowWindow(g_main,SW_SHOW); SetForegroundWindow(g_main);
    if(g_search) SetFocus(g_search);
    InvalidateRect(g_main,nullptr,FALSE);
    ArmPickerIdleRefresh();
    traceEvent.result=PickerTraceOpenResult::Shown;
}
static void MoveSel(int dx,int dy){ if(PickerInteractionBusy(g_picker,g_pickerGesture)||g_tiles.empty())return; int selected=g_picker.selectedIndex; if(selected<0||selected>=(int)g_tiles.size())selected=0; int r=selected/g_cols,c=selected%g_cols; c+=dx;r+=dy; int n=(int)g_tiles.size();
    if(c<0)c=0; if(c>=g_cols)c=g_cols-1; if(r<0)r=0; int idx=r*g_cols+c; if(idx>=n)idx=n-1; if(idx<0)idx=0; SetPickerSelectionCurrent(idx); RefreshPickerPaintCache(); InvalidateRect(g_main,nullptr,FALSE); }

// ================================ GUI: tray ==================================
static bool OwnedAppIconCapacityAvailable() noexcept {
    return g_appIconOwnershipReady &&
        g_ownedIcons.size()+g_failedOwnedIconReleases.size()<
            MAX_OWNED_APP_ICONS;
}

static HICON TrackOwnedAppIcon(HICON icon) noexcept {
    if(!icon) return nullptr;
    if(!OwnedAppIconCapacityAvailable()){
        if(!DestroyIcon(icon))
            (void)g_failedOwnedIconReleases.retain(icon);
        return nullptr;
    }
    // InitMetrics reserved every lifetime slot before any icon acquisition;
    // HICON is trivially copied, so this publication cannot allocate or throw.
    g_ownedIcons.push_back(icon);
    return icon;
}

static HICON LoadAppIcon(int cx,int cy){
    if(!OwnedAppIconCapacityAvailable()) return nullptr;
    // 1) встроенный ресурс (vde.res, см. vde.rc)
    HICON h=(HICON)LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_APPICON),IMAGE_ICON,cx,cy,LR_DEFAULTCOLOR);
    if(h)return TrackOwnedAppIcon(h);
    // 2) внешний файл vde.ico рядом с exe (если ресурс не вшит)
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr,path,MAX_PATH); std::wstring p=path;
    size_t s=p.find_last_of(L"\\/"); if(s!=std::wstring::npos)p=p.substr(0,s+1); p+=L"vde.ico";
    h=(HICON)LoadImageW(nullptr,p.c_str(),IMAGE_ICON,cx,cy,LR_LOADFROMFILE);
    if(h)return TrackOwnedAppIcon(h);
    // 3) системная заглушка
    HICON fallback=LoadIconW(nullptr,IDI_APPLICATION);
    return fallback?TrackOwnedAppIcon(CopyIcon(fallback)):nullptr;
}
static void TrayAdd(HWND hwnd){
    g_nid.cbSize=sizeof(g_nid); g_nid.hWnd=hwnd; g_nid.uID=1; g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; g_nid.uCallbackMessage=WM_TRAY;
    g_nid.hIcon=LoadAppIcon(GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON));
    wcsncpy_s(g_nid.szTip, g_degraded ? L"Virtual Desktop Extension (compatibility issue - see About)" : APP_NAME, _TRUNCATE);
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

struct PickerExactActivationProductContext {
    uint64_t activationId=0;
};

static WindowIdentityRecapture PickerProductRecaptureExactIdentity(
        void*,const WindowIdentityKey& identity) noexcept {
    return RecaptureGenericWindowIdentity(identity);
}

static void PickerProductHideExactPopup(void*) noexcept {
    HidePicker(PickerHideDisposition::DismissSession);
}

static HRESULT PickerProductSwitchExactDesktop(
        void*,const GUID& destination,bool& invoked) noexcept {
    return SwitchDesktopWithForegroundHandoff(destination,invoked);
}

static bool PickerProductReadExactCurrentDesktop(
        void*,GUID& current) noexcept {
    PickerTraceCurrentDesktopFacts facts;
    current=CurrentDesktopGuid(&facts);
    EmitPickerTraceCurrentDesktopFacts(0,0,facts);
    return facts.validity==PickerReadValidity::Valid &&
           !GuidIsZero(current);
}

static PickerExactActivationRouteValidation
PickerProductValidateExactActivationRoute(
        void*,const PickerExactActivationRequest& request) noexcept {
    const HWND target=reinterpret_cast<HWND>(request.hwnd);
    GUID rawDesktop{};
    HRESULT rawResult=E_NOINTERFACE;
    if(target && g_vdmDoc){
        try {
            rawResult=g_vdmDoc->GetWindowDesktopId(
                target,&rawDesktop);
        } catch(...) { rawResult=E_FAIL; }
    }
    EmitPickerTraceHResult(
        PickerTraceApiKind::GetWindowDesktopIdTarget,
        0,0,rawResult,target!=nullptr,target,GUID{},rawDesktop);

    BOOL onCurrentDesktop=FALSE;
    HRESULT membershipResult=E_NOINTERFACE;
    if(target && g_vdmDoc){
        try {
            membershipResult=
                g_vdmDoc->IsWindowOnCurrentVirtualDesktop(
                    target,&onCurrentDesktop);
        } catch(...) { membershipResult=E_FAIL; }
    }

    std::vector<DeskRec> desktops;
    std::string desktopError;
    bool desktopSnapshotReady=false;
    try {
        desktopSnapshotReady=CurrentDesktops(
            desktops,&desktopError);
    } catch(...) { desktopSnapshotReady=false; }
    const bool rawDesktopExists=desktopSnapshotReady &&
        ConcreteDesktopExists(rawDesktop,desktops,DeskGuid);
    const TargetDesktopRoute freshRoute=DecideTargetDesktopRoute(
        rawResult,!GuidIsZero(rawDesktop),rawDesktopExists,
        membershipResult,onCurrentDesktop!=FALSE);

    TargetMobilityDecision mobilityDecision;
    bool pinServiceAvailableForPolicy=true;
    if(PickerExactActivationUsesVisualRoute(request)){
        IApplicationView* rawView=nullptr;
        HRESULT viewResult=E_NOINTERFACE;
        if(g_avc){
            try {
                viewResult=g_avc->GetViewForHwnd(
                    target,&rawView);
            } catch(...) { viewResult=E_FAIL; }
        }
        EmitPickerTraceHResult(
            PickerTraceApiKind::GetViewForHwnd,
            0,0,viewResult,g_avc!=nullptr,target);
        ScopedComPtr<IApplicationView> view(rawView);
        TargetMobilityProbeFacts mobilityFacts;
        mobilityDecision=QueryTargetWindowMobility(
            request.identity,freshRoute,
            SUCCEEDED(viewResult)?view.get():nullptr,
            mobilityFacts);
        pinServiceAvailableForPolicy=
            g_pinnedApps!=nullptr || !view;
    }
    return ValidatePickerExactActivationRoute(
        request,SUCCEEDED(rawResult),rawDesktop,
        rawDesktopExists,SUCCEEDED(membershipResult),
        onCurrentDesktop!=FALSE,mobilityDecision.disposition,
        pinServiceAvailableForPolicy);
}

static HWND PickerProductGetForegroundWindow(void*) noexcept {
    return GetForegroundWindow();
}

static DWORD PickerProductGetWindowThread(
        void*,HWND window) noexcept {
    DWORD ignored=0;
    return window
        ?GetWindowThreadProcessId(window,&ignored):0;
}

static DWORD PickerProductGetCurrentThread(void*) noexcept {
    return GetCurrentThreadId();
}

static BOOL PickerProductAttachExactInput(
        void*,DWORD source,DWORD destination,BOOL attach) noexcept {
    return AttachThreadInput(source,destination,attach);
}

static BOOL PickerProductIsExactTargetIconic(
        void*,HWND target) noexcept {
    return IsIconic(target);
}

static BOOL PickerProductRestoreExactTarget(
        void*,HWND target,int command) noexcept {
    return ShowWindow(target,command);
}

static BOOL PickerProductSetExactForeground(
        void*,HWND target) noexcept {
    return SetForegroundWindow(target);
}

static PickerTraceExactActivationResult MapPickerExactActivationResult(
        PickerExactActivationCallOutcome outcome) noexcept {
    switch(outcome){
    case PickerExactActivationCallOutcome::RejectKeepPopup:
        return PickerTraceExactActivationResult::RejectKeepPopup;
    case PickerExactActivationCallOutcome::SwitchOnly:
        return PickerTraceExactActivationResult::SwitchOnly;
    case PickerExactActivationCallOutcome::IdentityLost:
        return PickerTraceExactActivationResult::IdentityLost;
    case PickerExactActivationCallOutcome::DesktopMismatch:
        return PickerTraceExactActivationResult::DesktopMismatch;
    case PickerExactActivationCallOutcome::GlobalMembershipLost:
        return PickerTraceExactActivationResult::GlobalMembershipLost;
    case PickerExactActivationCallOutcome::ForegroundRejected:
        return PickerTraceExactActivationResult::ForegroundRejected;
    case PickerExactActivationCallOutcome::ExactForeground:
        return PickerTraceExactActivationResult::ExactForeground;
    }
    return PickerTraceExactActivationResult::RejectKeepPopup;
}

static int FindPickerTileByGuid(const GUID& guid) noexcept {
    if(GuidIsZero(guid)) return -1;
    for(size_t index=0;index<g_tiles.size();++index)
        if(GuidEq(g_tiles[index].guid,guid))
            return static_cast<int>(index);
    return -1;
}

static void GoToDesktop(int idx) noexcept;

static PickerTraceExactActivationResult ActivateExactPickerRow(
        const PickerRowActionSnapshot& row) noexcept {
    PickerExactActivationRequest request=
        PickerExactActivationFromRow(row);
    PickerTraceExactActivationEvent traceEvent;
    traceEvent.activationId=g_pickerTrace.nextCorrelationId();
    traceEvent.destination=request.displayedDesktop;
    traceEvent.visualRoute=
        PickerExactActivationUsesVisualRoute(request);
    const int destinationIndex=
        FindPickerTileByGuid(request.displayedDesktop);
    traceEvent.tileIndex=destinationIndex;
    bool destinationExists=false;
    if(destinationIndex>=0){
        try {
            const ScopedComPtr<IVirtualDesktop> destination=
                GetDesktopByGuid(request.displayedDesktop);
            destinationExists=static_cast<bool>(destination);
        } catch(...) { destinationExists=false; }
    }
    const bool presentationCurrent=PickerRowPresentationCurrent(
        row,g_picker.modelGeneration,g_picker.rowLayoutEpoch);

    bool identityUpgraded=false;
    if(presentationCurrent &&
       request.admission==PickerRowAdmission::Verified){
        identityUpgraded=request.hwnd==request.identity.hwnd &&
            SameIdentity(request.identity,request.identity) &&
            RecaptureGenericWindowIdentity(request.identity)==
                WindowIdentityRecapture::Match;
    } else if(presentationCurrent && request.hwnd!=0){
        const WindowIdentityKey upgraded=CapturePickerWindowIdentity(
            reinterpret_cast<HWND>(request.hwnd));
        if(SameIdentity(upgraded,upgraded) &&
           upgraded.hwnd==request.hwnd){
            request.identity=upgraded;
            request.admission=PickerRowAdmission::Verified;
            identityUpgraded=true;
        }
    }
    const bool destinationIsCurrent=
        !GuidIsZero(g_picker.currentDesktop) &&
        GuidEq(g_picker.currentDesktop,request.displayedDesktop);
    const PickerExactActivationDecision decision=
        DecidePickerExactActivation(
            destinationExists,presentationCurrent,
            identityUpgraded,destinationIsCurrent);
    traceEvent.decision=decision;
    const auto finish=[&](PickerTraceExactActivationResult result) noexcept {
        traceEvent.result=result;
        g_pickerTrace.emit(traceEvent);
        g_pickerTrace.flushBoundary();
        return result;
    };
    if(decision==PickerExactActivationDecision::RejectKeepPopup)
        return finish(
            PickerTraceExactActivationResult::RejectKeepPopup);
    if(decision==PickerExactActivationDecision::SwitchOnly){
        const PickerTraceExactActivationResult result=
            presentationCurrent &&
            row.admission==PickerRowAdmission::Verified &&
            !identityUpgraded
            ?PickerTraceExactActivationResult::IdentityLost
            :PickerTraceExactActivationResult::SwitchOnly;
        GoToDesktop(destinationIndex);
        return finish(result);
    }

    PickerExactActivationProductContext context;
    context.activationId=traceEvent.activationId;
    PickerExactActivationCallOps ops;
    ops.context=&context;
    ops.recaptureIdentity=PickerProductRecaptureExactIdentity;
    ops.hidePopup=PickerProductHideExactPopup;
    ops.switchDesktop=PickerProductSwitchExactDesktop;
    ops.readCurrentDesktop=PickerProductReadExactCurrentDesktop;
    ops.validateRoute=PickerProductValidateExactActivationRoute;
    ops.getForegroundWindow=PickerProductGetForegroundWindow;
    ops.getWindowThreadProcessId=PickerProductGetWindowThread;
    ops.getCurrentThreadId=PickerProductGetCurrentThread;
    ops.attachThreadInput=PickerProductAttachExactInput;
    ops.isIconic=PickerProductIsExactTargetIconic;
    ops.showWindow=PickerProductRestoreExactTarget;
    ops.setForegroundWindow=PickerProductSetExactForeground;
    const PickerExactActivationCallResult result=
        ExecutePickerExactActivationCalls(decision,request,ops);
    return finish(MapPickerExactActivationResult(result.outcome));
}

// Переключиться на десктоп. SwitchDesktop = слот 6 vtable (совпадает на 23H2/24H2).
// Фокус-данс через Progman, как в референсе MScholtes: без него система может
// «вернуть» исходный десктоп из-за активного окна -> переключение уходило не туда.
static void GoToDesktop(int idx) noexcept {
    if(g_picker.controlledTransition()) return;
    if(idx<0||idx>=(int)g_tiles.size())return;
    HidePicker(PickerHideDisposition::DismissSession);
    bool invoked=false;
    (void)SwitchDesktopWithForegroundHandoff(
        g_tiles[idx].guid,invoked);
}
// Клик = переключение на десктоп; Ctrl = перенести активное окно туда.
static void Activate(int idx,bool ctrlMove,
                     PickerTraceActivationSource source) noexcept {
    const uint64_t activationId=g_pickerTrace.nextCorrelationId();
    (void)DispatchPickerActivation(
        activationId,source,g_picker.controlledTransition(),idx,
        static_cast<int>(g_tiles.size()),ctrlMove,
        [](uint64_t id,PickerTraceActivationSource activationSource,
           bool ctrl,int tileIndex) noexcept {
            PickerTraceActivationRequestEvent event;
            event.activationId=id;
            event.source=activationSource;
            event.ctrl=ctrl;
            event.tileIndex=tileIndex;
            g_pickerTrace.emit(event);
        },[](int index) noexcept {
            return SetPickerSelectionCurrent(index);
        },[]() noexcept {
            (void)RefreshPickerPaintCache();
        },[](int index) noexcept {
            GoToDesktop(index);
        },[](int index,uint64_t id) noexcept {
            if(index<0 || index>=static_cast<int>(g_tiles.size()))
                return;
            PickerActionRequest request;
            request.intent=PickerActionIntent::MoveAndFollow;
            request.destination=g_tiles[index].guid;
            request.popupActiveTarget=g_picker.activeWindow;
            request.ctrlAtDown=true;
            request.activationId=id;
            (void)BeginPickerAction(request);
        },[](uint64_t id,PickerTraceActivationResult result) noexcept {
            PickerTraceActivationResultEvent event;
            event.activationId=id;
            event.result=result;
            g_pickerTrace.emit(event);
        });
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
            if(!PickerUiActionAllowed(
                    g_picker,PickerUiAction::Settings)) return 0;
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
    g_settings=CreateWindowExW(0,L"VdeSettings",L"Settings - Virtual Desktop Extension",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_settings){ ShowWindow(g_settings,SW_SHOW); SetForegroundWindow(g_settings); }
}

// --------------------------- About window ------------------------------------
static HWND g_about=nullptr;
static void AboutCopy(HWND hwnd){
    std::wstring s=std::wstring(L"Virtual Desktop Extension v")+APP_VERSION+L" | info@conus.vision | Windows build "+std::to_wstring(GetWindowsBuild());
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
        std::wstring title=std::wstring(L"Virtual Desktop Extension for Windows 11  \x2014  v")+APP_VERSION;
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
    g_about=CreateWindowExW(0,L"VdeAbout",L"About - Virtual Desktop Extension",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
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
    g_compat=CreateWindowExW(WS_EX_TOPMOST,L"VdeCompat",L"Virtual Desktop Extension \x2014 compatibility issue",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
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
L"- Auto-save & auto-restore layout: the utility watches your browsers and restores the layout automatically at startup and about 20 seconds after a browser launches. A closed Firefox, Chrome, or Edge window keeps its remembered virtual desktop for 30 days. If it reappears before expiry, VDE restores it before updating the saved layout.\r\n"
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
    g_help=CreateWindowExW(0,L"VdeHelp",L"Help - Virtual Desktop Extension",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,sx,sy,ww,wh,nullptr,nullptr,g_inst,nullptr);
    if(g_help){ ShowWindow(g_help,SW_SHOW); SetForegroundWindow(g_help); }
}

static bool OpenPickerFooterLink(
        HWND owner,const PickerFooterActivation& activation) noexcept {
    PickerFooterActivation exact=activation;
    if(exact.link==PickerFooterLink::Repository) exact.url=REPO_URL;
    else if(exact.link==PickerFooterLink::ConusVision) exact.url=CONUS_URL;
    return DispatchPickerFooterActivation(exact,
        [&](const wchar_t* url)->intptr_t {
            const HINSTANCE opened=ShellExecuteW(
                owner,L"open",url,nullptr,nullptr,SW_SHOWNORMAL);
            return reinterpret_cast<intptr_t>(opened);
        },[](){
            Balloon(L"The selected project link could not be opened.");
        });
}

static LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    if(g_pickerDurableKickPending && !g_pickerPumpActive &&
       !g_pickerShutdownDrain && msg!=WM_PICKER_TRANSITION &&
       g_runtimeQuiescence.acceptsDispatch())
        PumpPickerTransitionWork();
    if(msg!=WM_TIMER && msg!=WM_PICKER_SEARCH_RETRY &&
       !g_pickerShutdownDrain &&
       !PickerInteractionBusy(g_picker,g_pickerGesture) &&
       g_runtimeQuiescence.acceptsDispatch() &&
       PickerTabSearchRetryDeliveryKickNeeded(
           g_pickerTabSearchCache,g_picker.modelGeneration,
           g_picker.searchText))
        SchedulePickerTabSearchRetry();
    switch(msg){
    case WM_HOTKEY: ShowPicker(CapturePickerTarget()); return 0;
    case WM_CLOSE:
        if(RoutePickerClose(g_picker)==PickerCloseRoute::Hide)
            HidePicker(PickerHideDisposition::DismissSession);
        return 0;
    case WM_PICKER_TRANSITION:
        if(g_runtimeQuiescence.acceptsDispatch())
            PumpPickerTransitionWork();
        return 0;
    case WM_PICKER_SEARCH_RETRY:
        if(!g_runtimeQuiescence.acceptsDispatch()) return 0;
        if(AcquirePickerTabSearchRetryPostLeaseWhenIdle(
                g_pickerTabSearchCache,
                PickerInteractionBusy(g_picker,g_pickerGesture),
                g_picker.modelGeneration,g_picker.searchText))
            EnsureTabSearch();
        return 0;
    case WM_PAINT:{
        ScopedPickerPaint paint(hwnd);
        HDC target=paint.get();
        if(!target) return 0;
        RECT client={0,0,0,0};
        if(!GetClientRect(hwnd,&client)) return 0;
        if(!g_pickerBuffer.ensure(target,client.right,client.bottom))
            return 0;
        Paint(target,g_pickerBuffer.get(),client);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:{
        const bool cacheReady=PickerPaintCacheMatches(
            g_picker,g_pickerPaintCache.generation);
        POINT point={0,0};
        if(cacheReady && GetCursorPos(&point) &&
           ScreenToClient(hwnd,&point) &&
           PickerFooterUsesHandCursor(HitCurrentPickerFooterLink(
               g_picker,g_pickerPaintCache.generation,
               g_pickerPaintCache.footer.layout,point))){
            HCURSOR hand=LoadCursorW(nullptr,IDC_HAND);
            if(hand){ SetCursor(hand); return TRUE; }
        }
        break;
    }
    case WM_CTLCOLOREDIT: if((HWND)lp==g_search){ HDC dc=(HDC)wp; SetTextColor(dc,CLR_TEXT); SetBkColor(dc,CLR_SEARCH);
        return reinterpret_cast<LRESULT>(g_searchBrush
            ?g_searchBrush:GetSysColorBrush(COLOR_WINDOW)); }
        return DefWindowProcW(hwnd,msg,wp,lp);
    case WM_KEYDOWN:{
        bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(wp==VK_CONTROL){ InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(wp==VK_ESCAPE){
            if(g_picker.controlledTransition()) RequestPickerCancellation();
            else if(g_pickerGesture.phase!=PickerPointerPhase::Idle)
                ResetPickerPointerGesture(hwnd,true);
            else HidePicker(PickerHideDisposition::DismissSession);
            return 0;
        }
        if(PickerInteractionBusy(g_picker,g_pickerGesture)) return 0;
        if(wp==VK_RETURN||wp==VK_SPACE){
            Activate(g_picker.selectedIndex,ctrl,
                     PickerTraceActivationSource::Keyboard);
            return 0;
        }
        if(wp>='1'&&wp<='9'){
            Activate(static_cast<int>(wp-'1'),ctrl,
                     PickerTraceActivationSource::Keyboard);
            return 0;
        }
        if(wp=='0'){
            Activate(9,ctrl,PickerTraceActivationSource::Keyboard);
            return 0;
        }
        if(wp==VK_LEFT){MoveSel(-1,0);return 0;} if(wp==VK_RIGHT){MoveSel(1,0);return 0;}
        if(wp==VK_UP){MoveSel(0,-1);return 0;} if(wp==VK_DOWN){MoveSel(0,1);return 0;}
        if(wp==VK_TAB){ bool sh=(GetKeyState(VK_SHIFT)&0x8000)!=0; int n=(int)g_tiles.size(); if(n){int selected=g_picker.selectedIndex; if(selected<0||selected>=n)selected=0; SetPickerSelectionCurrent((selected+(sh?-1:1)+n)%n); RefreshPickerPaintCache(); InvalidateRect(hwnd,nullptr,FALSE);} return 0; }
        return 0; }
    case WM_KEYUP:
        if(wp==VK_CONTROL) InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    case WM_LBUTTONDOWN:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        const bool ctrl=PickerMouseControlHeld(wp);
        PickerTraceMouseDownEvent mouseEvent;
        mouseEvent.rawWparam=static_cast<uint64_t>(wp);
        mouseEvent.x=pt.x;
        mouseEvent.y=pt.y;
        mouseEvent.ctrl=ctrl;
        mouseEvent.controlled=g_picker.controlledTransition();
        mouseEvent.gestureActive=
            g_pickerGesture.phase!=PickerPointerPhase::Idle;
        mouseEvent.searchActive=g_picker.searchActive;
        if(mouseEvent.controlled || mouseEvent.gestureActive){
            g_pickerTrace.emit(mouseEvent);
            return 0;
        }
        const bool cacheReady=PickerPaintCacheMatches(g_picker,g_pickerPaintCache.generation);
        const PickerFooterActivation footerActivation=
            ResolvePickerFooterActivation(
            g_picker,g_pickerPaintCache.generation,
            g_pickerPaintCache.footer.layout,pt);
        const bool clearSearchHit=cacheReady &&
            g_pickerPaintCache.clearButton.right>
                g_pickerPaintCache.clearButton.left &&
            PtInRect(&g_pickerPaintCache.clearButton,pt);
        RECT client={0,0,0,0};
        const bool searchHit=GetClientRect(hwnd,&client) &&
            PickerPointInRect(SearchBoxRect(client.right),pt);
        const int rowIndex=HitPickerRow(pt);
        int tileIndex=HitPickerTile(pt);
        if(rowIndex>=0 &&
           rowIndex<static_cast<int>(g_pickerPaintCache.hoverRows.size()))
            tileIndex=g_pickerPaintCache.hoverRows[
                rowIndex].snapshot.action.tileIndex;
        const PickerPointerActivation activation=
            ResolvePickerPointerActivation(
                footerActivation,clearSearchHit,searchHit,
                rowIndex,tileIndex);
        mouseEvent.target=activation.target;
        mouseEvent.tileIndex=activation.tileIndex;
        mouseEvent.rowIndex=activation.rowIndex;
        g_pickerTrace.emit(mouseEvent);
        if(DispatchPickerPointerActivation(activation,
            [&](const PickerFooterActivation& footer){
                OpenPickerFooterLink(hwnd,footer);
            },[&](){
                SetWindowTextW(g_search,L"");
                g_picker.searchActive=false;
                RefreshPickerPaintCache();
                SetFocus(g_search);
                InvalidateRect(hwnd,nullptr,FALSE);
            },[&](){
                g_picker.searchActive=true;
                RefreshPickerPaintCache();
                SetFocus(g_search);
                InvalidateRect(hwnd,nullptr,FALSE);
            },[&](int hitIndex,int rowTileIndex){
                if(hitIndex<0 ||
                   hitIndex>=static_cast<int>(
                       g_pickerPaintCache.hoverRows.size())) return;
                const PickerRowHitSnapshot& hitSnapshot=
                    g_pickerPaintCache.hoverRows[
                        hitIndex].snapshot;
                const PickerRowActionSnapshot row=
                    hitSnapshot.action;
                if(row.tileIndex!=rowTileIndex ||
                   !ArmPickerRowGesture(
                       g_pickerGesture,row,pt,ctrl,
                       g_picker.modelGeneration,
                       g_picker.rowLayoutEpoch)) return;
                EmitPickerGestureTrace(
                    g_pickerGesture,PickerPointerPhase::Idle,
                    PickerPointerPhase::Armed,
                    PickerGestureAction::None,false,-1);
                (void)CapturePickerDragPreview(hitSnapshot,pt);
                SetCapture(hwnd);
                const bool captureOwned=GetCapture()==hwnd;
                if(!captureOwned)
                    ResetPickerPointerGesture(hwnd,false);
            },[&](int index){
                if(g_picker.searchActive){
                    g_picker.searchActive=false;
                    RefreshPickerPaintCache();
                    InvalidateRect(hwnd,nullptr,FALSE);
                }
                Activate(index,ctrl,PickerTraceActivationSource::Mouse);
            })) return 0;
        if(g_picker.searchActive){
            g_picker.searchActive=false;
            RefreshPickerPaintCache();
            InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(g_picker.controlledTransition()) return 0;
        if(g_pickerGesture.phase!=PickerPointerPhase::Idle){
            RECT oldPreviewBounds={0,0,0,0};
            const bool oldPreviewVisible=
                CurrentPickerDragPreviewClientBounds(
                    hwnd,oldPreviewBounds);
            if(g_pickerDragPreview.captured)
                g_pickerDragPreview.pointer=pt;
            const PickerPointerGesture gestureFacts=g_pickerGesture;
            const PickerPointerPhase priorPhase=g_pickerGesture.phase;
            const int priorDropTileIndex=
                g_pickerGesture.dropTileIndex;
            const int candidateDropTile=HitPickerTile(pt);
            const PickerGestureAction gestureAction=
                UpdatePickerRowGesture(
                    g_pickerGesture,pt,
                    GetSystemMetrics(SM_CXDRAG),
                    GetSystemMetrics(SM_CYDRAG),
                    g_picker.modelGeneration,
                    g_picker.rowLayoutEpoch,
                    candidateDropTile);
            RECT newPreviewBounds={0,0,0,0};
            const bool newPreviewVisible=
                CurrentPickerDragPreviewClientBounds(
                    hwnd,newPreviewBounds);
            const RECT previewDirty=
                PickerDragPreviewDirtyBounds(
                    oldPreviewVisible,oldPreviewBounds,
                    newPreviewVisible,newPreviewBounds);
            if(gestureAction==PickerGestureAction::Cancel){
                EmitPickerGestureTrace(
                    gestureFacts,priorPhase,PickerPointerPhase::Idle,
                    PickerGestureAction::Cancel,false,
                    candidateDropTile);
                ResetPickerPointerGesture(hwnd,true,false);
                InvalidateRect(hwnd,nullptr,FALSE);
            } else {
                if(gestureAction==PickerGestureAction::DragStarted){
                    ResetPickerHoverState(
                        PickerHoverResetReason::ExplicitInvalidation);
                    EmitPickerGestureTrace(
                        gestureFacts,priorPhase,g_pickerGesture.phase,
                        gestureAction,true,candidateDropTile);
                }
                if(priorPhase!=g_pickerGesture.phase ||
                    priorDropTileIndex!=
                        g_pickerGesture.dropTileIndex)
                    InvalidateRect(hwnd,nullptr,FALSE);
                else if(previewDirty.right>previewDirty.left &&
                        previewDirty.bottom>previewDirty.top)
                    InvalidateRect(hwnd,&previewDirty,FALSE);
            }
            return 0;
        }
        const bool cacheReady=PickerPaintCacheMatches(g_picker,g_pickerPaintCache.generation);
        const PickerFooterLink footerHover=HitCurrentPickerFooterLink(
            g_picker,g_pickerPaintCache.generation,
            g_pickerPaintCache.footer.layout,pt);
        const PickerFooterMouseMoveEffects footerEffects=
            RoutePickerFooterMouseMove(
                g_pickerHoverState,footerHover,g_lastHoverRow!=-1);
        if(footerEffects.invalidateRowHover){
            InvalidateRect(hwnd,nullptr,FALSE);
        } else if(footerEffects.invalidateFooter){
            if(cacheReady)
                InvalidateRect(hwnd,
                    &g_pickerPaintCache.footer.layout.footer,FALSE);
            else
                InvalidateRect(hwnd,nullptr,FALSE);
        }
        if(PickerFooterSuppressesRowHover(footerHover)){
            if(footerEffects.resetRowTooltip)
                ResetPickerHoverTooltip();
            TRACKMOUSEEVENT tme={sizeof(tme)};
            tme.dwFlags=TME_LEAVE;
            tme.hwndTrack=hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }
        for(size_t index=0;index<g_tiles.size();++index){
            if(!PtInRect(&g_tiles[index].rc,pt)) continue;
            if(g_picker.selectedIndex!=static_cast<int>(index)){
                SetPickerSelectionCurrent(static_cast<int>(index));
                RefreshPickerPaintCache();
                InvalidateRect(hwnd,nullptr,FALSE);
            }
            break;
        }
        const bool rowCacheReady=PickerPaintCacheMatches(
            g_picker,g_pickerPaintCache.generation);
        const int hovRow=rowCacheReady?HitPickerRow(pt):-1;
        const PickerRowHoverUpdate rowHover=
            UpdatePickerRowHoverEvent(
                g_pickerHoverState,hovRow,
                rowCacheReady?g_pickerPaintCache.generation:0);
        if(rowHover.changed)
            InvalidateRect(hwnd,nullptr,FALSE);
        const bool tooltipHit=hovRow>=0 &&
            PtInRect(
                &g_pickerPaintCache.hoverRows[hovRow].snapshot.textRect,pt);
        if(tooltipHit &&
           g_pickerPaintCache.hoverRows[hovRow].snapshot.truncated &&
           g_tip){    // R9: full name on hover when truncated
            TrackPickerHoverTooltip(hwnd,g_pickerPaintCache.hoverRows[hovRow],
                hovRow,g_pickerPaintCache.generation,pt);
        } else if(g_lastHoverRow!=-1){ ResetPickerHoverTooltip(); }
        TRACKMOUSEEVENT tme={sizeof(tme)}; tme.dwFlags=TME_LEAVE; tme.hwndTrack=hwnd; TrackMouseEvent(&tme);
        return 0; }
    case WM_LBUTTONUP:{
        if(g_pickerGesture.phase==PickerPointerPhase::Idle) return 0;
        const PickerPointerGesture gestureFacts=g_pickerGesture;
        const POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        PickerRowActionSnapshot releaseRow;
        const int releaseRowIndex=HitPickerRow(pt);
        const PickerRowActionSnapshot* releaseRowPointer=nullptr;
        if(releaseRowIndex>=0 &&
           releaseRowIndex<static_cast<int>(
               g_pickerPaintCache.hoverRows.size())){
            releaseRow=g_pickerPaintCache.hoverRows[
                releaseRowIndex].snapshot.action;
            releaseRowPointer=&releaseRow;
        }
        const bool dragging=
            g_pickerGesture.phase==PickerPointerPhase::Dragging;
        const int destinationIndex=dragging
            ?g_pickerGesture.dropTileIndex
            :g_pickerGesture.row.tileIndex;
        const bool destinationExists=destinationIndex>=0 &&
            destinationIndex<static_cast<int>(g_tiles.size());
        const PickerGestureResolution resolution=
            ResolvePickerRowButtonUp(
                g_pickerGesture,releaseRowPointer,destinationExists,
                g_picker.modelGeneration,g_picker.rowLayoutEpoch);
        ResetPickerPointerGesture(hwnd,true,false);
        EmitPickerGestureTrace(
            gestureFacts,gestureFacts.phase,PickerPointerPhase::Idle,
            resolution.action,false,resolution.dropTileIndex);
        switch(resolution.action){
        case PickerGestureAction::Click:
            if(resolution.ctrlAtDown)
                Activate(resolution.row.tileIndex,true,
                         PickerTraceActivationSource::Mouse);
            else
                (void)ActivateExactPickerRow(resolution.row);
            break;
        case PickerGestureAction::SwitchOnly:
            (void)ActivateExactPickerRow(resolution.row);
            break;
        case PickerGestureAction::Drop:
            if(resolution.dropTileIndex>=0 &&
               resolution.dropTileIndex<
                   static_cast<int>(g_tiles.size())){
                PickerActionRequest request;
                request.intent=PickerActionIntent::RowMoveOnly;
                request.destination=
                    g_tiles[resolution.dropTileIndex].guid;
                request.hasRow=true;
                request.row=resolution.row;
                request.popupActiveTarget=g_picker.activeWindow;
                request.ctrlAtDown=resolution.ctrlAtDown;
                request.activationId=
                    g_pickerTrace.nextCorrelationId();
                (void)BeginPickerAction(request);
            }
            break;
        case PickerGestureAction::None:
        case PickerGestureAction::DragStarted:
        case PickerGestureAction::NoOp:
        case PickerGestureAction::Cancel:
            break;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        ResetPickerPointerGesture(hwnd,false);
        return 0;
    case WM_CANCELMODE:
        ResetPickerPointerGesture(hwnd,true);
        return 0;
    case WM_MOUSELEAVE:{
        const bool hoverChanged=
            g_pickerHoverState.footerLink!=PickerFooterLink::None ||
            g_pickerHoverState.hoveredRowIndex>=0;
        ResetPickerHoverState(PickerHoverResetReason::MouseLeave);
        if(hoverChanged)
            InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:{ POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&pt); int delta=GET_WHEEL_DELTA_WPARAM(wp);   // R8: scroll a tile's window list
        if(PickerInteractionBusy(g_picker,g_pickerGesture)) return 0;
        for(size_t tileIndex=0;tileIndex<g_tiles.size();++tileIndex){
            const Tile& t=g_tiles[tileIndex];
            if(!PtInRect(&t.rc,pt)) continue;
            const int maximum=PickerTileMaxScroll(t);
            const int next=AdvancePickerScroll(t.scroll,maximum,delta);
            if(next!=t.scroll &&
               PublishPickerModelPaintUpdate(
                    [&](std::vector<Tile>& tiles,PickerState& state){
                        if(tileIndex>=tiles.size()) return false;
                        Tile& stagedTile=tiles[tileIndex];
                        stagedTile.scroll=next;
                        const auto saved=state.scrollByDesktop.find(
                            stagedTile.guidKey);
                        if(saved==state.scrollByDesktop.end())
                            state.scrollByDesktop.emplace(
                                stagedTile.guidKey,next);
                        else
                            saved->second=next;
                        return true;
                    },true))
                PreloadVisiblePickerIcons();
            InvalidateRect(hwnd,nullptr,FALSE);
            break;
        }
        return 0; }
    case WM_COMMAND:                                            // R7: live-filter as the search text changes
        if(PickerInteractionBusy(g_picker,g_pickerGesture)) return 0;
        if(g_search && (HWND)lp==g_search && HIWORD(wp)==EN_CHANGE){
            int n=GetWindowTextLengthW(g_search); std::wstring s(n+1,0); GetWindowTextW(g_search,&s[0],n+1); s.resize(wcslen(s.c_str()));
            std::wstring searchText=LowerW(s);
            if(ApplyPickerSearchText(s,searchText)){
                PreloadVisiblePickerIcons();
                if(!g_picker.searchText.empty()) EnsureTabSearch();
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
        if(wp==TIMER_PICKER_ICON_PRELOAD){
            KillTimer(hwnd,TIMER_PICKER_ICON_PRELOAD);
            g_pickerIconPreloadTimerArmed=false;
            if(!IsWindowVisible(hwnd)){
                CancelPickerIconPreload(hwnd);
                return 0;
            }
            if(!g_pickerIconPreloadGate.dirty()) return 0;
            PreloadVisiblePickerIcons(true);
            InvalidateRect(hwnd,nullptr,FALSE);
            return 0;
        }
        if(wp==TIMER_PICKER_SEARCH_RETRY){
            KillTimer(hwnd,TIMER_PICKER_SEARCH_RETRY);
            if(PickerTabSearchRetryDeliveryReadyWhenIdle(
                    g_pickerTabSearchCache,
                    PickerInteractionBusy(g_picker,g_pickerGesture),
                    g_picker.modelGeneration,g_picker.searchText))
                EnsureTabSearch();
            return 0;
        }
        if(wp==TIMER_PICKER_TRANSITION){
            if(g_picker.controlledTransition() || g_pickerEffectScheduled ||
               g_pickerObservationKick.pending ||
               g_pickerTerminalizationPending){
                PumpPickerTransitionWork();
            } else {
                if(g_pickerGesture.phase!=PickerPointerPhase::Idle)
                    return 0;
                if(PickerTabSearchRetryDeliveryKickNeeded(
                        g_pickerTabSearchCache,g_picker.modelGeneration,
                        g_picker.searchText)){
                    SchedulePickerTabSearchRetry();
                    if(PickerTabSearchRetryDeliveryKickNeeded(
                            g_pickerTabSearchCache,
                            g_picker.modelGeneration,g_picker.searchText))
                        return 0;
                }
                KillTimer(hwnd,TIMER_PICKER_TRANSITION);
                RefreshPickerHighlightsLightweight();
                ArmPickerIdleRefresh();
            }
            return 0;
        }
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
    case WM_ACTIVATE:
        if(LOWORD(wp)==WA_INACTIVE &&
           !PickerInteractionBusy(g_picker,g_pickerGesture))
            HidePicker(PickerHideDisposition::DismissSession);
        return 0;
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
            if(g_picker.controlledTransition() && cmd!=209) return 0;
            if(cmd==200)ShowPicker(std::move(pickerTarget));
            else if(cmd==201)StartManualSave();
            else if(cmd==202)StartManualRestore(true);
            else if(cmd==204)StartManualRestore(false);
            else if(cmd==203)OpenSettings();
            else if(cmd==206)OpenHelp();
            else if(cmd==205)OpenAbout();
            else if(cmd==209){
                if(!RunTrayExit(
                        [](){
                            DrainPickerForShutdown();
                            return FinalizeAutoLayout();
                        },
                        [=](){ return DestroyWindow(hwnd)!=FALSE; },
                        [](){ g_checkpointController.finalization.reopen(); }))
                    ReportStorageError(
                        L"Exit could not be completed. VDE is still running; retry Exit.");
            }
        } else if(LOWORD(lp)==WM_LBUTTONDBLCLK)
            ShowPicker(CapturePickerTarget());
        return 0;
    case WM_QUERYENDSESSION:
        ResetPickerPointerGesture(hwnd,true);
        DrainPickerForShutdown();
        CheckpointAutoLayout(CheckpointReason::QueryEndSession);
        return TRUE;
    case WM_ENDSESSION:
        if(wp!=0){
            DrainPickerForShutdown();
            EndPickerVisualSessionRuntime();
        } else {
            ResetPickerPointerGesture(hwnd,true);
        }
        FinalizeSessionAndQuiesce(wp!=0,
            [](){ return FinalizeAutoLayout(); },
            [=](){ return QuiesceRuntime(hwnd); });
        return 0;
    case WM_DESTROY:
        DrainPickerForShutdown();
        EndPickerVisualSessionRuntime();
        FinalizeAutoLayout();
        QuiesceRuntime(hwnd);
        TrayRemove(); UnregisterHotKey(hwnd,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        noexcept {
    try { return WndProcImpl(hwnd,msg,wp,lp); }
    catch(...) { return 0; }
}

static bool CleanupUiResources() noexcept {
    g_pickerBuffer.reset();
    g_pickerDragBuffer.reset();
    if(!g_pickerBuffer.released()) return false;
    if(!g_pickerDragBuffer.released()) return false;
    if(!ClearWindowIconCache()) return false;
    bool released=true;
    HFONT* fonts[]={
        &g_uiFont,&g_fPT,&g_fPN,&g_fPI,&g_fPX,&g_searchFont
    };
    for(HFONT* font : fonts){
        if(*font){
            if(DeleteObject(*font)) *font=nullptr;
            else released=false;
        }
    }
    if(g_searchBrush){
        if(DeleteObject(g_searchBrush)) g_searchBrush=nullptr;
        else released=false;
    }
    size_t retained=0;
    for(HICON icon : g_ownedIcons){
        if(!icon) continue;
        if(DestroyIcon(icon)) continue;
        g_ownedIcons[retained++]=icon;
        released=false;
    }
    if(retained==0) g_ownedIcons.clear();
    else g_ownedIcons.resize(retained);
    if(!g_failedOwnedIconReleases.clear(
            [](HICON icon)->bool {
                return !icon || DestroyIcon(icon)!=FALSE;
            })) released=false;
    g_nid.hIcon=nullptr;
    return released && g_ownedIcons.empty() &&
        g_failedOwnedIconReleases.size()==0;
}

static bool DestroyUiWindow(HWND& window) noexcept {
    HWND owned=window;
    if(!owned) return true;
    if(IsWindow(owned) && !DestroyWindow(owned)) return false;
    window=nullptr;
    return true;
}

static bool DestroyAllUiWindows() noexcept {
    bool destroyed=true;
    if(!DestroyUiWindow(g_settings)) destroyed=false;
    if(!DestroyUiWindow(g_about)) destroyed=false;
    if(!DestroyUiWindow(g_help)) destroyed=false;
    if(!DestroyUiWindow(g_compat)) destroyed=false;
    if(!DestroyUiWindow(g_main)) destroyed=false;
    if(!destroyed) return false;
    g_search=nullptr;
    g_tip=nullptr;
    g_searchOrigProc=nullptr;
    return true;
}

static bool UnregisterUiClass(const wchar_t* name) noexcept {
    if(!g_inst) return true;
    SetLastError(ERROR_SUCCESS);
    if(UnregisterClassW(name,g_inst)) return true;
    return GetLastError()==ERROR_CLASS_DOES_NOT_EXIST;
}

static bool UnregisterUiClasses() noexcept {
    bool unregistered=true;
    if(!UnregisterUiClass(L"VdeSettings")) unregistered=false;
    if(!UnregisterUiClass(L"VdeAbout")) unregistered=false;
    if(!UnregisterUiClass(L"VdeCompat")) unregistered=false;
    if(!UnregisterUiClass(L"VdeHelp")) unregistered=false;
    if(!UnregisterUiClass(L"VdeWindow")) unregistered=false;
    return unregistered;
}

static void ShutdownUi() noexcept {
    if(g_uiShutdownComplete) return;
    const bool completed=g_uiTeardown.run(
        [](){ return DestroyAllUiWindows(); },
        [](){ return UnregisterUiClasses(); },
        [](){ return CleanupUiResources(); });
    EndPickerVisualSessionRuntime();
    g_uiShutdownComplete=completed;
}

class ScopedUiShutdown {
public:
    ScopedUiShutdown()=default;
    ScopedUiShutdown(const ScopedUiShutdown&)=delete;
    ScopedUiShutdown& operator=(const ScopedUiShutdown&)=delete;
    ~ScopedUiShutdown() noexcept { ShutdownUi(); }
};

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
    ScopedUiShutdown shutdown;
    int runResult=0;
    g_inst=hInst;
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_HOTKEY_CLASS|ICC_STANDARD_CLASSES|ICC_LINK_CLASS|ICC_BAR_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&icc);
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
        if(wait==WAIT_FAILED){
            const DWORD messageError=GetLastError();
            runResult=4;
            ReportStorageError(
                L"The Windows message pump failed (error "+
                std::to_wstring(messageError)+L").");
            break;
        }
        if(!PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) continue;
        if(msg.message==WM_QUIT) break;
        if(g_settings && IsDialogMessageW(g_settings,&msg)) continue;
        if(g_about && IsDialogMessageW(g_about,&msg)) continue;
        if(g_compat && IsDialogMessageW(g_compat,&msg)) continue;
        if(g_help && IsDialogMessageW(g_help,&msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return runResult;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int){
    SetProcessDPIAware();
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    const VdeLaunchOptions launch=ParseVdeLaunchOptions(argc,argv);
    if(argv) LocalFree(argv);
    const std::wstring cmd=launch.command;
    const bool cli=launch.cli;
    const bool tracePicker=launch.tracePicker;

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
            (void)g_pickerTrace.start(tracePicker,APP_VERSION);
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
            g_pickerTrace.close();
        }
        ReleaseServices();
        return dispatchResult;
    };

    int rc=1;
    try { rc=RunWithTrayInstanceScope(
        cli,acquireTrayInstance,dispatch,releaseTrayInstance); }
    catch(...) {
        ShutdownUi();
        ReleaseServices();
        g_pickerTrace.close();
        CoUninitialize();
        return 1;
    }
    ShutdownUi();
    g_pickerTrace.close();
    CoUninitialize();
    return rc;
}
