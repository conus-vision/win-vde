#pragma once

#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "picker_state.hpp"

struct VdeLaunchOptions {
    std::wstring command;
    bool cli=false;
    bool tracePicker=false;
};

VdeLaunchOptions ParseVdeLaunchOptions(
    int argc,const wchar_t* const* argv) noexcept;

enum class PickerTraceOpenResult : uint8_t {
    Shown, Degraded, ControlledTransition, WorkAreaUnavailable,
    ModelUnavailable, AdjustRectFailed, OuterSizeInvalid,
    PositionFailed, ClientRectFailed, PaintCacheFailed
};

enum class PickerTraceDesktopSnapshotStatus : uint8_t {
    NotAttempted, Complete, DesktopServiceMissing,
    GetDesktopsFailed, GetCountFailed, InvalidCount, GetAtFailed,
    GetIdFailed, InvalidGuid, AllocationFailure, Exception
};

enum class PickerTraceAltTabReason : uint8_t {
    Eligible, NotVisible, FirstTitleUnavailable,
    ToolWindow, RootOwnerMismatch
};

enum class PickerTraceEnumDecision : uint8_t {
    SkipNotVisible, SkipFirstTitleUnavailable, SkipToolWindow,
    SkipRootOwnerMismatch, SkipDesktopServiceMissing,
    SkipDesktopLookupFailed, SkipDesktopGuidZero,
    SkipDesktopTileMissing, SkipSecondTitleUnavailable,
    SkipSecondTitleReadFailed, SkipIdentityLost,
    DisplayOnlyPidUnavailable, DisplayOnlyProcessStartUnavailable,
    DisplayOnlyIdentityIndeterminate, Verified,
    AllocationFailure, GlobalSnapshotFailure, Count
};

enum class PickerTraceActivationSource : uint8_t { Mouse, Keyboard };

enum class PickerTraceActivationResult : uint8_t {
    AlreadyControlled, InvalidTile, SelectionPublicationFailed,
    RoutedPlainSwitch, DispatchedMoveEntry
};

enum class PickerTraceMoveBeginReason : uint8_t {
    Accepted, AlreadyControlled, InvalidIndex, SelectionIndexMismatch,
    SelectionDesktopMismatch, MainWindowMissing, DesktopManagerMissing,
    DesktopDocumentMissing, TargetMismatch, TargetWindowMissing,
    TargetWindowInvalid, DestinationZero, DestinationLookupFailed,
    CurrentDesktopUnavailable, PopupDesktopUnavailable, FastCaptureFailed,
    TargetDesktopUnavailable, IdentityMismatch, IdentityLost,
    IdentityIndeterminate, AcceptedPlanConflict, BoundRecordConflict,
    SafeOriginUnavailable, AcceptedOperationMissing,
    OperationClaimStageFailed, ReservationHandoffFailed,
    PendingAssociationStageFailed, ProvisionalInsertFailed,
    NoInitialEffect
};

enum class PickerTraceEffectStage : uint8_t {
    Queue, Execute, Observation, Reduce
};

enum class PickerTraceApiKind : uint8_t {
    GetViewForHwnd, MoveViewToDesktop, MoveWindowToDesktop,
    GetWindowDesktopIdTarget, GetWindowDesktopIdPopup,
    GetWindowDesktopIdCapture, GetDesktops, GetCount, GetAt, GetId,
    GetCurrentDesktop, AttachDesktopInput, AttachForegroundInput,
    SetForegroundWindow, DetachForegroundInput, DetachDesktopInput,
    SwitchDesktop, ShowWindowProgmanCleanup
};

enum class PickerTraceDesktopLookupStage : uint8_t {
    ValidateRequest, GetDesktops, GetCount, GetAt, GetId,
    Match, NotFound, Exception
};

enum class PickerTraceDesktopLookupUse : uint8_t {
    MoveEntryDestination, MoveTargetDestination, MovePopupDestination,
    SwitchPrecheckDestination, SwitchHandoffDestination
};

enum class PickerTraceRawResultKind : uint8_t {
    HResult, Win32Bool, PreviousVisibility, NoExtendedError
};

enum class PickerTraceDeliveryRoute : uint8_t {
    None, Posted, TimerArmed, InlineFallback, DelayedTimer,
    DurableExternalKick, ShutdownDrain
};

enum class PickerTraceTerminalizationReason : uint8_t {
    Completed, TerminalNotAcknowledged, PendingEffect,
    ReservationReleaseException, ReservationNotReleased,
    RuntimeKeyMissing, RuntimeNotReady, FinalizeStateFailed
};

enum class PickerTraceTerminalOutcome : uint8_t {
    Succeeded, Cancelled, Failed
};

