#include "picker_trace.hpp"

#include <bcrypt.h>
#include <cstdio>
#include <limits>
#include <new>

#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"advapi32.lib")

static bool PickerTraceIsCliCommand(const std::wstring& command) noexcept {
    return command==L"save" || command==L"restore" ||
        command==L"restore-auto" || command==L"status" ||
        command==L"list" || command==L"-h" ||
        command==L"--help" || command==L"/?";
}

VdeLaunchOptions ParseVdeLaunchOptions(
        int argc,const wchar_t* const* argv) noexcept {
    VdeLaunchOptions result;
    try {
        if(argc>=2 && argv && argv[1]) result.command=argv[1];
        result.cli=PickerTraceIsCliCommand(result.command);
        result.tracePicker=argc==2 && argv && argv[1] &&
            result.command==L"--trace-picker";
    } catch(...) {
        result=VdeLaunchOptions{};
    }
    return result;
}

static wchar_t PickerTraceReplacementCharacter() noexcept {
    return static_cast<wchar_t>(0xfffd);
}

template<size_t Capacity>
static bool PickerTraceCopySafeWide(const wchar_t* value,int length,
                                    std::array<wchar_t,Capacity>& output,
                                    uint16_t& outputLength) noexcept {
    output.fill(L'\0');
    outputLength=0;
    if(!value || length<0 || static_cast<size_t>(length)>=Capacity)
        return false;
    size_t written=0;
    for(int index=0;index<length;++index){
        const uint16_t unit=static_cast<uint16_t>(value[index]);
        if(unit>=0xd800 && unit<=0xdbff){
            if(index+1<length){
                const uint16_t next=static_cast<uint16_t>(value[index+1]);
                if(next>=0xdc00 && next<=0xdfff){
                    output[written++]=value[index];
                    output[written++]=value[++index];
                    continue;
                }
            }
            output[written++]=PickerTraceReplacementCharacter();
        } else if(unit>=0xdc00 && unit<=0xdfff){
            output[written++]=PickerTraceReplacementCharacter();
        } else {
            output[written++]=value[index];
        }
    }
    outputLength=static_cast<uint16_t>(written);
    return true;
}

PickerTraceSafeClassName MakePickerTraceSafeClassName(
        const wchar_t* value,int length) noexcept {
    PickerTraceSafeClassName result;
    result.available_=PickerTraceCopySafeWide(
        value,length,result.value_,result.length_);
    return result;
}

PickerTraceSafeImageBasename MakePickerTraceSafeImageBasename(
        const wchar_t* value,int length) noexcept {
    PickerTraceSafeImageBasename result;
    if(!value || length<0 || length>260) return result;
    for(int index=0;index<length;++index){
        if(value[index]==L'\\' || value[index]==L'/' || value[index]==L':')
            return result;
    }
    result.available_=PickerTraceCopySafeWide(
        value,length,result.value_,result.length_);
    return result;
}

PickerTraceAltTabReason DecidePickerTraceAltTabReason(
        bool visible,int titleLength,uint64_t exStyle,
        uintptr_t hwnd,uintptr_t rootOwner) noexcept {
    if(!visible) return PickerTraceAltTabReason::NotVisible;
    if(titleLength<=0)
        return PickerTraceAltTabReason::FirstTitleUnavailable;
    if((exStyle&static_cast<uint64_t>(WS_EX_TOOLWINDOW))!=0)
        return PickerTraceAltTabReason::ToolWindow;
    if(rootOwner!=hwnd) return PickerTraceAltTabReason::RootOwnerMismatch;
    return PickerTraceAltTabReason::Eligible;
}

PickerTraceAltTabFacts ObservePickerTraceAltTabWindow(
        HWND hwnd,const PickerTraceAltTabOps& ops) noexcept {
    PickerTraceAltTabFacts facts;
    try {
        if(!ops.isVisible){
            facts.reason=PickerTraceAltTabReason::NotVisible;
            return facts;
        }
        facts.visibleObserved=true;
        facts.visible=ops.isVisible(ops.context,hwnd)!=FALSE;
        if(!facts.visible){
            facts.reason=PickerTraceAltTabReason::NotVisible;
            return facts;
        }
        if(!ops.titleLength){
            facts.reason=PickerTraceAltTabReason::FirstTitleUnavailable;
            return facts;
        }
        facts.firstTitleObserved=true;
        facts.firstTitleLength=ops.titleLength(
            ops.context,hwnd,facts.firstTitleError);
        if(facts.firstTitleLength<=0){
            facts.reason=PickerTraceAltTabReason::FirstTitleUnavailable;
            return facts;
        }
        if(!ops.extendedStyle){
            facts.reason=PickerTraceAltTabReason::ToolWindow;
            return facts;
        }
        facts.exStyleObserved=true;
        facts.exStyle=ops.extendedStyle(
            ops.context,hwnd,facts.exStyleError);
        if((static_cast<uint64_t>(facts.exStyle)&
            static_cast<uint64_t>(WS_EX_TOOLWINDOW))!=0){
            facts.reason=PickerTraceAltTabReason::ToolWindow;
            return facts;
        }
        if(!ops.rootOwner){
            facts.reason=PickerTraceAltTabReason::RootOwnerMismatch;
            return facts;
        }
        facts.rootOwnerObserved=true;
        facts.rootOwner=ops.rootOwner(ops.context,hwnd);
        facts.reason=DecidePickerTraceAltTabReason(
            facts.visible,facts.firstTitleLength,
            static_cast<uint64_t>(facts.exStyle),
            reinterpret_cast<uintptr_t>(hwnd),
            reinterpret_cast<uintptr_t>(facts.rootOwner));
        return facts;
    } catch(...) {
        return facts;
    }
}

PickerTraceEnumDecision DecidePickerTraceEnumDecision(
        PickerTraceAltTabReason altTabReason,
        bool desktopServiceAvailable,HRESULT desktopResult,
        bool desktopGuidAvailable,int tileIndex,
        int secondTitleLength,int secondTitleCopied,
        bool pidAvailable,bool processStartAvailable,
        WindowIdentityRecapture recapture) noexcept {
    switch(altTabReason){
    case PickerTraceAltTabReason::NotVisible:
        return PickerTraceEnumDecision::SkipNotVisible;
    case PickerTraceAltTabReason::FirstTitleUnavailable:
        return PickerTraceEnumDecision::SkipFirstTitleUnavailable;
    case PickerTraceAltTabReason::ToolWindow:
        return PickerTraceEnumDecision::SkipToolWindow;
    case PickerTraceAltTabReason::RootOwnerMismatch:
        return PickerTraceEnumDecision::SkipRootOwnerMismatch;
    case PickerTraceAltTabReason::Eligible:
        break;
    }
    if(!desktopServiceAvailable)
        return PickerTraceEnumDecision::SkipDesktopServiceMissing;
    if(FAILED(desktopResult))
        return PickerTraceEnumDecision::SkipDesktopLookupFailed;
    if(!desktopGuidAvailable)
        return PickerTraceEnumDecision::SkipDesktopGuidZero;
    if(tileIndex<0) return PickerTraceEnumDecision::SkipDesktopTileMissing;
    if(secondTitleLength<=0)
        return PickerTraceEnumDecision::SkipSecondTitleUnavailable;
    if(secondTitleCopied<=0)
        return PickerTraceEnumDecision::SkipSecondTitleReadFailed;
    if(!pidAvailable)
        return PickerTraceEnumDecision::DisplayOnlyPidUnavailable;
    if(!processStartAvailable)
        return PickerTraceEnumDecision::DisplayOnlyProcessStartUnavailable;
    if(recapture==WindowIdentityRecapture::Lost)
        return PickerTraceEnumDecision::SkipIdentityLost;
    if(recapture==WindowIdentityRecapture::Indeterminate)
        return PickerTraceEnumDecision::DisplayOnlyIdentityIndeterminate;
    return PickerTraceEnumDecision::Verified;
}

#define VDE_TRACE_NAME_CASE(Type,Value,Name) case Type::Value: return Name

const char* PickerTraceOpenResultName(PickerTraceOpenResult value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,Shown,"shown");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,Degraded,"degraded");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,ControlledTransition,"controlled_transition");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,WorkAreaUnavailable,"work_area_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,ModelUnavailable,"model_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,AdjustRectFailed,"adjust_rect_failed");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,OuterSizeInvalid,"outer_size_invalid");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,PositionFailed,"position_failed");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,ClientRectFailed,"client_rect_failed");
    VDE_TRACE_NAME_CASE(PickerTraceOpenResult,PaintCacheFailed,"paint_cache_failed");
    }
    return "unknown";
}

const char* PickerTraceDesktopSnapshotStatusName(
        PickerTraceDesktopSnapshotStatus value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,NotAttempted,"not_attempted");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,Complete,"complete");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,DesktopServiceMissing,"desktop_service_missing");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,GetDesktopsFailed,"get_desktops_failed");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,GetCountFailed,"get_count_failed");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,InvalidCount,"invalid_count");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,GetAtFailed,"get_at_failed");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,GetIdFailed,"get_id_failed");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,InvalidGuid,"invalid_guid");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,AllocationFailure,"allocation_failure");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopSnapshotStatus,Exception,"exception");
    }
    return "unknown";
}

const char* PickerTraceAltTabReasonName(PickerTraceAltTabReason value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceAltTabReason,Eligible,"eligible");
    VDE_TRACE_NAME_CASE(PickerTraceAltTabReason,NotVisible,"not_visible");
    VDE_TRACE_NAME_CASE(PickerTraceAltTabReason,FirstTitleUnavailable,"first_title_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceAltTabReason,ToolWindow,"tool_window");
    VDE_TRACE_NAME_CASE(PickerTraceAltTabReason,RootOwnerMismatch,"root_owner_mismatch");
    }
    return "unknown";
}

const char* PickerTraceEnumDecisionName(PickerTraceEnumDecision value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipNotVisible,"skip_not_visible");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipFirstTitleUnavailable,"skip_first_title_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipToolWindow,"skip_tool_window");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipRootOwnerMismatch,"skip_root_owner_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipDesktopServiceMissing,"skip_desktop_service_missing");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipDesktopLookupFailed,"skip_desktop_lookup_failed");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipDesktopGuidZero,"skip_desktop_guid_zero");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipDesktopTileMissing,"skip_desktop_tile_missing");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipSecondTitleUnavailable,"skip_second_title_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipSecondTitleReadFailed,"skip_second_title_read_failed");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,SkipIdentityLost,"skip_identity_lost");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,DisplayOnlyPidUnavailable,"display_only_pid_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,DisplayOnlyProcessStartUnavailable,"display_only_process_start_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,DisplayOnlyIdentityIndeterminate,"display_only_identity_indeterminate");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,Verified,"verified");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,AllocationFailure,"allocation_failure");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,GlobalSnapshotFailure,"global_snapshot_failure");
    VDE_TRACE_NAME_CASE(PickerTraceEnumDecision,Count,"count");
    }
    return "unknown";
}

const char* PickerTraceActivationSourceName(
        PickerTraceActivationSource value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceActivationSource,Mouse,"mouse");
    VDE_TRACE_NAME_CASE(PickerTraceActivationSource,Keyboard,"keyboard");
    }
    return "unknown";
}

const char* PickerTraceActivationResultName(
        PickerTraceActivationResult value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceActivationResult,AlreadyControlled,"already_controlled");
    VDE_TRACE_NAME_CASE(PickerTraceActivationResult,InvalidTile,"invalid_tile");
    VDE_TRACE_NAME_CASE(PickerTraceActivationResult,SelectionPublicationFailed,"selection_publication_failed");
    VDE_TRACE_NAME_CASE(PickerTraceActivationResult,RoutedPlainSwitch,"routed_plain_switch");
    VDE_TRACE_NAME_CASE(PickerTraceActivationResult,DispatchedMoveEntry,"dispatched_move_entry");
    }
    return "unknown";
}

const char* PickerTraceMoveBeginReasonName(
        PickerTraceMoveBeginReason value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,Accepted,"accepted");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,AlreadyControlled,"already_controlled");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,InvalidIndex,"invalid_index");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,SelectionIndexMismatch,"selection_index_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,SelectionDesktopMismatch,"selection_desktop_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,MainWindowMissing,"main_window_missing");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,DesktopManagerMissing,"desktop_manager_missing");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,DesktopDocumentMissing,"desktop_document_missing");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,TargetMismatch,"target_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,TargetWindowMissing,"target_window_missing");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,TargetWindowInvalid,"target_window_invalid");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,DestinationZero,"destination_zero");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,DestinationLookupFailed,"destination_lookup_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,CurrentDesktopUnavailable,"current_desktop_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,PopupDesktopUnavailable,"popup_desktop_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,FastCaptureFailed,"fast_capture_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,TargetDesktopUnavailable,"target_desktop_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,IdentityMismatch,"identity_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,IdentityLost,"identity_lost");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,IdentityIndeterminate,"identity_indeterminate");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,AcceptedPlanConflict,"accepted_plan_conflict");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,BoundRecordConflict,"bound_record_conflict");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,SafeOriginUnavailable,"safe_origin_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,AcceptedOperationMissing,"accepted_operation_missing");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,OperationClaimStageFailed,"operation_claim_stage_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,ReservationHandoffFailed,"reservation_handoff_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,PendingAssociationStageFailed,"pending_association_stage_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,ProvisionalInsertFailed,"provisional_insert_failed");
    VDE_TRACE_NAME_CASE(PickerTraceMoveBeginReason,NoInitialEffect,"no_initial_effect");
    }
    return "unknown";
}

const char* PickerTraceEffectStageName(PickerTraceEffectStage value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceEffectStage,Queue,"queue");
    VDE_TRACE_NAME_CASE(PickerTraceEffectStage,Execute,"execute");
    VDE_TRACE_NAME_CASE(PickerTraceEffectStage,Observation,"observation");
    VDE_TRACE_NAME_CASE(PickerTraceEffectStage,Reduce,"reduce");
    }
    return "unknown";
}

