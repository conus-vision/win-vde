#include "picker_trace.hpp"

#include <cstdio>
#include <limits>

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
    return PickerTraceSerialize(envelope,"capture.title",[&](auto& json){
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
        return json.string("api",PickerTraceApiKindName(event.api)) &&
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
    },output);
}

bool SerializePickerTraceLine(
        const PickerTraceEnvelope& envelope,
        const PickerTraceTerminalizationAttemptEvent& event,
        std::string& output) noexcept {
    return PickerTraceSerialize(envelope,"terminalization.attempt",[&](auto& json){
        return json.unsignedNumber("generation",event.generation) &&
            json.unsignedNumber("attempt",event.attempt) &&
            json.string("reason",PickerTraceTerminalizationReasonName(event.reason)) &&
            json.string("incoming_delivery",PickerTraceDeliveryRouteName(event.incomingDelivery)) &&
            json.string("retry_delivery",PickerTraceDeliveryRouteName(event.retryDelivery)) &&
            json.boolean("incoming_delivery_available",event.incomingDeliveryAvailable) &&
            json.boolean("retry_delivery_available",event.retryDeliveryAvailable) &&
            json.boolean("terminal_acknowledged",event.terminalAcknowledged) &&
            json.boolean("pending_effect_none",event.pendingEffectNone) &&
            json.boolean("runtime_key_present",event.runtimeKeyPresent) &&
            json.string("first_release_action",PickerTraceTerminalGuardReleaseActionName(event.firstReleaseAction)) &&
            json.string("retry_release_action",PickerTraceTerminalGuardReleaseActionName(event.retryReleaseAction)) &&
            json.string("release_exception_stage",PickerTraceReservationExceptionStageName(event.releaseExceptionStage)) &&
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