enum class PickerTraceRollbackTrigger : uint8_t {
    None, Cancellation, TargetMove, TargetVerify,
    PopupMove, PopupVerify, DesktopSwitch,
    IdentityLost, IdentityIndeterminate, ReadUnavailable,
    RetryBudgetExhausted, QueueConflict, Exception
};

enum class PickerTraceDiagnosticCode : uint8_t {
    None, ApiRejected, VerificationMismatch, IdentityLost,
    IdentityIndeterminate, ReadUnavailable, RetryBudgetExhausted,
    QueueConflict, Exception, Cancelled
};

enum class PickerTraceReservationExceptionStage : uint8_t {
    None, FirstDecision, CheckpointCallback, Refind,
    SecondDecision, Erase
};

struct PickerTraceAltTabFacts {
    bool visibleObserved=false;
    bool visible=false;
    bool firstTitleObserved=false;
    int firstTitleLength=0;
    DWORD firstTitleError=ERROR_SUCCESS;
    bool exStyleObserved=false;
    LONG_PTR exStyle=0;
    DWORD exStyleError=ERROR_SUCCESS;
    bool rootOwnerObserved=false;
    HWND rootOwner=nullptr;
    PickerTraceAltTabReason reason=
        PickerTraceAltTabReason::FirstTitleUnavailable;
};

struct PickerTraceAltTabOps {
    void* context=nullptr;
    BOOL (*isVisible)(void*,HWND)=nullptr;
    int (*titleLength)(void*,HWND,DWORD&)=nullptr;
    LONG_PTR (*extendedStyle)(void*,HWND,DWORD&)=nullptr;
    HWND (*rootOwner)(void*,HWND)=nullptr;
};

struct PickerTraceDesktopSnapshotFacts {
    PickerTraceDesktopSnapshotStatus status=
        PickerTraceDesktopSnapshotStatus::NotAttempted;
    HRESULT result=E_NOTIMPL;
    int index=-1;
    uint32_t count=0;
};

PickerTraceAltTabReason DecidePickerTraceAltTabReason(
    bool visible,int titleLength,uint64_t exStyle,
    uintptr_t hwnd,uintptr_t rootOwner) noexcept;
PickerTraceAltTabFacts ObservePickerTraceAltTabWindow(
    HWND hwnd,const PickerTraceAltTabOps& ops) noexcept;
PickerTraceEnumDecision DecidePickerTraceEnumDecision(
    PickerTraceAltTabReason altTabReason,bool desktopServiceAvailable,
    HRESULT desktopResult,bool desktopGuidAvailable,int tileIndex,
    int secondTitleLength,int secondTitleCopied,bool pidAvailable,
    bool processStartAvailable,
    WindowIdentityRecapture recapture) noexcept;

class PickerTraceSafeClassName {
public:
    PickerTraceSafeClassName() noexcept=default;
    bool available() const noexcept { return available_; }
    uint16_t length() const noexcept { return length_; }
    const wchar_t* data() const noexcept { return value_.data(); }
private:
    friend PickerTraceSafeClassName MakePickerTraceSafeClassName(
        const wchar_t*,int) noexcept;
    std::array<wchar_t,129> value_{};
    uint16_t length_=0;
    bool available_=false;
};

class PickerTraceSafeImageBasename {
public:
    PickerTraceSafeImageBasename() noexcept=default;
    bool available() const noexcept { return available_; }
    uint16_t length() const noexcept { return length_; }
    const wchar_t* data() const noexcept { return value_.data(); }
private:
    friend PickerTraceSafeImageBasename MakePickerTraceSafeImageBasename(
        const wchar_t*,int) noexcept;
    std::array<wchar_t,261> value_{};
    uint16_t length_=0;
    bool available_=false;
};

PickerTraceSafeClassName MakePickerTraceSafeClassName(
    const wchar_t* value,int length) noexcept;
PickerTraceSafeImageBasename MakePickerTraceSafeImageBasename(
    const wchar_t* value,int length) noexcept;

struct PickerTraceEnvelope {
    std::array<unsigned char,16> session{};
    uint64_t seq=0;
    uint64_t ms=0;
};

struct PickerTraceCaptureEvent {
    uintptr_t hwnd=0;
    DWORD pid=0;
    DWORD tid=0;
    int titleLength=0;
    bool windowValid=false;
    bool titleRead=false;
    bool identityComplete=false;
    WindowIdentityRecapture recapture=WindowIdentityRecapture::Indeterminate;
    bool targetPublished=false;
};

