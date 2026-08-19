// layout.hpp — desktop/window layout records + (de)serialization + grace logic.
// Pure logic (no COM / no GUI) so it can be unit-tested (see tests/vdtest.cpp).
#pragma once
#include "str_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <queue>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstddef>
#include <utility>

struct DeskRec { int index; GUID guid; std::wstring name; };

inline std::string CountsToStr(const std::map<std::string,int>& c){ std::string s; bool f=true; for(auto& kv:c){ if(!f)s+=","; f=false; s+=kv.first+":"+std::to_string(kv.second);} return s; }
inline std::map<std::string,int> StrToCounts(const std::string& s){ std::map<std::string,int> c; size_t p=0;
    while(p<s.size()){ size_t comma=s.find(',',p); std::string item=s.substr(p,(comma==std::string::npos?s.size():comma)-p); p=(comma==std::string::npos?s.size():comma+1);
        size_t col=item.rfind(':'); if(col!=std::string::npos)c[item.substr(0,col)]=atoi(item.substr(col+1).c_str()); } return c; }

// ---- Layout v4 records ----
using UnixSeconds = long long;
static const UnixSeconds WINDOW_RETENTION_SECONDS = 30LL * 24 * 60 * 60;
static const int MISSING_RUNS_MAX = 3; // transitional legacy constant
static const std::size_t MAX_LAYOUT_RECORDS = 4096;
static const std::size_t MAX_MATCH_CANDIDATES = 8192;

struct LayoutWin {
    std::string recordId, app; int deskIndex=-1; GUID desktop={0};
    std::string activeTitle, activeDomain; int tabCount=0;
    std::map<std::string,int> counts;
    UnixSeconds lastSeenUtc=0, missingSinceUtc=0;
    int missingRuns=0; // transitional legacy field; ignored by v4 serialization
};

struct LayoutMatch {
    size_t savedIndex=0;
    size_t liveIndex=0;
    double score=0;
};

inline bool IsExpired(const LayoutWin& window, UnixSeconds nowUtc){
    return window.missingSinceUtc>0 && nowUtc>=window.missingSinceUtc &&
        nowUtc-window.missingSinceUtc>=WINDOW_RETENTION_SECONDS;
}

inline void MarkSeen(LayoutWin& window, UnixSeconds nowUtc){
    window.lastSeenUtc=nowUtc;
    window.missingSinceUtc=0;
}

inline void MarkMissing(LayoutWin& window, UnixSeconds nowUtc){
    if(window.missingSinceUtc==0)
        window.missingSinceUtc=window.lastSeenUtc>0 ? window.lastSeenUtc : nowUtc;
}

inline std::vector<LayoutWin> PruneExpired(const std::vector<LayoutWin>& input, UnixSeconds nowUtc){
    std::vector<LayoutWin> output;
    output.reserve(input.size());
    for(const auto& window : input) if(!IsExpired(window,nowUtc)) output.push_back(window);
    return output;
}

inline double LayoutScore(const LayoutWin& saved, const LayoutWin& live){
    if(saved.app!=live.app) return 0;
    if(saved.counts.empty() || live.counts.empty())
        return !saved.activeTitle.empty() && saved.activeTitle==live.activeTitle ? 1.0 : 0.0;

    long double dot=0, savedSquares=0, liveSquares=0;
    for(const auto& item : saved.counts){
        long double value=static_cast<long double>(item.second);
        savedSquares+=value*value;
        auto other=live.counts.find(item.first);
        if(other!=live.counts.end()) dot+=value*static_cast<long double>(other->second);
    }
    for(const auto& item : live.counts){
        long double value=static_cast<long double>(item.second);
        liveSquares+=value*value;
    }
    double cosine=0;
    if(savedSquares>0 && liveSquares>0){
        long double raw=dot/(std::sqrt(savedSquares)*std::sqrt(liveSquares));
        cosine=static_cast<double>(std::max(-1.0L,std::min(1.0L,raw)));
    }

    size_t intersection=0;
    for(const auto& item : saved.counts) if(live.counts.count(item.first)) ++intersection;
    size_t unionSize=saved.counts.size()+live.counts.size()-intersection;
    double jaccard=unionSize ? static_cast<double>(intersection)/static_cast<double>(unionSize) : 0;

    double active=0;
    if(!saved.activeTitle.empty() && saved.activeTitle==live.activeTitle) active=1;
    else if(!saved.activeDomain.empty() && saved.activeDomain==live.activeDomain) active=0.5;

    long long savedTabs=saved.tabCount, liveTabs=live.tabCount;
    long long difference=savedTabs>=liveTabs ? savedTabs-liveTabs : liveTabs-savedTabs;
    long long denominator=std::max(1LL,std::max(savedTabs,liveTabs));
    double tabs=1.0-std::min(1.0,static_cast<double>(difference)/static_cast<double>(denominator));
    return 0.40*cosine+0.25*jaccard+0.20*active+0.15*tabs;
}

