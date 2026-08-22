#pragma once

#include "str_util.hpp"
#include "move_queue.hpp"
#include "window_identity.hpp"
#include "window_mobility.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

inline const wchar_t* FooterRepoLabel() noexcept {
    return L"Virtual Desktop Extension";
}

inline const wchar_t* FooterMiddle() noexcept {
    return L" for Windows 11 by Volodymyr Moskvin (c) 2026 ";
}

inline const wchar_t* FooterConusLabel() noexcept {
    return L"Conus Vision";
}

inline const wchar_t* FooterRepoUrl() noexcept {
    return L"https://github.com/conus-vision/win-vde";
}

inline const wchar_t* FooterConusUrl() noexcept {
    return L"https://conus.vision";
}

inline std::wstring BuildFooterText(){
    return std::wstring(FooterRepoLabel())+
           FooterMiddle()+FooterConusLabel();
}

enum class PickerFooterLink {
    None,
    Repository,
    ConusVision
};

inline const wchar_t* PickerFooterUrl(PickerFooterLink link) noexcept {
    if(link==PickerFooterLink::Repository) return FooterRepoUrl();
    if(link==PickerFooterLink::ConusVision) return FooterConusUrl();
    return nullptr;
}

inline int PickerSaturatingInt(long long value) noexcept {
    if(value>(std::numeric_limits<int>::max)())
        return (std::numeric_limits<int>::max)();
    if(value<(std::numeric_limits<int>::min)())
        return (std::numeric_limits<int>::min)();
    return static_cast<int>(value);
}

inline int PickerScaleForDpi(int value,int dpi) noexcept {
    const int effectiveDpi=dpi>0?dpi:96;
    long long scaled=static_cast<long long>(value)*effectiveDpi;
    scaled+=scaled>=0?48:-48;
    return PickerSaturatingInt(scaled/96);
}

inline SIZE PickerDesiredClientSize(
        size_t desktopCount,int tileWidth,int tileHeight,int padding,
        int searchHeight,int headerHeight,int footerHeight,
        int footerMinimumWidth) noexcept {
    const size_t boundedCount=std::min(
        desktopCount,static_cast<size_t>((std::numeric_limits<int>::max)()));
    const int count=static_cast<int>(boundedCount);
    const int columns=std::min(std::max(1,count),5);
    const int rows=count==0?0:
        count/columns+(count%columns==0?0:1);
    const long long naturalWidth=static_cast<long long>(padding)+
        static_cast<long long>(columns)*
            (static_cast<long long>(tileWidth)+padding);
    const long long width=std::max(
        naturalWidth,static_cast<long long>(footerMinimumWidth));
    const long long height=static_cast<long long>(searchHeight)+
        headerHeight+padding+
        static_cast<long long>(rows)*
            (static_cast<long long>(tileHeight)+padding)+footerHeight;
    SIZE result={PickerSaturatingInt(std::max(0LL,width)),
                 PickerSaturatingInt(std::max(0LL,height))};
    return result;
}

inline POINT PickerCenteredOrigin(const RECT& workArea,
                                  const SIZE& outerSize) noexcept {
    const long long width=static_cast<long long>(workArea.right)-
        workArea.left;
    const long long height=static_cast<long long>(workArea.bottom)-
        workArea.top;
    POINT origin;
    origin.x=PickerSaturatingInt(static_cast<long long>(workArea.left)+
        (width-outerSize.cx)/2);
    origin.y=PickerSaturatingInt(static_cast<long long>(workArea.top)+
        (height-outerSize.cy)/2);
    return origin;
}

inline bool PickerOuterSizeValid(const SIZE& outerSize) noexcept {
    return outerSize.cx>0 && outerSize.cy>0;
}

struct PickerFooterLayout {
    RECT footer={0,0,0,0};
    RECT repoLink={0,0,0,0};
    RECT conusLink={0,0,0,0};
    POINT repoText={0,0};
    POINT middleText={0,0};
    POINT conusText={0,0};
};

inline bool BuildPickerFooterLayout(
        int clientWidth,int clientHeight,int padding,int footerHeight,
        int linkHeight,int repoWidth,int middleWidth,
        int conusWidth,PickerFooterLayout& output) noexcept {
    if(clientWidth<=0 || clientHeight<=0 || padding<0 ||
       footerHeight<=0 || linkHeight<=0 ||
       repoWidth<=0 || middleWidth<0 || conusWidth<=0 ||
       footerHeight>clientHeight || linkHeight>footerHeight)
        return false;
    const long long total=static_cast<long long>(repoWidth)+
        middleWidth+conusWidth;
    const long long required=total+2LL*padding;
    if(total>(std::numeric_limits<int>::max)() ||
       required>clientWidth) return false;
    const long long centered=(static_cast<long long>(clientWidth)-total)/2;
    const int x=static_cast<int>(std::max(
        static_cast<long long>(padding),centered));
    const int y=clientHeight-footerHeight;
    const int middleX=x+repoWidth;
    const int conusX=middleX+middleWidth;
    const int endX=conusX+conusWidth;
    const int linkBottom=y+linkHeight;
    if(x<padding || endX>clientWidth-padding ||
       y<0 || linkBottom>clientHeight) return false;
    PickerFooterLayout staged;
    staged.footer={0,y,clientWidth,clientHeight};
    staged.repoLink={x,y,middleX,linkBottom};
    staged.conusLink={conusX,y,endX,linkBottom};
    staged.repoText={x,y};
    staged.middleText={middleX,y};
    staged.conusText={conusX,y};
    output=staged;
    return true;
}

struct PickerFooterPaintCache {
    std::wstring repo,middle,conus;
    PickerFooterLayout layout;

    void swap(PickerFooterPaintCache& other) noexcept {
        static_assert(noexcept(repo.swap(other.repo)),
                      "footer text publication must be noexcept");
        repo.swap(other.repo);
        middle.swap(other.middle);
        conus.swap(other.conus);
        const PickerFooterLayout prior=layout;
        layout=other.layout;
        other.layout=prior;
    }

    void clear() noexcept {
        repo.clear();
        middle.clear();
        conus.clear();
        layout=PickerFooterLayout{};
    }
};

template<class Row>
struct PickerPaintCacheState {
    std::vector<Row> hoverRows;
    std::wstring switchHeader,moveHeader;
    PickerFooterPaintCache footer;
    uint64_t generation=0;
    int hintWidth=0;
    RECT clearButton={0,0,0,0};

    void swap(PickerPaintCacheState& other) noexcept {
        static_assert(noexcept(hoverRows.swap(other.hoverRows)),
                      "picker rows publication must be noexcept");
        static_assert(noexcept(switchHeader.swap(other.switchHeader)),
                      "picker header publication must be noexcept");
        hoverRows.swap(other.hoverRows);
        switchHeader.swap(other.switchHeader);
        moveHeader.swap(other.moveHeader);
        footer.swap(other.footer);
        const uint64_t priorGeneration=generation;
        generation=other.generation;
        other.generation=priorGeneration;
        const int priorHintWidth=hintWidth;
        hintWidth=other.hintWidth;
        other.hintWidth=priorHintWidth;
        const RECT priorClearButton=clearButton;
        clearButton=other.clearButton;
        other.clearButton=priorClearButton;
    }

    void clear() noexcept {
        hoverRows.clear();
        switchHeader.clear();
        moveHeader.clear();
        footer.clear();
        generation=0;
        hintWidth=0;
        clearButton={0,0,0,0};
    }
};

struct PickerFooterActivation {
    PickerFooterLink link=PickerFooterLink::None;
    const wchar_t* url=nullptr;
    bool consumed=false;
};

inline bool PickerFooterOpenSucceeded(intptr_t result) noexcept;

template<class Open,class Notify>
inline bool DispatchPickerFooterActivation(
        const PickerFooterActivation& activation,
        Open&& open,Notify&& notify) noexcept {
    if(!activation.consumed ||
       activation.link==PickerFooterLink::None || !activation.url)
        return false;
    intptr_t result=0;
    try { result=open(activation.url); }
    catch(...) { result=0; }
    if(!PickerFooterOpenSucceeded(result)){
        try { notify(); }
        catch(...) {}
    }
    return true;
}

enum class PickerRowAdmission {
    Skip,
    DisplayOnly,
    Verified
};

enum class PickerDesktopTileRoute {
    Exact,
    CurrentDesktopFallback,
    GloballyVisibleCurrentDesktopFallback,
    Skip
};

inline TargetDesktopRoute TargetRouteFromPickerTileRoute(
        PickerDesktopTileRoute route) noexcept {
    switch(route){
    case PickerDesktopTileRoute::Exact:
        return TargetDesktopRoute::Exact;
    case PickerDesktopTileRoute::GloballyVisibleCurrentDesktopFallback:
        return TargetDesktopRoute::GloballyVisible;
    case PickerDesktopTileRoute::CurrentDesktopFallback:
    case PickerDesktopTileRoute::Skip:
        return TargetDesktopRoute::Indeterminate;
    }
    return TargetDesktopRoute::Indeterminate;
}

struct PickerRowActionSnapshot {
    int tileIndex=-1;
    size_t windowIndex=0;
    uintptr_t hwnd=0;
    GUID displayedDesktop={0};
    GUID observedDesktop={0};
    GUID baseDesktop={0};
    WindowIdentityKey identity;
    PickerRowAdmission admission=PickerRowAdmission::DisplayOnly;
    TargetDesktopRoute desktopRoute=
        TargetDesktopRoute::Indeterminate;
    TargetMobility mobility=TargetMobility::Indeterminate;
    bool visuallyAssigned=false;
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;
    uint64_t paintGeneration=0;
};

struct PickerRowHitSnapshot {
    RECT hitRect={0,0,0,0};
    RECT textRect={0,0,0,0};
    std::wstring fullTitle;
    std::string runtimeKey;
    bool truncated=false;
    PickerRowActionSnapshot action;
};

inline bool PickerRowPresentationCurrent(
        const PickerRowActionSnapshot& row,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    return row.hwnd!=0 && row.tileIndex>=0 &&
           !GuidIsZero(row.displayedDesktop) &&
           row.modelGeneration==modelGeneration &&
           row.rowLayoutEpoch==rowLayoutEpoch;
}

inline bool PickerRowActionableForDrag(
        const PickerRowActionSnapshot& row,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    return PickerRowPresentationCurrent(
           row,modelGeneration,rowLayoutEpoch) &&
           row.admission==PickerRowAdmission::Verified &&
           row.hwnd==row.identity.hwnd &&
           SameIdentity(row.identity,row.identity);
}

enum class PickerPointerTarget {
    None,
    Footer,
    ClearSearch,
    Search,
    Row,
    Tile
};

struct PickerPointerActivation {
    PickerPointerTarget target=PickerPointerTarget::None;
    PickerFooterActivation footer;
    int rowIndex=-1;
    int tileIndex=-1;
};

enum class PickerActionIntent {
    TileSwitch,
    ActivateExact,
    MoveAndFollow,
    RowMoveOnly,
    VisualAndFollow,
    VisualOnly
};

struct PickerActionRequest {
    PickerActionIntent intent=PickerActionIntent::TileSwitch;
    GUID destination={0};
    bool hasRow=false;
    PickerRowActionSnapshot row;
    WindowIdentityKey popupActiveTarget;
    bool ctrlAtDown=false;
    uint64_t activationId=0;
};

enum class PickerActionDispatchResult {
    Rejected,
    SwitchedOnly,
    ActivationStarted,
    TransitionStarted
};

struct PickerActionIntentDecision {
    bool accepted=false;
    PickerActionIntent intent=PickerActionIntent::TileSwitch;
};

inline bool PickerActionUsesPopupActiveTarget(
        PickerActionIntent intent) noexcept {
    return intent==PickerActionIntent::MoveAndFollow ||
           intent==PickerActionIntent::VisualAndFollow;
}

inline bool PickerActionIsRowMove(
        PickerActionIntent intent) noexcept {
    return intent==PickerActionIntent::RowMoveOnly ||
           intent==PickerActionIntent::VisualOnly;
}

inline PickerActionIntentDecision DecidePickerActionIntent(
        bool hasRow,bool rowDrop,bool ctrlAtDown,
        PickerRowAdmission admission,bool identityUpgraded,
        TargetMoveDisposition disposition) noexcept {
    PickerActionIntentDecision decision;
    if(rowDrop){
        if(!hasRow || admission!=PickerRowAdmission::Verified)
            return decision;
        if(disposition==TargetMoveDisposition::Physical){
            decision.accepted=true;
            decision.intent=PickerActionIntent::RowMoveOnly;
        } else if(disposition==TargetMoveDisposition::VisualOnly){
            decision.accepted=true;
            decision.intent=PickerActionIntent::VisualOnly;
        }
        return decision;
    }
    if(ctrlAtDown){
        if(disposition==TargetMoveDisposition::Physical){
            decision.accepted=true;
            decision.intent=PickerActionIntent::MoveAndFollow;
        } else if(disposition==TargetMoveDisposition::VisualOnly){
            decision.accepted=true;
            decision.intent=PickerActionIntent::VisualAndFollow;
        }
        return decision;
    }
    decision.accepted=true;
    decision.intent=hasRow && identityUpgraded
        ? PickerActionIntent::ActivateExact
        : PickerActionIntent::TileSwitch;
    return decision;
}

inline PickerPointerActivation ResolvePickerPointerActivation(
        const PickerFooterActivation& footer,bool clearSearchHit,
        bool searchHit,int rowIndex,int tileIndex) noexcept {
    PickerPointerActivation activation;
    if(footer.consumed){
        activation.target=PickerPointerTarget::Footer;
        activation.footer=footer;
    } else if(clearSearchHit){
        activation.target=PickerPointerTarget::ClearSearch;
    } else if(searchHit){
        activation.target=PickerPointerTarget::Search;
    } else if(rowIndex>=0 && tileIndex>=0){
        activation.target=PickerPointerTarget::Row;
        activation.rowIndex=rowIndex;
        activation.tileIndex=tileIndex;
    } else if(tileIndex>=0){
        activation.target=PickerPointerTarget::Tile;
        activation.tileIndex=tileIndex;
    }
    return activation;
}

template<class OnFooter,class OnClearSearch,class OnSearch,
         class OnRow,class OnTile>
inline bool DispatchPickerPointerActivation(
        const PickerPointerActivation& activation,
        OnFooter&& onFooter,OnClearSearch&& onClearSearch,
        OnSearch&& onSearch,OnRow&& onRow,OnTile&& onTile) noexcept {
    try {
        switch(activation.target){
        case PickerPointerTarget::Footer:
            onFooter(activation.footer);
            return true;
        case PickerPointerTarget::ClearSearch:
            onClearSearch();
            return true;
        case PickerPointerTarget::Search:
            onSearch();
            return true;
        case PickerPointerTarget::Row:
            onRow(activation.rowIndex,activation.tileIndex);
            return true;
        case PickerPointerTarget::Tile:
            onTile(activation.tileIndex);
            return true;
        case PickerPointerTarget::None:
            break;
        }
    } catch(...) {
        return activation.target!=PickerPointerTarget::None;
    }
    return false;
}

inline bool PickerDragThresholdCrossed(
        POINT down,POINT current,int dragWidth,int dragHeight) noexcept {
    const long long width=std::max<long long>(1,dragWidth);
    const long long height=std::max<long long>(1,dragHeight);
    const long long left=width/2;
    const long long right=width-left;
    const long long top=height/2;
    const long long bottom=height-top;
    const long long dx=
        static_cast<long long>(current.x)-down.x;
    const long long dy=
        static_cast<long long>(current.y)-down.y;
    return dx < -left || dx >= right ||
           dy < -top || dy >= bottom;
}

enum class PickerPointerPhase {
    Idle,
    Armed,
    Dragging
};

enum class PickerGestureAction {
    None,
    DragStarted,
    Click,
    SwitchOnly,
    Drop,
    NoOp,
    Cancel
};

struct PickerPointerGesture {
    PickerPointerPhase phase=PickerPointerPhase::Idle;
    PickerRowActionSnapshot row;
    POINT down={0,0};
    bool ctrlAtDown=false;
    int dropTileIndex=-1;
    uint64_t rowLayoutEpoch=0;
};

struct PickerGestureResolution {
    PickerGestureAction action=PickerGestureAction::None;
    PickerRowActionSnapshot row;
    bool ctrlAtDown=false;
    int dropTileIndex=-1;
};

inline void CancelPickerRowGesture(
        PickerPointerGesture& gesture) noexcept {
    gesture.phase=PickerPointerPhase::Idle;
    gesture.row=PickerRowActionSnapshot{};
    gesture.down=POINT{0,0};
    gesture.ctrlAtDown=false;
    gesture.dropTileIndex=-1;
    gesture.rowLayoutEpoch=0;
}

inline bool SamePickerRowActionSnapshot(
        const PickerRowActionSnapshot& left,
        const PickerRowActionSnapshot& right) noexcept {
    return left.hwnd!=0 && left.hwnd==right.hwnd &&
           left.tileIndex==right.tileIndex &&
           left.windowIndex==right.windowIndex &&
           left.modelGeneration==right.modelGeneration &&
           left.rowLayoutEpoch==right.rowLayoutEpoch;
}

inline bool ArmPickerRowGesture(
        PickerPointerGesture& gesture,
        const PickerRowActionSnapshot& row,POINT down,
        bool ctrlAtDown,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    if(!PickerRowPresentationCurrent(
            row,modelGeneration,rowLayoutEpoch))
        return false;
    PickerPointerGesture staged;
    staged.phase=PickerPointerPhase::Armed;
    staged.row=row;
    staged.down=down;
    staged.ctrlAtDown=ctrlAtDown;
    staged.rowLayoutEpoch=rowLayoutEpoch;
    gesture=staged;
    return true;
}

inline PickerGestureAction UpdatePickerRowGesture(
        PickerPointerGesture& gesture,POINT current,
        int dragWidth,int dragHeight,
        uint64_t modelGeneration,uint64_t rowLayoutEpoch,
        int candidateDropTile) noexcept {
    if(gesture.phase==PickerPointerPhase::Idle)
        return PickerGestureAction::None;
    if(!PickerRowPresentationCurrent(
            gesture.row,modelGeneration,rowLayoutEpoch)){
        CancelPickerRowGesture(gesture);
        return PickerGestureAction::Cancel;
    }
    if(gesture.phase==PickerPointerPhase::Armed){
        if(!PickerDragThresholdCrossed(
                gesture.down,current,dragWidth,dragHeight))
            return PickerGestureAction::None;
        if(!PickerRowActionableForDrag(
                gesture.row,modelGeneration,rowLayoutEpoch)){
            CancelPickerRowGesture(gesture);
            return PickerGestureAction::Cancel;
        }
        gesture.phase=PickerPointerPhase::Dragging;
        gesture.dropTileIndex=candidateDropTile;
        return PickerGestureAction::DragStarted;
    }
    gesture.dropTileIndex=candidateDropTile;
    return PickerGestureAction::None;
}

