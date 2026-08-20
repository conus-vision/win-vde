// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include "session_worker.hpp" // must be self-contained at first include
#include "str_util.hpp"
#include "layout.hpp"
#include "layout_store.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include "lifecycle.hpp"

static int g_fail = 0, g_total = 0;
#define CHECK(c) do{ g_total++; if(!(c)){ g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static void test_etld1(){
    CHECK(etld1("mail.google.com") == "google.com");
    CHECK(etld1("docs.python.org") == "python.org");
    CHECK(hostOf("https://www.GitHub.com/x/y") == "github.com");
}
static void test_b64(){
    std::string s = "Inbox \xE2\x80\x94 Mozilla";   // includes a UTF-8 em dash
    CHECK(b64dec(b64enc(s)) == s);
}
static void test_b64_long_roundtrip(){
    std::string input; input.reserve(1024*1024);
    for(int i=0;i<1024*1024;++i) input.push_back((char)(i&0xFF));
    CHECK(b64dec(b64enc(input))==input);
}
static void test_strict_integer_parsing(){
    long long i64=0;
    CHECK(ParseI64Strict("-9223372036854775808", i64) && i64==LLONG_MIN);
    CHECK(ParseI64Strict("9223372036854775807", i64) && i64==LLONG_MAX);
    i64=17; CHECK(!ParseI64Strict("", i64) && i64==17);
    CHECK(!ParseI64Strict("12junk", i64) && i64==17);
    CHECK(!ParseI64Strict("9223372036854775808", i64) && i64==17);
    int value=0;
    CHECK(ParseIntStrict("-2147483648", value) && value==INT_MIN);
    CHECK(ParseIntStrict("2147483647", value) && value==INT_MAX);
    value=23; CHECK(!ParseIntStrict("2147483648", value) && value==23);
}
static void test_strict_base64_parsing(){
    std::string out;
    CHECK(b64decStrict("", out) && out.empty());
    CHECK(b64decStrict("TQ==", out) && out=="M");
    CHECK(b64decStrict("TWE=", out) && out=="Ma");
    CHECK(b64decStrict("TWFu", out) && out=="Man");
    out="sentinel"; CHECK(!b64decStrict("TQ=", out) && out=="sentinel");
    CHECK(!b64decStrict("TQ$=", out) && out=="sentinel");
    CHECK(!b64decStrict("A===", out) && out=="sentinel");
    CHECK(!b64decStrict("AA=A", out) && out=="sentinel");
    CHECK(!b64decStrict("=AAA", out) && out=="sentinel");
    CHECK(!b64decStrict("TR==", out) && out=="sentinel");
    CHECK(!b64decStrict("TWF=", out) && out=="sentinel");
}
static void test_strict_counts_parsing(){
    std::map<std::string,int> counts={{"old",9}};
    CHECK(ParseCountsStrict("", counts) && counts.empty());
    CHECK(ParseCountsStrict("mail.example:2,[::1]:3", counts));
    CHECK(counts.size()==2 && counts["mail.example"]==2 && counts["[::1]"]==3);
    counts={{"old",9}}; CHECK(!ParseCountsStrict("mail.example:0", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:-1", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1junk", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1,mail.example:2", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1,", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict(":1", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("missing-count", counts) && counts["old"]==9);
}

static GUID G(const wchar_t* s){ GUID g{}; StringToGuid(s, g); return g; }

static void test_layout_serializes_v4_header(){
    CHECK(SerializeLayout({}, {}).find("# VDE snapshot v4\n") == 0);
}

static void test_layout_roundtrip_v4(){
    std::vector<DeskRec> d; DeskRec d0; d0.index=0; d0.guid=G(L"{231A0000-0000-0000-0000-000000000001}"); d0.name=L"Work"; d.push_back(d0);
    std::vector<LayoutWin> w; LayoutWin w0;
    w0.recordId="{00000000-0000-0000-0000-000000000101}";
    w0.app="firefox"; w0.deskIndex=0; w0.desktop=d0.guid; w0.activeTitle="PR #42";
    w0.activeDomain="github.com"; w0.tabCount=5; w0.counts={{"github.com",4},{"docs.python.org",1}};
    w0.lastSeenUtc=1700000000; w0.missingSinceUtc=1700000100;
    w.push_back(w0);

    std::string s = SerializeLayout(d, w);
    std::vector<DeskRec> d2; std::vector<LayoutWin> w2; std::string error;
    CHECK(ParseLayout(s, d2, w2, 1800000000, &error));
    CHECK(error.empty());
    CHECK(d2.size()==1); CHECK(w2.size()==1);
    CHECK(w2[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(w2[0].app=="firefox"); CHECK(w2[0].deskIndex==0); CHECK(w2[0].activeTitle=="PR #42");
    CHECK(w2[0].activeDomain=="github.com"); CHECK(w2[0].tabCount==5);
    CHECK(w2[0].lastSeenUtc==1700000000); CHECK(w2[0].missingSinceUtc==1700000100);
    CHECK(w2[0].counts["github.com"]==4); CHECK(w2[0].counts["docs.python.org"]==1);
    CHECK(GuidEq(w2[0].desktop, d0.guid));
}
static void test_layout_parse_v2(){
    std::string v2 = "# VDE snapshot v2\n"
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Work") + "\n"
        "W\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("PR #42") + "\tgithub.com\t5\tgithub.com:4,docs.python.org:1\n";
    std::vector<DeskRec> d; std::vector<LayoutWin> w;
    CHECK(ParseLayout(v2, d, w, 1800000000));
    CHECK(w.size()==1); CHECK(w[0].app=="firefox"); CHECK(w[0].missingRuns==0);
    CHECK(w[0].lastSeenUtc==1800000000);
    CHECK(w[0].tabCount==5); CHECK(w[0].counts["docs.python.org"]==1);
}

static std::string V4Line(const char* guid, const char* recordId, const char* lastSeen, const char* missing){
    return std::string("W\tfirefox\t") + recordId + "\t0\t" + guid + "\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t" + lastSeen + "\t" + missing + "\n";
}

static void test_layout_rejects_invalid_desktop_guid_transactionally(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("not-a-guid", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_progid_as_desktop_guid(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("Shell.Application", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_integer_trailing_junk_transactionally(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", "{00000000-0000-0000-0000-000000000101}", "1700000000junk", "0");
    std::vector<DeskRec> d(1); d[0].index=77;
    std::vector<LayoutWin> w(1); w[0].app="sentinel";
    std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.size()==1 && d[0].index==77);
    CHECK(w.size()==1 && w[0].app=="sentinel");
}

static void test_layout_migrates_v3_record(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t2\n";
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error="stale"; int source=0;
    CHECK(ParseLayout(data, d, w, 1800000000, &error, &source));
    CHECK(error.empty()); CHECK(source==3); CHECK(w.size()==1);
    CHECK(!w[0].recordId.empty()); CHECK(w[0].lastSeenUtc==1800000000);
    CHECK(w[0].missingSinceUtc==1800000000);
    GUID id{}; CHECK(StringToGuid(U82W(w[0].recordId), id) && !GuidIsZero(id));
}

static void test_layout_rejects_negative_v3_missing_counter_transactionally(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t-1\n";
    std::vector<DeskRec> d(1); d[0].index=77; d[0].name=L"sentinel desk";
    std::vector<LayoutWin> w(1); w[0].app="sentinel"; w[0].activeTitle="sentinel title";
    std::string error;
    CHECK(!ParseLayout(data,d,w,1800000000,&error));
    CHECK(!error.empty());
    CHECK(d.size()==1 && d[0].index==77 && d[0].name==L"sentinel desk");
    CHECK(w.size()==1 && w[0].app=="sentinel" && w[0].activeTitle=="sentinel title");
}

static void test_layout_rejects_trailing_columns(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    data.insert(data.size()-1, "\textra");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_duplicate_record_ids(){
    const char* id="{00000000-0000-0000-0000-000000000101}";
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", id, "1700000000", "0") +
        V4Line("{231A0000-0000-0000-0000-000000000002}", id, "1700000001", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_enforces_total_record_cap_transactionally(){
    const char* desktop="{231A0000-0000-0000-0000-000000000001}";
    std::string data="# VDE snapshot v4\n";
    data.reserve(600000);
    std::string deskLine=std::string("D\t0\t")+desktop+"\t"+b64enc("Desk")+"\n";
    for(int i=0;i<2048;++i) data+=deskLine;
    for(int i=0;i<2048;++i){
        char id[64]; sprintf_s(id,"{00000000-0000-0000-0000-%012d}",i+1);
        data+=V4Line(desktop,id,"1700000000","0");
    }

    std::vector<DeskRec> acceptedDesks; std::vector<LayoutWin> acceptedWins; std::string error="stale";
    CHECK(ParseLayout(data,acceptedDesks,acceptedWins,1800000000,&error));
    CHECK(error.empty()); CHECK(acceptedDesks.size()==2048); CHECK(acceptedWins.size()==2048);
    CHECK(acceptedDesks.size()==2048 && acceptedDesks.front().name==L"Desk");
    CHECK(acceptedWins.size()==2048 && acceptedWins.back().recordId=="{00000000-0000-0000-0000-000000002048}");

    char overflowId[64]; sprintf_s(overflowId,"{00000000-0000-0000-0000-%012d}",2049);
    std::string overflow=data+V4Line(desktop,overflowId,"1700000000","0");
    DeskRec sentinelDesk{}; sentinelDesk.index=77;
    sentinelDesk.guid=G(L"{231A0000-0000-0000-0000-000000000077}"); sentinelDesk.name=L"sentinel desk";
    LayoutWin sentinelWin; sentinelWin.recordId="{00000000-0000-0000-0000-000000000077}";
    sentinelWin.app="chrome"; sentinelWin.deskIndex=-7;
    sentinelWin.desktop=G(L"{231A0000-0000-0000-0000-000000000078}");
    sentinelWin.activeTitle="sentinel title"; sentinelWin.activeDomain="sentinel.example";
    sentinelWin.tabCount=7; sentinelWin.counts={{"sentinel.example",7}};
    sentinelWin.lastSeenUtc=1700000077; sentinelWin.missingSinceUtc=1700000088; sentinelWin.missingRuns=2;
    std::vector<DeskRec> desksOut={sentinelDesk}; std::vector<LayoutWin> winsOut={sentinelWin}; error.clear();

    CHECK(!ParseLayout(overflow,desksOut,winsOut,1800000000,&error));
    CHECK(!error.empty());
    CHECK(desksOut.size()==1 && desksOut[0].index==77);
    CHECK(desksOut.size()==1 && GuidEq(desksOut[0].guid,sentinelDesk.guid));
    CHECK(desksOut.size()==1 && desksOut[0].name==L"sentinel desk");
    CHECK(winsOut.size()==1 && winsOut[0].recordId==sentinelWin.recordId);
    CHECK(winsOut.size()==1 && winsOut[0].app=="chrome");
    CHECK(winsOut.size()==1 && winsOut[0].deskIndex==-7);
    CHECK(winsOut.size()==1 && GuidEq(winsOut[0].desktop,sentinelWin.desktop));
    CHECK(winsOut.size()==1 && winsOut[0].activeTitle=="sentinel title");
    CHECK(winsOut.size()==1 && winsOut[0].activeDomain=="sentinel.example");
    CHECK(winsOut.size()==1 && winsOut[0].tabCount==7);
    CHECK(winsOut.size()==1 && winsOut[0].counts==sentinelWin.counts);
    CHECK(winsOut.size()==1 && winsOut[0].lastSeenUtc==1700000077);
    CHECK(winsOut.size()==1 && winsOut[0].missingSinceUtc==1700000088);
    CHECK(winsOut.size()==1 && winsOut[0].missingRuns==2);
}

static LayoutWin OldStyleRecord(){
    LayoutWin w;
    w.app="firefox"; w.deskIndex=0;
    w.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    w.activeTitle="Inbox"; w.activeDomain="mail.example"; w.tabCount=1;
    w.counts={{"mail.example",1}};
    return w;
}

static void test_retention_expiration_boundaries(){
    const UnixSeconds now=1700000000;
    LayoutWin w=OldStyleRecord();
    w.missingSinceUtc=now-29LL*24*60*60;
    CHECK(!IsExpired(w,now));
    w.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;
    CHECK(!IsExpired(w,now));
    w.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    CHECK(IsExpired(w,now));
}

static void test_retention_future_and_zero_missing_are_not_expired(){
    LayoutWin w=OldStyleRecord();
    w.missingSinceUtc=LLONG_MAX;
    CHECK(!IsExpired(w,1700000000));
    w.missingSinceUtc=0;
    CHECK(!IsExpired(w,LLONG_MAX));
}

static void test_retention_mark_seen_clears_missing_and_updates_last_seen(){
    LayoutWin w=OldStyleRecord();
    w.lastSeenUtc=1700000000;
    w.missingSinceUtc=1700000000;
    const UnixSeconds reappearanceUtc=w.missingSinceUtc+WINDOW_RETENTION_SECONDS-1;
    CHECK(!IsExpired(w,reappearanceUtc));
    MarkSeen(w,reappearanceUtc);
    CHECK(w.lastSeenUtc==reappearanceUtc);
    CHECK(w.missingSinceUtc==0);
}

static void test_retention_mark_missing_uses_last_seen_and_is_idempotent(){
    LayoutWin seen=OldStyleRecord();
    seen.lastSeenUtc=1700000000;
    MarkMissing(seen,1800000000);
    CHECK(seen.missingSinceUtc==1700000000);
    MarkMissing(seen,1900000000);
    CHECK(seen.missingSinceUtc==1700000000);

    LayoutWin neverSeen=OldStyleRecord();
    MarkMissing(neverSeen,1800000000);
    CHECK(neverSeen.missingSinceUtc==1800000000);
    MarkMissing(neverSeen,1900000000);
    CHECK(neverSeen.missingSinceUtc==1800000000);
}

static void test_retention_prune_preserves_order_duplicates_and_input(){
    const UnixSeconds now=1700000000;
    LayoutWin first=OldStyleRecord(); first.recordId="duplicate"; first.lastSeenUtc=11; first.missingSinceUtc=0;
    LayoutWin expired=OldStyleRecord(); expired.recordId="expired"; expired.lastSeenUtc=22;
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    LayoutWin duplicate=first; duplicate.lastSeenUtc=33;
    LayoutWin recent=OldStyleRecord(); recent.recordId="recent"; recent.lastSeenUtc=44;
    recent.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;
    std::vector<LayoutWin> input={first,expired,duplicate,recent};

    std::vector<LayoutWin> output=PruneExpired(input,now);

    CHECK(output.size()==3);
    CHECK(output[0].recordId=="duplicate" && output[0].lastSeenUtc==11);
    CHECK(output[1].recordId=="duplicate" && output[1].lastSeenUtc==33);
    CHECK(output[2].recordId=="recent" && output[2].lastSeenUtc==44);
    CHECK(input.size()==4);
    CHECK(input[0].recordId=="duplicate" && input[0].lastSeenUtc==11 && input[0].missingSinceUtc==0);
    CHECK(input[1].recordId=="expired" && input[1].lastSeenUtc==22);
    CHECK(input[2].recordId=="duplicate" && input[2].lastSeenUtc==33);
    CHECK(input[3].recordId=="recent" && input[3].lastSeenUtc==44);
}

static LayoutWin MatchRecord(const char* app, const char* title, const char* domain, int tabs,
        const std::map<std::string,int>& counts){
    LayoutWin w=OldStyleRecord();
    w.app=app; w.activeTitle=title; w.activeDomain=domain; w.tabCount=tabs; w.counts=counts;
    return w;
}

static LayoutMatch Candidate(size_t savedIndex, size_t liveIndex, double score){
    LayoutMatch match;
    match.savedIndex=savedIndex; match.liveIndex=liveIndex; match.score=score;
    return match;
}

static std::vector<std::pair<size_t,size_t>> MatchPairs(const std::vector<LayoutMatch>& matches){
    std::vector<std::pair<size_t,size_t>> pairs;
    for(const auto& match : matches) pairs.push_back(std::make_pair(match.savedIndex,match.liveIndex));
    return pairs;
}

static bool MatchesAreSortedAndUnique(const std::vector<LayoutMatch>& matches){
    std::set<size_t> saved,live;
    for(size_t i=0;i<matches.size();++i){
        if(i>0 && std::make_pair(matches[i].savedIndex,matches[i].liveIndex)<
                std::make_pair(matches[i-1].savedIndex,matches[i-1].liveIndex)) return false;
        if(!saved.insert(matches[i].savedIndex).second || !live.insert(matches[i].liveIndex).second) return false;
    }
    return true;
}

static bool SameCandidateInput(const std::vector<LayoutMatch>& left, const std::vector<LayoutMatch>& right){
    if(left.size()!=right.size()) return false;
    for(size_t i=0;i<left.size();++i){
        if(left[i].savedIndex!=right[i].savedIndex || left[i].liveIndex!=right[i].liveIndex) return false;
        if(std::isnan(left[i].score) && std::isnan(right[i].score)) continue;
        if(left[i].score!=right[i].score) return false;
    }
    return true;
}

static void test_layout_score_formula_and_fallback(){
    LayoutWin saved=MatchRecord("firefox","Same title","same.example",2,{{"a.example",1},{"b.example",1}});
    LayoutWin live=MatchRecord("firefox","Same title","same.example",2,{{"a.example",1},{"c.example",1}});
    const double expectedTitle=0.40*0.5+0.25*(1.0/3.0)+0.20+0.15;
    CHECK(std::fabs(LayoutScore(saved,live)-expectedTitle)<1e-12);

    live.activeTitle="Different title";
    const double expectedDomain=0.40*0.5+0.25*(1.0/3.0)+0.20*0.5+0.15;
    CHECK(std::fabs(LayoutScore(saved,live)-expectedDomain)<1e-12);

    saved=MatchRecord("firefox","Proportional","same.example",5,{{"a.example",3},{"b.example",4}});
    live=MatchRecord("firefox","Proportional","same.example",10,{{"a.example",6},{"b.example",8}});
    CHECK(std::fabs(LayoutScore(saved,live)-0.925)<1e-12);

    LayoutWin emptySaved=MatchRecord("firefox","Fallback","same.example",1,{});
    LayoutWin countedLive=MatchRecord("firefox","Fallback","other.example",9,{{"other.example",9}});
    CHECK(LayoutScore(emptySaved,countedLive)==1.0);
    countedLive.activeTitle="Other"; countedLive.activeDomain="same.example";
    CHECK(LayoutScore(emptySaved,countedLive)==0.0);
    countedLive.counts.clear(); countedLive.activeTitle.clear(); emptySaved.activeTitle.clear();
    CHECK(LayoutScore(emptySaved,countedLive)==0.0);
}

static void test_layout_score_browser_symmetry_and_cross_app_rejection(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        LayoutWin saved=MatchRecord(app,"Inbox","mail.example",2,{{"mail.example",2}});
        LayoutWin live=saved;
        CHECK(LayoutScore(saved,live)==1.0);
    }
    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",2,{{"mail.example",2}});
    LayoutWin chrome=firefox; chrome.app="chrome";
    CHECK(LayoutScore(firefox,chrome)==0.0);
    CHECK(LayoutScore(chrome,firefox)==0.0);
}

static void test_layout_score_identical_two_domain_is_exact(){
    LayoutWin saved=MatchRecord("firefox","Inbox","mail.example",5,
        {{"docs.example",1},{"mail.example",1}});
    LayoutWin live=saved;
    CHECK(LayoutScore(saved,live)==1.0);
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=MatchOneToOne({saved},{live},1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(matches.size()==1 && matches[0].savedIndex==0 && matches[0].liveIndex==0 &&
        matches[0].score==1.0);
}

static void test_match_one_to_one_duplicate_fingerprints_are_unique(){
    LayoutWin fingerprint=MatchRecord("firefox","Inbox","mail.example",2,{{"mail.example",2}});
    std::vector<LayoutWin> saved(3,fingerprint), live(2,fingerprint);
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=MatchOneToOne(saved,live,1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(matches.size()==2);
    CHECK(MatchesAreSortedAndUnique(matches));
    CHECK(saved.size()==3 && live.size()==2);
}

static void test_match_one_to_one_browser_apps_and_never_crosses_apps(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        LayoutWin record=MatchRecord(app,"Inbox","mail.example",1,{{"mail.example",1}});
        bool tooComplex=true;
        std::vector<LayoutMatch> matches=MatchOneToOne({record},{record},1.0,&tooComplex);
        CHECK(!tooComplex && matches.size()==1);
        CHECK(matches.size()==1 && matches[0].savedIndex==0 && matches[0].liveIndex==0);
    }

    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",1,{{"mail.example",1}});
    LayoutWin chrome=firefox; chrome.app="chrome";
    bool tooComplex=true;
    std::vector<LayoutMatch> cross=MatchOneToOne({firefox},{chrome},0.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(cross.empty());
}

static void test_match_one_to_one_score_evaluation_budget(){
    CHECK(MAX_MATCH_SCORE_EVALUATIONS==1000000);
    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",1,{});
    std::vector<LayoutWin> saved(4096,firefox),sameAppLive(4096,firefox);
    bool tooComplex=false;
    std::vector<LayoutMatch> result=MatchOneToOne(saved,sameAppLive,2.0,&tooComplex);
    CHECK(result.empty());
    CHECK(tooComplex);
    CHECK(saved.size()==4096 && sameAppLive.size()==4096);
    CHECK(saved[0].app=="firefox" && sameAppLive[0].activeTitle=="Inbox");

    LayoutWin chrome=firefox; chrome.app="chrome";
    std::vector<LayoutWin> crossAppLive(4096,chrome);
    tooComplex=true;
    result=MatchOneToOne(saved,crossAppLive,2.0,&tooComplex);
    CHECK(result.empty());
    CHECK(!tooComplex);

    LayoutWin arbitrary=firefox; arbitrary.app="arbitrary-browser";
    tooComplex=true;
    result=MatchOneToOne({arbitrary},{arbitrary},1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(result.size()==1 && result[0].savedIndex==0 && result[0].liveIndex==0);
}

static void test_assignment_maximizes_cardinality_before_score(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.90),Candidate(0,1,0.80),Candidate(1,0,0.85)
    };
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,1},{1,0}})));
    CHECK(std::fabs(matches[0].score-0.80)<1e-12 && std::fabs(matches[1].score-0.85)<1e-12);
}

static void test_assignment_maximizes_total_score_at_same_cardinality(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.90),Candidate(0,1,0.80),Candidate(1,0,0.70),Candidate(1,1,0.10)
    };
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,1},{1,0}})));
    CHECK(std::fabs(matches[0].score+matches[1].score-1.50)<1e-12);
}

static void test_assignment_ties_are_deterministic_across_input_order(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.5),Candidate(0,1,0.5),Candidate(1,0,0.5),Candidate(1,1,0.5)
    };
    const std::vector<std::pair<size_t,size_t>> expected={{0,0},{1,1}};
    CHECK(MatchPairs(AssignOneToOne(2,2,candidates))==expected);
    std::mt19937 rng(0x51A81E);
    for(int run=0;run<20;++run){
        std::shuffle(candidates.begin(),candidates.end(),rng);
        CHECK(MatchPairs(AssignOneToOne(2,2,candidates))==expected);
    }

    candidates={
        Candidate(0,0,0.5),Candidate(0,1,0.5),Candidate(0,2,0.5),
        Candidate(1,1,0.5),Candidate(1,2,0.5),Candidate(2,0,0.5)
    };
    const std::vector<std::pair<size_t,size_t>> collisionExpected={{0,2},{1,1},{2,0}};
    CHECK(MatchPairs(AssignOneToOne(3,3,candidates))==collisionExpected);
    std::reverse(candidates.begin(),candidates.end());
    CHECK(MatchPairs(AssignOneToOne(3,3,candidates))==collisionExpected);
}

static void test_assignment_filters_and_deduplicates_without_mutating_input(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.20),Candidate(0,0,0.90),Candidate(0,0,0.50),
        Candidate(1,1,0.70),Candidate(2,0,1.00),Candidate(0,2,1.00),
        Candidate(0,1,std::numeric_limits<double>::quiet_NaN()),
        Candidate(1,0,std::numeric_limits<double>::infinity()),Candidate(1,0,-0.10)
    };
    const std::vector<LayoutMatch> original=candidates;
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,0},{1,1}})));
    CHECK(matches.size()==2 && matches[0].score==0.90 && matches[1].score==0.70);
    CHECK(SameCandidateInput(candidates,original));
    CHECK(AssignOneToOne(0,2,candidates).empty());
    CHECK(AssignOneToOne(2,0,candidates).empty());
    CHECK(AssignOneToOne(2,2,{}).empty());
}

struct OracleAssignmentResult {
    bool initialized=false;
    long long totalUnits=0;
    long long deterministicTieSum=0;
    std::vector<std::pair<size_t,size_t>> pairs;
};

static void VisitOracleAssignments(size_t savedIndex, size_t savedCount, size_t liveCount,
        const std::vector<std::vector<long long>>& units, const std::vector<std::vector<long long>>& tieOrders,
        std::vector<bool>& usedLive, std::vector<std::pair<size_t,size_t>>& current,
        long long totalUnits, long long deterministicTieSum,
        OracleAssignmentResult& best){
    if(savedIndex==savedCount){
        if(!best.initialized || current.size()>best.pairs.size() ||
                (current.size()==best.pairs.size() && (totalUnits>best.totalUnits ||
                (totalUnits==best.totalUnits && deterministicTieSum<best.deterministicTieSum)))){
            best.initialized=true; best.totalUnits=totalUnits;
            best.deterministicTieSum=deterministicTieSum; best.pairs=current;
        }
        return;
    }
    VisitOracleAssignments(savedIndex+1,savedCount,liveCount,units,tieOrders,usedLive,current,
        totalUnits,deterministicTieSum,best);
    for(size_t liveIndex=0;liveIndex<liveCount;++liveIndex){
        if(usedLive[liveIndex] || units[savedIndex][liveIndex]<0) continue;
        usedLive[liveIndex]=true;
        current.push_back(std::make_pair(savedIndex,liveIndex));
        VisitOracleAssignments(savedIndex+1,savedCount,liveCount,units,tieOrders,usedLive,current,
            totalUnits+units[savedIndex][liveIndex],
            deterministicTieSum+tieOrders[savedIndex][liveIndex],best);
        current.pop_back();
        usedLive[liveIndex]=false;
    }
}

static OracleAssignmentResult ExhaustiveAssignmentOracle(size_t savedCount, size_t liveCount,
        const std::vector<LayoutMatch>& candidates){
    std::vector<std::vector<long long>> units(savedCount,std::vector<long long>(liveCount,-1));
    std::vector<std::vector<long long>> tieOrders(savedCount,std::vector<long long>(liveCount,-1));
    std::map<std::pair<size_t,size_t>,double> bestScores;
    for(const auto& candidate : candidates){
        if(candidate.savedIndex>=savedCount || candidate.liveIndex>=liveCount ||
                !std::isfinite(candidate.score) || candidate.score<0) continue;
        std::pair<size_t,size_t> key(candidate.savedIndex,candidate.liveIndex);
        auto existing=bestScores.find(key);
        if(existing==bestScores.end() || existing->second<candidate.score) bestScores[key]=candidate.score;
    }
    long long tieOrder=0;
    for(const auto& item : bestScores){
        units[item.first.first][item.first.second]=std::llround(item.second*1000000000.0);
        tieOrders[item.first.first][item.first.second]=++tieOrder;
    }
    OracleAssignmentResult result;
    std::vector<bool> usedLive(liveCount,false);
    std::vector<std::pair<size_t,size_t>> current;
    VisitOracleAssignments(0,savedCount,liveCount,units,tieOrders,usedLive,current,0,0,result);
    return result;
}

static long long AssignmentTieSum(const std::vector<LayoutMatch>& candidates,
        const std::vector<LayoutMatch>& matches){
    std::map<std::pair<size_t,size_t>,double> bestScores;
    for(const auto& candidate : candidates){
        if(!std::isfinite(candidate.score) || candidate.score<0) continue;
        std::pair<size_t,size_t> key(candidate.savedIndex,candidate.liveIndex);
        auto existing=bestScores.find(key);
        if(existing==bestScores.end() || existing->second<candidate.score) bestScores[key]=candidate.score;
    }
    std::set<std::pair<size_t,size_t>> selected;
    for(const auto& match : matches) selected.insert(std::make_pair(match.savedIndex,match.liveIndex));
    long long tieOrder=0,total=0;
    for(const auto& item : bestScores) if(++tieOrder && selected.count(item.first)) total+=tieOrder;
    return total;
}

static void test_assignment_randomized_against_exhaustive_oracle(){
    std::mt19937 rng(0xC0FFEE);
    for(int testCase=0;testCase<240;++testCase){
        size_t savedCount=rng()%5, liveCount=rng()%5;
        std::vector<LayoutMatch> candidates;
        for(size_t savedIndex=0;savedIndex<savedCount;++savedIndex){
            for(size_t liveIndex=0;liveIndex<liveCount;++liveIndex){
                if(rng()%100>=62) continue;
                long long scoreUnits=1+(rng()%7);
                candidates.push_back(Candidate(savedIndex,liveIndex,scoreUnits/1000000000.0));
            }
        }
        std::shuffle(candidates.begin(),candidates.end(),rng);
        OracleAssignmentResult expected=ExhaustiveAssignmentOracle(savedCount,liveCount,candidates);
        std::vector<LayoutMatch> actual=AssignOneToOne(savedCount,liveCount,candidates);
        long long actualUnits=0;
        for(const auto& match : actual) actualUnits+=std::llround(match.score*1000000000.0);
        CHECK(actual.size()==expected.pairs.size());
        CHECK(actualUnits==expected.totalUnits);
        CHECK(AssignmentTieSum(candidates,actual)==expected.deterministicTieSum);
        CHECK(MatchesAreSortedAndUnique(actual));
    }
}

static void test_assignment_candidate_cap_direct_and_generated(){
    std::vector<LayoutMatch> dense;
    dense.reserve(MAX_MATCH_CANDIDATES+1);
    for(size_t savedIndex=0;savedIndex<64;++savedIndex)
        for(size_t liveIndex=0;liveIndex<128;++liveIndex)
            dense.push_back(Candidate(savedIndex,liveIndex,1.0));
    CHECK(dense.size()==MAX_MATCH_CANDIDATES);
    bool tooComplex=true;
    std::vector<LayoutMatch> exact=AssignOneToOne(64,128,dense,&tooComplex);
    CHECK(!tooComplex && exact.size()==64 && MatchesAreSortedAndUnique(exact));

    dense.push_back(Candidate(64,0,1.0));
    tooComplex=false;
    CHECK(AssignOneToOne(65,128,dense,&tooComplex).empty());
    CHECK(tooComplex);

    LayoutWin fingerprint=MatchRecord("firefox","Inbox","mail.example",1,{{"mail.example",1}});
    std::vector<LayoutWin> saved(64,fingerprint),live(128,fingerprint);
    tooComplex=true;
    exact=MatchOneToOne(saved,live,1.0,&tooComplex);
    CHECK(!tooComplex && exact.size()==64 && MatchesAreSortedAndUnique(exact));
    saved.push_back(fingerprint);
    tooComplex=false;
    CHECK(MatchOneToOne(saved,live,1.0,&tooComplex).empty());
    CHECK(tooComplex);

    std::vector<LayoutMatch> sparse;
    sparse.reserve(MAX_MATCH_CANDIDATES);
    for(size_t index=0;index<MAX_MATCH_CANDIDATES;++index)
        sparse.push_back(Candidate(index,index,1.0));
    tooComplex=true;
    std::vector<LayoutMatch> sparseResult=AssignOneToOne(
        MAX_MATCH_CANDIDATES,MAX_MATCH_CANDIDATES,sparse,&tooComplex);
    CHECK(!tooComplex);
    CHECK(sparseResult.size()==MAX_MATCH_CANDIDATES && MatchesAreSortedAndUnique(sparseResult));
}

static void test_assignment_flow_work_budget_rejects_connected_cycle(){
    CHECK(MAX_MATCH_FLOW_WORK==1000000);
    const size_t nodeCount=MAX_MATCH_CANDIDATES/2;
    std::vector<LayoutMatch> candidates;
    candidates.reserve(MAX_MATCH_CANDIDATES);
    for(size_t index=0;index<nodeCount;++index){
        candidates.push_back(Candidate(index,index,1.0));
        candidates.push_back(Candidate(index,(index+1)%nodeCount,0.5));
    }
    CHECK(candidates.size()==MAX_MATCH_CANDIDATES);
    bool tooComplex=false;
    std::vector<LayoutMatch> result=AssignOneToOne(nodeCount,nodeCount,candidates,&tooComplex);
    CHECK(result.empty());
    CHECK(tooComplex);
    CHECK(candidates.size()==MAX_MATCH_CANDIDATES);
    CHECK(candidates.front().savedIndex==0 && candidates.front().liveIndex==0);
    CHECK(candidates.back().savedIndex==nodeCount-1 && candidates.back().liveIndex==0);
}

