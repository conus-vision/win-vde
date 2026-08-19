// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include "str_util.hpp"
#include "layout.hpp"
#include "layout_store.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "lifecycle.hpp"
#include "session.hpp"

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

static void test_lc_startup_present_restores(){
    LcState s; CHECK(LcOnStartup(s, true) == LcAction::StartupRestore);
    CHECK(s.prevPresent && s.restoredThisAppearance);
}
static void test_lc_startup_absent_none(){
    LcState s; CHECK(LcOnStartup(s, false) == LcAction::None); CHECK(!s.prevPresent);
}
static void test_lc_launch_then_settle_restores_once(){
    LcState s; LcOnStartup(s, false);
    CHECK(LcOnTick(s, true, 4) == LcAction::None);      // appearance tick 1
    CHECK(LcOnTick(s, true, 4) == LcAction::None);      // 2
    CHECK(LcOnTick(s, true, 4) == LcAction::None);      // 3
    CHECK(LcOnTick(s, true, 4) == LcAction::DoRestore); // 4 ⇒ settle reached
    CHECK(LcOnTick(s, true, 4) == LcAction::AutoSave);  // then periodic autosave
}
static void test_lc_absent_does_not_wipe_and_rearm(){
    LcState s; LcOnStartup(s, true);
    CHECK(LcOnTick(s, false, 4) == LcAction::None);     // present->absent: no wipe
    CHECK(!s.restoredThisAppearance);                  // re-armed
    CHECK(LcOnTick(s, true, 4) == LcAction::None);      // reappearance restarts settle
}
static void test_lc_exit(){
    LcState s; CHECK(LcOnExit(s, true) == LcAction::FinalSave);
    CHECK(LcOnExit(s, false) == LcAction::None);
}

// --- minimal SNSS encoder mirroring the REAL format ---
// cmds 0/2/7/8 = raw fixed structs of two int32 (no pickle header); cmd 6 = base::Pickle
// (4-byte-aligned fields; url = UTF-8 WriteString, title = UTF-16 WriteString16).
static void wInt(std::string& p,int v){ for(int i=0;i<4;i++)p.push_back((char)((v>>(8*i))&0xFF)); }   // raw int32
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
    auto w = ParseChromiumSNSS(makeSnss());
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
static void test_snss_garbage(){ auto w=ParseChromiumSNSS("not an snss file...."); CHECK(w.empty()); }

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
    test_lc_startup_present_restores();
    test_lc_startup_absent_none();
    test_lc_launch_then_settle_restores_once();
    test_lc_absent_does_not_wipe_and_rearm();
    test_lc_exit();
    test_bounded_read_exact_limit_and_preallocation_rejection();
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
    test_staged_backup_restore_mismatch_never_discards_intended_bytes_on_retry();
    test_partial_effect_replace_failure_remains_recoverable();
    test_first_write_preserves_existing_recovery_artifacts();
    test_failed_rollback_promotion_stays_recoverable_before_older_bak();
    test_recovery_write_preserves_known_good_backup_and_reports_cleanup_failure();
    test_preserve_retry_without_named_recovery_converges();
    test_same_revision_compares_every_field();
    test_two_actor_stale_save_is_rejected_without_overwrite();
    test_two_actor_recovered_source_stale_save_is_rejected();
    test_layout_mutex_zero_timeout_and_acquisition_after_release();
    test_layout_mutex_treats_abandoned_as_acquired();
    test_layout_fixture_removes_only_its_unique_tree();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