namespace layout_detail {

struct AssignmentCandidate {
    LayoutMatch match;
    long long scoreUnits=0;
    size_t savedNode=0;
    size_t liveNode=0;
    long long tieOrder=0;
};

inline bool ScaleMatchScore(double score, long long& scoreUnits){
    if(!std::isfinite(score) || score<0) return false;
    const long double scaled=static_cast<long double>(score)*1000000000.0L;
    const long double exclusiveLimit=9223372036854775808.0L;
    const long double roundingLimit=static_cast<long double>((std::numeric_limits<long long>::max)())-0.5L;
    if(!std::isfinite(scaled) || scaled>=exclusiveLimit || scaled>roundingLimit) return false;
    scoreUnits=std::llround(scaled);
    return scoreUnits>=0;
}

inline size_t MaximumCardinality(const std::vector<std::vector<size_t>>& adjacency, size_t liveCount){
    const size_t none=(std::numeric_limits<size_t>::max)();
    std::vector<size_t> savedMatch(adjacency.size(),none),liveMatch(liveCount,none);
    size_t cardinality=0;
    for(size_t root=0;root<adjacency.size();++root){
        if(savedMatch[root]!=none) continue;
        std::vector<char> seenSaved(adjacency.size(),0),seenLive(liveCount,0);
        std::vector<size_t> parentSaved(liveCount,none);
        std::queue<size_t> pending;
        seenSaved[root]=1;
        pending.push(root);
        size_t endpoint=none;
        while(!pending.empty() && endpoint==none){
            size_t savedNode=pending.front(); pending.pop();
            for(size_t liveNode : adjacency[savedNode]){
                if(seenLive[liveNode]) continue;
                seenLive[liveNode]=1;
                parentSaved[liveNode]=savedNode;
                if(liveMatch[liveNode]==none){ endpoint=liveNode; break; }
                size_t nextSaved=liveMatch[liveNode];
                if(!seenSaved[nextSaved]){ seenSaved[nextSaved]=1; pending.push(nextSaved); }
            }
        }
        if(endpoint==none) continue;
        for(size_t liveNode=endpoint;liveNode!=none;){
            size_t savedNode=parentSaved[liveNode];
            size_t priorLive=savedMatch[savedNode];
            savedMatch[savedNode]=liveNode;
            liveMatch[liveNode]=savedNode;
            liveNode=priorLive;
        }
        ++cardinality;
    }
    return cardinality;
}

struct WideSigned {
    bool negative=false;
    unsigned long long high=0;
    unsigned long long low=0;
};

inline WideSigned WideFromLongLong(long long value){
    WideSigned result;
    result.negative=value<0;
    result.low=result.negative ? 0ULL-static_cast<unsigned long long>(value) :
        static_cast<unsigned long long>(value);
    return result;
}

inline WideSigned WideNegated(WideSigned value){
    if(value.high!=0 || value.low!=0) value.negative=!value.negative;
    return value;
}

inline int CompareWideMagnitude(const WideSigned& left, const WideSigned& right){
    if(left.high!=right.high) return left.high<right.high ? -1 : 1;
    if(left.low!=right.low) return left.low<right.low ? -1 : 1;
    return 0;
}

inline bool WideLess(const WideSigned& left, const WideSigned& right){
    if(left.negative!=right.negative) return left.negative;
    int magnitude=CompareWideMagnitude(left,right);
    return left.negative ? magnitude>0 : magnitude<0;
}

inline bool CheckedAdd(const WideSigned& left, const WideSigned& right, WideSigned& result){
    if(left.negative==right.negative){
        result.negative=left.negative;
        result.low=left.low+right.low;
        unsigned long long carry=result.low<left.low ? 1ULL : 0ULL;
        result.high=left.high+right.high;
        if(result.high<left.high) return false;
        unsigned long long withoutCarry=result.high;
        result.high+=carry;
        if(result.high<withoutCarry) return false;
        return true;
    }
    int magnitude=CompareWideMagnitude(left,right);
    if(magnitude==0){ result=WideSigned(); return true; }
    const WideSigned& larger=magnitude>0 ? left : right;
    const WideSigned& smaller=magnitude>0 ? right : left;
    result.negative=larger.negative;
    result.low=larger.low-smaller.low;
    unsigned long long borrow=larger.low<smaller.low ? 1ULL : 0ULL;
    result.high=larger.high-smaller.high;
    if(borrow>result.high) return false;
    result.high-=borrow;
    if(result.high==0 && result.low==0) result.negative=false;
    return true;
}

struct FlowCost {
    WideSigned negativeScoreUnits;
    long long deterministicTieSum=0;
};

inline bool CostLess(const FlowCost& left, const FlowCost& right){
    return WideLess(left.negativeScoreUnits,right.negativeScoreUnits) ||
        (!WideLess(right.negativeScoreUnits,left.negativeScoreUnits) &&
         left.deterministicTieSum<right.deterministicTieSum);
}

inline bool CheckedAdd(long long left, long long right, long long& result){
    if(right>0 && left>(std::numeric_limits<long long>::max)()-right) return false;
    if(right<0 && left<(std::numeric_limits<long long>::min)()-right) return false;
    result=left+right;
    return true;
}

inline bool CheckedAdd(const FlowCost& left, const FlowCost& right, FlowCost& result){
    return CheckedAdd(left.negativeScoreUnits,right.negativeScoreUnits,result.negativeScoreUnits) &&
        CheckedAdd(left.deterministicTieSum,right.deterministicTieSum,result.deterministicTieSum);
}

struct FlowEdge {
    size_t to=0;
    size_t reverseIndex=0;
    int capacity=0;
    FlowCost cost;
};

inline size_t AddFlowEdge(std::vector<std::vector<FlowEdge>>& graph, size_t from, size_t to,
        const FlowCost& cost){
    FlowEdge forward;
    forward.to=to; forward.reverseIndex=graph[to].size(); forward.capacity=1; forward.cost=cost;
    FlowEdge reverse;
    reverse.to=from; reverse.reverseIndex=graph[from].size(); reverse.capacity=0;
    reverse.cost.negativeScoreUnits=WideNegated(cost.negativeScoreUnits);
    reverse.cost.deterministicTieSum=-cost.deterministicTieSum;
    size_t index=graph[from].size();
    graph[from].push_back(forward);
    graph[to].push_back(reverse);
    return index;
}

struct CandidateEdgeRef {
    size_t from=0;
    size_t edgeIndex=0;
    LayoutMatch match;
};

struct DisjointSet {
    std::vector<size_t> parent;
    std::vector<unsigned char> rank;
    explicit DisjointSet(size_t size):parent(size),rank(size,0){
        for(size_t index=0;index<size;++index) parent[index]=index;
    }
    size_t Find(size_t node){
        while(parent[node]!=node){ parent[node]=parent[parent[node]]; node=parent[node]; }
        return node;
    }
    void Unite(size_t left, size_t right){
        left=Find(left); right=Find(right);
        if(left==right) return;
        if(rank[left]<rank[right]) std::swap(left,right);
        parent[right]=left;
        if(rank[left]==rank[right]) ++rank[left];
    }
};

inline bool SolveFlowComponent(const std::vector<AssignmentCandidate>& candidates,
        const std::vector<size_t>& candidateIndices, size_t cardinality,
        std::vector<LayoutMatch>& result){
    if(cardinality==0) return true;
    std::vector<size_t> savedNodes,liveNodes;
    savedNodes.reserve(candidateIndices.size()); liveNodes.reserve(candidateIndices.size());
    for(size_t candidateIndex : candidateIndices){
        savedNodes.push_back(candidates[candidateIndex].savedNode);
        liveNodes.push_back(candidates[candidateIndex].liveNode);
    }
    std::sort(savedNodes.begin(),savedNodes.end());
    savedNodes.erase(std::unique(savedNodes.begin(),savedNodes.end()),savedNodes.end());
    std::sort(liveNodes.begin(),liveNodes.end());
    liveNodes.erase(std::unique(liveNodes.begin(),liveNodes.end()),liveNodes.end());

    const size_t source=0;
    const size_t savedOffset=1;
    const size_t liveOffset=savedOffset+savedNodes.size();
    const size_t sink=liveOffset+liveNodes.size();
    std::vector<std::vector<FlowEdge>> graph(sink+1);
    const FlowCost zeroCost{};
    for(size_t savedNode=0;savedNode<savedNodes.size();++savedNode)
        AddFlowEdge(graph,source,savedOffset+savedNode,zeroCost);

    std::vector<CandidateEdgeRef> candidateEdges;
    candidateEdges.reserve(candidateIndices.size());
    for(size_t candidateIndex : candidateIndices){
        const AssignmentCandidate& candidate=candidates[candidateIndex];
        FlowCost cost;
        cost.negativeScoreUnits=WideFromLongLong(-candidate.scoreUnits);
        cost.deterministicTieSum=candidate.tieOrder;
        size_t savedNode=static_cast<size_t>(std::lower_bound(
            savedNodes.begin(),savedNodes.end(),candidate.savedNode)-savedNodes.begin());
        size_t liveNode=static_cast<size_t>(std::lower_bound(
            liveNodes.begin(),liveNodes.end(),candidate.liveNode)-liveNodes.begin());
        CandidateEdgeRef edgeRef;
        edgeRef.from=savedOffset+savedNode;
        edgeRef.edgeIndex=AddFlowEdge(graph,edgeRef.from,liveOffset+liveNode,cost);
        edgeRef.match=candidate.match;
        candidateEdges.push_back(edgeRef);
    }
    for(size_t liveNode=0;liveNode<liveNodes.size();++liveNode)
        AddFlowEdge(graph,liveOffset+liveNode,sink,zeroCost);

    const size_t none=(std::numeric_limits<size_t>::max)();
    for(size_t flow=0;flow<cardinality;++flow){
        std::vector<FlowCost> distance(graph.size());
        std::vector<char> reached(graph.size(),0),inQueue(graph.size(),0);
        std::vector<size_t> previousNode(graph.size(),none),previousEdge(graph.size(),none);
        std::queue<size_t> pending;
        reached[source]=1; inQueue[source]=1; pending.push(source);
        while(!pending.empty()){
            size_t node=pending.front(); pending.pop(); inQueue[node]=0;
            for(size_t edgeIndex=0;edgeIndex<graph[node].size();++edgeIndex){
                const FlowEdge& edge=graph[node][edgeIndex];
                if(edge.capacity==0) continue;
                FlowCost next;
                if(!CheckedAdd(distance[node],edge.cost,next)) return false;
                if(reached[edge.to] && !CostLess(next,distance[edge.to])) continue;
                distance[edge.to]=next;
                reached[edge.to]=1;
                previousNode[edge.to]=node;
                previousEdge[edge.to]=edgeIndex;
                if(!inQueue[edge.to]){ inQueue[edge.to]=1; pending.push(edge.to); }
            }
        }
        if(!reached[sink]) return false;
        for(size_t node=sink;node!=source;){
            size_t prior=previousNode[node],edgeIndex=previousEdge[node];
            if(prior==none || edgeIndex==none) return false;
            FlowEdge& edge=graph[prior][edgeIndex];
            --edge.capacity;
            ++graph[node][edge.reverseIndex].capacity;
            node=prior;
        }
    }

    size_t priorSize=result.size();
    for(const auto& edgeRef : candidateEdges)
        if(graph[edgeRef.from][edgeRef.edgeIndex].capacity==0) result.push_back(edgeRef.match);
    return result.size()-priorSize==cardinality;
}

} // namespace layout_detail

