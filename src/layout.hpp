// layout.hpp — desktop/window layout records + (de)serialization + grace logic.
// Pure logic (no COM / no GUI) so it can be unit-tested (see tests/vdtest.cpp).
#pragma once
#include "str_util.hpp"
#include <string>
#include <vector>
#include <map>

struct DeskRec { int index; GUID guid; std::wstring name; };

inline std::string CountsToStr(const std::map<std::string,int>& c){ std::string s; bool f=true; for(auto& kv:c){ if(!f)s+=","; f=false; s+=kv.first+":"+std::to_string(kv.second);} return s; }
inline std::map<std::string,int> StrToCounts(const std::string& s){ std::map<std::string,int> c; size_t p=0;
    while(p<s.size()){ size_t comma=s.find(',',p); std::string item=s.substr(p,(comma==std::string::npos?s.size():comma)-p); p=(comma==std::string::npos?s.size():comma+1);
        size_t col=item.rfind(':'); if(col!=std::string::npos)c[item.substr(0,col)]=atoi(item.substr(col+1).c_str()); } return c; }

// ---- Layout v3 records ----
static const int MISSING_RUNS_MAX = 3;

struct LayoutWin {
    std::string app; int deskIndex=-1; GUID desktop={0};
    std::string activeTitle, activeDomain; int tabCount=0;
    std::map<std::string,int> counts; int missingRuns=0;
};

inline std::string SerializeLayout(const std::vector<DeskRec>& desks, const std::vector<LayoutWin>& wins){
    std::string out = "# VDE snapshot v3\n";
    for(const auto& d : desks){
        out += "D\t"; out += std::to_string(d.index); out += "\t";
        out += W2U8(GuidToString(d.guid)); out += "\t"; out += b64enc(W2U8(d.name)); out += "\n";
    }
    for(const auto& w : wins){
        out += "W\t"; out += w.app; out += "\t"; out += std::to_string(w.deskIndex); out += "\t";
        out += W2U8(GuidToString(w.desktop)); out += "\t"; out += b64enc(w.activeTitle); out += "\t";
        out += w.activeDomain; out += "\t"; out += std::to_string(w.tabCount); out += "\t";
        out += CountsToStr(w.counts); out += "\t"; out += std::to_string(w.missingRuns); out += "\n";
    }
    return out;
}

inline bool ParseLayout(const std::string& data, std::vector<DeskRec>& desks, std::vector<LayoutWin>& wins){
    int ver = 2; size_t pos = 0;
    while(pos < data.size()){
        size_t nl = data.find('\n', pos);
        std::string line = data.substr(pos, (nl==std::string::npos?data.size():nl)-pos);
        pos = (nl==std::string::npos?data.size():nl+1);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) continue;
        if(line[0]=='#'){ if(line.find("v3")!=std::string::npos) ver=3; continue; }
        std::vector<std::string> col; size_t p=0;
        for(;;){ size_t t=line.find('\t',p); col.push_back(line.substr(p,(t==std::string::npos?line.size():t)-p)); if(t==std::string::npos)break; p=t+1; }
        if(col.size()<4) continue;
        if(col[0]=="D"){ DeskRec d; d.index=atoi(col[1].c_str()); StringToGuid(U82W(col[2]),d.guid); d.name=U82W(b64dec(col[3])); desks.push_back(d); }
        else if(col[0]=="W"){
            LayoutWin w;
            if(ver>=3){
                if(col.size()<9) continue;
                w.app=col[1]; w.deskIndex=atoi(col[2].c_str()); StringToGuid(U82W(col[3]),w.desktop);
                w.activeTitle=b64dec(col[4]); w.activeDomain=col[5]; w.tabCount=atoi(col[6].c_str());
                w.counts=StrToCounts(col[7]); w.missingRuns=atoi(col[8].c_str());
            } else {
                w.app="firefox"; w.deskIndex=atoi(col[1].c_str()); StringToGuid(U82W(col[2]),w.desktop);
                w.activeTitle=b64dec(col[3]);
                if(col.size()>=7){ w.activeDomain=col[4]; w.tabCount=atoi(col[5].c_str()); w.counts=StrToCounts(col[6]); }
                w.missingRuns=0;
            }
            wins.push_back(w);
        }
    }
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
