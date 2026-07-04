# win-vde Phase 1 (Core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Firefox layout save/restore automatic and wipe-proof (grace period), add lifecycle auto-restore, relabel the tray menu, add About and a Windows-update "compatibility" dialog, add an autostart toggle, and stand up the public repo scaffolding.

**Architecture:** Extract the pure, file/COM-free logic (string utils, sessionstore parsing, fingerprint scoring, layout serialization, grace/merge, lifecycle state machine) out of the monolithic `vde.cpp` into headers so they can be unit-tested with a tiny `cl.exe`-built test harness. Keep all Win32/COM/GUI glue in `vde.cpp`, calling into the tested logic. The monitor timer drives a testable lifecycle state machine that decides when to restore/save.

**Tech Stack:** C++14, Win32/GDI, undocumented ImmersiveShell COM, MSVC (`cl.exe` 19.44 / VS 2022 Professional via `vcvars64.bat`), `rc.exe` for resources. No third-party libraries.

## Global Constraints

- Language/std: `cl /utf-8 /EHsc /W3 /std:c++14` (copied from `vde.cpp` build header).
- No third-party dependencies; only Win32 SDK libs already declared via `#pragma comment(lib, ...)`.
- Data dir: `%LOCALAPPDATA%\VirtualDesktopsExtention\`. Files: `layout-auto.txt` (auto, merged), `layout-manual.txt` (manual, full overwrite). Migrate legacy `layout.txt` → `layout-auto.txt`.
- Constants (exact): `MISSING_RUNS_MAX = 3`, `MONITOR_INTERVAL_MS = 5000`, `STARTUP_SETTLE_MS = 2500`, `LAUNCH_SETTLE_MS = 20000` (⇒ `LAUNCH_SETTLE_TICKS = 4`).
- Layout file format tag: `# VDE snapshot v3`; reader must also accept `v2` (app⇒`"firefox"`, `missingRuns⇒0`).
- Author/contacts (copy verbatim): **Volodymyr Moskvin**, **info@conus.vision**, **https://github.com/conus-vision/win-vde**. License: **MIT**, © 2026 Volodymyr Moskvin.
- UI strings in English; comments may be Russian (source is `/utf-8`).
- Toolchain path (this machine): `C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat`.
- Header-moved free functions must be marked `inline` (they live in headers included by both `vde.cpp` and the test exe).

---

## File Structure

```
win-vde/
  src/
    vde.cpp              # Win32/COM/GUI glue only (includes the headers below)
    str_util.hpp         # W2U8,U82W,b64enc/dec,hostOf,etld1,GUID helpers, DWORD GetWindowsBuild
    session_firefox.hpp  # mozLz4, JValue/JParser, SSWindow, extractWindows, ReadSessionstore, profile lookup
    fingerprint.hpp      # Fp, Score
    layout.hpp           # DeskRec, LayoutWin, FingerprintKey, SerializeLayout/ParseLayout, MergeAutoLayout, ReconcileGrace, constants
    lifecycle.hpp        # LcState, LcAction, LcOnStartup/LcOnTick/LcOnExit
    vde.rc               # + manifest resource line
    vde.ico
    vde.exe.manifest     # Common Controls v6
  tests/
    vdtest.cpp           # tiny CHECK-macro harness; includes the headers, tests pure logic
  build.bat              # vcvars + rc + cl  → build\vde.exe
  build-test.bat         # vcvars + cl tests\vdtest.cpp → build\vdtest.exe && run
  .gitignore             # build/  *.obj *.exe *.res .vs/
  LICENSE                # MIT
  README.md
```

The existing top-level `vde.cpp`, `vde.rc`, `vde.ico` are moved into `src/` in Task 1. Legacy siblings (`vdmgr.cpp`, `vdpick.cpp`, `vdtest_test.cpp`, `vde - Copy*.cpp`, prebuilt `*.exe/*.obj/*.res`) are **not** copied into the repo.

---

## Task 1: Repo scaffolding, manifest, build scripts

**Files:**
- Create: `win-vde/src/vde.cpp` (copy of `F:\_VDESKTOP_FF\vde.cpp`), `win-vde/src/vde.rc`, `win-vde/src/vde.ico`
- Create: `win-vde/src/vde.exe.manifest`, `win-vde/build.bat`, `win-vde/.gitignore`, `win-vde/LICENSE`, `win-vde/README.md`

**Interfaces:**
- Produces: a buildable repo tree; `build.bat` emits `build\vde.exe`.

- [ ] **Step 1: Copy sources into `src/`**

Copy `F:\_VDESKTOP_FF\vde.cpp` → `win-vde\src\vde.cpp`, `vde.rc` → `win-vde\src\vde.rc`, `vde.ico` → `win-vde\src\vde.ico`.

- [ ] **Step 2: Add the manifest** `win-vde/src/vde.exe.manifest`

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
        version="6.0.0.0" processorArchitecture="amd64"
        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
</assembly>
```

- [ ] **Step 3: Reference the manifest from `src/vde.rc`**

Append after the existing `101 ICON "vde.ico"` line:

```rc
1 24 "vde.exe.manifest"
```

(24 = `RT_MANIFEST`, 1 = `CREATEPROCESS_MANIFEST_RESOURCE_ID`.)

- [ ] **Step 4: Add `win-vde/build.bat`**

```bat
@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
rc /nologo /i src /fo build\vde.res src\vde.rc || exit /b 1
cl /nologo /utf-8 /EHsc /W3 /std:c++14 src\vde.cpp build\vde.res /Fe:build\vde.exe /Fo:build\ || exit /b 1
echo Built build\vde.exe
```

- [ ] **Step 5: Add `win-vde/.gitignore`**

```gitignore
build/
*.obj
*.res
*.exe
.vs/
```

- [ ] **Step 6: Add `win-vde/LICENSE` (MIT)**

Standard MIT text, first line: `MIT License` and copyright line `Copyright (c) 2026 Volodymyr Moskvin (conus.vision)`.

- [ ] **Step 7: Add a `win-vde/README.md` stub**

One-paragraph description + a "Build" section showing `build.bat`. (Fleshed out in Task 13.)

- [ ] **Step 8: Build and smoke-test**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build.bat"`
Expected: `Built build\vde.exe`, no errors.
Run: `cmd /c "F:\_VDESKTOP_FF\win-vde\build\vde.exe list"`
Expected: prints `Virtual desktops: N` and desktop lines (proves COM init + manifest load OK). If it prints the IID-mismatch line instead, that's still a clean exit — note it and continue.