inline PickerGestureResolution ResolvePickerRowButtonUp(
        const PickerPointerGesture& gesture,
        const PickerRowActionSnapshot* releaseRow,
        bool destinationExists,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    PickerGestureResolution result;
    result.row=gesture.row;
    result.ctrlAtDown=gesture.ctrlAtDown;
    result.dropTileIndex=gesture.dropTileIndex;
    if(gesture.phase==PickerPointerPhase::Idle){
        result.action=PickerGestureAction::Cancel;
        return result;
    }
    const bool presentationCurrent=PickerRowPresentationCurrent(
        gesture.row,modelGeneration,rowLayoutEpoch);
    if(gesture.phase==PickerPointerPhase::Armed){
        if(!presentationCurrent){
            result.action=!gesture.ctrlAtDown && destinationExists
                ? PickerGestureAction::SwitchOnly
                : PickerGestureAction::Cancel;
            return result;
        }
        result.action=releaseRow &&
            SamePickerRowActionSnapshot(gesture.row,*releaseRow)
                ? PickerGestureAction::Click
                : PickerGestureAction::Cancel;
        return result;
    }
    if(!PickerRowActionableForDrag(
            gesture.row,modelGeneration,rowLayoutEpoch) ||
       !destinationExists || gesture.dropTileIndex<0){
        result.action=PickerGestureAction::Cancel;
        return result;
    }
    result.action=gesture.dropTileIndex==gesture.row.tileIndex
        ? PickerGestureAction::NoOp
        : PickerGestureAction::Drop;
    return result;
}

inline bool PickerPointInRect(const RECT& rect,POINT point) noexcept {
    return rect.right>rect.left && rect.bottom>rect.top &&
           point.x>=rect.left && point.x<rect.right &&
           point.y>=rect.top && point.y<rect.bottom;
}

inline PickerFooterLink HitPickerFooterLink(
        const PickerFooterLayout& layout,POINT point) noexcept {
    if(PickerPointInRect(layout.repoLink,point))
        return PickerFooterLink::Repository;
    if(PickerPointInRect(layout.conusLink,point))
        return PickerFooterLink::ConusVision;
    return PickerFooterLink::None;
}

inline bool UpdatePickerFooterHover(
        PickerFooterLink& current,PickerFooterLink next) noexcept {
    if(current==next) return false;
    current=next;
    return true;
}

inline bool ResetPickerFooterHover(PickerFooterLink& current) noexcept {
    return UpdatePickerFooterHover(current,PickerFooterLink::None);
}

inline bool PickerFooterSuppressesRowHover(
        PickerFooterLink link) noexcept {
    return link!=PickerFooterLink::None;
}

enum class PickerHoverResetReason {
    None,
    CachePublication,
    CacheFailure,
    ExplicitInvalidation,
    Hide,
    MouseLeave
};

struct PickerHoverEventState {
    PickerFooterLink footerLink=PickerFooterLink::None;
    bool rowTooltipActive=false;
    PickerHoverResetReason lastResetReason=PickerHoverResetReason::None;
    uint64_t resetCount=0;
};

inline bool UpdatePickerFooterHoverEvent(
        PickerHoverEventState& state,PickerFooterLink next) noexcept {
    const bool changed=UpdatePickerFooterHover(state.footerLink,next);
    if(PickerFooterSuppressesRowHover(next))
        state.rowTooltipActive=false;
    return changed;
}

struct PickerFooterMouseMoveEffects {
    bool invalidateFooter=false;
    bool resetRowTooltip=false;
};

inline PickerFooterMouseMoveEffects RoutePickerFooterMouseMove(
        PickerHoverEventState& state,PickerFooterLink next,
        bool activeRowTooltip) noexcept {
    const bool hadRowTooltip=
        activeRowTooltip || state.rowTooltipActive;
    PickerFooterMouseMoveEffects effects;
    effects.invalidateFooter=
        UpdatePickerFooterHoverEvent(state,next);
    effects.resetRowTooltip=
        PickerFooterSuppressesRowHover(next) &&
        (effects.invalidateFooter || hadRowTooltip);
    return effects;
}

inline void ResetPickerHoverEventState(
        PickerHoverEventState& state,
        PickerHoverResetReason reason) noexcept {
    ResetPickerFooterHover(state.footerLink);
    state.rowTooltipActive=false;
    state.lastResetReason=reason;
    if(state.resetCount==(std::numeric_limits<uint64_t>::max)())
        state.resetCount=1;
    else
        ++state.resetCount;
}

inline bool PickerFooterUsesHandCursor(PickerFooterLink link) noexcept {
    return link!=PickerFooterLink::None;
}

inline bool PickerFooterOpenSucceeded(intptr_t result) noexcept {
    return result>32;
}

inline std::string GuidKey(const GUID& guid){
    return W2U8(GuidToString(guid));
}

enum class PickerPhase {
    Idle,
    TargetIssue, TargetVerify,
    IdentityVerifyBeforePopup,
    PopupIssue, PopupVerify,
    IdentityVerifyBeforeSwitch,
    SwitchIssue, DestinationVerify,
    PublishVisualAssignment,
    RollbackTargetIssue, RollbackTargetVerify,
    RollbackPopupIssue, RollbackPopupVerify,
    RollbackSwitchIssue, OriginVerify,
    SaveExactTarget, RefreshModel, FailureReport, FocusRestore
};

enum class PickerEvent {
    Begin, ApiCompleted, ReadbackCompleted, EffectCompleted,
    CancelRequested, Timer
};

enum class PickerEffectKind {
    None, ValidateTarget, MoveTarget, ReadTarget, MovePopup, ReadPopup,
    SwitchDesktop, ReadCurrent, PublishVisualAssignment,
    SaveExactTarget, Refresh,
    ShowAndFocus, Hide, ReportFailure
};

enum class PickerEffectExecutionRoute {
    Execute, DeferUntilDue, AcknowledgeWithoutUi
};

inline bool PickerEffectRequiresSettlingDelay(
        PickerEffectKind kind) noexcept {
    return kind==PickerEffectKind::ReadTarget ||
           kind==PickerEffectKind::ReadPopup ||
           kind==PickerEffectKind::ReadCurrent;
}

inline uint64_t PickerSettlingNotBeforeMs(
        uint64_t nowMs,uint64_t delayMs) noexcept {
    return nowMs>(std::numeric_limits<uint64_t>::max)()-delayMs
        ? (std::numeric_limits<uint64_t>::max)() : nowMs+delayMs;
}

inline uint64_t PickerSettlingDelayRemainingMs(
        uint64_t nowMs,uint64_t notBeforeMs) noexcept {
    return nowMs<notBeforeMs ? notBeforeMs-nowMs : 0;
}

inline bool PickerPumpImmediateKickAllowed(
        bool effectDeferredUntilDue) noexcept {
    return !effectDeferredUntilDue;
}

inline bool PickerDurableKickRequiredAfterDefer(
        bool deferAccepted,bool durableKickPublished) noexcept {
    return !deferAccepted || durableKickPublished;
}

inline PickerEffectExecutionRoute RoutePickerEffectExecution(
        PickerEffectKind kind,bool shutdownDrain) noexcept {
    if(!shutdownDrain) return PickerEffectExecutionRoute::Execute;
    if(PickerEffectRequiresSettlingDelay(kind))
        return PickerEffectExecutionRoute::DeferUntilDue;
    switch(kind){
    case PickerEffectKind::PublishVisualAssignment:
    case PickerEffectKind::Refresh:
    case PickerEffectKind::ShowAndFocus:
    case PickerEffectKind::ReportFailure:
        return PickerEffectExecutionRoute::AcknowledgeWithoutUi;
    default:
        return PickerEffectExecutionRoute::Execute;
    }
}

enum class PickerReadValidity { Unknown, Valid, Unavailable };
enum class PickerIdentityValidity { Unknown, Match, Lost, Indeterminate };
enum class PickerPopupRoute { Managed, StickyUnmanaged, Reject };

enum class PickerTransitionMode {
    MoveAndFollow,
    RowMoveOnly,
    VisualAndFollow,
    VisualOnly
};

enum class PickerHideDisposition {
    None,
    TransientRelocate,
    DismissSession
};

inline bool PickerHideEndsVisualSession(
        PickerHideDisposition disposition) noexcept {
    return disposition==PickerHideDisposition::DismissSession;
}

struct PickerTransitionPolicy {
    bool requiresCapturedActive=false;
    bool movesTarget=false;
    bool movesPopup=false;
    bool switchesDesktop=false;
    bool persistsTarget=false;
    bool publishesVisual=false;
    bool restoresPopupFocus=false;
    bool cancelDismissesSession=false;
};

inline PickerTransitionPolicy PickerPolicyFor(
        PickerTransitionMode mode) noexcept {
    PickerTransitionPolicy policy;
    switch(mode){
    case PickerTransitionMode::MoveAndFollow:
        policy.requiresCapturedActive=true;
        policy.movesTarget=true;
        policy.movesPopup=true;
        policy.switchesDesktop=true;
        policy.persistsTarget=true;
        policy.restoresPopupFocus=true;
        policy.cancelDismissesSession=true;
        break;
    case PickerTransitionMode::RowMoveOnly:
        policy.movesTarget=true;
        policy.persistsTarget=true;
        break;
    case PickerTransitionMode::VisualAndFollow:
        policy.requiresCapturedActive=true;
        policy.movesPopup=true;
        policy.switchesDesktop=true;
        policy.publishesVisual=true;
        policy.restoresPopupFocus=true;
        policy.cancelDismissesSession=true;
        break;
    case PickerTransitionMode::VisualOnly:
        policy.publishesVisual=true;
        break;
    }
    return policy;
}

struct PickerVisualAssignment {
    GUID baseDesktop={0};
    GUID destination={0};
};

using PickerVisualAssignments=
    std::map<std::string,PickerVisualAssignment>;

template<class Tiles,class TileGuid,class TileRows,
         class RowRuntimeKey,class PublishRow>
inline bool ApplyPickerVisualAssignmentsToModel(
        Tiles& tiles,PickerVisualAssignments& assignments,
        TileGuid&& tileGuid,TileRows&& tileRows,
        RowRuntimeKey&& rowRuntimeKey,PublishRow&& publishRow) noexcept {
    try {
        for(auto assignment=assignments.begin();
            assignment!=assignments.end();){
            size_t baseIndex=tiles.size();
            size_t destinationIndex=tiles.size();
            for(size_t index=0;index<tiles.size();++index){
                const GUID& candidate=tileGuid(tiles[index]);
                if(!GuidIsZero(candidate) &&
                   GuidEq(candidate,assignment->second.baseDesktop))
                    baseIndex=index;
                if(!GuidIsZero(candidate) &&
                   GuidEq(candidate,assignment->second.destination))
                    destinationIndex=index;
            }

            size_t sourceIndex=tiles.size();
            size_t rowIndex=0;
            bool duplicate=false;
            for(size_t tileIndex=0;tileIndex<tiles.size();++tileIndex){
                auto& rows=tileRows(tiles[tileIndex]);
                for(size_t index=0;index<rows.size();++index){
                    if(rowRuntimeKey(rows[index])!=assignment->first)
                        continue;
                    if(sourceIndex!=tiles.size()){
                        duplicate=true;
                        break;
                    }
                    sourceIndex=tileIndex;
                    rowIndex=index;
                }
                if(duplicate) break;
            }

            const bool stale=GuidIsZero(
                    assignment->second.baseDesktop) ||
                GuidIsZero(assignment->second.destination) ||
                GuidEq(assignment->second.baseDesktop,
                       assignment->second.destination) ||
                baseIndex==tiles.size() ||
                destinationIndex==tiles.size() ||
                sourceIndex==tiles.size() || duplicate;
            if(stale){
                assignment=assignments.erase(assignment);
                continue;
            }

            if(sourceIndex==destinationIndex){
                auto& rows=tileRows(tiles[sourceIndex]);
                publishRow(rows[rowIndex],assignment->second.baseDesktop,
                           assignment->second.destination,true);
            } else {
                auto& sourceRows=tileRows(tiles[sourceIndex]);
                auto moved=std::move(sourceRows[rowIndex]);
                publishRow(moved,assignment->second.baseDesktop,
                           assignment->second.destination,true);
                auto& destinationRows=tileRows(tiles[destinationIndex]);
                destinationRows.push_back(std::move(moved));
                sourceRows.erase(sourceRows.begin()+rowIndex);
            }
            ++assignment;
        }
        return true;
    } catch(...) {
        return false;
    }
}

enum class PickerVisualMutationKind {
    None,
    Upsert,
    Erase
};

struct PickerVisualAssignmentMutation {
    PickerVisualMutationKind kind=PickerVisualMutationKind::None;
    std::string runtimeKey;
    GUID baseDesktop={0};
    GUID destination={0};
};

enum class PopupSaveStatus { NotTracked, Saved, Failed };
enum class PopupSaveFailure {
    None, IdentityLost, IdentityIndeterminate, Classification,
    StorageUnavailable, StorageReadOnly, ReservationUnavailable,
    InvalidRecordId, DesktopUnavailable, WrongProfile,
    FingerprintUnavailable, IncompleteTitle, StageRejected,
    IdentityChanged, FlushFailed, Unexpected
};

inline bool PickerTerminalReservationAppAllowed(
        const std::string& reservedApp,
        const std::string& terminalApp) noexcept {
    return !terminalApp.empty() &&
           (reservedApp.empty() || reservedApp==terminalApp);
}

struct PickerObservation {
    PickerEvent event=PickerEvent::Timer;
    uint64_t generation=0;
    PickerEffectKind effectKind=PickerEffectKind::None;
    uint64_t effectSerial=0;
    PickerIdentityValidity identity=PickerIdentityValidity::Unknown;
    bool apiInvoked=false;
    bool apiAccepted=false;
    bool unissuedEffectCancelled=false;
    bool popupIsForeground=false;
    PopupSaveStatus saveStatus=PopupSaveStatus::Failed;
    PopupSaveFailure saveFailure=PopupSaveFailure::None;
    PickerReadValidity targetRead=PickerReadValidity::Unknown;
    PickerReadValidity popupRead=PickerReadValidity::Unknown;
    PickerReadValidity currentRead=PickerReadValidity::Unknown;
    GUID actualTargetDesktop={0};
    GUID actualPopupDesktop={0};
    GUID actualCurrentDesktop={0};
};

struct PickerEffect {
    PickerEffectKind kind=PickerEffectKind::None;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    GUID desktop={0};
    PickerHideDisposition hideDisposition=PickerHideDisposition::None;
};

struct PickerTransition {
    PickerPhase phase=PickerPhase::Idle;
    uint64_t generation=0;
    MoveToken reservationToken;
    PickerTransitionMode mode=PickerTransitionMode::MoveAndFollow;
    PickerVisualAssignmentMutation visualMutation;
    WindowIdentityKey target;
    WindowIdentityKey popupActiveTarget;
    std::string runtimeKey;
    std::string app;
    std::string pendingRecordId;
    std::wstring capturedTitle;
    GUID targetOrigin={0};
    GUID popupOrigin={0};
    PickerPopupRoute popupRoute=PickerPopupRoute::Managed;
    GUID currentOrigin={0};
    GUID destination={0};
    GUID observedTargetDesktop={0};
    GUID observedPopupDesktop={0};
    GUID observedCurrentDesktop={0};
    PickerReadValidity observedTargetValidity=PickerReadValidity::Unknown;
    PickerReadValidity observedPopupValidity=PickerReadValidity::Unknown;
    PickerReadValidity observedCurrentValidity=PickerReadValidity::Unknown;
    int forwardTargetAttempts=0;
    int forwardPopupAttempts=0;
    int forwardSwitchAttempts=0;
    int rollbackTargetAttempts=0;
    int rollbackPopupAttempts=0;
    int rollbackSwitchAttempts=0;
    int focusAttempts=0;
    bool targetMayHaveMoved=false;
    bool popupMayHaveMoved=false;
    bool switchMayHaveChanged=false;
    bool rollbackVerificationRequired=false;
    bool targetUnresolvedBeforeIssue=false;
    bool popupUnresolvedBeforeIssue=false;
    bool switchUnresolvedBeforeIssue=false;
    bool postSwitchPopupRepair=false;
    bool cancelRequested=false;
    bool dismissed=false;
    bool failed=false;
    std::wstring diagnostic;
    PickerEffectKind pendingEffect=PickerEffectKind::None;
    uint64_t effectSerial=0;
    bool hidePending=false;
    bool hideCompleted=false;
    bool failureReported=false;
    bool focusFailureTerminal=false;
    bool targetIdentityUnusable=false;
    bool commitCutoffReached=false;
    bool suppressFocus=false;
    bool terminalAcknowledged=false;
    bool capturedTitleComplete=false;
    PickerHideDisposition pendingHideDisposition=
        PickerHideDisposition::None;
    PickerPhase resumeAfterHide=PickerPhase::Idle;
    uint64_t identityGeneration=0;
    uint64_t lifecycleSaveGeneration=0;
    uint64_t lifecycleLayoutSignature=0;
    uint64_t lifecycleSessionSignature=0;

