#pragma once

#include "str_util.hpp"
#include "window_identity.hpp"

#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