struct PickerTraceOpenEvent {
    PickerTraceOpenResult result=PickerTraceOpenResult::ModelUnavailable;
    PickerTraceDesktopSnapshotStatus desktopSnapshot=
        PickerTraceDesktopSnapshotStatus::NotAttempted;
    HRESULT desktopSnapshotResult=E_NOTIMPL;
    int desktopSnapshotIndex=-1;
    uint32_t desktopSnapshotCount=0;
    GUID currentDesktop{};
    uint64_t modelGeneration=0;
    bool currentDesktopAvailable=false;
    bool targetIdentityPresent=false;
};

struct PickerTraceEnumBeginEvent {
    uint64_t modelGeneration=0;
    std::vector<GUID> desktops;
};

struct PickerTraceEnumWindowEvent {
    uint64_t modelGeneration=0;
    uint64_t enumSequence=0;
    uintptr_t hwnd=0;
    uintptr_t owner=0;
    uintptr_t rootOwner=0;
    uintptr_t lastActivePopup=0;
    DWORD pid=0;
    DWORD tid=0;
    PickerTraceSafeClassName className;
    PickerTraceSafeImageBasename imageBasename;
    bool visibleObserved=false;
    bool visible=false;
    bool firstTitleObserved=false;
    int firstTitleLength=0;
    DWORD firstTitleError=ERROR_SUCCESS;
    bool exStyleObserved=false;
    uint64_t exStyle=0;
    DWORD exStyleError=ERROR_SUCCESS;
    bool toolWindow=false;
    bool rootOwnerObserved=false;
    bool rootOwnerSelf=false;
    bool cloakedObserved=false;
    DWORD cloaked=0;
    HRESULT cloakedResult=E_NOTIMPL;
    PickerTraceAltTabReason altTabReason=
        PickerTraceAltTabReason::FirstTitleUnavailable;
    HRESULT desktopResult=E_NOTIMPL;
    GUID desktop{};
    int tileIndex=-1;
    bool secondTitleObserved=false;
    int secondTitleLength=0;
    int secondTitleCopied=0;
    DWORD secondTitleError=ERROR_SUCCESS;
    bool processStartCacheHit=false;
    bool processStartAvailable=false;
    DWORD processStartError=ERROR_SUCCESS;
    bool identityComplete=false;
    WindowIdentityRecapture recapture=WindowIdentityRecapture::Indeterminate;
    PickerTraceEnumDecision decision=
        PickerTraceEnumDecision::GlobalSnapshotFailure;
};

struct PickerTraceEnumEndEvent {
    uint64_t modelGeneration=0;
    uint64_t candidates=0;
    std::array<uint64_t,static_cast<size_t>(
        PickerTraceEnumDecision::Count)> counts{};
    bool enumWindowsReturned=false;
    DWORD enumWindowsError=ERROR_SUCCESS;
    bool modelPublished=false;
};

struct PickerTraceMouseDownEvent {
    uint64_t rawWparam=0;
    int x=0;
    int y=0;
    bool ctrl=false;
    bool controlled=false;
    bool searchActive=false;
    PickerPointerTarget target=PickerPointerTarget::None;
    int tileIndex=-1;
};

struct PickerTraceActivationRequestEvent {
    uint64_t activationId=0;
    PickerTraceActivationSource source=PickerTraceActivationSource::Mouse;
    bool ctrl=false;
    int tileIndex=-1;
};

struct PickerTraceActivationResultEvent {
    uint64_t activationId=0;
    PickerTraceActivationResult result=PickerTraceActivationResult::InvalidTile;
};

template<class EmitRequest,class Select,class Refresh,class PlainSwitch,
         class BeginMove,class EmitResult>
PickerTraceActivationResult DispatchPickerActivation(
        uint64_t activationId,PickerTraceActivationSource source,
        bool controlled,int index,int count,bool ctrlMove,
        EmitRequest emitRequest,
        Select select,Refresh refresh,PlainSwitch plainSwitch,
        BeginMove beginMove,EmitResult emitResult) noexcept {
    static_assert(noexcept(emitRequest(
        activationId,source,ctrlMove,index)),
        "activation request emitter must be noexcept");
    static_assert(noexcept(!select(index)),
        "activation selection callback must be noexcept");
    static_assert(noexcept(refresh()),
        "activation refresh callback must be noexcept");
    static_assert(noexcept(plainSwitch(index)),
        "activation plain-switch callback must be noexcept");
    static_assert(noexcept(beginMove(index,activationId)),
        "activation move-entry callback must be noexcept");
    static_assert(noexcept(emitResult(
        activationId,PickerTraceActivationResult::InvalidTile)),
        "activation result emitter must be noexcept");
    emitRequest(activationId,source,ctrlMove,index);
    const auto finish=[&](PickerTraceActivationResult result) noexcept {
        emitResult(activationId,result);
        return result;
    };
    if(controlled)
        return finish(PickerTraceActivationResult::AlreadyControlled);
    if(index<0 || index>=count)
        return finish(PickerTraceActivationResult::InvalidTile);
    if(!select(index))
        return finish(
            PickerTraceActivationResult::SelectionPublicationFailed);
    refresh();
    if(ctrlMove){
        beginMove(index,activationId);
        return finish(PickerTraceActivationResult::DispatchedMoveEntry);
    }
    plainSwitch(index);
    return finish(PickerTraceActivationResult::RoutedPlainSwitch);
}