    void swap(PickerTransition& other) noexcept {
        std::swap(phase,other.phase);
        std::swap(generation,other.generation);
        std::swap(reservationToken,other.reservationToken);
        std::swap(mode,other.mode);
        std::swap(visualMutation.kind,other.visualMutation.kind);
        visualMutation.runtimeKey.swap(
            other.visualMutation.runtimeKey);
        std::swap(visualMutation.baseDesktop,
                  other.visualMutation.baseDesktop);
        std::swap(visualMutation.destination,
                  other.visualMutation.destination);
        std::swap(target,other.target);
        std::swap(popupActiveTarget,other.popupActiveTarget);
        runtimeKey.swap(other.runtimeKey);
        app.swap(other.app);
        pendingRecordId.swap(other.pendingRecordId);
        capturedTitle.swap(other.capturedTitle);
        std::swap(targetOrigin,other.targetOrigin);
        std::swap(popupOrigin,other.popupOrigin);
        std::swap(popupRoute,other.popupRoute);
        std::swap(currentOrigin,other.currentOrigin);
        std::swap(destination,other.destination);
        std::swap(observedTargetDesktop,other.observedTargetDesktop);
        std::swap(observedPopupDesktop,other.observedPopupDesktop);
        std::swap(observedCurrentDesktop,other.observedCurrentDesktop);
        std::swap(observedTargetValidity,other.observedTargetValidity);
        std::swap(observedPopupValidity,other.observedPopupValidity);
        std::swap(observedCurrentValidity,other.observedCurrentValidity);
        std::swap(forwardTargetAttempts,other.forwardTargetAttempts);
        std::swap(forwardPopupAttempts,other.forwardPopupAttempts);
        std::swap(forwardSwitchAttempts,other.forwardSwitchAttempts);
        std::swap(rollbackTargetAttempts,other.rollbackTargetAttempts);
        std::swap(rollbackPopupAttempts,other.rollbackPopupAttempts);
        std::swap(rollbackSwitchAttempts,other.rollbackSwitchAttempts);
        std::swap(focusAttempts,other.focusAttempts);
        std::swap(targetMayHaveMoved,other.targetMayHaveMoved);
        std::swap(popupMayHaveMoved,other.popupMayHaveMoved);
        std::swap(switchMayHaveChanged,other.switchMayHaveChanged);
        std::swap(rollbackVerificationRequired,
                  other.rollbackVerificationRequired);
        std::swap(targetUnresolvedBeforeIssue,
                  other.targetUnresolvedBeforeIssue);
        std::swap(popupUnresolvedBeforeIssue,
                  other.popupUnresolvedBeforeIssue);
        std::swap(switchUnresolvedBeforeIssue,
                  other.switchUnresolvedBeforeIssue);
        std::swap(postSwitchPopupRepair,other.postSwitchPopupRepair);
        std::swap(cancelRequested,other.cancelRequested);
        std::swap(dismissed,other.dismissed);
        std::swap(failed,other.failed);
        diagnostic.swap(other.diagnostic);
        std::swap(pendingEffect,other.pendingEffect);
        std::swap(effectSerial,other.effectSerial);
        std::swap(hidePending,other.hidePending);
        std::swap(hideCompleted,other.hideCompleted);
        std::swap(failureReported,other.failureReported);
        std::swap(focusFailureTerminal,other.focusFailureTerminal);
        std::swap(targetIdentityUnusable,other.targetIdentityUnusable);
        std::swap(commitCutoffReached,other.commitCutoffReached);
        std::swap(suppressFocus,other.suppressFocus);
        std::swap(terminalAcknowledged,other.terminalAcknowledged);
        std::swap(capturedTitleComplete,other.capturedTitleComplete);
        std::swap(pendingHideDisposition,
                  other.pendingHideDisposition);
        std::swap(resumeAfterHide,other.resumeAfterHide);
        std::swap(identityGeneration,other.identityGeneration);
        std::swap(lifecycleSaveGeneration,other.lifecycleSaveGeneration);
        std::swap(lifecycleLayoutSignature,other.lifecycleLayoutSignature);
        std::swap(lifecycleSessionSignature,other.lifecycleSessionSignature);
    }
};

inline bool PickerRuntimeTerminalizationReady(
        const PickerTransition& transition,
        bool exactReservationReleased) noexcept {
    return transition.terminalAcknowledged &&
        transition.pendingEffect==PickerEffectKind::None &&
        !transition.runtimeKey.empty() && exactReservationReleased;
}

enum class PickerTerminalGuardReleaseAction {
    ResolvedAbsent, ConsumeExact, RetryExactOwner, Count
};

inline PickerTerminalGuardReleaseAction DecidePickerTerminalGuardRelease(
        bool keyFound,bool keyTokenMatches,
        bool tokenFoundAnywhere) noexcept {
    if(keyFound)
        return keyTokenMatches
            ? PickerTerminalGuardReleaseAction::ConsumeExact
            : PickerTerminalGuardReleaseAction::RetryExactOwner;
    return tokenFoundAnywhere
        ? PickerTerminalGuardReleaseAction::RetryExactOwner
        : PickerTerminalGuardReleaseAction::ResolvedAbsent;
}

enum class PickerTerminalNoProgressRoute {
    DelayedTimer, DurableExternalKick
};

inline PickerTerminalNoProgressRoute DecidePickerTerminalNoProgressRoute(
        bool shutdownDrain,bool delayedTimerArmed) noexcept {
    return !shutdownDrain && delayedTimerArmed
        ? PickerTerminalNoProgressRoute::DelayedTimer
        : PickerTerminalNoProgressRoute::DurableExternalKick;
}

enum class PickerUiAction {
    CtrlMove, PlainSwitch, FooterNavigation, CloseOrReopen, SearchEdit,
    Selection, ManualSave, ManualRestore, Settings, About, Help, TrayExit
};

enum class PickerCloseRoute { Hide, Reject };

enum class PickerIdleEditInputRoute { Edit, Grid, Swallow };

inline PickerIdleEditInputRoute RoutePickerIdleEditInput(
        UINT message,WPARAM value,bool controlDown) noexcept {
    if(message==WM_KEYDOWN){
        switch(value){
        case VK_ESCAPE:
        case VK_RETURN:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_TAB:
            return PickerIdleEditInputRoute::Grid;
        case VK_SPACE:
            if(controlDown) return PickerIdleEditInputRoute::Grid;
            break;
        }
    }
    if(message==WM_CHAR &&
       (value==L'\r' || value==27 || value==L'\t' ||
        (value==L' ' && controlDown)))
        return PickerIdleEditInputRoute::Swallow;
    return PickerIdleEditInputRoute::Edit;
}

inline bool PickerControlledEditMessageAllowed(UINT message) noexcept {
    switch(message){
    case WM_SETTEXT:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_UNICHAR:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
    case WM_IME_KEYDOWN:
    case WM_IME_KEYUP:
    case WM_CUT:
    case WM_PASTE:
    case WM_CLEAR:
    case WM_UNDO:
    case EM_REPLACESEL:
    case EM_UNDO:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_CONTEXTMENU:
        return false;
    default:
        // Painting, focus/caret, accessibility, destruction and other
        // non-input lifecycle messages must still reach the EDIT control.
        return true;
    }
}

inline bool PickerReservationReplacementAllowed(
        MoveOwner prior,MoveOwner replacement) noexcept {
    if(prior==MoveOwner::Picker) return false;
    if(replacement==MoveOwner::Picker)
        return prior==MoveOwner::AutoReconcile;
    return true;
}

template<class Reservations>
inline bool PickerReservationBlocks(
        const FastWin& window,const Reservations& reservations) noexcept {
    const auto found=reservations.find(RuntimeKey(window));
    return found!=reservations.end() &&
           SameIdentity(found->second,IdentityOf(window));
}

enum class PickerAutoSupersession {
    None, CancelQueued, WaitForIssuedReadback
};

inline PickerAutoSupersession DecidePickerAutoSupersession(
        bool hasAutomaticJob,bool waitingForVerify,
        bool issueAwaitingVerify) noexcept {
    if(!hasAutomaticJob) return PickerAutoSupersession::None;
    if(waitingForVerify || issueAwaitingVerify)
        return PickerAutoSupersession::WaitForIssuedReadback;
    return PickerAutoSupersession::CancelQueued;
}

enum class PickerKickRoute {
    Posted, TimerArmed, InlineFallback, Teardown, PendingPreserved
};

struct PickerKickState {
    bool pending=false;
    PickerObservation observation;
};

inline PickerKickRoute StagePickerObservationKick(
        PickerKickState& state,const PickerObservation& observation,
        bool postSucceeded,bool timerSucceeded,bool runtimeAlive) noexcept {
    if(state.pending) return PickerKickRoute::PendingPreserved;
    state.observation=observation;
    state.pending=true;
    if(postSucceeded) return PickerKickRoute::Posted;
    if(timerSucceeded) return PickerKickRoute::TimerArmed;
    return runtimeAlive ? PickerKickRoute::InlineFallback
                        : PickerKickRoute::Teardown;
}

inline bool ConsumePickerObservationKick(
        PickerKickState& state,PickerObservation& output) noexcept {
    if(!state.pending) return false;
    output=state.observation;
    state.pending=false;
    return true;
}

struct PopupSaveResult {
    PopupSaveStatus status=PopupSaveStatus::Failed;
    PopupSaveFailure failure=PopupSaveFailure::None;
    std::string app;
};

template<class Assign>
inline bool TryStagePickerPersistenceAppNoThrow(
        PopupSaveResult& result,const std::string& app,
        Assign&& assign) noexcept {
    try {
        assign(result.app,app);
        return true;
    } catch(...) { return false; }
}

inline bool TryStagePickerPersistenceAppNoThrow(
        PopupSaveResult& result,const std::string& app) noexcept {
    return TryStagePickerPersistenceAppNoThrow(
        result,app,[](std::string& output,const std::string& value){
            std::string staged=value;
            output.swap(staged);
        });
}

template<class Persist>
inline PopupSaveResult RunPickerPersistenceTransaction(
        PopupBrowserClassification classification,const std::string& app,
        PopupPersistenceReadiness readiness,Persist&& persist) noexcept {
    PopupSaveResult result;
    result.status=PopupSaveStatus::Failed;
    if(classification==PopupBrowserClassification::NotTracked){
        result.status=PopupSaveStatus::NotTracked;
        return result;
    }
    if(classification!=PopupBrowserClassification::Tracked || app.empty()){
        result.failure=PopupSaveFailure::Classification;
        TryStagePickerPersistenceAppNoThrow(result,app);
        return result;
    }
    if(readiness==PopupPersistenceReadiness::Unavailable){
        result.failure=PopupSaveFailure::StorageUnavailable;
        TryStagePickerPersistenceAppNoThrow(result,app);
        return result;
    }
    if(readiness!=PopupPersistenceReadiness::Ready){
        result.failure=PopupSaveFailure::StorageReadOnly;
        TryStagePickerPersistenceAppNoThrow(result,app);
        return result;
    }
    if(!TryStagePickerPersistenceAppNoThrow(result,app)){
        result.failure=PopupSaveFailure::Unexpected;
        return result;
    }
    try {
        PopupSaveResult persisted=persist(app);
        if(persisted.status!=PopupSaveStatus::NotTracked &&
           persisted.app.empty()){
            persisted.status=PopupSaveStatus::Failed;
            persisted.failure=PopupSaveFailure::Unexpected;
        }
        return persisted;
    } catch(...) {
        result.status=PopupSaveStatus::Failed;
        result.failure=PopupSaveFailure::Unexpected;
        return result;
    }
}

inline const wchar_t* PickerSaveFailureDiagnostic(
        PopupSaveFailure failure) noexcept {
    switch(failure){
    case PopupSaveFailure::IdentityLost:
        return L"The target window identity was lost before saving.";
    case PopupSaveFailure::IdentityIndeterminate:
        return L"The target window identity could not be verified before saving.";
    case PopupSaveFailure::Classification:
        return L"Browser classification failed; the destination was not saved.";
    case PopupSaveFailure::StorageUnavailable:
        return L"Automatic layout storage is unavailable; the destination remains unsaved.";
    case PopupSaveFailure::StorageReadOnly:
        return L"Automatic layout storage is read-only or disabled; the destination remains unsaved.";
    case PopupSaveFailure::ReservationUnavailable:
        return L"The exact picker reservation was unavailable while saving.";
    case PopupSaveFailure::InvalidRecordId:
        return L"The reserved browser record identifier was invalid.";
    case PopupSaveFailure::DesktopUnavailable:
        return L"The selected desktop could not be resolved while saving.";
    case PopupSaveFailure::WrongProfile:
        return L"The reserved browser record belongs to another profile.";
    case PopupSaveFailure::FingerprintUnavailable:
        return L"Fresh browser data and a safe provisional title were both unavailable.";
    case PopupSaveFailure::IncompleteTitle:
        return L"The browser title was incomplete, so no provisional record was created.";
    case PopupSaveFailure::StageRejected:
        return L"The exact browser record update could not be staged.";
    case PopupSaveFailure::IdentityChanged:
        return L"The target identity changed before the browser update could be published.";
    case PopupSaveFailure::FlushFailed:
        return L"The browser update remains queued because the layout file could not be written.";
    case PopupSaveFailure::Unexpected:
        return L"The browser destination could not be saved safely.";
    case PopupSaveFailure::None:
        break;
    }
    return L"The browser destination could not be saved.";
}

inline const wchar_t* PickerPostSaveIdentityDiagnostic(
        PopupSaveStatus status,
        PickerIdentityValidity identity) noexcept {
    const bool lost=identity==PickerIdentityValidity::Lost;
    if(status==PopupSaveStatus::Saved)
        return lost
            ? L"The destination was saved, but the target identity was lost afterward."
            : L"The destination was saved, but the target identity could not be revalidated afterward.";
    if(status==PopupSaveStatus::NotTracked)
        return lost
            ? L"The moved window identity was lost after the popup operation."
            : L"The moved window identity could not be revalidated after the popup operation.";
    return lost
        ? L"The target identity was lost after the persistence attempt."
        : L"The target identity could not be revalidated after the persistence attempt.";
}

template<class Complete>
inline bool CompletePickerLifecycleForSave(
        const PopupSaveResult& result,Complete&& complete) noexcept {
    if(result.status!=PopupSaveStatus::Saved || result.app.empty()) return false;
    try {
        complete(result.app);
        return true;
    } catch(...) { return false; }
}

inline bool PickerFreshRuntimeMatches(
        const WindowIdentityKey& expected,uint64_t expectedGeneration,
        const WindowIdentityKey& cached,uint64_t cachedGeneration) noexcept {
    return expectedGeneration!=0 && expectedGeneration==cachedGeneration &&
           SameIdentity(expected,cached);
}

inline bool PickerAcceptedFreshRowUsable(
        bool resultFresh,bool exactSessionAssociation,
        const WindowIdentityKey& identity,bool appMatches,
        const std::string& activeTitle,
        const std::map<std::string,int>& counts) noexcept {
    return resultFresh && exactSessionAssociation &&
        SameIdentity(identity,identity) && appMatches &&
        (!activeTitle.empty() || !counts.empty());
}

inline bool PickerRowUsesFreshFingerprint(
        bool resultFresh,bool rowUsableFresh) noexcept {
    return resultFresh && rowUsableFresh;
}

inline bool PickerUnboundRowEligibleForReconcilePlan(
        bool resultFresh,bool rowUsableFresh) noexcept {
    return !resultFresh || rowUsableFresh;
}

inline bool PickerUnboundPlanUsesFreshness(
        bool resultFresh,bool omittedUnusableRow) noexcept {
    return resultFresh && !omittedUnusableRow;
}

inline bool PickerTitleOnlyProvisionalFieldsUsable(
        const std::string& app,
        const std::string& normalizedTitle) noexcept {
    return !app.empty() && !normalizedTitle.empty();
}

enum class PickerFreshRecordSource {
    None, Reserved, AcceptedAfterEntry
};

inline PickerFreshRecordSource SelectPickerFreshRecordSource(
        const WindowIdentityKey& expected,uint64_t expectedGeneration,
        bool hasReserved,const WindowIdentityKey& reservedIdentity,
        uint64_t reservedGeneration,bool hasAcceptedAfterEntry,
        const WindowIdentityKey& acceptedIdentity,
        uint64_t acceptedGeneration) noexcept {
    if(hasAcceptedAfterEntry && PickerFreshRuntimeMatches(
            expected,expectedGeneration,
            acceptedIdentity,acceptedGeneration))
        return PickerFreshRecordSource::AcceptedAfterEntry;
    if(hasReserved && PickerFreshRuntimeMatches(
            expected,expectedGeneration,
            reservedIdentity,reservedGeneration))
        return PickerFreshRecordSource::Reserved;
    return PickerFreshRecordSource::None;
}

inline bool PickerHasSafeOriginRecord(bool titleComplete,
                                      bool hasAcceptedFresh) noexcept {
    return titleComplete || hasAcceptedFresh;
}

inline bool PickerMayReserveStableRecordId(
        bool tracked,bool persistenceReady,
        bool originDesktopValid) noexcept {
    return tracked && persistenceReady && originDesktopValid;
}

enum class PickerAcceptedPlanRecordResult {
    Unrelated, Selected, Rejected
};

inline PickerAcceptedPlanRecordResult AccumulatePickerAcceptedPlanRecord(
        const WindowIdentityKey& expected,const std::string& expectedApp,
        const WindowIdentityKey& candidate,const std::string& candidateApp,
        const std::string& candidateRecordId,std::string& selected,
        bool& found) noexcept {
    if(!SameIdentity(expected,candidate))
        return PickerAcceptedPlanRecordResult::Unrelated;
    if(expectedApp.empty() || candidateApp!=expectedApp ||
       candidateRecordId.empty())
        return PickerAcceptedPlanRecordResult::Rejected;
    if(found)
        return selected==candidateRecordId
            ? PickerAcceptedPlanRecordResult::Selected
            : PickerAcceptedPlanRecordResult::Rejected;
    try {
        std::string staged=candidateRecordId;
        selected.swap(staged);
        found=true;
        return PickerAcceptedPlanRecordResult::Selected;
    } catch(...) {
        return PickerAcceptedPlanRecordResult::Rejected;
    }
}

inline PickerAcceptedPlanRecordResult
AccumulatePickerAcceptedPlanRecordAdoptingApp(
        const WindowIdentityKey& expected,const std::string& capturedApp,
        const WindowIdentityKey& candidate,const std::string& candidateApp,
        const std::string& candidateRecordId,std::string& selectedApp,
        std::string& selectedRecordId,bool& found) noexcept {
    if(!SameIdentity(expected,candidate))
        return PickerAcceptedPlanRecordResult::Unrelated;
    if(candidateApp.empty() || candidateRecordId.empty() ||
       (!capturedApp.empty() && capturedApp!=candidateApp) ||
       (!selectedApp.empty() && selectedApp!=candidateApp))
        return PickerAcceptedPlanRecordResult::Rejected;
    if(found)
        return selectedApp==candidateApp &&
               selectedRecordId==candidateRecordId
            ? PickerAcceptedPlanRecordResult::Selected
            : PickerAcceptedPlanRecordResult::Rejected;
    try {
        std::string stagedApp=candidateApp;
        std::string stagedRecord=candidateRecordId;
        selectedApp.swap(stagedApp);
        selectedRecordId.swap(stagedRecord);
        found=true;
        return PickerAcceptedPlanRecordResult::Selected;
    } catch(...) {
        return PickerAcceptedPlanRecordResult::Rejected;
    }
}

template<class PendingMap>
inline bool StagePickerAcceptedPlanPendingAssociation(
        const PendingMap& current,const std::string& runtimeKey,
        const std::string& recordId,PendingMap& staged) noexcept {
    if(runtimeKey.empty() || recordId.empty()) return false;
    try {
        PendingMap next=current;
        auto existing=next.find(runtimeKey);
        if(existing==next.end()){
            if(!next.emplace(runtimeKey,recordId).second) return false;
        } else {
            existing->second=recordId;
        }
        staged.swap(next);
        return true;
    } catch(...) { return false; }
}

struct PickerOperationLifetimeClaim {
    std::string recordId;
    bool recordExisted=false;
    bool pickerPublished=false;
    bool pickerEpisodePublished=false;
    bool pickerTerminalObserved=false;
    bool pickerTargetRestored=false;
};

template<class ClaimMap>
inline bool StagePickerOperationLifetimeClaim(
        const ClaimMap& current,const std::string& runtimeKey,
        const std::string& recordId,bool recordExisted,
        ClaimMap& staged) noexcept {
    if(runtimeKey.empty() || recordId.empty()) return false;
    try {
        ClaimMap next=current;
        auto existing=next.find(runtimeKey);
        if(existing==next.end()){
            PickerOperationLifetimeClaim claim;
            claim.recordId=recordId;
            claim.recordExisted=recordExisted;
            if(!next.emplace(runtimeKey,std::move(claim)).second) return false;
        } else if(existing->second.recordId!=recordId ||
                  existing->second.recordExisted!=recordExisted) {
            return false;
        } else {
            // A repeated exact handoff is a new Picker ownership episode.
            // Preserve an older durable publication against the stale plan,
            // but only the newest episode may bypass rollback/rearm checks.
            existing->second.pickerEpisodePublished=false;
            existing->second.pickerTerminalObserved=false;
            existing->second.pickerTargetRestored=false;
        }
        staged.swap(next);
        return true;
    } catch(...) { return false; }
}

