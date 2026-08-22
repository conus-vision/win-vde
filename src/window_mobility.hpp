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

template<class Desktops,class GuidOf>
inline bool ConcreteDesktopExists(
        const GUID& observed,const Desktops& desktops,
        GuidOf&& guidOf) noexcept {
    if(GuidIsZero(observed)) return false;
    try {
        for(const auto& desktop : desktops){
            const GUID& candidate=guidOf(desktop);
            if(!GuidIsZero(candidate) &&
               GuidEq(candidate,observed))
                return true;
        }
    } catch(...) {
        return false;
    }
    return false;
}

template<class Issue>
inline HRESULT ExecutePhysicalTargetMoveDecision(
        WindowIdentityRecapture identity,TargetDesktopRoute route,
        const TargetMobilityDecision& mobility,
        bool sourceExists,bool destinationExists,bool sameSource,
        bool& invoked,Issue&& issue) noexcept {
    invoked=false;
    if(identity!=WindowIdentityRecapture::Match ||
       route!=TargetDesktopRoute::Exact ||
       mobility.mobility!=TargetMobility::Movable ||
       mobility.disposition!=TargetMoveDisposition::Physical ||
       !sourceExists || !destinationExists || sameSource)
        return E_ACCESSDENIED;
    try {
        invoked=true;
        return issue();
    } catch(...) {
        return E_FAIL;
    }
}