const char* PickerTraceApiKindName(PickerTraceApiKind value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetViewForHwnd,"get_view_for_hwnd");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,MoveViewToDesktop,"move_view_to_desktop");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,MoveWindowToDesktop,"move_window_to_desktop");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetWindowDesktopIdTarget,"get_window_desktop_id_target");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetWindowDesktopIdPopup,"get_window_desktop_id_popup");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetWindowDesktopIdCapture,"get_window_desktop_id_capture");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetDesktops,"get_desktops");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetCount,"get_count");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetAt,"get_at");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetId,"get_id");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,GetCurrentDesktop,"get_current_desktop");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,AttachDesktopInput,"attach_desktop_input");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,AttachForegroundInput,"attach_foreground_input");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,SetForegroundWindow,"set_foreground_window");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,DetachForegroundInput,"detach_foreground_input");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,DetachDesktopInput,"detach_desktop_input");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,SwitchDesktop,"switch_desktop");
    VDE_TRACE_NAME_CASE(PickerTraceApiKind,ShowWindowProgmanCleanup,"show_window_progman_cleanup");
    }
    return "unknown";
}

const char* PickerTraceDesktopLookupStageName(
        PickerTraceDesktopLookupStage value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,ValidateRequest,"validate_request");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,GetDesktops,"get_desktops");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,GetCount,"get_count");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,GetAt,"get_at");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,GetId,"get_id");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,Match,"match");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,NotFound,"not_found");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupStage,Exception,"exception");
    }
    return "unknown";
}

const char* PickerTraceDesktopLookupUseName(
        PickerTraceDesktopLookupUse value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupUse,MoveEntryDestination,"move_entry_destination");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupUse,MoveTargetDestination,"move_target_destination");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupUse,MovePopupDestination,"move_popup_destination");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupUse,SwitchPrecheckDestination,"switch_precheck_destination");
    VDE_TRACE_NAME_CASE(PickerTraceDesktopLookupUse,SwitchHandoffDestination,"switch_handoff_destination");
    }
    return "unknown";
}

const char* PickerTraceRawResultKindName(
        PickerTraceRawResultKind value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceRawResultKind,HResult,"hresult");
    VDE_TRACE_NAME_CASE(PickerTraceRawResultKind,Win32Bool,"win32_bool");
    VDE_TRACE_NAME_CASE(PickerTraceRawResultKind,PreviousVisibility,"previous_visibility");
    VDE_TRACE_NAME_CASE(PickerTraceRawResultKind,NoExtendedError,"no_extended_error");
    }
    return "unknown";
}

const char* PickerTraceDeliveryRouteName(
        PickerTraceDeliveryRoute value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,None,"none");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,Posted,"posted");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,TimerArmed,"timer_armed");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,InlineFallback,"inline_fallback");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,DelayedTimer,"delayed_timer");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,DurableExternalKick,"durable_external_kick");
    VDE_TRACE_NAME_CASE(PickerTraceDeliveryRoute,ShutdownDrain,"shutdown_drain");
    }
    return "unknown";
}

const char* PickerTraceTerminalizationReasonName(
        PickerTraceTerminalizationReason value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,Completed,"completed");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,TerminalNotAcknowledged,"terminal_not_acknowledged");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,PendingEffect,"pending_effect");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,ReservationReleaseException,"reservation_release_exception");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,ReservationNotReleased,"reservation_not_released");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,RuntimeKeyMissing,"runtime_key_missing");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,RuntimeNotReady,"runtime_not_ready");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalizationReason,FinalizeStateFailed,"finalize_state_failed");
    }
    return "unknown";
}

const char* PickerTraceTerminalOutcomeName(
        PickerTraceTerminalOutcome value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceTerminalOutcome,Succeeded,"succeeded");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalOutcome,Cancelled,"cancelled");
    VDE_TRACE_NAME_CASE(PickerTraceTerminalOutcome,Failed,"failed");
    }
    return "unknown";
}

const char* PickerTraceRollbackTriggerName(
        PickerTraceRollbackTrigger value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,None,"none");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,Cancellation,"cancellation");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,TargetMove,"target_move");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,TargetVerify,"target_verify");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,PopupMove,"popup_move");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,PopupVerify,"popup_verify");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,DesktopSwitch,"desktop_switch");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,IdentityLost,"identity_lost");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,IdentityIndeterminate,"identity_indeterminate");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,ReadUnavailable,"read_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,RetryBudgetExhausted,"retry_budget_exhausted");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,QueueConflict,"queue_conflict");
    VDE_TRACE_NAME_CASE(PickerTraceRollbackTrigger,Exception,"exception");
    }
    return "unknown";
}

const char* PickerTraceDiagnosticCodeName(
        PickerTraceDiagnosticCode value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,None,"none");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,ApiRejected,"api_rejected");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,VerificationMismatch,"verification_mismatch");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,IdentityLost,"identity_lost");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,IdentityIndeterminate,"identity_indeterminate");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,ReadUnavailable,"read_unavailable");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,RetryBudgetExhausted,"retry_budget_exhausted");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,QueueConflict,"queue_conflict");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,Exception,"exception");
    VDE_TRACE_NAME_CASE(PickerTraceDiagnosticCode,Cancelled,"cancelled");
    }
    return "unknown";
}

const char* PickerTraceReservationExceptionStageName(
        PickerTraceReservationExceptionStage value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,None,"none");
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,FirstDecision,"first_decision");
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,CheckpointCallback,"checkpoint_callback");
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,Refind,"refind");
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,SecondDecision,"second_decision");
    VDE_TRACE_NAME_CASE(PickerTraceReservationExceptionStage,Erase,"erase");
    }
    return "unknown";
}

const char* PickerTraceTerminalGuardReleaseActionName(
        PickerTerminalGuardReleaseAction value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerTerminalGuardReleaseAction,ResolvedAbsent,"resolved_absent");
    VDE_TRACE_NAME_CASE(PickerTerminalGuardReleaseAction,ConsumeExact,"consume_exact");
    VDE_TRACE_NAME_CASE(PickerTerminalGuardReleaseAction,RetryExactOwner,"retry_exact_owner");
    }
    return "unknown";
}

const char* PickerTracePhaseName(PickerPhase value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerPhase,Idle,"idle");
    VDE_TRACE_NAME_CASE(PickerPhase,TargetIssue,"target_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,TargetVerify,"target_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,IdentityVerifyBeforePopup,"identity_verify_before_popup");
    VDE_TRACE_NAME_CASE(PickerPhase,PopupIssue,"popup_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,PopupVerify,"popup_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,IdentityVerifyBeforeSwitch,"identity_verify_before_switch");
    VDE_TRACE_NAME_CASE(PickerPhase,SwitchIssue,"switch_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,DestinationVerify,"destination_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,RollbackTargetIssue,"rollback_target_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,RollbackTargetVerify,"rollback_target_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,RollbackPopupIssue,"rollback_popup_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,RollbackPopupVerify,"rollback_popup_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,RollbackSwitchIssue,"rollback_switch_issue");
    VDE_TRACE_NAME_CASE(PickerPhase,OriginVerify,"origin_verify");
    VDE_TRACE_NAME_CASE(PickerPhase,SaveExactTarget,"save_exact_target");
    VDE_TRACE_NAME_CASE(PickerPhase,RefreshModel,"refresh_model");
    VDE_TRACE_NAME_CASE(PickerPhase,FailureReport,"failure_report");
    VDE_TRACE_NAME_CASE(PickerPhase,FocusRestore,"focus_restore");
    }
    return "unknown";
}

const char* PickerTraceEventName(PickerEvent value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerEvent,Begin,"begin");
    VDE_TRACE_NAME_CASE(PickerEvent,ApiCompleted,"api_completed");
    VDE_TRACE_NAME_CASE(PickerEvent,ReadbackCompleted,"readback_completed");
    VDE_TRACE_NAME_CASE(PickerEvent,EffectCompleted,"effect_completed");
    VDE_TRACE_NAME_CASE(PickerEvent,CancelRequested,"cancel_requested");
    VDE_TRACE_NAME_CASE(PickerEvent,Timer,"timer");
    }
    return "unknown";
}

const char* PickerTraceEffectKindName(PickerEffectKind value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerEffectKind,None,"none");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ValidateTarget,"validate_target");
    VDE_TRACE_NAME_CASE(PickerEffectKind,MoveTarget,"move_target");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ReadTarget,"read_target");
    VDE_TRACE_NAME_CASE(PickerEffectKind,MovePopup,"move_popup");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ReadPopup,"read_popup");
    VDE_TRACE_NAME_CASE(PickerEffectKind,SwitchDesktop,"switch_desktop");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ReadCurrent,"read_current");
    VDE_TRACE_NAME_CASE(PickerEffectKind,SaveExactTarget,"save_exact_target");
    VDE_TRACE_NAME_CASE(PickerEffectKind,Refresh,"refresh");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ShowAndFocus,"show_and_focus");
    VDE_TRACE_NAME_CASE(PickerEffectKind,Hide,"hide");
    VDE_TRACE_NAME_CASE(PickerEffectKind,ReportFailure,"report_failure");
    }
    return "unknown";
}

const char* PickerTraceEffectExecutionRouteName(
        PickerEffectExecutionRoute value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerEffectExecutionRoute,Execute,"execute");
    VDE_TRACE_NAME_CASE(PickerEffectExecutionRoute,DeferUntilDue,"defer_until_due");
    VDE_TRACE_NAME_CASE(PickerEffectExecutionRoute,AcknowledgeWithoutUi,"acknowledge_without_ui");
    }
    return "unknown";
}

const char* PickerTraceIdentityValidityName(
        PickerIdentityValidity value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerIdentityValidity,Unknown,"unknown");
    VDE_TRACE_NAME_CASE(PickerIdentityValidity,Match,"match");
    VDE_TRACE_NAME_CASE(PickerIdentityValidity,Lost,"lost");
    VDE_TRACE_NAME_CASE(PickerIdentityValidity,Indeterminate,"indeterminate");
    }
    return "unknown";
}

const char* PickerTraceReadValidityName(PickerReadValidity value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerReadValidity,Unknown,"unknown");
    VDE_TRACE_NAME_CASE(PickerReadValidity,Valid,"valid");
    VDE_TRACE_NAME_CASE(PickerReadValidity,Unavailable,"unavailable");
    }
    return "unknown";
}

static PickerTraceApiKind PickerTraceLookupApiKind(
        PickerTraceDesktopLookupStage stage) noexcept {
    switch(stage){
    case PickerTraceDesktopLookupStage::GetCount:
        return PickerTraceApiKind::GetCount;
    case PickerTraceDesktopLookupStage::GetAt:
        return PickerTraceApiKind::GetAt;
    case PickerTraceDesktopLookupStage::GetId:
        return PickerTraceApiKind::GetId;
    default:
        return PickerTraceApiKind::GetDesktops;
    }
}

void EmitPickerTraceDesktopLookupStage(
        const PickerTraceDesktopLookupContext* context,
        PickerTraceDesktopLookupStage stage,int index,HRESULT result,
        const GUID& actual,bool matched) noexcept {
    if(!context || !context->trace || !context->trace->active()) return;
    PickerTraceApiResultEvent event;
    event.api=PickerTraceLookupApiKind(stage);
    event.lookupUse=context->use;
    event.lookupStage=stage;
    event.resultKind=PickerTraceRawResultKind::HResult;
    event.generation=context->generation;
    event.effectSerial=context->effectSerial;
    event.index=index;
    event.requestedDesktop=context->requested;
    event.actualDesktop=actual;
    event.hresult=result;
    event.boolResult=matched;
    event.invoked=stage==PickerTraceDesktopLookupStage::GetDesktops ||
        stage==PickerTraceDesktopLookupStage::GetCount ||
        stage==PickerTraceDesktopLookupStage::GetAt ||
        stage==PickerTraceDesktopLookupStage::GetId;
    context->trace->emit(event);
}

static void EmitPickerTraceApiResult(
        const PickerTraceApiEventObserver* observer,
        const PickerTraceApiResultEvent& event) noexcept {
    if(!observer || !observer->emit) return;
    try { observer->emit(observer->context,event); }
    catch(...) {}
}