template<class ClaimMap>
inline bool PickerOperationLifetimeClaimMatches(
        const ClaimMap& claims,const WindowIdentityKey& identity,
        const std::string& recordId) noexcept {
    if(!SameIdentity(identity,identity) || recordId.empty()) return false;
    try {
        const auto found=claims.find(RuntimeKey(identity));
        return found!=claims.end() && found->second.recordId==recordId;
    } catch(...) { return false; }
}

inline bool PickerOperationLifetimeClaimMustProtect(
        const PickerOperationLifetimeClaim& claim,
        bool recordCurrentlyExists) noexcept {
    return claim.pickerPublished && !claim.recordId.empty() &&
        (claim.recordExisted || recordCurrentlyExists);
}

inline bool PickerSavePublishesOperationLifetimeClaim(
        PopupSaveStatus status,PopupSaveFailure failure) noexcept {
    return status==PopupSaveStatus::Saved ||
        (status==PopupSaveStatus::Failed &&
         failure==PopupSaveFailure::FlushFailed);
}

inline bool PickerRowMoveSaveCommits(
        PopupSaveStatus status,PopupSaveFailure failure) noexcept {
    return (status==PopupSaveStatus::NotTracked &&
            failure==PopupSaveFailure::None) ||
           PickerSavePublishesOperationLifetimeClaim(status,failure);
}

template<class ClaimMap>
inline bool MarkPickerOperationLifetimeClaimPublished(
        ClaimMap& claims,const std::string& runtimeKey,
        const std::string& recordId,PopupSaveStatus status,
        PopupSaveFailure failure) noexcept {
    if(runtimeKey.empty() || recordId.empty() ||
       !PickerSavePublishesOperationLifetimeClaim(status,failure))
        return false;
    try {
        auto found=claims.find(runtimeKey);
        if(found==claims.end() || found->second.recordId!=recordId)
            return false;
        found->second.pickerPublished=true;
        found->second.pickerEpisodePublished=true;
        return true;
    } catch(...) { return false; }
}

template<class ClaimMap>
inline bool MarkPickerOperationLifetimeClaimTerminalOutcome(
        ClaimMap& claims,const std::string& runtimeKey,
        const std::string& recordId,bool targetRestored) noexcept {
    if(runtimeKey.empty() || recordId.empty()) return false;
    try {
        auto found=claims.find(runtimeKey);
        if(found==claims.end() || found->second.recordId!=recordId)
            return false;
        found->second.pickerTerminalObserved=true;
        found->second.pickerTargetRestored=targetRestored;
        return true;
    } catch(...) { return false; }
}

inline bool PickerTransitionTargetRestoredToOrigin(
        const PickerTransition& transition) noexcept {
    return !GuidIsZero(transition.targetOrigin) &&
        transition.observedTargetValidity==PickerReadValidity::Valid &&
        GuidEq(transition.observedTargetDesktop,transition.targetOrigin) &&
        !transition.targetMayHaveMoved &&
        !transition.targetIdentityUnusable;
}

enum class PickerOperationLifetimeClaimReleaseAction {
    ProtectPublished, RearmRestore, RearmOperation, ReleaseToPlan
};

inline PickerOperationLifetimeClaimReleaseAction
DecidePickerOperationLifetimeClaimRelease(
        const PickerOperationLifetimeClaim& claim,
        bool recordCurrentlyExists,bool restoreRequired) noexcept {
    if(!claim.pickerEpisodePublished){
        if(!claim.pickerTerminalObserved || !claim.pickerTargetRestored)
            return claim.pickerPublished || !restoreRequired
                ? PickerOperationLifetimeClaimReleaseAction::RearmOperation
                : PickerOperationLifetimeClaimReleaseAction::RearmRestore;
        if(PickerOperationLifetimeClaimMustProtect(
                claim,recordCurrentlyExists))
            return PickerOperationLifetimeClaimReleaseAction::ProtectPublished;
        if(restoreRequired)
            return PickerOperationLifetimeClaimReleaseAction::RearmRestore;
    }
    return PickerOperationLifetimeClaimMustProtect(
            claim,recordCurrentlyExists)
        ? PickerOperationLifetimeClaimReleaseAction::ProtectPublished
        : PickerOperationLifetimeClaimReleaseAction::ReleaseToPlan;
}

enum class PickerOperationClaimQueueAction {
    EraseClaimAndAwaitMove, RetainClaimAndRearmOperation
};

inline PickerOperationClaimQueueAction
DecidePickerOperationClaimQueuePublication(
        bool queuePublished) noexcept {
    return queuePublished
        ? PickerOperationClaimQueueAction::EraseClaimAndAwaitMove
        : PickerOperationClaimQueueAction::RetainClaimAndRearmOperation;
}

enum class PickerRearmedMoveArmAction { AwaitMove, RearmOperation };

inline PickerRearmedMoveArmAction DecidePickerRearmedMoveArmResult(
        bool timerArmed) noexcept {
    return timerArmed ? PickerRearmedMoveArmAction::AwaitMove
                      : PickerRearmedMoveArmAction::RearmOperation;
}

struct PickerOperationClaimRearmControl {
    bool moveQueued=false;
    bool armPending=false;
    bool retryOperation=false;
};

inline PickerOperationClaimQueueAction
ObservePickerOperationClaimQueuePublication(
        PickerOperationClaimRearmControl& control,
        bool queuePublished) noexcept {
    const PickerOperationClaimQueueAction action=
        DecidePickerOperationClaimQueuePublication(queuePublished);
    if(queuePublished) control.moveQueued=true;
    else control.retryOperation=true;
    return action;
}

inline void BeginPickerOperationClaimMoveArm(
        PickerOperationClaimRearmControl& control) noexcept {
    control.armPending=control.moveQueued;
}

inline PickerRearmedMoveArmAction CompletePickerOperationClaimMoveArm(
        PickerOperationClaimRearmControl& control,
        bool timerArmed) noexcept {
    const PickerRearmedMoveArmAction action=
        DecidePickerRearmedMoveArmResult(timerArmed);
    control.armPending=false;
    control.moveQueued=false;
    if(!timerArmed) control.retryOperation=true;
    return action;
}

inline bool PickerOperationClaimRearmRequiresRetry(
        const PickerOperationClaimRearmControl& control) noexcept {
    return control.retryOperation || control.armPending;
}

enum class PickerAutoCancelledMoveOwnerAction {
    IgnoreSupersession, RearmOperation
};

inline PickerAutoCancelledMoveOwnerAction
DecidePickerAutoCancelledMoveOwnerAction(
        bool timerArmFailureCancellation) noexcept {
    return timerArmFailureCancellation
        ? PickerAutoCancelledMoveOwnerAction::RearmOperation
        : PickerAutoCancelledMoveOwnerAction::IgnoreSupersession;
}

inline bool ShouldCancelMoveBeforeIssuedReadback(
        bool cancelRequested,bool retireAfterVerify,
        bool waitingForVerify) noexcept {
    return cancelRequested && !(retireAfterVerify && waitingForVerify);
}

template<class OperationMap>
inline typename OperationMap::iterator PickerOperationCursorNext(
        OperationMap& operations,bool haveCursor,
        uint64_t cursor) noexcept {
    try {
        return haveCursor ? operations.upper_bound(cursor)
                          : operations.begin();
    } catch(...) { return operations.end(); }
}

enum class PickerInFlightPlanEntryAction {
    Allow, ReuseAccepted, Wait
};

inline PickerInFlightPlanEntryAction DecidePickerInFlightPlanEntry(
        bool sameAppOperation,bool planInputKnown,bool exactInInput,
        bool acceptedExactRecord) noexcept {
    if(!sameAppOperation) return PickerInFlightPlanEntryAction::Allow;
    if(!planInputKnown) return PickerInFlightPlanEntryAction::Wait;
    if(!exactInInput) return PickerInFlightPlanEntryAction::Allow;
    return acceptedExactRecord
        ? PickerInFlightPlanEntryAction::ReuseAccepted
        : PickerInFlightPlanEntryAction::Wait;
}

enum class PickerLatePlanHandoffAction {
    Ignore, TransferBeforeSave, RejectPlan
};

inline PickerLatePlanHandoffAction DecidePickerLatePlanHandoff(
        bool exactPickerReservation,bool sameTransitionToken,
        bool commitCutoffReached) noexcept {
    if(!exactPickerReservation)
        return PickerLatePlanHandoffAction::Ignore;
    if(sameTransitionToken && !commitCutoffReached)
        return PickerLatePlanHandoffAction::TransferBeforeSave;
    return PickerLatePlanHandoffAction::RejectPlan;
}

template<class ClearBudget,class Publish>
inline bool CommitPickerAcceptedPlanRecordTransfer(
        ClearBudget&& clearBudget,Publish&& publish) noexcept {
    bool cleared=false;
    try { cleared=clearBudget(); }
    catch(...) { cleared=false; }
    if(!cleared) return false;
    try {
        publish();
        return true;
    } catch(...) { return false; }
}

struct PickerState {
    GUID currentDesktop={0};
    GUID selectedDesktop={0};
    int selectedIndex=-1;
    WindowIdentityKey activeWindow;
    std::wstring searchEditText;
    std::wstring searchText;
    bool searchActive=false;
    PickerTransition transition;
    PickerVisualAssignments visualAssignments;
    std::map<std::string,int> scrollByDesktop;
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;
    uint64_t paintGeneration=0;

    bool controlledTransition() const noexcept {
        return transition.phase!=PickerPhase::Idle;
    }

    void swap(PickerState& other) noexcept {
        static_assert(noexcept(searchEditText.swap(other.searchEditText)),
                      "picker edit-text swap must be noexcept");
        static_assert(noexcept(searchText.swap(other.searchText)),
                      "picker search swap must be noexcept");
        static_assert(noexcept(scrollByDesktop.swap(
                          other.scrollByDesktop)),
                      "picker scroll swap must be noexcept");
        static_assert(noexcept(visualAssignments.swap(
                          other.visualAssignments)),
                      "picker visual assignment swap must be noexcept");
        GUID guid=currentDesktop;
        currentDesktop=other.currentDesktop;
        other.currentDesktop=guid;
        guid=selectedDesktop;
        selectedDesktop=other.selectedDesktop;
        other.selectedDesktop=guid;
        const int index=selectedIndex;
        selectedIndex=other.selectedIndex;
        other.selectedIndex=index;
        const WindowIdentityKey identity=activeWindow;
        activeWindow=other.activeWindow;
        other.activeWindow=identity;
        searchEditText.swap(other.searchEditText);
        searchText.swap(other.searchText);
        const bool active=searchActive;
        searchActive=other.searchActive;
        other.searchActive=active;
        transition.swap(other.transition);
        visualAssignments.swap(other.visualAssignments);
        scrollByDesktop.swap(other.scrollByDesktop);
        const uint64_t model= modelGeneration;
        modelGeneration=other.modelGeneration;
        other.modelGeneration=model;
        const uint64_t rowLayout=rowLayoutEpoch;
        rowLayoutEpoch=other.rowLayoutEpoch;
        other.rowLayoutEpoch=rowLayout;
        const uint64_t generation=paintGeneration;
        paintGeneration=other.paintGeneration;
        other.paintGeneration=generation;
    }
};

inline bool PickerInteractionBusy(
        const PickerState& state,
        const PickerPointerGesture& gesture) noexcept {
    return state.controlledTransition() ||
           gesture.phase!=PickerPointerPhase::Idle;
}

inline bool StagePickerVisualAssignmentMutation(
        PickerState& state,
        const PickerVisualAssignmentMutation& mutation) noexcept {
    if(mutation.kind==PickerVisualMutationKind::None)
        return true;
    if(mutation.runtimeKey.empty())
        return false;
    if(mutation.kind==PickerVisualMutationKind::Upsert &&
       (GuidIsZero(mutation.baseDesktop) ||
        GuidIsZero(mutation.destination)))
        return false;
    try {
        PickerVisualAssignments staged=state.visualAssignments;
        if(mutation.kind==PickerVisualMutationKind::Erase){
            staged.erase(mutation.runtimeKey);
        } else if(mutation.kind==PickerVisualMutationKind::Upsert){
            PickerVisualAssignment assignment;
            const auto existing=staged.find(mutation.runtimeKey);
            assignment.baseDesktop=existing==staged.end()
                ? mutation.baseDesktop
                : existing->second.baseDesktop;
            if(GuidIsZero(assignment.baseDesktop))
                return false;
            assignment.destination=mutation.destination;
            if(GuidEq(assignment.baseDesktop,
                      assignment.destination))
                staged.erase(mutation.runtimeKey);
            else
                staged[mutation.runtimeKey]=assignment;
        } else {
            return false;
        }
        state.visualAssignments.swap(staged);
        return true;
    } catch(...) {
        return false;
    }
}

inline void EndPickerVisualSession(PickerState& state) noexcept {
    PickerVisualAssignments empty;
    state.visualAssignments.swap(empty);
}

inline bool PickerTransitionTargetsAuthorized(
        const PickerState& state,
        const PickerTransition& transition) noexcept {
    if(!SameIdentity(transition.target,transition.target))
        return false;
    const PickerTransitionPolicy policy=
        PickerPolicyFor(transition.mode);
    if(!policy.requiresCapturedActive)
        return true;
    const bool hasPopupActive=SameIdentity(
        transition.popupActiveTarget,
        transition.popupActiveTarget);
    return hasPopupActive &&
           SameIdentity(state.activeWindow,
                        transition.popupActiveTarget) &&
           SameIdentity(transition.target,
                        transition.popupActiveTarget);
}

inline bool PickerTransitionPopupRouteReady(
        const PickerTransition& transition) noexcept {
    return (transition.popupRoute==PickerPopupRoute::Managed &&
            !GuidIsZero(transition.popupOrigin)) ||
           (transition.popupRoute==PickerPopupRoute::StickyUnmanaged &&
            GuidIsZero(transition.popupOrigin));
}

inline bool PickerTransitionBeginPreconditions(
        const PickerState& state,
        const PickerTransition& transition) noexcept {
    if(transition.phase!=PickerPhase::Idle ||
       transition.generation==0 ||
       transition.pendingEffect!=PickerEffectKind::None ||
       transition.reservationToken.owner!=MoveOwner::Picker ||
       transition.reservationToken.operationId==0 ||
       transition.reservationToken.jobId==0 ||
       transition.runtimeKey.empty() ||
       !PickerTransitionTargetsAuthorized(state,transition) ||
       GuidIsZero(transition.destination))
        return false;

    switch(transition.mode){
    case PickerTransitionMode::MoveAndFollow:
        return state.selectedIndex>=0 &&
               GuidEq(state.selectedDesktop,
                      transition.destination) &&
               GuidEq(state.currentDesktop,
                      transition.currentOrigin) &&
               !GuidIsZero(transition.targetOrigin) &&
               !GuidIsZero(transition.currentOrigin) &&
               PickerTransitionPopupRouteReady(transition);
    case PickerTransitionMode::RowMoveOnly:
        return !GuidIsZero(transition.targetOrigin) &&
               !GuidEq(transition.targetOrigin,
                       transition.destination);
    case PickerTransitionMode::VisualAndFollow:
        return state.selectedIndex>=0 &&
               GuidEq(state.selectedDesktop,
                      transition.destination) &&
               GuidEq(state.currentDesktop,
                      transition.currentOrigin) &&
               !GuidIsZero(transition.currentOrigin) &&
               PickerTransitionPopupRouteReady(transition);
    case PickerTransitionMode::VisualOnly:
        return true;
    }
    return false;
}

inline bool SetPickerSearchText(PickerState& state,
                                const std::wstring& editText,
                                const std::wstring& normalized) noexcept {
    try {
        std::wstring stagedEdit=editText;
        std::wstring stagedNormalized=normalized;
        state.searchEditText.swap(stagedEdit);
        state.searchText.swap(stagedNormalized);
        return true;
    } catch(...) { return false; }
}

inline bool PrunePickerScrollState(
        PickerState& state,const std::vector<GUID>& desktops) noexcept {
    try {
        std::vector<std::string> liveKeys;
        liveKeys.reserve(desktops.size());
        for(const GUID& desktop : desktops)
            if(!GuidIsZero(desktop)) liveKeys.push_back(GuidKey(desktop));
        std::map<std::string,int> staged=state.scrollByDesktop;
        for(auto it=staged.begin();it!=staged.end();){
            if(std::find(liveKeys.begin(),liveKeys.end(),it->first)==
               liveKeys.end())
                it=staged.erase(it);
            else
                ++it;
        }
        state.scrollByDesktop.swap(staged);
        return true;
    } catch(...) { return false; }
}

inline bool AdoptPickerIdleActiveIdentity(
        PickerState& state,const WindowIdentityKey& candidate,
        uintptr_t popupHwnd,uintptr_t mainHwnd,
        bool popupVisible) noexcept {
    if(!popupVisible || state.controlledTransition() ||
       !SameIdentity(candidate,candidate) || candidate.hwnd==popupHwnd ||
       candidate.hwnd==mainHwnd)
        return false;
    state.activeWindow=candidate;
    return true;
}

enum class PickerForegroundObservation {
    Unavailable, Popup, ValidExternal, UnusableExternal
};

enum class PickerLightweightActiveUpdate { Preserved, Adopted, Cleared };

inline PickerLightweightActiveUpdate ApplyPickerLightweightHighlightSnapshot(
        PickerState& state,PickerReadValidity currentValidity,
        const GUID& currentDesktop,
        PickerForegroundObservation foreground,
        const WindowIdentityKey& foregroundIdentity,
        PickerIdentityValidity activeIdentity,
        uintptr_t popupHwnd,uintptr_t mainHwnd,
        bool popupVisible) noexcept {
    if(state.controlledTransition())
        return PickerLightweightActiveUpdate::Preserved;
    state.currentDesktop=
        currentValidity==PickerReadValidity::Valid &&
        !GuidIsZero(currentDesktop) ? currentDesktop : GUID{};
    if(!popupVisible)
        return PickerLightweightActiveUpdate::Preserved;
    if(foreground==PickerForegroundObservation::ValidExternal &&
       AdoptPickerIdleActiveIdentity(
           state,foregroundIdentity,popupHwnd,mainHwnd,popupVisible))
        return PickerLightweightActiveUpdate::Adopted;
    if(foreground==PickerForegroundObservation::UnusableExternal ||
       activeIdentity==PickerIdentityValidity::Lost){
        state.activeWindow=WindowIdentityKey{};
        return PickerLightweightActiveUpdate::Cleared;
    }
    return PickerLightweightActiveUpdate::Preserved;
}

