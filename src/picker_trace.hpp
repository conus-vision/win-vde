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