inline std::vector<LayoutMatch> AssignOneToOne(size_t savedCount, size_t liveCount,
        const std::vector<LayoutMatch>& inputCandidates, bool* tooComplex=nullptr){
    if(tooComplex) *tooComplex=false;
    auto fail = [&]()->std::vector<LayoutMatch> {
        if(tooComplex) *tooComplex=true;
        return std::vector<LayoutMatch>();
    };
    if(savedCount==0 || liveCount==0 || inputCandidates.empty()) return std::vector<LayoutMatch>();

    std::map<std::pair<size_t,size_t>,double> bestScores;
    for(const auto& candidate : inputCandidates){
        if(candidate.savedIndex>=savedCount || candidate.liveIndex>=liveCount ||
                !std::isfinite(candidate.score) || candidate.score<0) continue;
        std::pair<size_t,size_t> key(candidate.savedIndex,candidate.liveIndex);
        auto existing=bestScores.find(key);
        if(existing!=bestScores.end()){
            if(candidate.score>existing->second) existing->second=candidate.score;
            continue;
        }
        if(bestScores.size()>=MAX_MATCH_CANDIDATES) return fail();
        bestScores.insert(std::make_pair(key,candidate.score));
    }
    if(bestScores.empty()) return std::vector<LayoutMatch>();

    std::vector<size_t> savedNodes,liveNodes;
    savedNodes.reserve(bestScores.size()); liveNodes.reserve(bestScores.size());
    for(const auto& item : bestScores){
        savedNodes.push_back(item.first.first);
        liveNodes.push_back(item.first.second);
    }
    std::sort(savedNodes.begin(),savedNodes.end());
    savedNodes.erase(std::unique(savedNodes.begin(),savedNodes.end()),savedNodes.end());
    std::sort(liveNodes.begin(),liveNodes.end());
    liveNodes.erase(std::unique(liveNodes.begin(),liveNodes.end()),liveNodes.end());

    std::vector<layout_detail::AssignmentCandidate> candidates;
    candidates.reserve(bestScores.size());
    size_t candidateOrder=0;
    for(const auto& item : bestScores){
        layout_detail::AssignmentCandidate candidate;
        candidate.match.savedIndex=item.first.first;
        candidate.match.liveIndex=item.first.second;
        candidate.match.score=item.second;
        if(!layout_detail::ScaleMatchScore(item.second,candidate.scoreUnits)) return fail();
        candidate.savedNode=static_cast<size_t>(std::lower_bound(savedNodes.begin(),savedNodes.end(),item.first.first)-savedNodes.begin());
        candidate.liveNode=static_cast<size_t>(std::lower_bound(liveNodes.begin(),liveNodes.end(),item.first.second)-liveNodes.begin());
        candidate.tieOrder=static_cast<long long>(++candidateOrder);
        candidates.push_back(candidate);
    }

    layout_detail::DisjointSet partitions(savedNodes.size()+liveNodes.size());
    for(const auto& candidate : candidates)
        partitions.Unite(candidate.savedNode,savedNodes.size()+candidate.liveNode);
    std::map<size_t,std::vector<size_t>> groupedCandidates;
    for(size_t candidateIndex=0;candidateIndex<candidates.size();++candidateIndex)
        groupedCandidates[partitions.Find(candidates[candidateIndex].savedNode)].push_back(candidateIndex);

    struct ComponentPlan {
        std::vector<size_t> candidateIndices;
        size_t cardinality=0;
    };
    std::vector<ComponentPlan> plans;
    plans.reserve(groupedCandidates.size());
    size_t cardinality=0;
    for(const auto& group : groupedCandidates){
        std::vector<size_t> componentSaved,componentLive;
        componentSaved.reserve(group.second.size()); componentLive.reserve(group.second.size());
        for(size_t candidateIndex : group.second){
            componentSaved.push_back(candidates[candidateIndex].savedNode);
            componentLive.push_back(candidates[candidateIndex].liveNode);
        }
        std::sort(componentSaved.begin(),componentSaved.end());
        componentSaved.erase(std::unique(componentSaved.begin(),componentSaved.end()),componentSaved.end());
        std::sort(componentLive.begin(),componentLive.end());
        componentLive.erase(std::unique(componentLive.begin(),componentLive.end()),componentLive.end());
        std::vector<std::vector<size_t>> adjacency(componentSaved.size());
        for(size_t candidateIndex : group.second){
            const auto& candidate=candidates[candidateIndex];
            size_t savedNode=static_cast<size_t>(std::lower_bound(componentSaved.begin(),componentSaved.end(),candidate.savedNode)-componentSaved.begin());
            size_t liveNode=static_cast<size_t>(std::lower_bound(componentLive.begin(),componentLive.end(),candidate.liveNode)-componentLive.begin());
            adjacency[savedNode].push_back(liveNode);
        }
        ComponentPlan plan;
        plan.candidateIndices=group.second;
        plan.cardinality=layout_detail::MaximumCardinality(adjacency,componentLive.size());
        if(plan.cardinality==0) return fail();
        cardinality+=plan.cardinality;
        plans.push_back(plan);
    }

    std::vector<LayoutMatch> result;
    result.reserve(cardinality);
    for(const auto& plan : plans)
        if(!layout_detail::SolveFlowComponent(candidates,plan.candidateIndices,plan.cardinality,result)) return fail();
    if(result.size()!=cardinality) return fail();
    std::sort(result.begin(),result.end(),[](const LayoutMatch& left, const LayoutMatch& right){
        return left.savedIndex<right.savedIndex ||
            (left.savedIndex==right.savedIndex && left.liveIndex<right.liveIndex);
    });
    return result;
}

