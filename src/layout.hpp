// layout.hpp — desktop/window layout records + (de)serialization + grace logic.
// Pure logic (no COM / no GUI) so it can be unit-tested (see tests/vdtest.cpp).
#pragma once
#include "str_util.hpp"
#include <string>
#include <vector>
#include <map>
#include <set>

struct DeskRec { int index; GUID guid; std::wstring name; };

inline std::string CountsToStr(const std::map<std::string,int>& c){ std::string s; bool f=true; for(auto& kv:c){ if(!f)s+=","; f=false; s+=kv.first+":"+std::to_string(kv.second);} return s; }
inline std::map<std::string,int> StrToCounts(const std::string& s){ std::map<std::string,int> c; size_t p=0;
    while(p<s.size()){ size_t comma=s.find(',',p); std::string item=s.substr(p,(comma==std::string::npos?s.size():comma)-p); p=(comma==std::string::npos?s.size():comma+1);
        size_t col=item.rfind(':'); if(col!=std::string::npos)c[item.substr(0,col)]=atoi(item.substr(col+1).c_str()); } return c; }

// ---- Layout v4 records ----
using UnixSeconds = long long;
static const UnixSeconds WINDOW_RETENTION_SECONDS = 30LL * 24 * 60 * 60;
static const int MISSING_RUNS_MAX = 3; // transitional legacy constant

struct LayoutWin {
    std::string recordId, app; int deskIndex=-1; GUID desktop={0};
    std::string activeTitle, activeDomain; int tabCount=0;
    std::map<std::string,int> counts;
    UnixSeconds lastSeenUtc=0, missingSinceUtc=0;
    int missingRuns=0; // transitional legacy field; ignored by v4 serialization
};

inline std::string SerializeLayout(const std::vector<DeskRec>& desks, const std::vector<LayoutWin>& wins){
    std::string out = "# VDE snapshot v4\n";
    for(const auto& d : desks){
        out += "D\t"; out += std::to_string(d.index); out += "\t";
        out += W2U8(GuidToString(d.guid)); out += "\t"; out += b64enc(W2U8(d.name)); out += "\n";
    }
    for(const auto& w : wins){
        out += "W\t"; out += w.app; out += "\t"; out += w.recordId; out += "\t";
        out += std::to_string(w.deskIndex); out += "\t";
        out += W2U8(GuidToString(w.desktop)); out += "\t"; out += b64enc(w.activeTitle); out += "\t";
        out += w.activeDomain; out += "\t"; out += std::to_string(w.tabCount); out += "\t";
        out += CountsToStr(w.counts); out += "\t"; out += std::to_string(w.lastSeenUtc); out += "\t";
        out += std::to_string(w.missingSinceUtc); out += "\n";
    }
    return out;
}

inline std::string NewRecordId(){
    GUID id{};
    if(FAILED(CoCreateGuid(&id))) return std::string();
    return W2U8(GuidToString(id));
}

using RecordIdGenerator = std::string (*)();
inline bool PrepareTransitionalV4Records(std::vector<LayoutWin>& records, UnixSeconds nowUtc,
        std::string* errorOut=nullptr, RecordIdGenerator idGenerator=NewRecordId){
    std::vector<LayoutWin> prepared = records;
    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut = message;
        return false;
    };
    for(auto& record : prepared){
        if(GuidIsZero(record.desktop)) return fail("window record has a zero desktop GUID");
        if(record.lastSeenUtc<0) return fail("window record has a negative last-seen time");
        if(record.recordId.empty()){
            if(!idGenerator) return fail("record ID generator is unavailable");
            record.recordId = idGenerator();
            if(record.recordId.empty()) return fail("failed to generate record ID");
        }
        if(record.lastSeenUtc==0){
            if(nowUtc<=0) return fail("cannot initialize last-seen time from a nonpositive clock");
            record.lastSeenUtc = nowUtc;
        }
        if(record.missingRuns>0 && record.missingSinceUtc==0) record.missingSinceUtc = nowUtc;
    }
    records.swap(prepared);
    if(errorOut) errorOut->clear();
    return true;
}