static void test_assignment_checked_score_scaling_boundary(){
    const double nearLimit=9223372036.0;
    bool tooComplex=true;
    std::vector<LayoutMatch> accepted=AssignOneToOne(1,1,{Candidate(0,0,nearLimit)},&tooComplex);
    CHECK(!tooComplex);
    CHECK(accepted.size()==1 && accepted[0].score==nearLimit);

    const double excessive=9223372037.0;
    for(int run=0;run<2;++run){
        tooComplex=false;
        CHECK(AssignOneToOne(1,1,{Candidate(0,0,excessive)},&tooComplex).empty());
        CHECK(tooComplex);
    }

    std::vector<LayoutMatch> widePath={
        Candidate(0,2,0),Candidate(1,1,0),Candidate(1,2,nearLimit),
        Candidate(2,0,0),Candidate(2,1,nearLimit)
    };
    tooComplex=true;
    std::vector<LayoutMatch> wideResult=AssignOneToOne(3,3,widePath,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(wideResult)==std::vector<std::pair<size_t,size_t>>({{0,2},{1,1},{2,0}})));
}
static std::string FailingRecordIdGenerator(){ return std::string(); }
static std::string ConstantRecordIdGenerator(){ return "{00000000-0000-0000-0000-000000000099}"; }
static std::string MalformedRecordIdGenerator(){ return "not-a-guid"; }
static std::string ZeroRecordIdGenerator(){ return "{00000000-0000-0000-0000-000000000000}"; }

static void test_layout_legacy_migration_rejects_generated_id_collision_transactionally(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t0\n"
        "W\tchrome\t1\t{231A0000-0000-0000-0000-000000000002}\t" + b64enc("Calendar") +
        "\tcalendar.example\t1\tcalendar.example:1\t0\n";
    DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
    LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
    std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
    std::string error;
    CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,ConstantRecordIdGenerator));
    CHECK(!error.empty());
    CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
    CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
}

static void test_layout_legacy_migration_rejects_invalid_generated_ids_transactionally(){
    std::string data = "# VDE snapshot v2\n"
        "W\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\n";
    RecordIdGenerator generators[]={MalformedRecordIdGenerator,ZeroRecordIdGenerator};
    for(auto generator : generators){
        DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
        LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
        std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
        std::string error;
        CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,generator));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
        CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
    }
}

static void test_layout_rejects_embedded_carriage_returns_transactionally(){
    const std::string guid="{231A0000-0000-0000-0000-000000000001}";
    const std::string id="{00000000-0000-0000-0000-000000000101}";
    const std::string title=b64enc("Inbox");
    const std::string invalid[]={
        "# VDE snapshot v4\nW\tfirefox\t"+id+"\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\t1700000000\t0\n",
        "# VDE snapshot v4\nW\tfirefox\t"+id+"\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\t1700000000\t0\n",
        "# VDE snapshot v3\nW\tfirefox\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\t0\n",
        "# VDE snapshot v3\nW\tfirefox\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\t0\n",
        "# VDE snapshot v2\nW\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\n",
        "# VDE snapshot v2\nW\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\n"
    };
    for(const auto& data : invalid){
        DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
        LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
        std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
        std::string error;
        CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,ConstantRecordIdGenerator));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
        CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
    }
}

static void test_checked_snapshot_enforces_combined_record_cap(){
    DeskRec desk{}; desk.index=0; desk.guid=G(L"{231A0000-0000-0000-0000-000000000001}"); desk.name=L"Desk";
    std::vector<DeskRec> acceptedDesks(MAX_LAYOUT_RECORDS-1,desk);
    std::vector<LayoutWin> acceptedWins={OldStyleRecord()};
    std::string output="sentinel", error="stale";
    CHECK(BuildCheckedLayoutSnapshot(acceptedDesks,acceptedWins,1700000000,output,&error));
    CHECK(error.empty()); CHECK(output.find("# VDE snapshot v4\n")==0);
    CHECK(!acceptedWins[0].recordId.empty()); CHECK(acceptedWins[0].lastSeenUtc==1700000000);

    std::vector<DeskRec> overflowDesks(MAX_LAYOUT_RECORDS,desk);
    std::vector<LayoutWin> overflowWins={OldStyleRecord()};
    output="prior snapshot bytes"; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot(overflowDesks,overflowWins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(overflowWins.size()==1 && overflowWins[0].recordId.empty() && overflowWins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_zero_desktop_record_transactionally(){
    DeskRec invalidDesk{}; invalidDesk.index=0; invalidDesk.name=L"Invalid";
    std::vector<DeskRec> desks={invalidDesk}; std::vector<LayoutWin> wins;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot(desks,wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins.empty());
    CHECK(GuidIsZero(desks[0].guid) && desks[0].name==L"Invalid");
}

static void test_checked_snapshot_rejects_malformed_record_id_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].recordId="not-a-guid";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="not-a-guid" && wins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_zero_record_id_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].recordId="{00000000-0000-0000-0000-000000000000}";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000000}" && wins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_duplicate_record_ids_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord(),OldStyleRecord()};
    wins[0].recordId="{AAAAAAAA-BBBB-CCCC-DDDD-000000000001}";
    wins[1].recordId="aaaaaaaa-bbbb-cccc-dddd-000000000001";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==2 && wins[0].lastSeenUtc==0 && wins[1].lastSeenUtc==0);
    CHECK(wins[0].recordId.front()=='{' && wins[1].recordId.front()=='a');
}

static void test_checked_snapshot_rejects_generated_id_collision_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord(),OldStyleRecord()};
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error,ConstantRecordIdGenerator));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==2 && wins[0].recordId.empty() && wins[1].recordId.empty());
    CHECK(wins[0].lastSeenUtc==0 && wins[1].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_negative_missing_since_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].missingSinceUtc=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId.empty() && wins[0].lastSeenUtc==0 && wins[0].missingSinceUtc==-1);
}

static void test_checked_snapshot_rejects_negative_missing_bridge_clock_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()};
    wins[0].lastSeenUtc=1700000000; wins[0].missingRuns=1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,-1,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId.empty() && wins[0].missingSinceUtc==0 && wins[0].missingRuns==1);
}

static void test_checked_snapshot_rejects_zero_missing_bridge_clock_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()};
    wins[0].recordId="{00000000-0000-0000-0000-000000000101}";
    wins[0].lastSeenUtc=1700000000; wins[0].missingRuns=1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,0,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==0 && wins[0].missingRuns==1);
}

static void test_checked_snapshot_rejects_negative_transitional_missing_runs_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()};
    wins[0].recordId="{00000000-0000-0000-0000-000000000101}";
    wins[0].lastSeenUtc=1700000000; wins[0].missingRuns=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000100,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==0 && wins[0].missingRuns==-1);
}

static void test_checked_snapshot_accepts_supported_browser_apps(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].app=app;
        std::string output="sentinel", error="stale";
        CHECK(BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
        CHECK(error.empty()); CHECK(output.find(std::string("W\t")+app+"\t")!=std::string::npos);
    }
}

static void test_checked_snapshot_rejects_unsupported_app_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].app="opera";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_negative_tab_count_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].tabCount=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_invalid_counts_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].counts={{"mail.example",0}};
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());

    wins={OldStyleRecord()}; wins[0].counts={{"mail.example,evil",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());

    wins={OldStyleRecord()}; wins[0].counts={{"mail.example\tinjected",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally(){
    std::vector<LayoutWin> wins={OldStyleRecord()}; wins[0].activeDomain="mail.example\tinjected";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());

    wins={OldStyleRecord()}; wins[0].activeDomain="mail.example\ninjected"; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins[0].recordId.empty());
}

static void test_auto_snapshot_build_rejects_invalid_existing_bytes_transactionally(){
    const std::string invalidExisting[]={
        "# VDE snapshot v4\nW\ttruncated\n",
        "# VDE snapshot v5\n"
    };
    for(const auto& existing : invalidExisting){
        std::vector<LayoutWin> present={OldStyleRecord()};
        std::string output="prior snapshot bytes", error;
        CHECK(!BuildAutoLayoutSnapshot(&existing,{},present,1700000000,output,&error));
        CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
        CHECK(present.size()==1 && present[0].recordId.empty() && present[0].lastSeenUtc==0);
    }
}

static void test_auto_snapshot_build_forwards_generator_to_legacy_migration(){
    const std::string guid="{231A0000-0000-0000-0000-000000000001}";
    const std::string v2 = "# VDE snapshot v2\n"
        "W\t0\t"+guid+"\t"+b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\n";
    const std::string v3 = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t"+guid+"\t"+b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\t0\n"
        "W\tchrome\t1\t"+guid+"\t"+b64enc("Calendar")+"\tcalendar.example\t1\tcalendar.example:1\t0\n";
    const std::string snapshots[]={v2,v3};
    RecordIdGenerator generators[]={FailingRecordIdGenerator,ConstantRecordIdGenerator};
    for(size_t i=0;i<2;++i){
        std::vector<LayoutWin> present;
        std::string output="prior snapshot bytes", error;
        CHECK(!BuildAutoLayoutSnapshot(&snapshots[i],{},present,1700000000,output,&error,generators[i]));
        CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(present.empty());
    }
}

static void test_auto_snapshot_build_allows_missing_existing_file(){
    std::vector<LayoutWin> present={OldStyleRecord()};
    std::string output="sentinel", error="stale";
    CHECK(BuildAutoLayoutSnapshot(nullptr,{},present,1700000000,output,&error));
    CHECK(error.empty()); CHECK(output.find("# VDE snapshot v4\n")==0);
    std::vector<DeskRec> parsedDesks; std::vector<LayoutWin> parsed;
    CHECK(ParseLayout(output,parsedDesks,parsed,1800000000,&error));
    CHECK(error.empty()); CHECK(parsed.size()==1 && !parsed[0].recordId.empty());
}

static void test_prepare_transitional_records_roundtrip_and_keep_stable_id(){
    std::vector<LayoutWin> records={OldStyleRecord()};
    records[0].missingRuns=2;
    std::string error="stale";
    CHECK(PrepareTransitionalV4Records(records, 1700000000, &error));
    CHECK(error.empty()); CHECK(!records[0].recordId.empty());
    CHECK(records[0].lastSeenUtc==1700000000); CHECK(records[0].missingSinceUtc==1700000000);
    std::string id=records[0].recordId;
    CHECK(PrepareTransitionalV4Records(records, 1700001000, &error));
    CHECK(records[0].recordId==id); CHECK(records[0].lastSeenUtc==1700000000);

    std::string serialized=SerializeLayout({}, records);
    std::vector<DeskRec> parsedDesks; std::vector<LayoutWin> parsed; int version=0;
    CHECK(ParseLayout(serialized, parsedDesks, parsed, 1800000000, &error, &version));
    CHECK(version==4); CHECK(parsed.size()==1); CHECK(parsed[0].recordId==id);
    CHECK(parsed[0].lastSeenUtc==1700000000); CHECK(parsed[0].missingSinceUtc==1700000000);
}

static void test_prepare_transitional_records_rejects_zero_desktop_transactionally(){
    std::vector<LayoutWin> records={OldStyleRecord(),OldStyleRecord()};
    records[1].desktop=GUID{};
    std::string priorBytes="# existing snapshot bytes\n", fileBytes=priorBytes, error;
    if(PrepareTransitionalV4Records(records, 1700000000, &error))
        fileBytes=SerializeLayout({}, records);
    CHECK(!error.empty()); CHECK(records.size()==2);
    CHECK(fileBytes==priorBytes);
    CHECK(records[0].recordId.empty() && records[0].lastSeenUtc==0);
    CHECK(GuidIsZero(records[1].desktop));
}

static void test_prepare_transitional_records_rejects_negative_last_seen_transactionally(){
    std::vector<LayoutWin> records={OldStyleRecord()};
    records[0].lastSeenUtc=-1;
    std::string error;
    CHECK(!PrepareTransitionalV4Records(records, 1700000000, &error));
    CHECK(!error.empty()); CHECK(records[0].recordId.empty()); CHECK(records[0].lastSeenUtc==-1);
}

static void test_prepare_transitional_records_id_failure_keeps_prior_bytes(){
    std::vector<LayoutWin> records={OldStyleRecord()};
    std::string priorBytes="# existing snapshot bytes\n", fileBytes=priorBytes, error;
    if(PrepareTransitionalV4Records(records, 1700000000, &error, FailingRecordIdGenerator))
        fileBytes=SerializeLayout({}, records);
    CHECK(!error.empty()); CHECK(fileBytes==priorBytes);
    CHECK(records[0].recordId.empty()); CHECK(records[0].lastSeenUtc==0);
}

static void test_fingerprint_key_generic_vs_domain(){
    CHECK(FingerprintKey("firefox",{{"a.com",2}},"T") == FingerprintKey("firefox",{{"a.com",2}},"OTHER"));
    CHECK(FingerprintKey("explorer",{},"Downloads") != FingerprintKey("explorer",{},"Documents"));
}

static void test_lc_initial_absence_marks_missing_once(){
    LcState s;
    LcDecision first=LcObserve(s,false,0,0,0,0,100);
    CHECK(first.action==LcAction::MarkMissingFromLastSeen && first.generation!=0);
    CHECK(s.nextGeneration!=0 && s.nextGeneration!=first.generation);
    CHECK(LcObserve(s,false,0,0,0,0,101).action==LcAction::None);
}
static void test_lc_two_stable_present_snapshots_begin_restore(){
    LcState s;
    CHECK(LcObserve(s,true,1,11,21,31,100).action==LcAction::None);
    LcDecision begin=LcObserve(s,true,1,11,21,31,101);
    CHECK(begin.action==LcAction::BeginRestore && begin.generation!=0);
    CHECK(s.restoreInFlight && s.inFlightGeneration==begin.generation);
}
static void test_lc_stale_restore_completion_is_ignored(){
    LcState s;
    LcObserve(s,true,1,11,21,31,100);
    LcDecision begin=LcObserve(s,true,1,11,21,31,101);
    LcRestoreCompleted(s,begin.generation+1,LcRestoreOutcome::Success,22,32,102);
    CHECK(s.restoreInFlight && s.inFlightGeneration==begin.generation);
    CHECK(s.layoutSignature==21 && s.sessionStampSignature==31);
}
static void test_restore_budget_is_exact_keyed(){
    RestoreBudgets budgets;
    RestoreBudgetKey failed{"record","runtime-a","desktop-a"};
    CHECK(budgets.mayAttempt(failed));
    budgets.markExhausted(failed);
    CHECK(!budgets.mayAttempt(failed));
    CHECK(budgets.mayAttempt(RestoreBudgetKey{"record","runtime-a","desktop-b"}));
}

static void test_lc_timeout_is_per_wave_and_survives_clock_rollback(){
    LcState s;
    CHECK(LcObserve(s,true,1,10,20,30,1000).action==LcAction::None);
    CHECK(LcObserve(s,true,1,11,20,30,500).action==LcAction::None);
    CHECK(LcObserve(s,true,1,12,20,30,20499).action==LcAction::None);
    LcDecision first=LcObserve(s,true,1,13,20,30,20500);
    CHECK(first.action==LcAction::BeginRestore && first.generation!=0);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,20,30,21000);

    CHECK(LcObserve(s,true,2,20,20,30,30000).action==LcAction::None);
    CHECK(LcObserve(s,true,2,21,20,30,49999).action==LcAction::None);
    LcDecision second=LcObserve(s,true,2,22,20,30,50000);
    CHECK(second.action==LcAction::BeginRestore && second.generation!=first.generation);
}

static void test_lc_absence_transitions_mark_once_and_reappearance_rearms(){
    LcState s;
    LcDecision initialMissing=LcObserve(s,false,0,0,0,0,0);
    CHECK(initialMissing.action==LcAction::MarkMissingFromLastSeen &&
          initialMissing.generation!=0);
    CHECK(LcObserve(s,false,0,0,0,0,1).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,2).action==LcAction::None);
    LcDecision first=LcObserve(s,true,1,1,1,1,3);
    CHECK(first.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,1,1,4);
    LcDecision transitionMissing=LcObserve(s,false,0,0,1,1,5);
    CHECK(transitionMissing.action==LcAction::MarkMissingFromLastSeen &&
          transitionMissing.generation!=0 &&
          transitionMissing.generation!=initialMissing.generation);
    CHECK(LcObserve(s,false,0,0,1,1,6).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,7).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,8).action==LcAction::BeginRestore);
}

static void test_lc_exhausted_generation_suppresses_missing_action(){
    LcState initialAbsent;
    initialAbsent.nextGeneration=0;
    LcDecision first=LcObserve(initialAbsent,false,0,0,0,0,0);
    CHECK(first.action==LcAction::None && first.generation==0);
    CHECK(initialAbsent.initialized && !initialAbsent.present &&
          initialAbsent.nextGeneration==0);

    LcState active;
    active.nextGeneration=UINT64_MAX;
    LcObserve(active,true,1,10,100,20,0);
    LcDecision last=LcObserve(active,true,1,10,100,20,1);
    CHECK(last.action==LcAction::BeginRestore && last.generation==UINT64_MAX &&
          active.nextGeneration==0);
    LcDecision suppressed=LcObserve(active,false,0,0,100,20,2);
    CHECK(suppressed.action==LcAction::None && suppressed.generation==0);
    CHECK(!active.present && active.restoreInFlight &&
          active.inFlightGeneration==UINT64_MAX && active.rearmAfterFlight);
}

static void test_lc_absence_during_flight_clears_rearm_if_still_absent(){
    LcState s;
    LcObserve(s,true,1,10,100,20,0);
    LcDecision active=LcObserve(s,true,1,10,100,20,1);
    LcDecision missing=LcObserve(s,false,0,0,100,20,2);
    CHECK(missing.action==LcAction::MarkMissingFromLastSeen &&
          missing.generation!=0 && s.rearmAfterFlight);
    CHECK(s.restoreInFlight && s.inFlightGeneration==active.generation);
    LcRestoreCompleted(s,active.generation,LcRestoreOutcome::Success,100,20,3);
    CHECK(!s.present && !s.restoreInFlight && !s.restorePending &&
          !s.rearmAfterFlight);
    CHECK(LcObserve(s,false,0,0,100,20,4).action==LcAction::None);
}

static void test_lc_absence_reappearance_during_flight_queues_one_wave(){
    LcState s;
    LcObserve(s,true,1,10,100,20,0);
    LcDecision active=LcObserve(s,true,1,10,100,20,1);
    LcDecision missing=LcObserve(s,false,0,0,100,20,2);
    CHECK(missing.action==LcAction::MarkMissingFromLastSeen &&
          missing.generation!=0 && s.rearmAfterFlight);
    CHECK(LcObserve(s,true,1,11,100,20,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==active.generation &&
          s.rearmAfterFlight);
    LcRestoreCompleted(s,active.generation,LcRestoreOutcome::Success,100,20,4);
    CHECK(s.present && !s.restoreInFlight && s.restorePending &&
          !s.rearmAfterFlight);
    CHECK(LcObserve(s,true,1,11,100,20,5).action==LcAction::None);
    LcDecision rearmed=LcObserve(s,true,1,11,100,20,6);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=active.generation);
}

static void test_lc_firefox_chrome_edge_states_are_independent(){
    LcState firefox, chrome, edge;
    CHECK(LcObserve(firefox,true,10,10,10,10,0).action==LcAction::None);
    CHECK(LcObserve(chrome,true,20,20,20,20,0).action==LcAction::None);
    CHECK(LcObserve(edge,false,0,0,30,30,0).action==LcAction::MarkMissingFromLastSeen);
    LcDecision ff=LcObserve(firefox,true,10,10,10,10,1);
    LcDecision ch=LcObserve(chrome,true,20,20,20,20,1);
    CHECK(ff.action==LcAction::BeginRestore && ch.action==LcAction::BeginRestore);
    LcRestoreCompleted(firefox,ff.generation,LcRestoreOutcome::Success,10,10,2);
    LcRestoreCompleted(chrome,ch.generation,LcRestoreOutcome::Success,20,20,2);
    CHECK(LcObserve(edge,true,30,30,30,30,3).action==LcAction::None);
    CHECK(LcObserve(firefox,true,10,10,10,10,3).action==LcAction::None);
    CHECK(LcObserve(chrome,true,20,20,20,20,3).action==LcAction::None);
    CHECK(LcObserve(edge,true,30,30,30,30,4).action==LcAction::BeginRestore);
}

static void test_lc_layout_change_saves_but_restore_inputs_restore_first(){
    LcState s;
    LcObserve(s,true,1,1,10,1,0);
    LcDecision initial=LcObserve(s,true,1,1,10,1,1);
    LcRestoreCompleted(s,initial.generation,LcRestoreOutcome::Success,10,1,2);
    LcDecision save=LcObserve(s,true,1,1,11,1,3);
    CHECK(save.action==LcAction::SaveLayout && save.generation!=0);
    CHECK(s.saveInFlight && !s.restorePending && !s.restoreInFlight);

    LcState restoreFirst;
    LcObserve(restoreFirst,true,1,1,10,1,0);
    LcDecision done=LcObserve(restoreFirst,true,1,1,10,1,1);
    LcRestoreCompleted(restoreFirst,done.generation,LcRestoreOutcome::Success,10,1,2);
    CHECK(LcObserve(restoreFirst,true,1,2,11,2,3).action==LcAction::None);
    CHECK(LcObserve(restoreFirst,true,1,2,11,2,4).action==LcAction::BeginRestore);
}

static void test_lc_same_hwnd_new_fresh_session_starts_one_wave(){
    LcState s;
    LcObserve(s,true,7,70,700,1,0);
    LcDecision initial=LcObserve(s,true,7,70,700,1,1);
    LcRestoreCompleted(s,initial.generation,LcRestoreOutcome::Success,700,1,2);
    CHECK(LcObserve(s,true,7,71,700,2,3).action==LcAction::None);
    LcDecision refresh=LcObserve(s,true,7,71,700,2,4);
    CHECK(refresh.action==LcAction::BeginRestore && refresh.generation!=initial.generation);
    LcRestoreCompleted(s,refresh.generation,LcRestoreOutcome::Success,700,2,5);
    CHECK(LcObserve(s,true,7,71,700,2,6).action==LcAction::None);
    CHECK(LcObserve(s,true,7,71,700,2,7).action==LcAction::None);
}

static void test_lc_inflight_changes_queue_exactly_one_latest_rearm(){
    LcState s;
    LcObserve(s,true,1,10,100,1,0);
    LcDecision first=LcObserve(s,true,1,10,100,1,1);
    CHECK(LcObserve(s,true,2,20,101,2,2).action==LcAction::None);
    CHECK(LcObserve(s,true,3,30,102,3,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==first.generation && s.rearmAfterFlight);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,101,1,4);
    CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
    CHECK(s.windowSetSignature==3 && s.settleSignature==30 &&
          s.layoutSignature==102 && s.sessionStampSignature==3);
    CHECK(LcObserve(s,true,3,30,102,3,5).action==LcAction::None);
    LcDecision latest=LcObserve(s,true,3,30,102,3,6);
    CHECK(latest.action==LcAction::BeginRestore && latest.generation!=first.generation);
    LcRestoreCompleted(s,latest.generation,LcRestoreOutcome::Success,102,3,7);
    CHECK(LcObserve(s,true,3,30,102,3,8).action==LcAction::None);
}

static void test_lc_late_and_returning_sibling_each_start_one_wave(){
    LcState s;
    LcObserve(s,true,1,10,100,1,0);
    LcDecision onlyA=LcObserve(s,true,1,10,100,1,1);
    LcRestoreCompleted(s,onlyA.generation,LcRestoreOutcome::Success,100,1,2);

    CHECK(LcObserve(s,true,3,30,100,1,3).action==LcAction::None); // late B
    LcDecision withB=LcObserve(s,true,3,30,100,1,4);
    CHECK(withB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,withB.generation,LcRestoreOutcome::Success,100,1,5);
    CHECK(LcObserve(s,true,1,10,100,1,6).action==LcAction::None); // B disappears
    LcDecision withoutB=LcObserve(s,true,1,10,100,1,7);
    CHECK(withoutB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,withoutB.generation,LcRestoreOutcome::Success,100,1,8);
    CHECK(LcObserve(s,true,3,30,100,1,9).action==LcAction::None); // B returns
    LcDecision returnedB=LcObserve(s,true,3,30,100,1,10);
    CHECK(returnedB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,returnedB.generation,LcRestoreOutcome::Success,100,1,11);
    CHECK(LcObserve(s,true,3,30,100,1,12).action==LcAction::None);
}

static void test_lc_generation_max_is_issued_once_then_fails_closed(){
    LcState s;
    s.nextGeneration=UINT64_MAX;
    LcObserve(s,true,1,1,1,1,0);
    LcDecision last=LcObserve(s,true,1,1,1,1,1);
    CHECK(last.action==LcAction::BeginRestore && last.generation==UINT64_MAX);
    CHECK(s.nextGeneration==0);
    LcRestoreCompleted(s,last.generation,LcRestoreOutcome::Success,1,1,2);
    LcObserve(s,true,1,2,1,2,3);
    LcDecision exhausted=LcObserve(s,true,1,2,1,2,4);
    CHECK(exhausted.action==LcAction::None && exhausted.generation==0);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0 &&
          !s.restorePending && s.nextGeneration==0);
    const uint64_t completed=s.completedLayoutSignature;
    LcRestoreCompleted(s,UINT64_MAX,LcRestoreOutcome::Exhausted,999,999,5);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0 &&
          s.completedLayoutSignature==completed);
    CHECK(LcObserve(s,true,1,2,1,2,6).action==LcAction::None);
}

static LcDecision lc_begin_initial(LcState& s, uint64_t windowSet,
                                   uint64_t settle, uint64_t layout,
                                   uint64_t session, uint64_t now){
    CHECK(LcObserve(s,true,windowSet,settle,layout,session,now).action==LcAction::None);
    LcDecision begin=LcObserve(s,true,windowSet,settle,layout,session,now+1);
    CHECK(begin.action==LcAction::BeginRestore && begin.generation!=0);
    return begin;
}

static void test_lc_deferred_retries_three_times_with_exact_backoff(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    uint64_t completedAt=100;
    for(int attempt=1;attempt<=3;++attempt){
        LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                           100,20,completedAt);
        CHECK(s.deferredAttempts==attempt);
        if(attempt==3){
            CHECK(s.deferredUntilInputChanges && !s.restorePending &&
                  !s.restoreInFlight && s.retryNotBeforeMs==0);
            break;
        }
        const uint64_t readyAt=completedAt+30000;
        CHECK(s.restorePending && !s.deferredUntilInputChanges &&
              s.retryNotBeforeMs==readyAt);
        CHECK(LcObserve(s,true,1,10,100,20,completedAt+1).action==LcAction::None);
        CHECK(LcObserve(s,true,1,10,100,20,readyAt-1).action==LcAction::None);
        wave=LcObserve(s,true,1,10,100,20,readyAt);
        CHECK(wave.action==LcAction::BeginRestore && wave.generation!=0);
        completedAt=readyAt+100;
    }
    CHECK(LcObserve(s,true,1,10,101,20,1000000).action==LcAction::None);
    CHECK(s.deferredUntilInputChanges && s.deferredAttempts==3);
}

static void test_lc_deferred_key_change_resets_for_window_or_session(){
    for(int changeSession=0;changeSession<2;++changeSession){
        LcState s;
        LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
        uint64_t completedAt=100;
        for(int attempt=1;attempt<=3;++attempt){
            LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                               100,20,completedAt);
            if(attempt<3){
                const uint64_t readyAt=completedAt+30000;
                LcObserve(s,true,1,10,100,20,completedAt+1);
                wave=LcObserve(s,true,1,10,100,20,readyAt);
                CHECK(wave.action==LcAction::BeginRestore);
                completedAt=readyAt+100;
            }
        }
        const uint64_t changedWindow=changeSession ? 1 : 2;
        const uint64_t changedSession=changeSession ? 21 : 20;
        CHECK(LcObserve(s,true,changedWindow,11,100,changedSession,
                        completedAt+1).action==LcAction::None);
        CHECK(s.restorePending && !s.deferredUntilInputChanges &&
              s.deferredAttempts==0 && s.retryNotBeforeMs==0);
        LcDecision reset=LcObserve(s,true,changedWindow,11,100,changedSession,
                                   completedAt+2);
        CHECK(reset.action==LcAction::BeginRestore);
    }
}

static void test_lc_deferred_key_change_during_backoff_restarts_settle_now(){
    for(int changeSession=0;changeSession<2;++changeSession){
        LcState s;
        LcDecision first=lc_begin_initial(s,1,10,100,20,0);
        LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                           100,20,100);
        CHECK(s.deferredAttempts==1 && s.retryNotBeforeMs==30100);
        const uint64_t windowSet=changeSession ? 1 : 2;
        const uint64_t session=changeSession ? 21 : 20;
        CHECK(LcObserve(s,true,windowSet,11,100,session,1000).action==LcAction::None);
        CHECK(s.restorePending && s.deferredAttempts==0 &&
              s.retryNotBeforeMs==0 && s.stableSnapshots==1);
        LcDecision reset=LcObserve(s,true,windowSet,11,100,session,1001);
        CHECK(reset.action==LcAction::BeginRestore && reset.generation!=first.generation);
    }
}

static void test_lc_inflight_a_to_b_to_a_history_rearms_deferred_wave(){
    LcState s;
    LcDecision first=lc_begin_initial(s,1,10,100,20,0);
    CHECK(LcObserve(s,true,2,11,100,21,2).action==LcAction::None);
    CHECK(LcObserve(s,true,1,12,100,20,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==first.generation &&
          s.rearmAfterFlight && s.windowSetSignature==1 &&
          s.sessionStampSignature==20);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                       100,20,100);
    CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
    CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
          s.retryNotBeforeMs==0);
    CHECK(LcObserve(s,true,1,12,100,20,101).action==LcAction::None);
    LcDecision rearmed=LcObserve(s,true,1,12,100,20,102);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=first.generation);
}

static void test_lc_deferred_backoff_rebases_after_clock_rollback(){
    LcState s;
    LcDecision first=lc_begin_initial(s,1,10,100,20,10000);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                       100,20,10100);
    CHECK(s.retryNotBeforeMs==40100);
    CHECK(LcObserve(s,true,1,10,100,20,5000).action==LcAction::None);
    CHECK(s.retryNotBeforeMs==35000);
    CHECK(LcObserve(s,true,1,10,100,20,34999).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,35000).action==LcAction::BeginRestore);
}

static void test_lc_deferred_ignores_alternating_completion_session_payload(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    uint64_t completedAt=100;
    for(int attempt=1;attempt<=3;++attempt){
        const uint64_t untrustedCompletionSession=attempt%2 ? 99 : 20;
        LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                           100,untrustedCompletionSession,completedAt);
        CHECK(s.sessionStampSignature==20 && s.deferredAttempts==attempt);
        CHECK(s.deferredWindowSetSignature==1 &&
              s.deferredSessionStampSignature==20);
        if(attempt==3) break;
        const uint64_t readyAt=completedAt+30000;
        LcObserve(s,true,1,10,100,20,completedAt+1);
        wave=LcObserve(s,true,1,10,100,20,readyAt);
        CHECK(wave.action==LcAction::BeginRestore);
        completedAt=readyAt+100;
    }
    CHECK(s.deferredUntilInputChanges && !s.restorePending &&
          !s.restoreInFlight && s.retryNotBeforeMs==0);
    CHECK(LcObserve(s,true,1,10,100,20,1000000).action==LcAction::None);
}

static void test_lc_all_completion_outcomes_honor_one_queued_rearm(){
    const LcRestoreOutcome outcomes[]={
        LcRestoreOutcome::Success,
        LcRestoreOutcome::Deferred,
        LcRestoreOutcome::Exhausted
    };
    for(LcRestoreOutcome outcome:outcomes){
        LcState s;
        LcDecision first=lc_begin_initial(s,1,10,100,20,0);
        CHECK(LcObserve(s,true,2,11,101,21,2).action==LcAction::None);
        CHECK(LcObserve(s,true,3,12,102,22,3).action==LcAction::None);
        LcRestoreCompleted(s,first.generation,outcome,101,20,4);
        CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
        CHECK(s.windowSetSignature==3 && s.sessionStampSignature==22);
        CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
              s.retryNotBeforeMs==0);
        CHECK(LcObserve(s,true,3,12,102,22,5).action==LcAction::None);
        LcDecision latest=LcObserve(s,true,3,12,102,22,6);
        CHECK(latest.action==LcAction::BeginRestore && latest.generation!=first.generation);
    }
}

static void test_lc_exhausted_records_actual_layout_without_save_loop(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    CHECK(LcObserve(s,true,1,10,199,20,2).action==LcAction::None);
    LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Exhausted,199,20,3);
    CHECK(!s.restorePending && !s.restoreInFlight &&
          s.completedLayoutSignature==199);
    CHECK(LcObserve(s,true,1,10,199,20,4).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,199,20,5).action==LcAction::None);
}

static void test_lc_explicit_save_completion_is_generation_safe(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save=LcObserve(s,true,1,10,101,20,3);
    CHECK(save.action==LcAction::SaveLayout && s.saveInFlight);

    LcExplicitSaveCompleted(s,save.generation+1,101,20,4);
    CHECK(s.saveInFlight && s.saveGeneration==save.generation &&
          s.completedLayoutSignature==100);
    s.deferredAttempts=2;
    s.deferredUntilInputChanges=true;
    s.deferredWindowSetSignature=1;
    s.deferredSessionStampSignature=20;
    s.retryNotBeforeMs=99999;
    LcExplicitSaveCompleted(s,save.generation,101,20,5);
    CHECK(!s.saveInFlight && s.saveGeneration==0 &&
          s.completedLayoutSignature==101);
    CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
          s.retryNotBeforeMs==0);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0);
    CHECK(LcObserve(s,true,1,10,101,20,6).action==LcAction::None);

    LcDecision newer=LcObserve(s,true,1,10,102,20,7);
    CHECK(newer.action==LcAction::SaveLayout && newer.generation!=save.generation);
    LcExplicitSaveCompleted(s,save.generation,102,20,8);
    CHECK(s.saveInFlight && s.saveGeneration==newer.generation);
    LcExplicitSaveCompleted(s,newer.generation,102,20,9);
    CHECK(!s.saveInFlight && s.completedLayoutSignature==102);
}