- [ ] **Step 9: Commit**

```bash
git add src build.bat .gitignore LICENSE README.md
git commit -m "chore: scaffold win-vde repo (src/, manifest, build.bat, MIT)"
```

---

## Task 2: Extract pure logic into headers + test harness

**Files:**
- Create: `src/str_util.hpp`, `src/session_firefox.hpp`, `src/fingerprint.hpp`, `src/layout.hpp`
- Modify: `src/vde.cpp` (remove moved code, add `#include` of the new headers)
- Create: `tests/vdtest.cpp`, `build-test.bat`

**Interfaces:**
- Produces (str_util.hpp): `inline std::string W2U8(const std::wstring&)`, `U82W`, `b64enc`, `b64dec`, `hostOf`, `etld1`, `GuidToString`, `StringToGuid`, `GuidEq`, `GuidIsZero`, `DWORD GetWindowsBuild()`. Add `#pragma comment(lib,"ole32.lib")` here.
- Produces (session_firefox.hpp): `struct SSWindow{ std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount; }`, `inline std::vector<SSWindow> extractWindows(const JValue&)`, `mozlz4_decompress`, `JValue`/`JParser`, `ReadSessionstore`, `FirefoxProfileDir`.
- Produces (fingerprint.hpp): `struct Fp{...}`, `inline double Score(const Fp&, const Fp&)`.
- Produces (layout.hpp): `struct DeskRec`, `CountsToStr`, `StrToCounts` (moved here; used by serialize).

- [ ] **Step 1: Create `src/str_util.hpp`**

Move verbatim from `vde.cpp` (current lines ~112–161 and ~146–151): `W2U8`, `U82W`, `GuidToString`, `StringToGuid`, `GuidEq`, `GuidIsZero`, `B64`/`b64enc`/`b64dec`, `GetWindowsBuild`, and (from ~252–269) `hostOf`, `etld1`. Prefix each free function with `inline`. Add at top:

```cpp
#pragma once
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#pragma comment(lib, "ole32.lib")
```

- [ ] **Step 2: Create `src/session_firefox.hpp`**

`#pragma once`, `#include "str_util.hpp"`. Move `lz4_block_decompress`, `mozlz4_decompress`, `JValue`, `JParser`, `SSWindow`, `extractWindows`, `ReadFileBytes`, `FileExists`, `FirefoxProfileDir*`, `ReadSessionstore`, `CurrentSessionstorePath`, `FileMtime` (all currently in `vde.cpp`). Mark free functions `inline`.

- [ ] **Step 3: Create `src/fingerprint.hpp`**

`#pragma once`, `#include "str_util.hpp"`. Move `struct Fp` and `Score`. Mark `Score` `inline`.

- [ ] **Step 4: Create `src/layout.hpp`**

`#pragma once`, `#include "str_util.hpp"`. Move `struct DeskRec`, `CountsToStr`, `StrToCounts` (mark `inline`). Leave `SnapshotPath`/`WriteSnapshot`/`ReadSnapshot` in `vde.cpp` for now (rewritten in Task 3/7).

- [ ] **Step 5: Update `src/vde.cpp` includes**

Remove the now-moved definitions. After the existing `#include` block add:

```cpp
#include "str_util.hpp"
#include "session_firefox.hpp"
#include "fingerprint.hpp"
#include "layout.hpp"
```

- [ ] **Step 6: Add `build-test.bat`**

```bat
@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /utf-8 /EHsc /W3 /std:c++14 /I src tests\vdtest.cpp /Fe:build\vdtest.exe /Fo:build\ || exit /b 1
build\vdtest.exe
```

- [ ] **Step 7: Create `tests/vdtest.cpp` with a characterization test for moved logic**

```cpp
#include <cstdio>
#include "str_util.hpp"
#include "session_firefox.hpp"

static int g_fail = 0, g_total = 0;
#define CHECK(c) do{ g_total++; if(!(c)){ g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static void test_etld1(){
    CHECK(etld1("mail.google.com") == "google.com");
    CHECK(etld1("a.github.io")     == "a.github.io"); // registrable under github.io per simple ruleset
    CHECK(etld1("docs.python.org") == "python.org");
    CHECK(hostOf("https://www.GitHub.com/x/y") == "github.com");
}
static void test_b64(){
    std::string s = "PR #42 — Mozilla";
    CHECK(b64dec(b64enc(s)) == s);
}

int main(){
    test_etld1();
    test_b64();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
```

> If `etld1("a.github.io")` assertion does not match the existing implementation's behavior, change the expectation to match the current code (this is a characterization test — it pins existing behavior, it does not change it).

- [ ] **Step 8: Run the tests**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build-test.bat"`
Expected: `2/2 passed`, exit 0.

- [ ] **Step 9: Rebuild the app to confirm the refactor didn't break it**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build.bat"`
Expected: `Built build\vde.exe`. Run `build\vde.exe status` → same output shape as before (desktops + Firefox live windows).

- [ ] **Step 10: Commit**

```bash
git add src tests build-test.bat
git commit -m "refactor: extract pure logic to headers + add test harness"
```

---

## Task 3: Layout v3 model + serialize/parse (TDD)

**Files:**
- Modify: `src/layout.hpp`
- Modify: `tests/vdtest.cpp`