inline bool ParseLayout(const std::string& data, std::vector<DeskRec>& desksOut, std::vector<LayoutWin>& winsOut,
        UnixSeconds migrationNow, std::string* errorOut=nullptr, int* sourceVersionOut=nullptr){
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::set<std::string> recordIds;
    int version = 0;
    bool headerSeen = false, recordsSeen = false;
    size_t recordCount = 0, lineNumber = 0, pos = 0;

    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut = message;
        return false;
    };
    auto failLine = [&](const std::string& message)->bool {
        return fail("line " + std::to_string(lineNumber) + ": " + message);
    };
    auto validApp = [](const std::string& app)->bool {
        return app=="firefox" || app=="chrome" || app=="msedge";
    };
    auto parseNonzeroGuid = [](const std::string& text, GUID& guid)->bool {
        size_t offset = 0;
        if(text.size()==38){
            if(text.front()!='{' || text.back()!='}') return false;
            offset = 1;
        } else if(text.size()!=36) return false;
        auto isHex = [](char c)->bool {
            return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
        };
        for(size_t i=0;i<36;++i){
            bool dash = i==8 || i==13 || i==18 || i==23;
            char c = text[offset+i];
            if(dash ? c!='-' : !isHex(c)) return false;
        }
        std::string canonical = offset ? text : ("{" + text + "}");
        GUID parsed{};
        if(!StringToGuid(U82W(canonical), parsed) || GuidIsZero(parsed)) return false;
        guid = parsed;
        return true;
    };
    auto splitTabs = [](const std::string& line)->std::vector<std::string> {
        std::vector<std::string> fields;
        size_t fieldPos = 0;
        for(;;){
            size_t tab = line.find('\t', fieldPos);
            fields.push_back(line.substr(fieldPos, (tab==std::string::npos ? line.size() : tab) - fieldPos));
            if(tab==std::string::npos) break;
            fieldPos = tab + 1;
        }
        return fields;
    };

    while(pos < data.size()){
        ++lineNumber;
        size_t nl = data.find('\n', pos);
        size_t end = nl==std::string::npos ? data.size() : nl;
        std::string line = data.substr(pos, end - pos);
        pos = nl==std::string::npos ? data.size() : nl + 1;
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) continue;

        if(line[0]=='#'){
            if(recordsSeen) return failLine("header appears after records");
            if(headerSeen) return failLine("duplicate header");
            if(line=="# VDE snapshot v2") version=2;
            else if(line=="# VDE snapshot v3") version=3;
            else if(line=="# VDE snapshot v4") version=4;
            else return failLine("unknown or empty snapshot header");
            if(version<4 && migrationNow<=0) return failLine("legacy snapshot requires a positive migration time");
            headerSeen = true;
            continue;
        }

        if(!headerSeen) return failLine("record appears before snapshot header");
        recordsSeen = true;
        if(++recordCount > 4096) return failLine("snapshot record limit exceeded");
        std::vector<std::string> col = splitTabs(line);
        if(col.empty()) return failLine("empty record");

        if(col[0]=="D"){
            if(col.size()!=4) return failLine("desktop record must have exactly 4 fields");
            DeskRec d{};
            if(!ParseIntStrict(col[1], d.index)) return failLine("invalid desktop index");
            if(!parseNonzeroGuid(col[2], d.guid)) return failLine("invalid desktop GUID");
            std::string name;
            if(!b64decStrict(col[3], name)) return failLine("invalid desktop name encoding");
            d.name = U82W(name);
            desks.push_back(d);
            continue;
        }

        if(col[0]!="W") return failLine("unknown record type");
        LayoutWin w;
        std::string title;
        if(version==4){
            if(col.size()!=11) return failLine("v4 window record must have exactly 11 fields");
            w.app = col[1];
            if(!validApp(w.app)) return failLine("unsupported window app");
            GUID id{};
            if(!parseNonzeroGuid(col[2], id)) return failLine("invalid record ID");
            std::string idKey = W2U8(GuidToString(id));
            if(!recordIds.insert(idKey).second) return failLine("duplicate record ID");
            w.recordId = col[2];
            if(!ParseIntStrict(col[3], w.deskIndex)) return failLine("invalid window desktop index");
            if(!parseNonzeroGuid(col[4], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[5], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[6];
            if(!ParseIntStrict(col[7], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[8], w.counts)) return failLine("invalid window domain counts");
            if(!ParseI64Strict(col[9], w.lastSeenUtc) || w.lastSeenUtc<=0) return failLine("invalid last-seen time");
            if(!ParseI64Strict(col[10], w.missingSinceUtc) || w.missingSinceUtc<0) return failLine("invalid missing-since time");
        } else if(version==3){
            if(col.size()!=9) return failLine("v3 window record must have exactly 9 fields");
            w.app = col[1];
            if(!validApp(w.app)) return failLine("unsupported window app");
            if(!ParseIntStrict(col[2], w.deskIndex)) return failLine("invalid window desktop index");
            if(!parseNonzeroGuid(col[3], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[4], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[5];
            if(!ParseIntStrict(col[6], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[7], w.counts)) return failLine("invalid window domain counts");
            int oldMissing = 0;
            if(!ParseIntStrict(col[8], oldMissing)) return failLine("invalid legacy missing-run count");
            w.recordId = NewRecordId();
            if(w.recordId.empty()) return failLine("failed to generate record ID");
            w.lastSeenUtc = migrationNow;
            w.missingSinceUtc = oldMissing>0 ? migrationNow : 0;
        } else {
            if(col.size()!=7) return failLine("v2 window record must have exactly 7 fields");
            w.app = "firefox";
            if(!ParseIntStrict(col[1], w.deskIndex)) return failLine("invalid window desktop index");
            if(!parseNonzeroGuid(col[2], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[3], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[4];
            if(!ParseIntStrict(col[5], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[6], w.counts)) return failLine("invalid window domain counts");
            w.recordId = NewRecordId();
            if(w.recordId.empty()) return failLine("failed to generate record ID");
            w.lastSeenUtc = migrationNow;
        }
        wins.push_back(w);
    }

    if(!headerSeen) return fail("missing or empty snapshot header");
    desksOut.swap(desks);
    winsOut.swap(wins);
    if(errorOut) errorOut->clear();
    if(sourceVersionOut) *sourceVersionOut = version;
    return true;
}

// ---- Cross-restart identity + merge/grace ----
// Key that re-identifies a window across a restart (HWND is ephemeral):
// domain multiset when available (robust — session restore recreates tabs),
// else the active-window title (generic apps without tab data).
inline std::string FingerprintKey(const std::string& app, const std::map<std::string,int>& counts, const std::string& activeTitle){
    std::string k = app + "|";
    if(!counts.empty()){ bool f=true; for(const auto& kv:counts){ if(!f)k+=","; f=false; k+=kv.first+":"+std::to_string(kv.second);} }
    else k += "t:" + activeTitle;
    return k;
}

// Merge currently-present windows into the existing auto layout WITHOUT deleting
// absent windows (anti-wipe). Present windows: desk updated, missingRuns reset to 0.
inline std::vector<LayoutWin> MergeAutoLayout(const std::vector<LayoutWin>& existing, const std::vector<LayoutWin>& present){
    std::vector<LayoutWin> out = existing;
    std::map<std::string,int> idx;
    for(size_t i=0;i<out.size();++i) idx[FingerprintKey(out[i].app,out[i].counts,out[i].activeTitle)] = (int)i;
    for(const auto& p : present){
        std::string key = FingerprintKey(p.app,p.counts,p.activeTitle);
        auto it = idx.find(key);
        if(it!=idx.end()){
            LayoutWin& e = out[it->second];
            e.deskIndex=p.deskIndex; e.desktop=p.desktop; e.activeTitle=p.activeTitle;
            e.activeDomain=p.activeDomain; e.tabCount=p.tabCount; e.counts=p.counts; e.missingRuns=0;
        } else { LayoutWin n=p; n.missingRuns=0; idx[key]=(int)out.size(); out.push_back(n); }
    }
    return out;
}

// Age the auto layout by one utility run. For apps observed this run: seen
// windows reset to 0, unseen windows increment and are dropped at maxMissing.
// Records for apps NOT observed this run are left untouched.
inline std::vector<LayoutWin> ReconcileGrace(const std::vector<LayoutWin>& records,
        const std::set<std::string>& seenKeys, const std::set<std::string>& observedApps, int maxMissing){
    std::vector<LayoutWin> out;
    for(const auto& r : records){
        if(!observedApps.count(r.app)){ out.push_back(r); continue; }
        LayoutWin w = r;
        std::string key = FingerprintKey(w.app,w.counts,w.activeTitle);
        if(seenKeys.count(key)) w.missingRuns = 0; else w.missingRuns += 1;
        if(w.missingRuns < maxMissing) out.push_back(w);
    }
    return out;
}
