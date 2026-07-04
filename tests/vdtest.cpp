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
    std::string s = "PR #42 \xE2\x80\x94 Mozilla";   // includes a UTF-8 em dash
    CHECK(b64dec(b64enc(s)) == s);
}

static GUID G(const wchar_t* s){ GUID g{}; StringToGuid(s, g); return g; }

static void test_layout_roundtrip_v3(){
    std::vector<DeskRec> d; DeskRec d0; d0.index=0; d0.guid=G(L"{231A0000-0000-0000-0000-000000000001}"); d0.name=L"Work"; d.push_back(d0);
    std::vector<LayoutWin> w; LayoutWin w0;
    w0.app="firefox"; w0.deskIndex=0; w0.desktop=d0.guid; w0.activeTitle="PR #42";
    w0.activeDomain="github.com"; w0.tabCount=5; w0.counts={{"github.com",4},{"docs.python.org",1}}; w0.missingRuns=2;
    w.push_back(w0);

    std::string s = SerializeLayout(d, w);
    std::vector<DeskRec> d2; std::vector<LayoutWin> w2;
    CHECK(ParseLayout(s, d2, w2));
    CHECK(d2.size()==1); CHECK(w2.size()==1);
    CHECK(w2[0].app=="firefox"); CHECK(w2[0].deskIndex==0); CHECK(w2[0].activeTitle=="PR #42");
    CHECK(w2[0].activeDomain=="github.com"); CHECK(w2[0].tabCount==5); CHECK(w2[0].missingRuns==2);
    CHECK(w2[0].counts["github.com"]==4); CHECK(w2[0].counts["docs.python.org"]==1);
    CHECK(GuidEq(w2[0].desktop, d0.guid));
}
static void test_layout_parse_v2(){
    std::string v2 = "# VDE snapshot v2\n"
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Work") + "\n"
        "W\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("PR #42") + "\tgithub.com\t5\tgithub.com:4,docs.python.org:1\n";
    std::vector<DeskRec> d; std::vector<LayoutWin> w;
    CHECK(ParseLayout(v2, d, w));
    CHECK(w.size()==1); CHECK(w[0].app=="firefox"); CHECK(w[0].missingRuns==0);
    CHECK(w[0].tabCount==5); CHECK(w[0].counts["docs.python.org"]==1);
}

static LayoutWin LW(const char* app, int desk, std::map<std::string,int> c, const char* title=""){
    LayoutWin w; w.app=app; w.deskIndex=desk; w.counts=c; w.activeTitle=title; return w;
}
static void test_merge_upsert_and_keep(){
    std::vector<LayoutWin> existing = { LW("firefox",0,{{"github.com",3}}), LW("firefox",1,{{"jira.com",2}}) };
    existing[0].missingRuns=1; existing[1].missingRuns=1;
    std::vector<LayoutWin> present = { LW("firefox",2,{{"github.com",3}}) };  // only github present, moved to desk 2
    auto merged = MergeAutoLayout(existing, present);
    CHECK(merged.size()==2);                       // jira window kept (not wiped)
    int gi=-1, ji=-1;
    for(int i=0;i<(int)merged.size();++i){ if(merged[i].counts.count("github.com"))gi=i; if(merged[i].counts.count("jira.com"))ji=i; }
    CHECK(gi>=0 && ji>=0);
    CHECK(merged[gi].deskIndex==2); CHECK(merged[gi].missingRuns==0);  // present ⇒ updated + reset
    CHECK(merged[ji].deskIndex==1); CHECK(merged[ji].missingRuns==1);  // absent ⇒ untouched
}
static void test_merge_adds_new(){
    std::vector<LayoutWin> existing = {};
    std::vector<LayoutWin> present = { LW("firefox",0,{{"x.com",1}}) };
    auto merged = MergeAutoLayout(existing, present);
    CHECK(merged.size()==1); CHECK(merged[0].missingRuns==0);
}
static void test_fingerprint_key_generic_vs_domain(){
    CHECK(FingerprintKey("firefox",{{"a.com",2}},"T") == FingerprintKey("firefox",{{"a.com",2}},"OTHER"));
    CHECK(FingerprintKey("explorer",{},"Downloads") != FingerprintKey("explorer",{},"Documents"));
}

static void test_grace_seen_resets_unseen_increments(){
    std::vector<LayoutWin> recs = { LW("firefox",0,{{"a.com",1}}), LW("firefox",1,{{"b.com",1}}) };
    recs[0].missingRuns=2; recs[1].missingRuns=0;
    std::set<std::string> seen = { FingerprintKey("firefox",{{"a.com",1}},"") };  // only a.com seen
    std::set<std::string> apps = { "firefox" };
    auto out = ReconcileGrace(recs, seen, apps, MISSING_RUNS_MAX);
    CHECK(out.size()==2);
    int ai=-1,bi=-1; for(int i=0;i<(int)out.size();++i){ if(out[i].counts.count("a.com"))ai=i; if(out[i].counts.count("b.com"))bi=i; }
    CHECK(out[ai].missingRuns==0); CHECK(out[bi].missingRuns==1);
}
static void test_grace_drops_at_threshold(){
    std::vector<LayoutWin> recs = { LW("firefox",0,{{"b.com",1}}) };
    recs[0].missingRuns = MISSING_RUNS_MAX - 1;                 // 2 -> 3 ⇒ dropped
    auto out = ReconcileGrace(recs, {}, {"firefox"}, MISSING_RUNS_MAX);
    CHECK(out.size()==0);
}
static void test_grace_untouched_when_app_not_observed(){
    std::vector<LayoutWin> recs = { LW("chrome",0,{{"c.com",1}}) };
    recs[0].missingRuns = 2;
    auto out = ReconcileGrace(recs, {}, {"firefox"} /*chrome not observed*/, MISSING_RUNS_MAX);
    CHECK(out.size()==1); CHECK(out[0].missingRuns==2);
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
    CHECK(w[wi11].tabCount==1); CHECK(w[wi11].activeTitle=="Example");
}
static void test_snss_garbage(){ auto w=ParseChromiumSNSS("not an snss file...."); CHECK(w.empty()); }

int main(){
    test_etld1();
    test_b64();
    test_snss_parse();
    test_snss_garbage();
    test_layout_roundtrip_v3();
    test_layout_parse_v2();
    test_merge_upsert_and_keep();
    test_merge_adds_new();
    test_fingerprint_key_generic_vs_domain();
    test_grace_seen_resets_unseen_increments();
    test_grace_drops_at_threshold();
    test_grace_untouched_when_app_not_observed();
    test_lc_startup_present_restores();
    test_lc_startup_absent_none();
    test_lc_launch_then_settle_restores_once();
    test_lc_absent_does_not_wipe_and_rearm();
    test_lc_exit();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