PickerTraceForegroundHandoffResult ExecutePickerForegroundHandoffCalls(
        const PickerForegroundHandoffPlan& plan,HWND progman,
        DWORD desktopThread,DWORD foregroundThread,DWORD currentThread,
        const PickerTraceForegroundHandoffOps& ops,
        const PickerTraceApiEventObserver* observer) noexcept {
    PickerTraceForegroundHandoffResult result;
    const auto emitBool=[&](PickerTraceApiKind api,BOOL value,
                            DWORD source,DWORD destination,
                            HWND hwnd=nullptr,
                            PickerTraceRawResultKind kind=
                                PickerTraceRawResultKind::NoExtendedError)
                            noexcept {
        PickerTraceApiResultEvent event;
        event.api=api;
        event.resultKind=kind;
        event.hwnd=reinterpret_cast<uintptr_t>(hwnd);
        event.sourceThread=source;
        event.destinationThread=destination;
        event.boolResult=value!=FALSE;
        event.invoked=true;
        event.lastErrorAvailable=false;
        EmitPickerTraceApiResult(observer,event);
    };
    const auto callAttach=[&](DWORD source,BOOL attach) noexcept {
        try {
            return ops.attachThreadInput
                ? ops.attachThreadInput(
                    ops.context,source,currentThread,attach)
                : FALSE;
        } catch(...) { return FALSE; }
    };
    if(plan.attachDesktop){
        result.desktopAttachAttempted=true;
        const BOOL attached=callAttach(desktopThread,TRUE);
        result.desktopAttached=attached!=FALSE;
        emitBool(PickerTraceApiKind::AttachDesktopInput,attached,
                 desktopThread,currentThread);
    }
    if(plan.attachForeground){
        result.foregroundAttachAttempted=true;
        const BOOL attached=callAttach(foregroundThread,TRUE);
        result.foregroundAttached=attached!=FALSE;
        emitBool(PickerTraceApiKind::AttachForegroundInput,attached,
                 foregroundThread,currentThread);
    }
    if(plan.focusShell){
        try {
            result.focusResult=ops.setForegroundWindow
                ? ops.setForegroundWindow(ops.context,progman) : FALSE;
        } catch(...) { result.focusResult=FALSE; }
        emitBool(PickerTraceApiKind::SetForegroundWindow,
                 result.focusResult,0,0,progman);
    }
    if(result.foregroundAttached){
        result.foregroundDetachResult=
            callAttach(foregroundThread,FALSE);
        emitBool(PickerTraceApiKind::DetachForegroundInput,
                 result.foregroundDetachResult,
                 foregroundThread,currentThread);
    }
    if(result.desktopAttached){
        result.desktopDetachResult=callAttach(desktopThread,FALSE);
        emitBool(PickerTraceApiKind::DetachDesktopInput,
                 result.desktopDetachResult,desktopThread,currentThread);
    }
    result.invoked=true;
    try {
        result.switchResult=ops.switchDesktop
            ? ops.switchDesktop(ops.context) : E_POINTER;
    } catch(...) { result.switchResult=E_FAIL; }
    PickerTraceApiResultEvent switchEvent;
    switchEvent.api=PickerTraceApiKind::SwitchDesktop;
    switchEvent.resultKind=PickerTraceRawResultKind::HResult;
    switchEvent.hresult=result.switchResult;
    switchEvent.invoked=true;
    EmitPickerTraceApiResult(observer,switchEvent);

    if(progman){
        try {
            result.previousVisibility=ops.showWindow
                ? ops.showWindow(ops.context,progman,SW_MINIMIZE) : FALSE;
        } catch(...) { result.previousVisibility=FALSE; }
        emitBool(PickerTraceApiKind::ShowWindowProgmanCleanup,
                 result.previousVisibility,0,0,progman,
                 PickerTraceRawResultKind::PreviousVisibility);
    }
    return result;
}

static bool PickerTraceRollbackPhase(PickerPhase phase) noexcept {
    switch(phase){
    case PickerPhase::RollbackTargetIssue:
    case PickerPhase::RollbackTargetVerify:
    case PickerPhase::RollbackPopupIssue:
    case PickerPhase::RollbackPopupVerify:
    case PickerPhase::RollbackSwitchIssue:
    case PickerPhase::OriginVerify:
        return true;
    default:
        return false;
    }
}

void ObservePickerTraceTerminalMetadata(
        PickerTraceTerminalMetadata& metadata,
        PickerPhase before,PickerPhase after,
        const PickerObservation& observation,
        bool queueConflict,bool caughtException) noexcept {
    if(metadata.rollbackTrigger!=PickerTraceRollbackTrigger::None ||
       metadata.diagnosticCode!=PickerTraceDiagnosticCode::None) return;
    if(metadata.generation==0 || before==PickerPhase::Idle) return;
    if(caughtException){
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::Exception;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::Exception;
        return;
    }
    if(queueConflict){
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::QueueConflict;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::QueueConflict;
        return;
    }
    if(observation.generation!=metadata.generation) return;
    if(observation.event==PickerEvent::CancelRequested){
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::Cancellation;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::Cancelled;
        return;
    }
    if(PickerTraceRollbackPhase(before) ||
       (!PickerTraceRollbackPhase(after) &&
        after!=PickerPhase::RefreshModel)) return;
    if(observation.identity==PickerIdentityValidity::Lost){
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::IdentityLost;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::IdentityLost;
        return;
    }
    if(observation.identity==PickerIdentityValidity::Indeterminate){
        metadata.rollbackTrigger=
            PickerTraceRollbackTrigger::IdentityIndeterminate;
        metadata.diagnosticCode=
            PickerTraceDiagnosticCode::IdentityIndeterminate;
        return;
    }
    if(observation.targetRead==PickerReadValidity::Unavailable ||
       observation.popupRead==PickerReadValidity::Unavailable ||
       observation.currentRead==PickerReadValidity::Unavailable){
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::ReadUnavailable;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::ReadUnavailable;
        return;
    }
    switch(before){
    case PickerPhase::TargetIssue:
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::TargetMove;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::ApiRejected;
        break;
    case PickerPhase::TargetVerify:
    case PickerPhase::IdentityVerifyBeforePopup:
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::TargetVerify;
        metadata.diagnosticCode=
            PickerTraceDiagnosticCode::VerificationMismatch;
        break;
    case PickerPhase::PopupIssue:
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::PopupMove;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::ApiRejected;
        break;
    case PickerPhase::PopupVerify:
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::PopupVerify;
        metadata.diagnosticCode=
            PickerTraceDiagnosticCode::VerificationMismatch;
        break;
    case PickerPhase::IdentityVerifyBeforeSwitch:
        metadata.rollbackTrigger=
            PickerTraceRollbackTrigger::RetryBudgetExhausted;
        metadata.diagnosticCode=
            PickerTraceDiagnosticCode::RetryBudgetExhausted;
        break;
    case PickerPhase::SwitchIssue:
        metadata.rollbackTrigger=PickerTraceRollbackTrigger::DesktopSwitch;
        metadata.diagnosticCode=PickerTraceDiagnosticCode::ApiRejected;
        break;
    case PickerPhase::DestinationVerify:
        metadata.rollbackTrigger=
            observation.effectKind==PickerEffectKind::ReadPopup
                ? PickerTraceRollbackTrigger::PopupVerify
                : PickerTraceRollbackTrigger::DesktopSwitch;
        metadata.diagnosticCode=
            PickerTraceDiagnosticCode::VerificationMismatch;
        break;
    default:
        break;
    }
}

PickerEffect AdvancePickerTransitionTraced(
        PickerState& state,const PickerObservation& observation,
        PickerTraceSession* trace,
        PickerTraceTerminalMetadata* metadata) noexcept {
    const PickerPhase phaseBefore=state.transition.phase;
    const uint64_t generationBefore=state.transition.generation;
    const PickerEffect result=AdvancePickerTransition(state,observation);
    if(metadata && observation.event==PickerEvent::Begin &&
       result.kind!=PickerEffectKind::None){
        *metadata=PickerTraceTerminalMetadata{};
        metadata->generation=state.transition.generation;
    }
    if(metadata){
        ObservePickerTraceTerminalMetadata(
            *metadata,phaseBefore,state.transition.phase,
            observation,false,false);
    }
    if(trace && trace->active()){
        PickerTraceEffectEvent event;
        event.stage=PickerTraceEffectStage::Reduce;
        event.generation=generationBefore;
        event.effectSerial=observation.effectSerial;
        event.observationEvent=observation.event;
        event.effect=observation.effectKind;
        event.nextEffect=result.kind;
        event.phaseBefore=phaseBefore;
        event.phaseAfter=state.transition.phase;
        event.identity=observation.identity;
        event.targetRead=observation.targetRead;
        event.popupRead=observation.popupRead;
        event.currentRead=observation.currentRead;
        event.apiInvoked=observation.apiInvoked;
        event.apiAccepted=observation.apiAccepted;
        trace->emit(event);
    }
    return result;
}

void StorePickerTracePendingTerminalDelivery(
        PickerTracePendingTerminalDelivery& pending,uint64_t generation,
        PickerTraceDeliveryRoute route) noexcept {
    pending.generation=generation;
    pending.route=route;
    pending.available=true;
}

void ResetPickerTracePendingTerminalDelivery(
        PickerTracePendingTerminalDelivery& pending) noexcept {
    pending=PickerTracePendingTerminalDelivery{};
}

bool ConsumePickerTracePendingTerminalDelivery(
        PickerTracePendingTerminalDelivery& pending,uint64_t generation,
        PickerTraceDeliveryRoute& route) noexcept {
    route=PickerTraceDeliveryRoute::None;
    if(!pending.available) return false;
    if(pending.generation!=generation){
        ResetPickerTracePendingTerminalDelivery(pending);
        return false;
    }
    route=pending.route;
    ResetPickerTracePendingTerminalDelivery(pending);
    return true;
}

PickerTraceTerminalizationReason DecidePickerTraceTerminalizationReason(
        bool terminalAcknowledged,bool pendingEffectNone,
        bool releaseThrew,bool reservationReleased,
        bool runtimeKeyPresent,bool ready,bool finalized) noexcept {
    if(!terminalAcknowledged)
        return PickerTraceTerminalizationReason::TerminalNotAcknowledged;
    if(!pendingEffectNone)
        return PickerTraceTerminalizationReason::PendingEffect;
    if(releaseThrew)
        return PickerTraceTerminalizationReason::ReservationReleaseException;
    if(!reservationReleased)
        return PickerTraceTerminalizationReason::ReservationNotReleased;
    if(!runtimeKeyPresent)
        return PickerTraceTerminalizationReason::RuntimeKeyMissing;
    if(!ready)
        return PickerTraceTerminalizationReason::RuntimeNotReady;
    if(!finalized)
        return PickerTraceTerminalizationReason::FinalizeStateFailed;
    return PickerTraceTerminalizationReason::Completed;
}

PickerTraceDeliveryRoute DecidePickerTraceDeliveryRoute(
        bool shutdownDrain,bool delayedTimer,bool posted,bool timerArmed,
        bool inlineFallback,bool durableKick) noexcept {
    if(shutdownDrain) return PickerTraceDeliveryRoute::ShutdownDrain;
    if(delayedTimer) return PickerTraceDeliveryRoute::DelayedTimer;
    if(posted) return PickerTraceDeliveryRoute::Posted;
    if(timerArmed) return PickerTraceDeliveryRoute::TimerArmed;
    if(inlineFallback) return PickerTraceDeliveryRoute::InlineFallback;
    if(durableKick) return PickerTraceDeliveryRoute::DurableExternalKick;
    return PickerTraceDeliveryRoute::None;
}

PickerTraceTerminalizationEmission MapPickerTraceTerminalization(
        const PickerTraceTerminalizationRunResult& run,uint64_t generation,
        uint64_t attempt,
        const PickerTraceTerminalDeliveryFacts& delivery) noexcept {
    PickerTraceTerminalizationEmission result;
    result.attempt.generation=generation;
    result.attempt.attempt=attempt;
    result.attempt.reason=run.reason;
    result.attempt.incomingDelivery=delivery.incoming;
    result.attempt.retryDelivery=delivery.retry;
    result.attempt.incomingDeliveryAvailable=delivery.incomingAvailable;
    result.attempt.retryDeliveryAvailable=delivery.retryAvailable;
    result.attempt.terminalAcknowledged=run.terminalAcknowledged;
    result.attempt.pendingEffectNone=run.pendingEffectNone;
    result.attempt.runtimeKeyPresent=run.runtimeKeyPresent;
    result.attempt.releaseAttempted=run.release.attempted;
    result.attempt.firstReleaseActionAvailable=
        run.release.firstActionAvailable;
    result.attempt.retryReleaseActionAvailable=
        run.release.retryActionAvailable;
    result.attempt.releaseExceptionStageAvailable=
        run.release.exceptionStageAvailable;
    result.attempt.firstReleaseAction=run.release.firstAction;
    result.attempt.retryReleaseAction=run.release.retryAction;
    result.attempt.releaseExceptionStage=run.release.exceptionStage;
    result.attempt.releaseRetried=run.release.retried;
    result.attempt.releaseThrew=run.release.threw;
    result.attempt.reservationReleased=run.release.released;
    result.attempt.ready=run.ready;
    result.attempt.finalized=run.finalized;
    result.terminalAllowed=run.completed;
    return result;
}

void PublishPickerTraceTerminalization(
        const PickerTraceTerminalizationEmission& emission,
        const PickerTraceTransitionTerminalEvent* terminal,
        const PickerTraceTerminalizationEventObserver* observer) noexcept {
    if(!observer) return;
    if(observer->emitAttempt)
        observer->emitAttempt(observer->context,emission.attempt);
    if(!emission.terminalAllowed || !terminal) return;
    if(observer->emitTerminal)
        observer->emitTerminal(observer->context,*terminal);
    if(observer->flushBoundary)
        observer->flushBoundary(observer->context);
}

const char* PickerTraceRecaptureName(WindowIdentityRecapture value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(WindowIdentityRecapture,Match,"match");
    VDE_TRACE_NAME_CASE(WindowIdentityRecapture,Lost,"lost");
    VDE_TRACE_NAME_CASE(WindowIdentityRecapture,Indeterminate,"indeterminate");
    }
    return "unknown";
}

const char* PickerTracePointerTargetName(PickerPointerTarget value) noexcept {
    switch(value){
    VDE_TRACE_NAME_CASE(PickerPointerTarget,None,"none");
    VDE_TRACE_NAME_CASE(PickerPointerTarget,Footer,"footer");
    VDE_TRACE_NAME_CASE(PickerPointerTarget,ClearSearch,"clear_search");
    VDE_TRACE_NAME_CASE(PickerPointerTarget,Search,"search");
    VDE_TRACE_NAME_CASE(PickerPointerTarget,Tile,"tile");
    }
    return "unknown";
}

#undef VDE_TRACE_NAME_CASE

static bool PickerTraceWideToUtf8(
        const wchar_t* value,int length,std::string& output) noexcept {
    try {
        output.clear();
        if(!value || length<0) return false;
        if(length==0) return true;
        const int required=WideCharToMultiByte(
            CP_UTF8,WC_ERR_INVALID_CHARS,value,length,
            nullptr,0,nullptr,nullptr);
        if(required<=0) return false;
        std::string converted(static_cast<size_t>(required),'\0');
        const int written=WideCharToMultiByte(
            CP_UTF8,WC_ERR_INVALID_CHARS,value,length,
            &converted[0],required,nullptr,nullptr);
        if(written!=required) return false;
        output.swap(converted);
        return true;
    } catch(...) {
        output.clear();
        return false;
    }
}