struct PickerTabSearchCacheState {
    uint64_t attemptId=0;
    uint64_t modelGeneration=0;
    std::wstring query;
    bool ready=false;
    bool pending=false;
    bool retryNeeded=false;
    bool routeFreed=false;
    bool retryPosted=false;
    bool retryDeliveryPending=false;
    unsigned retryAttempts=0;
    unsigned blockedAppMask=0;
    bool blockedOnAnyRoute=false;
};

enum class PickerTabSearchRetryTrigger {
    ExactAppRoute, AnyRoute, Immediate
};

inline bool PickerTabSearchKeyMatches(
        const PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return modelGeneration!=0 && !normalizedQuery.empty() &&
           cache.modelGeneration==modelGeneration &&
           cache.query==normalizedQuery;
}

inline bool PickerTabSearchAttemptMatches(
        const PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return cache.pending && attemptId!=0 && cache.attemptId==attemptId &&
           PickerTabSearchKeyMatches(
               cache,modelGeneration,normalizedQuery);
}

inline unsigned PickerTabSearchAppBit(const std::string& app) noexcept {
    if(app=="firefox") return 1U;
    if(app=="chrome") return 2U;
    if(app=="edge") return 4U;
    return 8U;
}

inline bool PickerTabSearchCacheUsable(
        const PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return cache.ready && !cache.pending && !cache.retryNeeded &&
           PickerTabSearchKeyMatches(
               cache,modelGeneration,normalizedQuery);
}

inline bool BeginPickerTabSearchAttempt(
        PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(attemptId==0 || modelGeneration==0 || normalizedQuery.empty() ||
       (cache.pending && cache.attemptId==attemptId))
        return false;
    try {
        const bool retrying=!cache.pending && cache.retryNeeded &&
            PickerTabSearchKeyMatches(
                cache,modelGeneration,normalizedQuery);
        if(retrying && cache.retryAttempts>=4) return false;
        std::wstring staged=normalizedQuery;
        cache.attemptId=attemptId;
        cache.modelGeneration=modelGeneration;
        cache.query.swap(staged);
        cache.ready=false;
        cache.pending=true;
        cache.retryNeeded=false;
        cache.routeFreed=false;
        cache.retryPosted=false;
        cache.retryDeliveryPending=false;
        cache.blockedAppMask=0;
        cache.blockedOnAnyRoute=false;
        if(retrying) ++cache.retryAttempts;
        else cache.retryAttempts=0;
        return true;
    } catch(...) { return false; }
}

inline bool MarkPickerTabSearchRetryNeeded(
        PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,const std::wstring& normalizedQuery,
        const std::string& app,
        PickerTabSearchRetryTrigger trigger=
            PickerTabSearchRetryTrigger::ExactAppRoute) noexcept {
    if(!PickerTabSearchAttemptMatches(
            cache,attemptId,modelGeneration,normalizedQuery)) return false;
    cache.retryNeeded=true;
    cache.ready=false;
    if(trigger==PickerTabSearchRetryTrigger::ExactAppRoute)
        cache.blockedAppMask|=PickerTabSearchAppBit(app);
    else if(trigger==PickerTabSearchRetryTrigger::AnyRoute)
        cache.blockedOnAnyRoute=true;
    else
        cache.routeFreed=true;
    return true;
}

inline bool CompletePickerTabSearchAttempt(
        PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(!PickerTabSearchAttemptMatches(
            cache,attemptId,modelGeneration,normalizedQuery)) return false;
    cache.pending=false;
    cache.ready=!cache.retryNeeded;
    if(cache.ready){
        cache.routeFreed=false;
        cache.retryPosted=false;
        cache.retryDeliveryPending=false;
        cache.blockedAppMask=0;
        cache.blockedOnAnyRoute=false;
    }
    return true;
}

inline void NotePickerTabSearchRouteFreed(
        PickerTabSearchCacheState& cache,const std::string& app) noexcept {
    if(!cache.query.empty() &&
       (cache.blockedOnAnyRoute ||
        (cache.blockedAppMask&PickerTabSearchAppBit(app))!=0))
        cache.routeFreed=true;
}

inline bool PickerTabSearchRetryPostNeeded(
        const PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return cache.retryNeeded && !cache.pending && cache.routeFreed &&
           !cache.retryPosted && cache.retryAttempts<4 &&
           PickerTabSearchKeyMatches(
               cache,modelGeneration,normalizedQuery);
}

inline bool MarkPickerTabSearchRetryPosted(
        PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(!PickerTabSearchRetryPostNeeded(
            cache,modelGeneration,normalizedQuery)) return false;
    cache.retryPosted=true;
    cache.retryDeliveryPending=false;
    return true;
}

inline bool MarkPickerTabSearchRetryDeliveryFailed(
        PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(!PickerTabSearchRetryPostNeeded(
            cache,modelGeneration,normalizedQuery)) return false;
    cache.retryDeliveryPending=true;
    return true;
}

inline bool PickerTabSearchRetryDeliveryKickNeeded(
        const PickerTabSearchCacheState& cache,uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return cache.retryDeliveryPending &&
           PickerTabSearchRetryPostNeeded(
               cache,modelGeneration,normalizedQuery);
}

inline bool PickerTabSearchRetryDeliveryReadyWhenIdle(
        const PickerTabSearchCacheState& cache,bool controlledTransition,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    return !controlledTransition &&
           PickerTabSearchRetryDeliveryKickNeeded(
               cache,modelGeneration,normalizedQuery);
}

inline bool AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        PickerTabSearchCacheState& cache,bool controlledTransition,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(controlledTransition || !cache.retryPosted || cache.pending ||
       !cache.retryNeeded || !cache.routeFreed || cache.retryAttempts>=4 ||
       !PickerTabSearchKeyMatches(
           cache,modelGeneration,normalizedQuery)) return false;
    cache.retryPosted=false;
    cache.retryDeliveryPending=true;
    return true;
}

inline bool RestorePickerTabSearchRetryAfterEnsureFailure(
        PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,
        const std::wstring& normalizedQuery) noexcept {
    if(attemptId==0 || cache.attemptId!=attemptId ||
       !PickerTabSearchKeyMatches(
           cache,modelGeneration,normalizedQuery)) return false;
    cache.ready=false;
    cache.pending=false;
    cache.retryNeeded=true;
    cache.routeFreed=true;
    cache.retryPosted=false;
    cache.retryDeliveryPending=true;
    cache.blockedAppMask=0;
    cache.blockedOnAnyRoute=false;
    return true;
}

enum class PickerTabSearchEnsureOutcome {
    AttemptCommitted, RetryPreserved
};

template<class Cleanup>
inline bool CleanupPickerTabSearchPublishedOperation(
        bool published,Cleanup&& cleanup) noexcept {
    if(!published) return false;
    try {
        cleanup();
        return true;
    } catch(...) { return false; }
}

template<class Publish,class Dispatch,class Cleanup>
inline PickerTabSearchEnsureOutcome RunPickerTabSearchEnsureAttempt(
        PickerTabSearchCacheState& cache,uint64_t attemptId,
        uint64_t modelGeneration,const std::wstring& normalizedQuery,
        Publish&& publish,Dispatch&& dispatch,Cleanup&& cleanup) noexcept {
    bool begun=false;
    const auto cleanupNoThrow=[&]() noexcept {
        try { cleanup(); } catch(...) {}
    };
    try {
        if(!publish()){
            cleanupNoThrow();
            return PickerTabSearchEnsureOutcome::RetryPreserved;
        }
        if(!BeginPickerTabSearchAttempt(
                cache,attemptId,modelGeneration,normalizedQuery)){
            cleanupNoThrow();
            return PickerTabSearchEnsureOutcome::RetryPreserved;
        }
        begun=true;
        if(dispatch())
            return PickerTabSearchEnsureOutcome::AttemptCommitted;
    } catch(...) {}
    cleanupNoThrow();
    if(begun)
        RestorePickerTabSearchRetryAfterEnsureFailure(
            cache,attemptId,modelGeneration,normalizedQuery);
    return PickerTabSearchEnsureOutcome::RetryPreserved;
}

inline void InvalidatePickerTabSearchCache(
        PickerTabSearchCacheState& cache,uint64_t modelGeneration) noexcept {
    cache.modelGeneration=modelGeneration;
    cache.attemptId=0;
    cache.ready=false;
    cache.pending=false;
    cache.retryNeeded=false;
    cache.routeFreed=false;
    cache.retryPosted=false;
    cache.retryDeliveryPending=false;
    cache.retryAttempts=0;
    cache.blockedAppMask=0;
    cache.blockedOnAnyRoute=false;
    cache.query.clear();
}

inline bool PickerUiActionAllowed(const PickerState& state,
                                  PickerUiAction action) noexcept {
    return action==PickerUiAction::TrayExit || !state.controlledTransition();
}

inline PickerCloseRoute RoutePickerClose(const PickerState& state) noexcept {
    return state.controlledTransition()
        ? PickerCloseRoute::Reject
        : PickerCloseRoute::Hide;
}

enum class PickerShutdownRoute { Proceed, CancelThenProceed };

inline PickerShutdownRoute RoutePickerShutdown(
        const PickerState& state) noexcept {
    return state.controlledTransition()
        ? PickerShutdownRoute::CancelThenProceed
        : PickerShutdownRoute::Proceed;
}

template<class Cancel,class Pump>
inline bool RunPickerShutdownDrain(PickerState& state,Cancel&& cancel,
                                   Pump&& pump) noexcept {
    if(!state.controlledTransition()) return true;
    try { cancel(); } catch(...) {}
    for(unsigned pass=0;pass<4 && state.controlledTransition();++pass){
        try { pump(); } catch(...) {}
    }
    return !state.controlledTransition();
}

inline PickerEffect PickerNoEffect() noexcept {
    return PickerEffect{};
}

inline bool DiscardPickerUnissuedEffectForCancel(
        PickerEffect& scheduled,bool& hasScheduled,
        const PickerTransition& transition) noexcept {
    if(!hasScheduled || scheduled.kind==PickerEffectKind::None ||
       transition.dismissed || scheduled.kind==PickerEffectKind::Hide ||
       scheduled.generation!=transition.generation ||
       scheduled.effectSerial!=transition.effectSerial ||
       scheduled.kind!=transition.pendingEffect ||
       (transition.phase==PickerPhase::SaveExactTarget &&
        transition.commitCutoffReached) ||
       transition.phase==PickerPhase::RefreshModel) return false;
    scheduled=PickerEffect{};
    hasScheduled=false;
    return true;
}

inline bool PickerReadMatches(PickerReadValidity validity,
                              const GUID& actual,
                              const GUID& expected) noexcept {
    return validity==PickerReadValidity::Valid &&
           !GuidIsZero(actual) && !GuidIsZero(expected) &&
           GuidEq(actual,expected);
}

inline bool PickerForwardSwitchInvocationAllowed(
        PickerIdentityValidity identity,bool desktopReady) noexcept {
    return desktopReady && identity==PickerIdentityValidity::Match;
}

struct PickerForegroundHandoffPlan {
    bool focusShell=false;
    bool attachDesktop=false;
    bool attachForeground=false;
};

inline PickerForegroundHandoffPlan PlanPickerForegroundHandoff(
        bool shellFound,uint32_t desktopThread,
        uint32_t foregroundThread,uint32_t currentThread) noexcept {
    PickerForegroundHandoffPlan plan;
    if(!shellFound || desktopThread==0 || currentThread==0) return plan;
    plan.focusShell=true;
    plan.attachDesktop=desktopThread!=currentThread;
    plan.attachForeground=foregroundThread!=0 &&
        foregroundThread!=currentThread &&
        foregroundThread!=desktopThread;
    return plan;
}

inline bool PickerMouseControlHeld(WPARAM buttonState) noexcept {
    return (buttonState&MK_CONTROL)!=0;
}

inline GUID PickerEffectDesktop(const PickerTransition& transition,
                                PickerEffectKind kind) noexcept {
    switch(kind){
    case PickerEffectKind::MoveTarget:
        return transition.phase==PickerPhase::RollbackTargetIssue
            ? transition.targetOrigin : transition.destination;
    case PickerEffectKind::MovePopup:
        return transition.phase==PickerPhase::RollbackPopupIssue
            ? transition.currentOrigin : transition.destination;
    case PickerEffectKind::SwitchDesktop:
        return transition.phase==PickerPhase::RollbackSwitchIssue
            ? transition.currentOrigin : transition.destination;
    default:
        return GUID{};
    }
}

inline PickerEffect EmitPickerEffect(PickerState& state,
                                     PickerEffectKind kind) noexcept {
    if(kind==PickerEffectKind::None ||
       state.transition.pendingEffect!=PickerEffectKind::None)
        return PickerNoEffect();
    if(state.transition.effectSerial==
        (std::numeric_limits<uint64_t>::max)()){
        state.transition.failed=true;
        try { state.transition.diagnostic=
            L"The picker transition effect counter was exhausted."; }
        catch(...) { state.transition.diagnostic.clear(); }
        state.transition.suppressFocus=true;
        state.transition.terminalAcknowledged=true;
        return PickerNoEffect();
    }
    ++state.transition.effectSerial;
    if(state.transition.effectSerial==0) ++state.transition.effectSerial;
    state.transition.pendingEffect=kind;
    PickerEffect effect;
    effect.kind=kind;
    effect.generation=state.transition.generation;
    effect.effectSerial=state.transition.effectSerial;
    effect.desktop=PickerEffectDesktop(state.transition,kind);
    effect.hideDisposition=kind==PickerEffectKind::Hide
        ? state.transition.pendingHideDisposition
        : PickerHideDisposition::None;
    return effect;
}

inline bool PickerObservationAcknowledges(
        const PickerTransition& transition,
        const PickerObservation& observation) noexcept {
    if(observation.generation==0 ||
       observation.generation!=transition.generation ||
       transition.pendingEffect==PickerEffectKind::None ||
       observation.effectKind!=transition.pendingEffect ||
       observation.effectSerial==0 ||
       observation.effectSerial!=transition.effectSerial)
        return false;
    PickerEvent expected=PickerEvent::EffectCompleted;
    switch(transition.pendingEffect){
    case PickerEffectKind::MoveTarget:
    case PickerEffectKind::MovePopup:
    case PickerEffectKind::SwitchDesktop:
        expected=PickerEvent::ApiCompleted;
        break;
    case PickerEffectKind::ReadTarget:
    case PickerEffectKind::ReadPopup:
    case PickerEffectKind::ReadCurrent:
        expected=PickerEvent::ReadbackCompleted;
        break;
    case PickerEffectKind::ValidateTarget:
    case PickerEffectKind::PublishVisualAssignment:
    case PickerEffectKind::SaveExactTarget:
    case PickerEffectKind::Refresh:
    case PickerEffectKind::ShowAndFocus:
    case PickerEffectKind::Hide:
    case PickerEffectKind::ReportFailure:
        expected=PickerEvent::EffectCompleted;
        break;
    case PickerEffectKind::None:
        return false;
    }
    return observation.event==expected;
}

inline void PickerAppendDiagnostic(PickerTransition& transition,
                                   const wchar_t* text) noexcept {
    if(!text || !*text) return;
    try {
        if(!transition.diagnostic.empty()) transition.diagnostic+=L" ";
        transition.diagnostic+=text;
    } catch(...) {
        transition.diagnostic.clear();
    }
}

inline bool PickerIdentityMatches(PickerIdentityValidity identity) noexcept {
    return identity==PickerIdentityValidity::Match;
}

inline bool PickerIdentityIsUnusable(
        PickerIdentityValidity identity) noexcept {
    return identity!=PickerIdentityValidity::Match;
}

inline void PickerInvalidateObservedTarget(
        PickerTransition& transition) noexcept {
    transition.targetIdentityUnusable=true;
    transition.observedTargetValidity=PickerReadValidity::Unavailable;
    transition.observedTargetDesktop=GUID{};
}

inline void PickerAcknowledgeTerminal(
        PickerTransition& transition) noexcept {
    transition.pendingEffect=PickerEffectKind::None;
    transition.hidePending=false;
    transition.terminalAcknowledged=true;
}

inline bool FinalizePickerTransition(PickerState& state) noexcept {
    PickerTransition& transition=state.transition;
    if(!transition.terminalAcknowledged ||
       transition.pendingEffect!=PickerEffectKind::None)
        return false;
    const uint64_t generation=transition.generation;
    const uint64_t effectSerial=transition.effectSerial;
    PickerTransition idle;
    idle.generation=generation;
    idle.effectSerial=effectSerial;
    transition.swap(idle);
    return true;
}

inline PickerEffect PickerStartRefresh(PickerState& state) noexcept {
    state.transition.phase=PickerPhase::RefreshModel;
    return EmitPickerEffect(state,PickerEffectKind::Refresh);
}

inline PickerEffect PickerIssueTarget(PickerState& state,
                                      bool rollback) noexcept {
    PickerTransition& transition=state.transition;
    int& attempts=rollback ? transition.rollbackTargetAttempts
                           : transition.forwardTargetAttempts;
    if(attempts>=4) return PickerNoEffect();
    ++attempts;
    transition.targetUnresolvedBeforeIssue=transition.targetMayHaveMoved;
    transition.targetMayHaveMoved=true;
    transition.phase=rollback ? PickerPhase::RollbackTargetIssue
                              : PickerPhase::TargetIssue;
    return EmitPickerEffect(state,PickerEffectKind::MoveTarget);
}

inline PickerEffect PickerIssuePopup(PickerState& state,
                                     bool rollback) noexcept {
    PickerTransition& transition=state.transition;
    if(transition.popupRoute!=PickerPopupRoute::Managed)
        return PickerNoEffect();
    int& attempts=rollback ? transition.rollbackPopupAttempts
                           : transition.forwardPopupAttempts;
    if(attempts>=4) return PickerNoEffect();
    ++attempts;
    transition.popupUnresolvedBeforeIssue=transition.popupMayHaveMoved;
    transition.popupMayHaveMoved=true;
    transition.phase=rollback ? PickerPhase::RollbackPopupIssue
                              : PickerPhase::PopupIssue;
    return EmitPickerEffect(state,PickerEffectKind::MovePopup);
}

inline PickerEffect PickerIssueSwitch(PickerState& state,
                                      bool rollback) noexcept {
    PickerTransition& transition=state.transition;
    int& attempts=rollback ? transition.rollbackSwitchAttempts
                           : transition.forwardSwitchAttempts;
    if(attempts>=4) return PickerNoEffect();
    ++attempts;
    transition.switchUnresolvedBeforeIssue=transition.switchMayHaveChanged;
    transition.switchMayHaveChanged=true;
    transition.phase=rollback ? PickerPhase::RollbackSwitchIssue
                              : PickerPhase::SwitchIssue;
    return EmitPickerEffect(state,PickerEffectKind::SwitchDesktop);
}