inline std::vector<LayoutMatch> MatchOneToOne(const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live, double acceptScore, bool* tooComplex=nullptr){
    if(tooComplex) *tooComplex=false;
    std::vector<LayoutMatch> candidates;
    for(size_t savedIndex=0;savedIndex<saved.size();++savedIndex){
        for(size_t liveIndex=0;liveIndex<live.size();++liveIndex){
            if(saved[savedIndex].app!=live[liveIndex].app) continue;
            double score=LayoutScore(saved[savedIndex],live[liveIndex]);
            if(!(score>=acceptScore)) continue;
            if(candidates.size()>=MAX_MATCH_CANDIDATES){
                if(tooComplex) *tooComplex=true;
                return std::vector<LayoutMatch>();
            }
            LayoutMatch candidate;
            candidate.savedIndex=savedIndex; candidate.liveIndex=liveIndex; candidate.score=score;
            candidates.push_back(candidate);
        }
    }
    return AssignOneToOne(saved.size(),live.size(),candidates,tooComplex);
}

inline bool ParseNonzeroLayoutGuid(const std::string& text, GUID& guid, std::string* canonicalOut=nullptr){
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
    std::string canonicalText = offset ? text : ("{" + text + "}");
    GUID parsed{};
    if(!StringToGuid(U82W(canonicalText), parsed) || GuidIsZero(parsed)) return false;
    guid = parsed;
    if(canonicalOut) *canonicalOut=W2U8(GuidToString(parsed));
    return true;
}