**Interfaces:**
- Consumes: `str_util.hpp` (`GuidToString`, `StringToGuid`, `b64enc/dec`, `W2U8/U82W`), `CountsToStr/StrToCounts`, `DeskRec`.
- Produces:
  - `static const int MISSING_RUNS_MAX = 3;`
  - `struct LayoutWin { std::string app; int deskIndex=-1; GUID desktop={0}; std::string activeTitle, activeDomain; int tabCount=0; std::map<std::string,int> counts; int missingRuns=0; };`
  - `inline std::string SerializeLayout(const std::vector<DeskRec>&, const std::vector<LayoutWin>&);`
  - `inline bool ParseLayout(const std::string&, std::vector<DeskRec>&, std::vector<LayoutWin>&);`

- [ ] **Step 1: Write failing tests** (append to `tests/vdtest.cpp`, add `#include "layout.hpp"` and call from `main`)

```cpp
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
```

- [ ] **Step 2: Run tests, verify they FAIL to compile/pass**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build-test.bat"`
Expected: compile error (`LayoutWin`/`SerializeLayout` undefined).

- [ ] **Step 3: Implement in `src/layout.hpp`**

```cpp
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
```

- [ ] **Step 4: Run tests, verify PASS**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build-test.bat"`
Expected: all passed, exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/layout.hpp tests/vdtest.cpp
git commit -m "feat: layout v3 model + serialize/parse (v2 back-compat)"
```

---

## Task 4: FingerprintKey + MergeAutoLayout (TDD)

**Files:**
- Modify: `src/layout.hpp`, `tests/vdtest.cpp`

**Interfaces:**
- Consumes: `LayoutWin`.
- Produces:
  - `inline std::string FingerprintKey(const std::string& app, const std::map<std::string,int>& counts, const std::string& activeTitle);`
  - `inline std::vector<LayoutWin> MergeAutoLayout(const std::vector<LayoutWin>& existing, const std::vector<LayoutWin>& present);`

- [ ] **Step 1: Write failing tests** (append + call from `main`)

```cpp
static LayoutWin LW(const char* app, int desk, std::map<std::string,int> c, const char* title=""){
    LayoutWin w; w.app=app; w.deskIndex=desk; w.counts=c; w.activeTitle=title; return w;
}
static void test_merge_upsert_and_keep(){
    std::vector<LayoutWin> existing = { LW("firefox",0,{{"github.com",3}}), LW("firefox",1,{{"jira.com",2}}) };
    existing[0].missingRuns=1; existing[1].missingRuns=1;
    // Only the github window is present now, moved to desktop 2.
    std::vector<LayoutWin> present = { LW("firefox",2,{{"github.com",3}}) };
    auto merged = MergeAutoLayout(existing, present);
    CHECK(merged.size()==2);                       // jira window kept (not wiped)
    // find github record
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
    CHECK(FingerprintKey("firefox",{{"a.com",2}},"T") == FingerprintKey("firefox",{{"a.com",2}},"OTHER")); // domains win
    CHECK(FingerprintKey("explorer",{},"Downloads") != FingerprintKey("explorer",{},"Documents"));         // title used when no domains
}
```

- [ ] **Step 2: Run, verify FAIL** (`FingerprintKey`/`MergeAutoLayout` undefined).

- [ ] **Step 3: Implement in `src/layout.hpp`**

```cpp
inline std::string FingerprintKey(const std::string& app, const std::map<std::string,int>& counts, const std::string& activeTitle){
    std::string k = app + "|";
    if(!counts.empty()){ bool f=true; for(const auto& kv:counts){ if(!f)k+=","; f=false; k+=kv.first+":"+std::to_string(kv.second);} }
    else k += "t:" + activeTitle;
    return k;
}
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
```

- [ ] **Step 4: Run, verify PASS.**

- [ ] **Step 5: Commit**

```bash
git add src/layout.hpp tests/vdtest.cpp
git commit -m "feat: FingerprintKey + merge-based auto layout (anti-wipe)"
```

---

## Task 5: ReconcileGrace (TDD)

**Files:**
- Modify: `src/layout.hpp`, `tests/vdtest.cpp`

**Interfaces:**
- Consumes: `LayoutWin`, `FingerprintKey`, `MISSING_RUNS_MAX`.
- Produces: `inline std::vector<LayoutWin> ReconcileGrace(const std::vector<LayoutWin>& records, const std::set<std::string>& seenKeys, const std::set<std::string>& observedApps, int maxMissing);`

- [ ] **Step 1: Write failing tests**

```cpp
static void test_grace_seen_resets_unseen_increments(){
    std::vector<LayoutWin> recs = { LW("firefox",0,{{"a.com",1}}), LW("firefox",1,{{"b.com",1}}) };
    recs[0].missingRuns=2; recs[1].missingRuns=0;
    std::set<std::string> seen = { FingerprintKey("firefox",{{"a.com",1}},"") };  // only a.com seen
    std::set<std::string> apps = { "firefox" };
    auto out = ReconcileGrace(recs, seen, apps, MISSING_RUNS_MAX);
    // a.com seen ⇒ reset 0 kept; b.com unseen ⇒ 0->1 kept
    CHECK(out.size()==2);
    int ai=-1,bi=-1; for(int i=0;i<(int)out.size();++i){ if(out[i].counts.count("a.com"))ai=i; if(out[i].counts.count("b.com"))bi=i; }
    CHECK(out[ai].missingRuns==0); CHECK(out[bi].missingRuns==1);
}
static void test_grace_drops_at_threshold(){
    std::vector<LayoutWin> recs = { LW("firefox",0,{{"b.com",1}}) };
    recs[0].missingRuns = MISSING_RUNS_MAX - 1;                 // 2, will become 3 ⇒ dropped
    auto out = ReconcileGrace(recs, {}, {"firefox"}, MISSING_RUNS_MAX);
    CHECK(out.size()==0);
}
static void test_grace_untouched_when_app_not_observed(){
    std::vector<LayoutWin> recs = { LW("chrome",0,{{"c.com",1}}) };
    recs[0].missingRuns = 2;
    auto out = ReconcileGrace(recs, {}, {"firefox"} /*chrome not observed*/, MISSING_RUNS_MAX);
    CHECK(out.size()==1); CHECK(out[0].missingRuns==2);          // untouched
}
```

- [ ] **Step 2: Run, verify FAIL.**

- [ ] **Step 3: Implement in `src/layout.hpp`**

```cpp
inline std::vector<LayoutWin> ReconcileGrace(const std::vector<LayoutWin>& records,
        const std::set<std::string>& seenKeys, const std::set<std::string>& observedApps, int maxMissing){
    std::vector<LayoutWin> out;
    for(const auto& r : records){
        if(!observedApps.count(r.app)){ out.push_back(r); continue; }   // app not seen this run ⇒ don't touch
        LayoutWin w = r;
        std::string key = FingerprintKey(w.app,w.counts,w.activeTitle);
        if(seenKeys.count(key)) w.missingRuns = 0; else w.missingRuns += 1;
        if(w.missingRuns < maxMissing) out.push_back(w);
    }
    return out;
}
```

- [ ] **Step 4: Run, verify PASS.**

- [ ] **Step 5: Commit**

```bash
git add src/layout.hpp tests/vdtest.cpp
git commit -m "feat: grace-period reconcile (drop after MISSING_RUNS_MAX runs)"
```

---

## Task 6: Lifecycle state machine (TDD)

**Files:**
- Create: `src/lifecycle.hpp`
- Modify: `tests/vdtest.cpp`

**Interfaces:**
- Produces:
  - `enum class LcAction { None, StartupRestore, DoRestore, AutoSave, FinalSave };`
  - `struct LcState { bool prevPresent=false; bool restoredThisAppearance=false; bool launchPending=false; int stableTicks=0; };`
  - `inline LcAction LcOnStartup(LcState& s, bool present);`
  - `inline LcAction LcOnTick(LcState& s, bool present, int settleTicksNeeded);`
  - `inline LcAction LcOnExit(const LcState& s, bool present);`

- [ ] **Step 1: Write failing tests** (`#include "lifecycle.hpp"` in test)