inline PickerEffect PickerContinueRollbackAfterPopup(
        PickerState& state) noexcept {
    if(state.transition.switchMayHaveChanged)
        return PickerIssueSwitch(state,true);
    if(state.transition.rollbackVerificationRequired){
        state.transition.phase=PickerPhase::OriginVerify;
        return EmitPickerEffect(state,PickerEffectKind::ReadCurrent);
    }
    return PickerStartRefresh(state);
}

inline PickerEffect PickerContinueRollbackAfterTarget(
        PickerState& state) noexcept {
    if(state.transition.mode==PickerTransitionMode::RowMoveOnly)
        return PickerStartRefresh(state);
    if(state.transition.popupRoute==PickerPopupRoute::StickyUnmanaged)
        return PickerContinueRollbackAfterPopup(state);
    if(state.transition.popupMayHaveMoved)
        return PickerIssuePopup(state,true);
    if(state.transition.rollbackVerificationRequired){
        state.transition.phase=PickerPhase::RollbackPopupVerify;
        return EmitPickerEffect(state,PickerEffectKind::ReadPopup);
    }
    return PickerContinueRollbackAfterPopup(state);
}

inline PickerEffect PickerBeginRollback(PickerState& state,
                                        const wchar_t* diagnostic) noexcept {
    PickerTransition& transition=state.transition;
    transition.failed=true;
    PickerAppendDiagnostic(transition,diagnostic);
    const PickerTransitionPolicy policy=PickerPolicyFor(transition.mode);
    if(policy.publishesVisual){
        transition.targetMayHaveMoved=false;
        transition.targetUnresolvedBeforeIssue=false;
        transition.rollbackTargetAttempts=0;
        transition.rollbackVerificationRequired=
            transition.popupMayHaveMoved ||
            transition.switchMayHaveChanged;
        if(transition.mode==PickerTransitionMode::VisualOnly){
            PickerAcknowledgeTerminal(transition);
            return PickerNoEffect();
        }
        return PickerContinueRollbackAfterTarget(state);
    }
    if(transition.mode==PickerTransitionMode::RowMoveOnly){
        transition.rollbackVerificationRequired=
            transition.rollbackVerificationRequired ||
            transition.targetMayHaveMoved;
        if(!transition.rollbackVerificationRequired)
            return PickerStartRefresh(state);
        if(transition.targetIdentityUnusable){
            PickerAppendDiagnostic(
                transition,
                L"The target identity cannot be safely rolled back.");
            return PickerStartRefresh(state);
        }
        if(transition.targetMayHaveMoved)
            return PickerIssueTarget(state,true);
        transition.phase=PickerPhase::RollbackTargetVerify;
        return EmitPickerEffect(state,PickerEffectKind::ReadTarget);
    }
    transition.rollbackVerificationRequired=
        transition.rollbackVerificationRequired ||
        transition.targetMayHaveMoved || transition.popupMayHaveMoved ||
        transition.switchMayHaveChanged;
    if(!transition.rollbackVerificationRequired)
        return PickerStartRefresh(state);
    if(transition.targetMayHaveMoved && !transition.targetIdentityUnusable)
        return PickerIssueTarget(state,true);
    if(transition.targetMayHaveMoved && transition.targetIdentityUnusable)
        PickerAppendDiagnostic(
            transition,L"The target identity cannot be safely rolled back.");
    if(!transition.targetIdentityUnusable){
        transition.phase=PickerPhase::RollbackTargetVerify;
        return EmitPickerEffect(state,PickerEffectKind::ReadTarget);
    }
    return PickerContinueRollbackAfterTarget(state);
}

inline PickerEffect PickerForwardFailed(PickerState& state,
                                        const wchar_t* diagnostic) noexcept {
    return PickerBeginRollback(state,diagnostic);
}

inline PickerEffect PickerStartVisualPublication(
        PickerState& state) noexcept {
    state.transition.phase=PickerPhase::PublishVisualAssignment;
    return EmitPickerEffect(
        state,PickerEffectKind::PublishVisualAssignment);
}

inline PickerEffect PickerContinueVisualForwardToDestination(
        PickerState& state) noexcept {
    PickerTransition& transition=state.transition;
    const bool popupReady=
        transition.popupRoute==PickerPopupRoute::StickyUnmanaged ||
        PickerReadMatches(transition.observedPopupValidity,
            transition.observedPopupDesktop,transition.destination);
    if(PickerReadMatches(transition.observedCurrentValidity,
            transition.observedCurrentDesktop,transition.destination) &&
       popupReady)
        return PickerStartVisualPublication(state);
    if(transition.forwardSwitchAttempts>=4)
        return PickerForwardFailed(
            state,L"The desktop switch attempt budget was exhausted.");
    return PickerIssueSwitch(state,false);
}

inline PickerEffect PickerContinueForwardToDestination(
        PickerState& state) noexcept {
    PickerTransition& transition=state.transition;
    const bool popupReady=
        transition.popupRoute==PickerPopupRoute::StickyUnmanaged ||
        PickerReadMatches(transition.observedPopupValidity,
            transition.observedPopupDesktop,transition.destination);
    if(PickerReadMatches(transition.observedCurrentValidity,
            transition.observedCurrentDesktop,transition.destination) &&
       popupReady){
        transition.phase=PickerPhase::SaveExactTarget;
        transition.commitCutoffReached=true;
        return EmitPickerEffect(state,PickerEffectKind::SaveExactTarget);
    }
    if(transition.forwardSwitchAttempts>=4)
        return PickerForwardFailed(
            state,L"The desktop switch attempt budget was exhausted.");
    return PickerIssueSwitch(state,false);
}

inline PickerEffect PickerResumeAfterHide(PickerState& state) noexcept {
    PickerTransition& transition=state.transition;
    switch(transition.resumeAfterHide){
    case PickerPhase::RefreshModel:
    case PickerPhase::FailureReport:
    case PickerPhase::FocusRestore:
        PickerAcknowledgeTerminal(transition);
        return PickerNoEffect();
    case PickerPhase::RollbackTargetIssue:
    case PickerPhase::RollbackTargetVerify:
        if(!transition.targetIdentityUnusable &&
           (transition.targetMayHaveMoved ||
            transition.rollbackVerificationRequired)){
            transition.phase=PickerPhase::RollbackTargetVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadTarget);
        }
        return PickerContinueRollbackAfterTarget(state);
    case PickerPhase::RollbackPopupIssue:
    case PickerPhase::RollbackPopupVerify:
        if(transition.popupRoute==PickerPopupRoute::StickyUnmanaged)
            return PickerContinueRollbackAfterPopup(state);
        if(transition.popupMayHaveMoved ||
           transition.rollbackVerificationRequired){
            transition.phase=PickerPhase::RollbackPopupVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadPopup);
        }
        return PickerContinueRollbackAfterPopup(state);
    case PickerPhase::RollbackSwitchIssue:
    case PickerPhase::OriginVerify:
        if(transition.switchMayHaveChanged ||
           transition.rollbackVerificationRequired){
            transition.phase=PickerPhase::OriginVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadCurrent);
        }
        return PickerStartRefresh(state);
    default:
        break;
    }
    return PickerBeginRollback(state,nullptr);
}

