// tabsnap.hpp — browser session snapshots ("checkpoints") and the pure logic
// that turns one back into browser launch commands.
//
// The layout store (layout.hpp) remembers *where* a browser window lives: its
// virtual desktop plus a fingerprint used to recognize the window again.  It
// deliberately does not remember *what* the window contained, so it can never
// bring a window back once the browser forgot it.
//
// A session snapshot is the complementary record: for every tracked browser
// window it keeps the ordered tab list (full URL + title), the active tab, and
// the desktop the window was on.  Snapshots are written to a small rotating
// history — one "saved" slot plus the last four "exit" slots — so the user can
// reopen a whole browsing layout after the browser (or the machine) lost it.
//
// Everything here is pure logic (no COM, no GUI, no file system) so it can be
// unit-tested; see tests/vdtest.cpp.
#pragma once

#include "layout.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------- limits ----
static const size_t MAX_SNAPSHOT_WINDOWS = 512;
static const size_t MAX_SNAPSHOT_TABS_PER_WINDOW = 2000;
static const size_t MAX_SNAPSHOT_TABS_TOTAL = 20000;
static const size_t MAX_SNAPSHOT_URL_BYTES = 8192;
static const size_t MAX_SNAPSHOT_TITLE_BYTES = 4096;
static const unsigned long long MAX_SNAPSHOT_FILE_BYTES = 16ULL * 1024ULL * 1024ULL;
// Windows accepts 32767 characters; stay far below so a chunk can never be
// truncated by quoting overhead.
static const size_t MAX_REOPEN_COMMAND_CHARS = 8000;
static const size_t MAX_SNAPSHOT_EXIT_SLOTS = 4;

// ----------------------------------------------------------------- model ----
struct SnapTab {
    std::string url;
    std::string title;
};

struct SnapWindow {
    std::string app;            // "firefox" | "chrome" | "msedge"
    std::string recordId;       // owning layout record, empty when unknown
    GUID desktop = {0};
    int deskIndex = -1;
    int activeTab = -1;         // index into tabs, -1 when unknown
    std::string activeTitle;
    std::vector<SnapTab> tabs;
};

enum class SnapKind { Saved, Exit };

struct SessionSnapshot {
    SnapKind kind = SnapKind::Saved;
    UnixSeconds capturedUtc = 0;
    std::vector<DeskRec> desks;
    std::vector<SnapWindow> windows;
};

inline const char* SnapKindTag(SnapKind kind){
    return kind==SnapKind::Exit ? "exit" : "saved";
}

inline bool ParseSnapKindTag(const std::string& tag, SnapKind& out){
    if(tag=="saved"){ out=SnapKind::Saved; return true; }
    if(tag=="exit"){ out=SnapKind::Exit; return true; }
    return false;
}

inline size_t SnapshotTabCount(const SessionSnapshot& snapshot){
    size_t total=0;
    for(size_t i=0;i<snapshot.windows.size();++i)
        total+=snapshot.windows[i].tabs.size();
    return total;
}

inline size_t SnapshotWindowCountForApp(const SessionSnapshot& snapshot,
                                        const std::string& app){
    size_t total=0;
    for(size_t i=0;i<snapshot.windows.size();++i)
        if(snapshot.windows[i].app==app) ++total;
    return total;
}

inline std::set<std::string> SnapshotApps(const SessionSnapshot& snapshot){
    std::set<std::string> apps;
    for(size_t i=0;i<snapshot.windows.size();++i)
        if(!snapshot.windows[i].app.empty()) apps.insert(snapshot.windows[i].app);
    return apps;
}