struct PickerTraceMoveBeginEvent {
    uint64_t activationId=0;
    uint64_t generation=0;
    int tileIndex=-1;
    PickerTraceMoveBeginReason reason=PickerTraceMoveBeginReason::InvalidIndex;
    GUID targetOrigin{};
    GUID popupOrigin{};
    GUID currentOrigin{};
    GUID destination{};
    PickerEffectKind firstEffect=PickerEffectKind::None;
};

struct PickerTraceMoveBeginExceptionEvent {
    uint64_t activationId=0;
    bool transitionPublished=false;
};

struct PickerTraceEffectEvent {
    PickerTraceEffectStage stage=PickerTraceEffectStage::Queue;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    PickerEvent observationEvent=PickerEvent::Timer;
    PickerEffectKind effect=PickerEffectKind::None;
    PickerEffectKind nextEffect=PickerEffectKind::None;
    PickerPhase phaseBefore=PickerPhase::Idle;
    PickerPhase phaseAfter=PickerPhase::Idle;
    PickerIdentityValidity identity=PickerIdentityValidity::Unknown;
    PickerReadValidity targetRead=PickerReadValidity::Unknown;
    PickerReadValidity popupRead=PickerReadValidity::Unknown;
    PickerReadValidity currentRead=PickerReadValidity::Unknown;
    PickerEffectExecutionRoute executionRoute=PickerEffectExecutionRoute::Execute;
    PickerTraceDeliveryRoute delivery=PickerTraceDeliveryRoute::None;
    uint32_t deliveryAttempt=0;
    bool executionRouteAvailable=false;
    bool deliveryAvailable=false;
    bool apiInvoked=false;
    bool apiAccepted=false;
};

struct PickerTraceApiResultEvent {
    PickerTraceApiKind api=PickerTraceApiKind::GetDesktops;
    PickerTraceDesktopLookupUse lookupUse=
        PickerTraceDesktopLookupUse::MoveEntryDestination;
    PickerTraceDesktopLookupStage lookupStage=
        PickerTraceDesktopLookupStage::ValidateRequest;
    PickerTraceRawResultKind resultKind=PickerTraceRawResultKind::HResult;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    uintptr_t hwnd=0;
    DWORD sourceThread=0;
    DWORD destinationThread=0;
    int index=-1;
    GUID requestedDesktop{};
    GUID actualDesktop{};
    HRESULT hresult=E_NOTIMPL;
    bool boolResult=false;
    bool invoked=false;
    bool lastErrorAvailable=false;
    DWORD lastError=ERROR_SUCCESS;
};

class PickerTraceSession;

struct PickerTraceDesktopLookupContext {
    PickerTraceSession* trace=nullptr;
    PickerTraceDesktopLookupUse use=
        PickerTraceDesktopLookupUse::MoveEntryDestination;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    GUID requested{};
};

void EmitPickerTraceDesktopLookupStage(
    const PickerTraceDesktopLookupContext* context,
    PickerTraceDesktopLookupStage stage,int index,HRESULT result,
    const GUID& actual,bool matched) noexcept;

struct PickerTraceBoolCallResult {
    BOOL value=FALSE;
    DWORD immediateError=ERROR_SUCCESS;
};

template<class Call,class ReadError>
PickerTraceBoolCallResult CallPickerTraceBoolWithImmediateError(
        Call call,ReadError readError) noexcept {
    PickerTraceBoolCallResult result;
    try {
        result.value=call();
        result.immediateError=readError();
    } catch(...) {
        result.value=FALSE;
        result.immediateError=ERROR_GEN_FAILURE;
    }
    return result;
}

struct PickerTraceForegroundHandoffOps {
    void* context=nullptr;
    BOOL (*attachThreadInput)(void*,DWORD,DWORD,BOOL)=nullptr;
    BOOL (*setForegroundWindow)(void*,HWND)=nullptr;
    HRESULT (*switchDesktop)(void*)=nullptr;
    BOOL (*showWindow)(void*,HWND,int)=nullptr;
};

struct PickerTraceApiEventObserver {
    void* context=nullptr;
    void (*emit)(void*,const PickerTraceApiResultEvent&) noexcept=nullptr;
};