static void test_restore_budgets_isolate_siblings_runtime_and_destination(){
    RestoreBudgets budgets;
    const RestoreBudgetKey failedA{"record-a","runtime-a","desktop-a"};
    const RestoreBudgetKey siblingB{"record-b","runtime-b","desktop-a"};
    const RestoreBudgetKey changedRuntime{"record-a","runtime-a-new","desktop-a"};
    const RestoreBudgetKey changedDestination{"record-a","runtime-a","desktop-b"};
    budgets.markExhausted(failedA);
    CHECK(!budgets.mayAttempt(failedA));
    CHECK(budgets.mayAttempt(siblingB));
    CHECK(budgets.mayAttempt(changedRuntime));
    CHECK(budgets.mayAttempt(changedDestination));

    budgets.markExhausted(siblingB);
    budgets.clearExact(siblingB); // verified success clears only B
    CHECK(budgets.mayAttempt(siblingB));
    CHECK(!budgets.mayAttempt(failedA)); // sibling/session churn cannot reset A

    budgets.markExhausted(changedRuntime);
    budgets.markExhausted(changedDestination);
    budgets.markExhausted(siblingB);
    budgets.clearForExplicitRetry("record-a");
    CHECK(budgets.mayAttempt(failedA));
    CHECK(budgets.mayAttempt(changedRuntime));
    CHECK(budgets.mayAttempt(changedDestination));
    CHECK(!budgets.mayAttempt(siblingB));
}

static void test_restore_budgets_prune_only_dead_runtime_identities(){
    RestoreBudgets budgets;
    const RestoreBudgetKey liveA{"record-a","runtime-a","desktop"};
    const RestoreBudgetKey liveB{"record-b","runtime-b","desktop"};
    const RestoreBudgetKey dead{"record-dead","runtime-dead","desktop"};
    budgets.markExhausted(liveA);
    budgets.markExhausted(liveB);
    budgets.markExhausted(dead);
    budgets.pruneToLiveIdentities({"runtime-a","runtime-b"});
    CHECK(!budgets.mayAttempt(liveA));
    CHECK(!budgets.mayAttempt(liveB));
    CHECK(budgets.mayAttempt(dead));
    CHECK(budgets.size()==2);
    budgets.pruneToLiveIdentities({});
    CHECK(budgets.size()==0 && budgets.mayAttempt(liveA));
}

static RestoreBudgetKey numbered_budget_key(int number){
    const std::string suffix=std::to_string(number);
    return {"record-"+suffix,"runtime-"+suffix,"desktop"};
}

static void test_restore_budgets_cap_uses_deterministic_touch_lru(){
    RestoreBudgets budgets;
    for(int i=0;i<256;++i) budgets.markExhausted(numbered_budget_key(i));
    CHECK(budgets.size()==256);
    CHECK(!budgets.mayAttempt(numbered_budget_key(0))); // refresh oldest
    budgets.markExhausted(numbered_budget_key(256));
    CHECK(budgets.size()==256);
    CHECK(!budgets.mayAttempt(numbered_budget_key(0)));
    CHECK(budgets.mayAttempt(numbered_budget_key(1)));  // least-recent untouched evicted
    CHECK(!budgets.mayAttempt(numbered_budget_key(2)));
    CHECK(!budgets.mayAttempt(numbered_budget_key(256)));
}

// --- minimal SNSS encoder mirroring the REAL format ---
// cmds 0/2/7/8 = raw fixed structs of two int32 (no pickle header); cmd 6 = base::Pickle
// (4-byte-aligned fields; url = UTF-8 WriteString, title = UTF-16 WriteString16).
static void wInt(std::string& p,int v){ uint32_t bits=(uint32_t)(int32_t)v; for(int i=0;i<4;i++)p.push_back((char)((bits>>(8*i))&0xFF)); }   // raw int32
static void pkInt(std::string& p,int v){ while(p.size()%4)p.push_back(0); wInt(p,v); }                 // aligned
static void pkStr(std::string& p,const std::string& s){ pkInt(p,(int)s.size()); p+=s; while(p.size()%4)p.push_back(0); }
static void pkStr16(std::string& p,const std::string& s){ pkInt(p,(int)s.size()); for(char ch:s){ p.push_back(ch); p.push_back(0);} while(p.size()%4)p.push_back(0); }
static void snssFrame(std::string& f,unsigned char id,const std::string& content){
    unsigned sz=(unsigned)(content.size()+1);
    f.push_back((char)(sz&0xFF)); f.push_back((char)((sz>>8)&0xFF)); f.push_back((char)id); f+=content;
}
static void snssRaw(std::string& f,unsigned char id,int a,int b){ std::string c; wInt(c,a); wInt(c,b); snssFrame(f,id,c); }
static void snssPickle(std::string& f,unsigned char id,const std::string& payload){ std::string pk; wInt(pk,(int)payload.size()); pk+=payload; snssFrame(f,id,pk); }
static std::string makeSnss(){
    std::string f="SNSS"; wInt(f,3);
    snssRaw(f,0,10,1); snssRaw(f,0,10,2); snssRaw(f,0,11,3);   // SetTabWindow [win,tab]
    snssRaw(f,2,1,0); snssRaw(f,2,2,1); snssRaw(f,2,3,0);      // SetTabIndexInWindow [tab,idx]
    snssRaw(f,8,10,1); snssRaw(f,8,11,0);                      // SetSelectedTabInIndex [win,idx]
    auto nav=[&](int tab,int idx,const std::string& url,const std::string& title){ std::string p; pkInt(p,tab); pkInt(p,idx); pkStr(p,url); pkStr16(p,title); snssPickle(f,6,p); };
    nav(1,0,"https://github.com/x","GitHub"); nav(2,0,"https://docs.python.org/3","Python"); nav(3,0,"https://example.com/","Example");
    snssRaw(f,7,1,0); snssRaw(f,7,2,0); snssRaw(f,7,3,0);      // SetSelectedNavigationIndex [tab,idx]
    return f;
}
static void test_snss_parse(){
    std::vector<WinFp> w;
    CHECK(ParseChromiumSNSS(makeSnss(),w));
    CHECK(w.size()==2);
    int wi10=-1,wi11=-1; for(int i=0;i<(int)w.size();++i){ if(w[i].counts.count("github.com"))wi10=i; if(w[i].counts.count("example.com"))wi11=i; }
    CHECK(wi10>=0 && wi11>=0);
    CHECK(w[wi10].tabCount==2); CHECK(w[wi10].counts["github.com"]==1); CHECK(w[wi10].counts["python.org"]==1);
    CHECK(w[wi10].activeTitle=="Python"); CHECK(w[wi10].activeDomain=="python.org");
    CHECK(w[wi10].tabsBlob.find("GitHub")!=std::string::npos);     // all-tab blob has BOTH tabs (not just active)
    CHECK(w[wi10].tabsBlob.find("python.org")!=std::string::npos);
    CHECK(w[wi10].tabsBlob.find("github.com/x")!=std::string::npos);  // full URL path is searchable, not just the domain
    CHECK(w[wi11].tabCount==1); CHECK(w[wi11].activeTitle=="Example");
}
static void test_snss_garbage(){ std::vector<WinFp> w(1); CHECK(!ParseChromiumSNSS("not an snss file....",w)); CHECK(w.empty()); }

static void test_snss_truncated_frame_returns_no_partial_windows(){
    std::string bytes=makeSnss(); bytes.resize(bytes.size()-2);
    std::vector<WinFp> windows(1);
    CHECK(!ParseChromiumSNSS(bytes,windows));
    CHECK(windows.empty());
}

static void test_mozlz4_rejects_huge_declared_output(){
    std::string bytes("mozLz40\0",8); bytes.append(4,(char)0xff);
    std::string output="sentinel";
    CHECK(!MozLz4Decompress(bytes,MAX_BROWSER_SESSION_BYTES,output));
    CHECK(output.empty());
}

static void test_session_stamp_detects_change(){
    SessionStamp a; a.size=10; a.mtime=20;
    SessionStamp b=a;
    CHECK(a==b); b.mtime=21; CHECK(a!=b);
    b=a; b.changeTime=22; CHECK(a!=b);
    b=a; b.volumeSerial=23; CHECK(a!=b);
    b=a; b.fileIdLow=24; CHECK(a!=b);
    b=a; b.fileIdHigh=25; CHECK(a!=b);
}

static void test_firefox_json_rejects_trailing_and_excessive_depth(){
    JValue value;
    CHECK(!JParser("{} trailing").parse(value));
    std::string deep(MAX_JSON_DEPTH+1,'['); deep+="null";
    deep+=std::string(MAX_JSON_DEPTH+1,']');
    CHECK(!JParser(deep).parse(value));
}

static bool parseJsonWithLimits(const std::string& input,JValue& output,
                                const JsonLimits& limits){
    return JParser(input,limits).parse(output);
}

static void test_firefox_json_rejects_malformed_unicode_numbers_and_controls(){
    JValue value;
    CHECK(JParser("{\"ok\":true}").parse(value));
    CHECK(value.t==JValue::OBJ && value.find("ok") && value.find("ok")->b);
    const char* invalid[]={
        "\"unterminated", "\"raw\nnewline\"", "\"\\x\"", "\"\\u12\"",
        "\"\\ud800\"", "\"\\ud800\\u0041\"", "\"\\udc00\"",
        "01", "-01", "1.", ".1", "1e", "1e+", "+1", "--1", "1e309",
        "NaN", "Infinity"
    };
    for(const char* text:invalid){
        value.t=JValue::OBJ; value.obj["sentinel"]=JValue{};
        CHECK(!JParser(text).parse(value));
        CHECK(value.t==JValue::NUL && value.obj.empty() && value.arr.empty() && value.str.empty());
    }
    CHECK(JParser("\"\\ud83d\\ude00\"").parse(value));
    CHECK(value.t==JValue::STR && value.str=="\xf0\x9f\x98\x80");
    CHECK(JParser("[-0,0,1.25,-2E-3,1e308]").parse(value));
    const std::string malformedUtf8[]={
        std::string("\"\xc0\x80\"",4), std::string("\"\x80\"",3),
        std::string("\"\xed\xa0\x80\"",5), std::string("\"\xf4\x90\x80\x80\"",6),
        std::string("\"\xe2\x82\"",4)
    };
    for(size_t i=0;i<sizeof(malformedUtf8)/sizeof(malformedUtf8[0]);++i){
        CHECK(!JParser(malformedUtf8[i]).parse(value));
        CHECK(value.t==JValue::NUL);
    }
    CHECK(!JParser("1e-9999").parse(value));
}

static void test_firefox_json_depth_node_and_string_budget_boundaries(){
    JValue value;
    std::string boundary(MAX_JSON_DEPTH,'['); boundary+="null";
    boundary+=std::string(MAX_JSON_DEPTH,']');
    CHECK(JParser(boundary).parse(value));

    JsonLimits twoNodes(MAX_JSON_DEPTH,2,64);
    CHECK(parseJsonWithLimits("[null]",value,twoNodes));
    CHECK(!parseJsonWithLimits("[null,null]",value,twoNodes));
    CHECK(value.t==JValue::NUL);

    CHECK(parseJsonWithLimits("{\"aa\":\"bb\"}",value,JsonLimits(MAX_JSON_DEPTH,8,4)));
    CHECK(!parseJsonWithLimits("{\"aa\":\"bb\"}",value,JsonLimits(MAX_JSON_DEPTH,8,3)));
    CHECK(value.t==JValue::NUL);
    CHECK(parseJsonWithLimits("\"\\ud83d\\ude00\"",value,JsonLimits(MAX_JSON_DEPTH,2,4)));
    CHECK(!parseJsonWithLimits("\"\\ud83d\\ude00\"",value,JsonLimits(MAX_JSON_DEPTH,2,3)));
}

static void test_browser_parser_default_limits_are_exact(){
    CHECK(MAX_JSON_DEPTH==128);
    CHECK(MAX_JSON_NODES==2000000);
    CHECK(MAX_JSON_DECODED_STRING_BYTES==256ULL*1024ULL*1024ULL);
    CHECK(MAX_BROWSER_SESSION_BYTES==512ULL*1024ULL*1024ULL);
    JsonLimits json;
    CHECK(json.maxDepth==128 && json.maxNodes==2000000);
    CHECK(json.maxDecodedStringBytes==256ULL*1024ULL*1024ULL);
    SnssLimits snss;
    CHECK(snss.maxWindows==10000 && snss.maxTabs==100000);
    CHECK(snss.maxNavigations==1000000 && snss.maxCommands==2000000);
    CHECK(snss.maxSearchTextPerWindow==4ULL*1024ULL*1024ULL);
    CHECK(snss.maxRetainedTextBytes==256ULL*1024ULL*1024ULL);
}

static std::string snssWithTabWindows(int count){
    std::string file="SNSS"; wInt(file,3);
    for(int i=0;i<count;++i) snssRaw(file,0,100+i,1000+i);
    return file;
}

static void test_snss_rejects_zero_trailing_and_malformed_known_commands(){
    std::vector<WinFp> output(1);
    std::string trailing="SNSS"; wInt(trailing,3); trailing.push_back('\1');
    CHECK(!ParseChromiumSNSS(trailing,output)); CHECK(output.empty());
    std::string zero="SNSS"; wInt(zero,3); zero.push_back('\0'); zero.push_back('\0');
    CHECK(!ParseChromiumSNSS(zero,output)); CHECK(output.empty());
    std::string shortRaw="SNSS"; wInt(shortRaw,3); snssFrame(shortRaw,0,std::string(7,'x'));
    CHECK(!ParseChromiumSNSS(shortRaw,output)); CHECK(output.empty());
    std::string longRaw="SNSS"; wInt(longRaw,3); snssFrame(longRaw,0,std::string(9,'x'));
    CHECK(!ParseChromiumSNSS(longRaw,output)); CHECK(output.empty());
    std::string badPickle="SNSS"; wInt(badPickle,3); std::string content; wInt(content,100);
    snssFrame(badPickle,6,content);
    CHECK(!ParseChromiumSNSS(badPickle,output)); CHECK(output.empty());
}

static void test_snss_unique_id_and_command_cap_boundaries(){
    std::vector<WinFp> output;
    SnssLimits limits(2,2,2,8,4,1024);
    CHECK(ParseChromiumSNSS(snssWithTabWindows(2),output,limits));
    CHECK(output.size()==2);
    CHECK(!ParseChromiumSNSS(snssWithTabWindows(3),output,limits)); CHECK(output.empty());

    std::string duplicate="SNSS"; wInt(duplicate,3);
    snssRaw(duplicate,0,1,10); snssRaw(duplicate,0,1,10);
    CHECK(ParseChromiumSNSS(duplicate,output,limits));
    std::string negative="SNSS"; wInt(negative,3); snssRaw(negative,0,-1,10);
    CHECK(!ParseChromiumSNSS(negative,output,limits)); CHECK(output.empty());

    std::string commands="SNSS"; wInt(commands,3);
    snssFrame(commands,99,""); snssFrame(commands,99,""); snssFrame(commands,99,"");
    SnssLimits twoCommands(2,2,2,2,64,1024);
    CHECK(!ParseChromiumSNSS(commands,output,twoCommands)); CHECK(output.empty());
}

static void test_snss_window_and_tab_maps_have_independent_exact_caps(){
    std::vector<WinFp> output;
    std::string windows="SNSS"; wInt(windows,3);
    snssRaw(windows,8,1,0); snssRaw(windows,8,2,0);
    CHECK(ParseChromiumSNSS(windows,output,SnssLimits(2,2,2,2,64,64)) && output.empty());
    snssRaw(windows,8,3,0);
    CHECK(!ParseChromiumSNSS(windows,output,SnssLimits(2,3,2,3,64,64)) && output.empty());
    std::string tabs="SNSS"; wInt(tabs,3);
    snssRaw(tabs,2,10,0); snssRaw(tabs,2,20,0);
    CHECK(ParseChromiumSNSS(tabs,output,SnssLimits(2,2,2,2,64,64)) && output.empty());
    snssRaw(tabs,2,30,0);
    CHECK(!ParseChromiumSNSS(tabs,output,SnssLimits(2,2,2,3,64,64)) && output.empty());
}

static void test_snss_navigation_duplicate_and_search_budget_boundaries(){
    std::vector<WinFp> output;
    SnssLimits limits(2,3,2,16,4,1024);
    std::string exact="SNSS"; wInt(exact,3);
    snssRaw(exact,0,1,10); snssRaw(exact,0,1,11);
    CHECK(ParseChromiumSNSS(exact,output,limits));
    CHECK(output.size()==1 && output[0].tabsBlob.size()==4);
    snssRaw(exact,0,1,12);
    CHECK(!ParseChromiumSNSS(exact,output,limits)); CHECK(output.empty());

    auto nav=[&](std::string& file,int tab,int index){
        std::string payload; pkInt(payload,tab); pkInt(payload,index);
        pkStr(payload,"https://example.com/"); pkStr16(payload,"T");
        snssPickle(file,6,payload);
    };
    std::string navigations="SNSS"; wInt(navigations,3);
    nav(navigations,10,0); nav(navigations,10,0); nav(navigations,10,1);
    SnssLimits navLimits(2,3,2,16,128,1024);
    CHECK(ParseChromiumSNSS(navigations,output,navLimits));
    nav(navigations,10,2);
    CHECK(!ParseChromiumSNSS(navigations,output,navLimits)); CHECK(output.empty());
}

static std::string snssWithNavigationText(int window,int tab,int navigation,
                                          const std::string& url,const std::string& title){
    std::string file="SNSS"; wInt(file,3); snssRaw(file,0,window,tab);
    std::string payload; pkInt(payload,tab); pkInt(payload,navigation);
    pkStr(payload,url); pkStr16(payload,title); snssPickle(file,6,payload);
    return file;
}

static void test_snss_per_window_and_global_text_caps_are_exact(){
    std::vector<WinFp> output(1);
    std::string one=snssWithNavigationText(1,10,0,"abc","");
    CHECK(ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,5,3)));
    CHECK(output.size()==1 && output[0].tabsBlob==" abc ");
    CHECK(!ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,4,3)) && output.empty());
    CHECK(!ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,5,2)) && output.empty());

    std::string two="SNSS"; wInt(two,3);
    snssRaw(two,0,1,10); snssRaw(two,0,2,20);
    std::string first; pkInt(first,10); pkInt(first,0); pkStr(first,"abc"); pkStr16(first,"");
    std::string second; pkInt(second,20); pkInt(second,0); pkStr(second,"def"); pkStr16(second,"");
    snssPickle(two,6,first); snssPickle(two,6,second);
    CHECK(ParseChromiumSNSS(two,output,SnssLimits(2,2,2,4,5,6)) && output.size()==2);
    CHECK(!ParseChromiumSNSS(two,output,SnssLimits(2,2,2,4,5,5)) && output.empty());
    CHECK(!ParseChromiumSNSS(two,output,SnssLimits(2,2,2,3,5,6)) && output.empty());
}

static std::string makeMozLiteral(const std::string& decoded){
    std::string bytes("mozLz40\0",8); wInt(bytes,(int)decoded.size());
    if(decoded.empty()) return bytes;
    size_t literal=decoded.size();
    bytes.push_back((char)((literal<15?literal:15)<<4));
    if(literal>=15){ size_t extra=literal-15;
        while(extra>=255){ bytes.push_back((char)255); extra-=255; }
        bytes.push_back((char)extra);
    }
    bytes+=decoded; return bytes;
}

static void test_mozlz4_exact_decode_and_limit_boundaries(){
    std::string output="sentinel";
    CHECK(MozLz4Decompress(makeMozLiteral(""),MAX_BROWSER_SESSION_BYTES,output)); CHECK(output.empty());
    CHECK(MozLz4Decompress(makeMozLiteral("hello"),5,output)); CHECK(output=="hello");
    output="sentinel";
    CHECK(!MozLz4Decompress(makeMozLiteral("hello"),4,output)); CHECK(output.empty());
}

static void test_mozlz4_rejects_malformed_blocks_transactionally(){
    std::vector<std::string> malformed;
    malformed.push_back("");
    malformed.push_back(std::string("mozLz40\0",8));
    malformed.push_back(std::string("badLz400\0\0\0\0",12));
    std::string extension("mozLz40\0",8); wInt(extension,16);
    extension.push_back((char)0xf0); extension.push_back((char)255); malformed.push_back(extension);
    std::string zeroOffset("mozLz40\0",8); wInt(zeroOffset,5);
    zeroOffset.push_back((char)0x10); zeroOffset.push_back('a'); zeroOffset.push_back('\0'); zeroOffset.push_back('\0');
    malformed.push_back(zeroOffset);
    std::string trailing=makeMozLiteral("a"); trailing.push_back('x'); malformed.push_back(trailing);
    std::string emptyTrailing=makeMozLiteral(""); emptyTrailing.push_back('\0'); malformed.push_back(emptyTrailing);
    for(const auto& bytes:malformed){
        std::string output="sentinel";
        CHECK(!MozLz4Decompress(bytes,MAX_BROWSER_SESSION_BYTES,output)); CHECK(output.empty());
    }
}

static void test_lz4_match_offset_and_extension_arithmetic_edges(){
    uint8_t output[32]={0};
    const uint8_t repeated[]={0x10,'a',0x01,0x00};
    CHECK(Lz4BlockDecompress(repeated,sizeof(repeated),output,5)==5);
    CHECK(std::string((char*)output,5)=="aaaaa");
    const uint8_t distant[]={0x10,'a',0x02,0x00};
    CHECK(Lz4BlockDecompress(distant,sizeof(distant),output,5)<0);
    const uint8_t truncatedOffset[]={0x10,'a',0x01};
    CHECK(Lz4BlockDecompress(truncatedOffset,sizeof(truncatedOffset),output,5)<0);
    const uint8_t truncatedMatchExtension[]={0x1f,'a',0x01,0x00,0xff};
    CHECK(Lz4BlockDecompress(truncatedMatchExtension,sizeof(truncatedMatchExtension),output,sizeof(output))<0);
    CHECK(Lz4BlockDecompress(nullptr,1,output,sizeof(output))<0);
    CHECK(Lz4BlockDecompress(repeated,sizeof(repeated),nullptr,5)<0);

    size_t length=(std::numeric_limits<size_t>::max)()-1;
    CHECK(Lz4CheckedAdd(length,1) && length==(std::numeric_limits<size_t>::max)());
    CHECK(!Lz4CheckedAdd(length,1) && length==(std::numeric_limits<size_t>::max)());
}

static bool setSparseSessionFileSize(const std::wstring& path,unsigned long long size){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position; position.QuadPart=(LONGLONG)size;
    bool ok=SetFilePointerEx(file,position,nullptr,FILE_BEGIN)!=FALSE && SetEndOfFile(file)!=FALSE;
    CloseHandle(file); return ok;
}

static bool writeSessionStampFixture(const std::wstring& path,const std::string& bytes,
                                     const FILETIME& mtime,DWORD disposition){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
                            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER zero{};
    DWORD written=0;
    bool ok=SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE &&
            WriteFile(file,bytes.data(),(DWORD)bytes.size(),&written,nullptr)!=FALSE &&
            written==bytes.size() && SetEndOfFile(file)!=FALSE &&
            SetFileTime(file,nullptr,nullptr,&mtime)!=FALSE && FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    return ok;
}

static void test_session_stamp_detects_equal_metadata_replace_and_in_place_rewrite(){
    wchar_t temp[MAX_PATH+1]={0},pathA[MAX_PATH+1]={0},pathB[MAX_PATH+1]={0};
    DWORD length=GetTempPathW(MAX_PATH,temp);
    CHECK(length>0 && length<=MAX_PATH); if(length==0 || length>MAX_PATH) return;
    CHECK(GetTempFileNameW(temp,L"vdi",0,pathA)!=0);
    CHECK(GetTempFileNameW(temp,L"vdi",0,pathB)!=0);
    FILETIME fixedTime{}; GetSystemTimeAsFileTime(&fixedTime);
    CHECK(writeSessionStampFixture(pathA,"AAAA",fixedTime,CREATE_ALWAYS));
    CHECK(writeSessionStampFixture(pathB,"BBBB",fixedTime,CREATE_ALWAYS));
    SessionStamp original,replacementObject;
    CHECK(GetSessionStamp(pathA,original));
    CHECK(GetSessionStamp(pathB,replacementObject));
    CHECK(original.size==replacementObject.size && original.mtime==replacementObject.mtime);
    CHECK(original!=replacementObject);
    CHECK(MoveFileExW(pathB,pathA,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE);
    SessionStamp replaced;
    CHECK(GetSessionStamp(pathA,replaced));
    CHECK(replaced.size==original.size && replaced.mtime==original.mtime && replaced!=original);
    CHECK(replaced.volumeSerial==replacementObject.volumeSerial &&
          replaced.fileIdLow==replacementObject.fileIdLow && replaced.fileIdHigh==replacementObject.fileIdHigh);

    Sleep(20);
    CHECK(writeSessionStampFixture(pathA,"CCCC",fixedTime,OPEN_EXISTING));
    SessionStamp rewritten;
    CHECK(GetSessionStamp(pathA,rewritten));
    CHECK(rewritten.size==replaced.size && rewritten.mtime==replaced.mtime &&
          rewritten.volumeSerial==replaced.volumeSerial && rewritten.fileIdLow==replaced.fileIdLow &&
          rewritten.fileIdHigh==replaced.fileIdHigh && rewritten.changeTime!=replaced.changeTime);
    CHECK(rewritten!=replaced);
    DWORD handlesBefore=0,handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<256;++attempt){
        SessionStamp repeated;
        CHECK(GetSessionStamp(pathA,repeated) && repeated==rewritten);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
    CHECK(DeleteFileW(pathA)!=FALSE);
    DeleteFileW(pathB);
}

static void test_get_session_stamp_accepts_exact_cap_and_rejects_over(){
    wchar_t temp[MAX_PATH+1]={0},path[MAX_PATH+1]={0};
    DWORD length=GetTempPathW(MAX_PATH,temp);
    CHECK(length>0 && length<=MAX_PATH);
    if(length==0 || length>MAX_PATH) return;
    CHECK(GetTempFileNameW(temp,L"vds",0,path)!=0);
    if(!path[0]) return;
    CHECK(setSparseSessionFileSize(path,MAX_BROWSER_SESSION_BYTES));
    SessionStamp stamp; stamp.size=7; stamp.mtime=8;
    CHECK(GetSessionStamp(path,stamp) && stamp.size==MAX_BROWSER_SESSION_BYTES);
    CHECK(setSparseSessionFileSize(path,MAX_BROWSER_SESSION_BYTES+1));
    SessionStamp sentinel; sentinel.size=17; sentinel.mtime=18; stamp=sentinel;
    CHECK(!GetSessionStamp(path,stamp) && stamp==sentinel);
    CHECK(DeleteFileW(path)!=FALSE);
}

static void test_firefox_profile_ini_default_release_fallback(){
    const std::string fallback="[Profile0]\nName=release\nIsRelative=1\nPath=Profiles/demo.default-release\n";
    CHECK(ResolveFirefoxProfileDirectoryFromIni(L"C:\\Firefox",fallback)==
          L"C:\\Firefox\\Profiles\\demo.default-release");
    const std::string installed="[Install123]\nDefault=Profiles/main\n[Profile0]\nDefault=1\nPath=Profiles/other\n";
    CHECK(ResolveFirefoxProfileDirectoryFromIni(L"C:\\Firefox",installed)==
          L"C:\\Firefox\\Profiles\\main");
}

static void test_firefox_json_valid_empty_is_distinct_from_failure(){
    std::vector<WinFp> output(1);
    CHECK(ParseFirefoxSessionJson("{\"windows\":[]}",output)); CHECK(output.empty());
    output.push_back(WinFp{});
    CHECK(!ParseFirefoxSessionJson("{\"windows\":[",output)); CHECK(output.empty());
}

static void test_firefox_selected_index_rejects_int_min_without_overflow(){
    CHECK(!FirefoxSelectedTabMatches(INT_MIN,0));
    CHECK(!FirefoxSelectedTabMatches(0,0));
    CHECK(FirefoxSelectedTabMatches(1,0));
    CHECK(FirefoxSelectedTabMatches(INT_MAX,(size_t)INT_MAX-1));
    std::vector<WinFp> output;
    CHECK(ParseFirefoxSessionJson(
        "{\"windows\":[{\"selected\":-2147483648,\"tabs\":[{\"entries\":[{\"url\":\"https://example.com\",\"title\":\"Example\"}]}]}]}",
        output));
    CHECK(output.size()==1 && output[0].activeTitle.empty() && output[0].activeDomain.empty());
}

static AppProfile sessionTestProfile(const std::string& app,AppProfile::Sess session=AppProfile::FIREFOX){
    AppProfile profile;
    profile.id=app;
    profile.classNames.push_back(L"TestWindow");
    profile.exeName=L"test.exe";
    profile.titleSuffixes.push_back(L" - Test");
    profile.session=session;
    profile.userDataDir=L"test-data";
    return profile;
}

struct SessionPathFixture {
    std::wstring root,defaultDir,sessionsDir;
    std::vector<std::wstring> files;
    bool ready=false;
    SessionPathFixture(){
        wchar_t temp[MAX_PATH+1]={0},uniquePath[MAX_PATH+1]={0};
        DWORD length=GetTempPathW(MAX_PATH,temp);
        if(length==0 || length>MAX_PATH || !GetTempFileNameW(temp,L"vdp",0,uniquePath)) return;
        root=uniquePath;
        if(!DeleteFileW(root.c_str()) || !CreateDirectoryW(root.c_str(),nullptr)) return;
        defaultDir=root+L"\\Default"; sessionsDir=defaultDir+L"\\Sessions";
        if(!CreateDirectoryW(defaultDir.c_str(),nullptr) || !CreateDirectoryW(sessionsDir.c_str(),nullptr)) return;
        ready=true;
    }
    bool add(const wchar_t* name,unsigned long long modified,unsigned long long size=1){
        if(!ready) return false;
        std::wstring path=sessionsDir+L"\\"+name;
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER end; end.QuadPart=(LONGLONG)size;
        FILETIME time; ULARGE_INTEGER bits; bits.QuadPart=modified;
        time.dwLowDateTime=bits.LowPart; time.dwHighDateTime=bits.HighPart;
        bool ok=SetFilePointerEx(file,end,nullptr,FILE_BEGIN)!=FALSE && SetEndOfFile(file)!=FALSE &&
                SetFileTime(file,nullptr,nullptr,&time)!=FALSE;
        CloseHandle(file);
        if(ok) files.push_back(path); else DeleteFileW(path.c_str());
        return ok;
    }
    ~SessionPathFixture(){
        for(size_t i=0;i<files.size();++i) DeleteFileW(files[i].c_str());
        if(!sessionsDir.empty()) RemoveDirectoryW(sessionsDir.c_str());
        if(!defaultDir.empty()) RemoveDirectoryW(defaultDir.c_str());
        if(!root.empty()) RemoveDirectoryW(root.c_str());
    }
};

static void test_chromium_resolver_tracks_rotation_and_breaks_stamp_ties(){
    SessionPathFixture fixture;
    CHECK(fixture.ready); if(!fixture.ready) return;
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    profile.userDataDir=fixture.root;
    CHECK(fixture.add(L"Session_A",100));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_A");
    CHECK(fixture.add(L"Session_B",200));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_B");
    CHECK(fixture.add(L"Session_C",200));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_C");
    CHECK(fixture.add(L"Session_zero",300,0));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_C");
}

struct SessionResultSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<SessionResult> > results;
    std::atomic<bool> wrongMessage{false};
    bool post(HWND,UINT message,WPARAM,LPARAM value){
        if(message!=WM_SESSION_RESULT) wrongMessage=true;
        std::unique_ptr<SessionResult> owned((SessionResult*)value);
        { std::lock_guard<std::mutex> lock(mutex); results.push_back(std::move(owned)); }
        changed.notify_all();
        return true;
    }
    std::unique_ptr<SessionResult> waitFor(uint64_t requestId){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock,std::chrono::seconds(5),[&]{
            for(size_t i=0;i<results.size();++i) if(results[i]->requestId==requestId) return true;
            return false;
        });
        for(std::deque<std::unique_ptr<SessionResult> >::iterator it=results.begin();it!=results.end();++it){
            if((*it)->requestId==requestId){
                std::unique_ptr<SessionResult> found=std::move(*it);
                results.erase(it);
                return found;
            }
        }
        return std::unique_ptr<SessionResult>();
    }
};

static void test_already_posted_session_results_are_drained_and_freed(){
    HWND window=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,
                                GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr); if(!window) return;
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    SessionResult* result=new SessionResult(); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(PostMessageW(window,WM_SESSION_RESULT,0,(LPARAM)result)!=FALSE);
    CHECK(DrainPostedSessionResults(window)==1);
    CHECK(payload.use_count()==1);
    CHECK(DrainPostedSessionResults(window)==0);
    CHECK(DestroyWindow(window)!=FALSE);
}

