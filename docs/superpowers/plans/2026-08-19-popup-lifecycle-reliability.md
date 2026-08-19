# Popup and 30-Day Window Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (\`- [ ]\`) syntax for tracking.

**Goal:** Keep the desktop picker open across Ctrl+Click moves, clearly highlight the current desktop and active window, and replace the fragile three-run browser layout logic with an atomic, per-browser, 30-day lifecycle.

**Architecture:** Pure layout, lifecycle, move-queue, and picker-state modules own deterministic decisions and are tested by tests/vdtest.cpp. src/vde.cpp remains the Win32/COM orchestrator, while layout_store.hpp owns bounded reads, locking, backup, and atomic replacement; bounded asynchronous workers coalesce browser-session and reconciliation work so cold parsing and large matching graphs never block the UI thread.

**Tech Stack:** C++14, Win32/GDI, Windows virtual-desktop COM, Common Controls v6, MSVC 2022, the existing CHECK-based test executable.

---

## Execution prerequisites

- Read docs/superpowers/specs/2026-08-19-popup-lifecycle-reliability-design.md before Task 1.
- Use superpowers:using-git-worktrees before changing production code.
- Preserve unrelated user changes. The production-source baseline is `26481fc`;
  the approved design document is commit `e4ca435`.
- For every behavior below, run the stated RED command and observe the stated failure before adding production code.
- After each GREEN command, run the complete test executable, then make the listed focused commit.

## Final file map

- Modify src/layout.hpp — v4 records, strict parser, migration, retention, scoring, and one-to-one matching.
- Create src/layout_store.hpp — bounded file I/O, named transaction lock, backup, and atomic replace.
- Modify src/str_util.hpp — checked numeric/Base64/file helpers used by parsers.
- Modify src/session.hpp — strict SNSS framing, mozLz4 limits, stable file stamps, and session cache.
- Create src/session_worker.hpp — one bounded/coalesced status-bearing browser-session worker.
- Modify src/lifecycle.hpp — independent app settling and restore-before-save decisions.
- Create src/window_identity.hpp — shared exact runtime-window identity and validation.
- Create src/reconcile_worker.hpp — bounded/coalesced off-thread matching planner.
- Create src/move_queue.hpp — timer-driven move/verify state machine with four bounded attempts.
- Create src/picker_state.hpp — current/selected/active state, identity validation, color blending, and transition state.
- Create src/gdi_buffer.hpp — owned double-buffer DC/bitmap lifetime.
- Create src/icon_cache.hpp — bounded owned-icon cache with injectable copy/destroy operations.
- Modify src/vde.cpp — integrate storage, monitor, shutdown, move queue, popup, links, and cleanup.
- Modify tests/vdtest.cpp — all pure and Win32 resource regression tests.
- Modify README.md — exact 30-day, popup, and branding behavior.

### Task 1: Introduce layout v4 and strict parsing

**Files:**

- Modify: src/layout.hpp:9-115
- Modify: src/str_util.hpp:34-48
- Modify: src/vde.cpp — pass an explicit migration clock at existing parser call sites.
- Test: tests/vdtest.cpp:24-100

- [ ] **Step 1: Add a RED test proving serialization is still v3**

Add this test and call it from main:

