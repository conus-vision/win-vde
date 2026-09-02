// reopen_model.hpp — the selection model behind "Reopen browser windows".
//
// One checkpoint is shown as three cascading columns — desktops, the windows
// on the checked desktops, the tabs of the checked windows — each with check
// boxes and an "all" switch, plus a browser filter.  Checking a parent checks
// everything under it; a tab that is already open in the browser is shown but
// stays unchecked so a reopen never duplicates it.  What "Reopen" acts on is
// the checked tabs of the checked windows on the checked desktops that pass the
// browser filter, grouped back into their original windows so every window
// returns to the desktop it was on.
//
// Pure logic (no GUI) so it can be unit-tested; see tests/vdtest.cpp.
#pragma once

#include "tabsnap.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

struct ReopenTabItem {
    bool checked=false;
    bool open=false;            // the same URL is open in the browser right now
};

struct ReopenWindowItem {
    bool checked=false;
    size_t desktop=0;           // index into ReopenModel::desktops
    std::vector<ReopenTabItem> tabs;
};

struct ReopenDesktopItem {
    bool checked=false;
    GUID guid={0};
    int index=-1;
    std::wstring name;
    bool missing=false;            // no such desktop exists any more
    std::vector<size_t> windows;   // indices into ReopenModel::windows
};

struct ReopenModel {
    SessionSnapshot snapshot;
    std::vector<ReopenDesktopItem> desktops;   // ordered by desktop index
    std::vector<ReopenWindowItem> windows;     // parallel to snapshot.windows
    std::set<std::string> apps;                // browser filter: apps shown
    // Text filters, one per column.  They narrow what a column *shows* - and
    // therefore what its "all" box and the columns to its right act on - but
    // never what is selected: a checked tab hidden by a filter is still
    // reopened, and the status line counts it.
    std::wstring desktopFilter;
    std::wstring windowFilter;
    std::wstring tabFilter;
    // "Hide open": view-only switches, one per column.  A tab is open when the
    // browser shows its URL right now; a window is open when every tab it has
    // is; a desktop is open when every window it lists is.
    bool hideOpenDesktops=false;
    bool hideOpenWindows=false;
    bool hideOpenTabs=false;
};

inline std::wstring ReopenLowerText(std::wstring text){
    if(!text.empty()) CharLowerBuffW(&text[0],(DWORD)text.size());
    return text;
}

// Case-insensitive substring test; an empty needle matches everything.
inline bool ReopenTextContains(const std::wstring& haystack,const std::wstring& needle){
    if(needle.empty()) return true;
    return ReopenLowerText(haystack).find(ReopenLowerText(needle))!=std::wstring::npos;
}

inline const wchar_t* ReopenBrowserLabel(const std::string& app){
    if(app=="firefox") return L"Firefox";
    if(app=="chrome") return L"Chrome";
    if(app=="msedge") return L"Edge";
    return L"";
}

typedef std::map<std::string,std::set<std::string> > OpenUrlsByApp;

inline bool ReopenTabIsOpen(const OpenUrlsByApp& open,const std::string& app,
                            const std::string& url){
    OpenUrlsByApp::const_iterator found=open.find(app);
    return found!=open.end() && found->second.count(url)!=0;
}