static void test_session_status_and_acceptance_policy_contract(){
    CHECK(SessionDataUsable(SessionDataStatus::Fresh));
    CHECK(SessionDataUsable(SessionDataStatus::CachedStale));
    CHECK(!SessionDataUsable(SessionDataStatus::Unavailable));
    CHECK(!SessionDataUsable(SessionDataStatus::Superseded));
    SessionPolicy fresh=SessionAcceptancePolicy(SessionDataStatus::Fresh);
    CHECK(fresh.matchExisting && fresh.restoreExisting && fresh.createUnmatched);
    CHECK(fresh.updateFingerprints && fresh.markMissing && !fresh.deferOnce);
    SessionPolicy stale=SessionAcceptancePolicy(SessionDataStatus::CachedStale);
    CHECK(stale.matchExisting && stale.restoreExisting && !stale.createUnmatched);
    CHECK(!stale.updateFingerprints && !stale.markMissing && stale.unmatchedLiveWaits);
    SessionPolicy unavailable=SessionAcceptancePolicy(SessionDataStatus::Unavailable);
    CHECK(!unavailable.matchExisting && !unavailable.restoreExisting && !unavailable.createUnmatched);
    CHECK(!unavailable.updateFingerprints && !unavailable.markMissing && unavailable.deferOnce);
    SessionPolicy superseded=SessionAcceptancePolicy(SessionDataStatus::Superseded);
    CHECK(!superseded.matchExisting && !superseded.restoreExisting && !superseded.deferOnce);
}

static void test_session_cache_shares_payload_and_rejects_oversize(){
    std::vector<WinFp> seed(1); seed[0].tabsBlob="payload";
    size_t bytes=EstimateSessionPayloadBytes(seed);
    SessionCache cache(2,bytes);
    SessionCacheValue stored;
    SessionStamp stamp; stamp.size=10; stamp.mtime=20;
    CHECK(cache.Put("firefox",L"one",stamp,std::move(seed),101,1,stored));
    CHECK(stored.windows && cache.EntryCount()==1 && cache.RetainedBytes()==bytes);
    SessionCacheValue hit;
    CHECK(cache.FindExact("firefox",L"one",stamp,hit));
    CHECK(hit.windows.get()==stored.windows.get());

    std::vector<WinFp> second(1); second[0].tabsBlob="second";
    SessionCacheValue rejected;
    CHECK(!cache.Put("chrome",L"two",stamp,std::move(second),102,2,rejected));
    CHECK(!rejected.windows);
    stored.windows.reset(); hit.windows.reset();
    CHECK(cache.RetainedBytes()<=bytes);
}

static SessionFileReadResult successfulSessionRead(const std::string& bytes,
                                                    const SessionStamp& stamp){
    SessionFileReadResult result;
    result.status=FileReadStatus::Ok;
    result.bytes=bytes;
    result.readStamp=stamp;
    result.readStampKnown=true;
    return result;
}

static SessionFileReadResult successfulSessionRead(const std::string& bytes,
        unsigned long long size,unsigned long long mtime){
    SessionStamp stamp; stamp.size=size; stamp.mtime=mtime;
    return successfulSessionRead(bytes,stamp);
}

static void test_session_worker_valid_empty_is_fresh_and_cache_hit_is_shared(){
    SessionResultSink sink;
    std::atomic<int> reads(0),parses(0),stamps(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"session-file"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ ++stamps; stamp.size=5; stamp.mtime=9; return true; };
    ops.readFile=[&](const std::wstring&){ ++reads; return successfulSessionRead("valid",5,9); };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){ ++parses; output.clear(); return bytes=="valid"; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,1024*1024);
    SessionRequest first;
    first.requestId=1; first.app="firefox"; first.profile=sessionTestProfile("firefox");
    first.purpose=SessionPurpose::ManualRestore; first.identityGeneration=7;
    CHECK(worker.Request(first));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(1);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows && fresh->windows->empty());
    CHECK(fresh && fresh->purpose==SessionPurpose::ManualRestore && fresh->identityGeneration==7);
    SessionStamp expectedStamp; expectedStamp.size=5; expectedStamp.mtime=9;
    CHECK(fresh && fresh->sourceStampKnown && fresh->sourceStamp==expectedStamp);
    CHECK(fresh && fresh->dataStamp==expectedStamp && fresh->dataGeneration==1);
    const std::vector<WinFp>* identity=fresh?fresh->windows.get():nullptr;
    SessionRequest second=first; second.requestId=2; second.purpose=SessionPurpose::Search;
    CHECK(worker.Request(second));
    std::unique_ptr<SessionResult> cached=sink.waitFor(2);
    CHECK(cached && cached->status==SessionDataStatus::Fresh && cached->windows.get()==identity);
    CHECK(cached && cached->purpose==SessionPurpose::Search && cached->dataGeneration==1);
    CHECK(reads.load()==1 && parses.load()==1);
    worker.Stop();
    SessionRequest rejected=first; rejected.requestId=3;
    CHECK(!worker.Request(rejected));
}

static void test_session_worker_malformed_cold_is_unavailable(){
    SessionResultSink sink;
    std::atomic<int> parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"cold"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=11; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("malformed",11,1); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){ ++parses; output.push_back(WinFp{}); return false; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=20; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::AutoReconcile; request.identityGeneration=2;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(20);
    SessionStamp current; current.size=11; current.mtime=1;
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(result && result->sourceStampKnown && result->sourceStamp==current);
    CHECK(result && result->dataStamp==SessionStamp{} && result->dataGeneration==0);
    CHECK(parses.load()==1);
    worker.Stop();
}

static void test_session_worker_non_ok_reads_never_parse_and_publish_current_stamp(){
    SessionResultSink sink;
    std::atomic<int> reads(0),parses(0);
    const FileReadStatus statuses[]={FileReadStatus::Missing,FileReadStatus::Unavailable,FileReadStatus::TooLarge};
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"read-gated"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=23; stamp.mtime=45; return true; };
    ops.readFile=[&](const std::wstring&){ SessionFileReadResult result; result.status=statuses[reads++]; return result; };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>&){ ++parses; return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    SessionStamp current; current.size=23; current.mtime=45;
    for(int i=0;i<3;++i){
        request.requestId=30+i;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> result=sink.waitFor(request.requestId);
        CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
        CHECK(result && result->path==L"read-gated" && result->sourceStampKnown && result->sourceStamp==current);
        CHECK(result && result->dataStamp==SessionStamp{} && result->dataGeneration==0);
    }
    CHECK(reads.load()==3 && parses.load()==0);
    worker.Stop();
}

static void test_session_worker_disappeared_source_is_not_reported_as_current(){
    SessionResultSink sink;
    std::atomic<int> resolves(0),stampCalls(0);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ return ++resolves==1?std::wstring(L"gone"):std::wstring(); };
    ops.getStamp=[&](const std::wstring& path,SessionStamp& stamp){ ++stampCalls; stamp.size=4; stamp.mtime=5; return path==L"gone"; };
    ops.readFile=[](const std::wstring&){ SessionFileReadResult result; result.status=FileReadStatus::Missing; return result; };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>&){ return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=35; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(35);
    CHECK(result && result->status==SessionDataStatus::Unavailable);
    CHECK(result && result->path.empty() && !result->sourceStampKnown && result->sourceStamp==SessionStamp{});
    CHECK(stampCalls.load()==1);
    worker.Stop();
}

static void test_session_worker_stamp_change_uses_exact_path_cached_stale(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<bool> parseOk(true);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"same-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=9; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("bytes",9,mtime.load()); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp window; window.activeTitle="cached"; output.push_back(window); return parseOk.load();
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=21; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::HeartbeatSave; request.identityGeneration=4;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(21);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows->size()==1);
    const std::vector<WinFp>* identity=fresh?fresh->windows.get():nullptr;
    uint64_t generation=fresh?fresh->dataGeneration:0;

    mtime=2; parseOk=false; request.requestId=22;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(22);
    SessionStamp source; source.size=9; source.mtime=2;
    SessionStamp data; data.size=9; data.mtime=1;
    CHECK(stale && stale->status==SessionDataStatus::CachedStale && stale->windows.get()==identity);
    CHECK(stale && stale->sourceStampKnown && stale->sourceStamp==source);
    CHECK(stale && stale->dataStamp==data && stale->dataGeneration==generation);
    worker.Stop();
}

static void test_session_worker_rotation_during_parse_is_never_fresh(){
    SessionResultSink sink;
    std::atomic<int> resolves(0);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ return ++resolves==1?std::wstring(L"Session_old"):std::wstring(L"Session_new"); };
    ops.getStamp=[](const std::wstring& path,SessionStamp& stamp){ stamp.size=100; stamp.mtime=path==L"Session_old"?1:2; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("old",100,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); output.push_back(WinFp{}); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=23; request.app="chrome"; request.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    request.purpose=SessionPurpose::MetadataProbe; request.identityGeneration=8;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(23);
    SessionStamp current; current.size=100; current.mtime=2;
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(result && result->path==L"Session_new" && result->sourceStampKnown && result->sourceStamp==current);
    CHECK(result && result->dataGeneration==0 && result->dataStamp==SessionStamp{});
    worker.Stop();
}

static void test_session_worker_equal_metadata_replacement_never_publishes_old_bytes_fresh(){
    SessionResultSink sink;
    std::atomic<unsigned long long> objectId(1);
    std::atomic<bool> replaceDuringParse(true);
    std::atomic<int> reads(0),parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"same-metadata-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){
        unsigned long long identity=objectId.load();
        stamp.size=4; stamp.mtime=100; stamp.changeTime=1000+identity;
        stamp.volumeSerial=7; stamp.fileIdLow=identity; stamp.fileIdHigh=9;
        return true;
    };
    ops.readFile=[&](const std::wstring&){
        ++reads;
        unsigned long long identity=objectId.load();
        SessionStamp stamp; stamp.size=4; stamp.mtime=100; stamp.changeTime=1000+identity;
        stamp.volumeSerial=7; stamp.fileIdLow=identity; stamp.fileIdHigh=9;
        return successfulSessionRead(std::string(4,(char)('A'+(int)identity-1)),stamp);
    };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){
        ++parses; output.clear(); WinFp window; window.activeTitle=bytes; output.push_back(std::move(window));
        if(replaceDuringParse.exchange(false)) ++objectId;
        return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,4,1024*1024);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");

    request.requestId=620; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> coldRotated=sink.waitFor(620);
    CHECK(coldRotated && coldRotated->status==SessionDataStatus::Unavailable && !coldRotated->windows &&
          coldRotated->sourceStampKnown && coldRotated->sourceStamp.fileIdLow==2);

    request.requestId=621; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> freshB=sink.waitFor(621);
    CHECK(freshB && freshB->status==SessionDataStatus::Fresh && freshB->windows->at(0).activeTitle=="BBBB" &&
          freshB->dataStamp.fileIdLow==2 && freshB->dataGeneration==1);
    const std::vector<WinFp>* bIdentity=freshB?freshB->windows.get():nullptr;
    request.requestId=622; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> hitB=sink.waitFor(622);
    CHECK(hitB && hitB->status==SessionDataStatus::Fresh && hitB->windows.get()==bIdentity &&
          hitB->dataGeneration==1 && reads.load()==2 && parses.load()==2);

    objectId=3; replaceDuringParse=true; request.requestId=623; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> staleB=sink.waitFor(623);
    CHECK(staleB && staleB->status==SessionDataStatus::CachedStale && staleB->windows.get()==bIdentity &&
          staleB->dataStamp.fileIdLow==2 && staleB->sourceStampKnown && staleB->sourceStamp.fileIdLow==4);
    request.requestId=624; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> freshD=sink.waitFor(624);
    CHECK(freshD && freshD->status==SessionDataStatus::Fresh && freshD->windows->at(0).activeTitle=="DDDD" &&
          freshD->dataStamp.fileIdLow==4 && freshD->dataGeneration==2);
    worker.Stop();
}

static SessionStamp syntheticSessionStamp(unsigned long long revision){
    SessionStamp stamp;
    stamp.size=1; stamp.mtime=100+revision; stamp.changeTime=200+revision;
    stamp.volumeSerial=300; stamp.fileIdLow=400+revision; stamp.fileIdHigh=500;
    return stamp;
}

static void test_session_worker_rejects_aba_bytes_without_matching_handle_stamp(){
    SessionStamp endpoint=syntheticSessionStamp(1);
    SessionStamp bytesStamp=endpoint;
    std::string bytes="A";
    std::atomic<int> parses(0);
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"aba-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp=endpoint; return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead(bytes,bytesStamp); };
    ops.parse=[&](const AppProfile&,const std::string& input,std::vector<WinFp>& output){
        ++parses;
        output.clear(); WinFp window; window.activeTitle=input; output.push_back(window); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };

    SessionWorker cachedWorker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=203; CHECK(cachedWorker.Request(request));
    std::unique_ptr<SessionResult> seeded=sink.waitFor(203);
    CHECK(seeded && seeded->status==SessionDataStatus::Fresh && seeded->windows &&
          seeded->windows->at(0).activeTitle=="A" && seeded->dataStamp==endpoint);
    const std::vector<WinFp>* cachedIdentity=seeded?seeded->windows.get():nullptr;

    SessionStamp cachedStamp=endpoint;
    endpoint=syntheticSessionStamp(2);       // A is current at both endpoint observations.
    bytesStamp=syntheticSessionStamp(9);     // The exact read handle belonged to B.
    bytes="B";
    request.requestId=204; CHECK(cachedWorker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(204);
    CHECK(stale && stale->status==SessionDataStatus::CachedStale &&
          stale->windows.get()==cachedIdentity && stale->windows->at(0).activeTitle=="A");
    CHECK(stale && stale->sourceStampKnown && stale->sourceStamp==endpoint &&
          stale->dataStamp==cachedStamp);
    CHECK(cachedWorker.Stop());

    SessionResultSink coldSink;
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return coldSink.post(hwnd,message,wp,lp); };
    SessionWorker coldWorker((HWND)1,ops);
    request.requestId=205; CHECK(coldWorker.Request(request));
    std::unique_ptr<SessionResult> cold=coldSink.waitFor(205);
    CHECK(cold && cold->status==SessionDataStatus::Unavailable && !cold->windows &&
          cold->sourceStampKnown && cold->sourceStamp==endpoint && cold->dataGeneration==0);
    CHECK(parses.load()==1);
    CHECK(coldWorker.Stop());
}

static void test_session_worker_rotation_uses_only_exact_attempted_path_cache(){
    SessionResultSink sink;
    std::atomic<int> resolves(0);
    std::atomic<unsigned long long> oldMtime(1);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ int call=++resolves; return call<=3?std::wstring(L"Session_old"):std::wstring(L"Session_new"); };
    ops.getStamp=[&](const std::wstring& path,SessionStamp& stamp){ stamp.size=50; stamp.mtime=path==L"Session_old"?oldMtime.load():3; return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("bytes",50,oldMtime.load()); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.activeTitle="old-cache"; output.push_back(fp); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=24; request.app="chrome"; request.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    request.purpose=SessionPurpose::AutoReconcile; request.identityGeneration=9;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(24);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh);
    const std::vector<WinFp>* cachedIdentity=fresh?fresh->windows.get():nullptr;
    uint64_t cachedGeneration=fresh?fresh->dataGeneration:0;
    oldMtime=2; request.requestId=25;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> rotated=sink.waitFor(25);
    SessionStamp current; current.size=50; current.mtime=3;
    SessionStamp data; data.size=50; data.mtime=1;
    CHECK(rotated && rotated->status==SessionDataStatus::CachedStale);
    CHECK(rotated && rotated->path==L"Session_new" && rotated->sourceStampKnown && rotated->sourceStamp==current);
    CHECK(rotated && rotated->windows.get()==cachedIdentity && rotated->dataStamp==data && rotated->dataGeneration==cachedGeneration);
    worker.Stop();
}

struct BlockingSessionParse {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false;
    bool released=false;
    bool parse(const AppProfile&,const std::string&,std::vector<WinFp>& output){
        std::unique_lock<std::mutex> lock(mutex);
        entered=true; changed.notify_all();
        changed.wait(lock,[&]{ return released; });
        output.clear();
        return true;
    }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
};

static SessionWorkerOps coalescingWorkerOps(SessionResultSink& sink,BlockingSessionParse& blocker){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-session"; };
    ops.getStamp=[](const std::wstring& path,SessionStamp& stamp){ stamp.size=path.size(); stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring& path){ return successfulSessionRead("valid",path.size(),1); };
    ops.parse=[&](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){ return blocker.parse(profile,bytes,output); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    return ops;
}

static void test_session_worker_ten_rapid_requests_are_active_plus_newest_pending(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.identityGeneration=10;
    SessionPurpose purposes[10]={
        SessionPurpose::MetadataProbe,SessionPurpose::Search,SessionPurpose::Search,
        SessionPurpose::AutoReconcile,SessionPurpose::HeartbeatSave,SessionPurpose::ManualSave,
        SessionPurpose::ManualRestore,SessionPurpose::ManualSave,SessionPurpose::ManualRestore,
        SessionPurpose::ManualSave
    };
    request.requestId=100; request.purpose=purposes[0];
    CHECK(worker.Request(request)); CHECK(blocker.waitEntered());
    for(int i=1;i<10;++i){ request.requestId=100+i; request.purpose=purposes[i]; CHECK(worker.Request(request)); }
    CHECK(worker.OutstandingForApp("firefox")<=2);
    blocker.release();
    int fresh=0,superseded=0;
    for(int i=0;i<10;++i){
        std::unique_ptr<SessionResult> result=sink.waitFor(100+i);
        CHECK(result && result->purpose==purposes[i]);
        if(result && result->status==SessionDataStatus::Fresh) ++fresh;
        if(result && result->status==SessionDataStatus::Superseded) ++superseded;
    }
    CHECK(fresh==2 && superseded==8);
    CHECK(!sink.wrongMessage.load());
    worker.Stop();
}

static void test_session_worker_low_probe_cannot_replace_user_pending(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest active; active.requestId=200; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::Search;
    CHECK(worker.Request(active)); CHECK(blocker.waitEntered());
    SessionRequest manual=active; manual.requestId=201; manual.purpose=SessionPurpose::ManualRestore;
    SessionRequest probe=active; probe.requestId=202; probe.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(manual)); CHECK(worker.Request(probe));
    std::unique_ptr<SessionResult> dropped=sink.waitFor(202);
    CHECK(dropped && dropped->status==SessionDataStatus::Superseded && dropped->purpose==SessionPurpose::MetadataProbe);
    blocker.release();
    std::unique_ptr<SessionResult> activeResult=sink.waitFor(200);
    std::unique_ptr<SessionResult> manualResult=sink.waitFor(201);
    CHECK(activeResult && activeResult->status==SessionDataStatus::Fresh);
    CHECK(manualResult && manualResult->status==SessionDataStatus::Fresh && manualResult->purpose==SessionPurpose::ManualRestore);
    worker.Stop();
}

struct OrderedBlockingSessionParse {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> order;
    bool released=false;
    bool parse(const AppProfile& profile,std::vector<WinFp>& output){
        std::unique_lock<std::mutex> lock(mutex);
        order.push_back(profile.id); changed.notify_all();
        if(order.size()==1) changed.wait(lock,[&]{ return released; });
        output.clear(); return true;
    }
    bool waitFirst(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return !order.empty(); });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
    std::vector<std::string> snapshot(){ std::lock_guard<std::mutex> lock(mutex); return order; }
};

static void test_session_worker_cross_app_manual_preempts_pending_metadata(){
    SessionResultSink sink; OrderedBlockingSessionParse parser;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-priority"; };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[&](const AppProfile& profile,const std::string&,std::vector<WinFp>& output){ return parser.parse(profile,output); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=210; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitFirst());
    SessionRequest probe; probe.requestId=211; probe.app="chrome"; probe.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    probe.purpose=SessionPurpose::MetadataProbe;
    SessionRequest manual; manual.requestId=212; manual.app="msedge"; manual.profile=sessionTestProfile("msedge",AppProfile::CHROMIUM);
    manual.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(probe)); CHECK(worker.Request(manual));
    parser.release();
    CHECK(sink.waitFor(210)!=nullptr); CHECK(sink.waitFor(211)!=nullptr); CHECK(sink.waitFor(212)!=nullptr);
    std::vector<std::string> order=parser.snapshot();
    CHECK(order.size()==3);
    CHECK(order.size()==3 && order[0]=="firefox" && order[1]=="msedge" && order[2]=="chrome");
    worker.Stop();
}

static void test_session_worker_rejects_unsupported_app_queue_amplification(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest active; active.requestId=300; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(blocker.waitEntered());
    for(int i=0;i<100;++i){
        SessionRequest unsupported; unsupported.requestId=301+i; unsupported.app="other"+std::to_string(i);
        unsupported.profile=sessionTestProfile(unsupported.app); unsupported.purpose=SessionPurpose::MetadataProbe;
        CHECK(!worker.Request(unsupported));
    }
    CHECK(worker.PendingCount()<=3 && worker.ActiveCount()==1);
    blocker.release();
    CHECK(sink.waitFor(300)!=nullptr);
    worker.Stop();
}

static std::unique_ptr<SessionResult> coordinatorResult(uint64_t id,const std::string& app,
        SessionPurpose purpose,uint64_t generation,SessionDataStatus status){
    std::unique_ptr<SessionResult> result(new SessionResult());
    result->requestId=id; result->app=app; result->purpose=purpose;
    result->identityGeneration=generation; result->status=status;
    return result;
}

static void test_session_coordinator_preserves_purpose_and_shared_payload_identity(){
    SessionRequest captured;
    SessionCoordinator coordinator([&](const SessionRequest& request){ captured=request; return true; });
    AppProfile profile=sessionTestProfile("firefox");
    uint64_t requestId=coordinator.RequestSessionData(profile,41,SessionPurpose::ManualSave);
    CHECK(requestId!=0 && captured.requestId==requestId && captured.app=="firefox");
    CHECK(captured.purpose==SessionPurpose::ManualSave && captured.identityGeneration==41);
    CHECK(SessionProfilesEqual(captured.profile,profile));
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    const std::vector<WinFp>* identity=payload.get();
    std::unique_ptr<SessionResult> result=coordinatorResult(requestId,"firefox",SessionPurpose::ManualSave,41,SessionDataStatus::Fresh);
    result->windows=payload;
    CHECK(coordinator.AcceptSessionResult(std::move(result),profile,41));
    const SessionResult* accepted=coordinator.Latest("firefox");
    CHECK(accepted && accepted->purpose==SessionPurpose::ManualSave && accepted->windows.get()==identity);
}

static void test_session_coordinator_rejects_old_generation_profile_purpose_and_request(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    uint64_t oldId=coordinator.RequestSessionData(profile,5,SessionPurpose::Search);
    uint64_t latestId=coordinator.RequestSessionData(profile,5,SessionPurpose::Search);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(oldId,"chrome",SessionPurpose::Search,5,SessionDataStatus::Fresh),profile,5));
    CHECK(coordinator.AcceptSessionResult(coordinatorResult(latestId,"chrome",SessionPurpose::Search,5,SessionDataStatus::Fresh),profile,5));

    uint64_t generationId=coordinator.RequestSessionData(profile,6,SessionPurpose::AutoReconcile);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(generationId,"chrome",SessionPurpose::AutoReconcile,6,SessionDataStatus::Fresh),profile,7));
    uint64_t purposeId=coordinator.RequestSessionData(profile,7,SessionPurpose::HeartbeatSave);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(purposeId,"chrome",SessionPurpose::ManualSave,7,SessionDataStatus::Fresh),profile,7));
    uint64_t profileId=coordinator.RequestSessionData(profile,7,SessionPurpose::ManualRestore);
    AppProfile changed=profile; changed.userDataDir=L"changed";
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(profileId,"chrome",SessionPurpose::ManualRestore,7,SessionDataStatus::Fresh),changed,7));
}

static void test_session_profile_comparison_covers_every_config_field(){
    AppProfile original=sessionTestProfile("firefox");
    CHECK(SessionProfilesEqual(original,original));
    AppProfile changed=original; changed.id="chrome"; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.classNames.push_back(L"OtherClass"); CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.exeName=L"other.exe"; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.titleSuffixes.push_back(L" other"); CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.session=AppProfile::CHROMIUM; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.userDataDir=L"other-data"; CHECK(!SessionProfilesEqual(original,changed));
}

static void test_session_coordinator_superseded_only_releases_bookkeeping(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("msedge",AppProfile::CHROMIUM);
    uint64_t id=coordinator.RequestSessionData(profile,9,SessionPurpose::ManualRestore);
    CHECK(coordinator.PendingCount()==1);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(id,"msedge",SessionPurpose::ManualRestore,9,SessionDataStatus::Superseded),profile,9));
    CHECK(coordinator.PendingCount()==0 && coordinator.Latest("msedge")==nullptr);
}

static void test_session_coordinator_request_faults_are_transactional(){
    const SessionCoordinatorStep steps[]={
        SessionCoordinatorStep::RequestPrepare,
        SessionCoordinatorStep::PendingInsert,
        SessionCoordinatorStep::LatestRequestInsert
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        bool armed=false;
        size_t submits=0;
        SessionCoordinatorOps ops;
        ops.beforeStep=[&](SessionCoordinatorStep step){
            if(armed && step==steps[index]){
                if(index==2) throw std::runtime_error("coordinator runtime fault");
                throw std::bad_alloc();
            }
        };
        SessionCoordinator coordinator([&](const SessionRequest&){ ++submits; return true; },ops);
        AppProfile profile=sessionTestProfile("firefox");
        uint64_t first=coordinator.RequestSessionData(profile,30,SessionPurpose::Search);
        CHECK(first==1 && submits==1);
        std::unique_ptr<SessionResult> accepted=coordinatorResult(
            first,"firefox",SessionPurpose::Search,30,SessionDataStatus::Fresh);
        accepted->path=L"sentinel";
        CHECK(coordinator.AcceptSessionResult(std::move(accepted),profile,30));
        const SessionResult* prior=coordinator.Latest("firefox");
        CHECK(prior && prior->path==L"sentinel" && coordinator.PendingCount()==0);

        armed=true;
        bool threw=false;
        uint64_t failed=99;
        try { failed=coordinator.RequestSessionData(profile,31,SessionPurpose::ManualSave); }
        catch(...) { threw=true; }
        armed=false;
        CHECK(!threw && failed==0 && submits==1 && coordinator.PendingCount()==0);
        CHECK(coordinator.Latest("firefox")==prior && coordinator.Latest("firefox")->path==L"sentinel");
        uint64_t next=coordinator.RequestSessionData(profile,31,SessionPurpose::ManualSave);
        CHECK(next==2 && submits==2 && coordinator.PendingCount()==1);
    }

    size_t submits=0;
    bool reject=true,throwSubmit=false;
    SessionCoordinator coordinator([&](const SessionRequest&){
        ++submits;
        if(throwSubmit) throw std::length_error("submit fault");
        return !reject;
    });
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    CHECK(coordinator.RequestSessionData(profile,40,SessionPurpose::Search)==0);
    CHECK(coordinator.PendingCount()==0 && coordinator.Latest("chrome")==nullptr);
    reject=false; throwSubmit=true;
    bool threw=false; uint64_t failed=99;
    try { failed=coordinator.RequestSessionData(profile,40,SessionPurpose::Search); }
    catch(...) { threw=true; }
    CHECK(!threw && failed==0 && coordinator.PendingCount()==0);
    throwSubmit=false;
    CHECK(coordinator.RequestSessionData(profile,40,SessionPurpose::Search)==1);
    CHECK(submits==3 && coordinator.PendingCount()==1);
}

static void test_session_coordinator_accept_faults_preserve_pending_and_latest(){
    const SessionCoordinatorStep steps[]={
        SessionCoordinatorStep::AcceptPrepare,
        SessionCoordinatorStep::LatestResultInsert
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        bool armed=false;
        SessionCoordinatorOps ops;
        ops.beforeStep=[&](SessionCoordinatorStep step){
            if(armed && step==steps[index]){
                if(index) throw std::length_error("coordinator result fault");
                throw std::bad_alloc();
            }
        };
        SessionCoordinator coordinator([](const SessionRequest&){ return true; },ops);
        AppProfile firefox=sessionTestProfile("firefox");
        uint64_t oldId=coordinator.RequestSessionData(firefox,50,SessionPurpose::Search);
        std::unique_ptr<SessionResult> oldResult=coordinatorResult(
            oldId,"firefox",SessionPurpose::Search,50,SessionDataStatus::Fresh);
        oldResult->path=L"old";
        CHECK(coordinator.AcceptSessionResult(std::move(oldResult),firefox,50));
        const SessionResult* oldIdentity=coordinator.Latest("firefox");

        AppProfile chrome=sessionTestProfile("chrome",AppProfile::CHROMIUM);
        uint64_t id=coordinator.RequestSessionData(chrome,51,SessionPurpose::ManualRestore);
        CHECK(id!=0 && coordinator.PendingCount()==1);
        armed=true;
        bool threw=false,accepted=true;
        try {
            accepted=coordinator.AcceptSessionResult(
                coordinatorResult(id,"chrome",SessionPurpose::ManualRestore,51,SessionDataStatus::Fresh),chrome,51);
        } catch(...) { threw=true; }
        armed=false;
        CHECK(!threw && !accepted && coordinator.PendingCount()==1);
        CHECK(coordinator.Latest("firefox")==oldIdentity && coordinator.Latest("chrome")==nullptr);
        CHECK(coordinator.AcceptSessionResult(
            coordinatorResult(id,"chrome",SessionPurpose::ManualRestore,51,SessionDataStatus::Fresh),chrome,51));
        CHECK(coordinator.PendingCount()==0 && coordinator.Latest("chrome")!=nullptr);
    }
}

static void test_posted_session_result_is_owned_immediately_on_rejection(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("firefox");
    uint64_t id=coordinator.RequestSessionData(profile,12,SessionPurpose::Search);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result=coordinatorResult(id,"firefox",SessionPurpose::Search,11,SessionDataStatus::Fresh);
    result->windows=payload;
    SessionResult* raw=result.release();
    CHECK(payload.use_count()==2);
    CHECK(!AcceptPostedSessionResult(coordinator,(LPARAM)raw,profile,12));
    CHECK(payload.use_count()==1);
}

static void test_unavailable_defer_is_once_per_current_source_and_preserves_bytes(){
    SessionUnavailableDeferBudget budget;
    SessionResult unavailable;
    unavailable.app="firefox"; unavailable.path=L"one"; unavailable.status=SessionDataStatus::Unavailable;
    unavailable.sourceStampKnown=true; unavailable.sourceStamp.size=10; unavailable.sourceStamp.mtime=1;
    CHECK(budget.ShouldDefer(unavailable)); CHECK(!budget.ShouldDefer(unavailable));
    unavailable.sourceStamp.mtime=2;
    CHECK(budget.ShouldDefer(unavailable)); CHECK(!budget.ShouldDefer(unavailable));
    unavailable.status=SessionDataStatus::Superseded;
    CHECK(!budget.ShouldDefer(unavailable));
    const std::string original="serialized-layout-bytes";
    const std::string proposed="mutated";
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Unavailable,original,proposed)==original);
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Superseded,original,proposed)==original);
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Fresh,original,proposed)==proposed);
}

static void test_session_data_generation_is_per_app_and_hash_breaks_stamp_ties(){
    SessionResultSink sink;
    std::mutex contentMutex;
    std::string firefoxTitle="A";
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-path"; };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=7; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring& path){ return successfulSessionRead(W2U8(path),7,1); };
    ops.parse=[&](const AppProfile& profile,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp fp;
        { std::lock_guard<std::mutex> lock(contentMutex); fp.activeTitle=profile.id=="firefox"?firefoxTitle:"C"; }
        output.push_back(fp); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,1,1024*1024);
    SessionRequest firefox; firefox.requestId=400; firefox.app="firefox"; firefox.profile=sessionTestProfile("firefox"); firefox.purpose=SessionPurpose::MetadataProbe;
    SessionRequest chrome; chrome.requestId=401; chrome.app="chrome"; chrome.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM); chrome.purpose=SessionPurpose::AutoReconcile;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffA=sink.waitFor(400);
    CHECK(ffA && ffA->dataGeneration==1); ffA.reset();
    CHECK(worker.Request(chrome)); std::unique_ptr<SessionResult> cr=sink.waitFor(401);
    CHECK(cr && cr->dataGeneration==1); cr.reset();

    firefox.requestId=402;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffSame=sink.waitFor(402);
    CHECK(ffSame && ffSame->dataGeneration==1); ffSame.reset();
    chrome.requestId=403;
    CHECK(worker.Request(chrome)); cr=sink.waitFor(403); CHECK(cr && cr->dataGeneration==1); cr.reset();

    { std::lock_guard<std::mutex> lock(contentMutex); firefoxTitle="B"; }
    firefox.requestId=404;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffChanged=sink.waitFor(404);
    CHECK(ffChanged && ffChanged->dataGeneration==2 && ffChanged->windows->at(0).activeTitle=="B");
    SessionStamp stableSource; stableSource.size=7; stableSource.mtime=1;
    CHECK(ffChanged && ffChanged->sourceStampKnown && ffChanged->sourceStamp==stableSource && ffChanged->dataStamp==stableSource);
    const std::vector<WinFp>* changedIdentity=ffChanged?ffChanged->windows.get():nullptr;
    firefox.requestId=405;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffHit=sink.waitFor(405);
    CHECK(ffHit && ffHit->dataGeneration==2 && ffHit->windows.get()==changedIdentity);
    int refreshWaves=0;
    uint64_t acceptedGeneration=1;
    if(ffChanged && ffChanged->dataGeneration!=acceptedGeneration){ ++refreshWaves; acceptedGeneration=ffChanged->dataGeneration; }
    if(ffHit && ffHit->dataGeneration!=acceptedGeneration) ++refreshWaves;
    CHECK(refreshWaves==1);
    worker.Stop();
}