// ------------------------------------------------------------ validation ----
inline bool ValidSnapshotWindow(const SnapWindow& window){
    if(!IsSupportedLayoutApp(window.app)) return false;
    if(GuidIsZero(window.desktop)) return false;
    if(window.tabs.size()>MAX_SNAPSHOT_TABS_PER_WINDOW) return false;
    if(window.activeTab< -1 ||
       (window.activeTab>=0 &&
        (size_t)window.activeTab>=window.tabs.size())) return false;
    if(window.activeTitle.size()>MAX_SNAPSHOT_TITLE_BYTES) return false;
    if(!window.recordId.empty()){
        GUID parsed{};
        if(!ParseNonzeroLayoutGuid(window.recordId,parsed)) return false;
    }
    for(size_t i=0;i<window.tabs.size();++i){
        if(window.tabs[i].url.size()>MAX_SNAPSHOT_URL_BYTES) return false;
        if(window.tabs[i].title.size()>MAX_SNAPSHOT_TITLE_BYTES) return false;
    }
    return true;
}

inline bool ValidSnapshot(const SessionSnapshot& snapshot){
    if(snapshot.capturedUtc<=0) return false;
    if(snapshot.windows.size()>MAX_SNAPSHOT_WINDOWS) return false;
    if(SnapshotTabCount(snapshot)>MAX_SNAPSHOT_TABS_TOTAL) return false;
    for(size_t i=0;i<snapshot.windows.size();++i)
        if(!ValidSnapshotWindow(snapshot.windows[i])) return false;
    return true;
}

// --------------------------------------------------------- serialization ----
// Text, one record per line, tab separated, free text base64 encoded — the
// same shape layout.hpp uses so both files stay hand-readable and diffable.
//
//   # VDE session snapshot v1
//   M <kind> <capturedUtc>
//   D <index> <{desktop guid}> <b64 name>
//   W <app> <recordId|-> <deskIndex> <{desktop guid}> <activeTab> <b64 title>
//   T <b64 url> <b64 title>          (belongs to the preceding W)
inline std::string SerializeSessionSnapshot(const SessionSnapshot& snapshot){
    std::string out="# VDE session snapshot v1\n";
    out+="M\t"; out+=SnapKindTag(snapshot.kind); out+="\t";
    out+=std::to_string(snapshot.capturedUtc); out+="\n";
    for(size_t i=0;i<snapshot.desks.size();++i){
        const DeskRec& desk=snapshot.desks[i];
        out+="D\t"; out+=std::to_string(desk.index); out+="\t";
        out+=W2U8(GuidToString(desk.guid)); out+="\t";
        out+=b64enc(W2U8(desk.name)); out+="\n";
    }
    for(size_t i=0;i<snapshot.windows.size();++i){
        const SnapWindow& window=snapshot.windows[i];
        out+="W\t"; out+=window.app; out+="\t";
        out+=window.recordId.empty()?std::string("-"):window.recordId; out+="\t";
        out+=std::to_string(window.deskIndex); out+="\t";
        out+=W2U8(GuidToString(window.desktop)); out+="\t";
        out+=std::to_string(window.activeTab); out+="\t";
        out+=b64enc(window.activeTitle); out+="\n";
        for(size_t t=0;t<window.tabs.size();++t){
            out+="T\t"; out+=b64enc(window.tabs[t].url); out+="\t";
            out+=b64enc(window.tabs[t].title); out+="\n";
        }
    }
    return out;
}