// Everything starts checked except tabs that are already open, so "Reopen"
// without further clicks brings back exactly what is missing.
inline bool BuildReopenModel(const SessionSnapshot& snapshot,
                             const OpenUrlsByApp& openUrls,
                             const std::vector<DeskRec>& currentDesktops,
                             ReopenModel& output){
    try {
        ReopenModel model;
        model.snapshot=snapshot;
        model.apps=SnapshotApps(snapshot);
        std::map<std::string,size_t> desktopByGuid;
        model.windows.resize(snapshot.windows.size());
        for(size_t w=0;w<snapshot.windows.size();++w){
            const SnapWindow& window=snapshot.windows[w];
            const std::string key=W2U8(GuidToString(window.desktop));
            std::map<std::string,size_t>::iterator found=desktopByGuid.find(key);
            if(found==desktopByGuid.end()){
                ReopenDesktopItem desktop;
                desktop.checked=true;
                desktop.guid=window.desktop;
                desktop.index=window.deskIndex;
                for(size_t d=0;d<snapshot.desks.size();++d)
                    if(GuidEq(snapshot.desks[d].guid,window.desktop)){
                        desktop.name=snapshot.desks[d].name;
                        if(desktop.index<0) desktop.index=snapshot.desks[d].index;
                        break;
                    }
                if(desktop.name.empty())
                    desktop.name=L"Desktop "+std::to_wstring(desktop.index+1);
                // An empty current list means "unknown", never "all gone".
                if(!currentDesktops.empty()){
                    desktop.missing=true;
                    for(size_t c=0;c<currentDesktops.size();++c)
                        if(GuidEq(currentDesktops[c].guid,window.desktop)){
                            desktop.missing=false;
                            break;
                        }
                }
                model.desktops.push_back(desktop);
                found=desktopByGuid.insert(
                    std::make_pair(key,model.desktops.size()-1)).first;
            }
            ReopenWindowItem& item=model.windows[w];
            item.checked=true;
            item.desktop=found->second;
            item.tabs.resize(window.tabs.size());
            for(size_t t=0;t<window.tabs.size();++t){
                item.tabs[t].open=ReopenTabIsOpen(openUrls,window.app,window.tabs[t].url);
                item.tabs[t].checked=!item.tabs[t].open;
            }
            model.desktops[found->second].windows.push_back(w);
        }
        // Keep desktops in their on-screen order; window indices stay valid
        // because they point into model.windows, not into the desktop list —
        // but windows[].desktop does, so remap it after sorting.
        std::vector<size_t> order(model.desktops.size());
        for(size_t i=0;i<order.size();++i) order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){
            return model.desktops[a].index<model.desktops[b].index;
        });
        std::vector<size_t> newPosition(order.size());
        std::vector<ReopenDesktopItem> sorted;
        sorted.reserve(order.size());
        for(size_t i=0;i<order.size();++i){
            newPosition[order[i]]=i;
            sorted.push_back(model.desktops[order[i]]);
        }
        model.desktops.swap(sorted);
        for(size_t w=0;w<model.windows.size();++w)
            model.windows[w].desktop=newPosition[model.windows[w].desktop];
        output=std::move(model);
        return true;
    } catch(...) { return false; }
}

// ------------------------------------------------------------ visibility ---
inline bool ReopenWindowPassesFilter(const ReopenModel& model,size_t window){
    return window<model.windows.size() && window<model.snapshot.windows.size() &&
        model.apps.count(model.snapshot.windows[window].app)!=0;
}

inline bool ReopenDesktopMatches(const ReopenModel& model,size_t desktop){
    if(desktop>=model.desktops.size()) return false;
    const ReopenDesktopItem& item=model.desktops[desktop];
    return ReopenTextContains(
        std::to_wstring(item.index+1)+L" "+item.name,model.desktopFilter);
}

inline bool ReopenWindowMatches(const ReopenModel& model,size_t window){
    if(window>=model.snapshot.windows.size()) return false;
    const SnapWindow& item=model.snapshot.windows[window];
    return ReopenTextContains(
        U82W(item.activeTitle)+L" "+ReopenBrowserLabel(item.app),model.windowFilter);
}

inline bool ReopenTabMatches(const ReopenModel& model,size_t window,size_t tab){
    if(window>=model.snapshot.windows.size() ||
       tab>=model.snapshot.windows[window].tabs.size()) return false;
    const SnapTab& item=model.snapshot.windows[window].tabs[tab];
    return ReopenTextContains(U82W(item.title)+L" "+U82W(item.url),model.tabFilter);
}

inline bool ReopenWindowFullyOpen(const ReopenModel& model,size_t window){
    if(window>=model.windows.size() || model.windows[window].tabs.empty()) return false;
    for(size_t t=0;t<model.windows[window].tabs.size();++t)
        if(!model.windows[window].tabs[t].open) return false;
    return true;
}

inline bool ReopenDesktopFullyOpen(const ReopenModel& model,size_t desktop){
    if(desktop>=model.desktops.size()) return false;
    bool any=false;
    for(size_t i=0;i<model.desktops[desktop].windows.size();++i){
        const size_t window=model.desktops[desktop].windows[i];
        if(!ReopenWindowPassesFilter(model,window)) continue;
        any=true;
        if(!ReopenWindowFullyOpen(model,window)) return false;
    }
    return any;
}

// Desktops holding at least one window of a filtered-in browser, narrowed by
// the desktop filter and the hide-open switch.
inline std::vector<size_t> ReopenVisibleDesktops(const ReopenModel& model){
    std::vector<size_t> visible;
    for(size_t d=0;d<model.desktops.size();++d){
        if(!ReopenDesktopMatches(model,d)) continue;
        if(model.hideOpenDesktops && ReopenDesktopFullyOpen(model,d)) continue;
        for(size_t i=0;i<model.desktops[d].windows.size();++i)
            if(ReopenWindowPassesFilter(model,model.desktops[d].windows[i])){
                visible.push_back(d);
                break;
            }
    }
    return visible;
}