static void test_session_data_generation_is_monotonic_when_historical_cache_returns(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<bool> parseOk(true);
    std::atomic<int> reads(0),parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"generation-history"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){
        ++reads; unsigned long long current=mtime.load();
        return successfulSessionRead("S"+std::to_string(current),1,current);
    };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){
        ++parses; output.clear();
        if(!parseOk.load()) return false;
        WinFp window; window.activeTitle=bytes; output.push_back(std::move(window)); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,4,1024*1024);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");

    request.requestId=610; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> s1=sink.waitFor(610);
    CHECK(s1 && s1->status==SessionDataStatus::Fresh && s1->dataGeneration==1);
    const std::vector<WinFp>* s1Identity=s1?s1->windows.get():nullptr;

    mtime=2; request.requestId=611; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> s2=sink.waitFor(611);
    CHECK(s2 && s2->status==SessionDataStatus::Fresh && s2->dataGeneration==2);
    const std::vector<WinFp>* s2Identity=s2?s2->windows.get():nullptr;

    mtime=1; request.requestId=612; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> historicalFresh=sink.waitFor(612);
    CHECK(historicalFresh && historicalFresh->status==SessionDataStatus::Fresh &&
          historicalFresh->windows.get()==s1Identity && historicalFresh->dataGeneration==3);
    request.requestId=613; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> repeatedFresh=sink.waitFor(613);
    CHECK(repeatedFresh && repeatedFresh->windows.get()==s1Identity && repeatedFresh->dataGeneration==3);
    CHECK(reads.load()==2 && parses.load()==2);

    mtime=3; parseOk=false; request.requestId=614; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> historicalStale=sink.waitFor(614);
    SessionStamp s2Stamp; s2Stamp.size=1; s2Stamp.mtime=2;
    CHECK(historicalStale && historicalStale->status==SessionDataStatus::CachedStale &&
          historicalStale->windows.get()==s2Identity && historicalStale->dataStamp==s2Stamp &&
          historicalStale->dataGeneration==4);
    request.requestId=615; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> repeatedStale=sink.waitFor(615);
    CHECK(repeatedStale && repeatedStale->status==SessionDataStatus::CachedStale &&
          repeatedStale->windows.get()==s2Identity && repeatedStale->dataGeneration==4);
    CHECK(reads.load()==4 && parses.load()==4);
    worker.Stop();
}

static void test_session_data_generation_saturates_without_zero_or_rollback(){
    CHECK(NextSessionDataGeneration(0)==1);
    CHECK(NextSessionDataGeneration((std::numeric_limits<uint64_t>::max)()-1)==
          (std::numeric_limits<uint64_t>::max)());
    CHECK(NextSessionDataGeneration((std::numeric_limits<uint64_t>::max)())==
          (std::numeric_limits<uint64_t>::max)());
}

static void test_session_cache_enforces_sixteen_entry_lru_cap(){
    SessionCache cache(16,1024*1024);
    for(int i=0;i<17;++i){
        std::vector<WinFp> windows(1); windows[0].activeTitle=std::to_string(i);
        SessionStamp stamp; stamp.size=1; stamp.mtime=(unsigned long long)i;
        SessionCacheValue stored;
        CHECK(cache.Put("firefox",L"path"+std::to_wstring(i),stamp,std::move(windows),(uint64_t)i,(uint64_t)i+1,stored));
        CHECK(cache.EntryCount()<=16);
        stored.windows.reset();
    }
    SessionCacheValue value;
    SessionStamp first; first.size=1; first.mtime=0;
    SessionStamp last; last.size=1; last.mtime=16;
    CHECK(!cache.FindExact("firefox",L"path0",first,value));
    CHECK(cache.FindExact("firefox",L"path16",last,value));
    CHECK(cache.EntryCount()==16);
}

static void test_session_cache_byte_cap_counts_external_ui_payload(){
    std::vector<WinFp> sample(1); sample[0].tabsBlob=std::string(200,'x');
    size_t bytes=EstimateSessionPayloadBytes(sample);
    SessionStamp stamp; stamp.size=1; stamp.mtime=1;
    SessionCache tooSmall(16,bytes-1);
    SessionCacheValue rejected;
    CHECK(!tooSmall.Put("firefox",L"oversized",stamp,std::move(sample),1,1,rejected));
    CHECK(tooSmall.EntryCount()==0 && tooSmall.RetainedBytes()==0 && !rejected.windows);

    std::vector<WinFp> first(1); first[0].tabsBlob=std::string(200,'a');
    std::vector<WinFp> second(1); second[0].tabsBlob=std::string(200,'b');
    CHECK(EstimateSessionPayloadBytes(first)==bytes && EstimateSessionPayloadBytes(second)==bytes);
    SessionCache exact(16,bytes);
    SessionCacheValue uiOwned;
    CHECK(exact.Put("firefox",L"one",stamp,std::move(first),1,1,uiOwned));
    CHECK(exact.RetainedBytes()==bytes);
    SessionCacheValue cannotFit;
    CHECK(!exact.Put("chrome",L"two",stamp,std::move(second),2,1,cannotFit));
    CHECK(exact.RetainedBytes()==bytes && !cannotFit.windows);
    uiOwned.windows.reset();
    CHECK(exact.RetainedBytes()==bytes); // cache still owns the preserved LRU entry
    CHECK(exact.Put("chrome",L"two",stamp,std::move(second),2,1,cannotFit));
    CHECK(exact.RetainedBytes()==bytes);
}

static void test_post_message_failure_deletes_heap_result(){
    SessionWorkerOps ops;
    std::atomic<int> posts(0);
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){ ++posts; return false; };
    FillMissingSessionWorkerOps(ops);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result(new SessionResult()); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(!PostSessionResultOwned(ops,(HWND)1,std::move(result)));
    CHECK(posts.load()==1 && payload.use_count()==1);
}

static void test_session_worker_oversized_payload_is_unavailable(){
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"large"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("valid",5,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.tabsBlob=std::string(4096,'x'); output.push_back(std::move(fp)); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,128);
    SessionRequest request; request.requestId=410; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(410);
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(worker.CacheEntryCount()==0 && worker.RetainedBytes()==0);
    worker.Stop();
}

static void test_session_worker_stop_joins_and_suppresses_unposted_completion(){
    SessionResultSink sink; BlockingSessionParse blocker;
    std::atomic<int> posts(0);
    SessionWorkerOps ops=coalescingWorkerOps(sink,blocker);
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){ ++posts; return false; };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=420; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::ManualSave;
    CHECK(worker.Request(request)); CHECK(blocker.waitEntered());
    std::atomic<bool> stopped(false);
    std::thread stopper([&]{ worker.Stop(); stopped=true; });
    bool rejecting=false;
    for(int i=0;i<10000 && !rejecting;++i){ SessionRequest late=request; late.requestId=421; rejecting=!worker.Request(late); if(!rejecting) std::this_thread::yield(); }
    CHECK(rejecting); CHECK(!stopped.load());
    int postsWhenStopping=posts.load();
    blocker.release();
    stopper.join();
    CHECK(stopped.load() && posts.load()==postsWhenStopping);
    CHECK(worker.PendingCount()==0 && worker.ActiveCount()==0 && worker.RetainedBytes()==0);
    CHECK(!worker.Request(request));
}

struct BlockingSupersededPoster {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    bool post(HWND,UINT,WPARAM,LPARAM value){
        std::unique_ptr<SessionResult> result((SessionResult*)value);
        if(result && result->requestId==451 && result->status==SessionDataStatus::Superseded){
            std::unique_lock<std::mutex> lock(mutex);
            entered=true; changed.notify_all();
            changed.wait(lock,[&]{ return released; });
        }
        return true;
    }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
};

static void test_session_worker_stop_waits_for_inflight_superseded_post(){
    SessionResultSink unusedSink; BlockingSessionParse parser; BlockingSupersededPoster poster;
    SessionWorkerOps ops=coalescingWorkerOps(unusedSink,parser);
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return poster.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=450; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest queued=active; queued.requestId=451; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=452; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(queued));
    std::atomic<bool> replacementReturned(false),replacementAccepted(false);
    std::thread replacer([&]{ replacementAccepted=worker.Request(newest); replacementReturned=true; });
    CHECK(poster.waitEntered());
    std::mutex stopMutex; std::condition_variable stopChanged; bool stopped=false;
    std::thread stopper([&]{ worker.Stop(); { std::lock_guard<std::mutex> lock(stopMutex); stopped=true; } stopChanged.notify_all(); });
    for(int i=0;i<10000 && worker.PendingCount()!=0;++i) std::this_thread::yield();
    CHECK(worker.PendingCount()==0);
    SessionRequest late=active; late.requestId=453;
    CHECK(!worker.Request(late));
    parser.release();
    bool returnedWhilePostBlocked=false;
    {
        std::unique_lock<std::mutex> lock(stopMutex);
        returnedWhilePostBlocked=stopChanged.wait_for(lock,std::chrono::milliseconds(500),[&]{ return stopped; });
    }
    CHECK(!returnedWhilePostBlocked);
    poster.release(); replacer.join(); stopper.join();
    CHECK(replacementAccepted.load() && replacementReturned.load() && stopped);
}

struct ReentrantStopState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,returned=false;
    SessionWorker* worker=nullptr;
    std::shared_ptr<BlockingSessionParse> parser;
    void markEntered(){ std::lock_guard<std::mutex> lock(mutex); entered=true; changed.notify_all(); }
    void markReturned(){ std::lock_guard<std::mutex> lock(mutex); returned=true; changed.notify_all(); }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    bool waitReturned(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::milliseconds(500),[&]{ return returned; });
    }
};

static SessionWorkerOps reentrantStopWorkerOps(const std::shared_ptr<ReentrantStopState>& state,
                                               bool blockParser,bool stopOnlySuperseded){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"reentrant"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[state,blockParser](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){
        if(blockParser) return state->parser->parse(profile,bytes,output);
        output.clear(); return true;
    };
    ops.postMessage=[state,stopOnlySuperseded](HWND,UINT,WPARAM,LPARAM value){
        SessionResult* result=(SessionResult*)value;
        if(!stopOnlySuperseded || (result && result->status==SessionDataStatus::Superseded)){
            if(state->parser) state->parser->release();
            state->markEntered();
            state->worker->Stop();
            state->markReturned();
        }
        delete result; // ownership transfers only if the callback returns successfully
        return true;
    };
    return ops;
}

static void test_session_worker_reentrant_requester_poster_stop_completes(){
    std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
    state->parser.reset(new BlockingSessionParse());
    SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,true,true));
    state->worker=worker;
    SessionRequest active; active.requestId=460; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker->Request(active)); CHECK(state->parser->waitEntered());
    SessionRequest queued=active; queued.requestId=461; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=462; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(queued));
    std::thread replacer([worker,newest]{ worker->Request(newest); });
    CHECK(state->waitEntered());
    bool completed=state->waitReturned();
    CHECK(completed);
    if(completed){ replacer.join(); worker->Stop(); delete worker; }
    else replacer.detach(); // bounded RED: the isolated heap state lives until process exit
}

static void test_session_worker_reentrant_worker_poster_stop_defers_self_join(){
    std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
    SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,false,false));
    state->worker=worker;
    SessionRequest request; request.requestId=470; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(request)); CHECK(state->waitEntered());
    bool completed=state->waitReturned();
    CHECK(completed);
    if(completed){ worker->Stop(); delete worker; }
    // On RED the worker has exited after the caught self-join exception; keep it isolated.
}

struct DeterministicPostStopInterleave {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id requesterThread;
    bool callbackEntered=false;
    bool workerPostEntered=false;
    bool releaseWorkerPost=false;
    bool allowCallbackStop=false;
    bool callbackStopEntered=false;
    bool callbackJoinWaitEntered=false;
    bool releaseCallbackJoinWait=false;
    bool callbackReturned=false;
    bool callbackStopResult=true;
    bool waitForExternalStop=false;
    SessionWorker* worker=nullptr;
    std::shared_ptr<BlockingSessionParse> parser;

    void setRequesterThread(){
        std::lock_guard<std::mutex> lock(mutex);
        requesterThread=std::this_thread::get_id();
    }
    bool beforePost(){
        std::unique_lock<std::mutex> lock(mutex);
        if(callbackEntered && std::this_thread::get_id()!=requesterThread){
            workerPostEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return releaseWorkerPost; });
            return false; // deterministic RED cleanup: never wait on the held legacy fence
        }
        return true;
    }
    bool beforeJoinWait(){
        std::unique_lock<std::mutex> lock(mutex);
        if(waitForExternalStop && std::this_thread::get_id()==requesterThread){
            callbackJoinWaitEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return releaseCallbackJoinWait; });
            return false;
        }
        return true;
    }
    bool post(HWND,UINT,WPARAM,LPARAM value){
        std::unique_ptr<SessionResult> result((SessionResult*)value);
        if(result && result->requestId==481 && result->status==SessionDataStatus::Superseded){
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbackEntered=true;
                changed.notify_all();
            }
            parser->release();
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock,std::chrono::seconds(5),[&]{ return workerPostEntered; });
                if(waitForExternalStop)
                    changed.wait_for(lock,std::chrono::seconds(5),[&]{ return allowCallbackStop; });
                callbackStopEntered=true;
                changed.notify_all();
            }
            bool stopResult=worker->Stop();
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbackStopResult=stopResult;
                callbackReturned=true;
                changed.notify_all();
            }
        }
        return true;
    }
    bool waitFlag(bool DeterministicPostStopInterleave::*member,int milliseconds=5000){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{ return this->*member; });
    }
    void allowStop(){ std::lock_guard<std::mutex> lock(mutex); allowCallbackStop=true; changed.notify_all(); }
    void releasePost(){ std::lock_guard<std::mutex> lock(mutex); releaseWorkerPost=true; changed.notify_all(); }
    void releaseJoinWait(){ std::lock_guard<std::mutex> lock(mutex); releaseCallbackJoinWait=true; changed.notify_all(); }
};

static SessionWorkerOps deterministicPostStopOps(
        const std::shared_ptr<DeterministicPostStopInterleave>& state){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"deterministic-stop"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[state](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){
        return state->parser->parse(profile,bytes,output);
    };
    ops.beforePost=[state]{ return state->beforePost(); };
    ops.beforeJoinWait=[state]{ return state->beforeJoinWait(); };
    ops.postMessage=[state](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return state->post(hwnd,message,wp,lp); };
    return ops;
}

static SessionWorker* startDeterministicPostStop(
        const std::shared_ptr<DeterministicPostStopInterleave>& state,std::thread& requester){
    SessionWorker* worker=new SessionWorker((HWND)1,deterministicPostStopOps(state));
    state->worker=worker;
    SessionRequest active; active.requestId=480; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker->Request(active)); CHECK(state->parser->waitEntered());
    SessionRequest queued=active; queued.requestId=481; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=482; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(queued));
    requester=std::thread([state,worker,newest]{ state->setRequesterThread(); worker->Request(newest); });
    return worker;
}

static void test_session_worker_reentrant_stop_waits_for_confirmed_worker_post_path(){
    std::shared_ptr<DeterministicPostStopInterleave> state(new DeterministicPostStopInterleave());
    state->parser.reset(new BlockingSessionParse());
    std::thread requester;
    SessionWorker* worker=startDeterministicPostStop(state,requester);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::workerPostEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackStopEntered));
    bool returned=state->waitFlag(&DeterministicPostStopInterleave::callbackReturned,750);
    CHECK(returned);
    state->releasePost();
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackReturned));
    CHECK(!state->callbackStopResult);
    requester.join();
    CHECK(worker->Stop());
    delete worker;
}

static void test_session_worker_concurrent_external_and_reentrant_stop_do_not_cycle(){
    std::shared_ptr<DeterministicPostStopInterleave> state(new DeterministicPostStopInterleave());
    state->parser.reset(new BlockingSessionParse());
    state->waitForExternalStop=true;
    std::thread requester;
    SessionWorker* worker=startDeterministicPostStop(state,requester);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::workerPostEntered));
    std::mutex stoppedMutex; std::condition_variable stoppedChanged; bool externalReturned=false;
    std::thread external([&]{ worker->Stop(); { std::lock_guard<std::mutex> lock(stoppedMutex); externalReturned=true; } stoppedChanged.notify_all(); });
    for(int i=0;i<10000 && worker->PendingCount()!=0;++i) std::this_thread::yield();
    CHECK(worker->PendingCount()==0); // external Stop has closed the gate and cleared newest pending
    state->allowStop();
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackStopEntered));
    bool callbackReturned=state->waitFlag(&DeterministicPostStopInterleave::callbackReturned,750);
    bool stopReturnedBeforeRelease=false;
    {
        std::unique_lock<std::mutex> lock(stoppedMutex);
        stopReturnedBeforeRelease=stoppedChanged.wait_for(lock,std::chrono::milliseconds(100),[&]{ return externalReturned; });
    }
    CHECK(callbackReturned); CHECK(!stopReturnedBeforeRelease);
    state->releasePost();
    state->releaseJoinWait();
    bool stopReturned=false;
    {
        std::unique_lock<std::mutex> lock(stoppedMutex);
        stopReturned=stoppedChanged.wait_for(lock,std::chrono::seconds(5),[&]{ return externalReturned; });
    }
    CHECK(stopReturned);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackReturned));
    CHECK(!state->callbackStopResult);
    requester.join(); external.join(); delete worker;
}

static void test_session_worker_reentrant_worker_stop_survives_repeated_destruction(){
    for(int attempt=0;attempt<16;++attempt){
        std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
        SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,false,false));
        state->worker=worker;
        SessionRequest request; request.requestId=490+(uint64_t)attempt; request.app="firefox";
        request.profile=sessionTestProfile("firefox"); request.purpose=SessionPurpose::ManualRestore;
        CHECK(worker->Request(request)); CHECK(state->waitEntered()); CHECK(state->waitReturned());
        delete worker;
    }
}

static void test_worker_retained_budget_includes_posted_ui_ownership(){
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"owned"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("valid",5,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); output.push_back(WinFp{}); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=430; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> accepted=sink.waitFor(430);
    CHECK(accepted && accepted->windows && worker.RetainedBytes()>0);
    worker.Stop();
    CHECK(worker.RetainedBytes()>0);
    accepted.reset();
    CHECK(worker.RetainedBytes()==0);
}

static void test_failed_cache_replacement_preserves_exact_stale_payload(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<char> title('A');
    std::vector<WinFp> sizing(1); sizing[0].activeTitle=std::string(200,'A');
    size_t onePayload=EstimateSessionPayloadBytes(sizing);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"budgeted"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("valid",5,mtime.load()); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.activeTitle=std::string(200,title.load()); output.push_back(std::move(fp)); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,onePayload);
    SessionRequest request; request.requestId=440; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> oldUi=sink.waitFor(440);
    CHECK(oldUi && oldUi->status==SessionDataStatus::Fresh);
    const std::vector<WinFp>* oldIdentity=oldUi?oldUi->windows.get():nullptr;
    mtime=2; title='B'; request.requestId=441;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(441);
    SessionStamp data; data.size=5; data.mtime=1;
    SessionStamp source; source.size=5; source.mtime=2;
    CHECK(stale && stale->status==SessionDataStatus::CachedStale && stale->windows.get()==oldIdentity);
    CHECK(stale && stale->dataStamp==data && stale->sourceStampKnown && stale->sourceStamp==source);
    CHECK(worker.CacheEntryCount()==1 && worker.RetainedBytes()==onePayload);
    worker.Stop();
}

static void test_session_cache_put_is_strongly_transactional_at_every_fault_step(){
    const SessionCachePutStep steps[]={
        SessionCachePutStep::PayloadStorage,
        SessionCachePutStep::PayloadControl,
        SessionCachePutStep::KeyCopy,
        SessionCachePutStep::PathCopy,
        SessionCachePutStep::StampCopy,
        SessionCachePutStep::OutputCopy,
        SessionCachePutStep::ContainerReserve,
        SessionCachePutStep::DuplicateReplacement,
        SessionCachePutStep::ContainerInsert,
        SessionCachePutStep::OrderPublish
    };
    for(size_t stepIndex=0;stepIndex<sizeof(steps)/sizeof(steps[0]);++stepIndex){
        bool enabled=false;
        SessionCacheOps ops;
        ops.beforePutStep=[&](SessionCachePutStep step){
            if(enabled && step==steps[stepIndex]){
                if((stepIndex&1)==0) throw std::bad_alloc();
                throw std::length_error("session cache injected fault");
            }
        };
        SessionCache cache(2,1024*1024,ops);
        SessionStamp stampA; stampA.size=1; stampA.mtime=1;
        SessionStamp stampB; stampB.size=2; stampB.mtime=2;
        SessionStamp stampC; stampC.size=3; stampC.mtime=3;
        std::vector<WinFp> first(1); first[0].activeTitle="A";
        std::vector<WinFp> second(1); second[0].activeTitle="B";
        SessionCacheValue storedA,storedB;
        CHECK(cache.Put("firefox",L"A",stampA,std::move(first),11,1,storedA));
        CHECK(cache.Put("firefox",L"B",stampB,std::move(second),22,2,storedB));
        const std::vector<WinFp>* identityA=storedA.windows.get();
        const std::vector<WinFp>* identityB=storedB.windows.get();
        SessionCacheValue touch;
        CHECK(cache.FindExact("firefox",L"A",stampA,touch)); // B is the LRU victim
        touch.windows.reset();
        const size_t entriesBefore=cache.EntryCount(),bytesBefore=cache.RetainedBytes();

        const bool duplicate=steps[stepIndex]==SessionCachePutStep::DuplicateReplacement;
        std::vector<WinFp> candidate(1); candidate[0].activeTitle=duplicate?"A2":"C";
        SessionCacheValue failed;
        failed.path=L"sentinel-output";
        failed.stamp.size=91; failed.stamp.mtime=92;
        failed.contentHash=93; failed.dataGeneration=94; failed.retainedBytes=95;
        std::shared_ptr<std::vector<WinFp> > sentinelPayload(new std::vector<WinFp>(1));
        sentinelPayload->at(0).activeTitle="sentinel";
        failed.windows=sentinelPayload;
        const std::vector<WinFp>* sentinelIdentity=failed.windows.get();
        enabled=true;
        CHECK(!cache.Put("firefox",duplicate?L"A":L"C",duplicate?stampA:stampC,
                         std::move(candidate),33,3,failed));
        enabled=false;
        CHECK(failed.path==L"sentinel-output" && failed.stamp.size==91 && failed.stamp.mtime==92 &&
              failed.contentHash==93 && failed.dataGeneration==94 && failed.retainedBytes==95 &&
              failed.windows.get()==sentinelIdentity && failed.windows->at(0).activeTitle=="sentinel");
        CHECK(cache.EntryCount()==entriesBefore && cache.RetainedBytes()==bytesBefore);
        SessionCacheValue afterB,afterA;
        CHECK(cache.FindExact("firefox",L"B",stampB,afterB));
        CHECK(afterB.windows.get()==identityB && afterB.windows->at(0).activeTitle=="B");
        CHECK(cache.FindExact("firefox",L"A",stampA,afterA));
        CHECK(afterA.windows.get()==identityA && afterA.windows->at(0).activeTitle=="A");

        std::vector<WinFp> retry(1); retry[0].activeTitle=duplicate?"A2":"C";
        SessionCacheValue inserted;
        CHECK(cache.Put("firefox",duplicate?L"A":L"C",duplicate?stampA:stampC,
                        std::move(retry),33,3,inserted));
        CHECK(inserted.windows && inserted.windows.get()!=identityA && inserted.windows.get()!=identityB);
        if(!duplicate){
            SessionCacheValue evicted;
            CHECK(!cache.FindExact("firefox",L"B",stampB,evicted));
            CHECK(cache.FindExact("firefox",L"A",stampA,evicted) && evicted.windows.get()==identityA);
        }
    }
}

static SessionCacheValue sessionCacheOutputSentinel(){
    SessionCacheValue value;
    value.path=L"preserve-me"; value.stamp.size=71; value.stamp.mtime=72;
    value.contentHash=73; value.dataGeneration=74; value.retainedBytes=75;
    std::shared_ptr<std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    payload->at(0).activeTitle="preserve-me";
    value.windows=payload;
    return value;
}

static bool isSessionCacheOutputSentinel(const SessionCacheValue& value,
                                         const std::vector<WinFp>* identity){
    return value.path==L"preserve-me" && value.stamp.size==71 && value.stamp.mtime==72 &&
           value.contentHash==73 && value.dataGeneration==74 && value.retainedBytes==75 &&
           value.windows.get()==identity && value.windows && value.windows->at(0).activeTitle=="preserve-me";
}

static void test_session_cache_put_preserves_output_on_capacity_and_budget_rejection(){
    SessionStamp stamp; stamp.size=1; stamp.mtime=1;
    SessionCacheValue output=sessionCacheOutputSentinel();
    const std::vector<WinFp>* identity=output.windows.get();
    std::vector<WinFp> rejected(1); rejected[0].activeTitle="rejected";
    SessionCache noEntries(0,1024*1024);
    CHECK(!noEntries.Put("firefox",L"none",stamp,std::move(rejected),1,1,output));
    CHECK(isSessionCacheOutputSentinel(output,identity));
    CHECK(noEntries.EntryCount()==0 && noEntries.RetainedBytes()==0);

    output=sessionCacheOutputSentinel(); identity=output.windows.get();
    std::vector<WinFp> tooLarge(1); tooLarge[0].tabsBlob=std::string(512,'x');
    SessionCache noBytes(1,EstimateSessionPayloadBytes(tooLarge)-1);
    CHECK(!noBytes.Put("firefox",L"large",stamp,std::move(tooLarge),2,1,output));
    CHECK(isSessionCacheOutputSentinel(output,identity));
    CHECK(noBytes.EntryCount()==0 && noBytes.RetainedBytes()==0);

    std::vector<WinFp> first(1); first[0].tabsBlob=std::string(256,'a');
    std::vector<WinFp> second(1); second[0].tabsBlob=std::string(256,'b');
    size_t onePayload=EstimateSessionPayloadBytes(first);
    CHECK(EstimateSessionPayloadBytes(second)==onePayload);
    SessionCache exact(1,onePayload);
    SessionCacheValue uiHeld;
    CHECK(exact.Put("firefox",L"A",stamp,std::move(first),11,1,uiHeld));
    const std::vector<WinFp>* heldIdentity=uiHeld.windows.get();
    CHECK(!exact.Put("chrome",L"B",stamp,std::move(second),22,2,uiHeld));
    CHECK(uiHeld.path==L"A" && uiHeld.windows.get()==heldIdentity &&
          uiHeld.windows->at(0).tabsBlob==std::string(256,'a'));
    CHECK(exact.EntryCount()==1 && exact.RetainedBytes()==onePayload);
    SessionCacheValue stillA;
    CHECK(exact.FindExact("firefox",L"A",stamp,stillA) && stillA.windows.get()==heldIdentity);
}

static bool waitSessionWorkerIdle(SessionWorker& worker,const std::string& app){
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){
        if(worker.OutstandingForApp(app)==0 && worker.ActiveCount()==0) return true;
        std::this_thread::yield();
    }
    return worker.OutstandingForApp(app)==0 && worker.ActiveCount()==0;
}

static SessionWorkerOps exceptionWorkerOps(SessionResultSink& sink,
        const std::function<void(SessionWorkerStep)>& beforeStep=std::function<void(SessionWorkerStep)>()){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"exception-path"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=2; stamp.mtime=3; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",2,3); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp window; window.activeTitle="ok"; output.push_back(std::move(window)); return true;
    };
    ops.beforeWorkerStep=beforeStep;
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    return ops;
}

static void test_session_worker_contains_internal_allocation_faults_and_continues(){
    const SessionWorkerStep steps[]={
        SessionWorkerStep::AfterChoose,
        SessionWorkerStep::ActiveCopy,
        SessionWorkerStep::CacheLookup,
        SessionWorkerStep::GenerationPrepare,
        SessionWorkerStep::GenerationPublish
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        SessionResultSink sink;
        std::atomic<bool> armed(true);
        SessionWorkerOps ops=exceptionWorkerOps(sink,[&](SessionWorkerStep step){
            if(step==steps[index] && armed.exchange(false)){
                if(index==4) throw std::runtime_error("worker runtime fault");
                if((index&1)==0) throw std::bad_alloc();
                throw std::length_error("worker length fault");
            }
        });
        SessionWorker worker((HWND)1,ops);
        SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
        request.requestId=700+(uint64_t)index*2; request.purpose=SessionPurpose::ManualRestore;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> unavailable=sink.waitFor(request.requestId);
        CHECK(unavailable && unavailable->status==SessionDataStatus::Unavailable &&
              unavailable->requestId==request.requestId && unavailable->purpose==request.purpose && !unavailable->windows);
        CHECK(worker.ActiveCount()==0 && worker.OutstandingForApp("firefox")==0);
        request.requestId++;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> fresh=sink.waitFor(request.requestId);
        CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows &&
              fresh->windows->at(0).activeTitle=="ok");
        CHECK(worker.Stop());
    }
}

static void test_session_worker_result_allocation_failure_drops_only_that_request(){
    SessionResultSink sink;
    std::atomic<int> allocations(0);
    SessionWorkerOps ops=exceptionWorkerOps(sink);
    ops.makeResult=[&]()->std::unique_ptr<SessionResult>{
        int call=++allocations;
        if(call==1) throw std::bad_alloc();
        if(call==2) throw std::length_error("result allocation fault");
        return std::unique_ptr<SessionResult>(new SessionResult());
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=720; CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    request.requestId=721; CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        CHECK(sink.results.empty());
    }
    request.requestId=722; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(722);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && allocations.load()==3);
    CHECK(worker.ActiveCount()==0 && worker.Stop());
}

static void test_session_worker_result_prepare_fault_drops_unidentified_result_and_continues(){
    SessionResultSink sink;
    std::atomic<bool> armed(true);
    SessionWorkerOps ops=exceptionWorkerOps(sink,[&](SessionWorkerStep step){
        if(step==SessionWorkerStep::ResultPrepare && armed.exchange(false)) throw std::bad_alloc();
    });
    ops.makeResult=[](){
        std::unique_ptr<SessionResult> result(new SessionResult());
        result->requestId=9999; result->app="sentinel";
        return result;
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=725;
    CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        CHECK(sink.results.empty());
    }
    request.requestId=726;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(726);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh);
    CHECK(worker.Stop());
}

static void test_session_worker_superseded_result_factory_is_not_called_under_state_lock(){
    SessionResultSink sink; BlockingSessionParse parser;
    const std::thread::id requesterThread=std::this_thread::get_id();
    std::atomic<bool> requesterFactoryCall(false);
    SessionWorkerOps ops=coalescingWorkerOps(sink,parser);
    ops.makeResult=[&](){
        if(std::this_thread::get_id()==requesterThread){
            requesterFactoryCall=true;
            throw std::runtime_error("requester result factory callback");
        }
        return std::unique_ptr<SessionResult>(new SessionResult());
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=750; active.app="firefox";
    active.profile=sessionTestProfile("firefox"); active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest original=active; original.requestId=751; original.purpose=SessionPurpose::ManualSave;
    SessionRequest replacement=original; replacement.requestId=752; replacement.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(original));
    bool accepted=worker.Request(replacement);
    CHECK(accepted && !requesterFactoryCall.load());
    parser.release();
    CHECK(sink.waitFor(750)!=nullptr);
    if(accepted){
        std::unique_ptr<SessionResult> superseded=sink.waitFor(751);
        std::unique_ptr<SessionResult> newest=sink.waitFor(752);
        CHECK(superseded && superseded->status==SessionDataStatus::Superseded);
        CHECK(newest && newest->status==SessionDataStatus::Fresh);
    } else CHECK(sink.waitFor(751)!=nullptr);
    CHECK(worker.Stop());
}