```cpp
static void test_lc_startup_present_restores(){
    LcState s; CHECK(LcOnStartup(s, true) == LcAction::StartupRestore);
    CHECK(s.prevPresent && s.restoredThisAppearance);
}
static void test_lc_startup_absent_none(){
    LcState s; CHECK(LcOnStartup(s, false) == LcAction::None); CHECK(!s.prevPresent);
}
static void test_lc_launch_then_settle_restores_once(){
    LcState s; LcOnStartup(s, false);                 // absent at start
    CHECK(LcOnTick(s, true, 4) == LcAction::None);     // appearance tick 1 (stableTicks=1)
    CHECK(LcOnTick(s, true, 4) == LcAction::None);     // 2
    CHECK(LcOnTick(s, true, 4) == LcAction::None);     // 3
    CHECK(LcOnTick(s, true, 4) == LcAction::DoRestore);// 4 ⇒ settle reached
    CHECK(LcOnTick(s, true, 4) == LcAction::AutoSave); // afterwards periodic autosave
}
static void test_lc_absent_does_not_wipe_and_rearm(){
    LcState s; LcOnStartup(s, true);                   // present+restored
    CHECK(LcOnTick(s, false, 4) == LcAction::None);    // present->absent: no wipe
    CHECK(!s.restoredThisAppearance);                  // re-armed for next appearance
    CHECK(LcOnTick(s, true, 4) == LcAction::None);     // reappearance starts settle again
}
static void test_lc_exit(){
    LcState s; CHECK(LcOnExit(s, true) == LcAction::FinalSave);
    CHECK(LcOnExit(s, false) == LcAction::None);
}
```

- [ ] **Step 2: Run, verify FAIL.**

- [ ] **Step 3: Implement `src/lifecycle.hpp`**

```cpp
#pragma once
enum class LcAction { None, StartupRestore, DoRestore, AutoSave, FinalSave };
struct LcState { bool prevPresent=false; bool restoredThisAppearance=false; bool launchPending=false; int stableTicks=0; };

inline LcAction LcOnStartup(LcState& s, bool present){
    s.prevPresent = present;
    if(present){ s.restoredThisAppearance = true; s.launchPending = false; s.stableTicks = 0; return LcAction::StartupRestore; }
    return LcAction::None;
}
inline LcAction LcOnTick(LcState& s, bool present, int settleTicksNeeded){
    if(present && !s.prevPresent){                         // absent -> present (launch / session-restore)
        s.prevPresent = true; s.launchPending = true; s.restoredThisAppearance = false; s.stableTicks = 1;
        return LcAction::None;
    }
    if(present && s.prevPresent){
        if(s.launchPending){
            s.stableTicks++;
            if(s.stableTicks >= settleTicksNeeded){ s.launchPending=false; s.restoredThisAppearance=true; return LcAction::DoRestore; }
            return LcAction::None;
        }
        return LcAction::AutoSave;                          // steady state while present
    }
    // !present
    if(s.prevPresent){ s.prevPresent=false; s.launchPending=false; s.restoredThisAppearance=false; s.stableTicks=0; }
    return LcAction::None;                                  // present->absent or stay absent: never wipe
}
inline LcAction LcOnExit(const LcState&, bool present){ return present ? LcAction::FinalSave : LcAction::None; }
```

- [ ] **Step 4: Run, verify PASS.**

- [ ] **Step 5: Commit**

```bash
git add src/lifecycle.hpp tests/vdtest.cpp
git commit -m "feat: testable lifecycle state machine"
```

---

## Task 7: Layout file I/O, dual files, migration, save/restore rewire

**Files:**
- Modify: `src/vde.cpp` (replace `SnapshotPath`/`WriteSnapshot`/`ReadSnapshot`/`RunSave`/`RunRestore`/`CliRun`)

**Interfaces:**
- Consumes: `SerializeLayout`, `ParseLayout`, `MergeAutoLayout`, `LayoutWin`, `DeskRec`, `Fp`, `Score`.
- Produces (in `vde.cpp`, file-scope):
  - `std::wstring LayoutPath(bool manual)` → `...\layout-manual.txt` or `...\layout-auto.txt`
  - `void MigrateLegacyLayout()`
  - `std::vector<LayoutWin> CollectPresentWindows(std::set<std::string>& outSeenKeys, std::set<std::string>& outObservedApps)` — builds `LayoutWin` list for currently-open Firefox windows (desktop assigned) reusing `BuildLiveFingerprints`
  - `std::string RunSaveManual()`, `std::string RunSaveAuto()`, `std::string RunRestore(bool manual, std::vector<std::string>* linesOut=nullptr)`