inline PickerEffect AdvancePickerTransition(
        PickerState& state,const PickerObservation& observation) noexcept {
    PickerTransition& transition=state.transition;
    if(observation.generation==0 ||
       observation.generation!=transition.generation)
        return PickerNoEffect();

    if(transition.terminalAcknowledged)
        return PickerNoEffect();

    if(observation.event==PickerEvent::Begin){
        if(!PickerTransitionBeginPreconditions(state,transition))
            return PickerNoEffect();
        transition.cancelRequested=false;
        transition.dismissed=false;
        transition.failed=false;
        transition.hidePending=false;
        transition.hideCompleted=false;
        transition.failureReported=false;
        transition.focusFailureTerminal=false;
        transition.targetIdentityUnusable=false;
        transition.commitCutoffReached=false;
        transition.suppressFocus=false;
        transition.terminalAcknowledged=false;
        transition.pendingHideDisposition=
            PickerHideDisposition::None;
        transition.forwardTargetAttempts=0;
        transition.forwardPopupAttempts=0;
        transition.forwardSwitchAttempts=0;
        transition.rollbackTargetAttempts=0;
        transition.rollbackPopupAttempts=0;
        transition.rollbackSwitchAttempts=0;
        transition.focusAttempts=0;
        transition.targetMayHaveMoved=false;
        transition.popupMayHaveMoved=false;
        transition.switchMayHaveChanged=false;
        transition.rollbackVerificationRequired=false;
        transition.targetUnresolvedBeforeIssue=false;
        transition.popupUnresolvedBeforeIssue=false;
        transition.switchUnresolvedBeforeIssue=false;
        transition.postSwitchPopupRepair=false;
        transition.observedTargetDesktop=transition.targetOrigin;
        transition.observedPopupDesktop=
            transition.popupRoute==PickerPopupRoute::Managed
                ? transition.popupOrigin : GUID{};
        transition.observedCurrentDesktop=transition.currentOrigin;
        transition.observedTargetValidity=
            GuidIsZero(transition.targetOrigin)
                ? PickerReadValidity::Unavailable
                : PickerReadValidity::Valid;
        transition.observedPopupValidity=
            transition.popupRoute==PickerPopupRoute::Managed
                ? PickerReadValidity::Valid
                : PickerReadValidity::Unavailable;
        transition.observedCurrentValidity=
            GuidIsZero(transition.currentOrigin)
                ? PickerReadValidity::Unavailable
                : PickerReadValidity::Valid;
        transition.diagnostic.clear();
        if(PickerPolicyFor(transition.mode).publishesVisual){
            transition.phase=PickerPhase::IdentityVerifyBeforePopup;
            return EmitPickerEffect(
                state,PickerEffectKind::ValidateTarget);
        }
        return PickerIssueTarget(state,false);
    }

    if(observation.event==PickerEvent::CancelRequested){
        if(transition.phase==PickerPhase::Idle || transition.dismissed)
            return PickerNoEffect();
        if(transition.mode==PickerTransitionMode::VisualOnly){
            if(transition.cancelRequested)
                return PickerNoEffect();
            transition.cancelRequested=true;
            transition.failed=true;
            PickerAppendDiagnostic(
                transition,L"The visual assignment was cancelled.");
            if(observation.unissuedEffectCancelled){
                transition.pendingEffect=PickerEffectKind::None;
                PickerAcknowledgeTerminal(transition);
            } else if(transition.pendingEffect==PickerEffectKind::None){
                PickerAcknowledgeTerminal(transition);
            }
            return PickerNoEffect();
        }
        if(transition.mode==PickerTransitionMode::RowMoveOnly){
            if(transition.cancelRequested)
                return PickerNoEffect();
            transition.cancelRequested=true;
            transition.failed=true;
            PickerAppendDiagnostic(
                transition,L"The picker move was cancelled.");
            if(transition.phase==PickerPhase::SaveExactTarget ||
               transition.phase==PickerPhase::RefreshModel)
                return PickerNoEffect();
            if(observation.unissuedEffectCancelled){
                if(transition.pendingEffect==PickerEffectKind::MoveTarget)
                    transition.targetMayHaveMoved=
                        transition.targetUnresolvedBeforeIssue;
                transition.pendingEffect=PickerEffectKind::None;
                return PickerBeginRollback(state,nullptr);
            }
            if(transition.pendingEffect!=PickerEffectKind::None)
                return PickerNoEffect();
            return PickerBeginRollback(state,nullptr);
        }
        transition.resumeAfterHide=transition.phase;
        transition.cancelRequested=true;
        transition.dismissed=true;
        transition.failed=true;
        transition.pendingHideDisposition=
            PickerHideDisposition::DismissSession;
        PickerAppendDiagnostic(transition,L"The picker move was cancelled.");
        if(observation.unissuedEffectCancelled){
            if(transition.pendingEffect==PickerEffectKind::MoveTarget)
                transition.targetMayHaveMoved=
                    transition.targetUnresolvedBeforeIssue;
            else if(transition.pendingEffect==PickerEffectKind::MovePopup)
                transition.popupMayHaveMoved=
                    transition.popupUnresolvedBeforeIssue;
            else if(transition.pendingEffect==PickerEffectKind::SwitchDesktop)
                transition.switchMayHaveChanged=
                    transition.switchUnresolvedBeforeIssue;
        }
        if(((transition.phase==PickerPhase::SaveExactTarget &&
             transition.commitCutoffReached &&
             transition.pendingEffect==PickerEffectKind::SaveExactTarget) ||
            (transition.phase==PickerPhase::RefreshModel &&
             transition.pendingEffect==PickerEffectKind::Refresh) ||
            (transition.phase==PickerPhase::PublishVisualAssignment &&
             transition.pendingEffect==
                 PickerEffectKind::PublishVisualAssignment &&
             !observation.unissuedEffectCancelled)))
            return PickerNoEffect();
        transition.pendingEffect=PickerEffectKind::None;
        transition.hidePending=true;
        return EmitPickerEffect(state,PickerEffectKind::Hide);
    }

    if(!PickerObservationAcknowledges(transition,observation))
        return PickerNoEffect();
    const PickerEffectKind acknowledged=transition.pendingEffect;
    transition.pendingEffect=PickerEffectKind::None;

    if(acknowledged==PickerEffectKind::Hide){
        if(!transition.hidePending) return PickerNoEffect();
        transition.hidePending=false;
        transition.hideCompleted=true;
        if(transition.commitCutoffReached &&
           transition.resumeAfterHide!=PickerPhase::RefreshModel &&
           transition.resumeAfterHide!=PickerPhase::FailureReport &&
           transition.resumeAfterHide!=PickerPhase::FocusRestore)
            return PickerStartRefresh(state);
        return PickerResumeAfterHide(state);
    }

    switch(transition.phase){
    case PickerPhase::TargetIssue:
        if(acknowledged==PickerEffectKind::MoveTarget &&
           observation.event==PickerEvent::ApiCompleted){
            if(observation.apiInvoked)
                transition.rollbackVerificationRequired=true;
            if(!observation.apiInvoked ||
               !PickerIdentityMatches(observation.identity)){
                if(!observation.apiInvoked)
                    transition.targetMayHaveMoved=
                        transition.targetUnresolvedBeforeIssue;
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                return PickerForwardFailed(
                    state,L"The target move was not safely invoked.");
            }
            if(transition.mode==PickerTransitionMode::RowMoveOnly &&
               transition.cancelRequested)
                return PickerBeginRollback(state,nullptr);
            transition.phase=PickerPhase::TargetVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadTarget);
        }
        break;

    case PickerPhase::TargetVerify:
        if(acknowledged==PickerEffectKind::ReadTarget &&
           observation.event==PickerEvent::ReadbackCompleted){
            if(!PickerIdentityMatches(observation.identity)){
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                return PickerForwardFailed(
                    state,L"The target identity changed during verification.");
            }
            transition.observedTargetValidity=observation.targetRead;
            transition.observedTargetDesktop=observation.actualTargetDesktop;
            if(transition.mode==PickerTransitionMode::RowMoveOnly &&
               transition.cancelRequested){
                transition.targetMayHaveMoved=
                    !PickerReadMatches(
                        observation.targetRead,
                        observation.actualTargetDesktop,
                        transition.targetOrigin);
                if(!transition.targetMayHaveMoved){
                    transition.rollbackVerificationRequired=false;
                    return PickerStartRefresh(state);
                }
                return PickerBeginRollback(state,nullptr);
            }
            if(PickerReadMatches(observation.targetRead,
                    observation.actualTargetDesktop,transition.destination)){
                if(transition.mode==PickerTransitionMode::RowMoveOnly){
                    transition.phase=PickerPhase::SaveExactTarget;
                    transition.commitCutoffReached=true;
                    return EmitPickerEffect(
                        state,PickerEffectKind::SaveExactTarget);
                }
                transition.phase=PickerPhase::IdentityVerifyBeforePopup;
                return EmitPickerEffect(state,PickerEffectKind::ValidateTarget);
            }
            transition.targetMayHaveMoved=
                !PickerReadMatches(observation.targetRead,
                    observation.actualTargetDesktop,transition.targetOrigin);
            if(transition.forwardTargetAttempts<4){
                return PickerIssueTarget(state,false);
            }
            return PickerForwardFailed(
                state,L"The target did not reach the selected desktop.");
        }
        break;

    case PickerPhase::IdentityVerifyBeforePopup:
        if(acknowledged==PickerEffectKind::ValidateTarget &&
           observation.event==PickerEvent::EffectCompleted){
            if(!PickerIdentityMatches(observation.identity)){
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                return PickerForwardFailed(
                    state,L"The target identity changed before moving the picker.");
            }
            if(transition.mode==PickerTransitionMode::VisualOnly){
                if(transition.cancelRequested){
                    PickerAcknowledgeTerminal(transition);
                    return PickerNoEffect();
                }
                return PickerStartVisualPublication(state);
            }
            if(transition.mode==PickerTransitionMode::VisualAndFollow){
                if(transition.popupRoute==
                        PickerPopupRoute::StickyUnmanaged){
                    transition.phase=
                        PickerPhase::IdentityVerifyBeforeSwitch;
                    return PickerContinueVisualForwardToDestination(
                        state);
                }
                return PickerIssuePopup(state,false);
            }
            if(transition.popupRoute==PickerPopupRoute::StickyUnmanaged){
                transition.phase=PickerPhase::IdentityVerifyBeforeSwitch;
                return PickerContinueForwardToDestination(state);
            }
            return PickerIssuePopup(state,false);
        }
        break;

    case PickerPhase::PopupIssue:
        if(acknowledged==PickerEffectKind::MovePopup &&
           observation.event==PickerEvent::ApiCompleted){
            if(observation.apiInvoked)
                transition.rollbackVerificationRequired=true;
            if(!observation.apiInvoked){
                transition.popupMayHaveMoved=
                    transition.popupUnresolvedBeforeIssue;
                return PickerForwardFailed(
                    state,L"The picker move was not invoked.");
            }
            transition.phase=PickerPhase::PopupVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadPopup);
        }
        break;

    case PickerPhase::PopupVerify:
        if(acknowledged==PickerEffectKind::ReadPopup &&
           observation.event==PickerEvent::ReadbackCompleted){
            transition.observedPopupValidity=observation.popupRead;
            transition.observedPopupDesktop=observation.actualPopupDesktop;
            if(PickerReadMatches(observation.popupRead,
                    observation.actualPopupDesktop,transition.destination)){
                if(transition.postSwitchPopupRepair){
                    transition.postSwitchPopupRepair=false;
                    transition.observedCurrentValidity=
                        PickerReadValidity::Unknown;
                    transition.observedCurrentDesktop=GUID{};
                    transition.phase=PickerPhase::DestinationVerify;
                    return EmitPickerEffect(
                        state,PickerEffectKind::ReadCurrent);
                }
                transition.phase=PickerPhase::IdentityVerifyBeforeSwitch;
                return EmitPickerEffect(state,PickerEffectKind::ValidateTarget);
            }
            transition.popupMayHaveMoved=
                !PickerReadMatches(observation.popupRead,
                    observation.actualPopupDesktop,transition.currentOrigin);
            if(transition.forwardPopupAttempts<4){
                transition.phase=PickerPhase::IdentityVerifyBeforePopup;
                return EmitPickerEffect(state,PickerEffectKind::ValidateTarget);
            }
            return PickerForwardFailed(
                state,L"The picker did not reach the selected desktop.");
        }
        break;

    case PickerPhase::IdentityVerifyBeforeSwitch:
        if(acknowledged==PickerEffectKind::ValidateTarget &&
           observation.event==PickerEvent::EffectCompleted){
            if(!PickerIdentityMatches(observation.identity)){
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                return PickerForwardFailed(
                    state,L"The target identity changed before switching desktops.");
            }
            if(transition.mode==PickerTransitionMode::VisualAndFollow)
                return PickerContinueVisualForwardToDestination(state);
            return PickerContinueForwardToDestination(state);
        }
        break;

    case PickerPhase::SwitchIssue:
        if(acknowledged==PickerEffectKind::SwitchDesktop &&
           observation.event==PickerEvent::ApiCompleted){
            if(observation.apiInvoked)
                transition.rollbackVerificationRequired=true;
            if(!observation.apiInvoked ||
               !PickerIdentityMatches(observation.identity)){
                transition.switchMayHaveChanged=
                    transition.switchUnresolvedBeforeIssue;
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                return PickerForwardFailed(
                    state,!PickerIdentityMatches(observation.identity)
                        ? L"The target identity changed before the desktop switch could be invoked."
                        : L"The desktop switch was not invoked.");
            }
            transition.phase=PickerPhase::DestinationVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadCurrent);
        }
        break;

    case PickerPhase::DestinationVerify:
        if(acknowledged==PickerEffectKind::ReadCurrent &&
           observation.event==PickerEvent::ReadbackCompleted){
            transition.observedCurrentValidity=observation.currentRead;
            transition.observedCurrentDesktop=observation.actualCurrentDesktop;
            if(PickerReadMatches(observation.currentRead,
                    observation.actualCurrentDesktop,transition.destination)){
                if(transition.popupRoute==
                        PickerPopupRoute::StickyUnmanaged){
                    if(transition.mode==
                            PickerTransitionMode::VisualAndFollow)
                        return PickerStartVisualPublication(state);
                    transition.phase=PickerPhase::SaveExactTarget;
                    transition.commitCutoffReached=true;
                    return EmitPickerEffect(
                        state,PickerEffectKind::SaveExactTarget);
                }
                return EmitPickerEffect(state,PickerEffectKind::ReadPopup);
            }
            transition.switchMayHaveChanged=!PickerReadMatches(
                observation.currentRead,observation.actualCurrentDesktop,
                transition.currentOrigin);
            if(transition.forwardSwitchAttempts<4){
                transition.phase=PickerPhase::IdentityVerifyBeforeSwitch;
                return EmitPickerEffect(state,PickerEffectKind::ValidateTarget);
            }
            return PickerForwardFailed(
                state,L"Windows did not remain on the selected desktop.");
        }
        if(acknowledged==PickerEffectKind::ReadPopup &&
           observation.event==PickerEvent::ReadbackCompleted){
            transition.observedPopupValidity=observation.popupRead;
            transition.observedPopupDesktop=observation.actualPopupDesktop;
            if(PickerReadMatches(observation.popupRead,
                    observation.actualPopupDesktop,transition.destination)){
                if(transition.mode==
                        PickerTransitionMode::VisualAndFollow)
                    return PickerStartVisualPublication(state);
                transition.phase=PickerPhase::SaveExactTarget;
                transition.commitCutoffReached=true;
                return EmitPickerEffect(
                    state,PickerEffectKind::SaveExactTarget);
            }
            transition.popupMayHaveMoved=!PickerReadMatches(
                observation.popupRead,observation.actualPopupDesktop,
                transition.currentOrigin);
            if(transition.forwardPopupAttempts<4){
                transition.postSwitchPopupRepair=true;
                transition.observedCurrentValidity=
                    PickerReadValidity::Unknown;
                transition.observedCurrentDesktop=GUID{};
                transition.phase=PickerPhase::IdentityVerifyBeforePopup;
                return EmitPickerEffect(state,PickerEffectKind::ValidateTarget);
            }
            return PickerForwardFailed(
                state,L"The picker left the selected desktop after the switch.");
        }
        break;

    case PickerPhase::PublishVisualAssignment:
        if(acknowledged==
                PickerEffectKind::PublishVisualAssignment &&
           observation.event==PickerEvent::EffectCompleted){
            if(!observation.apiAccepted){
                return PickerForwardFailed(
                    state,L"The visual assignment could not be published.");
            }
            if(transition.mode==PickerTransitionMode::VisualOnly){
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            if(transition.mode==PickerTransitionMode::VisualAndFollow){
                if(transition.cancelRequested && transition.dismissed){
                    transition.resumeAfterHide=
                        PickerPhase::PublishVisualAssignment;
                    transition.hidePending=true;
                    transition.pendingHideDisposition=
                        PickerHideDisposition::DismissSession;
                    return EmitPickerEffect(
                        state,PickerEffectKind::Hide);
                }
                transition.phase=PickerPhase::FocusRestore;
                ++transition.focusAttempts;
                return EmitPickerEffect(
                    state,PickerEffectKind::ShowAndFocus);
            }
            return PickerForwardFailed(
                state,L"The visual publication mode was invalid.");
        }
        break;

    case PickerPhase::RollbackTargetIssue:
        if(acknowledged==PickerEffectKind::MoveTarget &&
           observation.event==PickerEvent::ApiCompleted){
            if(!PickerIdentityMatches(observation.identity)){
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                PickerAppendDiagnostic(
                    transition,L"The target rollback could not be invoked.");
                return PickerContinueRollbackAfterTarget(state);
            }
            if(!observation.apiInvoked)
                PickerAppendDiagnostic(
                    transition,L"The target rollback call was unavailable; its desktop will be read back.");
            transition.phase=PickerPhase::RollbackTargetVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadTarget);
        }
        break;

    case PickerPhase::RollbackTargetVerify:
        if(acknowledged==PickerEffectKind::ReadTarget &&
           observation.event==PickerEvent::ReadbackCompleted){
            if(!PickerIdentityMatches(observation.identity)){
                if(PickerIdentityIsUnusable(observation.identity))
                    PickerInvalidateObservedTarget(transition);
                PickerAppendDiagnostic(
                    transition,L"The target identity was lost during rollback.");
                return PickerContinueRollbackAfterTarget(state);
            }
            transition.observedTargetValidity=observation.targetRead;
            transition.observedTargetDesktop=observation.actualTargetDesktop;
            if(PickerReadMatches(observation.targetRead,
                    observation.actualTargetDesktop,transition.targetOrigin)){
                transition.targetMayHaveMoved=false;
                return PickerContinueRollbackAfterTarget(state);
            }
            transition.targetMayHaveMoved=true;
            if(transition.rollbackTargetAttempts<4){
                return PickerIssueTarget(state,true);
            }
            PickerAppendDiagnostic(
                transition,L"The target remains displaced after rollback.");
            return PickerContinueRollbackAfterTarget(state);
        }
        break;

    case PickerPhase::RollbackPopupIssue:
        if(acknowledged==PickerEffectKind::MovePopup &&
           observation.event==PickerEvent::ApiCompleted){
            if(!observation.apiInvoked){
                PickerAppendDiagnostic(
                    transition,L"The picker rollback call was unavailable; its desktop will be read back.");
            }
            transition.phase=PickerPhase::RollbackPopupVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadPopup);
        }
        break;

    case PickerPhase::RollbackPopupVerify:
        if(acknowledged==PickerEffectKind::ReadPopup &&
           observation.event==PickerEvent::ReadbackCompleted){
            transition.observedPopupValidity=observation.popupRead;
            transition.observedPopupDesktop=observation.actualPopupDesktop;
            if(PickerReadMatches(observation.popupRead,
                    observation.actualPopupDesktop,transition.currentOrigin)){
                transition.popupMayHaveMoved=false;
                return PickerContinueRollbackAfterPopup(state);
            }
            transition.popupMayHaveMoved=true;
            if(transition.rollbackPopupAttempts<4){
                return PickerIssuePopup(state,true);
            }
            PickerAppendDiagnostic(
                transition,L"The picker remains displaced after rollback.");
            transition.suppressFocus=true;
            return PickerContinueRollbackAfterPopup(state);
        }
        break;

    case PickerPhase::RollbackSwitchIssue:
        if(acknowledged==PickerEffectKind::SwitchDesktop &&
           observation.event==PickerEvent::ApiCompleted){
            if(!observation.apiInvoked){
                PickerAppendDiagnostic(
                    transition,L"The desktop rollback call was unavailable; the current desktop will be read back.");
            }
            transition.phase=PickerPhase::OriginVerify;
            return EmitPickerEffect(state,PickerEffectKind::ReadCurrent);
        }
        break;

    case PickerPhase::OriginVerify:
        if(acknowledged==PickerEffectKind::ReadCurrent &&
           observation.event==PickerEvent::ReadbackCompleted){
            transition.observedCurrentValidity=observation.currentRead;
            transition.observedCurrentDesktop=observation.actualCurrentDesktop;
            if(PickerReadMatches(observation.currentRead,
                    observation.actualCurrentDesktop,transition.currentOrigin)){
                transition.switchMayHaveChanged=false;
                transition.phase=PickerPhase::RefreshModel;
                return EmitPickerEffect(state,PickerEffectKind::Refresh);
            }
            transition.switchMayHaveChanged=true;
            if(transition.rollbackSwitchAttempts<4){
                return PickerIssueSwitch(state,true);
            }
            PickerAppendDiagnostic(
                transition,L"Windows remains on a displaced desktop after rollback.");
            transition.suppressFocus=true;
            return PickerStartRefresh(state);
        }
        break;

    case PickerPhase::SaveExactTarget:
        if(acknowledged==PickerEffectKind::SaveExactTarget &&
           observation.event==PickerEvent::EffectCompleted){
            if(transition.mode==PickerTransitionMode::RowMoveOnly){
                const bool commits=PickerRowMoveSaveCommits(
                    observation.saveStatus,observation.saveFailure);
                if(observation.saveStatus==PopupSaveStatus::Failed){
                    transition.failed=true;
                    PickerAppendDiagnostic(
                        transition,PickerSaveFailureDiagnostic(
                            observation.saveFailure));
                }
                if(!PickerIdentityMatches(observation.identity)){
                    PickerInvalidateObservedTarget(transition);
                    transition.failed=true;
                    PickerAppendDiagnostic(
                        transition,PickerPostSaveIdentityDiagnostic(
                            observation.saveStatus,
                            observation.identity));
                }
                if(commits){
                    transition.commitCutoffReached=true;
                    return PickerStartRefresh(state);
                }
                transition.commitCutoffReached=false;
                return PickerBeginRollback(
                    state,L"The target assignment was not published.");
            }
            if(observation.saveStatus==PopupSaveStatus::Failed){
                transition.failed=true;
                PickerAppendDiagnostic(
                    transition,PickerSaveFailureDiagnostic(
                        observation.saveFailure));
            }
            if(!PickerIdentityMatches(observation.identity)){
                PickerInvalidateObservedTarget(transition);
                transition.failed=true;
                PickerAppendDiagnostic(
                    transition,PickerPostSaveIdentityDiagnostic(
                        observation.saveStatus,observation.identity));
            }
            if(transition.dismissed && transition.cancelRequested){
                transition.resumeAfterHide=PickerPhase::SaveExactTarget;
                transition.hidePending=true;
                return EmitPickerEffect(state,PickerEffectKind::Hide);
            }
            return PickerStartRefresh(state);
        }
        break;

    case PickerPhase::RefreshModel:
        if(acknowledged==PickerEffectKind::Refresh &&
           observation.event==PickerEvent::EffectCompleted){
            if(transition.mode==PickerTransitionMode::RowMoveOnly){
                if(!observation.apiAccepted){
                    transition.failed=true;
                    PickerAppendDiagnostic(
                        transition,
                        L"The picker model could not be refreshed.");
                    return PickerStartRefresh(state);
                }
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            if(PickerPolicyFor(transition.mode).publishesVisual){
                if(!observation.apiAccepted){
                    transition.failed=true;
                    PickerAppendDiagnostic(
                        transition,
                        L"The picker model could not be refreshed.");
                }
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            if(!observation.apiAccepted){
                transition.failed=true;
                PickerAppendDiagnostic(
                    transition,L"The picker model could not be refreshed.");
            }
            if(transition.dismissed){
                if(transition.cancelRequested &&
                   !transition.hideCompleted){
                    transition.resumeAfterHide=PickerPhase::RefreshModel;
                    transition.hidePending=true;
                    return EmitPickerEffect(state,PickerEffectKind::Hide);
                }
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            if(transition.failed){
                transition.phase=PickerPhase::FailureReport;
                return EmitPickerEffect(state,PickerEffectKind::ReportFailure);
            }
            transition.phase=PickerPhase::FocusRestore;
            ++transition.focusAttempts;
            return EmitPickerEffect(state,PickerEffectKind::ShowAndFocus);
        }
        break;

    case PickerPhase::FailureReport:
        if(acknowledged==PickerEffectKind::ReportFailure &&
           observation.event==PickerEvent::EffectCompleted){
            transition.failureReported=true;
            if(transition.dismissed || transition.focusFailureTerminal ||
               transition.suppressFocus){
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            transition.phase=PickerPhase::FocusRestore;
            ++transition.focusAttempts;
            return EmitPickerEffect(state,PickerEffectKind::ShowAndFocus);
        }
        break;

    case PickerPhase::FocusRestore:
        if(acknowledged==PickerEffectKind::ShowAndFocus &&
           observation.event==PickerEvent::EffectCompleted){
            if(observation.popupIsForeground){
                PickerAcknowledgeTerminal(transition);
                return PickerNoEffect();
            }
            if(transition.focusAttempts<4){
                ++transition.focusAttempts;
                return EmitPickerEffect(state,PickerEffectKind::ShowAndFocus);
            }
            transition.failed=true;
            transition.focusFailureTerminal=true;
            PickerAppendDiagnostic(
                transition,L"The picker could not regain foreground focus.");
            transition.phase=PickerPhase::FailureReport;
            return EmitPickerEffect(state,PickerEffectKind::ReportFailure);
        }
        break;

    case PickerPhase::Idle:
        break;
    }
    return PickerNoEffect();
}

struct PickerTargetCaptureState {
    uintptr_t hwnd=0;
    WindowIdentityKey identity;
    std::wstring title;
};

struct PickerBitmapSelection {
    uintptr_t original=0;
    uintptr_t owned=0;
    uintptr_t selected=0;
};

template<class T>
class PickerScopedComOutput {
    T* value_=nullptr;
public:
    PickerScopedComOutput() noexcept=default;
    ~PickerScopedComOutput(){ reset(); }
    PickerScopedComOutput(const PickerScopedComOutput&)=delete;
    PickerScopedComOutput& operator=(const PickerScopedComOutput&)=delete;

    T** put() noexcept {
        reset();
        return &value_;
    }
    T* get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_!=nullptr; }
    void reset(T* value=nullptr) noexcept {
        T* prior=value_;
        value_=value;
        if(prior) prior->Release();
    }
};

inline bool IsCurrentDesktop(const PickerState& state,const GUID& desktop){
    return !GuidIsZero(state.currentDesktop) && !GuidIsZero(desktop) &&
           GuidEq(state.currentDesktop,desktop);
}

inline bool IsSelectedDesktop(const PickerState& state,const GUID& desktop){
    return !GuidIsZero(state.selectedDesktop) && !GuidIsZero(desktop) &&
           GuidEq(state.selectedDesktop,desktop);
}

inline bool IsActiveWindow(const PickerState& state,
                           const WindowIdentityKey& identity){
    return SameIdentity(state.activeWindow,identity);
}

inline bool CompletePickerTargetRecapture(
        PickerTargetCaptureState& capture,
        WindowIdentityRecapture recapture) noexcept {
    if(recapture==WindowIdentityRecapture::Match &&
       capture.hwnd==capture.identity.hwnd &&
       SameIdentity(capture.identity,capture.identity)) return true;
    capture.hwnd=0;
    capture.identity=WindowIdentityKey{};
    capture.title.clear();
    return false;
}

inline PickerDesktopTileRoute DecidePickerDesktopTileRoute(
        bool exactTileAvailable,bool currentTileAvailable,
        HRESULT desktopReadResult,HRESULT currentMembershipResult,
        bool onCurrentDesktop) noexcept {
    if(exactTileAvailable) return PickerDesktopTileRoute::Exact;
    if(currentTileAvailable && SUCCEEDED(currentMembershipResult) &&
       onCurrentDesktop){
        return SUCCEEDED(desktopReadResult)
            ? PickerDesktopTileRoute::
                GloballyVisibleCurrentDesktopFallback
            : PickerDesktopTileRoute::CurrentDesktopFallback;
    }
    return PickerDesktopTileRoute::Skip;
}

inline PickerDesktopTileRoute DecidePickerDesktopTileRoute(
        bool exactTileAvailable,bool currentTileAvailable,
        HRESULT currentMembershipResult,bool onCurrentDesktop) noexcept {
    return DecidePickerDesktopTileRoute(
        exactTileAvailable,currentTileAvailable,E_FAIL,
        currentMembershipResult,onCurrentDesktop);
}

template<class Decision>
struct PickerFinalRowRoute {
    PickerRowAdmission admission;
    Decision decision;

    PickerFinalRowRoute(PickerRowAdmission finalAdmission,
                        Decision finalDecision) noexcept
        : admission(finalAdmission),decision(finalDecision) {}
};

template<class Decision>
inline PickerFinalRowRoute<Decision> FinalizePickerRowRoute(
        PickerRowAdmission baseAdmission,
        PickerDesktopTileRoute desktopRoute,
        Decision baseDecision,Decision displayOnlyFallbackDecision,
        Decision verifiedFallbackDecision) noexcept {
    if(desktopRoute==PickerDesktopTileRoute::Skip)
        return PickerFinalRowRoute<Decision>(
            PickerRowAdmission::Skip,baseDecision);
    if(desktopRoute==PickerDesktopTileRoute::CurrentDesktopFallback &&
       baseAdmission!=PickerRowAdmission::Skip)
        return PickerFinalRowRoute<Decision>(
            PickerRowAdmission::DisplayOnly,displayOnlyFallbackDecision);
    if(desktopRoute==PickerDesktopTileRoute::
            GloballyVisibleCurrentDesktopFallback &&
       baseAdmission==PickerRowAdmission::Verified)
        return PickerFinalRowRoute<Decision>(
            PickerRowAdmission::Verified,verifiedFallbackDecision);
    if(desktopRoute==PickerDesktopTileRoute::
            GloballyVisibleCurrentDesktopFallback &&
       baseAdmission==PickerRowAdmission::DisplayOnly)
        return PickerFinalRowRoute<Decision>(
            PickerRowAdmission::DisplayOnly,displayOnlyFallbackDecision);
    return PickerFinalRowRoute<Decision>(baseAdmission,baseDecision);
}

enum class PickerPopupDesktopAssociation {
    UseObserved,
    RepairToCurrent,
    Reject
};

inline PickerPopupDesktopAssociation DecidePickerPopupDesktopAssociation(
        PickerReadValidity validity,const GUID& observed,
        const GUID& current) noexcept {
    if(GuidIsZero(current))
        return PickerPopupDesktopAssociation::Reject;
    if(validity==PickerReadValidity::Valid)
        return !GuidIsZero(observed)
            ? PickerPopupDesktopAssociation::UseObserved
            : PickerPopupDesktopAssociation::RepairToCurrent;
    if(validity==PickerReadValidity::Unavailable && GuidIsZero(observed))
        return PickerPopupDesktopAssociation::RepairToCurrent;
    return PickerPopupDesktopAssociation::Reject;
}

struct PickerPopupDesktopRead {
    HRESULT result=E_NOINTERFACE;
    PickerReadValidity validity=PickerReadValidity::Unavailable;
    GUID desktop{};

    PickerPopupDesktopRead() noexcept=default;
    PickerPopupDesktopRead(HRESULT value,PickerReadValidity readValidity,
                           const GUID& readDesktop) noexcept
        : result(value),validity(readValidity),desktop(readDesktop) {}
};

struct PickerPopupDesktopMove {
    bool invoked=false;
    HRESULT result=E_NOINTERFACE;

    PickerPopupDesktopMove() noexcept=default;
    PickerPopupDesktopMove(bool wasInvoked,HRESULT value) noexcept
        : invoked(wasInvoked),result(value) {}
};

struct PickerPopupBindingFacts {
    PickerPopupDesktopRead initial;
    PickerPopupDesktopMove move;
    PickerPopupDesktopRead verify;
    bool moveAttempted=false;
    bool verifyAttempted=false;
};

enum class PickerPopupBindingResult {
    UseObserved,
    Repaired,
    CurrentUnavailable,
    InitialUnsupported,
    MoveUnavailable,
    VerifyUnavailable,
    VerifyMismatch,
    Exception
};

template<class ReadCallback,class MoveCallback>
inline PickerPopupBindingResult DrivePickerPopupBinding(
        const GUID& current,ReadCallback&& read,MoveCallback&& move,
        PickerPopupBindingFacts& facts) noexcept {
    facts=PickerPopupBindingFacts{};
    if(GuidIsZero(current))
        return PickerPopupBindingResult::CurrentUnavailable;
    try {
        facts.initial=read();
        const PickerPopupDesktopAssociation association=
            DecidePickerPopupDesktopAssociation(
                facts.initial.validity,facts.initial.desktop,current);
        if(association==PickerPopupDesktopAssociation::UseObserved)
            return PickerPopupBindingResult::UseObserved;
        if(association!=PickerPopupDesktopAssociation::RepairToCurrent)
            return PickerPopupBindingResult::InitialUnsupported;

        const bool elementNotFound=
            facts.initial.result==TYPE_E_ELEMENTNOTFOUND;
        if(!elementNotFound)
            return PickerPopupBindingResult::InitialUnsupported;

        facts.moveAttempted=true;
        facts.move=move(current);
        if(!facts.move.invoked)
            return PickerPopupBindingResult::MoveUnavailable;

        facts.verifyAttempted=true;
        facts.verify=read();
        if(facts.verify.validity!=PickerReadValidity::Valid ||
           GuidIsZero(facts.verify.desktop))
            return PickerPopupBindingResult::VerifyUnavailable;
        if(!GuidEq(facts.verify.desktop,current))
            return PickerPopupBindingResult::VerifyMismatch;
        return PickerPopupBindingResult::Repaired;
    } catch(...) {
        return PickerPopupBindingResult::Exception;
    }
}

inline PickerPopupRoute DecidePickerPopupRoute(
        PickerPopupBindingResult binding,
        const PickerPopupBindingFacts& facts,
        bool popupIsToolWindow) noexcept {
    if(binding==PickerPopupBindingResult::UseObserved ||
       binding==PickerPopupBindingResult::Repaired)
        return PickerPopupRoute::Managed;
    const bool exactUnmanagedSignature=
        binding==PickerPopupBindingResult::VerifyUnavailable &&
        popupIsToolWindow &&
        facts.initial.result==TYPE_E_ELEMENTNOTFOUND &&
        facts.initial.validity==PickerReadValidity::Unavailable &&
        GuidIsZero(facts.initial.desktop) &&
        facts.moveAttempted && facts.move.invoked &&
        facts.move.result==S_OK &&
        facts.verifyAttempted &&
        facts.verify.result==TYPE_E_ELEMENTNOTFOUND &&
        facts.verify.validity==PickerReadValidity::Unavailable &&
        GuidIsZero(facts.verify.desktop);
    return exactUnmanagedSignature
        ? PickerPopupRoute::StickyUnmanaged
        : PickerPopupRoute::Reject;
}

inline PickerRowAdmission DecidePickerRowAdmission(
        bool altTabEligible,bool desktopAvailable,bool titleAvailable,
        bool identityComplete,WindowIdentityRecapture recapture) noexcept {
    if(!altTabEligible || !desktopAvailable || !titleAvailable)
        return PickerRowAdmission::Skip;
    if(identityComplete && recapture==WindowIdentityRecapture::Lost)
        return PickerRowAdmission::Skip;
    return identityComplete && recapture==WindowIdentityRecapture::Match
        ? PickerRowAdmission::Verified
        : PickerRowAdmission::DisplayOnly;
}

inline bool PickerRowUsesStableIdentity(
        PickerRowAdmission admission) noexcept {
    return admission==PickerRowAdmission::Verified;
}

inline bool PickerTargetMatchesActive(
        uintptr_t target,const WindowIdentityKey& active) noexcept {
    return target!=0 && target==active.hwnd &&
           SameIdentity(active,active);
}

inline bool PickerCommitIdentityAllowed(
        uintptr_t target,const WindowIdentityKey& active,
        const WindowIdentityKey& captured,
        WindowIdentityRecapture recapture) noexcept {
    return PickerTargetMatchesActive(target,active) &&
           recapture==WindowIdentityRecapture::Match &&
           SameIdentity(active,captured);
}

inline bool PickerSearchResultMatches(
        const WindowIdentityKey& row,
        const WindowIdentityKey& result) noexcept {
    return SameIdentity(row,result);
}

enum class PickerRowReadResult {
    Success,
    IdentityUnavailable,
    DesktopUnavailable,
    TitleUnavailable,
    IdentityChanged,
    AllocationFailure,
    GlobalSnapshotFailure
};

inline bool ContinuePickerRowEnumeration(
        PickerRowReadResult result,bool& modelFailed) noexcept {
    if(modelFailed) return false;
    if(result==PickerRowReadResult::AllocationFailure ||
       result==PickerRowReadResult::GlobalSnapshotFailure){
        modelFailed=true;
        return false;
    }
    return true;
}

inline bool SetPickerCurrentDesktop(PickerState& state,
                                    const GUID& desktop) noexcept {
    if(GuidIsZero(desktop)){
        state.currentDesktop=GUID{};
        return false;
    }
    state.currentDesktop=desktop;
    return true;
}

inline bool SetPickerSelection(PickerState& state,int index,
                               const GUID& desktop) noexcept {
    if(index<0 || GuidIsZero(desktop)) return false;
    state.selectedDesktop=desktop;
    state.selectedIndex=index;
    return true;
}

inline bool PreparePickerRefreshSelectionFromActual(
        PickerState& state) noexcept {
    if(!state.transition.failed && !state.transition.cancelRequested)
        return false;
    state.selectedDesktop=GUID{};
    state.selectedIndex=-1;
    if(state.transition.observedCurrentValidity==
           PickerReadValidity::Valid &&
       !GuidIsZero(state.transition.observedCurrentDesktop))
        state.selectedDesktop=state.transition.observedCurrentDesktop;
    return true;
}

inline bool ResolvePickerSelection(PickerState& state,
                                   const std::vector<GUID>& desktops) noexcept {
    if(desktops.empty()){
        state.selectedDesktop=GUID{};
        state.selectedIndex=-1;
        return false;
    }
    if(!GuidIsZero(state.selectedDesktop)){
        for(size_t index=0;index<desktops.size();++index)
            if(!GuidIsZero(desktops[index]) &&
               GuidEq(state.selectedDesktop,desktops[index]))
                return SetPickerSelection(
                    state,static_cast<int>(index),desktops[index]);
    }
    if(!GuidIsZero(state.currentDesktop)){
        for(size_t index=0;index<desktops.size();++index)
            if(!GuidIsZero(desktops[index]) &&
               GuidEq(state.currentDesktop,desktops[index]))
                return SetPickerSelection(
                    state,static_cast<int>(index),desktops[index]);
    }
    for(size_t index=0;index<desktops.size();++index)
        if(!GuidIsZero(desktops[index]))
            return SetPickerSelection(
                state,static_cast<int>(index),desktops[index]);
    state.selectedDesktop=GUID{};
    state.selectedIndex=-1;
    return false;
}

inline bool AppendUniquePickerDesktop(std::vector<GUID>& desktops,
                                      const GUID& desktop) noexcept {
    if(GuidIsZero(desktop)) return false;
    for(const GUID& existing : desktops)
        if(GuidEq(existing,desktop)) return false;
    try {
        desktops.push_back(desktop);
        return true;
    } catch(...) {
        return false;
    }
}

template<class Rows,class GetSearchText>
inline bool BuildPickerFilteredIndices(
        const Rows& rows,const std::wstring& searchText,
        std::vector<size_t>& output,GetSearchText&& getSearchText) noexcept {
    try {
        std::vector<size_t> staged;
        staged.reserve(rows.size());
        for(size_t index=0;index<rows.size();++index){
            const std::wstring& searchable=getSearchText(rows[index]);
            if(searchText.empty() ||
               searchable.find(searchText)!=std::wstring::npos)
                staged.push_back(index);
        }
        output.swap(staged);
        return true;
    } catch(...) {
        return false;
    }
}

inline PickerState PreservePickerUi(const PickerState& state){
    return state;
}

template<class Collect,class Publish>
inline bool RunPickerLightweightRefresh(
        PickerState& publishedState,Collect&& collect,
        Publish&& publish) noexcept {
    if(publishedState.controlledTransition()) return false;
    try {
        auto snapshot=collect();
        PickerState staged=PreservePickerUi(publishedState);
        if(!publish(snapshot,staged)) return false;
        publishedState.swap(staged);
        return true;
    } catch(...) { return false; }
}

inline void SwapPickerState(PickerState& left,PickerState& right) noexcept {
    static_assert(noexcept(left.swap(right)),
                  "picker state publication must be noexcept");
    left.swap(right);
}

template<class Model,class Build>
inline bool RunPickerRefreshTransaction(Model& publishedModel,
                                        PickerState& publishedState,
                                        Build&& build) noexcept {
    try {
        Model stagedModel;
        PickerState stagedState=PreservePickerUi(publishedState);
        if(!build(stagedModel,stagedState)) return false;
        static_assert(noexcept(publishedModel.swap(stagedModel)),
                      "picker model publication must be noexcept");
        publishedModel.swap(stagedModel);
        SwapPickerState(publishedState,stagedState);
        return true;
    } catch(...) {
        return false;
    }
}

template<class Model,class Cache,class Build>
inline bool RunPickerVisualRefreshTransaction(
        Model& publishedModel,Cache& publishedCache,
        PickerState& publishedState,
        const PickerVisualAssignmentMutation& mutation,
        Build&& build) noexcept {
    try {
        Model stagedModel;
        Cache stagedCache;
        PickerState stagedState=PreservePickerUi(publishedState);
        if(!StagePickerVisualAssignmentMutation(
                stagedState,mutation))
            return false;
        if(!build(stagedModel,stagedCache,stagedState))
            return false;
        static_assert(noexcept(
            publishedModel.swap(stagedModel)),
            "picker model publication must be noexcept");
        static_assert(noexcept(
            publishedCache.swap(stagedCache)),
            "picker paint publication must be noexcept");
        publishedModel.swap(stagedModel);
        publishedCache.swap(stagedCache);
        SwapPickerState(publishedState,stagedState);
        return true;
    } catch(...) {
        return false;
    }
}

template<class Model,class Build>
inline bool RunPickerRefreshWithCurrent(
        Model& publishedModel,PickerState& publishedState,
        const GUID& observedCurrent,Build&& build) noexcept {
    const bool currentAvailable=!GuidIsZero(observedCurrent);
    if(!currentAvailable)
        SetPickerCurrentDesktop(publishedState,GUID{});
    return RunPickerRefreshTransaction(
        publishedModel,publishedState,
        [&](Model& stagedModel,PickerState& stagedState){
            if(currentAvailable)
                SetPickerCurrentDesktop(stagedState,observedCurrent);
            else
                SetPickerCurrentDesktop(stagedState,GUID{});
            return build(stagedModel,stagedState);
        });
}

template<class Cache,class Build>
inline bool RunPickerPaintCacheTransaction(Cache& publishedCache,
                                           Build&& build) noexcept {
    try {
        Cache stagedCache;
        if(!build(stagedCache)) return false;
        static_assert(noexcept(publishedCache.swap(stagedCache)),
                      "picker paint-cache publication must be noexcept");
        publishedCache.swap(stagedCache);
        return true;
    } catch(...) {
        return false;
    }
}

template<class Cache,class Build,class BeforePublish,class OnFailure>
inline bool RefreshPickerPaintCacheTransaction(
        Cache& publishedCache,Build&& build,
        BeforePublish&& beforePublish,OnFailure&& onFailure) noexcept {
    static_assert(noexcept(publishedCache.swap(publishedCache)),
                  "picker paint-cache publication must be noexcept");
    static_assert(noexcept(beforePublish()),
                  "picker pre-publication hook must be noexcept");
    static_assert(noexcept(onFailure(publishedCache)),
                  "picker cache invalidation must be noexcept");
    try {
        Cache stagedCache;
        if(!build(stagedCache)){
            onFailure(publishedCache);
            return false;
        }
        beforePublish();
        publishedCache.swap(stagedCache);
        return true;
    } catch(...) {
        onFailure(publishedCache);
        return false;
    }
}

inline uint64_t BeginPickerPaintRefresh(PickerState& state) noexcept {
    if(state.paintGeneration==(std::numeric_limits<uint64_t>::max)())
        state.paintGeneration=1;
    else
        ++state.paintGeneration;
    if(state.paintGeneration==0) state.paintGeneration=1;
    return state.paintGeneration;
}

inline uint64_t AdvancePickerRowLayoutEpoch(PickerState& state) noexcept {
    if(state.rowLayoutEpoch==(std::numeric_limits<uint64_t>::max)())
        state.rowLayoutEpoch=1;
    else
        ++state.rowLayoutEpoch;
    if(state.rowLayoutEpoch==0) state.rowLayoutEpoch=1;
    return state.rowLayoutEpoch;
}

template<class Cache,class ResetHover>
inline void InvalidatePickerPaintCacheState(
        PickerState& state,Cache& cache,ResetHover&& resetHover) noexcept {
    static_assert(noexcept(resetHover()),
                  "picker hover reset must be noexcept");
    static_assert(noexcept(cache.clear()),
                  "picker cache invalidation must be noexcept");
    BeginPickerPaintRefresh(state);
    resetHover();
    cache.clear();
}

inline bool PickerPaintCacheMatches(const PickerState& state,
                                    uint64_t cacheGeneration) noexcept {
    return cacheGeneration!=0 &&
           state.paintGeneration==cacheGeneration;
}

inline PickerFooterLink HitCurrentPickerFooterLink(
        const PickerState& state,uint64_t cacheGeneration,
        const PickerFooterLayout& layout,POINT point) noexcept {
    return PickerPaintCacheMatches(state,cacheGeneration)
        ?HitPickerFooterLink(layout,point):PickerFooterLink::None;
}

inline PickerFooterActivation ResolvePickerFooterActivation(
        const PickerState& state,uint64_t cacheGeneration,
        const PickerFooterLayout& layout,POINT point) noexcept {
    PickerFooterActivation activation;
    activation.link=HitCurrentPickerFooterLink(
        state,cacheGeneration,layout,point);
    activation.url=PickerFooterUrl(activation.link);
    activation.consumed=activation.link!=PickerFooterLink::None &&
                        activation.url!=nullptr;
    return activation;
}

inline bool PickerHoverPairMatches(int previousRow,
                                   uint64_t previousGeneration,
                                   int row,uint64_t cacheGeneration) noexcept {
    return previousRow>=0 && row>=0 && previousRow==row &&
           previousGeneration!=0 &&
           previousGeneration==cacheGeneration;
}

inline bool PickerShowPreparationComplete(bool modelReady,
                                          bool paintCacheReady) noexcept {
    return modelReady && paintCacheReady;
}

inline COLORREF BlendColor(COLORREF background,COLORREF foreground,
                           BYTE alpha){
    const auto blend=[&](BYTE bg,BYTE fg){
        return static_cast<BYTE>((static_cast<unsigned>(bg)*(255-alpha)+
                                  static_cast<unsigned>(fg)*alpha+127)/255);
    };
    return RGB(blend(GetRValue(background),GetRValue(foreground)),
               blend(GetGValue(background),GetGValue(foreground)),
               blend(GetBValue(background),GetBValue(foreground)));
}

inline COLORREF PickerTileFill(COLORREF normal,COLORREF current,
                               COLORREF dimColor,bool isCurrent,bool dimmed){
    const COLORREF base=isCurrent?current:normal;
    return dimmed?BlendColor(base,dimColor,160):base;
}

inline COLORREF PickerTileBorder(bool selected,COLORREF active,
                                 COLORREF passive){
    return selected?active:passive;
}

inline int PickerVisibleScroll(int savedScroll,int maxScroll) noexcept {
    if(savedScroll<=0 || maxScroll<=0) return 0;
    return savedScroll>maxScroll?maxScroll:savedScroll;
}

inline int AdvancePickerScroll(int savedScroll,int maxScroll,
                               int wheelDelta) noexcept {
    const int maximum=maxScroll>0?maxScroll:0;
    const int visible=PickerVisibleScroll(savedScroll,maximum);
    if(wheelDelta>0) return visible>0?visible-1:0;
    if(wheelDelta<0) return visible<maximum?visible+1:maximum;
    return visible;
}

inline bool PublishPickerBitmapReplacement(
        PickerBitmapSelection& state,uintptr_t replacement,
        uintptr_t previouslySelected,uintptr_t& release) noexcept {
    if(replacement==0 || previouslySelected==0) return false;
    const uintptr_t expected=state.owned?state.owned:state.original;
    if(expected==0 || state.selected!=expected ||
       previouslySelected!=expected) return false;
    const uintptr_t oldOwned=state.owned;
    state.owned=replacement;
    state.selected=replacement;
    release=oldOwned;
    return true;
}