static void test_session_worker_request_fault_preserves_existing_pending(){
    SessionResultSink sink; BlockingSessionParse parser;
    std::atomic<bool> armed(false);
    SessionWorkerOps ops=coalescingWorkerOps(sink,parser);
    ops.beforeWorkerStep=[&](SessionWorkerStep step){
        if(step==SessionWorkerStep::RequestPrepare && armed.exchange(false)) throw std::bad_alloc();
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=730; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest original=active; original.requestId=731; original.purpose=SessionPurpose::ManualSave;
    CHECK(worker.Request(original));
    SessionRequest replacement=original; replacement.requestId=732; replacement.purpose=SessionPurpose::ManualRestore;
    armed=true;
    bool replacementThrew=false,replacementAccepted=true;
    try { replacementAccepted=worker.Request(replacement); } catch(...) { replacementThrew=true; }
    CHECK(!replacementThrew && !replacementAccepted);
    CHECK(worker.PendingCount()==1 && worker.OutstandingForApp("firefox")==2);
    parser.release();
    std::unique_ptr<SessionResult> activeResult=sink.waitFor(730);
    std::unique_ptr<SessionResult> originalResult=sink.waitFor(731);
    CHECK(activeResult && activeResult->status==SessionDataStatus::Fresh);
    CHECK(originalResult && originalResult->status==SessionDataStatus::Fresh &&
          originalResult->purpose==SessionPurpose::ManualSave);
    CHECK(worker.Stop());
}

static void test_session_cache_lookup_fault_preserves_output_and_lru(){
    bool armed=false;
    SessionCacheOps ops;
    ops.beforeLookupOutput=[&]{ if(armed){ armed=false; throw std::bad_alloc(); } };
    SessionCache cache(2,1024*1024,ops);
    SessionStamp stampA; stampA.size=1; stampA.mtime=1;
    SessionStamp stampB; stampB.size=2; stampB.mtime=2;
    SessionStamp stampC; stampC.size=3; stampC.mtime=3;
    std::vector<WinFp> a(1),b(1),c(1); a[0].activeTitle="A"; b[0].activeTitle="B"; c[0].activeTitle="C";
    SessionCacheValue outA,outB;
    CHECK(cache.Put("firefox",L"A",stampA,std::move(a),1,1,outA));
    CHECK(cache.Put("firefox",L"B",stampB,std::move(b),2,2,outB));
    outA.windows.reset(); outB.windows.reset();
    SessionCacheValue sentinel=sessionCacheOutputSentinel();
    const std::vector<WinFp>* sentinelIdentity=sentinel.windows.get();
    bool threw=false,found=false;
    armed=true;
    try { found=cache.FindExact("firefox",L"A",stampA,sentinel); } catch(...) { threw=true; }
    CHECK(!threw && !found);
    CHECK(isSessionCacheOutputSentinel(sentinel,sentinelIdentity));
    SessionCacheValue inserted;
    CHECK(cache.Put("firefox",L"C",stampC,std::move(c),3,3,inserted));
    SessionCacheValue lookup;
    CHECK(!cache.FindExact("firefox",L"A",stampA,lookup));
    CHECK(cache.FindExact("firefox",L"B",stampB,lookup) && lookup.windows->at(0).activeTitle=="B");
}

static void test_session_cache_runtime_fault_is_transactional(){
    bool armed=false;
    SessionCacheOps ops;
    ops.beforePutStep=[&](SessionCachePutStep step){
        if(armed && step==SessionCachePutStep::ContainerInsert) throw std::runtime_error("cache runtime fault");
    };
    SessionCache cache(2,1024*1024,ops);
    SessionStamp stampA; stampA.size=1; stampA.mtime=1;
    SessionStamp stampB; stampB.size=2; stampB.mtime=2;
    std::vector<WinFp> a(1),b(1); a[0].activeTitle="A"; b[0].activeTitle="B";
    SessionCacheValue stored;
    CHECK(cache.Put("firefox",L"A",stampA,std::move(a),1,1,stored));
    const std::vector<WinFp>* identity=stored.windows.get();
    SessionCacheValue output=sessionCacheOutputSentinel();
    const std::vector<WinFp>* outputIdentity=output.windows.get();
    bool threw=false,returned=true; armed=true;
    try { returned=cache.Put("firefox",L"B",stampB,std::move(b),2,2,output); } catch(...) { threw=true; }
    armed=false;
    CHECK(!threw && !returned);
    CHECK(isSessionCacheOutputSentinel(output,outputIdentity));
    CHECK(cache.EntryCount()==1);
    SessionCacheValue stillA;
    CHECK(cache.FindExact("firefox",L"A",stampA,stillA) && stillA.windows.get()==identity);
}

static void test_post_message_exception_deletes_heap_result(){
    SessionWorkerOps ops;
    ops.postMessage=[](HWND,UINT,WPARAM,LPARAM)->bool{ throw std::runtime_error("poster fault"); };
    FillMissingSessionWorkerOps(ops);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result(new SessionResult()); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(!PostSessionResultOwned(ops,(HWND)1,std::move(result)) && payload.use_count()==1);
}

// ---- failure-atomic layout-store tests ----

static bool IsFixtureRootSafe(const std::wstring& root){
    wchar_t tempBuffer[MAX_PATH+1]={0}, rootBuffer[32768]={0}, tempFullBuffer[32768]={0};
    DWORD tempLength=GetTempPathW(MAX_PATH,tempBuffer);
    if(tempLength==0 || tempLength>MAX_PATH) return false;
    DWORD rootLength=GetFullPathNameW(root.c_str(),32768,rootBuffer,nullptr);
    DWORD tempFullLength=GetFullPathNameW(tempBuffer,32768,tempFullBuffer,nullptr);
    if(rootLength==0 || rootLength>=32768 || tempFullLength==0 || tempFullLength>=32768) return false;
    std::wstring fullRoot(rootBuffer), fullTemp(tempFullBuffer);
    if(fullTemp.back()!=L'\\') fullTemp.push_back(L'\\');
    if(fullRoot.size()<=fullTemp.size() || _wcsnicmp(fullRoot.c_str(),fullTemp.c_str(),fullTemp.size())!=0)
        return false;
    return fullRoot.find(L"vde-layout-test-",fullTemp.size())==fullTemp.size();
}

static void RemoveFixtureTreeNoReparse(const std::wstring& path, const std::wstring& root){
    if(path.size()<root.size() || _wcsnicmp(path.c_str(),root.c_str(),root.size())!=0) return;
    DWORD rootAttributes=GetFileAttributesW(path.c_str());
    if(rootAttributes==INVALID_FILE_ATTRIBUTES) return;
    if(rootAttributes&FILE_ATTRIBUTE_REPARSE_POINT){
        if(rootAttributes&FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
        else DeleteFileW(path.c_str());
        return;
    }
    std::wstring pattern=path+L"\\*";
    WIN32_FIND_DATAW found{};
    HANDLE search=FindFirstFileW(pattern.c_str(),&found);
    if(search!=INVALID_HANDLE_VALUE){
        do{
            if(wcscmp(found.cFileName,L".")==0 || wcscmp(found.cFileName,L"..")==0) continue;
            std::wstring child=path+L"\\"+found.cFileName;
            if(child.size()<=root.size() || _wcsnicmp(child.c_str(),root.c_str(),root.size())!=0) continue;
            if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
                if(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(child.c_str());
                else RemoveFixtureTreeNoReparse(child,root);
            } else {
                if(!(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                    SetFileAttributesW(child.c_str(),FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while(FindNextFileW(search,&found));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

struct LayoutTempDir {
    std::wstring path;
    LayoutTempDir(){
        wchar_t temp[MAX_PATH+1]={0};
        DWORD length=GetTempPathW(MAX_PATH,temp);
        GUID id{};
        CHECK(length>0 && length<=MAX_PATH);
        CHECK(SUCCEEDED(CoCreateGuid(&id)));
        path=std::wstring(temp)+L"vde-layout-test-"+GuidToString(id);
        CHECK(IsFixtureRootSafe(path));
        CHECK(CreateDirectoryW(path.c_str(),nullptr)!=0);
    }
    ~LayoutTempDir(){
        if(!path.empty() && IsFixtureRootSafe(path)) RemoveFixtureTreeNoReparse(path,path);
    }
    std::wstring file(const wchar_t* name) const { return path+L"\\"+name; }
};

static bool WriteRawFile(const std::wstring& path, const std::string& bytes){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    size_t offset=0;
    bool ok=true;
    while(offset<bytes.size()){
        DWORD requested=(DWORD)(std::min)(bytes.size()-offset,(size_t)65536);
        DWORD written=0;
        if(!WriteFile(file,bytes.data()+offset,requested,&written,nullptr) || written==0 || written>requested){
            ok=false; break;
        }
        offset+=written;
    }
    if(ok && !FlushFileBuffers(file)) ok=false;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool ResizeRawFile(const std::wstring& path, unsigned long long size){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{}; position.QuadPart=(LONGLONG)size;
    bool ok=SetFilePointerEx(file,position,nullptr,FILE_BEGIN)!=0 && SetEndOfFile(file)!=0 &&
        FlushFileBuffers(file)!=0;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool SetRawFileMtime(const std::wstring& path,unsigned long long mtime){
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    ULARGE_INTEGER combined{};
    combined.QuadPart=mtime;
    FILETIME value{};
    value.dwLowDateTime=combined.LowPart;
    value.dwHighDateTime=combined.HighPart;
    bool ok=SetFileTime(file,nullptr,nullptr,&value)!=0;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool RawFileExists(const std::wstring& path){
    DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ReadRawFile(const std::wstring& path){
    FileReadResult read=ReadFileBytesBounded(path,MAX_LAYOUT_FILE_BYTES);
    return read.status==FileReadStatus::Ok ? read.bytes : std::string();
}

static void test_session_bounded_reader_binds_bytes_to_exact_aba_handle(){
    LayoutTempDir temp;
    std::wstring live=temp.file(L"live.bin");
    std::wstring incoming=temp.file(L"incoming.bin");
    std::wstring parkedA=temp.file(L"parked-a.bin");
    std::wstring parkedB=temp.file(L"parked-b.bin");
    CHECK(WriteRawFile(live,"AAAA"));
    CHECK(WriteRawFile(incoming,"BBBB"));
    SessionStamp stampA,stampB;
    CHECK(GetSessionStamp(live,stampA));
    CHECK(GetSessionStamp(incoming,stampB));
    CHECK(stampA!=stampB && stampA.size==stampB.size);
    CHECK(MoveFileExW(live.c_str(),parkedA.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);
    CHECK(MoveFileExW(incoming.c_str(),live.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);

    bool restored=false;
    SessionFileReadOps ops;
    ops.afterOpen=[&](HANDLE){
        bool movedB=MoveFileExW(live.c_str(),parkedB.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
        bool movedA=MoveFileExW(parkedA.c_str(),live.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
        restored=movedA&&movedB;
        if(!restored) throw std::runtime_error("ABA restore failed");
    };
    SessionFileReadResult read=ReadBrowserSessionFileBounded(live,MAX_BROWSER_SESSION_BYTES,ops);
    SessionStamp current;
    CHECK(restored && GetSessionStamp(live,current) &&
          current.volumeSerial==stampA.volumeSerial && current.fileIdLow==stampA.fileIdLow &&
          current.fileIdHigh==stampA.fileIdHigh);
    CHECK(read.status==FileReadStatus::Ok && read.bytes=="BBBB" && read.readStampKnown);
    CHECK(read.readStamp.volumeSerial==stampB.volumeSerial &&
          read.readStamp.fileIdLow==stampB.fileIdLow && read.readStamp.fileIdHigh==stampB.fileIdHigh &&
          read.readStamp!=current);
}

static void test_session_bounded_reader_rejects_handle_changes_and_close_failure(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"session.bin");
    CHECK(WriteRawFile(path,std::string(128*1024,'s')));

    SessionFileReadOps exactOps;
    auto exactRead=exactOps.readFile;
    auto exactStamp=exactOps.getStamp;
    std::vector<DWORD> requested;
    int stampCalls=0;
    exactOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        requested.push_back(amount); return exactRead(file,bytes,amount,read);
    };
    exactOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        ++stampCalls; return exactStamp(file,stamp);
    };
    SessionFileReadResult exact=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,exactOps);
    SessionStamp pathStamp;
    CHECK(exact.status==FileReadStatus::Ok && exact.readStampKnown &&
          GetSessionStamp(path,pathStamp) && exact.readStamp==pathStamp);
    CHECK(exact.bytes.size()==128*1024 && stampCalls==2 && requested.size()==3);
    CHECK((std::all_of)(requested.begin(),requested.end(),
        [](DWORD amount){ return amount<=64*1024; }));
    CHECK(requested.back()==1);

    SessionFileReadOps sharedWriteOps;
    bool concurrentWriterOpened=false;
    sharedWriteOps.afterOpen=[&](HANDLE){
        HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,nullptr);
        concurrentWriterOpened=writer!=INVALID_HANDLE_VALUE;
        if(concurrentWriterOpened) CloseHandle(writer);
    };
    SessionFileReadResult sharedWrite=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,sharedWriteOps);
    CHECK(concurrentWriterOpened && sharedWrite.status==FileReadStatus::Ok &&
          sharedWrite.readStampKnown);

    SessionFileReadOps truncatedOps;
    auto truncatedRead=truncatedOps.readFile;
    auto truncatedClose=truncatedOps.closeHandle;
    int truncatedCalls=0;
    int truncatedCloseCalls=0;
    truncatedOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        if(truncatedCalls++==0) return truncatedRead(file,bytes,(std::min)(amount,3UL),read);
        read=0; return TRUE;
    };
    truncatedOps.closeHandle=[&](HANDLE file){
        ++truncatedCloseCalls; return truncatedClose(file);
    };
    SessionFileReadResult truncated=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,truncatedOps);
    CHECK(truncatedCloseCalls==1 && truncated.status==FileReadStatus::Unavailable &&
          truncated.bytes.empty() && !truncated.readStampKnown);

    SessionFileReadOps growthOps;
    auto growthRead=growthOps.readFile;
    auto growthStamp=growthOps.getStamp;
    int growthCalls=0;
    int growthStampCalls=0;
    growthOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        if(++growthCalls==3){ *(char*)bytes='x'; read=1; return TRUE; }
        return growthRead(file,bytes,amount,read);
    };
    growthOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        ++growthStampCalls; return growthStamp(file,stamp);
    };
    SessionFileReadResult growth=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,growthOps);
    CHECK(growthStampCalls==2 && growth.status==FileReadStatus::Unavailable &&
          growth.bytes.empty() && !growth.readStampKnown);

    SessionFileReadOps changedOps;
    auto changedStamp=changedOps.getStamp;
    int changedCalls=0;
    changedOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        bool ok=changedStamp(file,stamp);
        if(ok && ++changedCalls==2) ++stamp.changeTime;
        return ok;
    };
    SessionFileReadResult changed=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,changedOps);
    CHECK(changedCalls==2 && changed.status==FileReadStatus::Unavailable &&
          changed.bytes.empty() && !changed.readStampKnown);

    SessionFileReadOps overLimitOps;
    auto overLimitStamp=overLimitOps.getStamp;
    int overLimitCalls=0;
    overLimitOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        bool ok=overLimitStamp(file,stamp);
        if(ok && ++overLimitCalls==2) stamp.size=128*1024+1;
        return ok;
    };
    SessionFileReadResult overLimit=ReadBrowserSessionFileBounded(path,128*1024,overLimitOps);
    CHECK(overLimitCalls==2 && overLimit.status==FileReadStatus::TooLarge &&
          overLimit.bytes.empty() && !overLimit.readStampKnown);

    SessionFileReadOps closeOps;
    auto realClose=closeOps.closeHandle;
    int closeCalls=0;
    closeOps.closeHandle=[&](HANDLE file)->BOOL{
        ++closeCalls; realClose(file); SetLastError(ERROR_INVALID_HANDLE); return FALSE;
    };
    SessionFileReadResult closeFailed=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,closeOps);
    CHECK(closeCalls==1 && closeFailed.status==FileReadStatus::Unavailable &&
          closeFailed.bytes.empty() && !closeFailed.readStampKnown);

    SessionFileReadOps throwingCloseOps;
    auto throwingRealClose=throwingCloseOps.closeHandle;
    HANDLE reusedHandle=nullptr;
    std::vector<HANDLE> otherHandles;
    int throwingCloseCalls=0;
    throwingCloseOps.closeHandle=[&](HANDLE file)->BOOL{
        ++throwingCloseCalls;
        if(!throwingRealClose(file)) throw std::runtime_error("real close failed");
        for(int attempt=0;attempt<256 && !reusedHandle;++attempt){
            HANDLE candidate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(candidate==file) reusedHandle=candidate;
            else if(candidate) otherHandles.push_back(candidate);
        }
        throw std::runtime_error("close callback fault after close");
    };
    SessionFileReadResult closeThrew=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,throwingCloseOps);
    DWORD handleFlags=0;
    bool reusedStillValid=reusedHandle && GetHandleInformation(reusedHandle,&handleFlags)!=FALSE;
    CHECK(throwingCloseCalls==1 && reusedHandle!=nullptr && reusedStillValid);
    CHECK(closeThrew.status==FileReadStatus::Unavailable && closeThrew.bytes.empty() &&
          !closeThrew.readStampKnown);
    if(reusedStillValid) CloseHandle(reusedHandle);
    for(size_t i=0;i<otherHandles.size();++i) CloseHandle(otherHandles[i]);

    SessionFileReadOps throwingStampOps;
    auto throwingStampClose=throwingStampOps.closeHandle;
    int throwingStampCloseCalls=0;
    throwingStampOps.getStamp=[](HANDLE,SessionStamp&)->bool{ throw std::bad_alloc(); };
    throwingStampOps.closeHandle=[&](HANDLE file){
        ++throwingStampCloseCalls; return throwingStampClose(file);
    };
    SessionFileReadResult stampThrew=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,throwingStampOps);
    CHECK(throwingStampCloseCalls==1 && stampThrew.status==FileReadStatus::Unavailable &&
          stampThrew.bytes.empty() && !stampThrew.readStampKnown);

    DWORD handlesBefore=0,handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<128;++attempt){
        SessionFileReadResult repeated=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES);
        CHECK(repeated.status==FileReadStatus::Ok && repeated.readStampKnown);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
}

static std::vector<std::wstring> DiagnosticCopies(const std::wstring& source){
    std::vector<std::wstring> copies;
    size_t slash=source.find_last_of(L"\\/");
    std::wstring directory=slash==std::wstring::npos ? L"." : source.substr(0,slash);
    std::wstring pattern=source+L".corrupt.*";
    WIN32_FIND_DATAW found{};
    HANDLE search=FindFirstFileW(pattern.c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return copies;
    do{
        if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) copies.push_back(directory+L"\\"+found.cFileName);
    } while(FindNextFileW(search,&found));
    FindClose(search);
    std::sort(copies.begin(),copies.end());
    return copies;
}

static std::string ValidLayoutBytes(const std::string& desktopName){
    return std::string("# VDE snapshot v4\n")+
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t"+b64enc(desktopName)+"\n";
}

static std::string LoadedDesktopName(const LayoutLoadResult& loaded){
    return loaded.desks.empty() ? std::string() : W2U8(loaded.desks.front().name);
}

static void test_bounded_read_exact_limit_and_preallocation_rejection(){
    LayoutTempDir temp;
    std::wstring exact=temp.file(L"exact.bin"), oversized=temp.file(L"oversized.bin");
    CHECK(ResizeRawFile(exact,MAX_LAYOUT_FILE_BYTES));
    LayoutFsOps exactOps;
    auto exactRead=exactOps.readFile;
    std::vector<DWORD> requestedSizes;
    exactOps.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        requestedSizes.push_back(requested);
        return exactRead(handle,buffer,requested,read);
    };
    FileReadResult atLimit=ReadFileBytesBounded(exact,MAX_LAYOUT_FILE_BYTES,exactOps);
    CHECK(atLimit.status==FileReadStatus::Ok);
    CHECK(atLimit.bytes.size()==(size_t)MAX_LAYOUT_FILE_BYTES);
    CHECK(requestedSizes.size()==257);
    CHECK((std::count)(requestedSizes.begin(),requestedSizes.end(),64*1024)==256);
    CHECK(!requestedSizes.empty() && requestedSizes.back()==1);
    CHECK((std::all_of)(requestedSizes.begin(),requestedSizes.end(),
        [](DWORD requested){ return requested<=64*1024; }));

    CHECK(ResizeRawFile(oversized,MAX_LAYOUT_FILE_BYTES+1));
    LayoutFsOps ops;
    auto realRead=ops.readFile;
    int readCalls=0;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        ++readCalls; return realRead(handle,buffer,requested,read);
    };
    FileReadResult tooLarge=ReadFileBytesBounded(oversized,MAX_LAYOUT_FILE_BYTES,ops);
    CHECK(tooLarge.status==FileReadStatus::TooLarge);
    CHECK(tooLarge.bytes.empty());
    CHECK(readCalls==0);
}

static void test_bounded_read_failures_are_transactional_and_status_bearing(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"input.bin"), missing=temp.file(L"missing.bin");
    CHECK(WriteRawFile(path,"abcdef"));

    LayoutFsOps partialOps;
    auto realRead=partialOps.readFile;
    int calls=0;
    partialOps.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        if(calls++==0) return realRead(handle,buffer,(std::min)(requested,3UL),read);
        read=0; SetLastError(ERROR_READ_FAULT); return FALSE;
    };
    FileReadResult partial=ReadFileBytesBounded(path,1024,partialOps);
    CHECK(partial.status==FileReadStatus::Unavailable);
    CHECK(partial.bytes.empty());
    CHECK(!partial.error.empty());

    LayoutFsOps changedOps;
    auto realSize=changedOps.getSize;
    int sizeCalls=0;
    changedOps.getSize=[&](HANDLE handle,unsigned long long& size)->BOOL{
        BOOL ok=realSize(handle,size);
        if(ok && ++sizeCalls>=2) ++size;
        return ok;
    };
    FileReadResult changed=ReadFileBytesBounded(path,1024,changedOps);
    CHECK(changed.status==FileReadStatus::Unavailable);
    CHECK(changed.bytes.empty());

    LayoutFsOps shortOps;
    shortOps.readFile=[](HANDLE,void*,DWORD,DWORD& read)->BOOL{ read=0; return TRUE; };
    FileReadResult shortened=ReadFileBytesBounded(path,1024,shortOps);
    CHECK(shortened.status==FileReadStatus::Unavailable);
    CHECK(shortened.bytes.empty());

    LayoutFsOps growthOps;
    auto growthRealSize=growthOps.getSize;
    int growthSizeCalls=0;
    growthOps.getSize=[&](HANDLE handle,unsigned long long& size)->BOOL{
        BOOL ok=growthRealSize(handle,size);
        if(ok && ++growthSizeCalls>=2) size=7;
        return ok;
    };
    FileReadResult grewOverLimit=ReadFileBytesBounded(path,6,growthOps);
    CHECK(grewOverLimit.status==FileReadStatus::TooLarge);
    CHECK(grewOverLimit.bytes.empty());

    LayoutFsOps mtimeOps;
    mtimeOps.getMtime=[](HANDLE,unsigned long long&)->BOOL{
        SetLastError(ERROR_LOCK_VIOLATION); return FALSE;
    };
    FileReadResult noMtime=ReadFileBytesBounded(path,1024,mtimeOps);
    CHECK(noMtime.status==FileReadStatus::Unavailable);
    CHECK(noMtime.bytes.empty());

    LayoutFsOps changedMtimeOps;
    auto realMtime=changedMtimeOps.getMtime;
    int mtimeCalls=0;
    changedMtimeOps.getMtime=[&](HANDLE handle,unsigned long long& mtime)->BOOL{
        BOOL ok=realMtime(handle,mtime);
        if(ok && ++mtimeCalls>=2) ++mtime;
        return ok;
    };
    FileReadResult changedMtime=ReadFileBytesBounded(path,1024,changedMtimeOps);
    CHECK(mtimeCalls>=2);
    CHECK(changedMtime.status==FileReadStatus::Unavailable);
    CHECK(changedMtime.bytes.empty());

    LayoutFsOps sizeOps;
    sizeOps.getSize=[](HANDLE,unsigned long long&)->BOOL{
        SetLastError(ERROR_CRC); return FALSE;
    };
    FileReadResult noSize=ReadFileBytesBounded(path,1024,sizeOps);
    CHECK(noSize.status==FileReadStatus::Unavailable);
    CHECK(noSize.bytes.empty());

    LayoutFsOps attributeOps;
    attributeOps.getAttributes=[&](const std::wstring& queried)->DWORD{
        if(queried==path){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_FILE_ATTRIBUTES; }
        return GetFileAttributesW(queried.c_str());
    };
    FileReadResult noAttributes=ReadFileBytesBounded(path,1024,attributeOps);
    CHECK(noAttributes.status==FileReadStatus::Unavailable);
    CHECK(noAttributes.bytes.empty());

    LayoutFsOps closeOps;
    auto realClose=closeOps.closeHandle;
    closeOps.closeHandle=[&](HANDLE handle)->BOOL{
        realClose(handle); SetLastError(ERROR_INVALID_HANDLE); return FALSE;
    };
    FileReadResult closeFailed=ReadFileBytesBounded(path,1024,closeOps);
    CHECK(closeFailed.status==FileReadStatus::Unavailable);
    CHECK(closeFailed.bytes.empty());

    LayoutFsOps accessOps;
    auto realOpen=accessOps.openFile;
    accessOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==path){ SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        return realOpen(opened,access,share,creation,flags);
    };
    FileReadResult unavailable=ReadFileBytesBounded(path,1024,accessOps);
    FileReadResult absent=ReadFileBytesBounded(missing,1024);
    CHECK(unavailable.status==FileReadStatus::Unavailable);
    CHECK(unavailable.win32Error==ERROR_ACCESS_DENIED);
    CHECK(absent.status==FileReadStatus::Missing);
    CHECK(absent.bytes.empty());

    std::string legacy="sentinel";
    CHECK(!ReadFileBytes(missing,legacy));
    CHECK(legacy=="sentinel");
}

static void test_bounded_read_denies_concurrent_in_place_writer(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"stable.bin");
    CHECK(WriteRawFile(path,std::string(128*1024,'s')));
    LayoutFsOps ops;
    auto realRead=ops.readFile;
    bool attempted=false, writerOpened=false;
    DWORD writerError=ERROR_SUCCESS;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        if(!attempted){
            attempted=true;
            HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,nullptr);
            writerOpened=writer!=INVALID_HANDLE_VALUE;
            writerError=writerOpened ? ERROR_SUCCESS : GetLastError();
            if(writerOpened) CloseHandle(writer);
        }
        return realRead(handle,buffer,requested,read);
    };
    FileReadResult read=ReadFileBytesBounded(path,1024*1024,ops);
    CHECK(read.status==FileReadStatus::Ok && read.bytes.size()==128*1024);
    CHECK(attempted);
    CHECK(!writerOpened);
    CHECK(writerError==ERROR_SHARING_VIOLATION);
}

static void test_layout_load_missing_and_valid_primary(){
    LayoutTempDir temp;
    std::wstring missing=temp.file(L"missing-layout.txt");
    LayoutLoadResult empty=LoadLayoutWithBackup(missing,1700000000);
    CHECK(empty.status==LayoutLoadStatus::Missing);
    CHECK(empty.writesAllowed);
    CHECK(!empty.usable());
    CHECK(empty.desks.empty() && empty.wins.empty());
    CHECK(empty.revision.sourcePath==missing && !empty.revision.exists);

    std::wstring primary=temp.file(L"layout.txt");
    std::string primaryBytes=ValidLayoutBytes("primary");
    CHECK(WriteRawFile(primary,primaryBytes));
    CHECK(WriteRawFile(primary+L".rollback","stale corrupt recovery"));
    LayoutLoadResult loaded=LoadLayoutWithBackup(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Valid);
    CHECK(loaded.writesAllowed && loaded.usable());
    CHECK(loaded.sourceVersion==4);
    CHECK(LoadedDesktopName(loaded)=="primary");
    CHECK(loaded.revision.exists && loaded.revision.sourcePath==primary);
    CHECK(loaded.revision.size==primaryBytes.size());
    CHECK(DiagnosticCopies(primary+L".rollback").empty());
}

static void test_layout_recovery_prefers_valid_rollback_and_preserves_primary(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    const std::string corrupt="corrupt primary", rollback=ValidLayoutBytes("rollback"), bak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,corrupt));
    CHECK(WriteRawFile(primary+L".rollback",rollback));
    CHECK(WriteRawFile(primary+L".bak",bak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(loaded.writesAllowed && loaded.usable());
    CHECK(LoadedDesktopName(loaded)=="rollback");
    CHECK(loaded.revision.sourcePath==primary+L".rollback");
    CHECK(ReadRawFile(primary+L".bak")==bak);
    std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
    CHECK(diagnostics.size()==1);
    CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);
}

static void test_layout_recovery_uses_bak_and_preserves_all_corruption(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string corruptPrimary="bad primary", corruptRollback="bad rollback", goodBak=ValidLayoutBytes("bak");
    CHECK(WriteRawFile(primary,corruptPrimary));
    CHECK(WriteRawFile(rollback,corruptRollback));
    CHECK(WriteRawFile(bak,goodBak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(LoadedDesktopName(loaded)=="bak");
    CHECK(loaded.revision.sourcePath==bak);
    std::vector<std::wstring> primaryDiagnostics=DiagnosticCopies(primary);
    std::vector<std::wstring> rollbackDiagnostics=DiagnosticCopies(rollback);
    CHECK(primaryDiagnostics.size()==1 && ReadRawFile(primaryDiagnostics.front())==corruptPrimary);
    CHECK(rollbackDiagnostics.size()==1 && ReadRawFile(rollbackDiagnostics.front())==corruptRollback);
    CHECK(ReadRawFile(primary)==corruptPrimary && ReadRawFile(rollback)==corruptRollback);
}

static void test_two_corrupt_streams_require_verified_diagnostics(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    const std::string badPrimary="bad-primary", badBak="bad-backup";
    CHECK(WriteRawFile(primary,badPrimary));
    CHECK(WriteRawFile(bak,badBak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved);
    CHECK(loaded.writesAllowed && !loaded.usable());
    CHECK(loaded.desks.empty() && loaded.wins.empty());
    std::vector<std::wstring> first=DiagnosticCopies(primary), second=DiagnosticCopies(bak);
    CHECK(first.size()==1 && ReadRawFile(first.front())==badPrimary);
    CHECK(second.size()==1 && ReadRawFile(second.front())==badBak);
    CHECK(ReadRawFile(primary)==badPrimary && ReadRawFile(bak)==badBak);
}

static void test_transient_primary_open_blocks_backup_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    CHECK(WriteRawFile(primary,"corrupt primary"));
    CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int backupOpenCalls=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && creation==OPEN_EXISTING){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
        if(opened==bak) ++backupOpenCalls;
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(loaded.status==LayoutLoadStatus::Unavailable);
    CHECK(!loaded.writesAllowed && !loaded.usable());
    CHECK(backupOpenCalls==0);
    CHECK(ReadRawFile(primary)=="corrupt primary");
    CHECK(DiagnosticCopies(primary).empty());
}

static void test_oversized_primary_blocks_all_recovery_without_mutation(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string rollbackBytes=ValidLayoutBytes("oversize-rollback"),
        backupBytes=ValidLayoutBytes("oversize-backup");
    CHECK(ResizeRawFile(primary,MAX_LAYOUT_FILE_BYTES+1));
    CHECK(WriteRawFile(rollback,rollbackBytes)); CHECK(WriteRawFile(bak,backupBytes));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int recoveryOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==rollback || opened==bak) ++recoveryOpens;
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed && !loaded.usable());
    CHECK(recoveryOpens==0);
    CHECK(ReadRawFile(rollback)==rollbackBytes && ReadRawFile(bak)==backupBytes);
    CHECK(DiagnosticCopies(primary).empty() && DiagnosticCopies(rollback).empty() && DiagnosticCopies(bak).empty());
}

static void test_missing_primary_uses_rollback_priority_then_backup_fallback(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        const std::string rollbackBytes=ValidLayoutBytes("plain-rollback"),
            backupBytes=ValidLayoutBytes("plain-older-backup");
        CHECK(WriteRawFile(rollback,rollbackBytes)); CHECK(WriteRawFile(bak,backupBytes));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==rollback && LoadedDesktopName(loaded)=="plain-rollback");
        CHECK(ReadRawFile(rollback)==rollbackBytes && ReadRawFile(bak)==backupBytes);
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string backupBytes=ValidLayoutBytes("plain-backup");
        CHECK(WriteRawFile(bak,backupBytes));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="plain-backup");
        CHECK(ReadRawFile(bak)==backupBytes);
    }
}

static void test_transient_and_corrupt_recovery_states(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        CHECK(WriteRawFile(rollback,ValidLayoutBytes("rollback")));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("bak")));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int bakOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==rollback && creation==OPEN_EXISTING){ SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
            if(opened==bak) ++bakOpens;
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(bakOpens==0);
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback";
        CHECK(WriteRawFile(rollback,"corrupt rollback"));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed);
        std::vector<std::wstring> diagnostics=DiagnosticCopies(rollback);
        CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())=="corrupt rollback");
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        CHECK(WriteRawFile(primary,"corrupt primary"));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && creation==OPEN_EXISTING){ SetLastError(ERROR_LOCK_VIOLATION); return INVALID_HANDLE_VALUE; }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(ReadRawFile(primary)=="corrupt primary" && ReadRawFile(bak)==ValidLayoutBytes("backup"));
    }
}

static void test_diagnostic_copy_failure_and_readback_mismatch_block_writes(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        CHECK(WriteRawFile(primary,"corrupt primary"));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        ops.copyFile=[](const std::wstring&,const std::wstring&,BOOL)->BOOL{
            SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.error.empty());
        CHECK(ReadRawFile(primary)=="corrupt primary" && ReadRawFile(bak)==ValidLayoutBytes("backup"));
        CHECK(DiagnosticCopies(primary).empty());
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string corrupt="corrupt primary";
        std::string sameLengthMismatch=corrupt;
        sameLengthMismatch[0]='C';
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            BOOL copied=realCopy(from,to,failIfExists);
            if(copied && to.find(L".corrupt.")!=std::wstring::npos)
                WriteRawFile(to,sameLengthMismatch);
            return copied;
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.error.empty());
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==ValidLayoutBytes("backup"));
        CHECK(DiagnosticCopies(primary).size()==1);
    }

    for(int failRead=0;failRead<2;++failRead){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string corrupt=failRead ? "corrupt readback read" : "corrupt readback open";
        const std::string recovery=ValidLayoutBytes("diagnostic-readback-recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(bak,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        auto realRead=ops.readFile;
        HANDLE diagnosticHandle=INVALID_HANDLE_VALUE;
        bool injected=false;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened.find(L".corrupt.")!=std::wstring::npos && creation==OPEN_EXISTING){
                if(!failRead){ injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
                diagnosticHandle=realOpen(opened,access,share,creation,flags);
                return diagnosticHandle;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
            if(failRead && handle==diagnosticHandle && !injected){
                injected=true; read=0; SetLastError(ERROR_READ_FAULT); return FALSE;
            }
            return realRead(handle,buffer,requested,read);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.usable() && !loaded.error.empty());
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==recovery);
        std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
        CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);
    }
}