inline bool ParseSessionSnapshot(const std::string& data,
                                 SessionSnapshot& output,
                                 std::string* errorOut=nullptr){
    SessionSnapshot parsed;
    bool headerSeen=false,metaSeen=false;
    size_t position=0,lineNumber=0,tabTotal=0;
    auto fail=[&](const std::string& message)->bool{
        if(errorOut) *errorOut="line "+std::to_string(lineNumber)+": "+message;
        return false;
    };
    if((unsigned long long)data.size()>MAX_SNAPSHOT_FILE_BYTES){
        if(errorOut) *errorOut="snapshot exceeds the 16 MiB limit";
        return false;
    }
    try {
        while(position<data.size()){
            size_t newline=data.find('\n',position);
            std::string line=data.substr(position,
                (newline==std::string::npos?data.size():newline)-position);
            position=newline==std::string::npos?data.size():newline+1;
            ++lineNumber;
            if(!line.empty() && line.back()=='\r') line.pop_back();
            if(line.empty()) continue;
            if(line[0]=='#'){
                if(headerSeen) return fail("duplicate header");
                if(line!="# VDE session snapshot v1")
                    return fail("unsupported snapshot header");
                headerSeen=true;
                continue;
            }
            if(!headerSeen) return fail("record before header");
            std::vector<std::string> fields;
            for(size_t fieldPos=0;;){
                size_t tab=line.find('\t',fieldPos);
                fields.push_back(line.substr(fieldPos,
                    (tab==std::string::npos?line.size():tab)-fieldPos));
                if(tab==std::string::npos) break;
                fieldPos=tab+1;
            }
            if(fields[0]=="M"){
                if(metaSeen) return fail("duplicate meta record");
                if(fields.size()!=3) return fail("meta record needs 3 fields");
                if(!ParseSnapKindTag(fields[1],parsed.kind))
                    return fail("unknown snapshot kind");
                long long captured=0;
                if(!ParseI64Strict(fields[2],captured) || captured<=0)
                    return fail("invalid capture time");
                parsed.capturedUtc=(UnixSeconds)captured;
                metaSeen=true;
                continue;
            }
            if(!metaSeen) return fail("record before meta");
            if(fields[0]=="D"){
                if(fields.size()!=4) return fail("desktop record needs 4 fields");
                DeskRec desk;
                if(!ParseIntStrict(fields[1],desk.index) || desk.index<0)
                    return fail("invalid desktop index");
                if(!StringToGuid(U82W(fields[2]),desk.guid) || GuidIsZero(desk.guid))
                    return fail("invalid desktop GUID");
                std::string name;
                if(!b64decStrict(fields[3],name)) return fail("invalid desktop name");
                desk.name=U82W(name);
                parsed.desks.push_back(desk);
                continue;
            }
            if(fields[0]=="W"){
                if(fields.size()!=7) return fail("window record needs 7 fields");
                if(parsed.windows.size()>=MAX_SNAPSHOT_WINDOWS)
                    return fail("too many windows");
                SnapWindow window;
                window.app=fields[1];
                if(!IsSupportedLayoutApp(window.app)) return fail("unsupported app");
                if(fields[2]!="-"){
                    GUID id{};
                    std::string canonical;
                    if(!ParseNonzeroLayoutGuid(fields[2],id,&canonical))
                        return fail("invalid record ID");
                    window.recordId=canonical;
                }
                if(!ParseIntStrict(fields[3],window.deskIndex))
                    return fail("invalid desktop index");
                if(!StringToGuid(U82W(fields[4]),window.desktop) ||
                   GuidIsZero(window.desktop))
                    return fail("invalid window desktop GUID");
                if(!ParseIntStrict(fields[5],window.activeTab) ||
                   window.activeTab< -1)
                    return fail("invalid active tab");
                if(!b64decStrict(fields[6],window.activeTitle))
                    return fail("invalid active title");
                if(window.activeTitle.size()>MAX_SNAPSHOT_TITLE_BYTES)
                    return fail("active title too long");
                parsed.windows.push_back(std::move(window));
                continue;
            }
            if(fields[0]=="T"){
                if(fields.size()!=3) return fail("tab record needs 3 fields");
                if(parsed.windows.empty()) return fail("tab record without a window");
                SnapWindow& window=parsed.windows.back();
                if(window.tabs.size()>=MAX_SNAPSHOT_TABS_PER_WINDOW)
                    return fail("too many tabs in one window");
                if(tabTotal>=MAX_SNAPSHOT_TABS_TOTAL)
                    return fail("too many tabs in the snapshot");
                SnapTab tab;
                if(!b64decStrict(fields[1],tab.url)) return fail("invalid tab URL");
                if(!b64decStrict(fields[2],tab.title)) return fail("invalid tab title");
                if(tab.url.size()>MAX_SNAPSHOT_URL_BYTES) return fail("tab URL too long");
                if(tab.title.size()>MAX_SNAPSHOT_TITLE_BYTES) return fail("tab title too long");
                window.tabs.push_back(std::move(tab));
                ++tabTotal;
                continue;
            }
            return fail("unknown record type");
        }
    } catch(...) {
        if(errorOut) *errorOut="out of memory parsing the snapshot";
        return false;
    }
    if(!headerSeen || !metaSeen){
        if(errorOut) *errorOut="snapshot is missing its header";
        return false;
    }
    for(size_t i=0;i<parsed.windows.size();++i){
        const SnapWindow& window=parsed.windows[i];
        if(window.activeTab>=0 && (size_t)window.activeTab>=window.tabs.size()){
            if(errorOut) *errorOut="active tab is out of range";
            return false;
        }
    }
    output=std::move(parsed);
    if(errorOut) errorOut->clear();
    return true;
}

