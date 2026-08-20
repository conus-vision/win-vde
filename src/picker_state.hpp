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
    GUID guid=left.currentDesktop;
    left.currentDesktop=right.currentDesktop;
    right.currentDesktop=guid;
    guid=left.selectedDesktop;
    left.selectedDesktop=right.selectedDesktop;
    right.selectedDesktop=guid;
    const int selectedIndex=left.selectedIndex;
    left.selectedIndex=right.selectedIndex;
    right.selectedIndex=selectedIndex;
    const WindowIdentityKey activeWindow=left.activeWindow;
    left.activeWindow=right.activeWindow;
    right.activeWindow=activeWindow;
    left.searchText.swap(right.searchText);
    const bool searchActive=left.searchActive;
    left.searchActive=right.searchActive;
    right.searchActive=searchActive;
    const bool controlledTransition=left.controlledTransition;
    left.controlledTransition=right.controlledTransition;
    right.controlledTransition=controlledTransition;
    left.scrollByDesktop.swap(right.scrollByDesktop);
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

inline int AdvancePickerScroll(int scroll,int wheelDelta) noexcept {
    if(wheelDelta>0) return scroll>0?scroll-1:0;
    if(wheelDelta<0 && scroll<(std::numeric_limits<int>::max)())
        return scroll+1;
    return scroll;
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