- [ ] **Step 1: Add paths + migration in `vde.cpp`**

```cpp
static std::wstring DataDir(){
    wchar_t base[MAX_PATH]={0}; GetEnvironmentVariableW(L"LOCALAPPDATA",base,MAX_PATH);
    std::wstring dir=std::wstring(base)+L"\\VirtualDesktopsExtention"; CreateDirectoryW(dir.c_str(),nullptr); return dir;
}
static std::wstring LayoutPath(bool manual){ return DataDir()+(manual?L"\\layout-manual.txt":L"\\layout-auto.txt"); }
static void MigrateLegacyLayout(){
    std::wstring legacy=DataDir()+L"\\layout.txt", autoP=LayoutPath(false);
    if(FileExists(legacy) && !FileExists(autoP)) MoveFileW(legacy.c_str(), autoP.c_str());
}
static bool WriteTextFile(const std::wstring& path, const std::string& text){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false; DWORD wr=0; BOOL ok=WriteFile(f,text.data(),(DWORD)text.size(),&wr,nullptr); CloseHandle(f); return ok&&wr==text.size();
}
```

- [ ] **Step 2: Build the "present windows" collector + desktop list helper**

Reuse existing `BuildLiveFingerprints()` (Firefox live windows joined to sessionstore) and `GetDesktopIndexByGuid`. Convert each `Fp` with a real desktop into a `LayoutWin` (`app="firefox"`), and record its `FingerprintKey` into `outSeenKeys`, `"firefox"` into `outObservedApps`:

```cpp
static std::vector<DeskRec> CurrentDesktops(){
    std::vector<DeskRec> desks; UINT count=0; if(g_vdmi) g_vdmi->GetCount(&count);
    for(UINT i=0;i<count;++i){ IVirtualDesktop* d=GetDesktopByIndex(i); if(!d)continue; GUID g={0};d->GetID(&g);
        DeskRec dr; dr.index=(int)i; dr.guid=g; dr.name=DesktopNameFromRegistry(g); desks.push_back(dr); d->Release(); }
    return desks;
}
static std::vector<LayoutWin> CollectPresentWindows(std::set<std::string>* seen, std::set<std::string>* apps){
    std::vector<LayoutWin> out; auto fps=BuildLiveFingerprints();
    for(auto& f:fps){ if(GuidIsZero(f.desktop))continue; LayoutWin w; w.app="firefox"; w.deskIndex=GetDesktopIndexByGuid(f.desktop);
        w.desktop=f.desktop; w.activeTitle=f.activeTitle; w.activeDomain=f.activeDomain; w.tabCount=f.tabCount; w.counts=f.counts; w.missingRuns=0;
        out.push_back(w);
        if(seen) seen->insert(FingerprintKey(w.app,w.counts,w.activeTitle));
        if(apps) apps->insert(w.app);
    }
    return out;
}
```

- [ ] **Step 3: Rewrite save/restore to use the two files**

```cpp
static std::string RunSaveManual(){
    auto present=CollectPresentWindows(nullptr,nullptr);
    if(present.empty()) return "No browser windows found. Nothing to save.";
    std::string text=SerializeLayout(CurrentDesktops(), present);   // full snapshot, overwrite
    if(!WriteTextFile(LayoutPath(true), text)) return "Failed to write manual layout.";
    char b[160]; sprintf_s(b,"Saved layout: %d window(s).",(int)present.size()); return b;
}
static std::string RunSaveAuto(std::set<std::string>* seen, std::set<std::string>* apps){
    auto present=CollectPresentWindows(seen,apps);
    if(present.empty()) return "";                                  // no windows ⇒ do not touch auto file (anti-wipe)
    std::vector<DeskRec> ed; std::vector<LayoutWin> existing; { std::string t; if(ReadFileBytes(LayoutPath(false),t)) ParseLayout(t,ed,existing); }
    auto merged=MergeAutoLayout(existing, present);
    WriteTextFile(LayoutPath(false), SerializeLayout(CurrentDesktops(), merged));
    return "auto-saved";
}
static std::string RunRestore(bool manual, std::vector<std::string>* linesOut){
    std::vector<DeskRec> savedDesks; std::vector<LayoutWin> saved; { std::string t; if(!ReadFileBytes(LayoutPath(manual),t) || !ParseLayout(t,savedDesks,saved) || saved.empty()) return manual?"No saved layout. Use 'Save windows layout' first.":"No auto layout yet."; }
    auto live=BuildLiveFingerprints(); if(live.empty()) return "No browser windows to restore.";
    UINT count=0; g_vdmi->GetCount(&count);
    // Convert saved LayoutWin -> Fp for existing Score(); match; move (logic identical to current RunRestore).
    // ... (reuse the current pairing loop; source rows are `saved`, filtered so only same-app windows can match)
    // Return "Restore: matched X/Y, moved M, failed F."
}
```

> The `RunRestore` pairing/moving body is the same algorithm currently in `vde.cpp` (`T_FLOOR=0.35`, `T_ACCEPT=0.55`, sort by score, assign, `MoveWindowToDesktop`, `VerifyOnDesktop`). Adapt it to read from `saved` (`LayoutWin`) by building an `Fp` per saved row (`counts/activeTitle/activeDomain/tabCount/desktop/deskIndex`), and add a guard so a live Firefox window only matches saved rows with `app=="firefox"`.

- [ ] **Step 4: Rewire CLI** in `CliRun`: `save` → `RunSaveManual()`; `restore` → `RunRestore(true,&lines)`; add `restore-auto` → `RunRestore(false,&lines)`. Update the usage text.

- [ ] **Step 5: Call `MigrateLegacyLayout()`** once at startup (top of `RunGui` and before `CliRun`).

- [ ] **Step 6: Build + manual verify**