inline bool IsSupportedLayoutApp(const std::string& app){
    return app=="firefox" || app=="chrome" || app=="msedge";
}

inline bool HasLayoutFieldBreak(const std::string& value){
    return value.find_first_of("\t\r\n")!=std::string::npos;
}

inline bool AreLayoutCountsSerializable(const std::map<std::string,int>& counts){
    for(const auto& item : counts){
        if(item.first.empty() || item.first.find_first_of(",\t\r\n")!=std::string::npos || item.second<=0)
            return false;
    }
    return true;
}

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
    std::set<std::string> recordIds;
    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut = message;
        return false;
    };
    for(auto& record : prepared){
        if(!IsSupportedLayoutApp(record.app)) return fail("window record has an unsupported app");
        if(GuidIsZero(record.desktop)) return fail("window record has a zero desktop GUID");
        if(HasLayoutFieldBreak(record.activeDomain)) return fail("window record domain contains a field delimiter");
        if(record.tabCount<0) return fail("window record has a negative tab count");
        if(!AreLayoutCountsSerializable(record.counts)) return fail("window record has invalid domain counts");
        if(record.lastSeenUtc<0) return fail("window record has a negative last-seen time");
        if(record.missingSinceUtc<0) return fail("window record has a negative missing-since time");
        if(record.missingRuns<0) return fail("window record has a negative legacy missing-run count");
        if(record.recordId.empty()){
            if(!idGenerator) return fail("record ID generator is unavailable");
            record.recordId = idGenerator();
            if(record.recordId.empty()) return fail("failed to generate record ID");
        }
        GUID id{}; std::string idKey;
        if(!ParseNonzeroLayoutGuid(record.recordId,id,&idKey)) return fail("window record has an invalid record ID");
        if(!recordIds.insert(idKey).second) return fail("window records contain a duplicate record ID");
        if(record.lastSeenUtc==0){
            if(nowUtc<=0) return fail("cannot initialize last-seen time from a nonpositive clock");
            record.lastSeenUtc = nowUtc;
        }
        if(record.missingRuns>0 && record.missingSinceUtc==0){
            if(nowUtc<=0) return fail("cannot initialize missing-since time from a nonpositive clock");
            record.missingSinceUtc = nowUtc;
        }
    }
    records.swap(prepared);
    if(errorOut) errorOut->clear();
    return true;
}