struct PickerTraceForegroundHandoffResult {
    HRESULT switchResult=E_FAIL;
    bool invoked=false;
    bool desktopAttachAttempted=false;
    bool desktopAttached=false;
    bool foregroundAttachAttempted=false;
    bool foregroundAttached=false;
    BOOL focusResult=FALSE;
    BOOL foregroundDetachResult=FALSE;
    BOOL desktopDetachResult=FALSE;
    BOOL previousVisibility=FALSE;
};

PickerTraceForegroundHandoffResult ExecutePickerForegroundHandoffCalls(
    const PickerForegroundHandoffPlan&,HWND progman,
    DWORD desktopThread,DWORD foregroundThread,DWORD currentThread,
    const PickerTraceForegroundHandoffOps&,
    const PickerTraceApiEventObserver* observer) noexcept;

struct PickerTraceScheduleResult {
    bool deferred=false;
    bool routeAvailable=false;
    PickerTraceDeliveryRoute route=PickerTraceDeliveryRoute::None;
};

inline PickerTraceScheduleResult MarkPickerTraceDurableKick(
        PickerTraceScheduleResult result) noexcept {
    result.routeAvailable=true;
    result.route=PickerTraceDeliveryRoute::DurableExternalKick;
    return result;
}

struct PickerTraceQueueCallerDecision {
    PickerTraceScheduleResult delivery;
    bool runInlinePump=false;
    bool claimDurableKickAtCaller=false;
};

inline PickerTraceQueueCallerDecision DecidePickerTraceExternalQueue(
        const PickerTraceScheduleResult& schedule,
        bool pumpActive) noexcept {
    PickerTraceQueueCallerDecision result;
    result.delivery=schedule;
    result.runInlinePump=!schedule.deferred && !pumpActive;
    return result;
}

inline PickerTraceQueueCallerDecision DecidePickerTracePumpRearm(
        const PickerTraceScheduleResult& schedule) noexcept {
    PickerTraceQueueCallerDecision result;
    result.delivery=schedule;
    if(!schedule.deferred){
        result.claimDurableKickAtCaller=true;
        result.delivery=MarkPickerTraceDurableKick(result.delivery);
    }
    return result;
}

template<class ArmTimer,class Post>
PickerTraceScheduleResult SchedulePickerTransitionWork(
        bool mainAvailable,bool shutdownDrain,uint64_t remainingMs,
        ArmTimer armTimer,Post post) noexcept {
    PickerTraceScheduleResult result;
    result.routeAvailable=true;
    if(!mainAvailable){
        result.route=PickerTraceDeliveryRoute::InlineFallback;
        return result;
    }
    if(shutdownDrain){
        result.route=PickerTraceDeliveryRoute::ShutdownDrain;
        return result;
    }
    try {
        if(remainingMs!=0){
            const UINT wait=static_cast<UINT>((std::min)(
                remainingMs,static_cast<uint64_t>(UINT_MAX)));
            if(armTimer(wait?wait:1)){
                result.deferred=true;
                result.route=PickerTraceDeliveryRoute::DelayedTimer;
                return result;
            }
            result.deferred=true;
            result.route=PickerTraceDeliveryRoute::DurableExternalKick;
            return result;
        }
        if(post()){
            result.deferred=true;
            result.route=PickerTraceDeliveryRoute::Posted;
            return result;
        }
        if(armTimer(1)){
            result.deferred=true;
            result.route=PickerTraceDeliveryRoute::TimerArmed;
            return result;
        }
    } catch(...) {
        result.deferred=false;
        result.route=PickerTraceDeliveryRoute::InlineFallback;
        return result;
    }
    result.route=PickerTraceDeliveryRoute::InlineFallback;
    return result;
}

struct PickerTraceTerminalMetadata {
    uint64_t generation=0;
    PickerTraceRollbackTrigger rollbackTrigger=
        PickerTraceRollbackTrigger::None;
    PickerTraceDiagnosticCode diagnosticCode=
        PickerTraceDiagnosticCode::None;
};

void ObservePickerTraceTerminalMetadata(
    PickerTraceTerminalMetadata&,PickerPhase before,PickerPhase after,
    const PickerObservation&,bool queueConflict,bool caughtException)
    noexcept;

PickerEffect AdvancePickerTransitionTraced(
    PickerState& state,const PickerObservation& observation,
    PickerTraceSession* trace,PickerTraceTerminalMetadata* metadata)
    noexcept;