Run: `build.bat`. Then with Firefox open across two desktops:
- `build\vde.exe save` → inspect `%LOCALAPPDATA%\VirtualDesktopsExtention\layout-manual.txt` contains `v3` + `W\tfirefox\t...` rows.
- Put an old `layout.txt` in the data dir, delete `layout-auto.txt`, run `build\vde.exe list` → `layout.txt` becomes `layout-auto.txt`.

- [ ] **Step 7: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: dual layout files (auto merge / manual full) + migration + CLI"
```

---

## Task 8: Monitor timer drives lifecycle (startup restore, launch settle, save-on-exit)

**Files:**
- Modify: `src/vde.cpp` (replace the `TIMER_AUTOFIX` handler and startup path)

**Interfaces:**
- Consumes: `LcState`, `LcOnStartup`, `LcOnTick`, `LcOnExit`, `RunSaveAuto`, `RunRestore(false,...)`, `ReconcileGrace`.

- [ ] **Step 1: Add monitor globals + session accumulators**

```cpp
#define TIMER_MONITOR 1
#define TIMER_STARTUP 2
static LcState g_lc;
static std::set<std::string> g_seenKeys;      // fingerprints seen at least once this run
static std::set<std::string> g_observedApps;  // apps seen at least once this run
static bool FirefoxPresent(){ return !EnumFirefoxWindows().empty(); }
```

- [ ] **Step 2: Replace the timer setup.** Constants: `MONITOR_INTERVAL_MS=5000`, `STARTUP_SETTLE_MS=2500`, `LAUNCH_SETTLE_TICKS=4`. In `RunGui`, after `TrayAdd`, always start the monitor: `SetTimer(g_main,TIMER_MONITOR,MONITOR_INTERVAL_MS,nullptr);` then run startup detection:

```cpp
bool present = FirefoxPresent();
LcAction a = LcOnStartup(g_lc, present);
if(a==LcAction::StartupRestore) SetTimer(g_main, TIMER_STARTUP, STARTUP_SETTLE_MS, nullptr); // one-shot settle then restore
```

- [ ] **Step 3: Implement the timer handler** (replace the old `WM_TIMER`/AutoFix block)

```cpp
case WM_TIMER:
    if(wp==TIMER_STARTUP){ KillTimer(hwnd,TIMER_STARTUP); if(FirefoxPresent()) Balloon(U82W(RunRestore(false))); return 0; }
    if(wp==TIMER_MONITOR){
        bool present = FirefoxPresent();
        if(present){ g_observedApps.insert("firefox");
            // accumulate seen keys for this run
            std::set<std::string> s,ap; CollectPresentWindows(&s,&ap); for(auto&k:s) g_seenKeys.insert(k);
        }
        LcAction a = LcOnTick(g_lc, present, LAUNCH_SETTLE_TICKS);
        if(a==LcAction::DoRestore)      Balloon(U82W(RunRestore(false)));
        else if(a==LcAction::AutoSave)  RunSaveAuto(&g_seenKeys,&g_observedApps);
    }
    return 0;
```

- [ ] **Step 4: Final save + grace on exit.** In `WM_DESTROY`, before `TrayRemove()`:

```cpp
if(LcOnExit(g_lc, FirefoxPresent())==LcAction::FinalSave){
    RunSaveAuto(&g_seenKeys,&g_observedApps);
    std::vector<DeskRec> d; std::vector<LayoutWin> recs; std::string t;
    if(ReadFileBytes(LayoutPath(false),t) && ParseLayout(t,d,recs)){
        auto kept = ReconcileGrace(recs, g_seenKeys, g_observedApps, MISSING_RUNS_MAX);
        WriteTextFile(LayoutPath(false), SerializeLayout(CurrentDesktops(), kept));
    }
}
```

- [ ] **Step 5: Remove the old AutoFix setting wiring** (`ApplyAutoFix`, `g_autoFix` timer usage). Keep `g_autoFix` as a "monitor enabled" flag if desired, defaulting **on**; if off, don't `SetTimer(TIMER_MONITOR)`. (Settings label updated in Task 12.)

- [ ] **Step 6: Build + manual verify (the core behavior)**

Run `build.bat`, then:
1. Open Firefox on Desktop 2. Launch `build\vde.exe`. Manually move the Firefox window to Desktop 1. Exit and relaunch `vde.exe`. Within ~3 s Firefox is moved back to Desktop 2. ✅ startup restore.
2. With `vde.exe` running and no Firefox, start Firefox. After ~20 s its windows are placed per the auto layout. ✅ launch settle.
3. With windows saved, close **all** Firefox windows; confirm `layout-auto.txt` still lists them (not wiped). Reopen Firefox → restored. ✅ anti-wipe.

- [ ] **Step 7: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: monitor drives lifecycle auto-restore + save-on-exit + grace"
```

---

## Task 9: Tray menu relabel

**Files:**
- Modify: `src/vde.cpp` (`WM_TRAY` menu block)

- [ ] **Step 1: Replace the popup menu** per spec §7.1:

```cpp
AppendMenuW(m,MF_STRING,200,L"Open desktop picker");
AppendMenuW(m,MF_SEPARATOR,0,nullptr);
AppendMenuW(m,MF_STRING,201,L"Save windows layout");
AppendMenuW(m,MF_STRING,202,L"Restore saved windows layout");
AppendMenuW(m,MF_STRING,204,L"Restore last auto saved layout");
AppendMenuW(m,MF_SEPARATOR,0,nullptr);
AppendMenuW(m,MF_STRING,203,L"Settings...");
AppendMenuW(m,MF_STRING,205,L"About...");
AppendMenuW(m,MF_STRING,209,L"Exit");
```

- [ ] **Step 2: Wire commands**

```cpp
if(cmd==200)ShowPicker();
else if(cmd==201)Balloon(U82W(RunSaveManual()));
else if(cmd==202)Balloon(U82W(RunRestore(true)));
else if(cmd==204)Balloon(U82W(RunRestore(false)));
else if(cmd==203)OpenSettings();
else if(cmd==205)OpenAbout();   // implemented in Task 10
else if(cmd==209)DestroyWindow(hwnd);
```

