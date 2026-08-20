#pragma once

#include "str_util.hpp"
#include "window_identity.hpp"

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

enum class PickerPointerTarget {
    None,
    Footer,
    ClearSearch,
    Search,
    Tile
};

struct PickerPointerActivation {
    PickerPointerTarget target=PickerPointerTarget::None;
    PickerFooterActivation footer;
    int tileIndex=-1;
};

inline PickerPointerActivation ResolvePickerPointerActivation(
        const PickerFooterActivation& footer,bool clearSearchHit,
        bool searchHit,int tileIndex) noexcept {
    PickerPointerActivation activation;
    if(footer.consumed){
        activation.target=PickerPointerTarget::Footer;
        activation.footer=footer;
    } else if(clearSearchHit){
        activation.target=PickerPointerTarget::ClearSearch;
    } else if(searchHit){
        activation.target=PickerPointerTarget::Search;
    } else if(tileIndex>=0){
        activation.target=PickerPointerTarget::Tile;
        activation.tileIndex=tileIndex;
    }
    return activation;
}

template<class OnFooter,class OnClearSearch,class OnSearch,class OnTile>
inline bool DispatchPickerPointerActivation(
        const PickerPointerActivation& activation,
        OnFooter&& onFooter,OnClearSearch&& onClearSearch,
        OnSearch&& onSearch,OnTile&& onTile) noexcept {
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

struct PickerState {
    GUID currentDesktop={0};
    GUID selectedDesktop={0};
    int selectedIndex=-1;
    WindowIdentityKey activeWindow;
    std::wstring searchText;
    bool searchActive=false;
    bool controlledTransition=false;
    std::map<std::string,int> scrollByDesktop;
    uint64_t paintGeneration=0;

    void swap(PickerState& other) noexcept {
        static_assert(noexcept(searchText.swap(other.searchText)),
                      "picker search swap must be noexcept");
        static_assert(noexcept(scrollByDesktop.swap(
                          other.scrollByDesktop)),
                      "picker scroll swap must be noexcept");
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
        searchText.swap(other.searchText);
        const bool active=searchActive;
        searchActive=other.searchActive;
        other.searchActive=active;
        const bool transition=controlledTransition;
        controlledTransition=other.controlledTransition;
        other.controlledTransition=transition;
        scrollByDesktop.swap(other.scrollByDesktop);
        const uint64_t generation=paintGeneration;
        paintGeneration=other.paintGeneration;
        other.paintGeneration=generation;
    }
};

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

inline bool AcceptPickerRowIdentity(
        const WindowIdentityKey& identity,
        WindowIdentityRecapture recapture) noexcept {
    return recapture==WindowIdentityRecapture::Match &&
           SameIdentity(identity,identity);
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