static std::string PickerTraceGuidText(const GUID& value) {
    char buffer[37]={0};
    const int written=std::snprintf(
        buffer,sizeof(buffer),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(value.Data1),
        static_cast<unsigned>(value.Data2),
        static_cast<unsigned>(value.Data3),
        static_cast<unsigned>(value.Data4[0]),
        static_cast<unsigned>(value.Data4[1]),
        static_cast<unsigned>(value.Data4[2]),
        static_cast<unsigned>(value.Data4[3]),
        static_cast<unsigned>(value.Data4[4]),
        static_cast<unsigned>(value.Data4[5]),
        static_cast<unsigned>(value.Data4[6]),
        static_cast<unsigned>(value.Data4[7]));
    return written==36 ? std::string(buffer,36) : std::string();
}

static std::string PickerTraceSessionText(
        const std::array<unsigned char,16>& session) {
    static const char digits[]="0123456789abcdef";
    std::string result(32,'0');
    for(size_t index=0;index<session.size();++index){
        result[index*2]=digits[(session[index]>>4)&0x0f];
        result[index*2+1]=digits[session[index]&0x0f];
    }
    return result;
}

class PickerTraceJsonObject {
public:
    PickerTraceJsonObject() { bytes_="{"; }

    bool string(const char* keyValue,const std::string& value) noexcept {
        try {
            if(!key(keyValue)) return false;
            bytes_.push_back('"');
            if(!appendEscaped(value,bytes_)) return false;
            bytes_.push_back('"');
            return true;
        } catch(...) { return false; }
    }

    bool boolean(const char* keyValue,bool value) noexcept {
        try {
            if(!key(keyValue)) return false;
            bytes_+=value ? "true" : "false";
            return true;
        } catch(...) { return false; }
    }

    bool unsignedNumber(const char* keyValue,uint64_t value) noexcept {
        try {
            if(!key(keyValue)) return false;
            bytes_+=std::to_string(value);
            return true;
        } catch(...) { return false; }
    }

    bool signedNumber(const char* keyValue,int64_t value) noexcept {
        try {
            if(!key(keyValue)) return false;
            bytes_+=std::to_string(value);
            return true;
        } catch(...) { return false; }
    }

    bool hex32(const char* keyValue,uint32_t value) noexcept {
        char buffer[11]={0};
        const int written=std::snprintf(
            buffer,sizeof(buffer),"0x%08lx",
            static_cast<unsigned long>(value));
        return written==10 && string(keyValue,std::string(buffer,10));
    }

    bool hex64(const char* keyValue,uint64_t value) noexcept {
        char buffer[19]={0};
        const int written=std::snprintf(
            buffer,sizeof(buffer),"0x%016llx",
            static_cast<unsigned long long>(value));
        return written==18 && string(keyValue,std::string(buffer,18));
    }

    bool guid(const char* keyValue,const GUID& value) noexcept {
        try {
            const std::string text=PickerTraceGuidText(value);
            return text.size()==36 && string(keyValue,text);
        } catch(...) { return false; }
    }

    bool raw(const char* keyValue,const std::string& value) noexcept {
        try {
            if(!key(keyValue)) return false;
            bytes_+=value;
            return true;
        } catch(...) { return false; }
    }

    bool finish(std::string& output) noexcept {
        try {
            bytes_+="}\n";
            output.swap(bytes_);
            return true;
        } catch(...) {
            output.clear();
            return false;
        }
    }

private:
    bool key(const char* value) noexcept {
        try {
            if(!value) return false;
            if(!first_) bytes_.push_back(',');
            first_=false;
            bytes_.push_back('"');
            if(!appendEscaped(std::string(value),bytes_)) return false;
            bytes_+="\":";
            return true;
        } catch(...) { return false; }
    }