- [ ] **Step 3: Temporary stub** `static void OpenAbout(){}` so it builds (replaced in Task 10).

- [ ] **Step 4: Build + manual verify** menu labels + Save/Restore-saved/Restore-auto all work.

- [ ] **Step 5: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: relabel tray menu (Save/Restore saved/Restore last auto)"
```

---

## Task 10: About dialog

**Files:**
- Modify: `src/vde.cpp`

**Interfaces:**
- Produces: `static void OpenAbout();`, `static const wchar_t* APP_VERSION = L"1.0.0";`

- [ ] **Step 1: Add version + link ids** near the other `#define`s:

```cpp
static const wchar_t* APP_VERSION = L"1.0.0";
#define IDC_LINK_MAIL 1101
#define IDC_LINK_REPO 1102
#define IDC_ABOUT_COPY 1103
```

- [ ] **Step 2: Register the About window class + WndProc.** Model it on the existing `SettingsProc`/`OpenSettings`. Create controls in `WM_CREATE`:
  - `STATIC` line 1: `Virtual Desktops Extension for Windows 11 — v1.0.0`
  - `STATIC` line 2: `Saves and restores browser windows across Windows 11 virtual desktops.`
  - `STATIC`: `Author: Volodymyr Moskvin`
  - `SysLink` (`WC_LINK`, i.e. class `L"SysLink"`), id `IDC_LINK_MAIL`, text: `<a href="mailto:info@conus.vision">info@conus.vision</a>`
  - `SysLink`, id `IDC_LINK_REPO`, text: `<a href="https://github.com/conus-vision/win-vde">github.com/conus-vision/win-vde</a>`
  - `STATIC`: `License: MIT` and `Windows build: <n>` (via `GetWindowsBuild()`)
  - `BUTTON` `IDC_ABOUT_COPY` "Copy", `BUTTON` `IDOK` "Close"

- [ ] **Step 3: Handle link clicks** in the About `WM_NOTIFY`:

```cpp
case WM_NOTIFY:{ NMHDR* n=(NMHDR*)lp;
    if(n->code==NM_CLICK||n->code==NM_RETURN){
        if(n->idFrom==IDC_LINK_MAIL) ShellExecuteW(hwnd,L"open",L"mailto:info@conus.vision",nullptr,nullptr,SW_SHOW);
        else if(n->idFrom==IDC_LINK_REPO) ShellExecuteW(hwnd,L"open",L"https://github.com/conus-vision/win-vde",nullptr,nullptr,SW_SHOW);
    } return 0; }
```

- [ ] **Step 4: Copy button** puts `Virtual Desktops Extension v1.0.0 | info@conus.vision | Windows build <n>` on the clipboard (`OpenClipboard`/`SetClipboardData(CF_UNICODETEXT,...)`).

- [ ] **Step 5: Ensure `ICC_LINK_CLASS`** is in the `InitCommonControlsEx` mask in `RunGui`:

```cpp
INITCOMMONCONTROLSEX icc={sizeof(icc), ICC_HOTKEY_CLASS|ICC_STANDARD_CLASSES|ICC_LINK_CLASS}; InitCommonControlsEx(&icc);
```

- [ ] **Step 6: Replace the Task 9 stub** with the real `OpenAbout()`.

- [ ] **Step 7: Build + manual verify:** tray → About shows; clicking the email opens the mail client; clicking the repo opens the browser; Copy fills the clipboard; build number shown.

- [ ] **Step 8: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: About dialog (author, contacts, repo link, version, build)"
```

---

## Task 11: Compatibility-break detection + dialog + degraded mode

**Files:**
- Modify: `src/vde.cpp`

**Interfaces:**
- Produces: `static bool SanityCheckServices();`, `static void ShowCompatIssue(bool buildChanged);`, `static bool g_degraded;`

- [ ] **Step 1: Sanity check** after `InitServices()`:

```cpp
static bool SanityCheckServices(){
    if(!g_vdmi) return false;
    UINT n=0; if(FAILED(g_vdmi->GetCount(&n))) return false;
    if(n<1 || n>64) return false;
    IVirtualDesktop* d=nullptr; if(FAILED(g_vdmi->GetCurrentDesktop(&d))||!d) return false;
    GUID g={0}; bool ok=SUCCEEDED(d->GetID(&g)) && !GuidIsZero(g); d->Release(); return ok;
}
```

- [ ] **Step 2: LastGoodBuild registry helpers** (in `HKCU\Software\VirtualDesktopsExtention`, value `LastGoodBuild` DWORD). `ReadLastGoodBuild()`, `WriteLastGoodBuild(DWORD)`.

- [ ] **Step 3: CompatIssue window.** A window like About with a `SysLink`, showing:
  - Title: `Virtual Desktops Extension — compatibility issue`
  - Body (STATIC, multi-line): `Moving other apps' windows between virtual desktops has no public Windows API, so this tool uses undocumented system interfaces. A recent Windows update likely changed them — this is expected and not a fault in your system.` + `Your Windows build: <n>.` + `Please email info@conus.vision (include the build number) so a fix can be posted, or watch the project page for an update:`
  - `SysLink` mail + `SysLink` repo (same URLs as About)
  - Buttons: `Copy details` (build + email to clipboard), `Close`.

- [ ] **Step 4: Wire detection** in `wWinMain` GUI branch. Replace the current `if(!InitServices()){ MessageBox... rc=2 }`:

```cpp
bool good = InitServices() && SanityCheckServices();
if(good){ WriteLastGoodBuild(GetWindowsBuild()); rc = RunGui(hInst); }
else {
    g_degraded = true;
    DWORD last = ReadLastGoodBuild();
    ShowCompatIssue(last!=0 && last!=GetWindowsBuild());
    rc = RunGui(hInst);   // still run: tray + About available, moves disabled
}
```