// ------------------------------------------------------- URL sanitation -----
// Only URLs a browser will accept from a command line are replayed.  Anything
// carrying a quote, whitespace or a control character is dropped rather than
// escaped: a snapshot must never be able to inject extra command-line
// arguments into a browser launch, and scriptable schemes must never be
// replayed at all.
inline bool ReopenSchemeAllowed(const std::string& url){
    static const char* allowed[]={"http://","https://","ftp://","file:///"};
    for(size_t i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i){
        const std::string prefix=allowed[i];
        if(url.size()<=prefix.size()) continue;
        bool match=true;
        for(size_t c=0;c<prefix.size();++c){
            char value=url[c];
            if(value>='A' && value<='Z') value=(char)(value-'A'+'a');
            if(value!=prefix[c]){ match=false; break; }
        }
        if(match) return true;
    }
    return false;
}

inline bool SanitizeReopenUrl(const std::string& url, std::string& output){
    output.clear();
    if(url.empty() || url.size()>MAX_SNAPSHOT_URL_BYTES) return false;
    if(url[0]=='-') return false;                     // never looks like a switch
    for(size_t i=0;i<url.size();++i){
        const unsigned char value=(unsigned char)url[i];
        if(value<0x20 || value==0x7f) return false;    // control characters
        if(value==' ' || value=='"' || value=='\\') return false;
    }
    if(!ReopenSchemeAllowed(url)) return false;
    output=url;
    return true;
}

// -------------------------------------------------------- reopen planning ---
// One CreateProcess invocation.  The first launch of a window carries the
// "new window" switch; the rest add tabs to the window that launch created.
struct ReopenLaunch {
    bool newWindow=false;
    std::vector<std::string> urls;
};

struct ReopenWindowJob {
    std::string app;
    std::string recordId;
    GUID desktop={0};
    int deskIndex=-1;
    std::string activeTitle;
    std::vector<std::string> urls;      // sanitized, in tab order
    std::vector<ReopenLaunch> launches;
};

struct ReopenPlan {
    std::vector<ReopenWindowJob> jobs;
    size_t skippedWindows=0;            // no replayable tab left
    size_t skippedTabs=0;               // dropped by sanitation
};

inline size_t ReopenLaunchCharCost(const std::string& url){
    return url.size()+3;                 // space + two quotes
}

// Splits a window's URLs into invocations that each stay inside the command
// line budget.  Firefox only understands one URL after -new-window, so its
// first invocation always carries exactly one.
inline std::vector<ReopenLaunch> BuildReopenLaunches(
        const std::string& app,const std::vector<std::string>& urls,
        size_t exeCharCost,size_t maxChars=MAX_REOPEN_COMMAND_CHARS){
    std::vector<ReopenLaunch> launches;
    if(urls.empty()) return launches;
    const bool firefox=app=="firefox";
    const size_t switchCost=firefox?13:14;  // " -new-window" / " --new-window"
    size_t index=0;
    ReopenLaunch first;
    first.newWindow=true;
    first.urls.push_back(urls[0]);
    size_t used=exeCharCost+switchCost+ReopenLaunchCharCost(urls[0]);
    ++index;
    if(!firefox){
        while(index<urls.size()){
            const size_t cost=ReopenLaunchCharCost(urls[index]);
            if(used+cost>maxChars) break;
            first.urls.push_back(urls[index]);
            used+=cost;
            ++index;
        }
    }
    launches.push_back(std::move(first));
    while(index<urls.size()){
        ReopenLaunch batch;
        size_t batchUsed=exeCharCost;
        while(index<urls.size()){
            const size_t cost=ReopenLaunchCharCost(urls[index]);
            if(!batch.urls.empty() && batchUsed+cost>maxChars) break;
            batch.urls.push_back(urls[index]);
            batchUsed+=cost;
            ++index;
        }
        launches.push_back(std::move(batch));
    }
    return launches;
}