struct PickerTraceTerminalizationAttemptEvent {
    uint64_t generation=0;
    uint64_t attempt=0;
    PickerTraceTerminalizationReason reason=
        PickerTraceTerminalizationReason::RuntimeNotReady;
    PickerTraceDeliveryRoute incomingDelivery=PickerTraceDeliveryRoute::None;
    PickerTraceDeliveryRoute retryDelivery=PickerTraceDeliveryRoute::None;
    bool incomingDeliveryAvailable=false;
    bool retryDeliveryAvailable=false;
    bool terminalAcknowledged=false;
    bool pendingEffectNone=false;
    bool runtimeKeyPresent=false;
    PickerTerminalGuardReleaseAction firstReleaseAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTerminalGuardReleaseAction retryReleaseAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTraceReservationExceptionStage releaseExceptionStage=
        PickerTraceReservationExceptionStage::None;
    bool releaseAttempted=false;
    bool firstReleaseActionAvailable=false;
    bool retryReleaseActionAvailable=false;
    bool releaseExceptionStageAvailable=false;
    bool releaseRetried=false;
    bool releaseThrew=false;
    bool reservationReleased=false;
    bool ready=false;
    bool finalized=false;
};

struct PickerTraceTransitionTerminalEvent {
    uint64_t generation=0;
    PickerTraceTerminalOutcome outcome=PickerTraceTerminalOutcome::Failed;
    PickerTraceRollbackTrigger rollbackTrigger=PickerTraceRollbackTrigger::None;
    PickerTraceDiagnosticCode diagnosticCode=PickerTraceDiagnosticCode::None;
    int forwardTargetAttempts=0;
    int forwardPopupAttempts=0;
    int forwardSwitchAttempts=0;
    int rollbackTargetAttempts=0;
    int rollbackPopupAttempts=0;
    int rollbackSwitchAttempts=0;
    int focusAttempts=0;
    PickerReadValidity targetRead=PickerReadValidity::Unknown;
    PickerReadValidity popupRead=PickerReadValidity::Unknown;
    PickerReadValidity currentRead=PickerReadValidity::Unknown;
    GUID targetDesktop{};
    GUID popupDesktop{};
    GUID currentDesktop{};
};

const char* PickerTraceOpenResultName(PickerTraceOpenResult) noexcept;
const char* PickerTraceDesktopSnapshotStatusName(
    PickerTraceDesktopSnapshotStatus) noexcept;
const char* PickerTraceAltTabReasonName(PickerTraceAltTabReason) noexcept;
const char* PickerTraceEnumDecisionName(PickerTraceEnumDecision) noexcept;
const char* PickerTraceActivationSourceName(
    PickerTraceActivationSource) noexcept;
const char* PickerTraceActivationResultName(
    PickerTraceActivationResult) noexcept;
const char* PickerTraceMoveBeginReasonName(PickerTraceMoveBeginReason) noexcept;
const char* PickerTraceEffectStageName(PickerTraceEffectStage) noexcept;
const char* PickerTraceApiKindName(PickerTraceApiKind) noexcept;
const char* PickerTraceDesktopLookupStageName(
    PickerTraceDesktopLookupStage) noexcept;
const char* PickerTraceDesktopLookupUseName(
    PickerTraceDesktopLookupUse) noexcept;
const char* PickerTraceRawResultKindName(PickerTraceRawResultKind) noexcept;
const char* PickerTraceDeliveryRouteName(PickerTraceDeliveryRoute) noexcept;
const char* PickerTraceTerminalizationReasonName(
    PickerTraceTerminalizationReason) noexcept;
const char* PickerTraceTerminalOutcomeName(PickerTraceTerminalOutcome) noexcept;
const char* PickerTraceRollbackTriggerName(PickerTraceRollbackTrigger) noexcept;
const char* PickerTraceDiagnosticCodeName(PickerTraceDiagnosticCode) noexcept;
const char* PickerTraceReservationExceptionStageName(
    PickerTraceReservationExceptionStage) noexcept;
const char* PickerTraceTerminalGuardReleaseActionName(
    PickerTerminalGuardReleaseAction) noexcept;
const char* PickerTracePhaseName(PickerPhase) noexcept;
const char* PickerTraceEventName(PickerEvent) noexcept;
const char* PickerTraceEffectKindName(PickerEffectKind) noexcept;
const char* PickerTraceEffectExecutionRouteName(
    PickerEffectExecutionRoute) noexcept;
const char* PickerTraceIdentityValidityName(PickerIdentityValidity) noexcept;
const char* PickerTraceReadValidityName(PickerReadValidity) noexcept;
const char* PickerTraceRecaptureName(WindowIdentityRecapture) noexcept;
const char* PickerTracePointerTargetName(PickerPointerTarget) noexcept;

#define VDE_DECLARE_PICKER_TRACE_SERIALIZER(EventType) \
    bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope, \
                                  const EventType& event, \
                                  std::string& output) noexcept

VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceCaptureEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceOpenEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceEnumBeginEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceEnumWindowEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceEnumEndEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceMouseDownEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceActivationRequestEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceActivationResultEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceMoveBeginEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceMoveBeginExceptionEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceEffectEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceApiResultEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceTerminalizationAttemptEvent);
VDE_DECLARE_PICKER_TRACE_SERIALIZER(PickerTraceTransitionTerminalEvent);