- [ ] **Step 5: Degraded behavior in `RunGui`/handlers.** When `g_degraded`: tray tip becomes `... (compatibility issue — see About)`; `ShowPicker`, `RunRestore`, `RunSaveManual/Auto`, and the monitor timer all early-return with a short balloon `Virtual-desktop features are unavailable on this Windows build.` Do **not** register `TIMER_MONITOR`. About and Exit still work. Guard COM-dereferencing paths against `g_degraded` (and null `g_vdmi`).

- [ ] **Step 6: Build + manual verify.** Normal machine: no dialog, `LastGoodBuild` written (check registry). To exercise the failure path, temporarily corrupt one hex digit of `kIID_IVirtualDesktopManagerInternal`, rebuild → CompatIssue dialog appears, tray still present, About works, moves disabled. Restore the IID and rebuild.

- [ ] **Step 7: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: detect broken undocumented COM after Windows update; CompatIssue dialog + degraded mode"
```

---

## Task 12: Autostart toggle + settings refresh

**Files:**
- Modify: `src/vde.cpp` (`SettingsProc`, `LoadSettings`/`SaveSettings`)

**Interfaces:**
- Produces: `static bool GetRunAtLogon();`, `static void SetRunAtLogon(bool);`, `#define IDC_AUTOSTART 1003`

- [ ] **Step 1: Registry Run helpers**

```cpp
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VAL = L"VirtualDesktopsExtention";
static bool GetRunAtLogon(){ HKEY hk; bool r=false; if(RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_READ,&hk)==ERROR_SUCCESS){ r=RegQueryValueExW(hk,RUN_VAL,0,0,0,0)==ERROR_SUCCESS; RegCloseKey(hk);} return r; }
static void SetRunAtLogon(bool on){ HKEY hk; if(RegOpenKeyExW(HKEY_CURRENT_USER,RUN_KEY,0,KEY_WRITE,&hk)!=ERROR_SUCCESS)return;
    if(on){ wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring q=L"\""+std::wstring(p)+L"\""; RegSetValueExW(hk,RUN_VAL,0,REG_SZ,(LPBYTE)q.c_str(),(DWORD)((q.size()+1)*sizeof(wchar_t))); }
    else RegDeleteValueW(hk,RUN_VAL);
    RegCloseKey(hk); }
```

- [ ] **Step 2: Add checkbox** in `SettingsProc` `WM_CREATE`: `BUTTON` `BS_AUTOCHECKBOX` id `IDC_AUTOSTART`, label `Start with Windows (run at logon)`. Initialize its check from `GetRunAtLogon()`. Rename the existing auto-fix checkbox label to `Auto-save & auto-restore layout`.

- [ ] **Step 3: Apply on OK** in `WM_COMMAND IDOK`: `SetRunAtLogon(IsDlgButtonChecked(hwnd,IDC_AUTOSTART)==BST_CHECKED);`

- [ ] **Step 4: Resize the settings window** to fit the extra row (increase the height constant in `OpenSettings` and move OK/Cancel down).

- [ ] **Step 5: Build + manual verify.** Settings → check "Start with Windows" → OK. Confirm `HKCU\...\Run\VirtualDesktopsExtention` holds the quoted exe path. Uncheck → value removed.

- [ ] **Step 6: Commit**

```bash
git add src/vde.cpp
git commit -m "feat: Start-with-Windows toggle in Settings"
```

---

## Task 13: README + final Phase-1 build

**Files:**
- Modify: `win-vde/README.md`

- [ ] **Step 1: Write the README** with sections: What it is (the 3 problems Windows/browsers don't solve); Features (Firefox auto-restore, anti-wipe grace, manual/auto layouts, desktop picker with hotkey, autostart, About); Install/Build (`build.bat`, VS 2022/2017 x64 Native Tools); Usage (tray menu items, `Ctrl+Alt+D` picker, CLI `vde save|restore|restore-auto|status|list`); Data files (`layout-auto.txt`, `layout-manual.txt`); Limitations (undocumented COM ⇒ a Windows update can break moving; degraded mode + CompatIssue dialog; desktop-only, geometry left to the browser); Author **Volodymyr Moskvin — info@conus.vision**; License MIT.

- [ ] **Step 2: Final clean build + test**

Run: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && build-test.bat && build.bat"`
Expected: `N/N passed` then `Built build\vde.exe`.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: Phase 1 README"
```

---

## Self-Review (completed)

**Spec coverage (Phase 1 scope):** R1 startup+launch restore → Task 8. R2 save-on-exit-if-present → Task 8. R3 anti-wipe/no-windows → Tasks 4,7,8. R5 menu → Task 9. R6 grace/missingRuns → Tasks 4,5,8. R11 About → Task 10. R12 CompatIssue/degraded → Task 11. Autostart → Task 12. Repo/manifest/build/LICENSE/README → Tasks 1,13. Layout v3 + migration → Tasks 3,7. (R4 Chrome/Edge/generic → Phase 2 plan. R7–R10 picker UX → Phase 3 plan.)

**Placeholder scan:** `RunRestore` body in Task 7 Step 3 intentionally reuses the existing algorithm and points at the exact constants/functions rather than repasting ~30 lines; all new logic is shown in full. No `TBD`/`add error handling`/vague steps.

**Type consistency:** `LayoutWin`, `FingerprintKey(app,counts,activeTitle)`, `MergeAutoLayout(existing,present)`, `ReconcileGrace(records,seenKeys,observedApps,max)`, `LcState`/`LcAction`/`LcOnStartup`/`LcOnTick`/`LcOnExit` are used identically across Tasks 3–8. Menu ids (200/201/202/203/204/205/209) and control ids (IDC_AUTOSTART=1003, IDC_LINK_MAIL=1101…) are unique and consistent.

## Deferred to their own plans

- **Phase 2 — Multi-app:** `AppProfile`, generalized `EnumAppWindows`, Chromium SNSS reader (Chrome+Edge) with generic title/exe fallback, per-app match filter, per-app settings checkboxes. (Spec §6, R4.)
- **Phase 3 — Picker UX:** search box, per-tile scroll, truncated-name tooltip, accent Ctrl+Click badge. (Spec §8, R7–R10.)