inline bool BuildReopenPlan(const SessionSnapshot& snapshot,
                            const std::set<std::string>& apps,
                            size_t exeCharCost,ReopenPlan& output){
    ReopenPlan plan;
    try {
        for(size_t i=0;i<snapshot.windows.size();++i){
            const SnapWindow& window=snapshot.windows[i];
            if(!apps.count(window.app)) continue;
            ReopenWindowJob job;
            job.app=window.app;
            job.recordId=window.recordId;
            job.desktop=window.desktop;
            job.deskIndex=window.deskIndex;
            job.activeTitle=window.activeTitle;
            for(size_t t=0;t<window.tabs.size();++t){
                std::string sanitized;
                if(!SanitizeReopenUrl(window.tabs[t].url,sanitized)){
                    ++plan.skippedTabs;
                    continue;
                }
                job.urls.push_back(sanitized);
            }
            if(job.urls.empty()){
                ++plan.skippedWindows;
                continue;
            }
            job.launches=BuildReopenLaunches(job.app,job.urls,exeCharCost);
            if(job.launches.empty()){
                ++plan.skippedWindows;
                continue;
            }
            plan.jobs.push_back(std::move(job));
        }
    } catch(...) { return false; }
    output=std::move(plan);
    return true;
}

// Command line for one invocation.  argv[0] is quoted; every URL is quoted and
// already known to hold no quote, backslash or space (SanitizeReopenUrl).
inline std::wstring BuildReopenCommandLine(const std::string& app,
                                           const std::wstring& exePath,
                                           const ReopenLaunch& launch){
    std::wstring command=L"\""+exePath+L"\"";
    if(launch.newWindow) command+=app=="firefox"?L" -new-window":L" --new-window";
    for(size_t i=0;i<launch.urls.size();++i){
        command+=L" \"";
        command+=U82W(launch.urls[i]);
        command+=L"\"";
    }
    return command;
}

// ------------------------------------------------------- checkpoint slots ---
// Slot 0 is the rolling "last saved" state; slots 1..4 are the last four
// shutdowns, newest first.
struct SnapshotSlot {
    size_t index=0;                     // 0 = saved, 1..4 = exit history
    bool present=false;
    SnapKind kind=SnapKind::Saved;
    UnixSeconds capturedUtc=0;
    size_t windows=0;
    size_t tabs=0;
    std::set<std::string> apps;
};

inline std::wstring SnapshotSlotFileName(size_t index){
    if(index==0) return L"session-saved.txt";
    return L"session-exit-"+std::to_wstring(index)+L".txt";
}

inline SnapshotSlot DescribeSnapshotSlot(size_t index,
                                         const SessionSnapshot& snapshot){
    SnapshotSlot slot;
    slot.index=index;
    slot.present=true;
    slot.kind=snapshot.kind;
    slot.capturedUtc=snapshot.capturedUtc;
    slot.windows=snapshot.windows.size();
    slot.tabs=SnapshotTabCount(snapshot);
    slot.apps=SnapshotApps(snapshot);
    return slot;
}

// Rotation order for the exit history: the oldest slot is dropped first so a
// failed rename can never overwrite a newer snapshot with an older one.
inline std::vector<std::pair<size_t,size_t> > SnapshotRotationSteps(
        size_t exitSlots=MAX_SNAPSHOT_EXIT_SLOTS){
    std::vector<std::pair<size_t,size_t> > steps;
    for(size_t target=exitSlots;target>1;--target)
        steps.push_back(std::make_pair(target-1,target));
    return steps;
}
