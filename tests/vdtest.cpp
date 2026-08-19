// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include <cstdio>
#include "str_util.hpp"
#include "layout.hpp"
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
static std::string FailingRecordIdGenerator(){ return std::string(); }
static std::string ConstantRecordIdGenerator(){ return "{00000000-0000-0000-0000-000000000099}"; }

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
    test_layout_rejects_trailing_columns();
    test_layout_rejects_duplicate_record_ids();
    test_layout_enforces_total_record_cap_transactionally();
    test_checked_snapshot_enforces_combined_record_cap();
    test_checked_snapshot_rejects_zero_desktop_record_transactionally();
    test_checked_snapshot_rejects_malformed_record_id_transactionally();
    test_checked_snapshot_rejects_zero_record_id_transactionally();
    test_checked_snapshot_rejects_duplicate_record_ids_transactionally();
    test_checked_snapshot_rejects_generated_id_collision_transactionally();
    test_checked_snapshot_rejects_negative_missing_since_transactionally();
    test_checked_snapshot_rejects_negative_missing_bridge_clock_transactionally();
    test_checked_snapshot_accepts_supported_browser_apps();
    test_checked_snapshot_rejects_unsupported_app_transactionally();
    test_checked_snapshot_rejects_negative_tab_count_transactionally();
    test_checked_snapshot_rejects_invalid_counts_transactionally();
    test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally();
    test_auto_snapshot_build_rejects_invalid_existing_bytes_transactionally();
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