static void test_second_diagnostic_copy_failure_and_collision_never_lose_evidence(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string badPrimary="bad primary", badBackup="bad backup";
        CHECK(WriteRawFile(primary,badPrimary));
        CHECK(WriteRawFile(bak,badBackup));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        int copyCalls=0;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(++copyCalls==2){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realCopy(from,to,failIfExists);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(copyCalls==2);
        CHECK(ReadRawFile(primary)==badPrimary && ReadRawFile(bak)==badBackup);
        std::vector<std::wstring> primaryCopies=DiagnosticCopies(primary), backupCopies=DiagnosticCopies(bak);
        CHECK(primaryCopies.size()==1 && ReadRawFile(primaryCopies.front())==badPrimary);
        CHECK(backupCopies.empty());
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        std::wstring collision=primary+L".corrupt.1700000000.0";
        const std::string corrupt="corrupt primary", sentinel="preexisting diagnostic";
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        CHECK(WriteRawFile(collision,sentinel));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(ReadRawFile(collision)==sentinel);
        CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==corrupt);
        CHECK(DiagnosticCopies(primary).size()==2);
    }
}

static void test_diagnostic_reuse_never_deletes_changed_corrupt_temp(){
    LayoutTempDir tempDir;
    std::wstring primary=tempDir.file(L"layout.txt"), temp=primary+L".tmp",
        diagnostic=temp+L".corrupt.1700000000.0";
    const std::string firstCorrupt="first corrupt temporary layout",
        changedCorrupt="changed corrupt temporary layout";
    CHECK(WriteRawFile(temp,firstCorrupt));
    CHECK(WriteRawFile(diagnostic,firstCorrupt));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==temp && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(temp,changedCorrupt));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(temp)==changedCorrupt);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(DiagnosticCopies(temp).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::CorruptPreserved && retry.writesAllowed);
    CHECK(!RawFileExists(temp));
    std::vector<std::wstring> diagnostics=DiagnosticCopies(temp);
    CHECK(diagnostics.size()==2);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(ReadRawFile(temp+L".corrupt.1700000000.1")==changedCorrupt);
}

static void test_diagnostic_reuse_never_overwrites_changed_corrupt_backup(){
    LayoutTempDir tempDir;
    std::wstring primary=tempDir.file(L"layout.txt"), backup=primary+L".bak",
        marker=primary+L".bak.previous.promote",
        diagnostic=backup+L".corrupt.1700000000.0";
    const std::string firstCorrupt="first corrupt backup",
        changedCorrupt="changed corrupt backup",
        recovery=ValidLayoutBytes("authoritative recovery");
    CHECK(WriteRawFile(backup,firstCorrupt));
    CHECK(WriteRawFile(marker,recovery));
    CHECK(WriteRawFile(diagnostic,firstCorrupt));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==backup && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(backup,changedCorrupt));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(backup)==changedCorrupt);
    CHECK(ReadRawFile(marker)==recovery);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(DiagnosticCopies(backup).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && LoadedDesktopName(retry)=="authoritative recovery");
    CHECK(ReadRawFile(backup)==recovery && ReadRawFile(marker)==recovery);
    CHECK(DiagnosticCopies(backup).size()==2);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(ReadRawFile(backup+L".corrupt.1700000000.1")==changedCorrupt);
}

static void test_diagnostic_preservation_revalidates_primary_before_backup_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
        diagnostic=primary+L".corrupt.1700000000.0";
    const std::string captured="captured corrupt primary",
        changed="changed corrupt primary", recovery=ValidLayoutBytes("backup recovery");
    CHECK(WriteRawFile(primary,captured));
    CHECK(WriteRawFile(backup,recovery));
    CHECK(WriteRawFile(diagnostic,captured));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==primary && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(primary,changed));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(backup)==recovery);
    CHECK(ReadRawFile(diagnostic)==captured && DiagnosticCopies(primary).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && ReadRawFile(primary)==changed);
    CHECK(DiagnosticCopies(primary).size()==2);
    CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==changed);
    LayoutLoadResult stable=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(stable.status==LayoutLoadStatus::Recovered && DiagnosticCopies(primary).size()==2);
}

static void test_diagnostic_preservation_revalidates_primary_before_displaced_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        backup=primary+L".bak", diagnostic=primary+L".corrupt.1700000000.0";
    const std::string captured="captured corrupt beside displaced",
        changed="changed corrupt beside displaced", recovery=ValidLayoutBytes("displaced recovery");
    CHECK(WriteRawFile(primary,captured));
    CHECK(WriteRawFile(displaced,recovery));
    CHECK(WriteRawFile(diagnostic,captured));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==primary && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(primary,changed));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(displaced)==recovery);
    CHECK(!RawFileExists(backup) && DiagnosticCopies(primary).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && ReadRawFile(backup)==recovery);
    CHECK(!RawFileExists(displaced) && ReadRawFile(primary)==changed);
    CHECK(DiagnosticCopies(primary).size()==2);
    LayoutLoadResult stable=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(stable.status==LayoutLoadStatus::Recovered && DiagnosticCopies(primary).size()==2);
}

static void test_fresh_diagnostic_copy_revalidates_primary_before_recovery(){
    for(int copyReturnsFalse=0;copyReturnsFalse<2;++copyReturnsFalse){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
            diagnostic=primary+L".corrupt.1700000000.0";
        const std::string captured="fresh-copy captured primary",
            changed="fresh-copy changed primary", recovery=ValidLayoutBytes("fresh-copy backup");
        CHECK(WriteRawFile(primary,captured)); CHECK(WriteRawFile(backup,recovery));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(from==primary && to==diagnostic && failIfExists && !injected){
                CHECK(realCopy(from,to,failIfExists)!=0);
                CHECK(WriteRawFile(primary,changed));
                injected=true;
                if(copyReturnsFalse){ SetLastError(ERROR_WRITE_FAULT); return FALSE; }
                return TRUE;
            }
            return realCopy(from,to,failIfExists);
        };

        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==changed && ReadRawFile(backup)==recovery);
        CHECK(ReadRawFile(diagnostic)==captured && DiagnosticCopies(primary).size()==1);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==backup);
        CHECK(DiagnosticCopies(primary).size()==2);
        CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==changed);
    }
}

static void test_diagnostic_source_reverify_transient_failure_retries_without_growth(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
        diagnostic=primary+L".corrupt.1700000000.0";
    const std::string corrupt="transient reverify corrupt",
        recovery=ValidLayoutBytes("transient reverify backup");
    CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(backup,recovery));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int primaryOpens=0;
    bool injected=false;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && ++primaryOpens==2){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(backup)==recovery);
    CHECK(ReadRawFile(diagnostic)==corrupt && DiagnosticCopies(primary).size()==1);
    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==backup);
    CHECK(DiagnosticCopies(primary).size()==1);
}

static void test_atomic_write_first_and_two_successful_writes(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    std::string first=ValidLayoutBytes("first"), second=ValidLayoutBytes("second"), error="stale";
    CHECK(AtomicWriteText(primary,first,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==first);
    CHECK(!RawFileExists(primary+L".bak") && !RawFileExists(primary+L".rollback"));
    CHECK(AtomicWriteText(primary,second,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==second);
    CHECK(ReadRawFile(primary+L".bak")==first);
    CHECK(!RawFileExists(primary+L".rollback"));
}

static void test_atomic_write_rejects_oversize_without_touching_destination(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    CHECK(WriteRawFile(primary,"old"));
    std::string oversized((size_t)MAX_LAYOUT_FILE_BYTES+1,'x'), error;
    LayoutFsOps ops;
    int openCalls=0;
    auto realOpen=ops.openFile;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        ++openCalls; return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,oversized,&error,false,ops));
    CHECK(!error.empty());
    CHECK(openCalls==0);
    CHECK(ReadRawFile(primary)=="old");
    CHECK(!RawFileExists(primary+L".tmp"));
}

static void test_atomic_write_faults_keep_old_or_recovery_bytes(){
    const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next");
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"write.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops; auto realWrite=ops.writeFile; int calls=0;
        ops.writeFile=[&](HANDLE handle,const void* buffer,DWORD requested,DWORD& written)->BOOL{
            if(calls++==0) return realWrite(handle,buffer,(std::min)(requested,3UL),written);
            written=0; SetLastError(ERROR_WRITE_FAULT); return FALSE;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"flush.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        ops.flushFile=[](HANDLE)->BOOL{ SetLastError(ERROR_WRITE_FAULT); return FALSE; };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"close.txt");
        LayoutFsOps ops; auto realClose=ops.closeHandle; int closes=0;
        ops.closeHandle=[&](HANDLE handle)->BOOL{
            BOOL closed=realClose(handle);
            if(closes++==0){ SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
            return closed;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(!RawFileExists(primary+L".tmp"));
        CHECK(ReadRawFile(primary+L".tmp.stage")==next);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"replace.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        ops.replaceFile=[](const std::wstring&,const std::wstring&,const std::wstring&,DWORD)->BOOL{
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
}

static void test_preexisting_rollback_promotion_failure_never_touches_primary(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending");
    const std::string retainedRollback=ValidLayoutBytes("retained-rollback"), olderBak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,olderBak));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(ReadRawFile(rollback)==retainedRollback);
    CHECK(ReadRawFile(bak)==olderBak);
}

static void test_rollback_promotion_readback_failure_restores_old_bak_and_rollback(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    std::wstring previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), olderBak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,olderBak));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool promoted=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak) promoted=true;
        return moved;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && promoted && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(ReadRawFile(rollback)==retainedRollback);
    CHECK(ReadRawFile(bak)==olderBak);
    CHECK(!RawFileExists(previous));
}

static void test_preexisting_rollback_promotion_mismatch_recovers_from_verified_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
        tempPath=primary+L".tmp";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), oldBackup=ValidLayoutBytes("old-backup"),
        poison="poisoned promoted rollback";
    CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    bool injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak && !injected){
            injected=true; CHECK(WriteRawFile(to,poison));
        }
        return moved;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==retainedRollback);
    CHECK(ReadRawFile(rollback)==poison && ReadRawFile(bak)==oldBackup);
    CHECK(ReadRawFile(previous)==oldBackup);

    LayoutFsOps stopAfterResolveOps;
    auto realOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,pending,&error,false,stopAfterResolveOps));
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==retainedRollback);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(promote));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty() && ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
}

static void test_post_replace_verification_failure_reports_false_with_bak_readable(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next");
    CHECK(WriteRawFile(primary,prior));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int primaryReadOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && creation==OPEN_EXISTING && ++primaryReadOpens==2){
            SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,next,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==next);
    CHECK(ReadRawFile(bak)==prior);
}

static void test_late_normal_write_failure_retains_staged_old_backup(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool finalPromotion=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak) finalPromotion=true;
        return moved;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && finalPromotion && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==pending);
    CHECK(ReadRawFile(bak)==current);
    CHECK(ReadRawFile(previous)==oldBackup);
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(previous));
}

static void test_retry_after_publish_finishes_pending_promotion_without_self_replace(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending");
    CHECK(WriteRawFile(primary,current));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    bool injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak && !injected){
            injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(rollback)==current);
    CHECK(!RawFileExists(bak));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback));
}

static void test_replace_failure_retains_staged_old_backup_until_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    ops.replaceFile=[](const std::wstring&,const std::wstring&,const std::wstring&,DWORD)->BOOL{
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(!RawFileExists(rollback));
    CHECK(ReadRawFile(bak)==retainedRollback);
    CHECK(ReadRawFile(previous)==oldBackup);
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
}

static void test_staged_backup_resolver_restores_missing_backup_before_cleanup(){
    for(int withRollback=0;withRollback<2;++withRollback){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", previous=bak+L".previous", tempPath=primary+L".tmp";
        const std::string current=ValidLayoutBytes("current"), staged=ValidLayoutBytes("staged"),
            recovery=ValidLayoutBytes("rollback"), pending=ValidLayoutBytes("pending");
        CHECK(WriteRawFile(primary,current));
        CHECK(WriteRawFile(previous,staged));
        if(withRollback) CHECK(WriteRawFile(rollback,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==tempPath && creation==CREATE_ALWAYS){
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==current);
        CHECK(ReadRawFile(bak)==(withRollback ? recovery : staged));
        CHECK(!RawFileExists(previous));
        CHECK(!RawFileExists(rollback));
    }
}

static void test_changed_request_first_reconciles_pending_normal_transaction(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", tempPath=primary+L".tmp";
    const std::string published=ValidLayoutBytes("published"), prior=ValidLayoutBytes("prior"),
        olderRollback=ValidLayoutBytes("older-rollback"), oldBackup=ValidLayoutBytes("old-backup"),
        changed=ValidLayoutBytes("changed");
    CHECK(WriteRawFile(primary,published));
    CHECK(WriteRawFile(rollback,prior));
    CHECK(WriteRawFile(bak,olderRollback));
    CHECK(WriteRawFile(previous,oldBackup));
    LayoutFsOps stopAfterReconcileOps;
    auto realOpen=stopAfterReconcileOps.openFile;
    stopAfterReconcileOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,changed,&error,false,stopAfterReconcileOps));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==published && ReadRawFile(bak)==prior);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
    CHECK(AtomicWriteText(primary,changed,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(bak)==published);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
}

static void test_failed_backup_staging_readback_does_not_poison_retries(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", stage=previous+L".stage";
    const std::string current=ValidLayoutBytes("current"), recovery=ValidLayoutBytes("recovery"),
        oldBackup=ValidLayoutBytes("old-backup"), pending=ValidLayoutBytes("pending"), poison="poison";
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,recovery));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        BOOL copied=realCopy(from,to,failIfExists);
        if(copied && to==stage && !injected){
            injected=true; CHECK(WriteRawFile(to,poison));
        }
        return copied;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(rollback)==recovery && ReadRawFile(bak)==oldBackup);
    CHECK(!RawFileExists(previous));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(stage));
}

static void test_pending_promotion_marker_recovers_post_move_verification_fault(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
        tempPath=primary+L".tmp";
    const std::string published=ValidLayoutBytes("published"), prior=ValidLayoutBytes("prior"),
        older=ValidLayoutBytes("older"), oldest=ValidLayoutBytes("oldest"),
        changed=ValidLayoutBytes("changed");
    CHECK(WriteRawFile(primary,published)); CHECK(WriteRawFile(rollback,prior));
    CHECK(WriteRawFile(bak,older)); CHECK(WriteRawFile(previous,oldest));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool moved=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL result=realMove(from,to,flags);
        if(result && from==rollback && to==bak) moved=true;
        return result;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && moved && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,changed,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==published && ReadRawFile(promote)==prior && ReadRawFile(previous)==oldest);
    CHECK(!RawFileExists(rollback) && ReadRawFile(bak)==prior);

    LayoutFsOps stopAfterResolveOps;
    auto secondRealOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return secondRealOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,changed,&error,false,stopAfterResolveOps));
    CHECK(ReadRawFile(primary)==published && ReadRawFile(bak)==prior);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(promote));
    CHECK(AtomicWriteText(primary,changed,&error));
    CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==published);
}

static void test_promotion_marker_false_after_poison_is_never_authoritative(){
    for(int phase=0;phase<3;++phase){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
            promoteStage=promote+L".stage";
        const std::string current=ValidLayoutBytes("current"), recovery=ValidLayoutBytes("recovery"),
            oldBackup=ValidLayoutBytes("old-backup"), pending=ValidLayoutBytes("pending"), poison="poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,oldBackup));
        if(phase<2) CHECK(WriteRawFile(rollback,recovery));
        if(phase==1) CHECK(WriteRawFile(previous,oldBackup));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(to==promoteStage && !injected){
                BOOL copied=realCopy(from,to,failIfExists);
                CHECK(copied!=0); CHECK(WriteRawFile(to,poison));
                injected=true; SetLastError(ERROR_GEN_FAILURE); return FALSE;
            }
            return realCopy(from,to,failIfExists);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
        CHECK(injected && !error.empty());
        CHECK(!RawFileExists(promote));
        CHECK(ReadRawFile(promoteStage)==poison);
        if(phase<2){
            CHECK(ReadRawFile(primary)==current && ReadRawFile(rollback)==recovery &&
                ReadRawFile(bak)==oldBackup);
        } else {
            CHECK(ReadRawFile(primary)==pending && ReadRawFile(rollback)==current &&
                ReadRawFile(bak)==oldBackup);
        }
        CHECK(AtomicWriteText(primary,pending,&error));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
        CHECK(!RawFileExists(rollback) && !RawFileExists(previous) &&
            !RawFileExists(promote) && !RawFileExists(promoteStage));
    }
}

static void test_transient_promoted_backup_read_is_retained_and_loads_before_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote";
    const std::string prior=ValidLayoutBytes("prior"), older=ValidLayoutBytes("older"),
        oldest=ValidLayoutBytes("oldest"), replacement=ValidLayoutBytes("replacement");
    CHECK(WriteRawFile(rollback,prior)); CHECK(WriteRawFile(bak,older)); CHECK(WriteRawFile(previous,oldest));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool moved=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL result=realMove(from,to,flags);
        if(result && from==rollback && to==bak) moved=true;
        return result;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && moved && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,replacement,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(!RawFileExists(primary) && !RawFileExists(rollback));
    CHECK(ReadRawFile(bak)==prior && ReadRawFile(promote)==prior && ReadRawFile(previous)==oldest);
    LayoutLoadResult beforeRetry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(beforeRetry.status==LayoutLoadStatus::Recovered && beforeRetry.writesAllowed);
    CHECK(beforeRetry.revision.sourcePath==bak && LoadedDesktopName(beforeRetry)=="prior");
    CHECK(AtomicWriteText(primary,replacement,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==prior);
}

static void test_loader_recovers_valid_authoritative_internal_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        promote=primary+L".bak.previous.promote";
    const std::string intended=ValidLayoutBytes("orphan-marker");
    CHECK(WriteRawFile(promote,intended));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
    CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="orphan-marker");
    CHECK(ReadRawFile(bak)==intended && ReadRawFile(promote)==intended);
}

static void test_loader_never_accepts_stale_bak_beside_authoritative_promotion_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", promote=previous+L".promote";
    const std::string intended=ValidLayoutBytes("intended"), stale=ValidLayoutBytes("stale"),
        oldest=ValidLayoutBytes("oldest");
    CHECK(WriteRawFile(bak,stale)); CHECK(WriteRawFile(previous,oldest));
    CHECK(WriteRawFile(promote,intended));
    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==promote && to==bak && !injected){
            injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };
    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(bak)==stale && ReadRawFile(promote)==intended);

    LayoutLoadResult recovered=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(recovered.status==LayoutLoadStatus::Recovered && recovered.writesAllowed);
    CHECK(recovered.revision.sourcePath==bak && LoadedDesktopName(recovered)=="intended");
    CHECK(ReadRawFile(bak)==intended && ReadRawFile(promote)==intended);
}

static void test_valid_primary_reconciles_authoritative_promotion_marker(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("valid-primary"),
            prior=ValidLayoutBytes("prior-from-rollback"), stale=ValidLayoutBytes("stale-backup");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(rollback,prior));
        CHECK(WriteRawFile(bak,stale)); CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="valid-primary");
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(rollback) && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("exact-primary"), prior=ValidLayoutBytes("exact-backup");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed);
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="exact-primary");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("missing-backup-primary"),
            prior=ValidLayoutBytes("marker-repair");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed);
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="missing-backup-primary");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
}

static void test_valid_primary_marker_faults_fail_closed_then_converge(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("corrupt-marker-primary"), corrupt="not a layout";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,corrupt));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed && !loaded.usable());
        CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==corrupt && !RawFileExists(bak));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("transient-marker-primary"),
            prior=ValidLayoutBytes("transient-marker-prior");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,prior));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==promote){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==prior && !RawFileExists(bak));
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("repair-fault-primary"),
            prior=ValidLayoutBytes("repair-fault-prior"), stale=ValidLayoutBytes("repair-fault-stale");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,stale));
        CHECK(WriteRawFile(promote,prior));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(from==promote && to==bak && !injected){
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realCopy(from,to,failIfExists);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==stale && ReadRawFile(promote)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(retry.revision.sourcePath==primary && ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
}

static void test_valid_primary_retries_cleanup_after_trigger_delete_false_effect(){
    for(int deletePromotionMarker=0;deletePromotionMarker<2;++deletePromotionMarker){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promote=previous+L".promote",
            promoteStage=promote+L".stage";
        const std::string current=ValidLayoutBytes(deletePromotionMarker ?
                "marker-delete-current" : "stage-delete-current"),
            prior=ValidLayoutBytes(deletePromotionMarker ?
                "marker-delete-prior" : "stage-delete-prior"),
            staged=ValidLayoutBytes("staged-old-backup"), poison="non-authoritative stage";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,staged));
        const std::wstring& trigger=deletePromotionMarker ? promote : promoteStage;
        CHECK(WriteRawFile(trigger,deletePromotionMarker ? prior : poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==trigger && !injected){
                CHECK(realDelete(deleted)!=0); injected=true;
                SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(!RawFileExists(trigger) && RawFileExists(previous));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);

        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed && retry.usable());
        CHECK(retry.revision.sourcePath==primary && LoadedDesktopName(retry)==
            (deletePromotionMarker ? "marker-delete-current" : "stage-delete-current"));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(previous) && !RawFileExists(promote) && !RawFileExists(promoteStage));
    }
}

static void test_first_publish_tmp_recovers_on_restart_after_move_failure(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), tempPath=primary+L".tmp";
    const std::string intended=ValidLayoutBytes("first-publish");
    LayoutFsOps writeOps;
    auto realMove=writeOps.moveFile;
    bool writeMoveFailed=false;
    writeOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==tempPath && to==primary && !writeMoveFailed){
            writeMoveFailed=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,intended,&error,false,writeOps));
    CHECK(writeMoveFailed && !error.empty() && !RawFileExists(primary));
    CHECK(ReadRawFile(tempPath)==intended);

    LayoutFsOps loadOps;
    auto loadRealMove=loadOps.moveFile;
    bool restartMoveFailed=false;
    loadOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==tempPath && to==primary && !restartMoveFailed){
            restartMoveFailed=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return loadRealMove(from,to,flags);
    };
    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,loadOps);
    CHECK(restartMoveFailed && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(!RawFileExists(primary) && ReadRawFile(tempPath)==intended);

    LayoutLoadResult recovered=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(recovered.status==LayoutLoadStatus::Recovered && recovered.writesAllowed);
    CHECK(recovered.revision.sourcePath==primary && LoadedDesktopName(recovered)=="first-publish");
    CHECK(ReadRawFile(primary)==intended && !RawFileExists(tempPath));
    LayoutLoadResult settled=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(settled.status==LayoutLoadStatus::Valid && LoadedDesktopName(settled)=="first-publish");
}

static void test_partial_valid_prefix_is_never_treated_as_committed_first_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), committed=primary+L".tmp",
        stage=committed+L".stage";
    const std::string prefix="# VDE snapshot v4\n", intended=ValidLayoutBytes("must-not-disappear");
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string parseError;
    int sourceVersion=0;
    CHECK(ParseLayout(prefix,desks,wins,1700000000,&parseError,&sourceVersion));
    CHECK(desks.empty() && wins.empty() && sourceVersion==4);
    LayoutFsOps ops;
    auto realWrite=ops.writeFile;
    bool injected=false;
    ops.writeFile=[&](HANDLE file,const void* buffer,DWORD requested,DWORD& written)->BOOL{
        if(!injected){
            DWORD prefixWritten=0;
            BOOL wrote=realWrite(file,buffer,(DWORD)prefix.size(),prefixWritten);
            CHECK(wrote!=0 && prefixWritten==prefix.size() && requested>prefixWritten);
            written=prefixWritten;
            injected=true;
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        return realWrite(file,buffer,requested,written);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,intended,&error,false,ops));
    CHECK(injected && !error.empty() && !RawFileExists(primary));
    CHECK(!RawFileExists(committed) && ReadRawFile(stage)==prefix);

    LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(first.status==LayoutLoadStatus::Missing && first.writesAllowed && !first.usable());
    CHECK(!RawFileExists(primary) && !RawFileExists(committed) && !RawFileExists(stage));
    LayoutLoadResult again=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(again.status==LayoutLoadStatus::Missing && again.writesAllowed);
}

static void test_temporary_commit_move_failure_converges_before_and_after_effect(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), committed=primary+L".tmp",
            stage=committed+L".stage";
        const std::string intended=ValidLayoutBytes(afterEffect ? "after-effect" : "before-effect");
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==stage && to==committed && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true;
                SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
                return FALSE;
            }
            return realMove(from,to,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,intended,&error,false,ops));
        CHECK(injected && !error.empty() && !RawFileExists(primary));
        CHECK(afterEffect ? ReadRawFile(committed)==intended : ReadRawFile(stage)==intended);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        if(afterEffect){
            CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed);
            CHECK(LoadedDesktopName(loaded)=="after-effect" && ReadRawFile(primary)==intended);
        } else {
            CHECK(loaded.status==LayoutLoadStatus::Missing && loaded.writesAllowed);
            CHECK(!RawFileExists(primary));
        }
        CHECK(!RawFileExists(committed) && !RawFileExists(stage));
    }
}

static void test_idempotent_retry_cleans_orphan_prior_backup_stage(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", stage=previous+L".stage";
    const std::string current=ValidLayoutBytes("current"), oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,oldBackup));
    CHECK(WriteRawFile(previous,oldBackup)); CHECK(WriteRawFile(stage,oldBackup));
    std::string error;
    CHECK(AtomicWriteText(primary,current,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==oldBackup);
    CHECK(!RawFileExists(previous) && !RawFileExists(stage));
}

static void test_orphan_promotion_stage_is_non_authoritative_and_converges(){
    for(int withPrevious=0;withPrevious<2;++withPrevious){
        for(int changedRequest=0;changedRequest<2;++changedRequest){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", promoteStage=previous+L".promote.stage";
            const std::string current=ValidLayoutBytes("orphan-current"),
                prior=ValidLayoutBytes("orphan-prior"), changed=ValidLayoutBytes("orphan-changed"),
                poison=ValidLayoutBytes("uncommitted-stage-poison");
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
            if(withPrevious) CHECK(WriteRawFile(previous,prior));
            CHECK(WriteRawFile(promoteStage,poison));
            const std::string& requested=changedRequest ? changed : current;
            std::string error;
            CHECK(AtomicWriteText(primary,requested,&error));
            CHECK(error.empty() && ReadRawFile(primary)==requested);
            CHECK(ReadRawFile(bak)==(changedRequest ? current : prior));
            CHECK(!RawFileExists(promoteStage) && !RawFileExists(previous));
        }
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promoteStage=previous+L".promote.stage";
        const std::string current=ValidLayoutBytes("delete-effect-current"),
            prior=ValidLayoutBytes("delete-effect-prior"), changed=ValidLayoutBytes("delete-effect-next"),
            poison=ValidLayoutBytes("delete-effect-stage");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(promoteStage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==promoteStage && !injected){
                CHECK(realDelete(deleted)!=0); injected=true;
                SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,changed,&error,false,ops));
        CHECK(injected && !error.empty() && !RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && ReadRawFile(previous)==prior);
        CHECK(AtomicWriteText(primary,changed,&error));
        CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==current);
        CHECK(!RawFileExists(previous));
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promoteStage=previous+L".promote.stage";
        const std::string current=ValidLayoutBytes("transient-current"),
            prior=ValidLayoutBytes("transient-prior"), poison=ValidLayoutBytes("transient-stage");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(promoteStage,poison));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int backupOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && ++backupOpens==2){
                SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,current,&error,false,ops));
        CHECK(!error.empty() && RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(AtomicWriteText(primary,current,&error));
        CHECK(error.empty() && !RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_non_authoritative_stages_are_discarded_without_reading_bytes(){
    for(int stageKind=0;stageKind<2;++stageKind){
        for(int withPrevious=0;withPrevious<2;++withPrevious){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("oversized-stage-current"),
                prior=ValidLayoutBytes("oversized-stage-prior"), older=ValidLayoutBytes("oversized-stage-older");
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
            if(withPrevious) CHECK(WriteRawFile(previous,older));
            CHECK(ResizeRawFile(stage,MAX_LAYOUT_FILE_BYTES+1));
            LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
            CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="oversized-stage-current");
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
            CHECK(!RawFileExists(stage) && !RawFileExists(previous));
        }

        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=stageKind ?
                previous+L".promote.stage" : previous+L".stage";
        const std::string current=ValidLayoutBytes("unreadable-stage-current"),
            prior=ValidLayoutBytes("unreadable-stage-prior"), poison="untrusted poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int stageOpenAttempts=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==stage){
                ++stageOpenAttempts; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(AtomicWriteText(primary,current,&error,false,ops));
        CHECK(error.empty() && stageOpenAttempts==0 && !RawFileExists(stage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_non_authoritative_stage_faults_fail_closed_and_retry(){
    for(int stageKind=0;stageKind<2;++stageKind){
        for(int afterEffect=0;afterEffect<2;++afterEffect){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("stage-delete-current"),
                prior=ValidLayoutBytes("stage-delete-prior"), poison="stage-delete-poison";
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
            LayoutFsOps ops;
            auto realDelete=ops.deleteFile;
            bool injected=false;
            ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
                if(deleted==stage && !injected){
                    if(afterEffect) CHECK(realDelete(deleted)!=0);
                    injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
                }
                return realDelete(deleted);
            };
            LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
            CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
            CHECK(afterEffect ? !RawFileExists(stage) : RawFileExists(stage));
            LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && !RawFileExists(stage));
        }

        {
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("stage-transient-current"),
                prior=ValidLayoutBytes("stage-transient-prior"), poison="stage-transient-poison";
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
            LayoutFsOps ops;
            auto realOpen=ops.openFile;
            int backupOpens=0;
            ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                    DWORD creation,DWORD flags)->HANDLE{
                if(opened==bak && ++backupOpens==2){
                    SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
                }
                return realOpen(opened,access,share,creation,flags);
            };
            LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
            CHECK(blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && RawFileExists(stage));
            LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(retry.status==LayoutLoadStatus::Valid && !RawFileExists(stage));
        }

        {
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string requested=ValidLayoutBytes("no-stage-survivor"), poison="orphan poison";
            CHECK(WriteRawFile(stage,poison));
            std::string error;
            CHECK(!AtomicWriteText(primary,requested,&error,false));
            CHECK(!error.empty() && !RawFileExists(primary) && !RawFileExists(bak));
            CHECK(ReadRawFile(stage)==poison && !RawFileExists(previous));
        }
    }
}

static void test_previous_stage_cleanup_reverifies_before_consuming_previous(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=previous+L".stage";
        const std::string current=ValidLayoutBytes("boundary-current"),
            prior=ValidLayoutBytes("boundary-prior"), poison="boundary poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        auto realOpen=ops.openFile;
        bool stageDeleted=false,injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            BOOL result=realDelete(deleted);
            if(result && deleted==stage) stageDeleted=true;
            return result;
        };
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && stageDeleted && !injected){
                injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(!RawFileExists(stage) && ReadRawFile(previous)==prior);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(stage) && !RawFileExists(previous));
    }

    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=previous+L".stage";
        const std::string current=ValidLayoutBytes("delete-boundary-current"),
            prior=ValidLayoutBytes("delete-boundary-prior"), poison="delete boundary poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==stage && !injected){
                if(afterEffect) CHECK(realDelete(deleted)!=0);
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(previous)==prior && ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(afterEffect ? !RawFileExists(stage) : RawFileExists(stage));
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(!RawFileExists(stage) && !RawFileExists(previous));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_staged_backup_restore_mismatch_never_discards_intended_bytes_on_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", restore=previous+L".restore", tempPath=primary+L".tmp";
    const std::string current=ValidLayoutBytes("current"), staged=ValidLayoutBytes("staged"),
        pending=ValidLayoutBytes("pending"), corrupted="corrupted restore readback";
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(previous,staged));
    LayoutFsOps corruptCopyOps;
    auto realCopy=corruptCopyOps.copyFile;
    bool corruptedCopy=false;
    corruptCopyOps.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        BOOL copied=realCopy(from,to,failIfExists);
        if(copied && from==previous && to==restore){
            corruptedCopy=true;
            CHECK(WriteRawFile(to,corrupted));
        }
        return copied;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,corruptCopyOps));
    CHECK(corruptedCopy && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(previous)==staged);
    CHECK(!RawFileExists(bak));

    LayoutFsOps stopAfterResolveOps;
    auto realOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,pending,&error,false,stopAfterResolveOps));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==staged);
    CHECK(!RawFileExists(previous));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
}

static void test_partial_effect_replace_failure_remains_recoverable(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"normal.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next"),
            changed=ValidLayoutBytes("changed"), older=ValidLayoutBytes("older");
        CHECK(WriteRawFile(primary,prior));
        CHECK(WriteRawFile(bak,older));
        LayoutFsOps ops;
        ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
                const std::wstring& backupName,DWORD)->BOOL{
            BOOL moved=MoveFileExW(replaced.c_str(),backupName.c_str(),
                MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            CHECK(moved!=0);
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
            return FALSE;
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(rollback)==prior);
        CHECK(ReadRawFile(bak)==older);
        CHECK(ReadRawFile(primary+L".tmp")==next);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered);
        CHECK(loaded.revision.sourcePath==rollback && LoadedDesktopName(loaded)=="prior");
        CHECK(AtomicWriteText(primary,changed,&error));
        CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(rollback) && !RawFileExists(primary+L".tmp"));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"recovery.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        const std::string corrupt="corrupt primary", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement");
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
                const std::wstring& backupName,DWORD)->BOOL{
            BOOL moved=MoveFileExW(replaced.c_str(),backupName.c_str(),
                MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            CHECK(moved!=0);
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
            return FALSE;
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(displaced)==corrupt);
        CHECK(ReadRawFile(bak)==good);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
}