// Selected = checked and on a checked desktop, regardless of text filters.
inline bool ReopenWindowSelected(const ReopenModel& model,size_t window){
    if(!ReopenWindowPassesFilter(model,window)) return false;
    const size_t desktop=model.windows[window].desktop;
    return desktop<model.desktops.size() && model.desktops[desktop].checked &&
        model.windows[window].checked;
}

inline std::vector<size_t> ReopenSelectedWindows(const ReopenModel& model){
    std::vector<size_t> selected;
    for(size_t d=0;d<model.desktops.size();++d)
        for(size_t i=0;i<model.desktops[d].windows.size();++i){
            const size_t window=model.desktops[d].windows[i];
            if(ReopenWindowSelected(model,window)) selected.push_back(window);
        }
    return selected;
}

// Visible = on a checked desktop that passes the desktop filter, and passing
// the window filter itself.
inline bool ReopenWindowVisible(const ReopenModel& model,size_t window){
    if(!ReopenWindowPassesFilter(model,window)) return false;
    const size_t desktop=model.windows[window].desktop;
    if(desktop>=model.desktops.size() || !model.desktops[desktop].checked) return false;
    if(!ReopenDesktopMatches(model,desktop) || !ReopenWindowMatches(model,window)) return false;
    if(model.hideOpenDesktops && ReopenDesktopFullyOpen(model,desktop)) return false;
    if(model.hideOpenWindows && ReopenWindowFullyOpen(model,window)) return false;
    return true;
}

inline std::vector<size_t> ReopenVisibleWindows(const ReopenModel& model){
    std::vector<size_t> visible;
    for(size_t d=0;d<model.desktops.size();++d)
        for(size_t i=0;i<model.desktops[d].windows.size();++i){
            const size_t window=model.desktops[d].windows[i];
            if(ReopenWindowVisible(model,window)) visible.push_back(window);
        }
    return visible;
}

// Tabs of the checked, visible windows — (window, tab) pairs in window order.
inline std::vector<std::pair<size_t,size_t> > ReopenVisibleTabs(
        const ReopenModel& model){
    std::vector<std::pair<size_t,size_t> > visible;
    const std::vector<size_t> windows=ReopenVisibleWindows(model);
    for(size_t i=0;i<windows.size();++i){
        const size_t window=windows[i];
        if(!model.windows[window].checked) continue;
        for(size_t t=0;t<model.windows[window].tabs.size();++t){
            if(model.hideOpenTabs && model.windows[window].tabs[t].open) continue;
            if(ReopenTabMatches(model,window,t))
                visible.push_back(std::make_pair(window,t));
        }
    }
    return visible;
}

// --------------------------------------------------------------- cascade ---
inline void ReopenSetTabChecked(ReopenModel& model,size_t window,size_t tab,
                                bool checked){
    if(window>=model.windows.size() || tab>=model.windows[window].tabs.size()) return;
    model.windows[window].tabs[tab].checked=checked;
}

// Checking a window checks its tabs — except the ones already open, which the
// user has to ask for one by one.  Unchecking clears every tab.
inline void ReopenSetWindowChecked(ReopenModel& model,size_t window,bool checked){
    if(window>=model.windows.size()) return;
    ReopenWindowItem& item=model.windows[window];
    item.checked=checked;
    for(size_t t=0;t<item.tabs.size();++t)
        item.tabs[t].checked=checked && !item.tabs[t].open;
}

inline void ReopenSetDesktopChecked(ReopenModel& model,size_t desktop,bool checked){
    if(desktop>=model.desktops.size()) return;
    ReopenDesktopItem& item=model.desktops[desktop];
    item.checked=checked;
    for(size_t i=0;i<item.windows.size();++i)
        if(ReopenWindowPassesFilter(model,item.windows[i]))
            ReopenSetWindowChecked(model,item.windows[i],checked);
}

inline void ReopenSetAllDesktopsChecked(ReopenModel& model,bool checked){
    const std::vector<size_t> visible=ReopenVisibleDesktops(model);
    for(size_t i=0;i<visible.size();++i)
        ReopenSetDesktopChecked(model,visible[i],checked);
}

inline void ReopenSetAllWindowsChecked(ReopenModel& model,bool checked){
    const std::vector<size_t> visible=ReopenVisibleWindows(model);
    for(size_t i=0;i<visible.size();++i)
        ReopenSetWindowChecked(model,visible[i],checked);
}

inline void ReopenSetAllTabsChecked(ReopenModel& model,bool checked){
    const std::vector<std::pair<size_t,size_t> > visible=ReopenVisibleTabs(model);
    for(size_t i=0;i<visible.size();++i){
        ReopenTabItem& tab=model.windows[visible[i].first].tabs[visible[i].second];
        tab.checked=checked && !tab.open;
    }
}

