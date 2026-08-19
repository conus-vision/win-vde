// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include "layout.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
#include "str_util.hpp"
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
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