#undef VDE_DECLARE_PICKER_TRACE_SERIALIZER

struct PickerTraceLimits {
    explicit PickerTraceLimits(
        uint64_t bytes=2ULL*1024ULL*1024ULL,
        uint32_t events=10000) noexcept
        :maxBytes(bytes),maxEvents(events){}
    uint64_t maxBytes;
    uint32_t maxEvents;
};

struct PickerTraceSinkOps {
    std::function<size_t(const void*,size_t)> write;
    std::function<bool()> flush;
    std::function<bool()> close;
    std::function<uint64_t()> monotonicMs;
};

class PickerTraceWriter {
public:
    PickerTraceWriter(PickerTraceSinkOps ops,PickerTraceLimits limits,
                      const std::array<unsigned char,16>& session) noexcept;
    ~PickerTraceWriter() noexcept;
    bool active() const noexcept;
    void flushBoundary() noexcept;
    void close() noexcept;
    void emit(const PickerTraceCaptureEvent&) noexcept;
    void emit(const PickerTraceOpenEvent&) noexcept;
    void emit(const PickerTraceEnumBeginEvent&) noexcept;
    void emit(const PickerTraceEnumWindowEvent&) noexcept;
    void emit(const PickerTraceEnumEndEvent&) noexcept;
    void emit(const PickerTraceMouseDownEvent&) noexcept;
    void emit(const PickerTraceActivationRequestEvent&) noexcept;
    void emit(const PickerTraceActivationResultEvent&) noexcept;
    void emit(const PickerTraceMoveBeginEvent&) noexcept;
    void emit(const PickerTraceMoveBeginExceptionEvent&) noexcept;
    void emit(const PickerTraceEffectEvent&) noexcept;
    void emit(const PickerTraceApiResultEvent&) noexcept;
    void emit(const PickerTraceTerminalizationAttemptEvent&) noexcept;
    void emit(const PickerTraceTransitionTerminalEvent&) noexcept;
    void emitStart(const struct PickerTraceStartEvent&,
                   const wchar_t* appVersion) noexcept;
private:
    template<class Event> void emitTyped(const Event&) noexcept;
    void truncate() noexcept;
    bool writeWhole(const std::string&) noexcept;
    PickerTraceSinkOps ops_;
    PickerTraceLimits limits_;
    PickerTraceEnvelope envelope_;
    uint64_t startMs_=0;
    uint64_t lastElapsedMs_=0;
    uint64_t bytesWritten_=0;
    uint32_t eventsWritten_=0;
    bool active_=false;
    bool truncated_=false;
    bool closed_=false;
};

struct PickerTraceDirectoryEntry {
    PickerTraceDirectoryEntry() noexcept=default;
    PickerTraceDirectoryEntry(std::wstring entryName,DWORD entryAttributes,
                              uint64_t entryLastWrite)
        :name(std::move(entryName)),attributes(entryAttributes),
         lastWrite100ns(entryLastWrite){}
    std::wstring name;
    DWORD attributes=0;
    uint64_t lastWrite100ns=0;
};

bool IsPickerTraceFileName(const std::wstring& value) noexcept;
bool PlanPickerTraceRetention(
    const std::vector<PickerTraceDirectoryEntry>& entries,
    uint64_t now100ns,size_t oldFilesToKeep,
    std::vector<size_t>& remove) noexcept;

struct PickerTraceStorageOps {
    std::function<bool(std::wstring&)> localAppData;
    std::function<DWORD(const std::wstring&)> getAttributes;
    std::function<BOOL(const std::wstring&)> createDirectory;
    std::function<bool(const std::wstring&,
                       std::vector<PickerTraceDirectoryEntry>&)> listDirectory;
    std::function<BOOL(const std::wstring&)> deleteFile;
    std::function<HANDLE(const std::wstring&,DWORD)> createNew;
    std::function<BOOL(HANDLE,const void*,DWORD,DWORD&)> writeFile;
    std::function<BOOL(HANDLE)> flushFile;
    std::function<BOOL(HANDLE)> closeHandle;
    std::function<uint64_t()> utcFileTime100ns;
    std::function<uint64_t()> monotonicMs;
};

struct PickerTraceOpenedFile {
    HANDLE handle=INVALID_HANDLE_VALUE;
    std::wstring path;
};

PickerTraceStorageOps DefaultPickerTraceStorageOps() noexcept;
bool OpenPickerTraceStorage(const PickerTraceStorageOps& ops,DWORD processId,
                            PickerTraceOpenedFile& output) noexcept;