static void test_first_write_preserves_existing_recovery_artifacts(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string recovery=ValidLayoutBytes("rollback"), older=ValidLayoutBytes("bak");
    const std::string created=ValidLayoutBytes("created");
    CHECK(WriteRawFile(rollback,recovery));
    CHECK(WriteRawFile(bak,older));
    std::string error;
    CHECK(AtomicWriteText(primary,created,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==created);
    CHECK(ReadRawFile(rollback)==recovery);
    CHECK(ReadRawFile(bak)==older);
}

static void test_failed_rollback_promotion_stays_recoverable_before_older_bak(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    std::string first=ValidLayoutBytes("first"), second=ValidLayoutBytes("second"), third=ValidLayoutBytes("third"), error;
    CHECK(AtomicWriteText(primary,first,&error));
    CHECK(AtomicWriteText(primary,second,&error));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return realMove(from,to,flags);
    };
    CHECK(!AtomicWriteText(primary,third,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==third);
    CHECK(ReadRawFile(rollback)==second);
    CHECK(ReadRawFile(bak)==first);
    CHECK(WriteRawFile(primary,"corrupt replacement"));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(LoadedDesktopName(loaded)=="second");
    CHECK(loaded.revision.sourcePath==rollback);
}

static void test_recovery_write_preserves_known_good_backup_and_reports_cleanup_failure(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        std::string good=ValidLayoutBytes("known-good"), replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(primary+L".displaced"));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string good=ValidLayoutBytes("known-good"), replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops; auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(RawFileExists(displaced));
        error="stale";
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string good=ValidLayoutBytes("known-good"), first=ValidLayoutBytes("first-retry"),
            changed=ValidLayoutBytes("changed-retry"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops; auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        CHECK(!AtomicWriteText(primary,first,&error,true,ops));
        CHECK(ReadRawFile(primary)==first && ReadRawFile(bak)==good && RawFileExists(displaced));
        CHECK(AtomicWriteText(primary,changed,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==changed && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string corrupt="corrupt primary", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced) return TRUE;
            return DeleteFileW(deleted.c_str());
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(ReadRawFile(displaced)==corrupt);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty() && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            displaced=primary+L".displaced", tempPath=primary+L".tmp.stage";
        std::string corrupt="corrupt displaced", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(displaced,corrupt)); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==tempPath && creation==CREATE_ALWAYS){
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(displaced)==corrupt && ReadRawFile(bak)==good);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
}

static void test_preserve_retry_without_named_recovery_converges(){
    for(int changedRetry=0;changedRetry<2;++changedRetry){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak", rollback=primary+L".rollback";
        const std::string old=ValidLayoutBytes("old"), first=ValidLayoutBytes("first"),
            changed=ValidLayoutBytes("changed");
        CHECK(WriteRawFile(primary,old));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,first,&error,true,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==first && ReadRawFile(displaced)==old);
        CHECK(!RawFileExists(bak) && !RawFileExists(rollback));
        const std::string& requested=changedRetry ? changed : first;
        CHECK(AtomicWriteText(primary,requested,&error,true));
        CHECK(error.empty() && ReadRawFile(primary)==requested && !RawFileExists(displaced));
        if(changedRetry) CHECK(ReadRawFile(bak)==old);
        else CHECK(!RawFileExists(bak));
    }

    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak", rollback=primary+L".rollback", committed=primary+L".tmp";
    const std::string old=ValidLayoutBytes("old-no-primary"), first=ValidLayoutBytes("first-no-primary"),
        changed=ValidLayoutBytes("changed-no-primary");
    CHECK(WriteRawFile(primary,old));
    LayoutFsOps ops;
    ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
            const std::wstring& backupName,DWORD)->BOOL{
        CHECK(MoveFileExW(replaced.c_str(),backupName.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0);
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return FALSE;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,first,&error,true,ops));
    CHECK(!error.empty() && !RawFileExists(primary) && ReadRawFile(displaced)==old);
    CHECK(ReadRawFile(committed)==first && !RawFileExists(bak) && !RawFileExists(rollback));
    CHECK(AtomicWriteText(primary,changed,&error,true));
    CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==old);
    CHECK(!RawFileExists(displaced) && !RawFileExists(committed));

    {
        LayoutTempDir direct;
        std::wstring missingPrimary=direct.file(L"layout.txt"),
            onlyDisplaced=missingPrimary+L".displaced", onlyBak=missingPrimary+L".bak";
        const std::string prior=ValidLayoutBytes("displaced-only-prior"),
            requested=ValidLayoutBytes("displaced-only-new");
        CHECK(WriteRawFile(onlyDisplaced,prior));
        CHECK(AtomicWriteText(missingPrimary,requested,&error,true));
        CHECK(error.empty() && ReadRawFile(missingPrimary)==requested && ReadRawFile(onlyBak)==prior);
        CHECK(!RawFileExists(onlyDisplaced));
    }

    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir interrupted;
        std::wstring retryPrimary=interrupted.file(L"layout.txt"),
            retryDisplaced=retryPrimary+L".displaced", retryBak=retryPrimary+L".bak";
        const std::string published=ValidLayoutBytes("published-before-retry"),
            prior=ValidLayoutBytes("prior-before-retry"), next=ValidLayoutBytes("next-retry");
        CHECK(WriteRawFile(retryPrimary,published)); CHECK(WriteRawFile(retryDisplaced,prior));
        LayoutFsOps moveOps;
        auto realMove=moveOps.moveFile;
        bool injected=false;
        moveOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==retryDisplaced && to==retryBak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        CHECK(!AtomicWriteText(retryPrimary,next,&error,true,moveOps));
        CHECK(injected && !error.empty() && ReadRawFile(retryPrimary)==published);
        CHECK(afterEffect ? ReadRawFile(retryBak)==prior : ReadRawFile(retryDisplaced)==prior);
        CHECK(AtomicWriteText(retryPrimary,next,&error,true));
        CHECK(error.empty() && ReadRawFile(retryPrimary)==next && ReadRawFile(retryBak)==prior);
        CHECK(!RawFileExists(retryDisplaced));
    }
}

static void test_displaced_restart_and_default_write_converge(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("restart-prior"),
            published=ValidLayoutBytes("restart-published");
        CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,published,&error,true,ops));
        CHECK(ReadRawFile(primary)==published && ReadRawFile(displaced)==prior && !RawFileExists(bak));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="restart-published");
        CHECK(ReadRawFile(primary)==published && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("default-prior"),
            published=ValidLayoutBytes("default-published"), next=ValidLayoutBytes("default-next");
        CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,published,&error,true,ops));
        CHECK(ReadRawFile(primary)==published && ReadRawFile(displaced)==prior);
        CHECK(AtomicWriteText(primary,next,&error,false));
        CHECK(error.empty() && ReadRawFile(primary)==next && ReadRawFile(bak)==published);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("sole-displaced-recovery");
        CHECK(WriteRawFile(displaced,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="sole-displaced-recovery");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(displaced) && !RawFileExists(primary));
    }
}

static void test_displaced_reconciliation_faults_retry_safely(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced";
        const std::string current=ValidLayoutBytes("delete-current"),
            prior=ValidLayoutBytes("delete-prior");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced && !injected){
                if(afterEffect) CHECK(realDelete(deleted)!=0);
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(primary)==current);
        CHECK(afterEffect ? !RawFileExists(displaced) : ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced";
        const std::string current=ValidLayoutBytes("transient-current"),
            prior=ValidLayoutBytes("transient-displaced");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        bool injected=false;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==displaced){
                injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && !RawFileExists(displaced));
    }
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("move-displaced");
        CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==displaced && to==bak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(afterEffect ? ReadRawFile(bak)==prior : ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed);
        CHECK(retry.revision.sourcePath==bak && ReadRawFile(bak)==prior && !RawFileExists(displaced));
    }
}

static void test_corrupt_primary_recovers_from_sole_valid_displaced(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak";
    const std::string corrupt="strictly corrupt primary", recovery=ValidLayoutBytes("displaced-recovery");
    CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
    CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="displaced-recovery");
    CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==recovery && !RawFileExists(displaced));
    std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
    CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==bak);
    CHECK(DiagnosticCopies(primary).size()==1);
}

static void test_corrupt_primary_displaced_faults_preserve_once_and_retry(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string corrupt="corrupt move primary", recovery=ValidLayoutBytes("move recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==displaced && to==bak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(primary)==corrupt);
        CHECK(afterEffect ? ReadRawFile(bak)==recovery : ReadRawFile(displaced)==recovery);
        std::vector<std::wstring> firstDiagnostics=DiagnosticCopies(primary);
        CHECK(firstDiagnostics.size()==1 && ReadRawFile(firstDiagnostics.front())==corrupt);

        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
        CHECK(retry.revision.sourcePath==bak && ReadRawFile(bak)==recovery && !RawFileExists(displaced));
        CHECK(DiagnosticCopies(primary).size()==1);
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string corrupt="corrupt transient primary", recovery=ValidLayoutBytes("transient recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int displacedOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==displaced && ++displacedOpens==2){
                SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(displacedOpens==2 && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(displaced)==recovery && !RawFileExists(bak));
        CHECK(DiagnosticCopies(primary).size()==1);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==bak);
        CHECK(ReadRawFile(bak)==recovery && !RawFileExists(displaced));
        CHECK(DiagnosticCopies(primary).size()==1);
    }
}

static void test_corrupt_displaced_is_preserved_but_never_recovered(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak";
    const std::string corruptPrimary="corrupt primary evidence",
        corruptDisplaced="corrupt displaced evidence";
    CHECK(WriteRawFile(primary,corruptPrimary)); CHECK(WriteRawFile(displaced,corruptDisplaced));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed && !loaded.usable());
    CHECK(ReadRawFile(primary)==corruptPrimary && !RawFileExists(displaced) && !RawFileExists(bak));
    std::vector<std::wstring> primaryDiagnostics=DiagnosticCopies(primary),
        displacedDiagnostics=DiagnosticCopies(displaced);
    CHECK(primaryDiagnostics.size()==1 && ReadRawFile(primaryDiagnostics.front())==corruptPrimary);
    CHECK(displacedDiagnostics.size()==1 && ReadRawFile(displacedDiagnostics.front())==corruptDisplaced);
    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::CorruptPreserved && retry.writesAllowed);
    CHECK(DiagnosticCopies(primary).size()==1 && DiagnosticCopies(displaced).size()==1);
}

static void test_same_revision_compares_every_field(){
    LayoutRevision base;
    base.sourcePath=L"a"; base.exists=true; base.size=12; base.mtime=34; base.contentHash=56;
    CHECK(SameRevision(base,base));
    LayoutRevision changed=base; changed.sourcePath=L"b"; CHECK(!SameRevision(base,changed));
    changed=base; changed.exists=false; CHECK(!SameRevision(base,changed));
    changed=base; changed.size=13; CHECK(!SameRevision(base,changed));
    changed=base; changed.mtime=35; CHECK(!SameRevision(base,changed));
    changed=base; changed.contentHash=57; CHECK(!SameRevision(base,changed));
}

static bool WriteIfCanonicalRevisionUnchanged(const std::wstring& primary,const LayoutRevision& expected,
        const std::string& bytes,bool preserveExistingBackup=false){
    ScopedLayoutLock lock(1000);
    if(!lock.acquired()) return false;
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    if(!SameRevision(current,expected)) return false;
    return AtomicWriteText(primary,bytes,nullptr,preserveExistingBackup);
}

static void test_two_actor_stale_save_is_rejected_without_overwrite(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    std::string first=ValidLayoutBytes("AAAA"), fromB=ValidLayoutBytes("BBBB"),
        staleA=ValidLayoutBytes("CCCC"), error;
    CHECK(first.size()==fromB.size() && first.size()==staleA.size());
    CHECK(AtomicWriteText(primary,first,&error));
    LayoutLoadResult actorA=LoadLayoutWithBackup(primary,1700000000);
    CHECK(actorA.status==LayoutLoadStatus::Valid);
    LayoutRevision directA=ReadLayoutRevisionLocked(primary);
    CHECK(actorA.revision.sourcePath==primary && directA.sourcePath==primary);
    CHECK(actorA.revision.exists && directA.exists);
    CHECK(actorA.revision.size==first.size() && directA.size==first.size());
    CHECK(actorA.revision.mtime==directA.mtime && actorA.revision.mtime!=0);
    CHECK(actorA.revision.contentHash==directA.contentHash && actorA.revision.contentHash!=0);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(primary,fromB,&error));
    }
    CHECK(SetRawFileMtime(primary,actorA.revision.mtime));
    LayoutRevision directB=ReadLayoutRevisionLocked(primary);
    CHECK(directB.sourcePath==actorA.revision.sourcePath);
    CHECK(directB.exists==actorA.revision.exists);
    CHECK(directB.size==actorA.revision.size);
    CHECK(directB.mtime==actorA.revision.mtime);
    CHECK(directB.contentHash!=actorA.revision.contentHash);
    CHECK(!SameRevision(directB,actorA.revision));
    CHECK(!WriteIfCanonicalRevisionUnchanged(primary,actorA.revision,staleA));
    CHECK(ReadRawFile(primary)==fromB);
}

static void test_two_actor_recovered_source_stale_save_is_rejected(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    std::string base=ValidLayoutBytes("recovery-base"), fromB=ValidLayoutBytes("B"),
        staleA=ValidLayoutBytes("A-stale"), error;
    CHECK(WriteRawFile(primary,"corrupt primary"));
    CHECK(WriteRawFile(bak,base));
    LayoutLoadResult actorA=LoadLayoutWithBackup(primary,1700000000);
    CHECK(actorA.status==LayoutLoadStatus::Recovered && actorA.revision.sourcePath==bak);
    LayoutRevision directRecovery=ReadLayoutRevisionLocked(bak);
    CHECK(actorA.revision.sourcePath==directRecovery.sourcePath);
    CHECK(actorA.revision.exists==directRecovery.exists && actorA.revision.exists);
    CHECK(actorA.revision.size==directRecovery.size && actorA.revision.size==base.size());
    CHECK(actorA.revision.mtime==directRecovery.mtime && actorA.revision.mtime!=0);
    CHECK(actorA.revision.contentHash==directRecovery.contentHash && actorA.revision.contentHash!=0);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(primary,fromB,&error,true));
    }
    CHECK(ReadRawFile(bak)==base);
    LayoutRevision canonicalAfterB=ReadLayoutRevisionLocked(primary);
    CHECK(canonicalAfterB.sourcePath==primary && canonicalAfterB.exists);
    CHECK(canonicalAfterB.size==fromB.size() && canonicalAfterB.mtime!=0 &&
        canonicalAfterB.contentHash!=0);
    CHECK(!SameRevision(actorA.revision,canonicalAfterB));
    CHECK(!WriteIfCanonicalRevisionUnchanged(primary,actorA.revision,staleA,true));
    CHECK(ReadRawFile(primary)==fromB);

    LayoutTempDir rollbackTemp;
    std::wstring rollbackPrimary=rollbackTemp.file(L"layout.txt"), rollback=rollbackPrimary+L".rollback";
    CHECK(WriteRawFile(rollbackPrimary,"corrupt primary"));
    CHECK(WriteRawFile(rollback,base));
    LayoutLoadResult rollbackActorA=LoadLayoutWithBackup(rollbackPrimary,1700000000);
    CHECK(rollbackActorA.status==LayoutLoadStatus::Recovered && rollbackActorA.revision.sourcePath==rollback);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(rollbackPrimary,fromB,&error,true));
    }
    CHECK(ReadRawFile(rollback)==base);
    CHECK(!WriteIfCanonicalRevisionUnchanged(rollbackPrimary,rollbackActorA.revision,staleA,true));
    CHECK(ReadRawFile(rollbackPrimary)==fromB);
}

struct LayoutLockThreadContext {
    HANDLE ready=nullptr;
    HANDLE release=nullptr;
    bool acquired=false;
};

static DWORD WINAPI HoldLayoutLockThread(void* opaque){
    LayoutLockThreadContext* context=(LayoutLockThreadContext*)opaque;
    ScopedLayoutLock lock(5000);
    context->acquired=lock.acquired();
    SetEvent(context->ready);
    if(lock.acquired()) WaitForSingleObject(context->release,5000);
    return 0;
}

struct AbandonedLayoutLockContext {
    HANDLE mutex=nullptr;
    HANDLE ready=nullptr;
    bool acquired=false;
};

static DWORD WINAPI AbandonLayoutLockThread(void* opaque){
    AbandonedLayoutLockContext* context=(AbandonedLayoutLockContext*)opaque;
    context->acquired=WaitForSingleObject(context->mutex,5000)==WAIT_OBJECT_0;
    SetEvent(context->ready);
    return 0; // Deliberately exits while owning the mutex.
}

static void test_layout_mutex_zero_timeout_and_acquisition_after_release(){
    LayoutLockThreadContext context;
    context.ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    context.release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(context.ready!=nullptr && context.release!=nullptr);
    HANDLE thread=CreateThread(nullptr,0,HoldLayoutLockThread,&context,0,nullptr);
    CHECK(thread!=nullptr);
    CHECK(WaitForSingleObject(context.ready,5000)==WAIT_OBJECT_0);
    CHECK(context.acquired);
    {
        ScopedLayoutLock blocked(0);
        CHECK(!blocked.acquired());
    }
    DWORD handlesBefore=0, handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<64;++attempt){
        ScopedLayoutLock blocked(0);
        CHECK(!blocked.acquired());
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
    CHECK(SetEvent(context.release)!=0);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(thread)!=0);
    CHECK(CloseHandle(context.ready)!=0);
    CHECK(CloseHandle(context.release)!=0);
    ScopedLayoutLock afterRelease(1000);
    CHECK(afterRelease.acquired());
}


static void test_layout_mutex_treats_abandoned_as_acquired(){
    HANDLE keeper=CreateMutexW(nullptr,FALSE,L"Local\\win-vde.layout-store.v1");
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(keeper!=nullptr && ready!=nullptr);
    AbandonedLayoutLockContext context;
    context.mutex=keeper; context.ready=ready;
    HANDLE thread=CreateThread(nullptr,0,AbandonLayoutLockThread,&context,0,nullptr);
    CHECK(thread!=nullptr);
    CHECK(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);
    CHECK(context.acquired);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(thread)!=0);
    {
        ScopedLayoutLock recovered(1000);
        CHECK(recovered.acquired());
    }
    CHECK(CloseHandle(ready)!=0);
    CHECK(CloseHandle(keeper)!=0);
}

static void test_layout_fixture_removes_only_its_unique_tree(){
    std::wstring fixturePath;
    {
        LayoutTempDir temp;
        fixturePath=temp.path;
        CHECK(WriteRawFile(temp.file(L"artifact.tmp"),"temporary"));
    }
    DWORD attributes=GetFileAttributesW(fixturePath.c_str());
    DWORD error=GetLastError();
    CHECK(attributes==INVALID_FILE_ATTRIBUTES);
    CHECK(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND);
}

int main(){
    test_etld1();
    test_b64();
    test_b64_long_roundtrip();
    test_strict_integer_parsing();
    test_strict_base64_parsing();
    test_strict_counts_parsing();
    test_snss_parse();
    test_snss_garbage();
    test_snss_truncated_frame_returns_no_partial_windows();
    test_mozlz4_rejects_huge_declared_output();
    test_session_stamp_detects_change();
    test_firefox_json_rejects_trailing_and_excessive_depth();
    test_firefox_json_rejects_malformed_unicode_numbers_and_controls();
    test_firefox_json_depth_node_and_string_budget_boundaries();
    test_browser_parser_default_limits_are_exact();
    test_snss_rejects_zero_trailing_and_malformed_known_commands();
    test_snss_unique_id_and_command_cap_boundaries();
    test_snss_window_and_tab_maps_have_independent_exact_caps();
    test_snss_navigation_duplicate_and_search_budget_boundaries();
    test_snss_per_window_and_global_text_caps_are_exact();
    test_mozlz4_exact_decode_and_limit_boundaries();
    test_mozlz4_rejects_malformed_blocks_transactionally();
    test_lz4_match_offset_and_extension_arithmetic_edges();
    test_get_session_stamp_accepts_exact_cap_and_rejects_over();
    test_session_stamp_detects_equal_metadata_replace_and_in_place_rewrite();
    test_firefox_profile_ini_default_release_fallback();
    test_firefox_json_valid_empty_is_distinct_from_failure();
    test_firefox_selected_index_rejects_int_min_without_overflow();
    test_chromium_resolver_tracks_rotation_and_breaks_stamp_ties();
    test_already_posted_session_results_are_drained_and_freed();
    test_session_status_and_acceptance_policy_contract();
    test_session_cache_shares_payload_and_rejects_oversize();
    test_session_worker_valid_empty_is_fresh_and_cache_hit_is_shared();
    test_session_worker_malformed_cold_is_unavailable();
    test_session_worker_non_ok_reads_never_parse_and_publish_current_stamp();
    test_session_worker_disappeared_source_is_not_reported_as_current();
    test_session_worker_stamp_change_uses_exact_path_cached_stale();
    test_session_worker_rotation_during_parse_is_never_fresh();
    test_session_worker_equal_metadata_replacement_never_publishes_old_bytes_fresh();
    test_session_worker_rejects_aba_bytes_without_matching_handle_stamp();
    test_session_worker_rotation_uses_only_exact_attempted_path_cache();
    test_session_worker_ten_rapid_requests_are_active_plus_newest_pending();
    test_session_worker_low_probe_cannot_replace_user_pending();
    test_session_worker_cross_app_manual_preempts_pending_metadata();
    test_session_worker_rejects_unsupported_app_queue_amplification();
    test_session_coordinator_preserves_purpose_and_shared_payload_identity();
    test_session_coordinator_rejects_old_generation_profile_purpose_and_request();
    test_session_profile_comparison_covers_every_config_field();
    test_session_coordinator_superseded_only_releases_bookkeeping();
    test_session_coordinator_request_faults_are_transactional();
    test_session_coordinator_accept_faults_preserve_pending_and_latest();
    test_posted_session_result_is_owned_immediately_on_rejection();
    test_unavailable_defer_is_once_per_current_source_and_preserves_bytes();
    test_session_data_generation_is_per_app_and_hash_breaks_stamp_ties();
    test_session_data_generation_is_monotonic_when_historical_cache_returns();
    test_session_data_generation_saturates_without_zero_or_rollback();
    test_session_cache_enforces_sixteen_entry_lru_cap();
    test_session_cache_byte_cap_counts_external_ui_payload();
    test_post_message_failure_deletes_heap_result();
    test_session_worker_oversized_payload_is_unavailable();
    test_session_worker_stop_joins_and_suppresses_unposted_completion();
    test_session_worker_stop_waits_for_inflight_superseded_post();
    test_session_worker_reentrant_requester_poster_stop_completes();
    test_session_worker_reentrant_worker_poster_stop_defers_self_join();
    test_session_worker_reentrant_stop_waits_for_confirmed_worker_post_path();
    test_session_worker_concurrent_external_and_reentrant_stop_do_not_cycle();
    test_session_worker_reentrant_worker_stop_survives_repeated_destruction();
    test_worker_retained_budget_includes_posted_ui_ownership();
    test_failed_cache_replacement_preserves_exact_stale_payload();
    test_session_cache_put_is_strongly_transactional_at_every_fault_step();
    test_session_cache_put_preserves_output_on_capacity_and_budget_rejection();
    test_session_worker_contains_internal_allocation_faults_and_continues();
    test_session_worker_result_allocation_failure_drops_only_that_request();
    test_session_worker_result_prepare_fault_drops_unidentified_result_and_continues();
    test_session_worker_superseded_result_factory_is_not_called_under_state_lock();
    test_session_worker_request_fault_preserves_existing_pending();
    test_session_cache_lookup_fault_preserves_output_and_lru();
    test_session_cache_runtime_fault_is_transactional();
    test_post_message_exception_deletes_heap_result();
    test_layout_serializes_v4_header();
    test_layout_roundtrip_v4();
    test_layout_parse_v2();
    test_layout_rejects_invalid_desktop_guid_transactionally();
    test_layout_rejects_progid_as_desktop_guid();
    test_layout_rejects_integer_trailing_junk_transactionally();
    test_layout_migrates_v3_record();
    test_layout_rejects_negative_v3_missing_counter_transactionally();
    test_layout_legacy_migration_rejects_generated_id_collision_transactionally();
    test_layout_legacy_migration_rejects_invalid_generated_ids_transactionally();
    test_layout_rejects_embedded_carriage_returns_transactionally();
    test_layout_rejects_trailing_columns();
    test_layout_rejects_duplicate_record_ids();
    test_layout_enforces_total_record_cap_transactionally();
    test_retention_expiration_boundaries();
    test_retention_future_and_zero_missing_are_not_expired();
    test_retention_mark_seen_clears_missing_and_updates_last_seen();
    test_retention_mark_missing_uses_last_seen_and_is_idempotent();
    test_retention_prune_preserves_order_duplicates_and_input();
    test_layout_score_formula_and_fallback();
    test_layout_score_browser_symmetry_and_cross_app_rejection();
    test_layout_score_identical_two_domain_is_exact();
    test_match_one_to_one_duplicate_fingerprints_are_unique();
    test_match_one_to_one_browser_apps_and_never_crosses_apps();
    test_match_one_to_one_score_evaluation_budget();
    test_assignment_maximizes_cardinality_before_score();
    test_assignment_maximizes_total_score_at_same_cardinality();
    test_assignment_ties_are_deterministic_across_input_order();
    test_assignment_filters_and_deduplicates_without_mutating_input();
    test_assignment_randomized_against_exhaustive_oracle();
    test_assignment_candidate_cap_direct_and_generated();
    test_assignment_flow_work_budget_rejects_connected_cycle();
    test_assignment_checked_score_scaling_boundary();
    test_checked_snapshot_enforces_combined_record_cap();
    test_checked_snapshot_rejects_zero_desktop_record_transactionally();
    test_checked_snapshot_rejects_malformed_record_id_transactionally();
    test_checked_snapshot_rejects_zero_record_id_transactionally();
    test_checked_snapshot_rejects_duplicate_record_ids_transactionally();
    test_checked_snapshot_rejects_generated_id_collision_transactionally();
    test_checked_snapshot_rejects_negative_missing_since_transactionally();
    test_checked_snapshot_rejects_negative_missing_bridge_clock_transactionally();
    test_checked_snapshot_rejects_zero_missing_bridge_clock_transactionally();
    test_checked_snapshot_rejects_negative_transitional_missing_runs_transactionally();
    test_checked_snapshot_accepts_supported_browser_apps();
    test_checked_snapshot_rejects_unsupported_app_transactionally();
    test_checked_snapshot_rejects_negative_tab_count_transactionally();
    test_checked_snapshot_rejects_invalid_counts_transactionally();
    test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally();
    test_auto_snapshot_build_rejects_invalid_existing_bytes_transactionally();
    test_auto_snapshot_build_forwards_generator_to_legacy_migration();
    test_auto_snapshot_build_allows_missing_existing_file();
    test_prepare_transitional_records_roundtrip_and_keep_stable_id();
    test_prepare_transitional_records_rejects_zero_desktop_transactionally();
    test_prepare_transitional_records_rejects_negative_last_seen_transactionally();
    test_prepare_transitional_records_id_failure_keeps_prior_bytes();
    test_fingerprint_key_generic_vs_domain();
    test_lc_initial_absence_marks_missing_once();
    test_lc_two_stable_present_snapshots_begin_restore();
    test_lc_stale_restore_completion_is_ignored();
    test_restore_budget_is_exact_keyed();
    test_lc_timeout_is_per_wave_and_survives_clock_rollback();
    test_lc_absence_transitions_mark_once_and_reappearance_rearms();
    test_lc_exhausted_generation_suppresses_missing_action();
    test_lc_absence_during_flight_clears_rearm_if_still_absent();
    test_lc_absence_reappearance_during_flight_queues_one_wave();
    test_lc_firefox_chrome_edge_states_are_independent();
    test_lc_layout_change_saves_but_restore_inputs_restore_first();
    test_lc_same_hwnd_new_fresh_session_starts_one_wave();
    test_lc_inflight_changes_queue_exactly_one_latest_rearm();
    test_lc_late_and_returning_sibling_each_start_one_wave();
    test_lc_generation_max_is_issued_once_then_fails_closed();
    test_lc_deferred_retries_three_times_with_exact_backoff();
    test_lc_deferred_key_change_resets_for_window_or_session();
    test_lc_deferred_key_change_during_backoff_restarts_settle_now();
    test_lc_inflight_a_to_b_to_a_history_rearms_deferred_wave();
    test_lc_deferred_backoff_rebases_after_clock_rollback();
    test_lc_deferred_ignores_alternating_completion_session_payload();
    test_lc_all_completion_outcomes_honor_one_queued_rearm();
    test_lc_exhausted_records_actual_layout_without_save_loop();
    test_lc_explicit_save_completion_is_generation_safe();
    test_restore_budgets_isolate_siblings_runtime_and_destination();
    test_restore_budgets_prune_only_dead_runtime_identities();
    test_restore_budgets_cap_uses_deterministic_touch_lru();
    test_bounded_read_exact_limit_and_preallocation_rejection();
    test_session_bounded_reader_binds_bytes_to_exact_aba_handle();
    test_session_bounded_reader_rejects_handle_changes_and_close_failure();
    test_bounded_read_failures_are_transactional_and_status_bearing();
    test_bounded_read_denies_concurrent_in_place_writer();
    test_layout_load_missing_and_valid_primary();
    test_layout_recovery_prefers_valid_rollback_and_preserves_primary();
    test_layout_recovery_uses_bak_and_preserves_all_corruption();
    test_two_corrupt_streams_require_verified_diagnostics();
    test_transient_primary_open_blocks_backup_recovery();
    test_oversized_primary_blocks_all_recovery_without_mutation();
    test_missing_primary_uses_rollback_priority_then_backup_fallback();
    test_transient_and_corrupt_recovery_states();
    test_diagnostic_copy_failure_and_readback_mismatch_block_writes();
    test_second_diagnostic_copy_failure_and_collision_never_lose_evidence();
    test_diagnostic_reuse_never_deletes_changed_corrupt_temp();
    test_diagnostic_reuse_never_overwrites_changed_corrupt_backup();
    test_diagnostic_preservation_revalidates_primary_before_backup_recovery();
    test_diagnostic_preservation_revalidates_primary_before_displaced_recovery();
    test_fresh_diagnostic_copy_revalidates_primary_before_recovery();
    test_diagnostic_source_reverify_transient_failure_retries_without_growth();
    test_atomic_write_first_and_two_successful_writes();
    test_atomic_write_rejects_oversize_without_touching_destination();
    test_atomic_write_faults_keep_old_or_recovery_bytes();
    test_preexisting_rollback_promotion_failure_never_touches_primary();
    test_rollback_promotion_readback_failure_restores_old_bak_and_rollback();
    test_preexisting_rollback_promotion_mismatch_recovers_from_verified_marker();
    test_post_replace_verification_failure_reports_false_with_bak_readable();
    test_late_normal_write_failure_retains_staged_old_backup();
    test_retry_after_publish_finishes_pending_promotion_without_self_replace();
    test_replace_failure_retains_staged_old_backup_until_retry();
    test_staged_backup_resolver_restores_missing_backup_before_cleanup();
    test_changed_request_first_reconciles_pending_normal_transaction();
    test_failed_backup_staging_readback_does_not_poison_retries();
    test_pending_promotion_marker_recovers_post_move_verification_fault();
    test_promotion_marker_false_after_poison_is_never_authoritative();
    test_transient_promoted_backup_read_is_retained_and_loads_before_retry();
    test_loader_recovers_valid_authoritative_internal_marker();
    test_loader_never_accepts_stale_bak_beside_authoritative_promotion_marker();
    test_valid_primary_reconciles_authoritative_promotion_marker();
    test_valid_primary_marker_faults_fail_closed_then_converge();
    test_valid_primary_retries_cleanup_after_trigger_delete_false_effect();
    test_first_publish_tmp_recovers_on_restart_after_move_failure();
    test_partial_valid_prefix_is_never_treated_as_committed_first_publish();
    test_temporary_commit_move_failure_converges_before_and_after_effect();
    test_idempotent_retry_cleans_orphan_prior_backup_stage();
    test_orphan_promotion_stage_is_non_authoritative_and_converges();
    test_non_authoritative_stages_are_discarded_without_reading_bytes();
    test_non_authoritative_stage_faults_fail_closed_and_retry();
    test_previous_stage_cleanup_reverifies_before_consuming_previous();
    test_staged_backup_restore_mismatch_never_discards_intended_bytes_on_retry();
    test_partial_effect_replace_failure_remains_recoverable();
    test_first_write_preserves_existing_recovery_artifacts();
    test_failed_rollback_promotion_stays_recoverable_before_older_bak();
    test_recovery_write_preserves_known_good_backup_and_reports_cleanup_failure();
    test_preserve_retry_without_named_recovery_converges();
    test_displaced_restart_and_default_write_converge();
    test_displaced_reconciliation_faults_retry_safely();
    test_corrupt_primary_recovers_from_sole_valid_displaced();
    test_corrupt_primary_displaced_faults_preserve_once_and_retry();
    test_corrupt_displaced_is_preserved_but_never_recovered();
    test_same_revision_compares_every_field();
    test_two_actor_stale_save_is_rejected_without_overwrite();
    test_two_actor_recovered_source_stale_save_is_rejected();
    test_layout_mutex_zero_timeout_and_acquisition_after_release();
    test_layout_mutex_treats_abandoned_as_acquired();
    test_layout_fixture_removes_only_its_unique_tree();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