// True when every visible row of that column is checked (drives the "all" box).
inline bool ReopenAllDesktopsChecked(const ReopenModel& model){
    const std::vector<size_t> visible=ReopenVisibleDesktops(model);
    if(visible.empty()) return false;
    for(size_t i=0;i<visible.size();++i)
        if(!model.desktops[visible[i]].checked) return false;
    return true;
}

inline bool ReopenAllWindowsChecked(const ReopenModel& model){
    const std::vector<size_t> visible=ReopenVisibleWindows(model);
    if(visible.empty()) return false;
    for(size_t i=0;i<visible.size();++i)
        if(!model.windows[visible[i]].checked) return false;
    return true;
}

inline bool ReopenAllTabsChecked(const ReopenModel& model){
    const std::vector<std::pair<size_t,size_t> > visible=ReopenVisibleTabs(model);
    bool any=false;
    for(size_t i=0;i<visible.size();++i){
        const ReopenTabItem& tab=model.windows[visible[i].first].tabs[visible[i].second];
        if(tab.open) continue;           // an open tab does not count either way
        any=true;
        if(!tab.checked) return false;
    }
    return any;
}

// --------------------------------------------------------------- summary ---
struct ReopenSelectionSummary {
    size_t windows=0;       // windows with at least one selected tab
    size_t tabs=0;
};

inline size_t ReopenCheckedTabCount(const ReopenModel& model,size_t window){
    size_t count=0;
    if(window>=model.windows.size()) return 0;
    for(size_t t=0;t<model.windows[window].tabs.size();++t)
        if(model.windows[window].tabs[t].checked) ++count;
    return count;
}

inline ReopenSelectionSummary ReopenSummarize(const ReopenModel& model){
    ReopenSelectionSummary summary;
    const std::vector<size_t> windows=ReopenSelectedWindows(model);
    for(size_t i=0;i<windows.size();++i){
        const size_t tabs=ReopenCheckedTabCount(model,windows[i]);
        if(tabs==0) continue;
        ++summary.windows;
        summary.tabs+=tabs;
    }
    return summary;
}

// The selection as launch jobs: one per window that still has a selected tab,
// carrying only the selected tabs, headed for the window's own desktop.
inline bool BuildReopenJobsFromSelection(const ReopenModel& model,
                                         size_t exeCharCost,
                                         std::vector<ReopenWindowJob>& output,
                                         size_t& skippedTabs){
    std::vector<ReopenWindowJob> jobs;
    skippedTabs=0;
    try {
        const std::vector<size_t> windows=ReopenSelectedWindows(model);
        for(size_t i=0;i<windows.size();++i){
            const size_t index=windows[i];
            const ReopenWindowItem& item=model.windows[index];
            const SnapWindow& window=model.snapshot.windows[index];
            ReopenWindowJob job;
            job.app=window.app;
            job.recordId=window.recordId;
            job.desktop=window.desktop;
            job.deskIndex=window.deskIndex;
            job.activeTitle=window.activeTitle;
            for(size_t t=0;t<item.tabs.size() && t<window.tabs.size();++t){
                if(!item.tabs[t].checked) continue;
                std::string sanitized;
                if(!SanitizeReopenUrl(window.tabs[t].url,sanitized)){
                    ++skippedTabs;
                    continue;
                }
                job.urls.push_back(sanitized);
            }
            if(job.urls.empty()) continue;
            job.launches=BuildReopenLaunches(job.app,job.urls,exeCharCost);
            if(job.launches.empty()) continue;
            jobs.push_back(std::move(job));
        }
    } catch(...) { return false; }
    output.swap(jobs);
    return true;
}

// After a reopen the selected tabs are open, so they must not be offered for
// duplication again.
inline void ReopenMarkJobsOpen(ReopenModel& model,
                               const std::vector<ReopenWindowJob>& jobs){
    std::map<std::string,std::set<std::string> > opened;
    for(size_t j=0;j<jobs.size();++j)
        for(size_t u=0;u<jobs[j].urls.size();++u)
            opened[jobs[j].app].insert(jobs[j].urls[u]);
    for(size_t w=0;w<model.windows.size() && w<model.snapshot.windows.size();++w){
        const SnapWindow& window=model.snapshot.windows[w];
        for(size_t t=0;t<model.windows[w].tabs.size() && t<window.tabs.size();++t){
            if(!ReopenTabIsOpen(opened,window.app,window.tabs[t].url)) continue;
            model.windows[w].tabs[t].open=true;
            model.windows[w].tabs[t].checked=false;
        }
    }
}