enum class PickerTraceDigestStatus : uint8_t {
    Available, PathUnavailable, NormalizeFailed, OpenFailed,
    MetadataFailed, ReadFailed, CryptoFailed
};

struct PickerTraceDigest {
    PickerTraceDigestStatus status=PickerTraceDigestStatus::PathUnavailable;
    std::array<unsigned char,32> bytes{};
    DWORD win32Error=ERROR_SUCCESS;
    LONG cryptoStatus=0;
    bool available=false;
};

struct PickerTraceProvenanceOps {
    std::function<bool(std::wstring&)> modulePath;
    std::function<HANDLE(const std::wstring&)> openRead;
    std::function<BOOL(HANDLE,void*,DWORD,DWORD&)> readFile;
    std::function<BOOL(HANDLE,LONGLONG,DWORD)> seekFile;
    std::function<bool(HANDLE,uint64_t&,uint64_t&)> fileMetadata;
    std::function<BOOL(HANDLE)> closeHandle;
    std::function<bool(void*,size_t)> randomBytes;
    std::function<bool(DWORD&)> processSessionId;
    std::function<bool(DWORD&,bool&)> processIntegrity;
    std::function<DWORD()> processId;
    std::function<DWORD()> threadId;
    std::function<DWORD()> windowsBuild;
};

struct PickerTraceRuntimeOps {
    PickerTraceStorageOps storage;
    PickerTraceProvenanceOps provenance;
};

PickerTraceProvenanceOps DefaultPickerTraceProvenanceOps() noexcept;
PickerTraceRuntimeOps DefaultPickerTraceRuntimeOps() noexcept;

PickerTraceDigest PickerTraceSha256Bytes(
    const void* bytes,size_t size) noexcept;
PickerTraceDigest PickerTraceSha256File(
    const std::wstring& path,const PickerTraceProvenanceOps& ops) noexcept;
std::string PickerTraceDigestHex(const PickerTraceDigest&) noexcept;
bool NormalizePickerTraceModulePath(
    const std::wstring& input,std::string& normalizedUtf8) noexcept;
bool ReadPickerTracePeTimestamp(
    const std::wstring& path,const PickerTraceProvenanceOps& ops,
    uint32_t& timestamp,DWORD& win32Error) noexcept;

struct PickerTraceStartEvent {
    PickerTraceDigest imageDigest;
    PickerTraceDigest pathDigest;
    PickerTraceSafeImageBasename moduleBasename;
    uint64_t fileSize=0;
    uint64_t lastWrite100ns=0;
    uint32_t peTimestamp=0;
    DWORD pid=0;
    DWORD tid=0;
    DWORD processSessionId=0;
    DWORD integrityRid=0;
    DWORD windowsBuild=0;
    bool fileMetadataAvailable=false;
    bool peTimestampAvailable=false;
    bool processSessionAvailable=false;
    bool integrityAvailable=false;
    bool elevated=false;
};

const char* PickerTraceDigestStatusName(PickerTraceDigestStatus) noexcept;
bool SerializePickerTraceLine(
    const PickerTraceEnvelope&,const PickerTraceStartEvent&,
    const wchar_t* appVersion,std::string& output) noexcept;

class PickerTraceSession {
public:
    PickerTraceSession() noexcept;
    explicit PickerTraceSession(PickerTraceRuntimeOps ops,
                                PickerTraceLimits limits=
                                    PickerTraceLimits()) noexcept;
    ~PickerTraceSession() noexcept;
    bool start(bool requested,const wchar_t* appVersion) noexcept;
    bool active() const noexcept;
    bool requested() const noexcept;
    uint64_t nextCorrelationId() noexcept;
    void flushBoundary() noexcept;
    void close() noexcept;
    void emit(const PickerTraceCaptureEvent&) noexcept;
    void emit(const PickerTraceOpenEvent&) noexcept;
    void emit(const PickerTraceEnumBeginEvent&) noexcept;
    void emit(const PickerTraceEnumWindowEvent&) noexcept;
    void emit(const PickerTraceEnumEndEvent&) noexcept;
    void emit(const PickerTraceMouseDownEvent&) noexcept;
    void emit(const PickerTraceActivationRequestEvent&) noexcept;
    void emit(const PickerTraceActivationResultEvent&) noexcept;
    void emit(const PickerTraceMoveBeginEvent&) noexcept;
    void emit(const PickerTraceMoveBeginExceptionEvent&) noexcept;
    void emit(const PickerTraceEffectEvent&) noexcept;
    void emit(const PickerTraceApiResultEvent&) noexcept;
    void emit(const PickerTraceTerminalizationAttemptEvent&) noexcept;
    void emit(const PickerTraceTransitionTerminalEvent&) noexcept;
    std::wstring pathForLocalInspection() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