inline bool ParseLayout(const std::string& data, std::vector<DeskRec>& desksOut, std::vector<LayoutWin>& winsOut,
        UnixSeconds migrationNow, std::string* errorOut=nullptr, int* sourceVersionOut=nullptr,
        RecordIdGenerator idGenerator=NewRecordId){
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
    auto assignGeneratedRecordId = [&](LayoutWin& record)->bool {
        if(!idGenerator) return failLine("record ID generator is unavailable");
        std::string generated = idGenerator();
        if(generated.empty()) return failLine("failed to generate record ID");
        GUID id{}; std::string idKey;
        if(!ParseNonzeroLayoutGuid(generated,id,&idKey)) return failLine("generated an invalid record ID");
        if(!recordIds.insert(idKey).second) return failLine("generated a duplicate record ID");
        record.recordId = generated;
        return true;
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
        if(++recordCount > MAX_LAYOUT_RECORDS) return failLine("snapshot record limit exceeded");
        std::vector<std::string> col = splitTabs(line);
        if(col.empty()) return failLine("empty record");

        if(col[0]=="D"){
            if(col.size()!=4) return failLine("desktop record must have exactly 4 fields");
            DeskRec d{};
            if(!ParseIntStrict(col[1], d.index)) return failLine("invalid desktop index");
            if(!ParseNonzeroLayoutGuid(col[2], d.guid)) return failLine("invalid desktop GUID");
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
            if(!IsSupportedLayoutApp(w.app)) return failLine("unsupported window app");
            GUID id{}; std::string idKey;
            if(!ParseNonzeroLayoutGuid(col[2], id, &idKey)) return failLine("invalid record ID");
            if(!recordIds.insert(idKey).second) return failLine("duplicate record ID");
            w.recordId = col[2];
            if(!ParseIntStrict(col[3], w.deskIndex)) return failLine("invalid window desktop index");
            if(!ParseNonzeroLayoutGuid(col[4], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[5], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[6];
            if(HasLayoutFieldBreak(w.activeDomain)) return failLine("window domain contains a field delimiter");
            if(!ParseIntStrict(col[7], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[8], w.counts)) return failLine("invalid window domain counts");
            if(!AreLayoutCountsSerializable(w.counts)) return failLine("window domain counts contain a field delimiter");
            if(!ParseI64Strict(col[9], w.lastSeenUtc) || w.lastSeenUtc<=0) return failLine("invalid last-seen time");
            if(!ParseI64Strict(col[10], w.missingSinceUtc) || w.missingSinceUtc<0) return failLine("invalid missing-since time");
        } else if(version==3){
            if(col.size()!=9) return failLine("v3 window record must have exactly 9 fields");
            w.app = col[1];
            if(!IsSupportedLayoutApp(w.app)) return failLine("unsupported window app");
            if(!ParseIntStrict(col[2], w.deskIndex)) return failLine("invalid window desktop index");
            if(!ParseNonzeroLayoutGuid(col[3], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[4], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[5];
            if(HasLayoutFieldBreak(w.activeDomain)) return failLine("window domain contains a field delimiter");
            if(!ParseIntStrict(col[6], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[7], w.counts)) return failLine("invalid window domain counts");
            if(!AreLayoutCountsSerializable(w.counts)) return failLine("window domain counts contain a field delimiter");
            int oldMissing = 0;
            if(!ParseIntStrict(col[8], oldMissing) || oldMissing<0) return failLine("invalid legacy missing-run count");
            if(!assignGeneratedRecordId(w)) return false;
            w.lastSeenUtc = migrationNow;
            w.missingSinceUtc = oldMissing>0 ? migrationNow : 0;
        } else {
            if(col.size()!=7) return failLine("v2 window record must have exactly 7 fields");
            w.app = "firefox";
            if(!ParseIntStrict(col[1], w.deskIndex)) return failLine("invalid window desktop index");
            if(!ParseNonzeroLayoutGuid(col[2], w.desktop)) return failLine("invalid window desktop GUID");
            if(!b64decStrict(col[3], title)) return failLine("invalid window title encoding");
            w.activeTitle = title;
            w.activeDomain = col[4];
            if(HasLayoutFieldBreak(w.activeDomain)) return failLine("window domain contains a field delimiter");
            if(!ParseIntStrict(col[5], w.tabCount) || w.tabCount<0) return failLine("invalid window tab count");
            if(!ParseCountsStrict(col[6], w.counts)) return failLine("invalid window domain counts");
            if(!AreLayoutCountsSerializable(w.counts)) return failLine("window domain counts contain a field delimiter");
            if(!assignGeneratedRecordId(w)) return false;
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

inline bool BuildCheckedLayoutSnapshot(const std::vector<DeskRec>& desks, std::vector<LayoutWin>& wins,
        UnixSeconds nowUtc, std::string& textOut, std::string* errorOut=nullptr,
        RecordIdGenerator idGenerator=NewRecordId){
    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut=message;
        return false;
    };
    if(desks.size()>MAX_LAYOUT_RECORDS || wins.size()>MAX_LAYOUT_RECORDS-desks.size())
        return fail("snapshot record limit exceeded");
    for(const auto& desk : desks) if(GuidIsZero(desk.guid)) return fail("desktop record has a zero GUID");
    std::vector<LayoutWin> prepared=wins;
    std::string prepareError;
    if(!PrepareTransitionalV4Records(prepared,nowUtc,&prepareError,idGenerator)) return fail(prepareError);
    std::string serialized=SerializeLayout(desks,prepared);
    wins.swap(prepared);
    textOut.swap(serialized);
    if(errorOut) errorOut->clear();
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

inline bool BuildAutoLayoutSnapshot(const std::string* existingBytes, const std::vector<DeskRec>& currentDesks,
        const std::vector<LayoutWin>& present, UnixSeconds nowUtc, std::string& textOut,
        std::string* errorOut=nullptr, RecordIdGenerator idGenerator=NewRecordId){
    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut=message;
        return false;
    };
    std::vector<LayoutWin> existing;
    if(existingBytes){
        std::vector<DeskRec> ignoredDesks;
        std::string parseError;
        if(!ParseLayout(*existingBytes,ignoredDesks,existing,nowUtc,&parseError,nullptr,idGenerator))
            return fail("invalid existing auto snapshot: "+parseError);
    }
    std::vector<LayoutWin> merged=MergeAutoLayout(existing,present);
    return BuildCheckedLayoutSnapshot(currentDesks,merged,nowUtc,textOut,errorOut,idGenerator);
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
