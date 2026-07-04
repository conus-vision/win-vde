# win-vde Phase 2 (Multi-app) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (or subagent-driven-development). Steps use `- [ ]` checkboxes.

**Goal:** Extend save/restore beyond Firefox to Chrome, Edge, and (mechanism for) other multi-window apps — matched robustly by window title/exe, with Chrome/Edge additionally fingerprinted by tab **domains** parsed from their SNSS session files.

**Architecture:** Introduce an `AppProfile` (window class + exe + title suffixes + session source) and a per-app collector. Firefox uses its `sessionstore` reader; Chrome/Edge use a new **SNSS** reader (validated against real files: `"SNSS"` + int32 version + `[uint16 size][uint8 id][pickle]`; cmd 0 = tab→window, cmd 6 = tab nav `[int tab][int index][string url][string title]`, cmd 7 = selected nav, cmd 2/8 = tab index / selected tab). Any window with no domain data falls back to **title matching**. The monitor/save/restore iterate all enabled apps; restore only matches windows of the same app.

**Tech Stack:** C++14, Win32, MSVC (`vcvars64` + `cl`/`rc`). No third-party libs.

## Global Constraints

- Same as Phase 1 (`/utf-8 /EHsc /W3 /std:c++14`, data dir `%LOCALAPPDATA%\VirtualDesktopsExtention\`, MIT, author Volodymyr Moskvin / info@conus.vision).
- Canonical SNSS command IDs (stable in Chromium): `kSetTabWindow=0`, `kSetTabIndexInWindow=2`, `kUpdateTabNavigation=6`, `kSetSelectedNavigationIndex=7`, `kSetSelectedTabInIndex=8`. Parser must **degrade gracefully** (any unreadable command/pickle is skipped, never crashes).
- **Privacy:** never commit real browser session files. Tests use a synthetic SNSS fixture generated in-test; validation against real files is local-only.
- Chrome user data: `%LOCALAPPDATA%\Google\Chrome\User Data`; Edge: `%LOCALAPPDATA%\Microsoft\Edge\User Data`. Session files: `<UserData>\Default\Sessions\Session_*` (use newest non-empty).
- Window classes: Firefox `MozillaWindowClass` / `firefox.exe`; Chrome & Edge `Chrome_WidgetWin_1` / `chrome.exe`, `msedge.exe`. Title suffixes: `" - Google Chrome"`, `" - Microsoft Edge"` (and en-dash `" – "` / em-dash `" — "` variants), plus existing Firefox suffixes.

---

## File Structure

```
src/
  session.hpp     # NEW: Firefox sessionstore reader (moved from vde.cpp) + SNSS reader; struct WinFp
  fingerprint.hpp # + add `std::string app` to Fp
  appprofile.hpp  # NEW: AppProfile struct + built-in profile list
  vde.cpp         # generalize enumeration/collect/restore to iterate profiles; settings checkboxes
tests/
  vdtest.cpp      # + SNSS synthetic round-trip tests
```

`session.hpp` owns all session parsing (this also completes the Phase-1-deferred extraction of the Firefox session code out of `vde.cpp`). `WinFp` is the neutral per-window fingerprint both readers emit.

---

## Task 1: session.hpp — move Firefox reader + add WinFp (no behavior change)

**Files:** Create `src/session.hpp`; modify `src/vde.cpp` (remove moved code, include header).

**Interfaces — Produces:**
- `struct WinFp { std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount=0; };`
- `inline std::vector<WinFp> ReadFirefoxWindows();` (was `ReadSessionstore` returning `SSWindow`; rename type to `WinFp`)
- Moves into the header: `lz4_block_decompress`, `mozlz4_decompress`, `JValue`, `JParser`, `extractWindows`, `ReadFileBytes`, `FileExists`, `FirefoxProfileDir*`, `ReadSessionstore`→`ReadFirefoxWindows`, `CurrentSessionstorePath`, `FileMtime`.

- [ ] **Step 1:** Create `src/session.hpp` (`#pragma once`, `#include "str_util.hpp"`). Move the listed functions verbatim from `vde.cpp`, marking free functions `inline`. Rename `struct SSWindow` → `struct WinFp` (same fields) and `ReadSessionstore(...)` → `ReadFirefoxWindows(...)` returning `std::vector<WinFp>`.
- [ ] **Step 2:** In `vde.cpp` delete the moved definitions; add `#include "session.hpp"` after `#include "layout.hpp"`. Update the one caller in `BuildLiveFingerprints` (`ReadSessionstore()` → `ReadFirefoxWindows()`) and any `SSWindow` references → `WinFp`. `status` CLI uses `ReadSessionstore(&up)` — update to `ReadFirefoxWindows(&up)`.
- [ ] **Step 3:** Build the app: `cmd /c "cd /d F:\_VDESKTOP_FF\win-vde && call .\build.bat"` → `Built build\vde.exe`.
- [ ] **Step 4:** Run tests `build-test.bat` (still 52/52) and `build\vde.exe status` (exit 0, same output shape).
- [ ] **Step 5:** Commit: `git add src/session.hpp src/vde.cpp && git commit -m "refactor: move Firefox session reader into session.hpp (WinFp)"`

---

## Task 2: SNSS reader (TDD) in session.hpp

**Files:** modify `src/session.hpp`, `tests/vdtest.cpp`.

**Interfaces — Produces:**
- `inline std::vector<WinFp> ParseChromiumSNSS(const std::string& bytes);` — parse a whole SNSS file image into per-window fingerprints (`activeTitle` = active tab title, `counts` = per-tab current domain multiset, `tabCount`).
- `inline std::wstring FindChromiumSessionFile(const std::wstring& userDataDir);` — newest non-empty `Default\Sessions\Session_*`.
- `inline std::vector<WinFp> ReadChromiumWindows(const std::wstring& userDataDir);` — read file + `ParseChromiumSNSS`.

- [ ] **Step 1: Write failing tests** (append to `tests/vdtest.cpp`, `#include "session.hpp"`). Build a synthetic SNSS image in-test with a tiny encoder, then assert parse results:

```cpp
// --- minimal SNSS encoder (mirrors the reader; aligns to 4 like base::Pickle) ---
static void pkInt(std::string& p,int v){ while(p.size()%4)p.push_back(0); for(int i=0;i<4;i++)p.push_back((char)((v>>(8*i))&0xFF)); }
static void pkStr(std::string& p,const std::string& s){ pkInt(p,(int)s.size()); p+=s; while(p.size()%4)p.push_back(0); }
static void cmd(std::string& f,unsigned char id,const std::string& payload){
    std::string pk; int32_t psz=(int32_t)payload.size();               // pickle = [uint32 payloadSize][payload]
    for(int i=0;i<4;i++)pk.push_back((char)((psz>>(8*i))&0xFF)); pk+=payload;
    uint16_t sz=(uint16_t)(pk.size()+1);                                // +1 for id byte
    f.push_back((char)(sz&0xFF)); f.push_back((char)((sz>>8)&0xFF)); f.push_back((char)id); f+=pk;
}
static std::string makeSnss(){
    std::string f="SNSS"; int32_t ver=3; for(int i=0;i<4;i++)f.push_back((char)((ver>>(8*i))&0xFF));
    // window 10: tab 1 (github), tab 2 (docs.python) active; window 11: tab 3 (example)
    { std::string p; pkInt(p,10); pkInt(p,1); cmd(f,0,p); }            // SetTabWindow tab1->win10
    { std::string p; pkInt(p,10); pkInt(p,2); cmd(f,0,p); }            // tab2->win10
    { std::string p; pkInt(p,11); pkInt(p,3); cmd(f,0,p); }            // tab3->win11
    { std::string p; pkInt(p,1); pkInt(p,0); cmd(f,2,p); }             // tab1 index 0 in window
    { std::string p; pkInt(p,2); pkInt(p,1); cmd(f,2,p); }             // tab2 index 1
    { std::string p; pkInt(p,3); pkInt(p,0); cmd(f,2,p); }             // tab3 index 0
    { std::string p; pkInt(p,10); pkInt(p,1); cmd(f,8,p); }            // window10 selected tab index = 1 (tab2)
    { std::string p; pkInt(p,11); pkInt(p,0); cmd(f,8,p); }            // window11 selected index 0 (tab3)
    auto nav=[&](int tab,int idx,const std::string& url,const std::string& title){ std::string p; pkInt(p,tab); pkInt(p,idx); pkStr(p,url); pkStr(p,title); cmd(f,6,p); };
    nav(1,0,"https://github.com/x","GitHub");
    nav(2,0,"https://docs.python.org/3","Python");
    nav(3,0,"https://example.com/","Example");
    { std::string p; pkInt(p,1); pkInt(p,0); cmd(f,7,p);} { std::string p; pkInt(p,2); pkInt(p,0); cmd(f,7,p);} { std::string p; pkInt(p,3); pkInt(p,0); cmd(f,7,p);}
    return f;
}
static void test_snss_parse(){
    auto w = ParseChromiumSNSS(makeSnss());
    CHECK(w.size()==2);
    // window 10 has two domains, active tab = tab2 (Python / docs.python.org)
    int wi10=-1,wi11=-1; for(int i=0;i<(int)w.size();++i){ if(w[i].counts.count("github.com"))wi10=i; if(w[i].counts.count("example.com"))wi11=i; }
    CHECK(wi10>=0 && wi11>=0);
    CHECK(w[wi10].tabCount==2); CHECK(w[wi10].counts["github.com"]==1); CHECK(w[wi10].counts["python.org"]==1);
    CHECK(w[wi10].activeTitle=="Python"); CHECK(w[wi10].activeDomain=="python.org");
    CHECK(w[wi11].tabCount==1); CHECK(w[wi11].activeTitle=="Example");
}
static void test_snss_garbage(){ auto w=ParseChromiumSNSS("not an snss file...."); CHECK(w.empty()); }
```

Call both from `main`.

- [ ] **Step 2:** Run `build-test.bat` → expect FAIL (`ParseChromiumSNSS` undefined).
- [ ] **Step 3: Implement in `src/session.hpp`:**

```cpp
// ---- Chromium SNSS session reader ----
struct SnssPR { const uint8_t* p; size_t n; size_t i=0;
    void align(){ i=(i+3)&~size_t(3); }
    bool rInt(int32_t& v){ align(); if(i+4>n)return false; v=(int32_t)(p[i]|(p[i+1]<<8)|(p[i+2]<<16)|((uint32_t)p[i+3]<<24)); i+=4; return true; }
    bool rStr(std::string& s){ int32_t L; if(!rInt(L))return false; if(L<0||i+(size_t)L>n)return false; s.assign((const char*)p+i,(size_t)L); i+=(size_t)L; return true; }
};
inline std::vector<WinFp> ParseChromiumSNSS(const std::string& data){
    std::vector<WinFp> out;
    const uint8_t* b=(const uint8_t*)data.data(); size_t sz=data.size();
    if(sz<8 || !(b[0]=='S'&&b[1]=='N'&&b[2]=='S'&&b[3]=='S')) return out;
    std::map<int,int> tabWin, tabIdx, winSel, tabSelNav;
    std::map<int,std::map<int,std::pair<std::string,std::string>>> tabNav; // tab -> navIdx -> {domain,title}
    size_t pos=8;
    while(pos+2<=sz){
        uint16_t cs=(uint16_t)(b[pos]|(b[pos+1]<<8)); pos+=2; if(cs==0||pos+cs>sz)break;
        uint8_t id=b[pos]; const uint8_t* c=b+pos+1; size_t clen=(size_t)cs-1; pos+=cs;
        if(clen<4) continue;
        SnssPR pr{c+4,clen-4};                                   // skip 4-byte pickle header
        if(id==0){ int32_t w,t; if(pr.rInt(w)&&pr.rInt(t)) tabWin[t]=w; }
        else if(id==2){ int32_t t,ix; if(pr.rInt(t)&&pr.rInt(ix)) tabIdx[t]=ix; }
        else if(id==8){ int32_t w,ix; if(pr.rInt(w)&&pr.rInt(ix)) winSel[w]=ix; }
        else if(id==7){ int32_t t,ix; if(pr.rInt(t)&&pr.rInt(ix)) tabSelNav[t]=ix; }
        else if(id==6){ int32_t t,ni; std::string url,title;
            if(pr.rInt(t)&&pr.rInt(ni)&&pr.rStr(url)&&pr.rStr(title)){
                std::string d=etld1(hostOf(url));                // "" for chrome://, about:, etc.
                tabNav[t][ni]={d,title};
            }
        }
    }
    // group tabs by window
    std::map<int,std::vector<int>> winTabs; for(auto& kv:tabWin) winTabs[kv.second].push_back(kv.first);
    auto curNav=[&](int tab)->std::pair<std::string,std::string>{
        auto it=tabNav.find(tab); if(it==tabNav.end()||it->second.empty()) return {"",""};
        int sel = tabSelNav.count(tab)?tabSelNav[tab]:it->second.rbegin()->first;
        auto e=it->second.find(sel); if(e==it->second.end()) e=std::prev(it->second.end()); return e->second;
    };
    for(auto& kv:winTabs){ int w=kv.first; WinFp fp; fp.tabCount=0;
        int selIdx = winSel.count(w)?winSel[w]:-1; int activeTab=-1;
        for(int t:kv.second){ auto nav=curNav(t); if(!nav.first.empty()){ fp.counts[nav.first]++; } fp.tabCount++;
            if(tabIdx.count(t)&&tabIdx[t]==selIdx) activeTab=t; }
        if(activeTab<0 && !kv.second.empty()) activeTab=kv.second.front();
        auto an=curNav(activeTab); fp.activeTitle=an.second; fp.activeDomain=an.first;
        if(fp.tabCount>0) out.push_back(fp);
    }
    return out;
}
inline std::wstring FindChromiumSessionFile(const std::wstring& userDataDir){
    std::wstring dir=userDataDir+L"\\Default\\Sessions\\Session_*";
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(dir.c_str(),&fd); if(h==INVALID_HANDLE_VALUE) return L"";
    std::wstring best; ULARGE_INTEGER bestT={0};
    do{ if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        if(fd.nFileSizeLow==0&&fd.nFileSizeHigh==0) continue;
        ULARGE_INTEGER t; t.LowPart=fd.ftLastWriteTime.dwLowDateTime; t.HighPart=fd.ftLastWriteTime.dwHighDateTime;
        if(t.QuadPart>bestT.QuadPart){ bestT=t; best=userDataDir+L"\\Default\\Sessions\\"+fd.cFileName; }
    }while(FindNextFileW(h,&fd)); FindClose(h); return best;
}
inline std::vector<WinFp> ReadChromiumWindows(const std::wstring& userDataDir){
    std::wstring f=FindChromiumSessionFile(userDataDir); if(f.empty()) return {};
    std::string bytes; if(!ReadFileBytes(f,bytes)) return {};
    return ParseChromiumSNSS(bytes);
}
```

- [ ] **Step 4:** Run `build-test.bat` → all pass.
- [ ] **Step 5: Local validation (not committed):** temporarily add `if(cmd==L"snss"){ ... print domains from ReadChromiumWindows(chrome/edge user data) ... }` OR reuse the scratchpad spike; confirm real Chrome/Edge produce sensible per-window domains. Report result, then remove any temp code.
- [ ] **Step 6:** Commit: `git add src/session.hpp tests/vdtest.cpp && git commit -m "feat: Chromium SNSS session reader (TDD)"`

---

## Task 3: AppProfile + generalized collectors + multi-app wiring

**Files:** Create `src/appprofile.hpp`; modify `src/fingerprint.hpp`, `src/vde.cpp`.

**Interfaces — Produces:**
- `fingerprint.hpp`: add `std::string app;` to `Fp`.
- `appprofile.hpp`: `struct AppProfile { std::string id; std::vector<std::wstring> classNames; std::wstring exeName; std::vector<std::wstring> titleSuffixes; enum Sess{NONE,FIREFOX,CHROMIUM} session; std::wstring userDataDir; };` and `inline std::vector<AppProfile> BuiltinProfiles();` (firefox, chrome, msedge; each gated by a bool arg or global toggles).
- `vde.cpp`: `EnumAppWindows(profile)`, `BuildLiveFingerprintsFor(profile)`, updated `CollectPresentWindows`, `AnyAppPresent()`, per-app match filter in `RunRestore`.

- [ ] **Step 1:** In `fingerprint.hpp` add `std::string app;` field to `Fp`.
- [ ] **Step 2:** Create `appprofile.hpp` with the struct + `BuiltinProfiles()` returning Firefox (`MozillaWindowClass`/`firefox.exe`/FIREFOX), Chrome (`Chrome_WidgetWin_1`/`chrome.exe`/CHROMIUM, userDataDir `%LOCALAPPDATA%\Google\Chrome\User Data`), Edge (`Chrome_WidgetWin_1`/`msedge.exe`/CHROMIUM, `%LOCALAPPDATA%\Microsoft\Edge\User Data`), each with its title suffixes. Gate by the global enable flags `g_appFirefox/g_appChrome/g_appEdge` (default all true).
- [ ] **Step 3:** In `vde.cpp` generalize `EnumFirefoxWindows` → `EnumAppWindows(const AppProfile&)` (filter class ∈ classNames AND exe == profile.exeName; keep visible + titled). Generalize `StripFirefoxSuffix` → `StripSuffixes(title, profile.titleSuffixes)`. Generalize `BuildLiveFingerprints` → `BuildLiveFingerprintsFor(const AppProfile&)`: enumerate, read session windows (`profile.session==FIREFOX?ReadFirefoxWindows():profile.session==CHROMIUM?ReadChromiumWindows(profile.userDataDir):{}`), bind by stripped title (fallback: title-only fingerprint when no session match), tag `fp.app=profile.id`.
- [ ] **Step 4:** Update `CollectPresentWindows` to loop `for(auto&pr:BuiltinProfiles()) for(auto&f:BuildLiveFingerprintsFor(pr)){...w.app=f.app...}`. Replace `FirefoxPresent()` with `AnyAppPresent()` (any enabled profile has ≥1 window); update all callers (`RunGui` startup, `WM_TIMER`, `WM_DESTROY`). In the timer's seen-set accumulation and `g_observedApps`, insert each present app id (not hardcoded "firefox").
- [ ] **Step 5:** In `RunRestore`, the pairing already filters `saved[i].app!="firefox"`; change to match against the live window's app: build `live` (Fp with `app`), and when scoring only pair `saved[i].app==live[j].app`.
- [ ] **Step 6:** Build; **manual verify** with Chrome and/or Edge open across desktops: `build\vde.exe status` lists their windows; `save` writes `W\tchrome\t…`/`W\tmsedge\t…` rows; move a Chrome window, exit, relaunch → restored. Also confirm Firefox still works.
- [ ] **Step 7:** Commit: `git add src/appprofile.hpp src/fingerprint.hpp src/vde.cpp && git commit -m "feat: AppProfile + multi-app collectors (Firefox/Chrome/Edge)"`

---

## Task 4: Per-app enable checkboxes in Settings

**Files:** modify `src/vde.cpp`.

- [ ] **Step 1:** Add globals `g_appFirefox=true,g_appChrome=true,g_appEdge=true;` and persist in `LoadSettings/SaveSettings` (reg values `AppFirefox/AppChrome/AppEdge`). `BuiltinProfiles()` includes a profile only if its flag is set.
- [ ] **Step 2:** In `SettingsProc` add three checkboxes (ids `IDC_APP_FF=1004/IDC_APP_CR=1005/IDC_APP_ED=1006`) under a "Track these apps:" label; init from globals; apply on OK; enlarge the window height.
- [ ] **Step 3:** Build; manual verify toggles persist and disable tracking of the unchecked app.
- [ ] **Step 4:** Commit: `git add src/vde.cpp && git commit -m "feat: per-app tracking toggles in Settings"`

---

## Self-Review

- **Spec coverage:** R4 (other browsers / multi-window) → Tasks 2–4. Chrome/Edge SNSS domain fingerprint → Tasks 2–3; generic **title fallback** → Task 3 Step 3 (windows without session match). Deferred Phase-1 extraction of the Firefox session reader → Task 1.
- **Privacy:** real session files never committed; tests use synthetic fixtures (Task 2 Step 1); real-file check is local (Step 5).
- **Type consistency:** `WinFp` used by both readers; `Fp.app` set by `BuildLiveFingerprintsFor` and filtered in `RunRestore`; `LayoutWin.app` already carries it through save/restore/grace.
- **Placeholder scan:** SNSS reader + synthetic encoder shown in full; generalizations name exact functions to rename. No TBD.

## Notes / risk

- SNSS command IDs 0/2/6/7/8 are Chromium canon and stable; the reader skips anything it can't parse, so an unknown build degrades to title matching, never crashes.
- Generic arbitrary-exe apps (beyond the three browsers) are supported by the same title-matching path but a UI to add custom exes is left as a later enhancement.