~~~cpp
static void test_layout_serializes_v4_header(){
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string text = SerializeLayout(desks, wins);
    CHECK(text.rfind("# VDE snapshot v4\n", 0) == 0);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: FAIL at test_layout_serializes_v4_header because SerializeLayout emits v3.

- [ ] **Step 2: Add the v4 fields and make only the header test GREEN**

Replace the old missing-run constant and extend LayoutWin:

~~~cpp
using UnixSeconds = long long;
static const UnixSeconds WINDOW_RETENTION_SECONDS = 30LL * 24LL * 60LL * 60LL;
static const int MISSING_RUNS_MAX = 3; // transitional; removed with old monitor in Task 8

struct LayoutWin {
    std::string recordId;
    std::string app;
    int deskIndex = -1;
    GUID desktop = {0};
    std::string activeTitle;
    std::string activeDomain;
    int tabCount = 0;
    std::map<std::string,int> counts;
    UnixSeconds lastSeenUtc = 0;
    UnixSeconds missingSinceUtc = 0;
    int missingRuns = 0; // transitional compile bridge; ignored by v4, removed in Task 8
};
~~~

Retain `MISSING_RUNS_MAX` and the `missingRuns` member only as temporary
source-compatibility bridges while the old monitor still compiles. Neither the
v4 serializer nor any new test/logic may read or write them. Task 8 removes both
bridges together with the old monitor in one buildable cutover.

Change SerializeLayout to emit v4 and the exact 11 window columns:

~~~cpp
inline std::string SerializeLayout(const std::vector<DeskRec>& desks,
                                   const std::vector<LayoutWin>& wins){
    std::string out = "# VDE snapshot v4\n";
    for(const auto& d : desks){
        out += "D\t" + std::to_string(d.index) + "\t";
        out += W2U8(GuidToString(d.guid)) + "\t";
        out += b64enc(W2U8(d.name)) + "\n";
    }
    for(const auto& w : wins){
        out += "W\t" + w.app + "\t" + w.recordId + "\t";
        out += std::to_string(w.deskIndex) + "\t";
        out += W2U8(GuidToString(w.desktop)) + "\t";
        out += b64enc(w.activeTitle) + "\t" + w.activeDomain + "\t";
        out += std::to_string(w.tabCount) + "\t" + CountsToStr(w.counts) + "\t";
        out += std::to_string(w.lastSeenUtc) + "\t";
        out += std::to_string(w.missingSinceUtc) + "\n";
    }
    return out;
}
~~~

Update the existing round-trip fixture to populate `recordId`, `lastSeenUtc`, and
`missingSinceUtc` instead of `missingRuns`. Delete
`test_merge_upsert_and_keep`, `test_merge_adds_new`,
`test_grace_seen_resets_unseen_increments`, `test_grace_drops_at_threshold`, and
`test_grace_untouched_when_app_not_observed`, plus their calls from `main`;
Tasks 2 and 6 replace them with duplicate-safe fake-clock coverage.
Rename `test_layout_roundtrip_v3` to `test_layout_roundtrip_v4` and pass
`1800000000` plus an error string to its `ParseLayout` call. Update
`test_layout_parse_v2` to pass the same explicit migration time and assert
`lastSeenUtc == 1800000000`; no caller may rely on a zero/default migration
clock.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: the v4 header test passes; old parser/round-trip assertions still fail until the next step.

- [ ] **Step 3: Add RED strict-parser and migration tests**

Add these helpers and tests, then call each from main:

~~~cpp
static std::string V4Line(const std::string& guid,
                          const std::string& recordId,
                          const std::string& lastSeen,
                          const std::string& missing){
    return "# VDE snapshot v4\n"
           "W\tfirefox\t" + recordId + "\t1\t" + guid + "\t" +
           b64enc("Inbox") + "\tmail.example\t1\tmail.example:1\t" +
           lastSeen + "\t" + missing + "\n";
}

static void test_layout_v4_roundtrip_fields(){
    LayoutWin w = LW("firefox", 2, {{"github.com", 2}}, "PR");
    w.desktop = G(L"{231A0000-0000-0000-0000-000000000003}");
    w.recordId = "{00000000-0000-0000-0000-000000000101}";
    w.lastSeenUtc = 1700000000;
    w.missingSinceUtc = 1700000100;
    std::vector<DeskRec> d;
    std::vector<LayoutWin> parsed;
    std::string error;
    CHECK(ParseLayout(SerializeLayout(d, {w}), d, parsed, 1800000000, &error));
    CHECK(parsed.size() == 1);
    CHECK(parsed[0].recordId == w.recordId);
    CHECK(parsed[0].lastSeenUtc == 1700000000);
    CHECK(parsed[0].missingSinceUtc == 1700000100);
}

static void test_layout_rejects_invalid_guid(){
    std::vector<DeskRec> d;
    std::vector<LayoutWin> w;
    std::string error;
    CHECK(!ParseLayout(V4Line("not-a-guid",
                              "{00000000-0000-0000-0000-000000000101}",
                              "1700000000", "0"),
                       d, w, 1800000000, &error));
    CHECK(!error.empty());
    CHECK(d.empty() && w.empty());
}

static void test_layout_rejects_invalid_integer(){
    std::vector<DeskRec> d;
    std::vector<LayoutWin> w;
    std::string error;
    CHECK(!ParseLayout(V4Line("{231A0000-0000-0000-0000-000000000001}",
                              "{00000000-0000-0000-0000-000000000101}",
                              "1700000000junk", "0"),
                       d, w, 1800000000, &error));
    CHECK(d.empty() && w.empty());
}

static void test_layout_migrates_v3_with_full_retention(){
    std::string v3 = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" +
        b64enc("PR") + "\tgithub.com\t1\tgithub.com:1\t2\n";
    std::vector<DeskRec> d;
    std::vector<LayoutWin> w;
    std::string error;
    CHECK(ParseLayout(v3, d, w, 1800000000, &error));
    CHECK(w.size() == 1);
    CHECK(!w[0].recordId.empty());
    CHECK(w[0].lastSeenUtc == 1800000000);
    CHECK(w[0].missingSinceUtc == 1800000000);
}

static void test_layout_rejects_trailing_columns_and_duplicate_ids(){
    const std::string guid = "{231A0000-0000-0000-0000-000000000001}";
    const std::string id = "{00000000-0000-0000-0000-000000000101}";
    std::vector<DeskRec> d;
    std::vector<LayoutWin> w;
    std::string error;
    CHECK(!ParseLayout(V4Line(guid, id, "1700000000", "0") + "junk\n",
                       d, w, 1800000000, &error));
    CHECK(!ParseLayout(V4Line(guid, id, "1700000000", "0") +
                       V4Line(guid, id, "1700000001", "0").substr(18),
                       d, w, 1800000000, &error));
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure for the new ParseLayout signature or assertion failures because v4 validation/migration does not exist.

- [ ] **Step 4: Implement strict helpers and transactional ParseLayout**

Add checked helpers to str_util.hpp:

~~~cpp
#include <cerrno>
#include <climits>

inline bool ParseI64Strict(const std::string& s, long long& out){
    if(s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long value = _strtoi64(s.c_str(), &end, 10);
    if(errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = value;
    return true;
}

inline bool ParseIntStrict(const std::string& s, int& out){
    long long value = 0;
    if(!ParseI64Strict(s, value) || value < INT_MIN || value > INT_MAX) return false;
    out = static_cast<int>(value);
    return true;
}

inline bool b64decStrict(const std::string& in, std::string& out){
    if(in.size() % 4 != 0) return false;
    for(size_t i = 0; i < in.size(); ++i){
        unsigned char c = static_cast<unsigned char>(in[i]);
        bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '/';
        bool padding = c == '=' && i >= in.size() - 2;
        if(!alphabet && !padding) return false;
        if(c == '=' && i + 1 < in.size() && in[i + 1] != '=') return false;
    }
    out = b64dec(in);
    return true;
}

inline bool ParseCountsStrict(const std::string& text,
                              std::map<std::string,int>& counts){
    std::map<std::string,int> parsed;
    if(text.empty()){
        counts.clear();
        return true;
    }
    size_t pos = 0;
    while(pos < text.size()){
        size_t comma = text.find(',', pos);
        std::string item = text.substr(
            pos, (comma == std::string::npos ? text.size() : comma) - pos);
        size_t colon = item.rfind(':');
        int count = 0;
        if(colon == std::string::npos || colon == 0 ||
           !ParseIntStrict(item.substr(colon + 1), count) || count <= 0)
            return false;
        std::string domain = item.substr(0, colon);
        if(parsed.count(domain)) return false;
        parsed[domain] = count;
        if(comma == std::string::npos) break;
        pos = comma + 1;
    }
    counts.swap(parsed);
    return true;
}
~~~

Add these declarations/helpers to layout.hpp and replace ParseLayout with a transactional parser:

~~~cpp
inline std::string NewRecordId(){
    GUID guid = {0};
    if(FAILED(CoCreateGuid(&guid))) return "";
    return W2U8(GuidToString(guid));
}

inline bool ParseLayout(const std::string& data,
                        std::vector<DeskRec>& desksOut,
                        std::vector<LayoutWin>& winsOut,
                        UnixSeconds migrationNow,
                        std::string* errorOut = nullptr,
                        int* sourceVersionOut = nullptr){
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::set<std::string> recordIds;
    int version = 0;
    bool headerSeen = false;
    size_t recordCount = 0;
    static const size_t MAX_LAYOUT_RECORDS = 4096;
    size_t pos = 0;
    auto fail = [&](const std::string& message){
        if(errorOut) *errorOut = message;
        return false;
    };

    while(pos < data.size()){
        size_t nl = data.find('\n', pos);
        std::string line = data.substr(pos, (nl == std::string::npos ? data.size() : nl) - pos);
        pos = nl == std::string::npos ? data.size() : nl + 1;
        if(!line.empty() && line.back() == '\r') line.pop_back();
        if(line.empty()) continue;
        if(line[0] == '#'){
            if(headerSeen || recordCount != 0) return fail("duplicate layout header");
            if(line == "# VDE snapshot v2") version = 2;
            else if(line == "# VDE snapshot v3") version = 3;
            else if(line == "# VDE snapshot v4") version = 4;
            else return fail("unsupported layout header");
            headerSeen = true;
            continue;
        }
        if(version == 0) return fail("missing layout header");
        if(++recordCount > MAX_LAYOUT_RECORDS) return fail("too many layout records");

        std::vector<std::string> col;
        size_t field = 0;
        for(;;){
            size_t tab = line.find('\t', field);
            col.push_back(line.substr(field, (tab == std::string::npos ? line.size() : tab) - field));
            if(tab == std::string::npos) break;
            field = tab + 1;
        }

        if(col[0] == "D"){
            if(col.size() != 4) return fail("invalid desktop field count");
            DeskRec d;
            std::string name;
            if(!ParseIntStrict(col[1], d.index) || !StringToGuid(U82W(col[2]), d.guid) ||
               GuidIsZero(d.guid) || !b64decStrict(col[3], name))
                return fail("invalid desktop record");
            d.name = U82W(name);
            desks.push_back(d);
            continue;
        }
        if(col[0] != "W") return fail("unknown layout record");

        LayoutWin w;
        if(version == 4){
            if(col.size() != 11) return fail("invalid v4 window field count");
            long long lastSeen = 0, missing = 0;
            if(col[1] != "firefox" && col[1] != "chrome" && col[1] != "msedge")
                return fail("invalid v4 identity");
            GUID recordGuid = {0};
            if(!StringToGuid(U82W(col[2]), recordGuid) || GuidIsZero(recordGuid) ||
               recordIds.count(col[2]) != 0 ||
               !ParseIntStrict(col[3], w.deskIndex) ||
               !StringToGuid(U82W(col[4]), w.desktop) || GuidIsZero(w.desktop) ||
               !b64decStrict(col[5], w.activeTitle) ||
               !ParseIntStrict(col[7], w.tabCount) || w.tabCount < 0 ||
               !ParseCountsStrict(col[8], w.counts) ||
               !ParseI64Strict(col[9], lastSeen) ||
               !ParseI64Strict(col[10], missing) ||
               lastSeen <= 0 || missing < 0)
                return fail("invalid v4 window record");
            w.app = col[1];
            w.recordId = col[2];
            w.activeDomain = col[6];
            w.lastSeenUtc = lastSeen;
            w.missingSinceUtc = missing;
            recordIds.insert(w.recordId);
        } else {
            size_t expected = version == 3 ? 9 : 7;
            if(col.size() != expected || migrationNow <= 0)
                return fail("invalid legacy window record");
            int offset = version == 3 ? 1 : 0;
            w.app = version == 3 ? col[1] : "firefox";
            if(w.app != "firefox" && w.app != "chrome" && w.app != "msedge")
                return fail("invalid legacy app");
            if(!ParseIntStrict(col[1 + offset], w.deskIndex) ||
               !StringToGuid(U82W(col[2 + offset]), w.desktop) ||
               GuidIsZero(w.desktop) ||
               !b64decStrict(col[3 + offset], w.activeTitle))
                return fail("invalid legacy window record");
            w.activeDomain = col[4 + offset];
            if(!ParseIntStrict(col[5 + offset], w.tabCount) || w.tabCount < 0)
                return fail("invalid legacy tab count");
            if(!ParseCountsStrict(col[6 + offset], w.counts))
                return fail("invalid legacy counts");
            int oldMissing = 0;
            if(version == 3 && !ParseIntStrict(col[8], oldMissing))
                return fail("invalid legacy missing count");
            w.recordId = NewRecordId();
            if(w.recordId.empty()) return fail("record id generation failed");
            w.lastSeenUtc = migrationNow;
            w.missingSinceUtc = oldMissing > 0 ? migrationNow : 0;
            recordIds.insert(w.recordId);
        }
        wins.push_back(w);
    }

    if(version == 0) return fail("empty layout");
    desksOut.swap(desks);
    winsOut.swap(wins);
    if(sourceVersionOut) *sourceVersionOut = version;
    if(errorOut) errorOut->clear();
    return true;
}
~~~

Add `UtcNowSeconds` to `vde.cpp` now (using `GetSystemTimeAsFileTime`) and update
every existing production `ParseLayout` call to pass that value explicitly.
Do not add a default/legacy overload: migration time must always be visible at
the call site. Task 8 reuses this helper rather than defining it again.

Before any Tasks 1-7 legacy producer calls `SerializeLayout`, normalize its
mutable local records transactionally:

~~~cpp
static bool PrepareTransitionalV4Records(std::vector<LayoutWin>& records,
                                          UnixSeconds nowUtc){
    std::vector<LayoutWin> prepared = records;
    for(LayoutWin& record : prepared){
        if(record.recordId.empty()) record.recordId = NewRecordId();
        if(record.recordId.empty() || record.lastSeenUtc < 0 ||
           GuidIsZero(record.desktop)) return false;
        if(record.lastSeenUtc == 0) record.lastSeenUtc = nowUtc;
        if(record.missingRuns > 0 && record.missingSinceUtc == 0)
            record.missingSinceUtc = nowUtc;
    }
    records.swap(prepared);
    return true;
}
~~~

Apply this to automatic, manual, and CLI save vectors; on failure abort without
touching the existing file. Add a regression that an old-style record with only
app/desktop/fingerprint gains a stable ID/nonzero timestamp, serializes, and
parses back, plus a zero-desktop/ID-generation failure that leaves the prior
file byte-identical. This bridge prevents any intermediate build from emitting
a v4 file that its own strict parser rejects; Task 8 deletes the bridge after
all producers natively populate v4 fields.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0, no FAIL lines, and the final passed count is greater than 65.

- [ ] **Step 5: Commit layout v4**

~~~powershell
git add src/layout.hpp src/str_util.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(layout): add strict v4 format"
~~~

### Task 2: Add exact retention and duplicate-safe matching

**Files:**

- Modify: src/layout.hpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED retention boundary tests**

~~~cpp
static void test_retention_keeps_29_days(){
    const UnixSeconds now = 2000000000;
    LayoutWin w = LW("firefox", 0, {{"a.com", 1}});
    w.recordId = "{00000000-0000-0000-0000-000000000201}";
    w.missingSinceUtc = now - WINDOW_RETENTION_SECONDS + 1;
    CHECK(!IsExpired(w, now));
    CHECK(PruneExpired({w}, now).size() == 1);
}

static void test_retention_expires_exactly_30_days(){
    const UnixSeconds now = 2000000000;
    LayoutWin w = LW("firefox", 0, {{"a.com", 1}});
    w.recordId = "{00000000-0000-0000-0000-000000000202}";
    w.missingSinceUtc = now - WINDOW_RETENTION_SECONDS;
    CHECK(IsExpired(w, now));
    CHECK(PruneExpired({w}, now).empty());
}

static void test_reappearance_clears_missing_before_expiry(){
    const UnixSeconds now = 2000000000;
    LayoutWin w = LW("firefox", 0, {{"a.com", 1}});
    w.recordId = "{00000000-0000-0000-0000-000000000203}";
    w.missingSinceUtc = now - 10;
    MarkSeen(w, now);
    CHECK(w.lastSeenUtc == now);
    CHECK(w.missingSinceUtc == 0);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure C3861 for IsExpired, PruneExpired, or MarkSeen.

- [ ] **Step 2: Implement retention helpers**

~~~cpp
inline bool IsExpired(const LayoutWin& w, UnixSeconds nowUtc){
    return w.missingSinceUtc > 0 && nowUtc >= w.missingSinceUtc &&
           nowUtc - w.missingSinceUtc >= WINDOW_RETENTION_SECONDS;
}

inline void MarkSeen(LayoutWin& w, UnixSeconds nowUtc){
    w.lastSeenUtc = nowUtc;
    w.missingSinceUtc = 0;
}

inline void MarkMissing(LayoutWin& w, UnixSeconds nowUtc){
    if(w.missingSinceUtc == 0)
        w.missingSinceUtc = w.lastSeenUtc > 0 ? w.lastSeenUtc : nowUtc;
}

inline std::vector<LayoutWin> PruneExpired(const std::vector<LayoutWin>& input,
                                           UnixSeconds nowUtc){
    std::vector<LayoutWin> output;
    for(const auto& w : input)
        if(!IsExpired(w, nowUtc)) output.push_back(w);
    return output;
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: all retention tests pass.

- [ ] **Step 3: Add RED duplicate matching tests**

~~~cpp
static LayoutWin Identified(const char* id, const char* app, int desk,
                            std::map<std::string,int> counts, const char* title){
    LayoutWin w = LW(app, desk, counts, title);
    w.recordId = id;
    return w;
}

static void test_matching_keeps_duplicate_fingerprints_one_to_one(){
    std::vector<LayoutWin> saved = {
        Identified("{00000000-0000-0000-0000-000000000211}", "firefox", 0,
                   {{"same.com", 1}}, "Same"),
        Identified("{00000000-0000-0000-0000-000000000212}", "firefox", 1,
                   {{"same.com", 1}}, "Same")
    };
    std::vector<LayoutWin> live = {
        LW("firefox", 3, {{"same.com", 1}}, "Same"),
        LW("firefox", 4, {{"same.com", 1}}, "Same")
    };
    auto pairs = MatchOneToOne(saved, live, 0.55);
    CHECK(pairs.size() == 2);
    CHECK(pairs[0].savedIndex != pairs[1].savedIndex);
    CHECK(pairs[0].liveIndex != pairs[1].liveIndex);
}

static void test_matching_never_crosses_apps(){
    std::vector<LayoutWin> saved = {
        Identified("{00000000-0000-0000-0000-000000000213}", "firefox", 0,
                   {{"same.com", 1}}, "Same")
    };
    std::vector<LayoutWin> live = {
        LW("chrome", 1, {{"same.com", 1}}, "Same")
    };
    CHECK(MatchOneToOne(saved, live, 0.55).empty());
}

static void test_assignment_does_not_let_best_edge_block_two_matches(){
    std::vector<LayoutMatch> candidates = {
        {0, 0, 0.90}, {0, 1, 0.80}, {1, 0, 0.85}
    };
    auto pairs = AssignOneToOne(2, 2, candidates);
    CHECK(pairs.size() == 2);
    CHECK(std::any_of(pairs.begin(), pairs.end(), [](const LayoutMatch& p){
        return p.savedIndex == 0 && p.liveIndex == 1;
    }));
    CHECK(std::any_of(pairs.begin(), pairs.end(), [](const LayoutMatch& p){
        return p.savedIndex == 1 && p.liveIndex == 0;
    }));
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure C3861 because MatchOneToOne is not defined.

- [ ] **Step 4: Move scoring into layout.hpp and implement deterministic assignment**

Add direct `#include <algorithm>`, `#include <cmath>`, `#include <limits>`, and
`#include <queue>` to `layout.hpp`; the header must compile without relying on
include order in `vde.cpp` or the tests.

~~~cpp
struct LayoutMatch {
    size_t savedIndex = 0;
    size_t liveIndex = 0;
    double score = 0;
};

inline double LayoutScore(const LayoutWin& saved, const LayoutWin& live){
    if(saved.app != live.app) return 0;
    if(!saved.counts.empty() && !live.counts.empty()){
        double dot = 0, na = 0, nb = 0;
        for(const auto& kv : saved.counts){
            na += double(kv.second) * kv.second;
            auto it = live.counts.find(kv.first);
            if(it != live.counts.end()) dot += double(kv.second) * it->second;
        }
        for(const auto& kv : live.counts) nb += double(kv.second) * kv.second;
        double cosine = na && nb ? dot / (std::sqrt(na) * std::sqrt(nb)) : 0;
        std::set<std::string> domains;
        int intersection = 0;
        for(const auto& kv : saved.counts) domains.insert(kv.first);
        for(const auto& kv : live.counts){
            if(saved.counts.count(kv.first)) ++intersection;
            domains.insert(kv.first);
        }
        double jaccard = domains.empty() ? 0 : double(intersection) / domains.size();
        double active = saved.activeTitle == live.activeTitle && !saved.activeTitle.empty()
            ? 1.0
            : (!saved.activeDomain.empty() && saved.activeDomain == live.activeDomain ? 0.5 : 0.0);
        int maximum = std::max(std::max(saved.tabCount, live.tabCount), 1);
        double tabs = 1.0 - std::min(1.0,
            std::abs(saved.tabCount - live.tabCount) / double(maximum));
        return 0.40 * cosine + 0.25 * jaccard + 0.20 * active + 0.15 * tabs;
    }
    return !saved.activeTitle.empty() && saved.activeTitle == live.activeTitle ? 1.0 : 0.0;
}

inline std::vector<LayoutMatch> AssignOneToOne(
        size_t savedCount, size_t liveCount,
        const std::vector<LayoutMatch>& inputCandidates,
        bool* tooComplex = nullptr);

inline std::vector<LayoutMatch> MatchOneToOne(const std::vector<LayoutWin>& saved,
                                               const std::vector<LayoutWin>& live,
                                               double acceptScore,
                                               bool* tooComplex = nullptr){
    static const size_t MAX_MATCH_CANDIDATES = 8192;
    if(tooComplex) *tooComplex = false;
    std::vector<LayoutMatch> candidates;
    for(size_t s = 0; s < saved.size(); ++s)
        for(size_t l = 0; l < live.size(); ++l){
            double score = LayoutScore(saved[s], live[l]);
            if(score >= acceptScore){
                if(candidates.size() == MAX_MATCH_CANDIDATES){
                    if(tooComplex) *tooComplex = true;
                    return {};
                }
                candidates.push_back({s, l, score});
            }
        }
    return AssignOneToOne(saved.size(), live.size(), candidates, tooComplex);
}
~~~

Implement `AssignOneToOne` as a deterministic min-cost bipartite flow, not as
the previous greedy scan:

1. Validate every candidate index, discard non-finite/negative scores, cap the
   graph at 8,192 edges, scale scores with checked
   `llround(score * 1'000'000'000.0)`, and sort edges by
   `{savedIndex, liveIndex}`.
2. First compute maximum cardinality `K` with a standard augmenting-path
   bipartite matcher over the candidate graph.
3. Build `source -> saved -> live -> sink` capacity-one edges and send exactly
   `K` units with a checked min-cost flow implementation. Represent path cost
   as a lexicographic pair `{negativeScoreUnits, deterministicTieSum}`; never
   multiply score by graph size. Use checked additions and deterministic lower
   edge indices on equal pairs.
4. Return saturated saved/live edges, sorted by `{savedIndex, liveIndex}`.

This objective is maximum-cardinality first, maximum-total-score second, and
stable index order last. Add randomized small-graph tests comparing it with an
exhaustive assignment oracle in addition to the blocking-candidate regression.
Add a dense graph just over the cap and near-limit score test: it must report
`tooComplex` quickly without overflow, matching, missing-marking, or mutation.

Remove the duplicate Score implementation from vde.cpp only when Task 8 switches restore to LayoutScore.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0 and both duplicate tests pass.

- [ ] **Step 5: Commit retention and matching**

~~~powershell
git add src/layout.hpp tests/vdtest.cpp
git commit -m "fix(layout): add 30-day retention"
~~~

### Task 3: Add bounded reads and failure-atomic layout storage

**Files:**

- Create: src/layout_store.hpp
- Modify: src/str_util.hpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED status and fault-injection tests**

Add temp-directory fixtures and an injected `LayoutFsOps` seam around open,
read, write, flush, replace, move, copy, delete, and attribute queries. Test:

- a file above 16 MiB is rejected before allocation;
- a partial/error read never publishes partial bytes;
- missing primary and backup returns `Missing`;
- valid primary returns `Valid`;
- corrupt primary plus valid backup returns `Recovered`, keeps the backup, and
  preserves the corrupt primary diagnostically;
- two confirmed corrupt files return `CorruptPreserved` only after both
  existing byte streams were copied successfully;
- an injected transient primary-open error returns `Unavailable`, not empty or
  backup recovery;
- an injected diagnostic-copy failure returns `Unavailable` and sets
  `writesAllowed == false`;
- an injected atomic replace/backup failure returns false, leaves a readable
  old primary/recovery copy, makes the caller keep dirty state, and proves the
  loader prefers the retained `.rollback` over an older `.bak`;
- two successful writes produce new primary plus previous primary in `.bak`;
- recovery write with `preserveExistingBackup == true` does not replace the
  known-good backup.

Expected RED result: current boolean load and unchecked Copy/rotation paths
cannot distinguish these outcomes.

- [ ] **Step 2: Implement bounded, status-bearing reads**

Replace the compatibility reader used by layouts with:

~~~cpp
enum class FileReadStatus { Ok, Missing, Unavailable, TooLarge };

struct FileReadResult {
    FileReadStatus status = FileReadStatus::Unavailable;
    std::string bytes;
    DWORD win32Error = ERROR_SUCCESS;
    std::string error;
};

FileReadResult ReadFileBytesBounded(const std::wstring& path,
                                    unsigned long long limit);
~~~

Return `Missing` only for `ERROR_FILE_NOT_FOUND` or
`ERROR_PATH_NOT_FOUND`. Sharing violations, access errors, short reads,
attribute/size failures, and files that grow during reading are
`Unavailable`; never swap partial data into the result. Read in fixed 64 KiB
chunks and enforce the limit both before and during the loop.

Keep a thin boolean compatibility wrapper only for non-layout legacy callers;
all layout decisions use `FileReadStatus` directly.

- [ ] **Step 3: Implement failure-atomic write and explicit load states**

Make `layout_store.hpp` self-contained with direct `layout.hpp`, `<map>`,
`<set>`, `<string>`, `<vector>`, and `<windows.h>` includes. Create:

~~~cpp
static const unsigned long long MAX_LAYOUT_FILE_BYTES =
    16ULL * 1024ULL * 1024ULL;

enum class LayoutLoadStatus {
    Missing, Valid, Recovered, CorruptPreserved, Unavailable
};

struct LayoutRevision {
    std::wstring sourcePath;
    unsigned long long size = 0;
    unsigned long long mtime = 0;
    uint64_t contentHash = 0;
    bool exists = false;
};

struct LayoutLoadResult {
    LayoutLoadStatus status = LayoutLoadStatus::Unavailable;
    bool writesAllowed = false;
    int sourceVersion = 0;
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    LayoutRevision revision;
    std::string error;
    bool usable() const {
        return status == LayoutLoadStatus::Valid ||
               status == LayoutLoadStatus::Recovered;
    }
};

class ScopedLayoutLock {
public:
    explicit ScopedLayoutLock(DWORD timeoutMs = 0);
    ~ScopedLayoutLock();
    bool acquired() const;
    ScopedLayoutLock(const ScopedLayoutLock&) = delete;
    ScopedLayoutLock& operator=(const ScopedLayoutLock&) = delete;
private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

bool AtomicWriteText(const std::wstring& path, const std::string& data,
                     std::string* errorOut = nullptr,
                     bool preserveExistingBackup = false);

LayoutLoadResult LoadLayoutWithBackup(const std::wstring& path,
                                      UnixSeconds nowUtc,
                                      DWORD lockTimeoutMs = 0);

LayoutLoadResult LoadLayoutWithBackupLocked(const std::wstring& path,
                                            UnixSeconds nowUtc);
LayoutRevision ReadLayoutRevisionLocked(const std::wstring& path);
bool SameRevision(const LayoutRevision& a, const LayoutRevision& b);
~~~

Implement `SameRevision` as an equality check over source path, existence,
size, mtime, and content hash. Add one test per differing field so no partial
revision comparison can admit a stale write.

Keep one outer `ScopedLayoutLock` for an entire read/modify/write transaction;
`AtomicWriteText` itself never takes that mutex.
The tray/UI always passes a zero/short timeout and retries from a timer instead
of blocking the message loop; CLI-only transactions may pass 5000 ms. The lock
owns a named mutex handle, treats `WAIT_ABANDONED` as acquired, releases only
when acquired, and closes its handle on every path.

The public read-only `LoadLayoutWithBackup` acquires once and delegates to
`LoadLayoutWithBackupLocked`; code that will modify uses an already-acquired
`ScopedLayoutLock` and calls only the Locked form before `AtomicWriteText`.
Never call the locking wrapper while holding the mutex. Return a revision
(primary/recovery path, size, mtime, and content hash). Long-lived automatic
state compares that revision again under the write lock; if another actor
changed it, abort the write, reload/reconcile, and keep dirty rather than losing
the other update. Add a two-actor test where B writes between A's load/save and
A is rejected without overwriting B.

`AtomicWriteText` writes and flushes `path.tmp`, then:

- when primary exists and the known backup may rotate, first resolve any prior
  `path.rollback` into `path.bak` or fail before touching primary. Call
  `ReplaceFileW` with `path.rollback` as its backup filename and
  `REPLACEFILE_WRITE_THROUGH`, then promote rollback to `.bak` with checked
  write-through `MoveFileExW`;
- if that promotion fails, return false, keep `path.rollback`, and leave the
  caller dirty. Recovery reads `.rollback` before `.bak`, so the latest prior
  primary is never stranded in an ignored filename;
- when recovering from a known-good backup, use a separate displaced name as
  the ReplaceFile backup parameter, keep the existing `.bak`, and delete only
  that disposable artifact after success;
- when no primary exists, use write-through `MoveFileExW`;
- on any required write/flush/replace/move/backup verification failure, return
  false, preserve every recovery artifact, populate the error, and leave the
  caller dirty for retry.

After a normal replacement, verify that both the new primary and backup are
readable before reporting success. A cleanup failure for an extra artifact is
reported but never triggers a broader delete.

`LoadLayoutWithBackup` treats a valid `.rollback` as the first recovery
candidate, then `.bak`, and follows this decision table:

| Primary | Backup | Result |
|---|---|---|
| missing | missing | `Missing`, writes allowed |
| valid | any | `Valid`, writes allowed |
| transient/unreadable | any | `Unavailable`, writes blocked |
| corrupt | valid | preserve corrupt primary, then `Recovered` |
| corrupt | transient/unreadable | `Unavailable`, writes blocked |
| corrupt | missing/corrupt | preserve every corrupt existing file, then `CorruptPreserved` |
| missing | valid | `Recovered` |
| missing | transient/corrupt | `Unavailable` unless corrupt bytes were safely preserved |

Every diagnostic `CopyFileW` result is checked and the copied bytes are read
back/size-checked. Diagnostic names include UTC plus a collision-safe counter or
GUID and never overwrite an earlier diagnostic. If preservation fails, return `Unavailable` with
`writesAllowed == false`; no later auto/manual save may replace the source.

Only `Missing` and confirmed `CorruptPreserved` may initialize an empty
layout. `Unavailable` pauses automatic mutation and retries later. `Recovered`
loads the backup and sets the next write to preserve that backup.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: all read-status, transient-open, diagnostic-copy, rotation, recovery,
and size-boundary tests pass.

- [ ] **Step 4: Commit atomic storage**

~~~powershell
git add src/layout_store.hpp src/str_util.hpp tests/vdtest.cpp
git commit -m "fix(store): make layout writes failure-atomic"
~~~
### Task 4: Harden browser session input and add cache metadata

**Files:**

- Modify: src/session.hpp
- Create: src/session_worker.hpp
- Modify: src/vde.cpp:152-265,350-424
- Test: tests/vdtest.cpp:127-163

- [ ] **Step 1: Add RED malformed SNSS and mozLz4 limit tests**

~~~cpp
static void test_snss_truncated_frame_returns_no_partial_windows(){
    std::string bytes = makeSnss();
    bytes.resize(bytes.size() - 2);
    std::vector<WinFp> windows = {WinFp{}};
    CHECK(!ParseChromiumSNSS(bytes, windows));
    CHECK(windows.empty());
}

static void test_mozlz4_rejects_huge_declared_output(){
    std::string bytes("mozLz40\0", 8);
    bytes.push_back(char(0xff));
    bytes.push_back(char(0xff));
    bytes.push_back(char(0xff));
    bytes.push_back(char(0x7f));
    std::string output;
    CHECK(!MozLz4Decompress(bytes, 512ULL * 1024ULL * 1024ULL, output));
    CHECK(output.empty());
}

static void test_session_stamp_detects_change(){
    SessionStamp a;
    a.size = 10;
    a.mtime = 20;
    SessionStamp b = a;
    CHECK(a == b);
    b.mtime = 21;
    CHECK(a != b);
}

static void test_firefox_json_rejects_trailing_and_excessive_depth(){
    JValue value;
    CHECK(!JParser("{} trailing").parse(value));
    std::string deep(129, '[');
    deep += "null";
    deep += std::string(129, ']');
    CHECK(!JParser(deep).parse(value));
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: truncated SNSS assertion fails and MozLz4Decompress/SessionStamp are undefined.

Move `JValue` from `vde.cpp` to `session.hpp`, add `<cerrno>`, `<cmath>`, and
`<cstdlib>`, and replace
`JParser` with this strict bounded implementation:

~~~cpp
static const unsigned MAX_JSON_DEPTH = 128;
static const size_t MAX_JSON_NODES = 2000000;
static const size_t MAX_JSON_DECODED_STRING_BYTES = 256ULL * 1024ULL * 1024ULL;

struct JParser {
    const char* p;
    const char* end;
    bool ok = true;
    explicit JParser(const std::string& input)
        : p(input.data()), end(input.data() + input.size()) {}

    void ws(){
        while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool parse(JValue& valueOut){
        ws();
        bool parsed = value(valueOut, 0);
        ws();
        return parsed && ok && p == end;
    }
    void appendUtf8(std::string& output, unsigned codePoint){
        if(codePoint <= 0x7f) output.push_back(static_cast<char>(codePoint));
        else if(codePoint <= 0x7ff){
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if(codePoint <= 0xffff){
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }
    bool hex4(unsigned& valueOut){
        if(end - p < 4){ ok = false; return false; }
        unsigned value = 0;
        for(int i = 0; i < 4; ++i){
            char c = *p++;
            value <<= 4;
            if(c >= '0' && c <= '9') value |= c - '0';
            else if(c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if(c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else { ok = false; return false; }
        }
        valueOut = value;
        return true;
    }
    bool str(std::string& output){
        if(p >= end || *p != '"'){ ok = false; return false; }
        ++p;
        while(p < end){
            unsigned char byte = static_cast<unsigned char>(*p++);
            if(byte == '"') return true;
            if(byte < 0x20){ ok = false; return false; }
            if(byte != '\\'){
                output.push_back(static_cast<char>(byte));
                continue;
            }
            if(p >= end){ ok = false; return false; }
            char escaped = *p++;
            switch(escaped){
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    unsigned codePoint = 0;
                    if(!hex4(codePoint)) return false;
                    if(codePoint >= 0xd800 && codePoint <= 0xdbff){
                        if(end - p < 2 || p[0] != '\\' || p[1] != 'u'){
                            ok = false; return false;
                        }
                        p += 2;
                        unsigned low = 0;
                        if(!hex4(low) || low < 0xdc00 || low > 0xdfff){
                            ok = false; return false;
                        }
                        codePoint = 0x10000 + ((codePoint - 0xd800) << 10) +
                                    (low - 0xdc00);
                    } else if(codePoint >= 0xdc00 && codePoint <= 0xdfff){
                        ok = false; return false;
                    }
                    appendUtf8(output, codePoint);
                    break;
                }
                default: ok = false; return false;
            }
        }
        ok = false;
        return false;
    }
    bool number(JValue& valueOut){
        const char* start = p;
        if(p < end && *p == '-') ++p;
        if(p >= end){ ok = false; return false; }
        if(*p == '0') ++p;
        else if(*p >= '1' && *p <= '9') while(p < end && *p >= '0' && *p <= '9') ++p;
        else { ok = false; return false; }
        if(p < end && *p == '.'){
            ++p;
            const char* fraction = p;
            while(p < end && *p >= '0' && *p <= '9') ++p;
            if(p == fraction){ ok = false; return false; }
        }
        if(p < end && (*p == 'e' || *p == 'E')){
            ++p;
            if(p < end && (*p == '+' || *p == '-')) ++p;
            const char* exponent = p;
            while(p < end && *p >= '0' && *p <= '9') ++p;
            if(p == exponent){ ok = false; return false; }
        }
        std::string token(start, p);
        char* convertedEnd = nullptr;
        errno = 0;
        double converted = strtod(token.c_str(), &convertedEnd);
        if(errno == ERANGE || convertedEnd != token.c_str() + token.size() ||
           !std::isfinite(converted)){
            ok = false; return false;
        }
        valueOut.t = JValue::NUM;
        valueOut.num = converted;
        return true;
    }
    bool value(JValue& valueOut, unsigned depth){
        if(depth > MAX_JSON_DEPTH){ ok = false; return false; }
        ws();
        if(p >= end){ ok = false; return false; }
        if(*p == '"'){ valueOut.t = JValue::STR; return str(valueOut.str); }
        if(*p == '{') return object(valueOut, depth);
        if(*p == '[') return array(valueOut, depth);
        if(end - p >= 4 && memcmp(p, "true", 4) == 0){
            p += 4; valueOut.t = JValue::BOOL; valueOut.b = true; return true;
        }
        if(end - p >= 5 && memcmp(p, "false", 5) == 0){
            p += 5; valueOut.t = JValue::BOOL; valueOut.b = false; return true;
        }
        if(end - p >= 4 && memcmp(p, "null", 4) == 0){
            p += 4; valueOut.t = JValue::NUL; return true;
        }
        return number(valueOut);
    }
    bool object(JValue& valueOut, unsigned depth){
        valueOut.t = JValue::OBJ;
        ++p;
        ws();
        if(p < end && *p == '}'){ ++p; return true; }
        while(p < end){
            ws();
            std::string key;
            if(!str(key)) return false;
            ws();
            if(p >= end || *p != ':'){ ok = false; return false; }
            ++p;
            JValue child;
            if(!value(child, depth + 1)) return false;
            valueOut.obj[key] = std::move(child);
            ws();
            if(p < end && *p == ','){ ++p; continue; }
            if(p < end && *p == '}'){ ++p; return true; }
            ok = false; return false;
        }
        ok = false; return false;
    }
    bool array(JValue& valueOut, unsigned depth){
        valueOut.t = JValue::ARR;
        ++p;
        ws();
        if(p < end && *p == ']'){ ++p; return true; }
        while(p < end){
            JValue child;
            if(!value(child, depth + 1)) return false;
            valueOut.arr.push_back(std::move(child));
            ws();
            if(p < end && *p == ','){ ++p; continue; }
            if(p < end && *p == ']'){ ++p; return true; }
            ok = false; return false;
        }
        ok = false; return false;
    }
};
~~~

This rejects truncated strings, invalid surrogates/numbers, trailing bytes, and
excessive recursion before a Firefox session tree is published. Increment a
node counter before every value and a cumulative decoded-string counter while
appending; reject before either cap is exceeded. Add breadth and cumulative
string-limit tests, not only the depth test.

- [ ] **Step 2: Make SNSS framing fail transactionally**

Replace ParseChromiumSNSS with this complete transactional decoder:

~~~cpp
inline bool ParseChromiumSNSS(const std::string& data,
                             std::vector<WinFp>& output){
    output.clear();
    std::vector<WinFp> parsed;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(data.data());
    size_t sz = data.size();
    if(sz < 8 || !(b[0]=='S' && b[1]=='N' && b[2]=='S' && b[3]=='S')) return false;
    bool malformed = false;
    std::map<int,int> tabWin, tabIdx, winSel, tabSelNav;
    std::map<int,std::map<int,std::pair<std::string,std::string>>> tabNav;
    size_t pos = 8;
    while(pos < sz){
        if(pos + 2 > sz){ malformed = true; break; }
        uint16_t cs = static_cast<uint16_t>(b[pos] | (b[pos + 1] << 8));
        pos += 2;
        if(cs == 0 || pos + cs > sz){ malformed = true; break; }
        uint8_t id = b[pos];
        const uint8_t* c = b + pos + 1;
        size_t clen = static_cast<size_t>(cs) - 1;
        pos += cs;
        auto raw2 = [&](int32_t& first, int32_t& second){
            if(clen < 8) return false;
            first = static_cast<int32_t>(
                c[0] | (c[1] << 8) | (c[2] << 16) | (uint32_t(c[3]) << 24));
            second = static_cast<int32_t>(
                c[4] | (c[5] << 8) | (c[6] << 16) | (uint32_t(c[7]) << 24));
            return true;
        };
        if(id == 0){
            int32_t window = 0, tab = 0;
            if(!raw2(window, tab)){ malformed = true; break; }
            tabWin[tab] = window;
        } else if(id == 2){
            int32_t tab = 0, index = 0;
            if(!raw2(tab, index)){ malformed = true; break; }
            tabIdx[tab] = index;
        } else if(id == 7){
            int32_t tab = 0, index = 0;
            if(!raw2(tab, index)){ malformed = true; break; }
            tabSelNav[tab] = index;
        } else if(id == 8){
            int32_t window = 0, index = 0;
            if(!raw2(window, index)){ malformed = true; break; }
            winSel[window] = index;
        } else if(id == 6){
            if(clen < 4){ malformed = true; break; }
            SnssPR reader{c + 4, clen - 4};
            int32_t tab = 0, navigation = 0;
            std::string url, title;
            if(!reader.rInt(tab) || !reader.rInt(navigation) ||
               !reader.rStr(url) || !reader.rStr16(title)){
                malformed = true;
                break;
            }
            tabNav[tab][navigation] = {url, title};
        }
    }
    if(malformed) return false;

    std::map<int,std::vector<int>> winTabs;
    for(const auto& item : tabWin) winTabs[item.second].push_back(item.first);
    auto currentNavigation = [&](int tab){
        std::pair<std::string,std::string> empty;
        auto found = tabNav.find(tab);
        if(found == tabNav.end() || found->second.empty()) return empty;
        int selected = tabSelNav.count(tab)
            ? tabSelNav[tab] : found->second.rbegin()->first;
        auto navigation = found->second.find(selected);
        if(navigation == found->second.end()) navigation = std::prev(found->second.end());
        return navigation->second;
    };
    for(const auto& item : winTabs){
        int window = item.first;
        WinFp fingerprint;
        int selectedIndex = winSel.count(window) ? winSel[window] : -1;
        int activeTab = -1;
        for(int tab : item.second){
            auto navigation = currentNavigation(tab);
            std::string domain = etld1(hostOf(navigation.first));
            if(!domain.empty()) fingerprint.counts[domain]++;
            ++fingerprint.tabCount;
            fingerprint.tabsBlob += navigation.second + " " + navigation.first + " ";
            if(tabIdx.count(tab) && tabIdx[tab] == selectedIndex) activeTab = tab;
        }
        if(activeTab < 0 && !item.second.empty()) activeTab = item.second.front();
        auto active = currentNavigation(activeTab);
        fingerprint.activeTitle = active.second;
        fingerprint.activeDomain = etld1(hostOf(active.first));
        if(fingerprint.tabCount > 0) parsed.push_back(fingerprint);
    }
    output.swap(parsed);
    return true;
}
~~~

Before inserting into the SNSS maps, enforce explicit caps (10,000 windows,
100,000 tabs, 1,000,000 navigations, and 4 MiB of search text per resulting
window) and reject transactionally when any is exceeded. Add a generated
command-count regression so a bounded file cannot amplify into unbounded map
memory.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: SNSS tests pass; mozLz4 and stamp tests still fail to compile.

- [ ] **Step 3: Move bounded mozLz4 decode and file stamps into session.hpp**

Add `#include <new>` for the explicit `std::bad_alloc` guard below.

Add:

~~~cpp
static const unsigned long long MAX_BROWSER_SESSION_BYTES =
    512ULL * 1024ULL * 1024ULL;

inline long Lz4BlockDecompress(const uint8_t* source, size_t sourceLength,
                               uint8_t* destination, size_t destinationCapacity){
    size_t sourcePos = 0, destinationPos = 0;
    while(sourcePos < sourceLength){
        uint8_t token = source[sourcePos++];
        size_t literalLength = token >> 4;
        if(literalLength == 15){
            uint8_t next = 0;
            do {
                if(sourcePos >= sourceLength) return -1;
                next = source[sourcePos++];
                literalLength += next;
            } while(next == 255);
        }
        if(sourcePos + literalLength > sourceLength ||
           destinationPos + literalLength > destinationCapacity) return -1;
        memcpy(destination + destinationPos, source + sourcePos, literalLength);
        destinationPos += literalLength;
        sourcePos += literalLength;
        if(destinationPos == destinationCapacity) break;
        if(sourcePos >= sourceLength) break;
        if(sourcePos + 2 > sourceLength) return -1;
        size_t offset = size_t(source[sourcePos]) |
                        (size_t(source[sourcePos + 1]) << 8);
        sourcePos += 2;
        if(offset == 0 || offset > destinationPos) return -1;
        size_t matchLength = token & 0x0f;
        if(matchLength == 15){
            uint8_t next = 0;
            do {
                if(sourcePos >= sourceLength) return -1;
                next = source[sourcePos++];
                matchLength += next;
            } while(next == 255);
        }
        matchLength += 4;
        if(destinationPos + matchLength > destinationCapacity) return -1;
        size_t matchPos = destinationPos - offset;
        for(size_t i = 0; i < matchLength; ++i)
            destination[destinationPos + i] = destination[matchPos + i];
        destinationPos += matchLength;
    }
    return static_cast<long>(destinationPos);
}

inline bool MozLz4Decompress(const std::string& data, unsigned long long outputLimit,
                             std::string& output){
    static const uint8_t magic[8] = {0x6d,0x6f,0x7a,0x4c,0x7a,0x34,0x30,0x00};
    output.clear();
    if(data.size() < 12 || memcmp(data.data(), magic, 8) != 0) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    uint32_t size = uint32_t(p[8]) | (uint32_t(p[9]) << 8) |
                    (uint32_t(p[10]) << 16) | (uint32_t(p[11]) << 24);
    if(static_cast<unsigned long long>(size) > outputLimit) return false;
    try {
        output.resize(size);
    } catch(const std::bad_alloc&) {
        output.clear();
        return false;
    }
    if(size == 0) return true;
    long decoded = Lz4BlockDecompress(
        p + 12, data.size() - 12,
        reinterpret_cast<uint8_t*>(&output[0]), size);
    if(decoded != static_cast<long>(size)){
        output.clear();
        return false;
    }
    return true;
}

struct SessionStamp {
    unsigned long long size = 0;
    unsigned long long mtime = 0;
    bool operator==(const SessionStamp& other) const {
        return size == other.size && mtime == other.mtime;
    }
    bool operator!=(const SessionStamp& other) const { return !(*this == other); }
};

inline bool GetSessionStamp(const std::wstring& path, SessionStamp& stamp){
    WIN32_FILE_ATTRIBUTE_DATA data;
    if(!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER size, time;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    stamp.size = size.QuadPart;
    stamp.mtime = time.QuadPart;
    return stamp.size <= MAX_BROWSER_SESSION_BYTES;
}
~~~

Move `lz4_block_decompress`, `mozlz4_decompress`, and parsing primitives out of
`vde.cpp`. Retain a clearly marked transitional synchronous orchestration wrapper
only so the pre-Task-8 monitor remains buildable; it delegates to the bounded
session.hpp primitives and is deleted when Task 8 wires the worker. The worker
accepts only a `FileReadStatus::Ok` bounded result before calling
`MozLz4Decompress(bytes, MAX_BROWSER_SESSION_BYTES, json)`.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0 and all malformed/limit/stamp tests pass.

- [ ] **Step 4: Add a bounded asynchronous session worker**

Create `src/session_worker.hpp` and include it from `vde.cpp`. It is the final
path for cold/changed Firefox/Chromium data; Task 8 performs the atomic call-site
cutover so no such file is then read, decompressed, or parsed on the window
thread.
Include its direct dependencies (`<atomic>`, `<condition_variable>`, `<deque>`,
`<memory>`, `<mutex>`, and `<thread>`); do not rely on `vde.cpp` include order.

Use status-bearing messages:

~~~cpp
enum class SessionDataStatus { Fresh, CachedStale, Unavailable, Superseded };
enum class SessionPurpose {
    AutoReconcile, HeartbeatSave, ManualSave, ManualRestore, Search,
    MetadataProbe
};

struct SessionRequest {
    uint64_t requestId = 0;
    std::string app;
    AppProfile profile;
    SessionPurpose purpose = SessionPurpose::MetadataProbe;
    uint64_t identityGeneration = 0;
};

struct SessionResult {
    uint64_t requestId = 0;
    std::string app;
    std::wstring path;
    SessionPurpose purpose = SessionPurpose::MetadataProbe;
    uint64_t identityGeneration = 0;
    SessionStamp sourceStamp;
    bool sourceStampKnown = false;
    SessionStamp dataStamp;
    uint64_t dataGeneration = 0;
    SessionDataStatus status = SessionDataStatus::Unavailable;
    std::shared_ptr<const std::vector<WinFp>> windows;
};

inline bool SessionDataUsable(SessionDataStatus status){
    return status == SessionDataStatus::Fresh ||
           status == SessionDataStatus::CachedStale;
}
~~~

The worker owns session-path discovery, all file stamps, and the only
`SessionCache` (maximum 16 entries and 256 MiB estimated retained strings/maps,
evicting least-recently-used entries before insertion), one
`std::thread`, mutex, condition variable, and a coalesced pending map keyed by
app. At most one active read and one newest pending request per Firefox, Chrome,
or Edge profile exist; replacing a pending request posts a `Superseded` result
for the older request rather than growing a queue.
Use explicit priority `ManualSave/ManualRestore > AutoReconcile/HeartbeatSave >
Search > metadata probe`: a periodic probe is dropped/coalesced while higher
priority work for that app is pending and can never starve a user command.
Cache and UI results share one immutable payload through `shared_ptr`; never
copy a potentially large fingerprint vector. Count cache plus accepted UI
payloads against the same memory budget and reject an individual result that
cannot fit after eviction.

For each request the worker performs, in order:

1. resolve the session path from the copied profile, then stamp-before;
2. return an exact path+stamp cache entry as `Fresh` without reopening/parsing;
3. otherwise perform the bounded read;
4. transactionally parse/decompress into a local vector;
5. re-resolve the session path, then stamp-after and publish it separately as
   `sourceStamp` when known;
6. publish `Fresh` (including a successfully parsed empty vector) only when
   path-before equals path-after and both stamps match each other;
7. otherwise publish the last cache entry for that exact path as
   `CachedStale`, or `Unavailable` if no cache exists.

`dataStamp` identifies the cached/fresh vector; `sourceStamp` identifies the
currently observed input even when cached data is returned. Deferred retry keys
use `sourceStamp` when known, so a changed source resets the bounded budget.
The worker increments `dataGeneration` only when the accepted path/stamp (or a
content hash used to break a stamp tie) changes; cache hits keep the same value,
so routine requests do not manufacture lifecycle waves.

Post a heap-owned result through `WM_SESSION_RESULT`; if `PostMessageW`
fails, delete it on the worker. The UI handler immediately wraps `lp` in
`std::unique_ptr<SessionResult>` and accepts it only when request ID, app,
identity generation, and the current profile configuration still match its
pending operation. Old generations
are ignored and freed.

Add a small injected reader/poster seam and RED tests for:

- valid empty parse is `Fresh`;
- malformed or stamp-changing input returns `CachedStale` when cached and
  `Unavailable` when cold;
- Chromium rotation to a newer `Session_*` path during parsing cannot publish
  the old path as Fresh;
- a late completion for an older identity generation is rejected;
- ten rapid requests for one app leave no more than one active plus one pending;
- `SessionCache` remains at or below 16 entries and its retained-byte budget;
- request/result purpose survives dispatch, and priority/coalescing returns a
  `Superseded` result with the original purpose;
- a stale result may match an existing record but cannot update its fingerprint;
- unavailable data leaves serialized records byte-for-byte unchanged and
  schedules one bounded defer;
- stop rejects new work, wakes the worker, joins it, and frees an unposted
  result.

Use `StopSessionWorker()` before destroying the message window. After joining,
drain/free any already-posted `WM_SESSION_RESULT` payloads; do not detach the
thread and do not let it reference HWND state after stop.

- [ ] **Step 5: Expose a generation-qualified coordinator for later UI cutover**

Maintain a UI-thread map of the latest accepted result per app. Expose
`RequestSessionData(profile, identityGeneration, purpose)` plus a result acceptor
that checks request/app/purpose/generation. Unit-test it now with caller-supplied
monotonic generations; Task 8 supplies
`AppFastSnapshot::identityGeneration` and wires
`BeginAppReconcile`, heartbeat, manual save/restore, and tab search. Until that
single cutover, the old monitor uses only the bounded transitional wrapper, so
every intermediate commit builds. `Superseded` only releases old request
bookkeeping; it is never usable data.

After the Task 8 cutover, every present stable profile also coalesces a lightweight
worker-side metadata probe. Path discovery and `GetFileAttributesEx` stay on
the worker; an unchanged path/stamp returns the shared exact cache entry, while
a changed stamp/path triggers one parse and a new data generation. Thus title,
tab, and domain changes refresh even when the HWND set never changes. Test a
stable HWND whose session file changes and assert one fingerprint-refresh wave.

Add/test a pure `SessionAcceptancePolicy(SessionDataStatus)` now; it has no
dependency on the Task 8 fast-snapshot type. Task 8 implements
`BuildLiveLayouts(SessionResult, AppFastSnapshot)` and swaps production callers
in the same commit. That merge never touches disk and carries
`fingerprintsFresh = (status == Fresh)`:

- `Fresh`: restore/match, create new records, and update fingerprints;
- `CachedStale`: existing records may be matched/restored, but their stored
  fingerprint fields stay unchanged and unmatched live windows wait for Fresh;
- `Unavailable`: defer without restore, create, missing-mark, or overwrite.

An absent app needs no session file: mark its records missing from
`lastSeenUtc` and prune immediately. Finalization never waits for the worker:
it updates actual desktops of successfully bound identities, preserves
unbound/failed destinations and fingerprints, marks truly absent apps, prunes,
and writes atomically. Finalization is explicitly not a `SessionPurpose` and
never enqueues work: it consumes only already-accepted UI cache data plus the
bound fast snapshot. Tray manual save reports completion asynchronously; CLI
may use the same bounded reader synchronously because it has no UI loop.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
rg -n "ParseChromiumSNSS|MozLz4Decompress" src\vde.cpp
~~~

Expected: both builds exit 0; direct parser calls occur only in session.hpp/the
worker. The marked transitional bounded wrapper may still be reached by the old
monitor until Task 8; Task 8's final search proves no UI handler retains it.
- [ ] **Step 6: Commit session hardening**

~~~powershell
git add src/session.hpp src/session_worker.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "fix(session): reject partial browser data"
~~~

### Task 5: Replace the global lifecycle with generation-safe per-app decisions

**Files:**

- Modify: src/lifecycle.hpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED lifecycle generation and retry tests**

Delete the old global-presence/run-count tests. Add fake-clock tests for:

- a present app begins restore after two identical settle snapshots;
- Firefox and Chrome state machines are independent;
- a desktop-only change after a completed restore returns `SaveLayout`, not
  `BeginRestore`;
- an accepted new Fresh session generation with the same HWND starts one
  fingerprint-refresh wave; a title-only bound record gains its domain/tab
  fingerprint without moving the window;
- the first observation of an absent app returns
  `MarkMissingFromLastSeen`, so a record last seen 31 days ago is immediately
  pruned;
- a window-set/session change during an in-flight generation arms exactly one
  later restore wave after that generation completes;
- completion for an old generation is ignored;
- an exhausted move budget for the same
  `{recordId,fullRuntimeIdentity,destination}` never starts another move;
  unrelated session writes or a new sibling window do not reset it, while a
  changed target identity/destination or explicit retry permits one new budget;
- `Deferred` retries at most three times for one
  `{windowSetSignature,sessionStamp}`, respects a 30-second backoff, and resets
  only when either key changes;
- a new wave gets its own 20-second maximum settle timeout;
- one absence transition marks missing once.
- after the first restore wave succeeds, a late second HWND changes the window
  set and starts exactly one new restore-before-save wave without moving the
  already bound first window;
- with Firefox continuously present, Edge can independently become absent,
  receive its own missing timestamp, and restore its retained window when Edge
  alone reappears.
- while Firefox window A remains present, Firefox sibling B can disappear, be
  missing-marked independently, then reappear and restore before save; aggregate
  app presence must not suppress either transition.

Expected RED result: the current global `LcState` and boolean completion cannot
represent generations, deferred input, or per-window exhaustion.

- [ ] **Step 2: Implement the pure lifecycle state and budgets**

Make `lifecycle.hpp` self-contained with direct `<cstdint>`, `<set>`, and
`<string>` includes plus `layout.hpp`.

Use these public types:

~~~cpp
enum class LcAction {
    None, BeginRestore, SaveLayout, MarkMissingFromLastSeen
};
enum class LcRestoreOutcome { Success, Deferred, Exhausted };

struct LcDecision {
    LcAction action = LcAction::None;
    uint64_t generation = 0;
};

struct RestoreBudgetKey {
    std::string recordId;
    std::string fullRuntimeIdentity;
    std::string destinationGuid;
};

struct LcState {
    bool initialized = false;
    bool present = false;
    bool restorePending = false;
    bool restoreInFlight = false;
    bool rearmAfterFlight = false;
    bool deferredUntilInputChanges = false;
    int stableSnapshots = 0;
    int deferredAttempts = 0;
    uint64_t nextGeneration = 1;
    uint64_t inFlightGeneration = 0;
    uint64_t windowSetSignature = 0;
    uint64_t settleSignature = 0;
    uint64_t layoutSignature = 0;
    uint64_t sessionStampSignature = 0;
    uint64_t appearanceSinceMs = 0;
    uint64_t retryNotBeforeMs = 0;
};
~~~

`LcObserve` accepts presence, window-set, settle, layout, session-stamp, and
monotonic time and returns `LcDecision`. Implement these rules:

- first absent observation initializes state and returns
  `MarkMissingFromLastSeen`; subsequent continued absence is None;
- present-after-absence, a new window set, or an accepted changed Fresh session
  stamp/generation starts a pending settle wave (bound identities refresh their
  fingerprints but are never moved by that wave);
- while a generation is in flight, changed window/session signatures update the
  latest values and set `rearmAfterFlight` without invalidating the active
  operation;
- a pending wave begins once two snapshots share settle signature or its
  20-second timeout expires, subject to retry backoff;
- desktop-only changes return SaveLayout only with no pending/in-flight wave;
- elapsed checks guard clock rollback/unsigned underflow.

`LcRestoreCompleted(state,generation,outcome,layoutSig,sessionStamp,now)`
ignores a generation other than `inFlightGeneration`. Success completes or
starts the single rearmed wave. Deferred increments the keyed attempt count and
backs off 30 seconds; after attempt three it waits for a changed window
set/session stamp. Exhausted completes the wave and records the observed layout
signature so the same failed actual placement does not trigger a save loop; a
queued rearm still starts once.

Keep failed move budgets separate from app-wide state:

~~~cpp
class RestoreBudgets {
public:
    bool mayAttempt(const RestoreBudgetKey&) const;
    void markExhausted(const RestoreBudgetKey&);
    void clearExact(const RestoreBudgetKey&);
    void clearForExplicitRetry(const std::string& recordId);
    void pruneToLiveIdentities(const std::set<std::string>& liveRuntimeKeys);
};
~~~

This prevents one failed window from blocking successful siblings or being
retried because a sibling/session changed. A successful move calls `clearExact`
with the same full key; a manual popup move calls `clearForExplicitRetry`. Cap the stored keys
(for example 256 LRU entries) and discard only identities that are no longer
live/retained. Add failed-A + changing-session/new-B tests: B proceeds and A is
not retried.

Add `LcExplicitSaveCompleted` to update the layout signature and clear any
deferred/save state for the exact app generation without fabricating a restore
success.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: all generation, bounded retry, initial absence, and independent-app
tests pass.

- [ ] **Step 3: Commit independent lifecycle**

~~~powershell
git add src/lifecycle.hpp tests/vdtest.cpp
git commit -m "fix(lifecycle): bound per-app restore waves"
~~~
### Task 6: Plan restore-before-save reconciliation

**Files:**

- Modify: src/lifecycle.hpp
- Modify: src/layout.hpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED reconciliation tests**

~~~cpp
static void test_reconcile_restores_saved_a_and_creates_new_b(){
    const UnixSeconds now = 2000000000;
    LayoutWin savedA = Identified(
        "{00000000-0000-0000-0000-000000000301}", "firefox", 0,
        {{"a.com", 1}}, "A");
    savedA.desktop = G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveA = LW("firefox", 3, {{"a.com", 1}}, "A");
    liveA.desktop = G(L"{231A0000-0000-0000-0000-000000000004}");
    LayoutWin liveB = LW("firefox", 4, {{"b.com", 1}}, "B");
    liveB.desktop = G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan = PlanAppReconcile({savedA}, {liveA, liveB}, "firefox", now);
    CHECK(plan.restores.size() == 1);
    CHECK(plan.restores[0].liveIndex == 0);
    CHECK(plan.newRecords.size() == 1 && plan.newRecords[0].liveIndex == 1);
    CHECK(!plan.newRecords[0].recordId.empty());

    auto committed = CommitAppReconcile({savedA}, {liveA, liveB}, plan, {0}, now);
    CHECK(committed.size() == 2);
    CHECK(GuidEq(committed[0].desktop, savedA.desktop));
    CHECK(committed[0].missingSinceUtc == 0);
    CHECK(committed[1].activeTitle == "B");
    CHECK(GuidEq(committed[1].desktop, liveB.desktop));
}

static void test_failed_restore_keeps_saved_destination(){
    const UnixSeconds now = 2000000000;
    LayoutWin saved = Identified(
        "{00000000-0000-0000-0000-000000000302}", "chrome", 0,
        {{"a.com", 1}}, "A");
    saved.desktop = G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin live = LW("chrome", 4, {{"a.com", 1}}, "A");
    live.desktop = G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan plan = PlanAppReconcile({saved}, {live}, "chrome", now);
    auto committed = CommitAppReconcile({saved}, {live}, plan, {}, now);
    CHECK(committed.size() == 1);
    CHECK(GuidEq(committed[0].desktop, saved.desktop));
    CHECK(committed[0].lastSeenUtc == now);
}

static void test_reconcile_marks_only_observed_app_missing(){
    const UnixSeconds now = 2000000000;
    LayoutWin ff = Identified(
        "{00000000-0000-0000-0000-000000000303}", "firefox", 0,
        {{"a.com", 1}}, "A");
    LayoutWin chrome = Identified(
        "{00000000-0000-0000-0000-000000000304}", "chrome", 1,
        {{"b.com", 1}}, "B");
    auto plan = PlanAppReconcile({ff, chrome}, {}, "chrome", now);
    auto committed = CommitAppReconcile({ff, chrome}, {}, plan, {}, now);
    CHECK(committed[0].missingSinceUtc == 0);
    CHECK(committed[1].missingSinceUtc != 0);
}

static void test_expired_reappearance_is_new_not_restored(){
    const UnixSeconds now = 2000000000;
    LayoutWin expired = Identified(
        "{00000000-0000-0000-0000-000000000305}", "firefox", 0,
        {{"a.com", 1}}, "A");
    expired.missingSinceUtc = now - WINDOW_RETENTION_SECONDS;
    LayoutWin live = LW("firefox", 4, {{"a.com", 1}}, "A");
    live.desktop = G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan plan = PlanAppReconcile({expired}, {live}, "firefox", now);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.size() == 1);
    CHECK(plan.newRecords[0].recordId != expired.recordId);
    auto committed = CommitAppReconcile({expired}, {live}, plan, {}, now);
    CHECK(committed.size() == 1);
    CHECK(committed[0].recordId == plan.newRecords[0].recordId);
    CHECK(GuidEq(committed[0].desktop, live.desktop));
}

static void test_reserved_live_record_cannot_be_stolen_by_unbound_duplicate(){
    const UnixSeconds now = 2000000000;
    LayoutWin bound = Identified(
        "{00000000-0000-0000-0000-000000000306}", "chrome", 0,
        {{"same.com", 1}}, "Same");
    LayoutWin newcomer = LW("chrome", 2, {{"same.com", 1}}, "Same");
    std::set<std::string> reserved = {bound.recordId};
    ReconcilePlan plan = PlanAppReconcile(
        {bound}, {newcomer}, "chrome", now, reserved);
    CHECK(plan.matches.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(plan.newRecords.size() == 1);
}

static void test_cached_stale_match_preserves_fingerprint_and_defers_unmatched(){
    const UnixSeconds now = 2000000000;
    LayoutWin saved = Identified(
        "{00000000-0000-0000-0000-000000000307}", "msedge", 0,
        {{"saved.example", 2}}, "Saved");
    saved.activeDomain = "saved.example";
    saved.tabCount = 2;
    LayoutWin matched = LW("msedge", 0, {{"saved.example", 2}}, "Saved");
    matched.activeDomain = "stale.example";
    matched.tabCount = 9;
    LayoutWin unmatched = LW("msedge", 1, {{"new.example", 1}}, "New");
    ReconcilePlan plan = PlanAppReconcile(
        {saved}, {matched, unmatched}, "msedge", now, {},
        ReconcileFreshness::CachedStale);
    CHECK(plan.matches.size() == 1);
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    auto committed = CommitAppReconcile({saved}, {matched, unmatched}, plan, {}, now);
    CHECK(committed.size() == 1);
    CHECK(committed[0].counts == saved.counts);
    CHECK(committed[0].activeDomain == saved.activeDomain);
    CHECK(committed[0].tabCount == saved.tabCount);
}
~~~

Also add and call `test_late_window_after_first_wave_restores_before_save` and
`test_edge_retention_is_independent_while_firefox_stays_open`, plus
`test_firefox_sibling_reappears_while_first_window_stays_open`. Drive the former
through two lifecycle generations plus `PlanAppReconcile`, asserting the bound
first record is reserved and only the late HWND is restored/created. Drive the
latter with separate `LcState`/record sets for `firefox` and `msedge`, asserting
the Firefox timestamps and placement remain byte-identical across Edge
absence/reappearance.
For the sibling regression, keep record/runtime A reserved and continuously
bound, mark only B missing on the changed window set, then reintroduce a matching
B on the wrong desktop; assert exactly B queues restore and its saved destination
is not overwritten before verified success.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failures because ReconcilePlan, PlanAppReconcile, and CommitAppReconcile do not exist.

- [ ] **Step 2: Implement the pure plan and commit phases**

Add to lifecycle.hpp after including layout.hpp:

~~~cpp
struct RestoreRequest {
    size_t savedIndex = 0;
    size_t liveIndex = 0;
    GUID destination = {0};
};

struct NewRecordRequest {
    size_t liveIndex = 0;
    std::string recordId;
};

enum class ReconcileFreshness { Fresh, CachedStale };

struct ReconcilePlan {
    std::string app;
    UnixSeconds nowUtc = 0;
    ReconcileFreshness freshness = ReconcileFreshness::Fresh;
    bool deferred = false;
    std::vector<LayoutMatch> matches;
    std::vector<RestoreRequest> restores;
    std::vector<NewRecordRequest> newRecords;
    std::vector<size_t> missingSavedIndices;
};

inline ReconcilePlan PlanAppReconcile(const std::vector<LayoutWin>& existing,
                                       const std::vector<LayoutWin>& live,
                                       const std::string& app,
                                       UnixSeconds nowUtc,
                                       const std::set<std::string>& reservedRecordIds = {},
                                       ReconcileFreshness freshness = ReconcileFreshness::Fresh){
    ReconcilePlan plan;
    plan.app = app;
    plan.nowUtc = nowUtc;
    plan.freshness = freshness;
    std::vector<LayoutWin> eligible;
    std::vector<size_t> originalIndex;
    for(size_t i = 0; i < existing.size(); ++i)
        if(existing[i].app == app && !IsExpired(existing[i], nowUtc) &&
           !reservedRecordIds.count(existing[i].recordId)){
            eligible.push_back(existing[i]);
            originalIndex.push_back(i);
        }
    bool tooComplex = false;
    std::vector<LayoutMatch> assigned =
        MatchOneToOne(eligible, live, 0.55, &tooComplex);
    if(tooComplex){
        plan.deferred = true;
        return plan;
    }
    for(const LayoutMatch& match : assigned)
        plan.matches.push_back({originalIndex[match.savedIndex],
                                match.liveIndex, match.score});
    std::vector<bool> matchedSaved(existing.size(), false), matchedLive(live.size(), false);
    for(const auto& match : plan.matches){
        matchedSaved[match.savedIndex] = true;
        matchedLive[match.liveIndex] = true;
        if(!GuidEq(existing[match.savedIndex].desktop, live[match.liveIndex].desktop))
            plan.restores.push_back({
                match.savedIndex, match.liveIndex, existing[match.savedIndex].desktop
            });
    }
    for(size_t i = 0; freshness == ReconcileFreshness::Fresh &&
                           i < existing.size(); ++i)
        if(existing[i].app == app && !matchedSaved[i] &&
           !reservedRecordIds.count(existing[i].recordId))
            plan.missingSavedIndices.push_back(i);
    for(size_t i = 0; freshness == ReconcileFreshness::Fresh &&
                           i < live.size(); ++i)
        if(live[i].app == app && !matchedLive[i]){
            std::string recordId = NewRecordId();
            if(!recordId.empty()) plan.newRecords.push_back({i, recordId});
        }
    return plan;
}

inline std::vector<LayoutWin> CommitAppReconcile(
        const std::vector<LayoutWin>& existing,
        const std::vector<LayoutWin>& live,
        const ReconcilePlan& plan,
        const std::set<size_t>& successfulRestoreLiveIndices,
        UnixSeconds nowUtc){
    if(plan.deferred) return existing;
    std::vector<LayoutWin> output = existing;
    std::set<size_t> matchedSaved;
    for(const auto& match : plan.matches){
        matchedSaved.insert(match.savedIndex);
        LayoutWin& record = output[match.savedIndex];
        GUID savedDestination = record.desktop;
        int savedIndex = record.deskIndex;
        std::string recordId = record.recordId;
        if(plan.freshness == ReconcileFreshness::Fresh){
            record.activeTitle = live[match.liveIndex].activeTitle;
            record.activeDomain = live[match.liveIndex].activeDomain;
            record.tabCount = live[match.liveIndex].tabCount;
            record.counts = live[match.liveIndex].counts;
        }
        record.recordId = recordId;
        MarkSeen(record, nowUtc);
        bool neededRestore = !GuidEq(savedDestination, live[match.liveIndex].desktop);
        bool restoreSucceeded = successfulRestoreLiveIndices.count(match.liveIndex) != 0;
        if(!neededRestore){
            record.desktop = live[match.liveIndex].desktop;
            record.deskIndex = live[match.liveIndex].deskIndex;
        } else if(restoreSucceeded) {
            record.desktop = savedDestination;
            record.deskIndex = savedIndex;
        } else {
            record.desktop = savedDestination;
            record.deskIndex = savedIndex;
        }
    }
    for(size_t index : plan.missingSavedIndices) MarkMissing(output[index], nowUtc);
    for(const auto& request : plan.newRecords){
        LayoutWin record = live[request.liveIndex];
        record.recordId = request.recordId;
        MarkSeen(record, nowUtc);
        output.push_back(record);
    }
    return PruneExpired(output, nowUtc);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0; saved A remains assigned to its old desktop, new B is added immediately, failed restore does not overwrite destination, and Firefox is untouched by Chrome reconciliation.

- [ ] **Step 3: Commit reconciliation**

~~~powershell
git add src/layout.hpp src/lifecycle.hpp tests/vdtest.cpp
git commit -m "feat(lifecycle): reconcile windows safely"
~~~

### Task 7: Add an owner-aware timer-driven move queue

**Files:**

- Create: src/move_queue.hpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED bounded-attempt and dispatch-identity tests**

Define test jobs with a full token:

~~~cpp
static MoveJob MJ(MoveOwner owner, uint64_t operationId,
                  uint64_t jobId, const char* runtimeKey){
    MoveJob job;
    job.token = {owner, operationId, jobId, 0};
    job.runtimeKey = runtimeKey;
    job.destination = G(L"{231A0000-0000-0000-0000-000000000001}");
    return job;
}
~~~

Add tests proving:

- each job alternates Issue then Verify;
- four failed Issue/Verify attempts produce one failed result and remove the
  job;
- a failed job does not stop the next job;
- result preserves every `MoveToken` field;
- accepted+verified returns `Succeeded`; repeated transient failure returns
  `Exhausted`; an invalid identity/desktop returns `PermanentFailure`; explicit
  supersession returns `Cancelled` and does not consume a retry budget;
- automatic and manual jobs with the same runtime key remain independently
  addressable by job ID;
- `cancelJob(jobId)` and
  `cancelOperation(owner,operationId)` return one failed completion per
  removed job, including a current Verify job, without cancelling another
  owner's operation;
- delivering the same returned cancellation twice is harmless at the
  owner-specific dispatcher (tested with a fake operation map).

Expected RED result: current code has no token, operation cancellation, or
owner-safe identity.

- [ ] **Step 2: Implement the pure queue**

Create a self-contained C++14 header with direct `<cstddef>`, `<cstdint>`,
`<deque>`, `<string>`, `<vector>`, and `<windows.h>` includes:

~~~cpp
enum class MoveAction { None, Issue, Verify };
enum class MoveOwner { AutoReconcile, ManualTray, Picker };
enum class MoveTerminal {
    None, Succeeded, Cancelled, PermanentFailure, Exhausted
};
enum class MoveAttemptOutcome {
    Accepted, OnDestination, TransientFailure, PermanentFailure
};

struct MoveToken {
    MoveOwner owner = MoveOwner::AutoReconcile;
    uint64_t operationId = 0;
    uint64_t jobId = 0;
    size_t itemIndex = 0;
};

struct MoveJob {
    MoveToken token;
    std::string runtimeKey; // diagnostics/coalescing only; never dispatch key
    std::string recordId;
    GUID destination = {0};
    int attempts = 0;
    bool waitingForVerify = false;
};

struct MoveResult {
    bool completed = false;
    MoveTerminal terminal = MoveTerminal::None;
    int attempts = 0;
    MoveToken token;
    std::string runtimeKey;
    std::string recordId;
};
~~~

`MoveQueue` exposes `enqueue`, `empty`, `front`, `nextAction`,
`onIssued(MoveAttemptOutcome)`, `onVerified(MoveAttemptOutcome)`, `cancelJob`,
and `cancelOperation`.
`cancelOperation` returns a vector because multiple queued items can belong to
one operation. Every completion copies the token before erasing its job.
Attempts increment only on Issue and are capped at four; a transient failed
Verify resets the job to Issue unless the cap is reached. Permanent failure
finishes immediately, cancellation returns `Cancelled`, and only four
transient cycles return `Exhausted`. No method sleeps, calls Win32 move
APIs, or invokes an owner callback.

Never use `runtimeKey` to retrieve the completion owner. Runtime strings may
coincide across auto/manual/picker operations; only the globally monotonic
`jobId` keys the runtime binding.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: all move-queue tests pass without `Sleep`.

- [ ] **Step 3: Commit the queue**

~~~powershell
git add src/move_queue.hpp tests/vdtest.cpp
git commit -m "feat(restore): add owner-aware move queue"
~~~
### Task 8: Integrate the v4 monitor, cache, restore queue, and final snapshots

**Files:**

- Create: src/window_identity.hpp
- Create: src/reconcile_worker.hpp
- Modify: src/layout.hpp
- Modify: src/layout_store.hpp
- Modify: src/vde.cpp:61-95,383-535,804-863,1137-1326
- Modify: src/lifecycle.hpp
- Modify: tests/vdtest.cpp

- [ ] **Step 1: Add a RED idempotent-finalization test**

Add this small pure guard to the desired lifecycle API:

~~~cpp
static void test_finalization_runs_once(){
    FinalizationState state;
    CHECK(state.begin());
    CHECK(!state.begin());
    state.finish();
    CHECK(state.finished);
    CHECK(!state.begin());

    FinalizationState retryable;
    CHECK(retryable.begin());
    retryable.retry();
    CHECK(retryable.begin());
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure because FinalizationState does not exist.

- [ ] **Step 2: Implement the finalization guard**

Add to lifecycle.hpp:

~~~cpp
struct FinalizationState {
    bool running = false;
    bool finished = false;
    bool begin(){
        if(running || finished) return false;
        running = true;
        return true;
    }
    void finish(){
        running = false;
        finished = true;
    }
    void retry(){
        running = false;
    }
};
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: all pure tests pass.

- [ ] **Step 3: Replace run-based globals with v4 runtime state**

First create a self-contained `window_identity.hpp`; shared structs must not
depend on a type declared only in `vde.cpp`:

~~~cpp
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <windows.h>

struct WindowIdentityKey {
    uintptr_t hwnd = 0;
    DWORD pid = 0;
    uint64_t processStart = 0;
};

struct FastWin {
    std::string app;
    HWND hwnd = nullptr;
    DWORD pid = 0;
    uint64_t processStart = 0;
    GUID desktop = {0};
    std::wstring title;
};

inline WindowIdentityKey IdentityOf(const FastWin& window){
    WindowIdentityKey key;
    key.hwnd = reinterpret_cast<uintptr_t>(window.hwnd);
    key.pid = window.pid;
    key.processStart = window.processStart;
    return key;
}

inline bool SameIdentity(const WindowIdentityKey& a,
                         const WindowIdentityKey& b){
    return a.hwnd == b.hwnd && a.pid == b.pid &&
           a.processStart != 0 && a.processStart == b.processStart;
}

inline std::string RuntimeKey(const WindowIdentityKey& key){
    return std::to_string(static_cast<unsigned long long>(key.hwnd)) + ":" +
           std::to_string(static_cast<unsigned long long>(key.pid)) + ":" +
           std::to_string(static_cast<unsigned long long>(key.processStart));
}

inline std::string RuntimeKey(const FastWin& window){
    return RuntimeKey(IdentityOf(window));
}

struct SnapshotVersions {
    uint64_t identityGeneration = 0;
    uint64_t contentGeneration = 0;
};

class SnapshotVersionTracker {
public:
    SnapshotVersions observe(const std::string& app,
                             uint64_t identityQualityConfigSignature,
                             uint64_t fullContentSignature);
private:
    struct Entry {
        bool initialized = false;
        uint64_t identitySignature = 0;
        uint64_t contentSignature = 0;
        SnapshotVersions versions;
    };
    uint64_t nextGeneration_ = 1;
    std::map<std::string,Entry> entries_;
};
~~~

`observe` allocates a nonzero monotonic value only when the corresponding
signature changes; identical observations return identical generations. On
counter wrap, clear the tracker and restart at 1 rather than emitting zero.
Add pure tests for unchanged snapshots, layout/title-only changes (content only),
identity/quality/profile-config changes (both generations), and the delimiter
collision `{"a","bc"} -> {"ab","c"}` changing content generation.

Add the conflict/rebase model to `layout_store.hpp`, not `vde.cpp`, so it is a
pure testable API:

~~~cpp
enum class RecordDeltaKind {
    ExplicitUpsert, ValidatedRuntimeUpsert, MissingMark, ExpireDelete
};

struct RecordDelta {
    RecordDeltaKind kind = RecordDeltaKind::ValidatedRuntimeUpsert;
    bool erase = false;
    LayoutWin record;
    LayoutRevision baseRevision;
    bool baseRecordPresent = false;
    LayoutWin baseRecord;
    UnixSeconds changedUtc = 0;
    uint64_t causalGeneration = 0;
};

struct RebaseResult {
    std::vector<LayoutWin> records;
    std::set<std::string> deferredConflictRecordIds;
};

RebaseResult RebaseRecordDeltas(
    const std::vector<LayoutWin>& latest,
    const LayoutRevision& latestRevision,
    const std::map<std::string,RecordDelta>& deltas,
    UnixSeconds nowUtc);
~~~

Implement it in the header from the deterministic ID/conflict rules in Step 5
and cover it directly from `vdtest.cpp`. `SameRevision` is the Task 3
`layout_store.hpp` API and compares every revision field, including path/hash.

Add includes and globals in vde.cpp:

~~~cpp
#include "layout_store.hpp"
#include "move_queue.hpp"
#include "window_identity.hpp"

#define TIMER_MOVE_VERIFY 3
#define TIMER_HEARTBEAT 4
#define MOVE_VERIFY_INTERVAL_MS 150
#define HEARTBEAT_INTERVAL_MS (6 * 60 * 60 * 1000)

static std::vector<LayoutWin> g_autoRecords;
static std::vector<DeskRec> g_autoDesktops;
static std::map<std::string,LcState> g_lifecycleByApp;
static MoveQueue g_moveQueue;
static FinalizationState g_finalization;
static bool g_autoDirty = false;
static bool g_autoLoaded = false;
static bool g_autoWritesAllowed = false;
static bool g_preserveBackupOnNextWrite = false;
static LayoutRevision g_autoRevision;
static std::map<std::string,RecordDelta> g_dirtyRecordDeltas;
static std::set<std::string> g_deferredConflictRecordIds;
struct ReservedAutoIdentity {
    uint64_t pickerGeneration = 0;
    WindowIdentityKey identity;
    std::string app;
    std::string recordId;
    GUID originDesktop = {0};
    LayoutWin provisionalOriginRecord;
    bool hasProvisionalOriginRecord = false;
};
static std::map<std::string,ReservedAutoIdentity> g_reservedAutoIdentities;
static bool g_heartbeatDeferred = false;
static uint64_t g_nextAutoLoadRetryMs = 0;
static unsigned g_autoLoadRetryAttempt = 0;
~~~

Delete `g_lc`, `g_seenKeys`, `g_observedApps`, `g_lastLayoutSig`, missing-run
reconciliation, the old `RunSaveAuto` implementation, and the transitional
`MISSING_RUNS_MAX`/`LayoutWin::missingRuns`/`PrepareTransitionalV4Records`
bridges retained by Task 1. Reuse the explicit
`UtcNowSeconds` helper added in Task 1.

- [ ] **Step 4: Collect each HWND once per monitor tick**

Add the following single-pass collector. It calls `EnumWindows` exactly once,
opens each accepted PID at most once, and calls `GetWindowDesktopId` once per
accepted HWND:

~~~cpp
struct AppFastSnapshot {
    std::vector<FastWin> windows;
    uint64_t identityGeneration = 0;
    uint64_t generation = 0;
    uint64_t windowSetSignature = 0;
    uint64_t settleSignature = 0;
    uint64_t layoutSignature = 0;
    bool enumerationComplete = true;
    bool desktopLookupsComplete = true;
};

static uint64_t HashBytes(uint64_t hash, const void* value, size_t size){
    const unsigned char* bytes = static_cast<const unsigned char*>(value);
    for(size_t i = 0; i < size; ++i){
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t HashCombine(uint64_t hash, uint64_t value){
    return HashBytes(hash ? hash : 1469598103934665603ULL,
                     &value, sizeof(value));
}

static uint64_t HashGuid(const GUID& guid){
    return HashBytes(1469598103934665603ULL, &guid, sizeof(guid));
}

static uint64_t ProfileConfigSignature(const AppProfile& profile){
    uint64_t hash = HashBytes(1469598103934665603ULL,
                              profile.id.data(), profile.id.size());
    auto addWide = [&](const std::wstring& value){
        hash = HashBytes(hash, value.data(), value.size() * sizeof(wchar_t));
        hash = HashCombine(hash, value.size());
    };
    addWide(profile.exeName);
    for(const auto& value : profile.classNames) addWide(value);
    for(const auto& value : profile.titleSuffixes) addWide(value);
    hash = HashCombine(hash, static_cast<uint64_t>(profile.session));
    addWide(profile.userDataDir);
    return hash;
}

struct ProcessSnapshot {
    std::wstring image;
    unsigned long long started = 0;
};

static ProcessSnapshot ReadProcessSnapshot(DWORD pid){
    ProcessSnapshot value;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if(!process) return value;
    std::vector<wchar_t> path(32768, L'\0');
    DWORD pathSize = static_cast<DWORD>(path.size());
    if(QueryFullProcessImageNameW(process, 0, path.data(), &pathSize))
        value.image.assign(path.data(), pathSize);
    FILETIME created = {0}, exited = {0}, kernel = {0}, user = {0};
    if(GetProcessTimes(process, &created, &exited, &kernel, &user)){
        ULARGE_INTEGER ticks;
        ticks.LowPart = created.dwLowDateTime;
        ticks.HighPart = created.dwHighDateTime;
        value.started = ticks.QuadPart;
    }
    CloseHandle(process);
    return value;
}

struct FastEnumContext {
    const std::vector<AppProfile>* profiles = nullptr;
    std::map<std::string,AppFastSnapshot>* snapshots = nullptr;
    std::map<DWORD,ProcessSnapshot> processes;
};

static const AppProfile* ClassifyBrowserCandidate(
        const wchar_t* className, const std::wstring& image,
        const std::vector<AppProfile>& profiles){
    for(const AppProfile& profile : profiles){
        bool classMatches = std::find(profile.classNames.begin(),
                                      profile.classNames.end(), className) !=
                            profile.classNames.end();
        if(classMatches && EndsWithI(image, profile.exeName)) return &profile;
    }
    return nullptr;
}

static BOOL CALLBACK EnumFastWindow(HWND hwnd, LPARAM parameter){
    FastEnumContext& context = *reinterpret_cast<FastEnumContext*>(parameter);
    if(!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE)) return TRUE;
    wchar_t className[64] = {0};
    if(GetClassNameW(hwnd, className, 64) <= 0) return TRUE;

    bool trackedClass = false;
    for(const AppProfile& profile : *context.profiles)
        if(std::find(profile.classNames.begin(), profile.classNames.end(), className) !=
           profile.classNames.end()){
            trackedClass = true;
            break;
        }
    if(!trackedClass) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto process = context.processes.find(pid);
    if(process == context.processes.end())
        process = context.processes.emplace(pid, ReadProcessSnapshot(pid)).first;
    if(process->second.image.empty() || process->second.started == 0){
        for(const AppProfile& profile : *context.profiles)
            if(std::find(profile.classNames.begin(), profile.classNames.end(),
                         className) != profile.classNames.end())
                (*context.snapshots)[profile.id].enumerationComplete = false;
        return TRUE;
    }

    const AppProfile* profile = ClassifyBrowserCandidate(
        className, process->second.image, *context.profiles);
    if(profile){
        int titleLength = GetWindowTextLengthW(hwnd);
        std::wstring title;
        if(titleLength > 0){
            title.resize(static_cast<size_t>(titleLength) + 1, L'\0');
            int copied = GetWindowTextW(hwnd, &title[0], titleLength + 1);
            if(copied <= 0){
                (*context.snapshots)[profile->id].enumerationComplete = false;
                return TRUE;
            }
            title.resize(static_cast<size_t>(copied));
        }
        FastWin fast;
        fast.app = profile->id;
        fast.hwnd = hwnd;
        fast.pid = pid;
        fast.processStart = process->second.started;
        fast.title = title;
        HRESULT desktopResult = g_vdmDoc
            ? g_vdmDoc->GetWindowDesktopId(hwnd, &fast.desktop)
            : E_NOINTERFACE;
        if(FAILED(desktopResult) || GuidIsZero(fast.desktop))
            (*context.snapshots)[profile->id].desktopLookupsComplete = false;
        (*context.snapshots)[profile->id].windows.push_back(std::move(fast));
    }
    return TRUE;
}

static std::map<std::string,AppFastSnapshot> CollectFastSnapshots(){
    static SnapshotVersionTracker versions;
    std::vector<AppProfile> profiles = ActiveProfiles();
    std::map<std::string,AppFastSnapshot> result;
    for(const AppProfile& profile : profiles) result[profile.id];
    FastEnumContext context{&profiles, &result, {}};
    if(!EnumWindows(EnumFastWindow, reinterpret_cast<LPARAM>(&context)))
        for(auto& entry : result) entry.second.enumerationComplete = false;
    for(const AppProfile& profile : profiles){
        AppFastSnapshot& snapshot = result[profile.id];
        std::sort(snapshot.windows.begin(), snapshot.windows.end(),
                  [](const FastWin& a, const FastWin& b){
                      return reinterpret_cast<uintptr_t>(a.hwnd) <
                             reinterpret_cast<uintptr_t>(b.hwnd);
                  });
        for(const FastWin& fast : snapshot.windows){
            uint64_t identity = HashCombine(
                0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fast.hwnd)));
            identity = HashCombine(identity, static_cast<uint64_t>(fast.pid));
            identity = HashCombine(identity, fast.processStart);
            snapshot.windowSetSignature =
                HashCombine(snapshot.windowSetSignature, identity);
            snapshot.layoutSignature = HashCombine(
                snapshot.layoutSignature, HashGuid(fast.desktop));
        }
        snapshot.settleSignature = snapshot.windowSetSignature;
        for(const FastWin& fast : snapshot.windows){
            snapshot.settleSignature = HashCombine(
                snapshot.settleSignature, fast.title.size());
            snapshot.settleSignature = HashBytes(
                snapshot.settleSignature, fast.title.data(),
                fast.title.size() * sizeof(wchar_t));
        }
        uint64_t identitySignature = HashCombine(
            snapshot.windowSetSignature, ProfileConfigSignature(profile));
        identitySignature = HashCombine(identitySignature,
            snapshot.enumerationComplete ? 1 : 0);
        identitySignature = HashCombine(identitySignature,
            snapshot.desktopLookupsComplete ? 1 : 0);
        uint64_t contentSignature = HashCombine(
            identitySignature, snapshot.layoutSignature);
        contentSignature = HashCombine(contentSignature, snapshot.settleSignature);
        SnapshotVersions assigned = versions.observe(
            profile.id, identitySignature, contentSignature);
        snapshot.identityGeneration = assigned.identityGeneration;
        snapshot.generation = assigned.contentGeneration;
    }
    return result;
}
~~~

Session path discovery and file stamps are intentionally absent from this UI
collector; only the worker in Task 4 may perform them. Delete `EnumAppWindows`,
`AnyAppPresent`, and `LayoutSignature` after all callers
have moved to `CollectFastSnapshots`; otherwise the old multi-pass path can
accidentally return through a future call site.
Session requests validate `identityGeneration`; reconcile plans and manual
snapshot transactions validate the full content `generation`. Re-collecting an
unchanged snapshot therefore cannot stale an async result, while a changed HWND,
quality/config state, title, or desktop still invalidates the affected work.
An empty title is a valid transient browser-window state: classification happens
before title capture, and that HWND remains in the snapshot with an empty title.
Add an injected enumeration regression proving it is neither dropped nor used to
mark a retained sibling missing; only an actual title API failure after a
positive length makes that profile snapshot incomplete.

- [ ] **Step 5: Load, reconcile, and atomically save the in-memory layout**

Add:

~~~cpp
static bool LoadAutoLayout(){
    LayoutLoadResult loaded = LoadLayoutWithBackup(LayoutPath(false), UtcNowSeconds());
    if(loaded.status == LayoutLoadStatus::Unavailable){
        g_autoWritesAllowed = false;
        Balloon(L"The saved layout is temporarily unavailable; automatic changes are paused and will retry.");
        return false;
    }
    if(loaded.status == LayoutLoadStatus::Missing ||
       loaded.status == LayoutLoadStatus::CorruptPreserved){
        g_autoDesktops.clear();
        g_autoRecords.clear();
        g_autoLoaded = true;
        g_autoWritesAllowed = loaded.writesAllowed;
        g_autoDirty = false;
        g_preserveBackupOnNextWrite = false;
        g_autoRevision = loaded.revision;
        if(loaded.status == LayoutLoadStatus::CorruptPreserved)
            Balloon(L"Both layout copies were invalid. Diagnostic copies were kept; VDE started with an empty layout.");
        return true;
    }
    g_autoDesktops = loaded.desks;
    std::vector<LayoutWin> pruned = PruneExpired(loaded.wins, UtcNowSeconds());
    bool removedExpired = pruned.size() != loaded.wins.size();
    g_autoRecords.swap(pruned);
    g_autoLoaded = true;
    g_autoWritesAllowed = loaded.writesAllowed;
    g_autoRevision = loaded.revision;
    bool recovered = loaded.status == LayoutLoadStatus::Recovered;
    g_autoDirty = recovered || loaded.sourceVersion < 4 || removedExpired;
    g_preserveBackupOnNextWrite = recovered;
    if(recovered)
        Balloon(L"The primary layout was invalid; VDE restored the valid backup.");
    return true;
}

static bool MigrateLegacyLayout(){
    ScopedLayoutLock lock;
    if(!lock.acquired()) return false;
    std::wstring legacy = DataDir() + L"\\layout.txt";
    std::wstring automatic = LayoutPath(false);
    FileReadResult target = ReadFileBytesBounded(
        automatic, MAX_LAYOUT_FILE_BYTES);
    if(target.status == FileReadStatus::Ok) return true;
    if(target.status != FileReadStatus::Missing) return false;
    FileReadResult source = ReadFileBytesBounded(legacy, MAX_LAYOUT_FILE_BYTES);
    if(source.status == FileReadStatus::Missing) return true;
    if(source.status != FileReadStatus::Ok) return false;
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string error;
    int version = 0;
    if(!ParseLayout(source.bytes, desks, wins, UtcNowSeconds(), &error, &version))
        return false;
    if(!AtomicWriteText(automatic, SerializeLayout(desks, wins), &error))
        return false;
    // Retire only after the v4 target is durable. Failure to rename the source
    // is non-destructive; the existing automatic target wins on next launch.
    MoveFileExW(legacy.c_str(), (legacy + L".migrated").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return true;
}

static bool PersistAutoLayout(){
    if(!g_autoLoaded || !g_autoWritesAllowed) return false;
    ScopedLayoutLock lock;
    if(!lock.acquired()){
        g_autoDirty = true;
        return false;
    }
    LayoutRevision current = ReadLayoutRevisionLocked(LayoutPath(false));
    if(SameRevision(current, g_autoRevision) &&
       !g_deferredConflictRecordIds.empty()){
        g_autoDirty = true;
        return false;
    }
    if(!SameRevision(current, g_autoRevision)){
        LayoutLoadResult latest = LoadLayoutWithBackupLocked(
            LayoutPath(false), UtcNowSeconds());
        if(!latest.usable()){
            g_autoDirty = true;
            return false;
        }
        // Rebase only explicit/validated local deltas by recordId. Preserve
        // external-only IDs; never replace the whole newer vector blindly.
        RebaseResult rebased = RebaseRecordDeltas(
            latest.wins, latest.revision,
            g_dirtyRecordDeltas, UtcNowSeconds());
        g_deferredConflictRecordIds = rebased.deferredConflictRecordIds;
        if(!g_deferredConflictRecordIds.empty()){
            g_autoRecords = rebased.records;
            g_autoRevision = latest.revision;
            g_autoDirty = true;
            return false;
        }
        g_autoRecords = std::move(rebased.records);
        g_autoRevision = latest.revision;
    }
    std::string error;
    std::string text = SerializeLayout(CurrentDesktops(), g_autoRecords);
    if(!AtomicWriteText(LayoutPath(false), text, &error,
                        g_preserveBackupOnNextWrite)){
        g_autoDirty = true;
        Balloon(L"Could not save the automatic layout; the previous copy was kept.");
        return false;
    }
    g_preserveBackupOnNextWrite = false;
    g_autoRevision = ReadLayoutRevisionLocked(LayoutPath(false));
    g_dirtyRecordDeltas.clear();
    g_deferredConflictRecordIds.clear();
    g_autoDirty = false;
    return true;
}

static bool IsTrackedBrowserWindow(const WindowIdentityKey& expected,
                                   std::string* appOut){
    HWND hwnd = reinterpret_cast<HWND>(expected.hwnd);
    if(!IsWindow(hwnd)) return false;
    DWORD actualPid = 0;
    GetWindowThreadProcessId(hwnd, &actualPid);
    if(actualPid != expected.pid) return false;
    wchar_t className[64] = {0};
    if(GetClassNameW(hwnd, className, 64) <= 0) return false;
    ProcessSnapshot process = ReadProcessSnapshot(actualPid);
    std::vector<AppProfile> profiles = ActiveProfiles();
    const AppProfile* profile = ClassifyBrowserCandidate(
        className, process.image, profiles);
    if(!profile || process.started != expected.processStart) return false;
    if(appOut) *appOut = profile->id;
    return true;
}

static bool AutoPersistenceReady(){
    return g_autoFix && g_autoLoaded && g_autoWritesAllowed && !g_degraded;
}

static HRESULT IssueWindowMove(HWND hwnd, const GUID& destinationGuid){
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if(pid == GetCurrentProcessId())
        return g_vdmDoc->MoveWindowToDesktop(hwnd, destinationGuid);
    IVirtualDesktop* destination = GetDesktopByGuid(destinationGuid);
    if(!destination) return E_INVALIDARG;
    IApplicationView* view = nullptr;
    HRESULT result = g_avc->GetViewForHwnd(hwnd, &view);
    if(SUCCEEDED(result) && view)
        result = g_vdmi->MoveViewToDesktop(view, destination);
    else if(SUCCEEDED(result))
        result = E_FAIL;
    if(view) view->Release();
    destination->Release();
    return result;
}
~~~

`ClassifyBrowserCandidate(className,image,profiles)` is the sole class/executable
classifier used by both the collector and explicit popup persistence. The
collector supplies its cached process snapshot; the popup path recaptures PID,
process start, class, and image. Classification is independent of
`AutoPersistenceReady`: a configured browser with unavailable/read-only storage
is still tracked and must produce an explicit save failure. Add tests for same
exe/wrong class, same class/wrong exe, disabled profile, reused process-start
time, and unavailable/read-only automatic storage.

Delete the old `VerifyOnDesktop` and blocking `MoveWindowToDesktop` helpers after
manual, auto, and picker call sites use `IssueWindowMove` plus their timer-driven
read-back.

`PersistAutoLayout` owns this transaction lock. `AtomicWriteText` never acquires
one, so there is no recursive wait on the named mutex.
Every semantic record upsert/delete also records a `RecordDelta`; last-seen-only
touches remain in memory until heartbeat/final and do not create a delta. Capture
the current `g_autoRevision`, the prior same-ID record (or its absence), the
mutation kind/time, and the runtime generation when the delta is created.
`RebaseRecordDeltas` compares each delta's base revision and base record with the
newly loaded ID-keyed record, preserves external-only IDs, and applies an
unchanged-base delta directly. For a same-ID concurrent change, a still-valid
explicit/runtime upsert wins only when its `changedUtc` is newer than the latest
record's semantic timestamp; an identity/generation mismatch, tie, older delta,
or destructive missing/expiry delta is deferred, remains dirty, and cannot
overwrite the newer record. An expiry tombstone may apply after a revision
change only when the latest record is independently expired at `nowUtc`.
Keep the returned deferred record-ID set in `g_deferredConflictRecordIds`.
A second persist against the same disk revision must fail without serializing or
clearing any delta. Only a newer accepted mutation for that exact record may
replace its delta and clear its conflict ID, or a newly observed disk revision
may trigger another rebase; unrelated mutations never clear the blocked ID.
Add two-actor tests for both different-ID merge and same-ID conflict (newer local
validated upsert wins; older/tied local change and stale tombstone preserve the
disk record and remain pending). Call persist twice after the deferred conflict
and assert the second call still performs no write and retains the delta/conflict
set. No stale whole-vector overwrite is permitted.
Add a migration fault test: inject a failed target atomic write and assert the
legacy `layout.txt` remains byte-identical and no partially published automatic
layout exists. A successful migration must parse first, install v4 atomically,
then retire (or harmlessly leave) the source.

- [ ] **Step 6: Integrate generation-safe per-window reconciliation**

Replace the old monitor body with an event-driven pipeline. `TIMER_MONITOR`
does only the fast single-pass HWND snapshot and lifecycle observation; it never
reads a browser file. For present profiles it may enqueue/coalesce the Task 4
worker metadata probe, which performs path/stamp work off-thread.
`WM_SESSION_RESULT` resumes the operation that requested
that generation. `TIMER_MOVE_VERIFY` advances one nonblocking move action.

Apply one quality gate before any lifecycle observation: if enumeration is
incomplete for a profile, do not call `LcObserve`, mark missing, prune, bind, or
save that profile. If any accepted window lacks a valid desktop GUID, preserve
that identity's saved desktop and retry; never serialize `GUID_NULL`.

Use owner- and operation-qualified move identities:

~~~cpp
struct MoveRuntimeBinding {
    FastWin window;
    GUID destination = {0};
};

struct RuntimeRecordBinding {
    std::string app;
    std::string recordId;
    WindowIdentityKey identity;
};
~~~

Key `g_moveRuntime` by `jobId`, not by runtime string. Keep owner-specific
operation maps and outstanding counts. A single `DispatchMoveResult` routes by
`MoveToken.owner`; a stale result whose operation ID is no longer live is
ignored after its binding is erased. `CancelOperation(owner,id)` completes
every cancelled item exactly once. Manual or picker work may supersede an
automatic job for the same full identity, but must deliver the cancellation to
the original automatic batch.

Immediately before every Issue and every Verify, recapture and compare the full
`{HWND,pid,processStart}` identity. A vanished/reused identity completes as
`Cancelled`; an unknown/deleted destination or non-retryable COM error completes
as `PermanentFailure`; only retryable Issue/read-back failures consume the four
attempts and can become `Exhausted`. Owner handlers must not charge Cancelled to
an exhaustion budget.

For each accepted `Fresh` or `CachedStale` session result:

1. Re-collect/validate the fast snapshot generation. Abort if enumeration or any
   desktop lookup was incomplete.
2. Drop runtime bindings whose full `{HWND,pid,processStart}` identity is no
   longer present.
3. Process still-bound windows first. Their actual desktop is authoritative
   user movement and is saved immediately. Update fingerprint fields only from
   `Fresh`; `CachedStale` only touches last-seen and preserves the previous
   fingerprint.
4. Reserve those bound record IDs. Build reconciliation input only from
   unbound live identities and saved records not reserved by another live
   identity. `PlanAppReconcile` accepts a
   `reservedRecordIds` set and excludes it both from matching and from
   missing-marking.
5. Match/prune expired records before any restore. `Fresh` may create unmatched
   new records at their actual desktop. `CachedStale` may restore a confident
   existing match, but leaves unmatched live windows pending until Fresh.
6. Queue only required unbound-window restores. Bind a matched runtime only if
   it was already on the destination or its verified move succeeded. Never bind
   a failed restore; that preserves its remembered destination during heartbeat
   and final snapshots.
7. Commit successful, failed, new, and missing items independently. One failed
   window does not discard successful sibling updates.

Steps 4-5 (candidate scoring/assignment) run on one bounded, coalesced
`ReconcileWorker`, never in `WM_SESSION_RESULT` or another UI handler. Its
request owns immutable saved/live snapshots plus app, freshness, reserved IDs,
operation ID, and full content generation; its posted result is accepted only when
all IDs still match. At most one active plus one pending request per app exists.
`tooComplex` returns a deferred/no-mutation result and a rate-limited warning.
Stop/join this worker with the session worker at shutdown. Add a responsiveness
test using an injected slow scorer: enqueue/UI return is immediate, stale result
is ignored, and no more than one active plus one pending request is retained.

Treat last-seen refresh as in-memory bookkeeping: it does not by itself mark
the file dirty every monitor tick. Desktop/fingerprint/new/missing/prune changes
do mark dirty and are coalesced into one short (maximum 500 ms) atomic write;
heartbeat and finalization force a checkpoint. Rate-limit identical storage
error balloons until state changes or at least five minutes pass, while dirty
state continues to retry.

Add a regression where bound window A is manually moved while unbound window B
appears: A's new desktop is saved, only B participates in restore-before-save,
and no job moves A back. Also test two identical fingerprints with one bound ID;
the unbound window cannot steal the reserved ID.

The per-app restore operation stores
`{waveGeneration, windowSetSignature, sessionStamp}`. Completion is delivered
to `LcRestoreCompleted` with that generation and one of
`Success`, `Deferred`, or `Exhausted`; stale generations cannot mutate
state. A signature change during an in-flight operation arms exactly one later
wave. The four MoveQueue attempts are the total budget for a
`{recordId,fullRuntimeIdentity,destination}` key. Exhaustion blocks only that
failed identity until its own identity/destination changes or the user
explicitly retries; unrelated session/sibling churn cannot reset it. It does
not loop every monitor tick and does not block healthy windows.
Session deferral is limited to three retries per
`{windowSetSignature,sessionStamp}`, then waits for a new stamp/window set.

`SaveObservedApp` updates only validated bound IDs. If an unbound identity is
present it requests reconciliation and returns without overwriting a remembered
destination. `TouchBoundRecords` never clears missing or changes a desktop for
an identity that is no longer fully valid.

The final snapshot is separate and never waits for session or move work:

- bound identities take their actual current desktop and retain the latest good
  fingerprint if no Fresh session result exists;
- a successfully reconciled unbound identity is already bound and follows that
  rule;
- failed/unbound matched identities keep the saved destination;
- failed/reappeared unbound identities first match by their recorded pending
  record ID, then by strict same-app title fallback; they keep the saved
  destination and are never overwritten by the actual failed position;
- a remaining genuinely unmatched runtime identity is appended even if its
  session read has not completed, using a provisional title-only fingerprint
  and its actual desktop. This shutdown-only fallback ensures a window opened
  immediately before Exit is remembered; the next Fresh observation upgrades
  that same record rather than adding a duplicate;
- an actually absent app is marked missing from last-seen even on the first
  startup observation;
- expired records are pruned and the result is written atomically.

Finalization applies the same quality gate per profile/window: a fully
incomplete profile is preserved byte-for-byte; valid identities may update,
while an identity with failed desktop read-back retains its saved desktop. A
manual Save aborts the whole manual transaction on any incomplete enumeration
or desktop lookup and leaves the previous file byte-identical.

Add finalization regressions for (a) a new browser window followed immediately
by Exit, (b) a failed reappeared match plus a new sibling, and (c) zero live
windows: the new position persists, the failed saved destination survives, and
all absent records are missing-marked/pruned without waiting for session data.
Add three quality regressions: initial monitor with partial `EnumWindows`, final
snapshot with one failed desktop lookup, and manual Save with an incomplete
snapshot. None may mark/prune good data or write a zero GUID.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
rg -n "Sleep\(|g_seenKeys|missingRuns|g_restoreByRuntime|ReadSessionFor" src
~~~

Expected: tests/build exit 0; search finds none of the removed paths. Required
tests include mixed automatic/manual runtime keys, stale operation completion,
partial batch success, no repeat after exhaustion, one rearm after in-flight
signature change, deferred retry reset on new stamp, initial-absence expiry, and
the bound-A/unbound-B regression.
- [ ] **Step 7: Add normal and Windows-session finalization**

Add:

~~~cpp
enum class CheckpointReason {
    Heartbeat, SettingsChange, QueryEndSession, Finalize
};

static bool CheckpointAutoLayout(CheckpointReason reason){
    if(!g_autoFix || g_degraded) return true;
    if(reason == CheckpointReason::Heartbeat &&
       !g_reservedAutoIdentities.empty()){
        g_heartbeatDeferred = true;
        return true;
    }
    auto snapshots = CollectFastSnapshots();
    CommitFinalSnapshots(snapshots, g_reservedAutoIdentities);
    // A forced checkpoint persists refreshed last-seen values even when no
    // semantic desktop/fingerprint delta set dirty during the normal monitor.
    g_autoDirty = true;
    return PersistAutoLayout();
}

static void FinalizeAutoLayout(){
    if(!g_autoFix || g_degraded || !g_finalization.begin()) return;
    if(CheckpointAutoLayout(CheckpointReason::Finalize)) g_finalization.finish();
    else g_finalization.retry(); // WM_DESTROY gets one more safe attempt.
}
~~~

In the existing `WM_TIMER` branch, route the heartbeat explicitly before the
monitor/move cases:

~~~cpp
if(wp == TIMER_HEARTBEAT){
    CheckpointAutoLayout(CheckpointReason::Heartbeat);
    return 0;
}
~~~

Wire messages:

~~~cpp
case WM_QUERYENDSESSION:
    // Shutdown can still be cancelled, so checkpoint without permanently
    // disabling later saves.
    CheckpointAutoLayout(CheckpointReason::QueryEndSession);
    return TRUE;
case WM_ENDSESSION:
    if(wp) FinalizeAutoLayout();
    return 0;
case WM_DESTROY:
    FinalizeAutoLayout();
    KillTimer(hwnd, TIMER_MONITOR);
    KillTimer(hwnd, TIMER_MOVE_VERIFY);
    KillTimer(hwnd, TIMER_HEARTBEAT);
    StopSessionWorker();
    StopReconcileWorker();
    DrainPostedSessionResults(hwnd);
    DrainPostedReconcileResults(hwnd);
    TrayRemove();
    UnregisterHotKey(hwnd, 1);
    PostQuitMessage(0);
    return 0;
~~~

Factor the message decisions behind an injected checkpoint seam and add tests
that one `TIMER_HEARTBEAT` dispatch performs exactly one checkpoint, that a
failed heartbeat write keeps dirty state/retries without spinning, and that no
heartbeat runs before load succeeds or after auto-fix is disabled. Also test
for `WM_QUERYENDSESSION -> WM_ENDSESSION(TRUE) -> WM_DESTROY`: Query checkpoints
without finalizing, successful End prevents a duplicate Destroy write, and a
failed End write leaves state retryable so Destroy attempts again. Cover an
immediately opened new window, a normal move, and zero live windows in this
session-end chain, not only ordinary Exit.

`CommitFinalSnapshots` treats every entry in `g_reservedAutoIdentities` as
in-flight: it never copies that runtime identity's possibly intermediate actual
desktop. For a bound reservation it preserves the saved record ID byte-for-byte.
For a tracked, unbound reservation it upserts the captured
`provisionalOriginRecord` (stable newly allocated ID, app, best accepted
fingerprint/title, last-seen, and pre-move origin desktop), never the in-flight
desktop. Non-browser reservations have no provisional record and are omitted.
A heartbeat defers while any reservation exists. Query-end/finalize cannot wait,
so they checkpoint all other identities while preserving the reserved record
byte-for-byte. Task 11 runs one deferred heartbeat after the terminal transition
acknowledgement. Test target-move Issue -> heartbeat and Issue -> session-end:
neither may serialize the intermediate destination; siblings still checkpoint.
Add an unbound-new-browser session-end case: exactly one provisional record with
the reservation's stable ID and origin desktop survives, then the next Fresh
startup observation upgrades that same ID rather than creating a duplicate.

At startup, call `MigrateLegacyLayout` and then `LoadAutoLayout` before enabling
timers. If either returns false, leave auto-fix paused and show one retry/error
balloon. Initialize each app state by feeding its first `FastSnapshot` to
`LcObserve`, and start `TIMER_HEARTBEAT` only after loading succeeds.
Forward-declare `CheckpointAutoLayout`, `MigrateLegacyLayout`, and
`LoadAutoLayout`, then replace `ApplyAutoFix` with:

~~~cpp
static void ScheduleAutoLoadRetry(uint64_t nowMs){
    const uint64_t delay = std::min<uint64_t>(60000,
        1000ULL << std::min<unsigned>(g_autoLoadRetryAttempt++, 6));
    g_nextAutoLoadRetryMs = nowMs + delay;
}

static bool TryLoadAutoLayoutAndInitialize(){
    if(!MigrateLegacyLayout() || !LoadAutoLayout()) return false;
    g_autoLoadRetryAttempt = 0;
    g_nextAutoLoadRetryMs = 0;
    InitializeLifecycleFromFirstSnapshots(CollectFastSnapshots());
    SetTimer(g_main, TIMER_HEARTBEAT, HEARTBEAT_INTERVAL_MS, nullptr);
    return true;
}

static void ApplyAutoFix(){
    if(g_autoFix && !g_degraded){
        if(!g_autoLoaded && !TryLoadAutoLayoutAndInitialize()){
            ScheduleAutoLoadRetry(MonotonicNowMs());
            SetTimer(g_main, TIMER_MONITOR, MONITOR_INTERVAL_MS, nullptr);
            return;
        }
        SetTimer(g_main, TIMER_MONITOR, MONITOR_INTERVAL_MS, nullptr);
    } else {
        KillTimer(g_main, TIMER_MONITOR);
        KillTimer(g_main, TIMER_HEARTBEAT);
        KillTimer(g_main, TIMER_MOVE_VERIFY);
        CancelMoveOwner(MoveOwner::AutoReconcile);
        CancelPendingSessionPurpose(SessionPurpose::AutoReconcile);
        g_pendingAutoOperations.clear();
        g_recordByRuntime.clear();
        g_lifecycleByApp.clear();
        g_autoLoaded = false;
    }
}
~~~

At the top of the existing `TIMER_MONITOR` route, before collecting for normal
lifecycle work:

~~~cpp
if(g_autoFix && !g_autoLoaded){
    uint64_t nowMs = MonotonicNowMs();
    if(nowMs < g_nextAutoLoadRetryMs) return 0;
    if(!TryLoadAutoLayoutAndInitialize()) ScheduleAutoLoadRetry(nowMs);
    return 0;
}
~~~

No lifecycle/missing/prune/save work runs before a successful load. Add a fake
clock/filesystem test for `Unavailable -> Unavailable -> Valid`: attempts obey
the capped backoff, the valid prior bytes are never replaced, initialization
runs once on recovery, and only then does the heartbeat start.

In Settings OK handling, first read `newAutoFix`, `newFirefox`, `newChrome`, and
`newEdge` into locals. If auto-fix is currently enabled and either it is being
disabled or any app checkbox changes, call
`CheckpointAutoLayout(CheckpointReason::SettingsChange)` before
assigning any of the four globals. If that checkpoint fails, keep the old
settings/runtime state, show one error, and leave the dialog open for retry;
do not silently disable the only future save path. On success assign them, save settings, clear the
per-app runtime/lifecycle maps when the profile set changed by cancelling only
`MoveOwner::AutoReconcile` and `SessionPurpose::AutoReconcile`, clearing
`g_pendingAutoOperations`, `g_recordByRuntime`, and `g_lifecycleByApp`; leave
manual/picker operations in their owner maps and keep `TIMER_MOVE_VERIFY` alive
while any such job remains. Then call
`ApplyAutoFix`. Enabling loads/reconciles the current layout before the first
timer tick; disabling saves the last current snapshot and cancels only
automatic restore jobs.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
~~~

Expected: both commands exit 0; a source search finds WM_QUERYENDSESSION and no old LcOnExit call.

- [ ] **Step 8: Prevent concurrent tray instances**

After computing `cli` and after successful `CoInitializeEx`, add one
process-lifetime mutex only for the tray path:

~~~cpp
HANDLE trayMutex = nullptr;
if(!cli){
    trayMutex = CreateMutexW(nullptr, TRUE,
        L"Local\\VirtualDesktopExtension.Tray.Instance");
    if(!trayMutex || GetLastError() == ERROR_ALREADY_EXISTS){
        if(trayMutex) CloseHandle(trayMutex);
        MessageBoxW(nullptr, L"Virtual Desktop Extension is already running.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }
}
~~~

After GUI/CLI dispatch and `ReleaseServices`, run:

~~~cpp
if(trayMutex){
    ReleaseMutex(trayMutex);
    CloseHandle(trayMutex);
}
CoUninitialize();
~~~

CLI commands therefore never take the tray-instance mutex; save/restore commands
use only `ScopedLayoutLock` for their layout transaction.

Run:

~~~powershell
cmd /c build.bat
~~~

Expected: exit 0 and no new warnings.

- [ ] **Step 9: Route manual checkpoints and restores through v4 storage**

Manual layout data uses the same strict parser, bounded read, named transaction
lock, and atomic writer, but it is not aged by the automatic monitor.

For tray **Save layout**, create a `ManualSaveOperation` with one captured fast
snapshot/generation and asynchronously request Fresh session data for every
enabled profile that has windows. Finish once: if any result is unavailable,
stale, superseded, the fast generation changed, enumeration was incomplete, or
any desktop GUID lookup failed, keep the previous manual file
and report retry. Otherwise assign new record IDs, set
`missingSinceUtc == 0`, serialize v4, and call `AtomicWriteText` under one
outer `ScopedLayoutLock`. Never parse a browser session in the tray handler.

For tray **Restore layout**, load through
`LoadLayoutWithBackup(LayoutPath(manual), now)`, resolve desktops by GUID only,
collect a fast snapshot, obtain session results asynchronously, and use
`MatchOneToOne`. Define:

~~~cpp
struct ManualMoveOperation {
    uint64_t operationId = 0;
    size_t outstanding = 0;
    size_t succeeded = 0;
    size_t failed = 0;
    bool completionReported = false;
    std::set<uint64_t> liveJobIds;
};
~~~

Each tray job receives a new `jobId`, a
`MoveToken{ManualTray, operationId, jobId, itemIndex}`, and a job-ID runtime
binding. Before enqueuing it, cancel an automatic job for the same full identity
and dispatch that cancellation to its automatic batch. The common move
dispatcher routes each result to `AcceptManualMoveResult`; it verifies the
operation/job membership, erases it once, decrements outstanding once, and
shows one summary balloon only when outstanding reaches zero. It never mutates
the loaded manual layout or automatic records.

Add `ResolveSavedDesktop` to `layout.hpp` and return `-1` when a saved GUID
no longer exists. Unknown/deleted desktops are skipped and counted as failures;
never fall back to desktop index zero.

CLI commands use the same bounded reader, matcher, `MoveQueue`, identity
checks, and four-attempt budget in local state. A CLI may block on a waitable
timer because it has no GUI loop:

~~~cpp
static bool WaitForCliVerify(HANDLE timer, DWORD delayMs){
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(delayMs) * 10000LL;
    return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
           WaitForSingleObject(timer, delayMs + 1000) == WAIT_OBJECT_0;
}
~~~

It must not use `Sleep`, global tray operation maps, or the tray-instance
mutex. Add tests for auto and manual jobs sharing a runtime identity without
result theft, a stale manual operation result, one completion report, partial
failure counts, cancellation, missing desktop, and proof that restore leaves
`layout-manual.txt` byte-identical.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
rg -n "RunSaveManual|RunRestore|AtomicWriteText|LoadLayoutWithBackup|WriteTextFile|Sleep\(" src\vde.cpp
~~~

Expected: builds pass; tray browser reads are asynchronous; CLI waits only with
the waitable timer; old `WriteTextFile` and `Sleep(` calls are absent.
- [ ] **Step 10: Commit monitor integration**

~~~powershell
git add src/vde.cpp src/layout.hpp src/layout_store.hpp src/lifecycle.hpp src/window_identity.hpp src/reconcile_worker.hpp tests/vdtest.cpp
git commit -m "refactor(monitor): use v4 app snapshots"
~~~

### Task 9: Add pure picker state and independent highlights

**Files:**

- Create: src/picker_state.hpp
- Modify: src/window_identity.hpp
- Modify: src/vde.cpp:570-727
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED state and color tests**

~~~cpp
static WindowIdentityKey IK(uintptr_t hwnd, DWORD pid, uint64_t started){
    WindowIdentityKey key;
    key.hwnd = hwnd;
    key.pid = pid;
    key.processStart = started;
    return key;
}

static void test_picker_distinguishes_current_selected_and_active(){
    PickerState state;
    state.currentDesktop = G(L"{231A0000-0000-0000-0000-000000000001}");
    state.selectedDesktop = G(L"{231A0000-0000-0000-0000-000000000002}");
    state.activeWindow = IK(10, 20, 30);
    CHECK(IsCurrentDesktop(state, state.currentDesktop));
    CHECK(IsSelectedDesktop(state, state.selectedDesktop));
    CHECK(!IsCurrentDesktop(state, state.selectedDesktop));
    CHECK(IsActiveWindow(state, IK(10, 20, 30)));
    CHECK(!IsActiveWindow(state, IK(10, 20, 31)));
}

static void test_picker_refresh_preserves_search_and_scroll(){
    PickerState state;
    state.searchText = L"github";
    state.searchActive = true;
    GUID desktop = G(L"{231A0000-0000-0000-0000-000000000001}");
    state.scrollByDesktop[GuidKey(desktop)] = 3;
    PickerState refreshed = PreservePickerUi(state);
    CHECK(refreshed.searchText == L"github");
    CHECK(refreshed.searchActive);
    CHECK(refreshed.scrollByDesktop[GuidKey(desktop)] == 3);
}

static void test_blend_color_respects_zero_and_full_alpha(){
    COLORREF background = RGB(10, 20, 30);
    COLORREF accent = RGB(110, 120, 130);
    CHECK(BlendColor(background, accent, 0) == background);
    CHECK(BlendColor(background, accent, 255) == accent);
}

static void test_dim_search_keeps_current_desktop_distinct(){
    COLORREF normalDim = PickerTileFill(
        RGB(20,20,20), RGB(20,90,160), RGB(8,8,8), false, true);
    COLORREF currentDim = PickerTileFill(
        RGB(20,20,20), RGB(20,90,160), RGB(8,8,8), true, true);
    CHECK(currentDim != normalDim);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure because picker_state.hpp types/functions do not exist.

- [ ] **Step 2: Implement picker_state.hpp**

~~~cpp
#pragma once
#include "str_util.hpp"
#include "window_identity.hpp"
#include <map>

inline std::string GuidKey(const GUID& guid){ return W2U8(GuidToString(guid)); }

struct PickerState {
    GUID currentDesktop = {0};
    GUID selectedDesktop = {0};
    int selectedIndex = -1;
    WindowIdentityKey activeWindow;
    std::wstring searchText;
    bool searchActive = false;
    bool controlledTransition = false;
    std::map<std::string,int> scrollByDesktop;
};

inline bool IsCurrentDesktop(const PickerState& state, const GUID& desktop){
    return GuidEq(state.currentDesktop, desktop);
}

inline bool IsSelectedDesktop(const PickerState& state, const GUID& desktop){
    return GuidEq(state.selectedDesktop, desktop);
}

inline bool IsActiveWindow(const PickerState& state, const WindowIdentityKey& identity){
    return SameIdentity(state.activeWindow, identity);
}

inline PickerState PreservePickerUi(const PickerState& state){ return state; }

inline COLORREF BlendColor(COLORREF background, COLORREF foreground, BYTE alpha){
    auto blend = [&](BYTE bg, BYTE fg){
        return static_cast<BYTE>((unsigned(bg) * (255 - alpha) +
                                  unsigned(fg) * alpha + 127) / 255);
    };
    return RGB(blend(GetRValue(background), GetRValue(foreground)),
               blend(GetGValue(background), GetGValue(foreground)),
               blend(GetBValue(background), GetBValue(foreground)));
}

inline COLORREF PickerTileFill(COLORREF normal, COLORREF current,
                               COLORREF dimColor, bool isCurrent, bool dimmed){
    COLORREF base = isCurrent ? current : normal;
    return dimmed ? BlendColor(base, dimColor, 160) : base;
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0 and all picker-state tests pass.

- [ ] **Step 3: Capture the actual current desktop and target identity**

In vde.cpp add:

~~~cpp
#include "picker_state.hpp"
static PickerState g_picker;

static GUID CurrentDesktopGuid(){
    GUID guid = {0};
    IVirtualDesktop* desktop = nullptr;
    if(g_vdmi && SUCCEEDED(g_vdmi->GetCurrentDesktop(&desktop)) && desktop){
        desktop->GetID(&guid);
        desktop->Release();
    }
    return guid;
}

static WindowIdentityKey CaptureIdentity(HWND hwnd){
    WindowIdentityKey key;
    key.hwnd = reinterpret_cast<uintptr_t>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    key.pid = pid;
    key.processStart = ReadProcessSnapshot(key.pid).started;
    return key;
}

static bool IdentityStillValid(const WindowIdentityKey& expected){
    HWND hwnd = reinterpret_cast<HWND>(expected.hwnd);
    return IsWindow(hwnd) && SameIdentity(expected, CaptureIdentity(hwnd));
}
~~~

BuildModel must set g_picker.currentDesktop from CurrentDesktopGuid and initialize selectedDesktop to current only when the previous selected GUID no longer exists. It must never derive currentDesktop from g_target.

- [ ] **Step 4: Paint independent tile and active-row backgrounds**

In Paint:

~~~cpp
const COLORREF currentTile = BlendColor(CLR_TILE, CLR_ACTIVE, 48);
const COLORREF activeRow = BlendColor(CLR_TILE, CLR_ACTIVE, 72);

bool isCurrent = IsCurrentDesktop(g_picker, t.guid);
bool isSelected = IsSelectedDesktop(g_picker, t.guid);
COLORREF fill = PickerTileFill(
    CLR_TILE, currentTile, CLR_TILE_DIM, isCurrent, dim);
FillRoundRect(hdc, t.rc, S(10), fill,
              isSelected ? CLR_ACTIVE : CLR_PASSIVE,
              isSelected ? S(2) : S(1));
~~~

Extend WinItem with WindowIdentityKey identity. Before drawing an active row, fill a rounded row rectangle with activeRow and draw a narrow CLR_ACTIVE bar at its left. The test for active is IsActiveWindow(g_picker, w->identity), not GetForegroundWindow, because popup owns foreground focus.

Mouse hover changes only `selectedDesktop` and `selectedIndex` together. It does
not change `currentDesktop`; Task 11 routes every selector through one helper.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
~~~

Expected: both commands exit 0; source contains separate isCurrent and isSelected values.

- [ ] **Step 5: Commit independent highlights**

~~~powershell
git add src/picker_state.hpp src/window_identity.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(picker): highlight active context"
~~~

### Task 10: Add footer links and correct visible branding

**Files:**

- Modify: src/picker_state.hpp
- Modify: src/vde.cpp:61-65,586-655,657-727,1137-1180
- Modify: tests/vdtest.cpp
- Modify: README.md

- [ ] **Step 1: Add footer geometry and exact text**

First add a RED pure regression:

~~~cpp
static void test_footer_literal_and_links_are_exact(){
    CHECK(BuildFooterText() ==
          L"Virtual Desktop Extension for Windows 11 by Volodymyr Moskvin (c) 2026 Conus Vision");
    CHECK(std::wstring(FooterRepoUrl()) ==
          L"https://github.com/conus-vision/win-vde");
    CHECK(std::wstring(FooterConusUrl()) == L"https://conus.vision");
}
~~~

Run `cmd /c build-test.bat` and observe the expected compile failure because the
footer helpers do not exist, then add the implementation below.

Add the pure source of truth to `picker_state.hpp`:

~~~cpp
inline const wchar_t* FooterRepoLabel(){ return L"Virtual Desktop Extension"; }
inline const wchar_t* FooterMiddle(){
    return L" for Windows 11 by Volodymyr Moskvin (c) 2026 ";
}
inline const wchar_t* FooterConusLabel(){ return L"Conus Vision"; }
inline const wchar_t* FooterRepoUrl(){
    return L"https://github.com/conus-vision/win-vde";
}
inline const wchar_t* FooterConusUrl(){ return L"https://conus.vision"; }
inline std::wstring BuildFooterText(){
    return std::wstring(FooterRepoLabel()) + FooterMiddle() + FooterConusLabel();
}
~~~

Then add:

~~~cpp
static int FOOTER_H = 34;
static int FOOTER_MIN_W = 720;
static RECT g_repoLinkRect = {0,0,0,0};
static RECT g_conusLinkRect = {0,0,0,0};
static int g_hoverFooterLink = 0; // 0 none, 1 repository, 2 Conus Vision
static const wchar_t* APP_NAME = L"Virtual Desktop Extension for Windows 11";
static const wchar_t* APP_SHORT = L"Virtual Desktop Extension";
static const wchar_t* REPO_URL = FooterRepoUrl();
static const wchar_t* CONUS_URL = FooterConusUrl();
~~~

In InitMetrics set `FOOTER_H = S(34)` and `FOOTER_MIN_W = S(720)`. Add
`FOOTER_H` to `DesiredClientSize` height, keep tile positions unchanged, and set
`size.cx = std::max(size.cx, FOOTER_MIN_W)` so the exact one-line footer cannot
clip when only one desktop exists. Paint it centered below the final tile row:

~~~cpp
std::wstring repo = FooterRepoLabel();
std::wstring middle = FooterMiddle();
std::wstring conus = FooterConusLabel();
int y = client.bottom - FOOTER_H;
SIZE repoExtent = {0}, middleExtent = {0}, conusExtent = {0};
SelectObject(hdc, fI);
GetTextExtentPoint32W(hdc, repo.c_str(), static_cast<int>(repo.size()), &repoExtent);
GetTextExtentPoint32W(hdc, middle.c_str(), static_cast<int>(middle.size()), &middleExtent);
GetTextExtentPoint32W(hdc, conus.c_str(), static_cast<int>(conus.size()), &conusExtent);
int totalWidth = repoExtent.cx + middleExtent.cx + conusExtent.cx;
int x = std::max(PAD, (client.right - totalWidth) / 2);
SetTextColor(hdc, g_hoverFooterLink == 1 ? CLR_HEAD : CLR_ACTIVE);
g_repoLinkRect = {x, y, x + repoExtent.cx, y + S(22)};
TextOutW(hdc, x, y, repo.c_str(), static_cast<int>(repo.size()));
x += repoExtent.cx;
SetTextColor(hdc, CLR_HINT);
TextOutW(hdc, x, y, middle.c_str(), static_cast<int>(middle.size()));
x += middleExtent.cx;
SetTextColor(hdc, g_hoverFooterLink == 2 ? CLR_HEAD : CLR_ACTIVE);
g_conusLinkRect = {x, y, x + conusExtent.cx, y + S(22)};
TextOutW(hdc, x, y, conus.c_str(), static_cast<int>(conus.size()));
~~~

- [ ] **Step 2: Add link hit-testing before tile activation**

At the top of WM_LBUTTONDOWN:

~~~cpp
if(PtInRect(&g_repoLinkRect, pt)){
    ShellExecuteW(hwnd, L"open", REPO_URL, nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}
if(PtInRect(&g_conusLinkRect, pt)){
    ShellExecuteW(hwnd, L"open", CONUS_URL, nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}
~~~

Add WM_SETCURSOR:

~~~cpp
case WM_SETCURSOR:{
    POINT point;
    GetCursorPos(&point);
    ScreenToClient(hwnd, &point);
    if(PtInRect(&g_repoLinkRect, point) || PtInRect(&g_conusLinkRect, point)){
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    }
    break;
}
~~~

At the start of `WM_MOUSEMOVE`, before tile hover logic, update only on a state
transition:

~~~cpp
int footerHover = PtInRect(&g_repoLinkRect, pt) ? 1 :
                  (PtInRect(&g_conusLinkRect, pt) ? 2 : 0);
if(footerHover != g_hoverFooterLink){
    g_hoverFooterLink = footerHover;
    RECT client = {0};
    GetClientRect(hwnd, &client);
    RECT footer = {0, client.bottom - FOOTER_H, client.right, client.bottom};
    InvalidateRect(hwnd, &footer, FALSE);
}
~~~

- [ ] **Step 3: Update visible strings and README without renaming legacy storage**

Replace visible Extention/Desktops Extention spellings in window titles, balloons, Help, About, tray tooltip, and comments shown to users. Keep these exact compatibility identifiers unchanged:

~~~text
%LOCALAPPDATA%\VirtualDesktopsExtention
HKCU\Software\VirtualDesktopsExtention
HKCU Run value VirtualDesktopsExtention
~~~

In README, state:

~~~markdown
- **30-day closed-window memory** — a closed Firefox, Chrome, or Edge window
  keeps its remembered virtual desktop for 30 days. If it reappears before
  expiry, VDE restores it before updating the saved layout.
- **Persistent Ctrl+Click picker** — moving the active window also switches to
  the destination desktop while keeping the picker open and its active context
  highlighted.
~~~

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
rg -n "Virtual Desktops Extention|Virtual Desktop Extention" src README.md
~~~

Expected: tests/build exit 0, including the complete literal and both URL
assertions. The search may match only documented legacy registry/data
identifiers, never a visible APP_NAME, title, help string, or README heading.

- [ ] **Step 4: Commit footer and branding**

~~~powershell
git add src/picker_state.hpp src/vde.cpp tests/vdtest.cpp README.md
git commit -m "feat(picker): add branded footer links"
~~~

### Task 11: Keep popup open with one verified Ctrl+Click state machine

**Files:**

- Modify: src/picker_state.hpp
- Modify: src/vde.cpp
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add RED reducer, selection, cancellation, and ordering tests**

Add one pure transition reducer test fixture. Use these phases (do not model the
flow with independent booleans):

~~~cpp
enum class PickerPhase {
    Idle,
    TargetIssue, TargetVerify,
    IdentityVerifyBeforePopup,
    PopupIssue, PopupVerify,
    IdentityVerifyBeforeSwitch,
    SwitchIssue, DestinationVerify,
    RollbackTargetIssue, RollbackTargetVerify,
    RollbackPopupIssue, RollbackPopupVerify,
    RollbackSwitchIssue, OriginVerify,
    SaveExactTarget, RefreshModel, FailureReport, FocusRestore
};
~~~

Add and call tests proving all of the following:

- mouse hover, arrows, Tab, and numeric selection all call one
  `SetPickerSelection(state, index, guid)` helper, so `selectedIndex` and
  `selectedDesktop` cannot diverge and no `g_sel` global remains;
- Ctrl+Click, Ctrl+Enter, and Ctrl+Space read the same selected GUID and enter
  `TargetIssue` only from `Idle`;
- a second Ctrl move, plain click/Enter desktop switch, footer navigation, and
  close/reopen commands are rejected while the phase is not `Idle`;
- the successful effect order is target issue/read-back, target identity
  revalidation, popup issue/read-back, target identity revalidation, desktop
  switch, current/popup destination read-back, exact-runtime save, refresh,
  then confirmed focus restore;
- identity loss after the target read-back but before popup/switch enters
  rollback and produces no switch and no save effect;
- every accepted partial forward operation remains controlled until target,
  popup, and current-desktop rollback read-backs finish;
- exhausting four rollback checks reports the actual target/popup/current
  state and never falsely changes the model to the origin;
- Esc during a transition sets `dismissed` and `cancelRequested`, stops new
  forward effects, performs any required verified rollback, and never shows or
  focuses the popup again;
- stale timer/message observations with an old generation are ignored;
- cancelling a pending automatic restore captures its `recordId`; successful
  Ctrl-move updates that exact record and leaves no duplicate ID/record;
- search text, search-active state, selected GUID, active identity, and
  per-desktop scroll survive a successful refresh.

Expected RED result: the current direct `Commit`/rollback path cannot express
these phases or pass the ordering and cancellation cases.

- [ ] **Step 2: Make picker state the single source of truth**

Extend `PickerState` with one nested transition value containing:

~~~cpp
struct PickerTransition {
    PickerPhase phase = PickerPhase::Idle;
    uint64_t generation = 0;
    WindowIdentityKey target;
    std::string pendingRecordId;
    GUID targetOrigin = {0};
    GUID popupOrigin = {0};
    GUID currentOrigin = {0};
    GUID destination = {0};
    GUID observedTargetDesktop = {0};
    GUID observedPopupDesktop = {0};
    GUID observedCurrentDesktop = {0};
    int forwardTargetAttempts = 0;
    int forwardPopupAttempts = 0;
    int forwardSwitchAttempts = 0;
    int rollbackTargetAttempts = 0;
    int rollbackPopupAttempts = 0;
    int rollbackSwitchAttempts = 0;
    int focusAttempts = 0;
    bool targetMayHaveMoved = false;
    bool popupMayHaveMoved = false;
    bool switchMayHaveChanged = false;
    bool cancelRequested = false;
    bool dismissed = false;
    bool failed = false;
    std::wstring diagnostic;
};
~~~

`PickerState::controlledTransition()` returns
`transition.phase != PickerPhase::Idle`; remove the writable
`controlledTransition` flag and the 250 ms guard timer. Add
`SetPickerSelection` and route every mouse/keyboard selection path through it.
The helper updates `PickerState::selectedIndex` and `selectedDesktop`; delete
the `g_sel` global entirely.

Move the actual search flag into `g_picker.searchActive`. Remove
`g_searchActive`; edit notifications update `g_picker.searchText` and
`g_picker.searchActive` directly. `PreservePickerUi` is then a real snapshot
of all preserved interaction state.

Define a pure:

~~~cpp
enum class PickerEvent {
    Begin, ApiCompleted, ReadbackCompleted, EffectCompleted,
    CancelRequested, Timer
};

struct PickerObservation {
    PickerEvent event = PickerEvent::Timer;
    uint64_t generation = 0;
    bool apiAccepted = false;
    bool identityValid = false;
    bool targetOnExpectedDesktop = false;
    bool popupOnExpectedDesktop = false;
    bool currentOnExpectedDesktop = false;
    bool popupIsForeground = false;
    GUID actualTargetDesktop = {0};
    GUID actualPopupDesktop = {0};
    GUID actualCurrentDesktop = {0};
};

enum class PickerEffectKind {
    None, ValidateTarget, MoveTarget, ReadTarget, MovePopup, ReadPopup,
    SwitchDesktop, ReadCurrent, SaveExactTarget, Refresh,
    ShowAndFocus, Hide, ReportFailure
};

struct PickerEffect {
    PickerEffectKind kind = PickerEffectKind::None;
    uint64_t generation = 0;
    GUID desktop = {0};
};

PickerEffect AdvancePickerTransition(PickerState&, const PickerObservation&);
~~~

Implement the reducer with a table covering every phase above. The event kind
determines how the current effect completed; a default/timer observation is
never treated as an API failure. Save, refresh, failure-report, and focus are
acknowledged post-action phases, so every terminal effect is emitted exactly
once and Idle is reached only after its acknowledgement. Each forward Issue
phase allows at most four Issue/Verify cycles for that object. Rollback resets
and uses separate four-attempt budgets. Treat even a failed HRESULT as
“may have moved” until read-back proves otherwise. Any forward failure or
cancellation enters rollback if an API may have accepted a move;
otherwise it terminates. Rollback verifies target at `targetOrigin`, moves and
verifies the popup at `currentOrigin` (while retaining `popupOrigin` for
diagnostics), then verifies the current desktop at `currentOrigin`. This keeps
the popup on the desktop that will be shown. If rollback is only partially
successful, terminal state is derived from the read-back GUIDs and the failure
message names the part that remains displaced. Never claim “original desktop
restored” without all three confirmations.
`FocusRestore` emits `ShowAndFocus`, then consumes an `EffectCompleted`
observation with `popupIsForeground`; retry at most four times and terminate
with an accurate focus warning rather than looping.

- [ ] **Step 3: Drive only reducer effects from the window procedure**

Capture `{HWND,pid,processStart}` before the popup takes foreground focus for
hotkey, tray, and double-click entry. On Ctrl activation:

1. Require `Idle`, nonzero current/selected GUIDs from the current desktop list,
   a valid selected tile, and a still-valid captured identity.
2. Increment the transition generation and capture target, popup, and current
   origin GUIDs separately plus the destination.
3. Cancel only a queued automatic job for that exact identity through the
   owner-aware move dispatcher, but first copy that job's optional `recordId`
   into the transition. Deliver its cancellation result to its original app
   batch.
4. Feed observations to `AdvancePickerTransition` from one short timer. Each
   returned effect performs exactly one COM call or read-back and posts the
   observation tagged with the same generation.

Before cancelling auto work, reserve a `ReservedAutoIdentity` under
`RuntimeKey(capturedIdentity)`. Capture generation, exact identity, app, pending
record ID when bound, and the pre-move origin desktop. For a tracked,
`AutoPersistenceReady()` unbound browser, allocate (without publishing) one
stable record ID and capture a safe origin record using the accepted Fresh
fingerprint or provisional title;
for ordinary windows leave the provisional flag false. The monitor and
checkpoint merger must neither enqueue nor normally save that identity until
the reducer reaches its terminal acknowledgement; add races where a monitor
tick and a heartbeat each land between target and popup phases.

Before every target Issue/Verify, immediately after target verification before
moving the popup, and again after popup verification before switching,
revalidate the full captured identity. Move the
popup itself to the destination and verify its desktop before calling
`SwitchDesktop`. After switching, verify both `CurrentDesktopGuid()` and the
popup desktop before any save or model refresh.

All `WM_LBUTTONDOWN`, `WM_KEYDOWN`, search-control commands, footer clicks,
plain `GoToDesktop`, and `HidePicker` entry paths consult the phase. During a
transition they do nothing, except Esc: Esc records cancellation, hides once,
and lets the reducer roll back without any later Show/Focus effect.
`WM_ACTIVATE/WA_INACTIVE` hides only in `Idle`; a non-idle transition owns
visibility. Do not use a delayed deactivation guard.

Define `SavePopupMovedWindow` rather than leaving it implicit:

~~~cpp
enum class PopupSaveStatus { NotTracked, Saved, Failed };
struct PopupSaveResult {
    PopupSaveStatus status = PopupSaveStatus::Failed;
    std::string app;
};
~~~

Its first action is to revalidate the captured identity and call
`IsTrackedBrowserWindow(identity,&app)`. If classification returns false
(ordinary window or disabled browser profile), return `NotTracked` without
touching automatic state. If it returns true but `AutoPersistenceReady()` is
false, return `Failed` with `app` and a precise storage/auto-fix diagnostic;
never silently treat an enabled tracked browser as untracked. The popup
move/switch itself still succeeds, but a tracked save failure is surfaced and
keeps its prior destination/delta retryable. If the writable tracked transition
captured a pending automatic `recordId`, update/bind exactly that record first;
never run matching or allocate a second record for it. For a valid bound
identity it updates only that record's desktop/last-seen and preserves its last
good fingerprint unless a Fresh result for the same identity generation exists.
For an unbound identity it uses that Fresh fingerprint when available; an
explicit user move with no usable session data may create one provisional
title-only record for this exact identity/destination using the reservation's
already allocated stable ID (the sole exception to
automatic “no title-only creation”), binds it immediately, and replaces its
fingerprint on the next Fresh result. It never runs general matching or changes
a duplicate sibling record.
Add the race regression: pending auto restore -> Ctrl-move -> success produces
exactly one record with the original pending `recordId` and the user-selected
desktop. Add `test_ctrl_move_non_browser_never_mutates_auto_layout`, covering an
unbound ordinary window with and without Fresh cached browser data elsewhere.
Add `test_ctrl_move_tracked_browser_unwritable_reports_failed`, asserting no
lifecycle completion callback and no mutation/file write.

On transition success, execute `SavePopupMovedWindow` for the captured identity
only after all read-backs. Call `LcExplicitSaveCompleted(result.app,...)` only
when `result.status == Saved`; `NotTracked` has no profile callback and is a
normal popup-only success, while `Failed` keeps the prior record/destination and
reports the persistence failure. Then refresh the model, set both current and
selected desktop to the verified destination, and restore topmost/focus. On move
failure, preserve the remembered destination for any failed automatic record,
rebuild current/selection from actual read-backs, show/focus only when
`dismissed == false`, and display the precise diagnostic.

Only after the reducer's terminal acknowledgement, release this generation's
entry from `g_reservedAutoIdentities`. If it was the last reservation and
`g_heartbeatDeferred` is set, clear the flag and post one heartbeat checkpoint;
never run it inline inside the reducer. Do this for success, verified rollback,
partial rollback, and dismissed completion. Add typed-result/callback-count and
deferred-heartbeat-on-terminal regressions.

- [ ] **Step 4: Preserve UI state through the verified refresh**

At entry to `RefreshPickerModelPreservingUi`, copy edit text into
`g_picker.searchText` and copy every tile's scroll into
`g_picker.scrollByDesktop`. Preserve selected GUID and active identity, rebuild,
then:

- set current from `CurrentDesktopGuid()`;
- restore selected GUID if it still exists, otherwise select current through
  `SetPickerSelection`;
- restore each scroll by desktop GUID;
- erase scroll entries for GUIDs no longer returned by the desktop manager, so
  repeated create/delete cycles cannot grow the map forever;
- restore edit text and `g_picker.searchActive`;
- rebuild tab blobs only when search text is non-empty.

The function must not read or write a second search-active global.
While the popup is visible and the reducer is Idle, a lightweight refresh timer
collects one fast snapshot and rebuilds current/active highlights through this
same preservation helper. It never reads session files. Add a test/QA case for
an external desktop switch and foreground-window change while the popup stays
open.

- [ ] **Step 5: Verify persistent behavior and commit**

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
rg -n "Sleep\(|RollBackPickerMove|TIMER_PICKER_GUARD|g_searchActive|controlledTransition =" src\vde.cpp src\picker_state.hpp
~~~

Expected: both builds exit 0; reducer tests pass; the search finds none of the
removed fire-and-forget/duplicate-state paths. Manual checks cover Ctrl+Click,
Ctrl+Enter, Ctrl+Space, keyboard selection, Esc during each forward phase, injected target
and popup move failures, switch failure, and duplicate automatic/manual work.

~~~powershell
git add src/picker_state.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(picker): verify persistent desktop moves"
~~~
### Task 12: Fix GDI ownership and picker hot paths

**Files:**

- Create: src/gdi_buffer.hpp
- Create: src/icon_cache.hpp
- Modify: src/vde.cpp:588-625,656-727,1293
- Test: tests/vdtest.cpp

- [ ] **Step 1: Add a RED GDI resize ownership test**

~~~cpp
static void test_gdi_buffer_resize_does_not_leak_selected_bitmaps(){
    HDC screen = GetDC(nullptr);
    if(!screen) return;
    DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    {
        GdiBuffer buffer;
        for(int i = 0; i < 100; ++i)
            CHECK(buffer.ensure(screen, 200 + i, 120 + i));
    }
    DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    ReleaseDC(nullptr, screen);
    CHECK(after <= before + 1);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure because GdiBuffer is undefined.

- [ ] **Step 2: Implement gdi_buffer.hpp**

~~~cpp
#pragma once
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

class GdiBuffer {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP original_ = nullptr;
    int width_ = 0;
    int height_ = 0;
public:
    GdiBuffer() = default;
    GdiBuffer(const GdiBuffer&) = delete;
    GdiBuffer& operator=(const GdiBuffer&) = delete;
    ~GdiBuffer(){ reset(); }
    bool ensure(HDC reference, int width, int height){
        if(width <= 0 || height <= 0) return false;
        if(!dc_){
            dc_ = CreateCompatibleDC(reference);
            if(!dc_) return false;
            original_ = static_cast<HBITMAP>(GetCurrentObject(dc_, OBJ_BITMAP));
        }
        if(bitmap_ && width_ == width && height_ == height) return true;
        HBITMAP replacement = CreateCompatibleBitmap(reference, width, height);
        if(!replacement) return false;
        HGDIOBJ previous = SelectObject(dc_, replacement);
        if(!previous || previous == HGDI_ERROR){
            DeleteObject(replacement);
            return false;
        }
        HBITMAP old = bitmap_;
        bitmap_ = replacement;
        width_ = width;
        height_ = height;
        if(old) DeleteObject(old);
        return true;
    }
    void reset(){
        if(dc_ && original_) SelectObject(dc_, original_);
        if(bitmap_) DeleteObject(bitmap_);
        if(dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        original_ = nullptr;
        width_ = height_ = 0;
    }
    HDC get() const { return dc_; }
};
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0 and the GDI object count remains stable.

- [ ] **Step 3: Replace raw picker double-buffer globals**

Include gdi_buffer.hpp, replace g_memDC/g_memBmp/g_memW/g_memH with:

~~~cpp
static GdiBuffer g_pickerBuffer;
~~~

At Paint start:

~~~cpp
if(!g_pickerBuffer.ensure(hdcReal, client.right, client.bottom)) return;
HDC hdc = g_pickerBuffer.get();
~~~

Delete the old selected-bitmap deletion code. At RunGui shutdown call g_pickerBuffer.reset after the message loop.

- [ ] **Step 4: Release fonts, brushes, and owned icons exactly once**

Replace the function-local `WM_CTLCOLOREDIT` brush with
`static HBRUSH g_searchBrush = nullptr;`, creating it once in `InitMetrics`.
Track every owned icon returned by `LoadAppIcon` in
`static std::vector<HICON> g_ownedIcons`; when the fallback is
`HICON fallback = LoadIconW(nullptr, IDI_APPLICATION);`, store
`CopyIcon(fallback)` rather than the shared handle. Create `CleanupUiResources`:

~~~cpp
static void CleanupUiResources(){
    g_pickerBuffer.reset();
    ClearWindowIconCache(); // forward-declare; implemented in Step 5
    HFONT* fonts[] = {&g_uiFont, &g_fPT, &g_fPN, &g_fPI, &g_fPX, &g_searchFont};
    for(HFONT* font : fonts){
        if(*font){
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    if(g_searchBrush){
        DeleteObject(g_searchBrush);
        g_searchBrush = nullptr;
    }
    for(HICON icon : g_ownedIcons) if(icon) DestroyIcon(icon);
    g_ownedIcons.clear();
    g_nid.hIcon = nullptr;
}
~~~

Window-icon cache ownership is handled separately in Step 5. On loop exit,
destroy any open Settings/About/Help/Compatibility
windows; `WM_DESTROY` has already called `TrayRemove`. Unregister `VdeSettings`, `VdeAbout`, `VdeCompat`,
`VdeHelp`, and `VdeWindow`, then call `CleanupUiResources` once. This makes class
icon ownership end before `DestroyIcon`.

Call `CleanupUiResources` once after `GetMessage` exits; remove the old separate
`DeleteObject(g_uiFont)` and any separate `DestroyIcon(g_nid.hIcon)` calls.
Replace `while(GetMessageW(...))` with a `for(;;)` loop that stores the return in
`BOOL messageResult`, breaks on `0`, and also breaks/report-errors on `-1`
without dispatching the undefined `MSG` contents.

- [ ] **Step 5: Bound icon lookup and make Paint draw-only**

Add `std::vector<size_t> filtered` to `Tile`. Create a self-contained,
test-visible `icon_cache.hpp` with injectable ownership operations:

~~~cpp
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <windows.h>

struct IconCacheOps {
    std::function<HICON(HICON)> copy;
    std::function<void(HICON)> destroy;
};

class OwnedIconCache {
public:
    OwnedIconCache(size_t limit, IconCacheOps ops);
    ~OwnedIconCache();
    OwnedIconCache(const OwnedIconCache&) = delete;
    OwnedIconCache& operator=(const OwnedIconCache&) = delete;
    HICON getAndTouch(const std::string& key, uint64_t touch);
    HICON peek(const std::string& key) const;
    HICON insertBorrowed(const std::string& key, HICON borrowed, uint64_t touch);
    void pruneTo(const std::set<std::string>& liveKeys);
    void clear();
    size_t size() const;
private:
    struct Entry { HICON owned = nullptr; uint64_t touched = 0; };
    void enforceLimit();
    size_t limit_;
    IconCacheOps ops_;
    std::map<std::string,Entry> entries_;
};
~~~

Implement every method inline: `insertBorrowed` copies before publication,
destroys a replaced owned handle exactly once, immediately enforces the LRU
limit, and returns only a still-retained owned handle; prune/evict/clear/destructor
use the injected destroy callback. Reject a zero limit or missing callbacks at
construction without accepting entries.

Use that cache from `vde.cpp`, keyed by the bounded runtime identity string:

~~~cpp
static uint64_t g_iconTouch = 0;
static HICON g_sharedFallbackIcon = LoadIconW(nullptr, IDI_APPLICATION);
static OwnedIconCache g_windowIconCache(256, {
    [](HICON icon){ return CopyIcon(icon); },
    [](HICON icon){ if(icon) DestroyIcon(icon); }
});

static HICON LoadWindowIconOutsidePaint(const FastWin& fast){
    std::string key = RuntimeKey(fast);
    HICON cached = g_windowIconCache.getAndTouch(key, ++g_iconTouch);
    if(cached) return cached;
    HICON icon = reinterpret_cast<HICON>(
        GetClassLongPtrW(fast.hwnd, GCLP_HICONSM));
    if(!icon) icon = reinterpret_cast<HICON>(
        GetClassLongPtrW(fast.hwnd, GCLP_HICON));
    if(!icon){
        DWORD_PTR response = 0;
        SendMessageTimeoutW(fast.hwnd, WM_GETICON, ICON_SMALL2, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 25, &response);
        icon = reinterpret_cast<HICON>(response);
    }
    if(!icon) icon = g_sharedFallbackIcon; // borrowed stable system source
    HICON owned = icon
        ? g_windowIconCache.insertBorrowed(key, icon, ++g_iconTouch) : nullptr;
    return owned ? owned : g_sharedFallbackIcon;
}

static HICON CachedWindowIcon(const FastWin& fast){
    HICON cached = g_windowIconCache.peek(RuntimeKey(fast));
    return cached ? cached : g_sharedFallbackIcon;
}

static void PruneIconCache(const std::set<std::string>& liveKeys){
    g_windowIconCache.pruneTo(liveKeys);
}

static void ClearWindowIconCache(){
    g_windowIconCache.clear();
}

static void RebuildFilteredRows(){
    for(Tile& tile : g_tiles){
        tile.filtered.clear();
        for(size_t i = 0; i < tile.windows.size(); ++i)
            if(g_picker.searchText.empty() ||
               tile.windows[i].search.find(g_picker.searchText) != std::wstring::npos)
                tile.filtered.push_back(i);
        const int visibleRows = 4;
        int maximum = std::max(0,
            static_cast<int>(tile.filtered.size()) - visibleRows);
        tile.scroll = std::max(0, std::min(tile.scroll, maximum));
    }
}
~~~

Never cache a borrowed class/window icon directly: its owner may replace and
destroy it without changing HWND identity. Every cached entry owns `CopyIcon`;
prune/LRU and `CleanupUiResources` (via `ClearWindowIconCache`) call
`DestroyIcon` exactly once before erase. The shared system fallback is never
destroyed.
Add a fake icon-ops test proving replacement/prune/shutdown balance copies and
destroys. Preload 257 distinct still-live identities without a model rebuild and
assert the cache is already capped at 256 immediately after the last insertion.

`EnumAll` captures `FastWin`/`WindowIdentityKey` once and accumulates its runtime
key in a local live-key set, but does not send `WM_GETICON` for every window.
After filtering/layout/scroll changes, preload icons only for rows that can be
painted (class icons are immediate; at most the visible rows can incur the 25 ms
timeout) by calling `LoadWindowIconOutsidePaint`, then invalidate. Paint calls
only `CachedWindowIcon`; it never calls the loader, sends a message, mutates the
cache, enumerates, filters, or parses.
`BuildModel` calls `PruneIconCache` after enumeration. On `EN_CHANGE`, lowercase
the edit text once, call `EnsureTabSearch` once (which consumes the Task 4 cache),
then call `RebuildFilteredRows` and invalidate. Paint iterates
`tile.filtered` only and performs no lowercase conversion, browser-session read,
or filter scan. `BuildModel` and `RefreshPickerModelPreservingUi` also call
`RebuildFilteredRows` after their model/search data is ready.

Run:

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
~~~

Expected: tests and build exit 0; source search finds no CreateCompatibleBitmap
outside GdiBuffer, no SendMessageTimeout value 200, and no
`LoadWindowIconOutsidePaint` call inside `Paint`.

- [ ] **Step 6: Commit resource and hot-path fixes**

~~~powershell
git add src/gdi_buffer.hpp src/icon_cache.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "perf(picker): bound UI work and GDI use"
~~~

### Task 13: Complete docs, audit coverage, and verification

**Files:**

- Modify: README.md
- Modify: src/vde.cpp
- Modify: tests/vdtest.cpp

- [ ] **Step 1: Add final parser/error regression cases**

Add and call tests for:

~~~cpp
static void test_layout_rejects_invalid_base64(){
    std::string text = V4Line(
        "{231A0000-0000-0000-0000-000000000001}",
        "{00000000-0000-0000-0000-000000000401}",
        "1700000000", "0");
    size_t title = text.find(b64enc("Inbox"));
    text.replace(title, b64enc("Inbox").size(), "%%%=");
    std::vector<DeskRec> d;
    std::vector<LayoutWin> w;
    std::string error;
    CHECK(!ParseLayout(text, d, w, 1800000000, &error));
}

static void test_unknown_desktop_guid_is_not_index_zero(){
    LayoutWin saved = Identified(
        "{00000000-0000-0000-0000-000000000402}", "firefox", 0,
        {{"a.com", 1}}, "A");
    saved.desktop = G(L"{231A0000-0000-0000-0000-000000009999}");
    CHECK(ResolveSavedDesktop(saved, {}) == -1);
}

static void test_disabled_app_is_not_marked_newly_missing(){
    const UnixSeconds now = 2000000000;
    LayoutWin firefox = Identified(
        "{00000000-0000-0000-0000-000000000403}", "firefox", 0,
        {{"a.com", 1}}, "A");
    std::vector<LayoutWin> records = MarkOnlyObservedAppsMissing(
        {firefox}, {}, {"chrome"}, now);
    CHECK(records[0].missingSinceUtc == 0);
}
~~~

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: compile failure because MarkOnlyObservedAppsMissing is undefined,
proving disabled-app bookkeeping is not yet covered.

- [ ] **Step 2: Implement disabled-app missing bookkeeping**

Add to layout.hpp:

~~~cpp
inline std::vector<LayoutWin> MarkOnlyObservedAppsMissing(
        const std::vector<LayoutWin>& records,
        const std::set<std::string>& liveRecordIds,
        const std::set<std::string>& observedApps,
        UnixSeconds nowUtc){
    std::vector<LayoutWin> output = records;
    for(auto& record : output)
        if(observedApps.count(record.app) && !liveRecordIds.count(record.recordId))
            MarkMissing(record, nowUtc);
    return PruneExpired(output, nowUtc);
}
~~~

Confirm every restore call site uses the Task 8 `ResolveSavedDesktop` helper and
never falls back to desktop index zero when GUID resolution fails.

Make the documented desktop manager a required capability and make partial COM
initialization cleanup idempotent:

~~~cpp
static void ReleaseServices(){
    if(g_vdmDoc){ g_vdmDoc->Release(); g_vdmDoc = nullptr; }
    if(g_avc){ g_avc->Release(); g_avc = nullptr; }
    if(g_vdmi){ g_vdmi->Release(); g_vdmi = nullptr; }
    if(g_shell){ g_shell->Release(); g_shell = nullptr; }
}

static bool InitServices(){
    HRESULT result = CoCreateInstance(
        kCLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
        __uuidof(IServiceProvider), reinterpret_cast<void**>(&g_shell));
    if(SUCCEEDED(result)) result = g_shell->QueryService(
        kCLSID_VirtualDesktopManagerInternal, kIID_IVirtualDesktopManagerInternal,
        reinterpret_cast<void**>(&g_vdmi));
    if(SUCCEEDED(result)) result = g_shell->QueryService(
        kIID_IApplicationViewCollection, kIID_IApplicationViewCollection,
        reinterpret_cast<void**>(&g_avc));
    if(SUCCEEDED(result)) result = CoCreateInstance(
        CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
        IID_IVirtualDesktopManager, reinterpret_cast<void**>(&g_vdmDoc));
    if(FAILED(result) || !g_vdmDoc){
        ReleaseServices();
        return false;
    }
    return true;
}
~~~

Forward-declare `ReleaseServices` before `InitServices`, and make
`SanityCheckServices` return false when any of `g_vdmi`, `g_avc`, or `g_vdmDoc`
is null. Remove the old behavior where verification returned success when
`g_vdmDoc` was absent.

Run:

~~~powershell
cmd /c build-test.bat
~~~

Expected: exit 0 with no FAIL lines.

- [ ] **Step 3: Update version and final README semantics**

Set the visible version constant exactly:

~~~cpp
static const wchar_t* APP_VERSION = L"1.1.0";
~~~

Add this README behavior block after the existing Features section:

~~~markdown
### Window memory and desktop picker

- Automatic window memory covers every Firefox, Google Chrome, and Microsoft
  Edge top-level browser window, and no other application.
- A closed browser window remains remembered for exactly 30 days. Reopening it
  before expiry restores its remembered virtual desktop before the rolling
  layout is updated; records expire at the 30-day boundary.
- Ctrl+Click moves the active window, switches to the target desktop, and keeps
  the picker open with the current desktop and active window highlighted.
  Ordinary click switches desktops and closes the picker.
- The footer links to [Virtual Desktop Extension](https://github.com/conus-vision/win-vde)
  and [Conus Vision](https://conus.vision).
- Layout v4 is migrated automatically from v2/v3. The legacy
  `%LOCALAPPDATA%\\VirtualDesktopsExtention` directory and matching registry key
  keep their historical spelling for compatibility.
~~~

- [ ] **Step 4: Run the complete fresh verification suite**

~~~powershell
cmd /c build-test.bat
cmd /c build.bat
git diff --check
git status --short
~~~

Expected:

- build-test.bat exits 0, prints no FAIL, and reports every test passed;
- build.bat exits 0 and prints Built build\vde.exe with no compiler warnings introduced by this branch;
- git diff --check prints nothing;
- git status lists only the intended source, test, README, and plan-progress changes before the final commit.

- [ ] **Step 5: Run targeted source audits**

~~~powershell
rg -n "missingRuns|MISSING_RUNS_MAX|g_seenKeys|LcOnExit|Sleep\\(" src tests
rg -n "CREATE_ALWAYS" src
rg -n "Virtual Desktops Extention|Virtual Desktop Extention" src README.md
rg -n "WM_QUERYENDSESSION|WM_ENDSESSION|controlledTransition|Conus Vision" src
~~~

Expected:

- first search returns no old lifecycle symbols and no Sleep in move verification;
- CREATE_ALWAYS appears only in the temporary-file writer, never against layout-auto.txt directly;
- misspelled branding appears only in explicitly documented legacy registry/data identifiers;
- final search finds session-end handling, popup transition guard, and footer text.

- [ ] **Step 6: Request code review and resolve every Critical/Important finding**

Dispatch a reviewer with:

~~~text
DESCRIPTION: layout v4 migration/retention, atomic store, per-app monitor,
async restore queue, persistent Ctrl+Click picker, footer links, and GDI cleanup
PLAN_OR_REQUIREMENTS: docs/superpowers/specs/2026-08-19-popup-lifecycle-reliability-design.md
BASE_SHA: 26481fc (production source baseline; approved design is e4ca435)
HEAD_SHA: current branch HEAD
FOCUS: data-loss paths, restore-before-save ordering, HWND identity,
shutdown idempotency, COM/GDI ownership, UI-thread blocking, and spec coverage
~~~

For every Critical or Important finding, add a failing regression test when deterministic, observe RED, apply the smallest fix, rerun build-test.bat and build.bat, and include the correction in the final commit.

- [ ] **Step 7: Commit final integration and documentation**

~~~powershell
git add src tests README.md docs/superpowers/plans/2026-08-19-popup-lifecycle-reliability.md docs/superpowers/specs/2026-08-19-popup-lifecycle-reliability-design.md
git commit -m "feat: complete reliable desktop workflow"
~~~

- [ ] **Step 8: Perform interactive Windows QA**

Use a normal interactive Windows 11 session:

1. Open the popup and verify separate current-desktop fill, selected outline, and active-window row.
2. Ctrl+Click another desktop: target moves, Windows switches, popup stays topmost, search/scroll remain, highlights move.
3. Repeat Ctrl+Click without closing popup.
4. Use ordinary click and verify popup closes after switch.
5. Verify the footer reads exactly `Virtual Desktop Extension for Windows 11 by Volodymyr Moskvin (c) 2026 Conus Vision`, then test Esc, outside click, search-all-tabs, wheel, tooltip, GitHub link, and Conus Vision link.
6. Keep Firefox open while launching Chrome; confirm Chrome restore occurs.
7. Keep one Firefox window open, close/reopen another, and confirm its saved destination returns.
8. Start with saved A plus new B; verify A restores and B exists in layout-auto.txt immediately.
9. Compare GetGuiResources GR_GDIOBJECTS before and after 100 popup open/resize/close cycles; the count must stabilize rather than grow each cycle.
10. Corrupt a disposable copy of layout-auto.txt, start VDE, and verify no blind moves occur and a valid .bak is used.
11. In a disposable Windows user session/VM, move/open a browser window and
    sign out normally; on the next sign-in verify the `WM_QUERYENDSESSION` /
    `WM_ENDSESSION` checkpoint restored it. Repeat with zero live windows and
    confirm missing/retention state was saved. Do not automate forced shutdown
    against the user's working session.

Record any environment-only limitation explicitly in the handoff; do not claim interactive behavior was verified if this session cannot run Explorer virtual-desktop COM.

## Plan self-review checklist

- Layout v4, strict migration, duplicate matching, 30-day boundary: Tasks 1-2.
- Atomic write, backup, bounded/partial I/O, concurrency: Task 3.
- mozLz4/SNSS hardening and browser-session cache: Task 4.
- Per-browser/window-set lifecycle and restore-before-save: Tasks 5-6.
- Nonblocking four-attempt move verification: Task 7.
- Monitor integration, zero-window exit, shutdown, mutex: Task 8.
- Current desktop, selection, active window, semitransparent fills: Task 9.
- GitHub/Conus links and corrected visible branding: Task 10.
- Move + switch + keep-open Ctrl+Click and immediate browser save: Task 11.
- GDI leak, icon timeout, draw-only Paint, bounded runtime state: Task 12.
- Edge cases, review, full verification, and manual Windows QA: Task 13.
