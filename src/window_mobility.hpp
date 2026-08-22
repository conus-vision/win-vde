#pragma once

#include "window_identity.hpp"
#include "str_util.hpp"

#include <cstdint>
#include <vector>

enum class MobilityEvidence : uint8_t {
    Unknown,
    Negative,
    Positive
};

enum class TargetDesktopRoute : uint8_t {
    Exact,
    GloballyVisible,
    Indeterminate
};

enum class TargetMobility : uint8_t {
    Movable,
    ViewPinned,
    AppPinned,
    Immovable,
    Indeterminate
};

enum class TargetMoveDisposition : uint8_t {
    Physical,
    VisualOnly,
    Reject
};

struct TargetMobilityEvidence {
    TargetDesktopRoute desktopRoute=TargetDesktopRoute::Indeterminate;
    MobilityEvidence viewPinned=MobilityEvidence::Unknown;
    MobilityEvidence appPinned=MobilityEvidence::Unknown;
    MobilityEvidence canMove=MobilityEvidence::Unknown;
};

struct TargetMobilityDecision {
    TargetMobility mobility=TargetMobility::Indeterminate;
    TargetMoveDisposition disposition=TargetMoveDisposition::Reject;
};

inline MobilityEvidence MobilityEvidenceFromBoolean(
        HRESULT result,BOOL value) noexcept {
    if(FAILED(result)) return MobilityEvidence::Unknown;
    return value ? MobilityEvidence::Positive : MobilityEvidence::Negative;
}

inline TargetDesktopRoute DecideTargetDesktopRoute(
        HRESULT desktopRead,bool desktopNonzero,bool desktopInSnapshot,
        HRESULT membershipRead,bool onCurrentDesktop) noexcept {
    if(SUCCEEDED(desktopRead) && desktopNonzero && desktopInSnapshot)
        return TargetDesktopRoute::Exact;
    if(SUCCEEDED(desktopRead) &&
       SUCCEEDED(membershipRead) && onCurrentDesktop)
        return TargetDesktopRoute::GloballyVisible;
    return TargetDesktopRoute::Indeterminate;
}

inline TargetMobilityDecision DecideTargetMobility(
        const TargetMobilityEvidence& evidence) noexcept {
    TargetMobilityDecision result;
    if(evidence.viewPinned==MobilityEvidence::Positive){
        result.mobility=TargetMobility::ViewPinned;
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.appPinned==MobilityEvidence::Positive){
        result.mobility=TargetMobility::AppPinned;
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.desktopRoute==TargetDesktopRoute::GloballyVisible){
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.desktopRoute!=TargetDesktopRoute::Exact ||
       evidence.viewPinned==MobilityEvidence::Unknown ||
       evidence.appPinned==MobilityEvidence::Unknown ||
       evidence.canMove==MobilityEvidence::Unknown)
        return result;
    if(evidence.canMove==MobilityEvidence::Negative){
        result.mobility=TargetMobility::Immovable;
        return result;
    }
    if(evidence.viewPinned==MobilityEvidence::Negative &&
       evidence.appPinned==MobilityEvidence::Negative &&
       evidence.canMove==MobilityEvidence::Positive){
        result.mobility=TargetMobility::Movable;
        result.disposition=TargetMoveDisposition::Physical;
    }
    return result;
}
