// tab_strip.hpp -- a Chromium browser's tabs, read from its own window.
//
// Chrome and Edge keep their session file locked while a window is open, so VDE
// cannot read it.  The tab strip is still there through UI Automation: titles and
// the active tab, no addresses.  A busy browser can block the read, and asking
// costs the browser some work, so it happens on a worker thread and only when the
// picker needs it.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "window_identity.hpp"

struct TabStripRow {
    std::wstring className;
    std::wstring automationId;
    std::wstring name;
    bool selected=false;
};

struct TabStripWindow {
    WindowIdentityKey identity;
    bool ok=false;
    std::vector<std::wstring> titles;
    int activeTab=-1;
};

inline bool TabStripProgramEligible(const std::wstring& programKey) noexcept {
    static const wchar_t* const programs[]={
        L"msedge",L"chrome",L"brave",L"opera",L"chromium",
    };
    for(const wchar_t* program : programs)
        if(programKey==program) return true;
    return false;
}

// A page can expose tabs of its own, and they sit in the same tree.  A browser's
// tab is a native view: its class is Tab, EdgeTab or TabView and its id is view_N,
// while a page's class and id are whatever the page's HTML says.
inline bool TabStripRowIsBrowserTab(const TabStripRow& row){
    if(row.automationId.compare(0,5,L"view_")!=0) return false;
    if(row.className==L"TabView") return true;
    const size_t length=row.className.size();
    return length>=3 && row.className.compare(length-3,3,L"Tab")==0;
}

// The browser adds its hover card to the name: "Title - Memory usage - 20.6 MB",
// in the language of its UI.  The shape gives it away: " - words - number unit".
inline bool DropTabStripSizeEnding(std::wstring& name){
    const size_t lastDash=name.rfind(L" - ");
    if(lastDash==std::wstring::npos || lastDash==0) return false;
    size_t position=lastDash+3;
    const size_t digitsStart=position;
    while(position<name.size() &&
          (std::iswdigit(name[position]) || name[position]==L'.' || name[position]==L','))
        ++position;
    if(position==digitsStart || !std::iswdigit(name[digitsStart])) return false;
    while(position<name.size() && std::iswspace(name[position])) ++position;
    const size_t unit=name.size()-position;
    if(unit<1 || unit>3) return false;
    for(size_t i=position;i<name.size();++i)
        if(!std::iswalpha(name[i])) return false;
    const size_t labelDash=name.rfind(L" - ",lastDash-1);
    if(labelDash==std::wstring::npos || labelDash==0 || labelDash+3>=lastDash) return false;
    name.erase(labelDash);
    return true;
}

// It adds the tab's state as well.  Nothing but the word marks these, so only the
// English ones are known; in another language the word stays in the title.
inline bool DropTabStripStateEnding(std::wstring& name){
    static const wchar_t* const states[]={
        L" - Pinned",L" - Sleeping",L" - Inactive",L" - Audio playing",
        L" - Audio muted",L" - Network error",L" - Crashed",
    };
    for(const wchar_t* state : states){
        const size_t length=std::wcslen(state);
        if(name.size()>length && name.compare(name.size()-length,length,state)==0){
            name.erase(name.size()-length);
            return true;
        }
    }
    return false;
}

inline std::wstring CleanTabStripTitle(const std::wstring& name){
    std::wstring title=name;
    while(DropTabStripSizeEnding(title) || DropTabStripStateEnding(title)) {}
    return title;
}

inline void FillTabStripWindow(const std::vector<TabStripRow>& rows,TabStripWindow& output){
    output.titles.clear();
    output.activeTab=-1;
    for(const TabStripRow& row : rows){
        if(!TabStripRowIsBrowserTab(row)) continue;
        if(row.selected && output.activeTab<0)
            output.activeTab=static_cast<int>(output.titles.size());
        output.titles.push_back(CleanTabStripTitle(row.name));
    }
}

// One thread, started by the first request.  A window is read once however many
// times it was asked for while it waited.
class TabStripWorker {
public:
    struct Ops {
        std::function<void()> threadBegin;
        std::function<void()> threadEnd;
        std::function<bool(HWND,std::vector<TabStripRow>&)> read;
        std::function<bool(TabStripWindow*)> post;   // takes ownership when it returns true
    };

    explicit TabStripWorker(Ops ops):ops_(std::move(ops)){}
    ~TabStripWorker(){ Stop(); }
    TabStripWorker(const TabStripWorker&)=delete;
    TabStripWorker& operator=(const TabStripWorker&)=delete;

    bool Request(const WindowIdentityKey& identity) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) return false;
            for(const WindowIdentityKey& queued : queue_)
                if(SameIdentity(queued,identity)) return true;
            queue_.push_back(identity);
            if(!thread_.joinable()) thread_=std::thread([this](){ Run(); });
            changed_.notify_one();
            return true;
        } catch(...) { return false; }
    }

    void Stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_=true;
            queue_.clear();
        }
        changed_.notify_all();
        if(thread_.joinable()) thread_.join();
    }

private:
    void Run() noexcept {
        if(ops_.threadBegin){ try { ops_.threadBegin(); } catch(...) {} }
        for(;;){
            WindowIdentityKey identity;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock,[this](){ return stopping_ || !queue_.empty(); });
                if(stopping_) break;
                identity=queue_.front();
                queue_.pop_front();
            }
            try {
                std::unique_ptr<TabStripWindow> result(new TabStripWindow());
                result->identity=identity;
                std::vector<TabStripRow> rows;
                result->ok=ops_.read(reinterpret_cast<HWND>(identity.hwnd),rows);
                if(result->ok) FillTabStripWindow(rows,*result);
                if(ops_.post(result.get())) result.release();
            } catch(...) {}
        }
        if(ops_.threadEnd){ try { ops_.threadEnd(); } catch(...) {} }
    }

    Ops ops_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<WindowIdentityKey> queue_;
    std::thread thread_;
    bool stopping_=false;
};