    static bool appendEscaped(const std::string& value,
                              std::string& output) noexcept {
        try {
            static const char digits[]="0123456789abcdef";
            for(unsigned char byte : value){
                if(byte=='"' || byte=='\\'){
                    output.push_back('\\');
                    output.push_back(static_cast<char>(byte));
                } else if(byte<0x20){
                    output+="\\u00";
                    output.push_back(digits[(byte>>4)&0x0f]);
                    output.push_back(digits[byte&0x0f]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
            }
            return true;
        } catch(...) { return false; }
    }

    std::string bytes_;
    bool first_=true;
};

static bool PickerTraceBegin(PickerTraceJsonObject& json,
                             const PickerTraceEnvelope& envelope,
                             const char* eventName) noexcept {
    try {
        return json.unsignedNumber("schema",1) &&
               json.string("session",PickerTraceSessionText(envelope.session)) &&
               json.unsignedNumber("seq",envelope.seq) &&
               json.unsignedNumber("ms",envelope.ms) &&
               json.string("event",eventName ? eventName : "unknown");
    } catch(...) { return false; }
}

template<class Append>
static bool PickerTraceSerialize(const PickerTraceEnvelope& envelope,
                                 const char* eventName,Append append,
                                 std::string& output) noexcept {
    output.clear();
    try {
        PickerTraceJsonObject json;
        if(!PickerTraceBegin(json,envelope,eventName) || !append(json))
            return false;
        return json.finish(output);
    } catch(...) {
        output.clear();
        return false;
    }
}

static bool PickerTraceGuidArray(const std::vector<GUID>& values,
                                 std::string& output) noexcept {
    output.clear();
    try {
        output.push_back('[');
        for(size_t index=0;index<values.size();++index){
            if(index) output.push_back(',');
            const std::string guid=PickerTraceGuidText(values[index]);
            if(guid.size()!=36) return false;
            output.push_back('"');
            output+=guid;
            output.push_back('"');
        }
        output.push_back(']');
        return true;
    } catch(...) {
        output.clear();
        return false;
    }
}

template<size_t Size>
static bool PickerTraceCountArray(const std::array<uint64_t,Size>& values,
                                  std::string& output) noexcept {
    output.clear();
    try {
        output.push_back('[');
        for(size_t index=0;index<values.size();++index){
            if(index) output.push_back(',');
            output+=std::to_string(values[index]);
        }
        output.push_back(']');
        return true;
    } catch(...) {
        output.clear();
        return false;
    }
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceCaptureEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"picker.capture",[&](auto& json){
        return json.hex64("hwnd",static_cast<uint64_t>(event.hwnd)) &&
            json.unsignedNumber("pid",event.pid) &&
            json.unsignedNumber("tid",event.tid) &&
            json.signedNumber("title_length",event.titleLength) &&
            json.boolean("window_valid",event.windowValid) &&
            json.boolean("title_read",event.titleRead) &&
            json.boolean("identity_complete",event.identityComplete) &&
            json.string("recapture",PickerTraceRecaptureName(event.recapture)) &&
            json.boolean("target_published",event.targetPublished);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceOpenEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"picker.open",[&](auto& json){
        return json.string("result",PickerTraceOpenResultName(event.result)) &&
            json.string("desktop_snapshot",PickerTraceDesktopSnapshotStatusName(event.desktopSnapshot)) &&
            json.hex32("desktop_snapshot_result",static_cast<uint32_t>(event.desktopSnapshotResult)) &&
            json.signedNumber("desktop_snapshot_index",event.desktopSnapshotIndex) &&
            json.unsignedNumber("desktop_snapshot_count",event.desktopSnapshotCount) &&
            json.guid("current_desktop",event.currentDesktop) &&
            json.unsignedNumber("model_generation",event.modelGeneration) &&
            json.boolean("current_desktop_available",event.currentDesktopAvailable) &&
            json.boolean("target_identity_present",event.targetIdentityPresent);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceEnumBeginEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"enum.begin",[&](auto& json){
        std::string desktops;
        return PickerTraceGuidArray(event.desktops,desktops) &&
            json.unsignedNumber("model_generation",event.modelGeneration) &&
            json.raw("desktops",desktops);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceEnumWindowEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"enum.window",[&](auto& json){
        std::string className;
        std::string imageBasename;
        if(event.className.available() &&
           !PickerTraceWideToUtf8(event.className.data(),
                                  event.className.length(),className)) return false;
        if(event.imageBasename.available() &&
           !PickerTraceWideToUtf8(event.imageBasename.data(),
                                  event.imageBasename.length(),imageBasename)) return false;
        bool ok=json.unsignedNumber("model_generation",event.modelGeneration) &&
            json.unsignedNumber("enum_sequence",event.enumSequence) &&
            json.hex64("hwnd",static_cast<uint64_t>(event.hwnd)) &&
            json.hex64("owner",static_cast<uint64_t>(event.owner)) &&
            json.hex64("root_owner",static_cast<uint64_t>(event.rootOwner)) &&
            json.hex64("last_active_popup",static_cast<uint64_t>(event.lastActivePopup)) &&
            json.unsignedNumber("pid",event.pid) &&
            json.unsignedNumber("tid",event.tid) &&
            json.boolean("class_name_available",event.className.available());
        if(ok && event.className.available()) ok=json.string("class_name",className);
        ok=ok && json.boolean("image_basename_available",event.imageBasename.available());
        if(ok && event.imageBasename.available()) ok=json.string("image_basename",imageBasename);
        return ok &&
            json.boolean("visible_observed",event.visibleObserved) &&
            json.boolean("visible",event.visible) &&
            json.boolean("first_title_observed",event.firstTitleObserved) &&
            json.signedNumber("first_title_length",event.firstTitleLength) &&
            json.hex32("first_title_error",event.firstTitleError) &&
            json.boolean("ex_style_observed",event.exStyleObserved) &&
            json.hex64("ex_style",event.exStyle) &&
            json.hex32("ex_style_error",event.exStyleError) &&
            json.boolean("tool_window",event.toolWindow) &&
            json.boolean("root_owner_observed",event.rootOwnerObserved) &&
            json.boolean("root_owner_self",event.rootOwnerSelf) &&
            json.boolean("cloaked_observed",event.cloakedObserved) &&
            json.unsignedNumber("cloaked",event.cloaked) &&
            json.hex32("cloaked_result",static_cast<uint32_t>(event.cloakedResult)) &&
            json.string("alt_tab_reason",PickerTraceAltTabReasonName(event.altTabReason)) &&
            json.hex32("desktop_result",static_cast<uint32_t>(event.desktopResult)) &&
            json.guid("desktop",event.desktop) &&
            json.signedNumber("tile_index",event.tileIndex) &&
            json.boolean("second_title_observed",event.secondTitleObserved) &&
            json.signedNumber("second_title_length",event.secondTitleLength) &&
            json.signedNumber("second_title_copied",event.secondTitleCopied) &&
            json.hex32("second_title_error",event.secondTitleError) &&
            json.boolean("process_start_cache_hit",event.processStartCacheHit) &&
            json.boolean("process_start_available",event.processStartAvailable) &&
            json.hex32("process_start_error",event.processStartError) &&
            json.boolean("identity_complete",event.identityComplete) &&
            json.string("recapture",PickerTraceRecaptureName(event.recapture)) &&
            json.string("decision",PickerTraceEnumDecisionName(event.decision));
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceEnumEndEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"enum.end",[&](auto& json){
        std::string counts;
        return PickerTraceCountArray(event.counts,counts) &&
            json.unsignedNumber("model_generation",event.modelGeneration) &&
            json.unsignedNumber("candidates",event.candidates) &&
            json.raw("decision_counts",counts) &&
            json.boolean("enum_windows_returned",event.enumWindowsReturned) &&
            json.hex32("enum_windows_error",event.enumWindowsError) &&
            json.boolean("model_published",event.modelPublished);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceMouseDownEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"mouse.down",[&](auto& json){
        return json.hex64("raw_wparam",event.rawWparam) &&
            json.signedNumber("x",event.x) &&
            json.signedNumber("y",event.y) &&
            json.boolean("ctrl",event.ctrl) &&
            json.boolean("controlled",event.controlled) &&
            json.boolean("search_active",event.searchActive) &&
            json.string("target",PickerTracePointerTargetName(event.target)) &&
            json.signedNumber("tile_index",event.tileIndex);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceActivationRequestEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"activation.request",[&](auto& json){
        return json.unsignedNumber("activation_id",event.activationId) &&
            json.string("source",PickerTraceActivationSourceName(event.source)) &&
            json.boolean("ctrl",event.ctrl) &&
            json.signedNumber("tile_index",event.tileIndex);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceActivationResultEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"activation.result",[&](auto& json){
        return json.unsignedNumber("activation_id",event.activationId) &&
            json.string("result",PickerTraceActivationResultName(event.result));
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceMoveBeginEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"move.begin",[&](auto& json){
        return json.unsignedNumber("activation_id",event.activationId) &&
            json.unsignedNumber("generation",event.generation) &&
            json.signedNumber("tile_index",event.tileIndex) &&
            json.string("reason",PickerTraceMoveBeginReasonName(event.reason)) &&
            json.guid("target_origin",event.targetOrigin) &&
            json.guid("popup_origin",event.popupOrigin) &&
            json.guid("current_origin",event.currentOrigin) &&
            json.guid("destination",event.destination) &&
            json.string("first_effect",PickerTraceEffectKindName(event.firstEffect));
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceMoveBeginExceptionEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"move.begin.exception",[&](auto& json){
        return json.unsignedNumber("activation_id",event.activationId) &&
            json.boolean("transition_published",event.transitionPublished);
    },output);
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceEffectEvent& event,
                              std::string& output) noexcept {
    try {
        const std::string eventName=std::string("effect.")+
            PickerTraceEffectStageName(event.stage);
        return PickerTraceSerialize(envelope,eventName.c_str(),[&](auto& json){
            return json.unsignedNumber("generation",event.generation) &&
                json.unsignedNumber("effect_serial",event.effectSerial) &&
                json.string("observation_event",PickerTraceEventName(event.observationEvent)) &&
                json.string("effect",PickerTraceEffectKindName(event.effect)) &&
                json.string("next_effect",PickerTraceEffectKindName(event.nextEffect)) &&
                json.string("phase_before",PickerTracePhaseName(event.phaseBefore)) &&
                json.string("phase_after",PickerTracePhaseName(event.phaseAfter)) &&
                json.string("identity",PickerTraceIdentityValidityName(event.identity)) &&
                json.string("target_read",PickerTraceReadValidityName(event.targetRead)) &&
                json.string("popup_read",PickerTraceReadValidityName(event.popupRead)) &&
                json.string("current_read",PickerTraceReadValidityName(event.currentRead)) &&
                json.string("execution_route",PickerTraceEffectExecutionRouteName(event.executionRoute)) &&
                json.string("delivery",PickerTraceDeliveryRouteName(event.delivery)) &&
                json.unsignedNumber("delivery_attempt",event.deliveryAttempt) &&
                json.boolean("execution_route_available",event.executionRouteAvailable) &&
                json.boolean("delivery_available",event.deliveryAvailable) &&
                json.boolean("api_invoked",event.apiInvoked) &&
                json.boolean("api_accepted",event.apiAccepted);
        },output);
    } catch(...) {
        output.clear();
        return false;
    }
}

bool SerializePickerTraceLine(const PickerTraceEnvelope& envelope,
                              const PickerTraceApiResultEvent& event,
                              std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"api.result",[&](auto& json){
        const bool common=
            json.string("api",PickerTraceApiKindName(event.api)) &&
            json.string("lookup_use",PickerTraceDesktopLookupUseName(event.lookupUse)) &&
            json.string("lookup_stage",PickerTraceDesktopLookupStageName(event.lookupStage)) &&
            json.string("result_kind",PickerTraceRawResultKindName(event.resultKind)) &&
            json.unsignedNumber("generation",event.generation) &&
            json.unsignedNumber("effect_serial",event.effectSerial) &&
            json.hex64("hwnd",static_cast<uint64_t>(event.hwnd)) &&
            json.unsignedNumber("source_thread",event.sourceThread) &&
            json.unsignedNumber("destination_thread",event.destinationThread) &&
            json.signedNumber("index",event.index) &&
            json.guid("requested_desktop",event.requestedDesktop) &&
            json.guid("actual_desktop",event.actualDesktop) &&
            json.hex32("hresult",static_cast<uint32_t>(event.hresult)) &&
            json.boolean("bool_result",event.boolResult) &&
            json.boolean("invoked",event.invoked) &&
            json.boolean("last_error_available",event.lastErrorAvailable) &&
            json.hex32("last_error",event.lastError);
        if(!common) return false;
        return event.resultKind!=PickerTraceRawResultKind::PreviousVisibility ||
            json.boolean("previously_visible",event.boolResult);
    },output);
}

bool SerializePickerTraceLine(
        const PickerTraceEnvelope& envelope,
        const PickerTraceTerminalizationAttemptEvent& event,
        std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"terminalization.attempt",[&](auto& json){
        bool ok=json.unsignedNumber("generation",event.generation) &&
            json.unsignedNumber("attempt",event.attempt) &&
            json.string("reason",PickerTraceTerminalizationReasonName(event.reason)) &&
            json.string("incoming_delivery",PickerTraceDeliveryRouteName(event.incomingDelivery)) &&
            json.string("retry_delivery",PickerTraceDeliveryRouteName(event.retryDelivery)) &&
            json.boolean("incoming_delivery_available",event.incomingDeliveryAvailable) &&
            json.boolean("retry_delivery_available",event.retryDeliveryAvailable) &&
            json.boolean("terminal_acknowledged",event.terminalAcknowledged) &&
            json.boolean("pending_effect_none",event.pendingEffectNone) &&
            json.boolean("runtime_key_present",event.runtimeKeyPresent);
        if(ok && event.firstReleaseActionAvailable)
            ok=json.string("first_release_action",
                PickerTraceTerminalGuardReleaseActionName(
                    event.firstReleaseAction));
        if(ok && event.retryReleaseActionAvailable)
            ok=json.string("retry_release_action",
                PickerTraceTerminalGuardReleaseActionName(
                    event.retryReleaseAction));
        if(ok && event.releaseExceptionStageAvailable)
            ok=json.string("release_exception_stage",
                PickerTraceReservationExceptionStageName(
                    event.releaseExceptionStage));
        return ok &&
            json.boolean("release_attempted",event.releaseAttempted) &&
            json.boolean("first_release_action_available",event.firstReleaseActionAvailable) &&
            json.boolean("retry_release_action_available",event.retryReleaseActionAvailable) &&
            json.boolean("release_exception_stage_available",event.releaseExceptionStageAvailable) &&
            json.boolean("release_retried",event.releaseRetried) &&
            json.boolean("release_threw",event.releaseThrew) &&
            json.boolean("reservation_released",event.reservationReleased) &&
            json.boolean("ready",event.ready) &&
            json.boolean("finalized",event.finalized);
    },output);
}

bool SerializePickerTraceLine(
        const PickerTraceEnvelope& envelope,
        const PickerTraceTransitionTerminalEvent& event,
        std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"transition.terminal",[&](auto& json){
        return json.unsignedNumber("generation",event.generation) &&
            json.string("outcome",PickerTraceTerminalOutcomeName(event.outcome)) &&
            json.string("rollback_trigger",PickerTraceRollbackTriggerName(event.rollbackTrigger)) &&
            json.string("diagnostic_code",PickerTraceDiagnosticCodeName(event.diagnosticCode)) &&
            json.signedNumber("forward_target_attempts",event.forwardTargetAttempts) &&
            json.signedNumber("forward_popup_attempts",event.forwardPopupAttempts) &&
            json.signedNumber("forward_switch_attempts",event.forwardSwitchAttempts) &&
            json.signedNumber("rollback_target_attempts",event.rollbackTargetAttempts) &&
            json.signedNumber("rollback_popup_attempts",event.rollbackPopupAttempts) &&
            json.signedNumber("rollback_switch_attempts",event.rollbackSwitchAttempts) &&
            json.signedNumber("focus_attempts",event.focusAttempts) &&
            json.string("target_read",PickerTraceReadValidityName(event.targetRead)) &&
            json.string("popup_read",PickerTraceReadValidityName(event.popupRead)) &&
            json.string("current_read",PickerTraceReadValidityName(event.currentRead)) &&
            json.guid("target_desktop",event.targetDesktop) &&
            json.guid("popup_desktop",event.popupDesktop) &&
            json.guid("current_desktop",event.currentDesktop);
    },output);
}

static bool SerializePickerTraceTruncatedLine(
        const PickerTraceEnvelope& envelope,std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"trace.truncated",
        [](auto&){ return true; },output);
}

PickerTraceWriter::PickerTraceWriter(
        PickerTraceSinkOps ops,PickerTraceLimits limits,
        const std::array<unsigned char,16>& session) noexcept
    :ops_(std::move(ops)),limits_(limits) {
    envelope_.session=session;
    if(!ops_.write || !ops_.flush || !ops_.close || !ops_.monotonicMs ||
       limits_.maxBytes==0 || limits_.maxEvents==0) return;
    try {
        startMs_=ops_.monotonicMs();
        active_=true;
    } catch(...) {
        active_=false;
    }
}

PickerTraceWriter::~PickerTraceWriter() noexcept {
    close();
}

bool PickerTraceWriter::active() const noexcept {
    return active_ && !closed_;
}

void PickerTraceWriter::flushBoundary() noexcept {
    if(!active()) return;
    try {
        if(!ops_.flush()) active_=false;
    } catch(...) {
        active_=false;
    }
}

void PickerTraceWriter::close() noexcept {
    if(closed_) return;
    closed_=true;
    active_=false;
    try {
        if(ops_.close) (void)ops_.close();
    } catch(...) {}
}

bool PickerTraceWriter::writeWhole(const std::string& bytes) noexcept {
    if(!active() || bytes.empty()) return false;
    try {
        const size_t written=ops_.write(bytes.data(),bytes.size());
        if(written!=bytes.size()){
            active_=false;
            return false;
        }
        return true;
    } catch(...) {
        active_=false;
        return false;
    }
}

void PickerTraceWriter::truncate() noexcept {
    if(!active() || truncated_) return;
    truncated_=true;
    PickerTraceEnvelope candidate=envelope_;
    std::string line;
    try {
        if(candidate.seq==(std::numeric_limits<uint64_t>::max)()){
            active_=false;
            return;
        }
        const uint64_t now=ops_.monotonicMs();
        uint64_t elapsed=now>=startMs_ ? now-startMs_ : 0;
        if(elapsed<lastElapsedMs_) elapsed=lastElapsedMs_;
        candidate.seq++;
        candidate.ms=elapsed;
        if(eventsWritten_>=limits_.maxEvents ||
           !SerializePickerTraceTruncatedLine(candidate,line) ||
           line.size()>limits_.maxBytes-bytesWritten_ ||
           !writeWhole(line)){
            active_=false;
            return;
        }
        envelope_=candidate;
        lastElapsedMs_=elapsed;
        bytesWritten_+=static_cast<uint64_t>(line.size());
        ++eventsWritten_;
    } catch(...) {}
    active_=false;
}

template<class Event>
void PickerTraceWriter::emitTyped(const Event& event) noexcept {
    if(!active()) return;
    try {
        PickerTraceEnvelope candidate=envelope_;
        if(candidate.seq==(std::numeric_limits<uint64_t>::max)()){
            active_=false;
            return;
        }
        const uint64_t now=ops_.monotonicMs();
        uint64_t elapsed=now>=startMs_ ? now-startMs_ : 0;
        if(elapsed<lastElapsedMs_) elapsed=lastElapsedMs_;
        candidate.seq++;
        candidate.ms=elapsed;

        std::string line;
        if(!SerializePickerTraceLine(candidate,event,line)){
            active_=false;
            return;
        }
        if(eventsWritten_>=limits_.maxEvents-1){
            truncate();
            return;
        }

        PickerTraceEnvelope reserveEnvelope=candidate;
        if(reserveEnvelope.seq==(std::numeric_limits<uint64_t>::max)()){
            active_=false;
            return;
        }
        ++reserveEnvelope.seq;
        reserveEnvelope.ms=(std::numeric_limits<uint64_t>::max)();
        std::string reserve;
        if(!SerializePickerTraceTruncatedLine(reserveEnvelope,reserve)){
            active_=false;
            return;
        }
        const uint64_t remaining=limits_.maxBytes-bytesWritten_;
        if(line.size()>remaining || reserve.size()>remaining-line.size()){
            truncate();
            return;
        }
        if(!writeWhole(line)) return;
        envelope_=candidate;
        lastElapsedMs_=elapsed;
        bytesWritten_+=static_cast<uint64_t>(line.size());
        ++eventsWritten_;
    } catch(...) {
        active_=false;
    }
}

#define VDE_DEFINE_PICKER_TRACE_EMIT(EventType) \
    void PickerTraceWriter::emit(const EventType& event) noexcept { \
        emitTyped(event); \
    }

VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceCaptureEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceOpenEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceEnumBeginEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceEnumWindowEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceEnumEndEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceMouseDownEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceActivationRequestEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceActivationResultEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceMoveBeginEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceMoveBeginExceptionEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceEffectEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceApiResultEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceTerminalizationAttemptEvent)
VDE_DEFINE_PICKER_TRACE_EMIT(PickerTraceTransitionTerminalEvent)

#undef VDE_DEFINE_PICKER_TRACE_EMIT

static bool PickerTraceFixedDecimal(const std::wstring& value,size_t begin,
                                    size_t count,unsigned& output) noexcept {
    output=0;
    if(begin>value.size() || count>value.size()-begin) return false;
    for(size_t index=0;index<count;++index){
        const wchar_t digit=value[begin+index];
        if(digit<L'0' || digit>L'9') return false;
        output=output*10+static_cast<unsigned>(digit-L'0');
    }
    return true;
}

static bool PickerTraceLeapYear(unsigned year) noexcept {
    return (year%4==0 && year%100!=0) || year%400==0;
}

bool IsPickerTraceFileName(const std::wstring& value) noexcept {
    try {
        static const std::wstring prefix=L"picker-";
        static const std::wstring suffix=L".jsonl";
        if(value.size()<35 || value.size()>44 ||
           value.compare(0,prefix.size(),prefix)!=0 ||
           value.compare(value.size()-suffix.size(),suffix.size(),suffix)!=0 ||
           value[15]!=L'T' || value[22]!=L'.' ||
           value[26]!=L'Z' || value[27]!=L'-') return false;
        unsigned year=0,month=0,day=0,hour=0,minute=0,second=0,millis=0;
        if(!PickerTraceFixedDecimal(value,7,4,year) ||
           !PickerTraceFixedDecimal(value,11,2,month) ||
           !PickerTraceFixedDecimal(value,13,2,day) ||
           !PickerTraceFixedDecimal(value,16,2,hour) ||
           !PickerTraceFixedDecimal(value,18,2,minute) ||
           !PickerTraceFixedDecimal(value,20,2,second) ||
           !PickerTraceFixedDecimal(value,23,3,millis)) return false;
        if(year==0 || month<1 || month>12 || hour>23 ||
           minute>59 || second>59 || millis>999) return false;
        static const unsigned daysByMonth[]={
            0,31,28,31,30,31,30,31,31,30,31,30,31
        };
        unsigned maximumDay=daysByMonth[month];
        if(month==2 && PickerTraceLeapYear(year)) maximumDay=29;
        if(day<1 || day>maximumDay) return false;

        const size_t pidBegin=28;
        const size_t pidEnd=value.size()-suffix.size();
        const size_t pidDigits=pidEnd-pidBegin;
        if(pidDigits<1 || pidDigits>10 || value[pidBegin]==L'0') return false;
        uint64_t pid=0;
        for(size_t index=pidBegin;index<pidEnd;++index){
            if(value[index]<L'0' || value[index]>L'9') return false;
            pid=pid*10+static_cast<unsigned>(value[index]-L'0');
        }
        return pid!=0;
    } catch(...) {
        return false;
    }
}

bool PlanPickerTraceRetention(
        const std::vector<PickerTraceDirectoryEntry>& entries,
        uint64_t now100ns,size_t oldFilesToKeep,
        std::vector<size_t>& remove) noexcept {
    remove.clear();
    try {
        const uint64_t sevenDays=7ULL*24ULL*60ULL*60ULL*10000000ULL;
        std::vector<size_t> retained;
        std::vector<bool> selected(entries.size(),false);
        for(size_t index=0;index<entries.size();++index){
            const PickerTraceDirectoryEntry& entry=entries[index];
            const bool regular=
                (entry.attributes&FILE_ATTRIBUTE_DIRECTORY)==0 &&
                (entry.attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0;
            if(!regular || !IsPickerTraceFileName(entry.name)) continue;
            const bool expired=now100ns>entry.lastWrite100ns &&
                now100ns-entry.lastWrite100ns>sevenDays;
            if(expired) selected[index]=true;
            else retained.push_back(index);
        }
        std::sort(retained.begin(),retained.end(),[&](size_t left,size_t right){
            if(entries[left].lastWrite100ns!=entries[right].lastWrite100ns)
                return entries[left].lastWrite100ns>
                       entries[right].lastWrite100ns;
            if(entries[left].name!=entries[right].name)
                return entries[left].name>entries[right].name;
            return left<right;
        });
        for(size_t index=oldFilesToKeep;index<retained.size();++index)
            selected[retained[index]]=true;
        for(size_t index=0;index<selected.size();++index)
            if(selected[index]) remove.push_back(index);
        return true;
    } catch(...) {
        remove.clear();
        return false;
    }
}

static uint64_t PickerTraceFileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER integer{};
    integer.LowPart=value.dwLowDateTime;
    integer.HighPart=value.dwHighDateTime;
    return integer.QuadPart;
}

static bool PickerTraceIsAbsoluteDirectoryPath(
        const std::wstring& value) noexcept {
    if(value.size()>=3 &&
       ((value[0]>=L'A' && value[0]<=L'Z') ||
        (value[0]>=L'a' && value[0]<=L'z')) &&
       value[1]==L':' && (value[2]==L'\\' || value[2]==L'/')) return true;
    if(value.size()>=5 &&
       (value[0]==L'\\' || value[0]==L'/') &&
       (value[1]==L'\\' || value[1]==L'/')){
        const size_t serverEnd=value.find_first_of(L"\\/",2);
        return serverEnd!=std::wstring::npos && serverEnd>2 &&
               serverEnd+1<value.size();
    }
    return false;
}

static bool PickerTraceHasDotDotComponent(
        const std::wstring& value) noexcept {
    size_t begin=0;
    while(begin<=value.size()){
        const size_t end=value.find_first_of(L"\\/",begin);
        const size_t count=(end==std::wstring::npos ? value.size() : end)-begin;
        if(count==2 && value[begin]==L'.' && value[begin+1]==L'.') return true;
        if(end==std::wstring::npos) break;
        begin=end+1;
    }
    return false;
}

static bool PickerTraceValidateDirectoryAttributes(DWORD attributes) noexcept {
    return attributes!=INVALID_FILE_ATTRIBUTES &&
           (attributes&FILE_ATTRIBUTE_DIRECTORY)!=0 &&
           (attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0;
}

static bool PickerTraceEnsureDirectory(const PickerTraceStorageOps& ops,
                                       const std::wstring& path,
                                       bool mayCreate) noexcept {
    try {
        DWORD attributes=ops.getAttributes(path);
        if(attributes==INVALID_FILE_ATTRIBUTES && mayCreate){
            (void)ops.createDirectory(path);
            attributes=ops.getAttributes(path);
        }
        return PickerTraceValidateDirectoryAttributes(attributes);
    } catch(...) {
        return false;
    }
}

static std::wstring PickerTraceJoinPath(const std::wstring& parent,
                                        const wchar_t* child) {
    std::wstring result=parent;
    if(!result.empty() && result.back()!=L'\\' && result.back()!=L'/')
        result.push_back(L'\\');
    result+=child;
    return result;
}

static bool PickerTraceFileNameForTime(uint64_t fileTime100ns,DWORD processId,
                                       std::wstring& output) noexcept {
    output.clear();
    if(processId==0) return false;
    try {
        ULARGE_INTEGER integer{};
        integer.QuadPart=fileTime100ns;
        FILETIME fileTime{};
        fileTime.dwLowDateTime=integer.LowPart;
        fileTime.dwHighDateTime=integer.HighPart;
        SYSTEMTIME utc{};
        if(!FileTimeToSystemTime(&fileTime,&utc)) return false;
        wchar_t buffer[64]={0};
        const int written=swprintf_s(
            buffer,_countof(buffer),
            L"picker-%04u%02u%02uT%02u%02u%02u.%03uZ-%lu.jsonl",
            static_cast<unsigned>(utc.wYear),
            static_cast<unsigned>(utc.wMonth),
            static_cast<unsigned>(utc.wDay),
            static_cast<unsigned>(utc.wHour),
            static_cast<unsigned>(utc.wMinute),
            static_cast<unsigned>(utc.wSecond),
            static_cast<unsigned>(utc.wMilliseconds),
            static_cast<unsigned long>(processId));
        if(written<=0) return false;
        output.assign(buffer,static_cast<size_t>(written));
        return IsPickerTraceFileName(output);
    } catch(...) {
        output.clear();
        return false;
    }
}

bool OpenPickerTraceStorage(const PickerTraceStorageOps& ops,DWORD processId,
                            PickerTraceOpenedFile& output) noexcept {
    output.handle=INVALID_HANDLE_VALUE;
    output.path.clear();
    if(!ops.localAppData || !ops.getAttributes || !ops.createDirectory ||
       !ops.listDirectory || !ops.deleteFile || !ops.createNew ||
       !ops.utcFileTime100ns) return false;
    try {
        std::wstring base;
        if(!ops.localAppData(base) ||
           !PickerTraceIsAbsoluteDirectoryPath(base) ||
           PickerTraceHasDotDotComponent(base)) return false;
        std::replace(base.begin(),base.end(),L'/',L'\\');
        while(base.size()>3 && base.back()==L'\\') base.pop_back();
        if(!PickerTraceEnsureDirectory(ops,base,false)) return false;

        const std::wstring product=PickerTraceJoinPath(
            base,L"VirtualDesktopsExtention");
        if(!PickerTraceEnsureDirectory(ops,product,true)) return false;
        const std::wstring diagnostics=PickerTraceJoinPath(
            product,L"diagnostics");
        if(!PickerTraceEnsureDirectory(ops,diagnostics,true)) return false;

        const uint64_t now=ops.utcFileTime100ns();
        std::vector<PickerTraceDirectoryEntry> entries;
        if(!ops.listDirectory(diagnostics,entries)) return false;
        std::vector<size_t> remove;
        if(!PlanPickerTraceRetention(entries,now,2,remove)) return false;

        // Names are revalidated immediately before direct-child deletion.
        // This blocks accidental traversal; it is not a handle-relative
        // adversarial TOCTOU sandbox.
        for(size_t index : remove){
            if(index>=entries.size()) return false;
            const PickerTraceDirectoryEntry& entry=entries[index];
            if(!IsPickerTraceFileName(entry.name) ||
               (entry.attributes&FILE_ATTRIBUTE_DIRECTORY)!=0 ||
               (entry.attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0)
                return false;
            if(!ops.deleteFile(PickerTraceJoinPath(
                    diagnostics,entry.name.c_str()))) return false;
        }

        std::wstring fileName;
        if(!PickerTraceFileNameForTime(now,processId,fileName)) return false;
        const std::wstring filePath=PickerTraceJoinPath(
            diagnostics,fileName.c_str());
        output.path=filePath;
        const HANDLE handle=ops.createNew(filePath,CREATE_NEW);
        if(!handle || handle==INVALID_HANDLE_VALUE){
            output.path.clear();
            return false;
        }
        output.handle=handle;
        return true;
    } catch(...) {
        output.handle=INVALID_HANDLE_VALUE;
        output.path.clear();
        return false;
    }
}

PickerTraceStorageOps DefaultPickerTraceStorageOps() noexcept {
    PickerTraceStorageOps ops;
    try {
        ops.localAppData=[](std::wstring& output) noexcept {
            output.clear();
            try {
                const DWORD required=GetEnvironmentVariableW(
                    L"LOCALAPPDATA",nullptr,0);
                if(required==0) return false;
                std::wstring buffer(static_cast<size_t>(required),L'\0');
                const DWORD written=GetEnvironmentVariableW(
                    L"LOCALAPPDATA",&buffer[0],required);
                if(written==0 || written>=required) return false;
                buffer.resize(written);
                output.swap(buffer);
                return true;
            } catch(...) { output.clear(); return false; }
        };
        ops.getAttributes=[](const std::wstring& path) noexcept {
            return GetFileAttributesW(path.c_str());
        };
        ops.createDirectory=[](const std::wstring& path) noexcept {
            return CreateDirectoryW(path.c_str(),nullptr);
        };
        ops.listDirectory=[](const std::wstring& directory,
                             std::vector<PickerTraceDirectoryEntry>& output) noexcept {
            output.clear();
            WIN32_FIND_DATAW data{};
            HANDLE find=INVALID_HANDLE_VALUE;
            try {
                const std::wstring pattern=PickerTraceJoinPath(directory,L"*");
                find=FindFirstFileW(pattern.c_str(),&data);
                if(find==INVALID_HANDLE_VALUE)
                    return GetLastError()==ERROR_FILE_NOT_FOUND;
                for(;;){
                    if(wcscmp(data.cFileName,L".")!=0 &&
                       wcscmp(data.cFileName,L"..")!=0){
                        ULARGE_INTEGER lastWrite{};
                        lastWrite.LowPart=data.ftLastWriteTime.dwLowDateTime;
                        lastWrite.HighPart=data.ftLastWriteTime.dwHighDateTime;
                        output.emplace_back(data.cFileName,data.dwFileAttributes,
                                            lastWrite.QuadPart);
                    }
                    if(!FindNextFileW(find,&data)) break;
                }
                const DWORD error=GetLastError();
                FindClose(find);
                find=INVALID_HANDLE_VALUE;
                return error==ERROR_NO_MORE_FILES;
            } catch(...) {
                if(find!=INVALID_HANDLE_VALUE) FindClose(find);
                output.clear();
                return false;
            }
        };
        ops.deleteFile=[](const std::wstring& path) noexcept {
            return DeleteFileW(path.c_str());
        };
        ops.createNew=[](const std::wstring& path,DWORD disposition) noexcept {
            return CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                               nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
        };
        ops.writeFile=[](HANDLE handle,const void* bytes,DWORD size,
                         DWORD& written) noexcept {
            written=0;
            return WriteFile(handle,bytes,size,&written,nullptr);
        };
        ops.flushFile=[](HANDLE handle) noexcept {
            return FlushFileBuffers(handle);
        };
        ops.closeHandle=[](HANDLE handle) noexcept {
            return CloseHandle(handle);
        };
        ops.utcFileTime100ns=[]() noexcept {
            FILETIME value{};
            GetSystemTimeAsFileTime(&value);
            return PickerTraceFileTimeValue(value);
        };
        ops.monotonicMs=[]() noexcept {
            return static_cast<uint64_t>(GetTickCount64());
        };
    } catch(...) {
        return PickerTraceStorageOps{};
    }
    return ops;
}

class PickerTraceSha256Accumulator {
public:
    PickerTraceSha256Accumulator() noexcept {
        status_=BCryptOpenAlgorithmProvider(
            &algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0);
        if(!BCRYPT_SUCCESS(status_)) return;
        ULONG objectBytes=0;
        ULONG copied=0;
        status_=BCryptGetProperty(
            algorithm_,BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),
            &copied,0);
        if(!BCRYPT_SUCCESS(status_) || copied!=sizeof(objectBytes) ||
           objectBytes==0) return;
        try { object_.resize(objectBytes); }
        catch(...) {
            status_=static_cast<NTSTATUS>(0xc0000017L);
            return;
        }
        status_=BCryptCreateHash(
            algorithm_,&hash_,object_.data(),objectBytes,
            nullptr,0,0);
    }

    ~PickerTraceSha256Accumulator() noexcept {
        if(hash_) BCryptDestroyHash(hash_);
        if(algorithm_) BCryptCloseAlgorithmProvider(algorithm_,0);
    }

    bool ready() const noexcept {
        return algorithm_ && hash_ && BCRYPT_SUCCESS(status_);
    }

    bool update(const void* bytes,size_t size) noexcept {
        if(!ready() || (!bytes && size)) return false;
        const unsigned char* at=static_cast<const unsigned char*>(bytes);
        while(size){
            const ULONG chunk=size>(std::numeric_limits<ULONG>::max)()
                ? (std::numeric_limits<ULONG>::max)()
                : static_cast<ULONG>(size);
            status_=BCryptHashData(hash_,const_cast<PUCHAR>(at),chunk,0);
            if(!BCRYPT_SUCCESS(status_)) return false;
            at+=chunk;
            size-=chunk;
        }
        return true;
    }

    bool finish(std::array<unsigned char,32>& output) noexcept {
        if(!ready()) return false;
        status_=BCryptFinishHash(
            hash_,output.data(),static_cast<ULONG>(output.size()),0);
        return BCRYPT_SUCCESS(status_);
    }

    LONG status() const noexcept { return static_cast<LONG>(status_); }

private:
    BCRYPT_ALG_HANDLE algorithm_=nullptr;
    BCRYPT_HASH_HANDLE hash_=nullptr;
    std::vector<unsigned char> object_;
    NTSTATUS status_=static_cast<NTSTATUS>(0xc0000001L);
};

const char* PickerTraceDigestStatusName(
        PickerTraceDigestStatus value) noexcept {
    switch(value){
    case PickerTraceDigestStatus::Available: return "available";
    case PickerTraceDigestStatus::PathUnavailable: return "path_unavailable";
    case PickerTraceDigestStatus::NormalizeFailed: return "normalize_failed";
    case PickerTraceDigestStatus::OpenFailed: return "open_failed";
    case PickerTraceDigestStatus::MetadataFailed: return "metadata_failed";
    case PickerTraceDigestStatus::ReadFailed: return "read_failed";
    case PickerTraceDigestStatus::CryptoFailed: return "crypto_failed";
    }
    return "unknown";
}

PickerTraceDigest PickerTraceSha256Bytes(
        const void* bytes,size_t size) noexcept {
    PickerTraceDigest result;
    result.status=PickerTraceDigestStatus::CryptoFailed;
    if(!bytes && size){
        result.win32Error=ERROR_INVALID_PARAMETER;
        return result;
    }
    try {
        PickerTraceSha256Accumulator hash;
        if(!hash.ready() || !hash.update(bytes,size) ||
           !hash.finish(result.bytes)){
            result.cryptoStatus=hash.status();
            return result;
        }
        result.status=PickerTraceDigestStatus::Available;
        result.cryptoStatus=0;
        result.available=true;
        return result;
    } catch(...) {
        return result;
    }
}

static void PickerTraceCloseProvenanceHandle(
        const PickerTraceProvenanceOps& ops,HANDLE handle) noexcept {
    if(!handle || handle==INVALID_HANDLE_VALUE || !ops.closeHandle) return;
    try { (void)ops.closeHandle(handle); }
    catch(...) {}
}

PickerTraceDigest PickerTraceSha256File(
        const std::wstring& path,const PickerTraceProvenanceOps& ops) noexcept {
    PickerTraceDigest result;
    result.status=PickerTraceDigestStatus::OpenFailed;
    if(path.empty() || !ops.openRead || !ops.readFile || !ops.closeHandle){
        result.win32Error=ERROR_INVALID_PARAMETER;
        return result;
    }
    HANDLE handle=INVALID_HANDLE_VALUE;
    try {
        SetLastError(ERROR_SUCCESS);
        handle=ops.openRead(path);
        if(!handle || handle==INVALID_HANDLE_VALUE){
            result.win32Error=GetLastError();
            if(result.win32Error==ERROR_SUCCESS)
                result.win32Error=ERROR_OPEN_FAILED;
            return result;
        }
        PickerTraceSha256Accumulator hash;
        if(!hash.ready()){
            result.status=PickerTraceDigestStatus::CryptoFailed;
            result.cryptoStatus=hash.status();
            PickerTraceCloseProvenanceHandle(ops,handle);
            return result;
        }
        std::array<unsigned char,64*1024> buffer{};
        for(;;){
            DWORD read=0;
            SetLastError(ERROR_SUCCESS);
            if(!ops.readFile(handle,buffer.data(),
                             static_cast<DWORD>(buffer.size()),read)){
                result.status=PickerTraceDigestStatus::ReadFailed;
                result.win32Error=GetLastError();
                if(result.win32Error==ERROR_SUCCESS)
                    result.win32Error=ERROR_READ_FAULT;
                PickerTraceCloseProvenanceHandle(ops,handle);
                return result;
            }
            if(read>buffer.size()){
                result.status=PickerTraceDigestStatus::ReadFailed;
                result.win32Error=ERROR_INVALID_DATA;
                PickerTraceCloseProvenanceHandle(ops,handle);
                return result;
            }
            if(read==0) break;
            if(!hash.update(buffer.data(),read)){
                result.status=PickerTraceDigestStatus::CryptoFailed;
                result.cryptoStatus=hash.status();
                PickerTraceCloseProvenanceHandle(ops,handle);
                return result;
            }
        }
        if(!hash.finish(result.bytes)){
            result.status=PickerTraceDigestStatus::CryptoFailed;
            result.cryptoStatus=hash.status();
            PickerTraceCloseProvenanceHandle(ops,handle);
            return result;
        }
        PickerTraceCloseProvenanceHandle(ops,handle);
        result.status=PickerTraceDigestStatus::Available;
        result.available=true;
        return result;
    } catch(...) {
        PickerTraceCloseProvenanceHandle(ops,handle);
        if(result.status==PickerTraceDigestStatus::OpenFailed)
            result.win32Error=ERROR_UNHANDLED_EXCEPTION;
        else
            result.status=PickerTraceDigestStatus::ReadFailed;
        return result;
    }
}

std::string PickerTraceDigestHex(const PickerTraceDigest& digest) noexcept {
    if(!digest.available) return std::string();
    try {
        static const char digits[]="0123456789abcdef";
        std::string result(digest.bytes.size()*2,'0');
        for(size_t index=0;index<digest.bytes.size();++index){
            result[index*2]=digits[(digest.bytes[index]>>4)&0x0f];
            result[index*2+1]=digits[digest.bytes[index]&0x0f];
        }
        return result;
    } catch(...) {
        return std::string();
    }
}

static bool PickerTraceWidePrefixEqualInsensitive(
        const std::wstring& value,const wchar_t* prefix) noexcept {
    if(!prefix) return false;
    const size_t count=wcslen(prefix);
    if(value.size()<count) return false;
    for(size_t index=0;index<count;++index){
        wchar_t left=value[index];
        wchar_t right=prefix[index];
        if(left>=L'A' && left<=L'Z') left+=L'a'-L'A';
        if(right>=L'A' && right<=L'Z') right+=L'a'-L'A';
        if(left!=right) return false;
    }
    return true;
}

bool NormalizePickerTraceModulePath(
        const std::wstring& input,std::string& normalizedUtf8) noexcept {
    normalizedUtf8.clear();
    try {
        if(input.empty() || input.find(L'\0')!=std::wstring::npos ||
           input.size()>static_cast<size_t>((std::numeric_limits<int>::max)()))
            return false;
        std::wstring normalized=input;
        if(PickerTraceWidePrefixEqualInsensitive(normalized,L"\\\\?\\UNC\\"))
            normalized=L"\\\\"+normalized.substr(8);
        else if(PickerTraceWidePrefixEqualInsensitive(normalized,L"\\\\?\\"))
            normalized.erase(0,4);
        std::replace(normalized.begin(),normalized.end(),L'/',L'\\');
        if(!PickerTraceIsAbsoluteDirectoryPath(normalized)) return false;
        const int count=static_cast<int>(normalized.size());
        const int required=LCMapStringEx(
            LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,
            normalized.data(),count,nullptr,0,nullptr,nullptr,0);
        if(required!=count) return false;
        std::wstring lower(static_cast<size_t>(required),L'\0');
        if(LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,
                         normalized.data(),count,&lower[0],required,
                         nullptr,nullptr,0)!=required) return false;
        return PickerTraceWideToUtf8(
            lower.data(),static_cast<int>(lower.size()),normalizedUtf8);
    } catch(...) {
        normalizedUtf8.clear();
        return false;
    }
}

static bool PickerTraceReadExact(
        const PickerTraceProvenanceOps& ops,HANDLE handle,
        void* output,DWORD size,DWORD& error) noexcept {
    error=ERROR_SUCCESS;
    unsigned char* destination=static_cast<unsigned char*>(output);
    DWORD total=0;
    try {
        while(total<size){
            DWORD read=0;
            SetLastError(ERROR_SUCCESS);
            if(!ops.readFile(handle,destination+total,size-total,read)){
                error=GetLastError();
                if(error==ERROR_SUCCESS) error=ERROR_READ_FAULT;
                return false;
            }
            if(read==0 || read>size-total){
                error=read==0 ? ERROR_HANDLE_EOF : ERROR_INVALID_DATA;
                return false;
            }
            total+=read;
        }
        return true;
    } catch(...) {
        error=ERROR_UNHANDLED_EXCEPTION;
        return false;
    }
}

bool ReadPickerTracePeTimestamp(
        const std::wstring& path,const PickerTraceProvenanceOps& ops,
        uint32_t& timestamp,DWORD& win32Error) noexcept {
    timestamp=0;
    win32Error=ERROR_SUCCESS;
    if(path.empty() || !ops.openRead || !ops.readFile ||
       !ops.seekFile || !ops.closeHandle){
        win32Error=ERROR_INVALID_PARAMETER;
        return false;
    }
    HANDLE handle=INVALID_HANDLE_VALUE;
    try {
        SetLastError(ERROR_SUCCESS);
        handle=ops.openRead(path);
        if(!handle || handle==INVALID_HANDLE_VALUE){
            win32Error=GetLastError();
            if(win32Error==ERROR_SUCCESS) win32Error=ERROR_OPEN_FAILED;
            return false;
        }
        IMAGE_DOS_HEADER dos{};
        if(!PickerTraceReadExact(ops,handle,&dos,sizeof(dos),win32Error) ||
           dos.e_magic!=IMAGE_DOS_SIGNATURE ||
           dos.e_lfanew<static_cast<LONG>(sizeof(dos)) ||
           dos.e_lfanew>64L*1024L*1024L){
            if(win32Error==ERROR_SUCCESS) win32Error=ERROR_BAD_EXE_FORMAT;
            PickerTraceCloseProvenanceHandle(ops,handle);
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        if(!ops.seekFile(handle,dos.e_lfanew,FILE_BEGIN)){
            win32Error=GetLastError();
            if(win32Error==ERROR_SUCCESS) win32Error=ERROR_SEEK;
            PickerTraceCloseProvenanceHandle(ops,handle);
            return false;
        }
        DWORD signature=0;
        IMAGE_FILE_HEADER header{};
        if(!PickerTraceReadExact(
                ops,handle,&signature,sizeof(signature),win32Error) ||
           signature!=IMAGE_NT_SIGNATURE ||
           !PickerTraceReadExact(
                ops,handle,&header,sizeof(header),win32Error)){
            if(win32Error==ERROR_SUCCESS) win32Error=ERROR_BAD_EXE_FORMAT;
            PickerTraceCloseProvenanceHandle(ops,handle);
            return false;
        }
        PickerTraceCloseProvenanceHandle(ops,handle);
        timestamp=header.TimeDateStamp;
        return true;
    } catch(...) {
        PickerTraceCloseProvenanceHandle(ops,handle);
        win32Error=ERROR_UNHANDLED_EXCEPTION;
        return false;
    }
}

bool SerializePickerTraceLine(
        const PickerTraceEnvelope& envelope,const PickerTraceStartEvent& event,
        const wchar_t* appVersion,std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"trace.start",[&](auto& json){
        std::string version;
        std::string basename;
        const wchar_t* safeVersion=appVersion ? appVersion : L"";
        const size_t versionLength=wcslen(safeVersion);
        if(versionLength>static_cast<size_t>((std::numeric_limits<int>::max)()) ||
           !PickerTraceWideToUtf8(safeVersion,
                                  static_cast<int>(versionLength),version))
            return false;
        if(event.moduleBasename.available() &&
           !PickerTraceWideToUtf8(event.moduleBasename.data(),
                                  event.moduleBasename.length(),basename))
            return false;
        bool ok=json.string("app_version",version) &&
            json.string("image_digest_status",
                        PickerTraceDigestStatusName(event.imageDigest.status)) &&
            json.boolean("image_digest_available",event.imageDigest.available);
        if(ok && event.imageDigest.available)
            ok=json.string("image_digest",PickerTraceDigestHex(event.imageDigest));
        ok=ok && json.hex32("image_digest_win32_error",event.imageDigest.win32Error) &&
            json.hex32("image_digest_crypto_status",
                       static_cast<uint32_t>(event.imageDigest.cryptoStatus)) &&
            json.string("path_digest_status",
                        PickerTraceDigestStatusName(event.pathDigest.status)) &&
            json.boolean("path_digest_available",event.pathDigest.available);
        if(ok && event.pathDigest.available)
            ok=json.string("path_digest",PickerTraceDigestHex(event.pathDigest));
        ok=ok && json.hex32("path_digest_win32_error",event.pathDigest.win32Error) &&
            json.hex32("path_digest_crypto_status",
                       static_cast<uint32_t>(event.pathDigest.cryptoStatus)) &&
            json.boolean("module_basename_available",
                         event.moduleBasename.available());
        if(ok && event.moduleBasename.available())
            ok=json.string("module_basename",basename);
        return ok &&
            json.unsignedNumber("file_size",event.fileSize) &&
            json.unsignedNumber("last_write_100ns",event.lastWrite100ns) &&
            json.hex32("pe_timestamp",event.peTimestamp) &&
            json.unsignedNumber("pid",event.pid) &&
            json.unsignedNumber("tid",event.tid) &&
            json.unsignedNumber("process_session_id",event.processSessionId) &&
            json.unsignedNumber("integrity_rid",event.integrityRid) &&
            json.unsignedNumber("windows_build",event.windowsBuild) &&
            json.boolean("file_metadata_available",event.fileMetadataAvailable) &&
            json.boolean("pe_timestamp_available",event.peTimestampAvailable) &&
            json.boolean("process_session_available",event.processSessionAvailable) &&
            json.boolean("integrity_available",event.integrityAvailable) &&
            json.boolean("elevated",event.elevated);
    },output);
}

void PickerTraceWriter::emitStart(
        const PickerTraceStartEvent& event,
        const wchar_t* appVersion) noexcept {
    if(!active()) return;
    try {
        PickerTraceEnvelope candidate=envelope_;
        if(candidate.seq==(std::numeric_limits<uint64_t>::max)()){
            active_=false;
            return;
        }
        const uint64_t now=ops_.monotonicMs();
        uint64_t elapsed=now>=startMs_ ? now-startMs_ : 0;
        if(elapsed<lastElapsedMs_) elapsed=lastElapsedMs_;
        candidate.seq++;
        candidate.ms=elapsed;
        std::string line;
        if(!SerializePickerTraceLine(candidate,event,appVersion,line)){
            active_=false;
            return;
        }
        if(eventsWritten_>=limits_.maxEvents-1){
            truncate();
            return;
        }
        PickerTraceEnvelope reserveEnvelope=candidate;
        if(reserveEnvelope.seq==(std::numeric_limits<uint64_t>::max)()){
            active_=false;
            return;
        }
        ++reserveEnvelope.seq;
        reserveEnvelope.ms=(std::numeric_limits<uint64_t>::max)();
        std::string reserve;
        if(!SerializePickerTraceTruncatedLine(reserveEnvelope,reserve)){
            active_=false;
            return;
        }
        const uint64_t remaining=limits_.maxBytes-bytesWritten_;
        if(line.size()>remaining || reserve.size()>remaining-line.size()){
            truncate();
            return;
        }
        if(!writeWhole(line)) return;
        envelope_=candidate;
        lastElapsedMs_=elapsed;
        bytesWritten_+=static_cast<uint64_t>(line.size());
        ++eventsWritten_;
    } catch(...) {
        active_=false;
    }
}

PickerTraceProvenanceOps DefaultPickerTraceProvenanceOps() noexcept {
    PickerTraceProvenanceOps ops;
    try {
        ops.modulePath=[](std::wstring& output) noexcept {
            output.clear();
            try {
                std::vector<wchar_t> buffer(1024,L'\0');
                while(buffer.size()<=32768){
                    SetLastError(ERROR_SUCCESS);
                    const DWORD written=GetModuleFileNameW(
                        nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
                    if(written==0) return false;
                    if(written<buffer.size()-1 ||
                       (written<buffer.size() &&
                        GetLastError()!=ERROR_INSUFFICIENT_BUFFER)){
                        output.assign(buffer.data(),written);
                        return true;
                    }
                    buffer.resize((std::min)(buffer.size()*2,
                                             static_cast<size_t>(32769)),L'\0');
                }
                return false;
            } catch(...) { output.clear(); return false; }
        };
        ops.openRead=[](const std::wstring& path) noexcept {
            return CreateFileW(path.c_str(),GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        };
        ops.readFile=[](HANDLE handle,void* output,DWORD size,
                        DWORD& read) noexcept {
            read=0;
            return ReadFile(handle,output,size,&read,nullptr);
        };
        ops.seekFile=[](HANDLE handle,LONGLONG distance,DWORD origin) noexcept {
            LARGE_INTEGER move{};
            move.QuadPart=distance;
            return SetFilePointerEx(handle,move,nullptr,origin);
        };
        ops.fileMetadata=[](HANDLE handle,uint64_t& size,
                            uint64_t& lastWrite) noexcept {
            LARGE_INTEGER fileSize{};
            FILETIME write{};
            if(!GetFileSizeEx(handle,&fileSize) || fileSize.QuadPart<0 ||
               !GetFileTime(handle,nullptr,nullptr,&write)) return false;
            size=static_cast<uint64_t>(fileSize.QuadPart);
            lastWrite=PickerTraceFileTimeValue(write);
            return true;
        };
        ops.closeHandle=[](HANDLE handle) noexcept { return CloseHandle(handle); };
        ops.randomBytes=[](void* output,size_t size) noexcept {
            if(!output || size>static_cast<size_t>((std::numeric_limits<ULONG>::max)()))
                return false;
            return BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr,static_cast<PUCHAR>(output),static_cast<ULONG>(size),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        };
        ops.processSessionId=[](DWORD& output) noexcept {
            return ProcessIdToSessionId(GetCurrentProcessId(),&output)!=FALSE;
        };
        ops.processIntegrity=[](DWORD& rid,bool& elevated) noexcept {
            rid=0;
            elevated=false;
            HANDLE token=nullptr;
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))
                return false;
            try {
                DWORD required=0;
                GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&required);
                if(required==0){ CloseHandle(token); return false; }
                std::vector<unsigned char> buffer(required);
                if(!GetTokenInformation(token,TokenIntegrityLevel,
                        buffer.data(),required,&required)){
                    CloseHandle(token);
                    return false;
                }
                const TOKEN_MANDATORY_LABEL* label=
                    reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
                if(!label->Label.Sid || !IsValidSid(label->Label.Sid)){
                    CloseHandle(token);
                    return false;
                }
                const UCHAR count=*GetSidSubAuthorityCount(label->Label.Sid);
                if(count==0){ CloseHandle(token); return false; }
                rid=*GetSidSubAuthority(label->Label.Sid,count-1);
                TOKEN_ELEVATION elevation{};
                DWORD copied=0;
                if(!GetTokenInformation(token,TokenElevation,&elevation,
                                        sizeof(elevation),&copied)){
                    CloseHandle(token);
                    return false;
                }
                elevated=elevation.TokenIsElevated!=0;
                CloseHandle(token);
                return true;
            } catch(...) {
                CloseHandle(token);
                rid=0;
                elevated=false;
                return false;
            }
        };
        ops.processId=[]() noexcept { return GetCurrentProcessId(); };
        ops.threadId=[]() noexcept { return GetCurrentThreadId(); };
        ops.windowsBuild=[]() noexcept {
            using RtlGetVersionFunction=LONG (WINAPI*)(OSVERSIONINFOW*);
            HMODULE module=GetModuleHandleW(L"ntdll.dll");
            if(!module) return 0UL;
            const auto function=reinterpret_cast<RtlGetVersionFunction>(
                GetProcAddress(module,"RtlGetVersion"));
            if(!function) return 0UL;
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize=sizeof(version);
            return function(&version)==0 ? version.dwBuildNumber : 0UL;
        };
    } catch(...) {
        return PickerTraceProvenanceOps{};
    }
    return ops;
}

PickerTraceRuntimeOps DefaultPickerTraceRuntimeOps() noexcept {
    PickerTraceRuntimeOps result;
    try {
        result.storage=DefaultPickerTraceStorageOps();
        result.provenance=DefaultPickerTraceProvenanceOps();
    } catch(...) {
        return PickerTraceRuntimeOps{};
    }
    return result;
}

static PickerTraceStartEvent PickerTraceCollectStartEvent(
        const PickerTraceProvenanceOps& ops) noexcept {
    PickerTraceStartEvent event;
    try { if(ops.processId) event.pid=ops.processId(); } catch(...) {}
    try { if(ops.threadId) event.tid=ops.threadId(); } catch(...) {}
    try {
        if(ops.windowsBuild) event.windowsBuild=ops.windowsBuild();
    } catch(...) {}
    try {
        if(ops.processSessionId)
            event.processSessionAvailable=
                ops.processSessionId(event.processSessionId);
    } catch(...) {
        event.processSessionAvailable=false;
    }
    try {
        if(ops.processIntegrity)
            event.integrityAvailable=
                ops.processIntegrity(event.integrityRid,event.elevated);
    } catch(...) {
        event.integrityAvailable=false;
    }

    std::wstring path;
    try {
        if(!ops.modulePath || !ops.modulePath(path) || path.empty()) return event;
    } catch(...) {
        return event;
    }
    const size_t slash=path.find_last_of(L"\\/");
    const size_t basenameBegin=slash==std::wstring::npos ? 0 : slash+1;
    const size_t basenameLength=path.size()-basenameBegin;
    if(basenameLength<=260)
        event.moduleBasename=MakePickerTraceSafeImageBasename(
            path.data()+basenameBegin,static_cast<int>(basenameLength));

    std::string normalized;
    if(NormalizePickerTraceModulePath(path,normalized))
        event.pathDigest=PickerTraceSha256Bytes(
            normalized.data(),normalized.size());
    else
        event.pathDigest.status=PickerTraceDigestStatus::NormalizeFailed;
    event.imageDigest=PickerTraceSha256File(path,ops);

    if(ops.openRead && ops.fileMetadata && ops.closeHandle){
        HANDLE handle=INVALID_HANDLE_VALUE;
        try {
            handle=ops.openRead(path);
            if(handle && handle!=INVALID_HANDLE_VALUE){
                event.fileMetadataAvailable=ops.fileMetadata(
                    handle,event.fileSize,event.lastWrite100ns);
                PickerTraceCloseProvenanceHandle(ops,handle);
            }
        } catch(...) {
            PickerTraceCloseProvenanceHandle(ops,handle);
            event.fileMetadataAvailable=false;
        }
    }
    DWORD peError=ERROR_SUCCESS;
    event.peTimestampAvailable=ReadPickerTracePeTimestamp(
        path,ops,event.peTimestamp,peError);
    return event;
}

struct PickerTraceSession::Impl {
    Impl(PickerTraceRuntimeOps runtimeOps,PickerTraceLimits traceLimits)
        :ops(std::move(runtimeOps)),limits(traceLimits){}
    PickerTraceRuntimeOps ops;
    PickerTraceLimits limits;
    std::unique_ptr<PickerTraceWriter> writer;
    std::wstring path;
    uint64_t correlation=0;
    bool requested=false;
    bool attempted=false;
};

PickerTraceSession::PickerTraceSession() noexcept=default;

PickerTraceSession::PickerTraceSession(
        PickerTraceRuntimeOps ops,PickerTraceLimits limits) noexcept {
    try {
        impl_.reset(new(std::nothrow) Impl(std::move(ops),limits));
    } catch(...) {
        impl_.reset();
    }
}

PickerTraceSession::~PickerTraceSession() noexcept {
    close();
}

bool PickerTraceSession::start(
        bool requestedValue,const wchar_t* appVersion) noexcept {
    if(!requestedValue) return false;
    if(!impl_){
        try {
            impl_.reset(new(std::nothrow) Impl(
                DefaultPickerTraceRuntimeOps(),PickerTraceLimits()));
        } catch(...) {
            impl_.reset();
        }
    }
    if(!impl_) return false;
    if(impl_->attempted) return active();
    impl_->requested=true;
    impl_->attempted=true;
    PickerTraceOpenedFile opened;
    bool openedOwned=false;
    try {
        const PickerTraceProvenanceOps& provenance=impl_->ops.provenance;
        const PickerTraceStorageOps& storage=impl_->ops.storage;
        if(!provenance.processId || !provenance.randomBytes ||
           !storage.writeFile || !storage.flushFile ||
           !storage.closeHandle || !storage.monotonicMs) return false;
        const DWORD pid=provenance.processId();
        if(pid==0 || !OpenPickerTraceStorage(storage,pid,opened)) return false;
        openedOwned=true;

        std::array<unsigned char,16> session{};
        if(!provenance.randomBytes(session.data(),session.size())){
            try { (void)storage.closeHandle(opened.handle); } catch(...) {}
            return false;
        }

        PickerTraceSinkOps sink;
        const HANDLE handle=opened.handle;
        sink.write=[write=storage.writeFile,handle](
                const void* bytes,size_t size)->size_t {
            if(size>(std::numeric_limits<DWORD>::max)()) return 0;
            DWORD written=0;
            if(!write(handle,bytes,static_cast<DWORD>(size),written)) return 0;
            return written;
        };
        sink.flush=[flush=storage.flushFile,handle](){
            return flush(handle)!=FALSE;
        };
        sink.close=[closeHandle=storage.closeHandle,handle](){
            return closeHandle(handle)!=FALSE;
        };
        sink.monotonicMs=storage.monotonicMs;

        std::unique_ptr<PickerTraceWriter> writer(
            new(std::nothrow) PickerTraceWriter(
                std::move(sink),impl_->limits,session));
        if(!writer){
            try { (void)storage.closeHandle(opened.handle); } catch(...) {}
            return false;
        }
        openedOwned=false;
        if(!writer->active()) return false;
        const PickerTraceStartEvent startEvent=
            PickerTraceCollectStartEvent(provenance);
        writer->emitStart(startEvent,appVersion);
        if(!writer->active()) return false;
        impl_->path=opened.path;
        impl_->writer=std::move(writer);
        return true;
    } catch(...) {
        if(openedOwned && opened.handle && opened.handle!=INVALID_HANDLE_VALUE){
            try { (void)impl_->ops.storage.closeHandle(opened.handle); }
            catch(...) {}
        }
        return false;
    }
}

bool PickerTraceSession::active() const noexcept {
    return impl_ && impl_->writer && impl_->writer->active();
}

bool PickerTraceSession::requested() const noexcept {
    return impl_ && impl_->requested;
}

uint64_t PickerTraceSession::nextCorrelationId() noexcept {
    if(!active() || impl_->correlation==
       (std::numeric_limits<uint64_t>::max)()) return 0;
    return ++impl_->correlation;
}

void PickerTraceSession::flushBoundary() noexcept {
    if(impl_ && impl_->writer) impl_->writer->flushBoundary();
}

void PickerTraceSession::close() noexcept {
    if(impl_ && impl_->writer) impl_->writer->close();
}

#define VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(EventType) \
    void PickerTraceSession::emit(const EventType& event) noexcept { \
        if(impl_ && impl_->writer) impl_->writer->emit(event); \
    }

VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceCaptureEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceOpenEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceEnumBeginEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceEnumWindowEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceEnumEndEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceMouseDownEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceActivationRequestEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceActivationResultEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceMoveBeginEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceMoveBeginExceptionEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceEffectEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceApiResultEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceTerminalizationAttemptEvent)
VDE_DEFINE_PICKER_TRACE_SESSION_EMIT(PickerTraceTransitionTerminalEvent)

#undef VDE_DEFINE_PICKER_TRACE_SESSION_EMIT

std::wstring PickerTraceSession::pathForLocalInspection() const {
    try { return impl_ ? impl_->path : std::wstring(); }
    catch(...) { return std::wstring(); }
}
