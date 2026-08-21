// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include "window_identity.hpp" // must be self-contained at first include
#include "gdi_buffer.hpp"
#include "icon_cache.hpp"
#include "picker_state.hpp"
#include "reconcile_worker.hpp"
#include "session_worker.hpp"
#include "move_queue.hpp"
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
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <type_traits>
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

static WindowIdentityKey IK(uintptr_t hwnd,DWORD pid,uint64_t started){
    WindowIdentityKey key;
    key.hwnd=hwnd;
    key.pid=pid;
    key.processStart=started;
    return key;
}

static void test_footer_literal_and_links_are_exact(){
    CHECK(BuildFooterText()==
          L"Virtual Desktop Extension for Windows 11 by Volodymyr Moskvin (c) 2026 Conus Vision");
    CHECK(std::wstring(FooterRepoLabel())==
          L"Virtual Desktop Extension");
    CHECK(std::wstring(FooterMiddle())==
          L" for Windows 11 by Volodymyr Moskvin (c) 2026 ");
    CHECK(std::wstring(FooterConusLabel())==L"Conus Vision");
    CHECK(std::wstring(FooterRepoUrl())==
          L"https://github.com/conus-vision/win-vde");
    CHECK(std::wstring(FooterConusUrl())==L"https://conus.vision");
    CHECK(std::wstring(PickerFooterUrl(PickerFooterLink::Repository))==
          FooterRepoUrl());
    CHECK(std::wstring(PickerFooterUrl(PickerFooterLink::ConusVision))==
          FooterConusUrl());
    CHECK(PickerFooterUrl(PickerFooterLink::None)==nullptr);
}

static void test_footer_minimum_size_is_one_line_and_dpi_scaled(){
    CHECK(PickerScaleForDpi(34,96)==34);
    CHECK(PickerScaleForDpi(34,120)==43);
    CHECK(PickerScaleForDpi(34,144)==51);
    CHECK(PickerScaleForDpi(34,192)==68);
    CHECK(PickerScaleForDpi(720,120)==900);
    CHECK(PickerScaleForDpi(720,144)==1080);
    CHECK(PickerScaleForDpi(720,192)==1440);
    CHECK(PickerScaleForDpi(720,0)==720);

    const SIZE one=PickerDesiredClientSize(
        1,240,150,16,58,38,34,720);
    const SIZE oneWithoutFooter=PickerDesiredClientSize(
        1,240,150,16,58,38,0,720);
    CHECK(one.cx==720);
    CHECK(one.cy==312);
    CHECK(one.cy-oneWithoutFooter.cy==34);
    const SIZE oneAt125=PickerDesiredClientSize(
        1,PickerScaleForDpi(240,120),PickerScaleForDpi(150,120),
        PickerScaleForDpi(16,120),PickerScaleForDpi(58,120),
        PickerScaleForDpi(38,120),PickerScaleForDpi(34,120),
        PickerScaleForDpi(720,120));
    CHECK(oneAt125.cx==900);
    CHECK(oneAt125.cy-PickerDesiredClientSize(
        1,300,188,20,73,48,0,900).cy==43);
    const SIZE oneAt150=PickerDesiredClientSize(
        1,PickerScaleForDpi(240,144),PickerScaleForDpi(150,144),
        PickerScaleForDpi(16,144),PickerScaleForDpi(58,144),
        PickerScaleForDpi(38,144),PickerScaleForDpi(34,144),
        PickerScaleForDpi(720,144));
    CHECK(oneAt150.cx==1080);
    CHECK(oneAt150.cy==468);
    const SIZE five=PickerDesiredClientSize(
        5,240,150,16,58,38,34,720);
    CHECK(five.cx==1296);
    CHECK(five.cy==312);

    const SIZE oneAt200=PickerDesiredClientSize(
        1,480,300,32,116,76,68,1440);
    CHECK(oneAt200.cx==1440 && oneAt200.cy==624);
    PickerFooterLayout footerAt200;
    CHECK(BuildPickerFooterLayout(
        oneAt200.cx,oneAt200.cy,32,68,44,
        360,720,200,footerAt200));
    CHECK(footerAt200.footer.top==556);
    CHECK(footerAt200.repoText.x==80 &&
          footerAt200.conusLink.right==1360);
}

static bool SamePickerFooterLayout(
        const PickerFooterLayout& left,
        const PickerFooterLayout& right) noexcept {
    return left.footer.left==right.footer.left &&
           left.footer.top==right.footer.top &&
           left.footer.right==right.footer.right &&
           left.footer.bottom==right.footer.bottom &&
           left.repoLink.left==right.repoLink.left &&
           left.repoLink.top==right.repoLink.top &&
           left.repoLink.right==right.repoLink.right &&
           left.repoLink.bottom==right.repoLink.bottom &&
           left.conusLink.left==right.conusLink.left &&
           left.conusLink.top==right.conusLink.top &&
           left.conusLink.right==right.conusLink.right &&
           left.conusLink.bottom==right.conusLink.bottom &&
           left.repoText.x==right.repoText.x &&
           left.repoText.y==right.repoText.y &&
           left.middleText.x==right.middleText.x &&
           left.middleText.y==right.middleText.y &&
           left.conusText.x==right.conusText.x &&
           left.conusText.y==right.conusText.y;
}

static void test_footer_geometry_hit_hover_cursor_and_open_result_seams(){
    PickerFooterLayout layout;
    CHECK(BuildPickerFooterLayout(
        720,312,16,34,22,180,360,100,layout));
    CHECK(layout.footer.left==0 && layout.footer.top==278 &&
          layout.footer.right==720 && layout.footer.bottom==312);
    CHECK(layout.repoText.x==40 && layout.repoText.y==278);
    CHECK(layout.middleText.x==220 && layout.middleText.y==278);
    CHECK(layout.conusText.x==580 && layout.conusText.y==278);
    CHECK(layout.repoLink.left==40 && layout.repoLink.top==278 &&
          layout.repoLink.right==220 && layout.repoLink.bottom==300);
    CHECK(layout.conusLink.left==580 && layout.conusLink.top==278 &&
          layout.conusLink.right==680 && layout.conusLink.bottom==300);
    CHECK(layout.repoLink.top>=layout.footer.top &&
          layout.repoLink.bottom<=layout.footer.bottom);
    CHECK(layout.conusLink.top>=layout.footer.top &&
          layout.conusLink.bottom<=layout.footer.bottom);

    PickerFooterLayout exactFit;
    CHECK(BuildPickerFooterLayout(
        672,312,16,34,22,180,360,100,exactFit));
    CHECK(exactFit.repoText.x==16);
    CHECK(exactFit.repoLink.right<=exactFit.conusLink.left);
    CHECK(exactFit.conusLink.right==656);
    CHECK(exactFit.conusLink.right<=exactFit.footer.right-16);
    PickerFooterLayout exactFitAt150;
    CHECK(BuildPickerFooterLayout(
        1008,468,24,51,33,270,540,150,exactFitAt150));
    CHECK(exactFitAt150.repoText.x==24);
    CHECK(exactFitAt150.middleText.x==294);
    CHECK(exactFitAt150.conusText.x==834);
    CHECK(exactFitAt150.conusLink.right==984);

    PickerFooterLayout rejected=layout;
    CHECK(!BuildPickerFooterLayout(
        671,312,16,34,22,180,360,100,rejected));
    CHECK(SamePickerFooterLayout(rejected,layout));
    CHECK(!BuildPickerFooterLayout(
        720,20,16,34,22,180,360,100,rejected));
    CHECK(!BuildPickerFooterLayout(
        720,312,16,34,35,180,360,100,rejected));
    CHECK(!BuildPickerFooterLayout(
        720,312,16,34,22,-1,360,100,rejected));

    CHECK(HitPickerFooterLink(layout,POINT{40,278})==
          PickerFooterLink::Repository);
    CHECK(HitPickerFooterLink(layout,POINT{219,299})==
          PickerFooterLink::Repository);
    CHECK(HitPickerFooterLink(layout,POINT{220,278})==
          PickerFooterLink::None);
    CHECK(HitPickerFooterLink(layout,POINT{580,278})==
          PickerFooterLink::ConusVision);
    CHECK(HitPickerFooterLink(layout,POINT{680,278})==
          PickerFooterLink::None);
    PickerFooterLayout defensiveOverlap=layout;
    defensiveOverlap.conusLink=defensiveOverlap.repoLink;
    CHECK(HitPickerFooterLink(defensiveOverlap,POINT{40,278})==
          PickerFooterLink::Repository);

    PickerState generation;
    generation.paintGeneration=17;
    CHECK(HitCurrentPickerFooterLink(
        generation,16,layout,POINT{40,278})==PickerFooterLink::None);
    CHECK(HitCurrentPickerFooterLink(
        generation,17,layout,POINT{40,278})==
          PickerFooterLink::Repository);

    PickerFooterLink hover=PickerFooterLink::None;
    CHECK(UpdatePickerFooterHover(
        hover,PickerFooterLink::Repository));
    CHECK(hover==PickerFooterLink::Repository);
    CHECK(!UpdatePickerFooterHover(
        hover,PickerFooterLink::Repository));
    CHECK(UpdatePickerFooterHover(
        hover,PickerFooterLink::ConusVision));
    CHECK(hover==PickerFooterLink::ConusVision);
    CHECK(UpdatePickerFooterHover(hover,PickerFooterLink::None));
    CHECK(!ResetPickerFooterHover(hover));
    hover=PickerFooterLink::ConusVision;
    CHECK(ResetPickerFooterHover(hover));
    CHECK(hover==PickerFooterLink::None);
    CHECK(!PickerFooterSuppressesRowHover(PickerFooterLink::None));
    CHECK(PickerFooterSuppressesRowHover(PickerFooterLink::Repository));
    CHECK(PickerFooterSuppressesRowHover(PickerFooterLink::ConusVision));
    CHECK(!PickerFooterUsesHandCursor(PickerFooterLink::None));
    CHECK(PickerFooterUsesHandCursor(PickerFooterLink::Repository));
    CHECK(PickerFooterUsesHandCursor(PickerFooterLink::ConusVision));

    CHECK(!PickerFooterOpenSucceeded(0));
    CHECK(!PickerFooterOpenSucceeded(2));
    CHECK(!PickerFooterOpenSucceeded(31));
    CHECK(!PickerFooterOpenSucceeded(32));
    CHECK(PickerFooterOpenSucceeded(33));
    CHECK(PickerFooterOpenSucceeded(4096));
}

static void test_footer_cache_and_activation_are_transactional(){
    PickerFooterPaintCache footer;
    footer.repo=L"old repo";
    footer.middle=L"old middle";
    footer.conus=L"old conus";
    footer.layout.repoLink={1,2,3,4};
    PickerFooterPaintCache staged;
    staged.repo=FooterRepoLabel();
    staged.middle=FooterMiddle();
    staged.conus=FooterConusLabel();
    CHECK(BuildPickerFooterLayout(
        720,312,16,34,22,180,360,100,staged.layout));
    footer.swap(staged);
    CHECK(footer.repo==FooterRepoLabel());
    CHECK(footer.middle==FooterMiddle());
    CHECK(footer.conus==FooterConusLabel());
    CHECK(footer.layout.repoLink.left==40);
    footer.clear();
    CHECK(footer.repo.empty() && footer.middle.empty() &&
          footer.conus.empty());
    CHECK(footer.layout.repoLink.left==0 &&
          footer.layout.repoLink.right==0);

    PickerFooterLink cacheHover=PickerFooterLink::Repository;
    bool tooltipActive=true;
    CHECK(RefreshPickerPaintCacheTransaction(footer,
        [&](PickerFooterPaintCache& next){
            next.repo=FooterRepoLabel();
            next.middle=FooterMiddle();
            next.conus=FooterConusLabel();
            return BuildPickerFooterLayout(
                720,312,16,34,22,180,360,100,next.layout);
        },
        [&]() noexcept {
            ResetPickerFooterHover(cacheHover);
            tooltipActive=false;
        },
        [&](PickerFooterPaintCache& published) noexcept {
            ResetPickerFooterHover(cacheHover);
            tooltipActive=false;
            published.clear();
        }));
    CHECK(cacheHover==PickerFooterLink::None && !tooltipActive);
    CHECK(footer.layout.repoLink.left==40);

    cacheHover=PickerFooterLink::ConusVision;
    tooltipActive=true;
    CHECK(!RefreshPickerPaintCacheTransaction(footer,
        [&](PickerFooterPaintCache& next){
            next.repo=L"partial";
            next.layout.repoLink={90,90,100,100};
            return false;
        },
        [&]() noexcept {
            ResetPickerFooterHover(cacheHover);
            tooltipActive=false;
        },
        [&](PickerFooterPaintCache& published) noexcept {
            ResetPickerFooterHover(cacheHover);
            tooltipActive=false;
            published.clear();
        }));
    CHECK(cacheHover==PickerFooterLink::None && !tooltipActive);
    CHECK(footer.repo.empty() && footer.layout.repoLink.left==0 &&
          footer.layout.repoLink.right==0);

    PickerState state;
    state.paintGeneration=91;
    PickerFooterLayout layout;
    CHECK(BuildPickerFooterLayout(
        720,312,16,34,22,180,360,100,layout));
    const PickerFooterActivation repo=ResolvePickerFooterActivation(
        state,91,layout,POINT{40,278});
    CHECK(repo.link==PickerFooterLink::Repository);
    CHECK(repo.url==FooterRepoUrl());
    CHECK(repo.consumed);
    int opens=0,notifications=0;
    const wchar_t* openedUrl=nullptr;
    CHECK(DispatchPickerFooterActivation(repo,
        [&](const wchar_t* url)->intptr_t {
            ++opens;
            openedUrl=url;
            return 33;
        },
        [&](){ ++notifications; }));
    CHECK(opens==1 && notifications==0 && openedUrl==FooterRepoUrl());

    const PickerFooterActivation conus=ResolvePickerFooterActivation(
        state,91,layout,POINT{580,278});
    CHECK(conus.link==PickerFooterLink::ConusVision);
    CHECK(conus.url==FooterConusUrl());
    CHECK(DispatchPickerFooterActivation(conus,
        [&](const wchar_t* url)->intptr_t {
            ++opens;
            openedUrl=url;
            return 32;
        },
        [&](){ ++notifications; }));
    CHECK(opens==2 && notifications==1 && openedUrl==FooterConusUrl());

    const PickerFooterActivation stale=ResolvePickerFooterActivation(
        state,90,layout,POINT{40,278});
    CHECK(!stale.consumed && stale.link==PickerFooterLink::None &&
          stale.url==nullptr);
    CHECK(!PickerFooterUsesHandCursor(HitCurrentPickerFooterLink(
        state,90,layout,POINT{40,278})));
    CHECK(!DispatchPickerFooterActivation(stale,
        [&](const wchar_t*)->intptr_t { ++opens; return 33; },
        [&](){ ++notifications; }));
    CHECK(opens==2 && notifications==1);
}

static void test_composite_picker_cache_cannot_omit_footer_state(){
    PickerPaintCacheState<int> cache;
    cache.hoverRows={1,2};
    cache.switchHeader=L"old switch";
    cache.moveHeader=L"old move";
    cache.footer.repo=L"old repo";
    cache.footer.layout.repoLink={1,2,3,4};
    cache.generation=7;
    cache.hintWidth=8;
    cache.clearButton={9,10,11,12};

    CHECK(RefreshPickerPaintCacheTransaction(cache,
        [&](PickerPaintCacheState<int>& staged){
            staged.hoverRows={3};
            staged.switchHeader=L"new switch";
            staged.moveHeader=L"new move";
            staged.footer.repo=FooterRepoLabel();
            staged.footer.middle=FooterMiddle();
            staged.footer.conus=FooterConusLabel();
            staged.generation=17;
            staged.hintWidth=18;
            staged.clearButton={19,20,21,22};
            return BuildPickerFooterLayout(
                720,312,16,34,22,180,360,100,
                staged.footer.layout);
        },[]() noexcept {},
        [](PickerPaintCacheState<int>& published) noexcept {
            published.clear();
        }));
    CHECK((cache.hoverRows==std::vector<int>{3}));
    CHECK(cache.switchHeader==L"new switch" &&
          cache.moveHeader==L"new move");
    CHECK(cache.footer.repo==FooterRepoLabel() &&
          cache.footer.layout.repoLink.left==40);
    CHECK(cache.generation==17 && cache.hintWidth==18 &&
          cache.clearButton.left==19 && cache.clearButton.bottom==22);

    CHECK(!RefreshPickerPaintCacheTransaction(cache,
        [&](PickerPaintCacheState<int>& staged){
            staged.footer.repo=L"partial";
            staged.footer.layout.repoLink={80,81,82,83};
            staged.generation=99;
            return false;
        },[]() noexcept {},
        [](PickerPaintCacheState<int>& published) noexcept {
            published.clear();
        }));
    CHECK(cache.hoverRows.empty() && cache.switchHeader.empty() &&
          cache.moveHeader.empty());
    CHECK(cache.footer.repo.empty() &&
          cache.footer.layout.repoLink.left==0 &&
          cache.footer.layout.repoLink.right==0);
    CHECK(cache.generation==0 && cache.hintWidth==0 &&
          cache.clearButton.left==0 && cache.clearButton.right==0);
}

static void test_footer_first_route_consumes_before_search_and_tiles(){
    PickerState state;
    state.paintGeneration=31;
    PickerFooterLayout layout;
    CHECK(BuildPickerFooterLayout(
        720,312,16,34,22,180,360,100,layout));
    const PickerFooterActivation repo=ResolvePickerFooterActivation(
        state,31,layout,POINT{40,278});

    for(intptr_t shellResult : {static_cast<intptr_t>(32),
                                static_cast<intptr_t>(33)}){
        const PickerPointerActivation route=ResolvePickerPointerActivation(
            repo,true,true,4);
        CHECK(route.target==PickerPointerTarget::Footer);
        int footerCalls=0,searchCalls=0,tileCalls=0,notifications=0;
        CHECK(DispatchPickerPointerActivation(route,
            [&](const PickerFooterActivation& activation){
                ++footerCalls;
                CHECK(DispatchPickerFooterActivation(activation,
                    [&](const wchar_t*)->intptr_t { return shellResult; },
                    [&](){ ++notifications; }));
            },
            [&](){ ++searchCalls; },
            [&](){ ++searchCalls; },
            [&](int){ ++tileCalls; }));
        CHECK(footerCalls==1 && searchCalls==0 && tileCalls==0);
        CHECK(notifications==(shellResult<=32?1:0));
    }

    PickerFooterActivation none;
    PickerPointerActivation clear=ResolvePickerPointerActivation(
        none,true,true,4);
    CHECK(clear.target==PickerPointerTarget::ClearSearch);
    PickerPointerActivation search=ResolvePickerPointerActivation(
        none,false,true,4);
    CHECK(search.target==PickerPointerTarget::Search);
    PickerPointerActivation tile=ResolvePickerPointerActivation(
        none,false,false,4);
    CHECK(tile.target==PickerPointerTarget::Tile && tile.tileIndex==4);
}

static void test_footer_hover_event_state_covers_every_reset_path(){
    PickerHoverEventState state;
    state.rowTooltipActive=true;
    int invalidations=0;
    if(UpdatePickerFooterHoverEvent(
            state,PickerFooterLink::Repository)) ++invalidations;
    CHECK(invalidations==1 && !state.rowTooltipActive &&
          state.footerLink==PickerFooterLink::Repository);
    state.rowTooltipActive=true;
    if(UpdatePickerFooterHoverEvent(
            state,PickerFooterLink::Repository)) ++invalidations;
    CHECK(invalidations==1 && !state.rowTooltipActive);
    if(UpdatePickerFooterHoverEvent(
            state,PickerFooterLink::ConusVision)) ++invalidations;
    if(UpdatePickerFooterHoverEvent(
            state,PickerFooterLink::None)) ++invalidations;
    CHECK(invalidations==3);

    const std::vector<PickerHoverResetReason> reasons={
        PickerHoverResetReason::CachePublication,
        PickerHoverResetReason::CacheFailure,
        PickerHoverResetReason::ExplicitInvalidation,
        PickerHoverResetReason::Hide,
        PickerHoverResetReason::MouseLeave
    };
    uint64_t expectedResetCount=0;
    for(PickerHoverResetReason reason : reasons){
        state.footerLink=PickerFooterLink::Repository;
        state.rowTooltipActive=true;
        ResetPickerHoverEventState(state,reason);
        ++expectedResetCount;
        CHECK(state.footerLink==PickerFooterLink::None);
        CHECK(!state.rowTooltipActive);
        CHECK(state.lastResetReason==reason);
        CHECK(state.resetCount==expectedResetCount);
    }
}

static void test_footer_mousemove_resets_tooltip_only_once_per_transition(){
    PickerHoverEventState state;
    int invalidations=0,tooltipResets=0;
    PickerFooterMouseMoveEffects effects=RoutePickerFooterMouseMove(
        state,PickerFooterLink::Repository,false);
    if(effects.invalidateFooter) ++invalidations;
    if(effects.resetRowTooltip) ++tooltipResets;
    CHECK(invalidations==1 && tooltipResets==1);

    for(int pixel=0;pixel<20;++pixel){
        effects=RoutePickerFooterMouseMove(
            state,PickerFooterLink::Repository,false);
        if(effects.invalidateFooter) ++invalidations;
        if(effects.resetRowTooltip) ++tooltipResets;
    }
    CHECK(invalidations==1 && tooltipResets==1);

    effects=RoutePickerFooterMouseMove(
        state,PickerFooterLink::ConusVision,false);
    if(effects.invalidateFooter) ++invalidations;
    if(effects.resetRowTooltip) ++tooltipResets;
    CHECK(invalidations==2 && tooltipResets==2);

    state.rowTooltipActive=true;
    effects=RoutePickerFooterMouseMove(
        state,PickerFooterLink::ConusVision,true);
    if(effects.invalidateFooter) ++invalidations;
    if(effects.resetRowTooltip) ++tooltipResets;
    CHECK(invalidations==2 && tooltipResets==3);
    effects=RoutePickerFooterMouseMove(
        state,PickerFooterLink::ConusVision,false);
    CHECK(!effects.invalidateFooter && !effects.resetRowTooltip);
}

static void test_picker_distinguishes_current_selected_and_active(){
    PickerState state;
    const GUID current=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID selected=G(L"{231A0000-0000-0000-0000-000000000002}");
    state.currentDesktop=current;
    CHECK(SetPickerSelection(state,4,selected));
    state.activeWindow=IK(10,20,30);

    CHECK(IsCurrentDesktop(state,current));
    CHECK(!IsCurrentDesktop(state,selected));
    CHECK(IsSelectedDesktop(state,selected));
    CHECK(!IsSelectedDesktop(state,current));
    CHECK(state.selectedIndex==4);
    CHECK(IsActiveWindow(state,IK(10,20,30)));
    CHECK(!IsActiveWindow(state,IK(10,21,30)));
    CHECK(!IsActiveWindow(state,IK(10,20,31)));
}

static void test_picker_zero_guids_and_partial_identities_never_highlight(){
    PickerState state;
    const GUID zero={0};
    const GUID valid=G(L"{231A0000-0000-0000-0000-000000000001}");
    CHECK(!IsCurrentDesktop(state,zero));
    CHECK(!IsCurrentDesktop(state,valid));
    CHECK(!IsSelectedDesktop(state,zero));
    CHECK(!IsSelectedDesktop(state,valid));
    CHECK(!IsActiveWindow(state,IK(0,0,0)));

    CHECK(SetPickerSelection(state,3,valid));
    CHECK(!SetPickerSelection(state,0,zero));
    CHECK(!SetPickerSelection(state,-1,valid));
    CHECK(GuidEq(state.selectedDesktop,valid));
    CHECK(state.selectedIndex==3);
}

static void test_picker_active_identity_rejects_hwnd_reuse(){
    PickerState state;
    state.activeWindow=IK(0x1234,77,9001);
    CHECK(IsActiveWindow(state,IK(0x1234,77,9001)));
    CHECK(!IsActiveWindow(state,IK(0x1234,77,9002)));
    CHECK(!IsActiveWindow(state,IK(0x1234,78,9001)));
    CHECK(!IsActiveWindow(state,IK(0x1235,77,9001)));
    CHECK(!IsActiveWindow(state,IK(0x1234,77,0)));
}

static void test_picker_selection_resolution_preserves_then_falls_back(){
    const GUID first=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID current=G(L"{231A0000-0000-0000-0000-000000000002}");
    const GUID selected=G(L"{231A0000-0000-0000-0000-000000000003}");
    const GUID missing=G(L"{231A0000-0000-0000-0000-000000000004}");
    const std::vector<GUID> desktops={first,selected,current};

    PickerState state;
    state.currentDesktop=current;
    state.selectedDesktop=selected;
    state.selectedIndex=99;
    CHECK(ResolvePickerSelection(state,desktops));
    CHECK(GuidEq(state.selectedDesktop,selected));
    CHECK(state.selectedIndex==1);

    state.selectedDesktop=missing;
    state.selectedIndex=99;
    CHECK(ResolvePickerSelection(state,desktops));
    CHECK(GuidEq(state.selectedDesktop,current));
    CHECK(state.selectedIndex==2);

    state.currentDesktop=GUID{};
    state.selectedDesktop=missing;
    state.selectedIndex=99;
    CHECK(ResolvePickerSelection(state,desktops));
    CHECK(GuidEq(state.selectedDesktop,first));
    CHECK(state.selectedIndex==0);

    CHECK(!ResolvePickerSelection(state,{}));
    CHECK(GuidIsZero(state.selectedDesktop));
    CHECK(state.selectedIndex==-1);
}

static void test_picker_selection_updates_pair_without_touching_current(){
    PickerState state;
    const GUID current=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID selected=G(L"{231A0000-0000-0000-0000-000000000002}");
    state.currentDesktop=current;
    CHECK(SetPickerSelection(state,7,selected));
    CHECK(GuidEq(state.currentDesktop,current));
    CHECK(GuidEq(state.selectedDesktop,selected));
    CHECK(state.selectedIndex==7);
}

static void test_picker_refresh_preserves_search_scroll_and_identity(){
    PickerState state;
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    state.searchText=L"github";
    state.searchActive=true;
    state.transition.phase=PickerPhase::TargetVerify;
    state.currentDesktop=desktop;
    CHECK(SetPickerSelection(state,5,desktop));
    state.activeWindow=IK(10,20,30);
    state.scrollByDesktop[GuidKey(desktop)]=3;

    PickerState refreshed=PreservePickerUi(state);
    CHECK(refreshed.searchText==L"github");
    CHECK(refreshed.searchActive);
    CHECK(refreshed.controlledTransition());
    CHECK(IsCurrentDesktop(refreshed,desktop));
    CHECK(IsSelectedDesktop(refreshed,desktop));
    CHECK(refreshed.selectedIndex==5);
    CHECK(IsActiveWindow(refreshed,IK(10,20,30)));
    CHECK(refreshed.scrollByDesktop.at(GuidKey(desktop))==3);

    refreshed.searchText=L"changed";
    refreshed.scrollByDesktop[GuidKey(desktop)]=9;
    CHECK(state.searchText==L"github");
    CHECK(state.scrollByDesktop.at(GuidKey(desktop))==3);
}

static void test_picker_refresh_transaction_publishes_only_complete_stage(){
    const GUID priorDesktop=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    const GUID nextDesktop=G(
        L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<int> model={1,2};
    PickerState state;
    state.currentDesktop=priorDesktop;
    CHECK(SetPickerSelection(state,1,priorDesktop));
    state.searchText=L"preserve";

    CHECK(!RunPickerRefreshTransaction(model,state,
        [&](std::vector<int>& staged,PickerState& next)->bool {
            staged.push_back(7);
            next.currentDesktop=nextDesktop;
            throw std::bad_alloc();
        }));
    CHECK((model==std::vector<int>{1,2}));
    CHECK(IsCurrentDesktop(state,priorDesktop));
    CHECK(IsSelectedDesktop(state,priorDesktop));
    CHECK(state.selectedIndex==1 && state.searchText==L"preserve");

    CHECK(!RunPickerRefreshTransaction(model,state,
        [&](std::vector<int>& staged,PickerState& next){
            staged.push_back(8);
            next.currentDesktop=nextDesktop;
            return false;
        }));
    CHECK((model==std::vector<int>{1,2}));
    CHECK(IsCurrentDesktop(state,priorDesktop));

    CHECK(RunPickerRefreshTransaction(model,state,
        [&](std::vector<int>& staged,PickerState& next){
            staged.push_back(9);
            next.currentDesktop=nextDesktop;
            CHECK(SetPickerSelection(next,0,nextDesktop));
            return true;
        }));
    CHECK((model==std::vector<int>{9}));
    CHECK(IsCurrentDesktop(state,nextDesktop));
    CHECK(IsSelectedDesktop(state,nextDesktop));
    CHECK(state.selectedIndex==0 && state.searchText==L"preserve");
}

static void test_blend_color_respects_channels_and_alpha_endpoints(){
    const COLORREF background=RGB(10,20,30);
    const COLORREF accent=RGB(110,120,130);
    CHECK(BlendColor(background,accent,0)==background);
    CHECK(BlendColor(background,accent,255)==accent);
    CHECK(BlendColor(background,accent,128)==RGB(60,70,80));
}

static void test_picker_dim_search_keeps_current_and_selection_distinct(){
    const COLORREF normal=RGB(28,28,33);
    const COLORREF active=RGB(242,150,5);
    const COLORREF current=BlendColor(normal,active,48);
    const COLORREF dim=RGB(22,22,26);
    const COLORREF passive=RGB(107,96,79);
    const COLORREF normalDim=PickerTileFill(normal,current,dim,false,true);
    const COLORREF currentDim=PickerTileFill(normal,current,dim,true,true);
    CHECK(PickerTileFill(normal,current,dim,false,false)==normal);
    CHECK(PickerTileFill(normal,current,dim,true,false)==current);
    CHECK(normalDim==BlendColor(normal,dim,160));
    CHECK(currentDim==BlendColor(current,dim,160));
    CHECK(currentDim!=normalDim);
    CHECK(PickerTileBorder(false,active,passive)==passive);
    CHECK(PickerTileBorder(true,active,passive)==active);
}

static void test_picker_visible_scroll_clamps_without_mutating_saved_value(){
    const int savedScroll=9;
    CHECK(PickerVisibleScroll(savedScroll,3)==3);
    CHECK(savedScroll==9);
    CHECK(PickerVisibleScroll(2,3)==2);
    CHECK(PickerVisibleScroll(-4,3)==0);
    CHECK(PickerVisibleScroll(2,-1)==0);
}

static void test_picker_wheel_scroll_saturates_at_integer_bounds(){
    CHECK(AdvancePickerScroll(0,3,120)==0);
    CHECK(AdvancePickerScroll(3,3,120)==2);
    CHECK(AdvancePickerScroll(2,3,-120)==3);
    CHECK(AdvancePickerScroll(3,3,-120)==3);
    CHECK(AdvancePickerScroll(99,3,120)==2);
    CHECK(AdvancePickerScroll(99,3,-120)==3);
    CHECK(AdvancePickerScroll(99,3,0)==3);
    CHECK(AdvancePickerScroll(2,3,0)==2);
    CHECK(AdvancePickerScroll(INT_MAX,0,-120)==0);
}

static void test_picker_target_failed_recapture_clears_entire_capture(){
    PickerTargetCaptureState capture;
    capture.hwnd=0x1234;
    capture.identity=IK(0x1234,77,9001);
    capture.title=L"target";
    CHECK(CompletePickerTargetRecapture(
        capture,WindowIdentityRecapture::Match));
    CHECK(capture.hwnd==0x1234 && capture.title==L"target");

    CHECK(!CompletePickerTargetRecapture(
        capture,WindowIdentityRecapture::Lost));
    CHECK(capture.hwnd==0);
    CHECK(!IsActiveWindow(PickerState{},capture.identity));
    CHECK(capture.title.empty());

    capture.hwnd=0x5678;
    capture.identity=IK(0x5678,88,9002);
    capture.title=L"other";
    CHECK(!CompletePickerTargetRecapture(
        capture,WindowIdentityRecapture::Indeterminate));
    CHECK(capture.hwnd==0 && capture.identity.hwnd==0 &&
          capture.title.empty());
}

static void test_picker_row_requires_complete_stable_identity(){
    const WindowIdentityKey complete=IK(0x1234,77,9001);
    CHECK(AcceptPickerRowIdentity(
        complete,WindowIdentityRecapture::Match));
    CHECK(!AcceptPickerRowIdentity(
        complete,WindowIdentityRecapture::Lost));
    CHECK(!AcceptPickerRowIdentity(
        complete,WindowIdentityRecapture::Indeterminate));
    CHECK(!AcceptPickerRowIdentity(
        IK(0x1234,77,0),WindowIdentityRecapture::Match));
}

static void test_picker_failed_current_read_clears_only_current(){
    PickerState state;
    const GUID current=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID selected=G(L"{231A0000-0000-0000-0000-000000000002}");
    state.currentDesktop=current;
    CHECK(SetPickerSelection(state,4,selected));
    CHECK(!SetPickerCurrentDesktop(state,GUID{}));
    CHECK(GuidIsZero(state.currentDesktop));
    CHECK(IsSelectedDesktop(state,selected));
    CHECK(state.selectedIndex==4);
    CHECK(SetPickerCurrentDesktop(state,current));
    CHECK(IsCurrentDesktop(state,current));
}

static void test_picker_desktop_snapshot_rejects_zero_and_duplicates(){
    const GUID first=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID second=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<GUID> snapshot;
    CHECK(AppendUniquePickerDesktop(snapshot,first));
    CHECK(!AppendUniquePickerDesktop(snapshot,GUID{}));
    CHECK(!AppendUniquePickerDesktop(snapshot,first));
    CHECK(snapshot.size()==1 && GuidEq(snapshot[0],first));
    CHECK(AppendUniquePickerDesktop(snapshot,second));
    CHECK(snapshot.size()==2 && GuidEq(snapshot[1],second));
}

static void test_picker_filter_cache_is_transactional_and_precomputed(){
    const std::vector<std::wstring> rows={L"github issue",L"mail",L"github pr"};
    std::vector<size_t> filtered={99};
    CHECK(BuildPickerFilteredIndices(rows,L"github",filtered,
        [](const std::wstring& value)->const std::wstring& { return value; }));
    CHECK((filtered==std::vector<size_t>{0,2}));

    const std::vector<size_t> prior=filtered;
    CHECK(!BuildPickerFilteredIndices(rows,L"github",filtered,
        [&](const std::wstring& value)->const std::wstring& {
            if(value==L"mail") throw std::bad_alloc();
            return value;
        }));
    CHECK(filtered==prior);
}

static void test_picker_bitmap_replacement_deselects_before_delete(){
    PickerBitmapSelection state;
    state.original=11;
    state.selected=11;
    uintptr_t release=0;
    CHECK(PublishPickerBitmapReplacement(state,22,11,release));
    CHECK(state.owned==22 && state.selected==22 && release==0);
    CHECK(PublishPickerBitmapReplacement(state,33,22,release));
    CHECK(state.owned==33 && state.selected==33 && release==22);

    const PickerBitmapSelection prior=state;
    release=77;
    CHECK(!PublishPickerBitmapReplacement(state,44,11,release));
    CHECK(state.owned==prior.owned && state.selected==prior.selected);
    CHECK(release==77);
}

static void test_gdi_buffer_resize_does_not_leak_selected_bitmaps(){
    HDC screen=GetDC(nullptr);
    if(!screen) return;
    {
        GdiBuffer warm;
        CHECK(warm.ensure(screen,64,64));
        warm.reset();
        warm.reset();
    }
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int cycle=0;cycle<4;++cycle){
        GdiBuffer buffer;
        for(int i=0;i<100;++i)
            CHECK(buffer.ensure(screen,200+i,120+i));
        if((cycle&1)==0){
            buffer.reset();
            buffer.reset();
        }
    }
    const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    ReleaseDC(nullptr,screen);
    CHECK(after<=before+1);
}

static void test_gdi_buffer_is_noncopyable(){
    CHECK(!std::is_copy_constructible<GdiBuffer>::value);
    CHECK(!std::is_copy_assignable<GdiBuffer>::value);
}

struct FakeGdiBufferState {
    uintptr_t dc=1;
    uintptr_t original=10;
    uintptr_t selected=10;
    uintptr_t nextBitmap=20;
    uintptr_t reportedPrevious=0;
    bool failCreateDc=false;
    bool failCurrentObject=false;
    bool failCreateBitmap=false;
    bool failSelect=false;
    int failSelectOnCall=0;
    bool failDeleteDc=false;
    bool failDeleteObject=false;
    bool deletedWhileSelected=false;
    bool duplicateDelete=false;
    int createDcCalls=0;
    int createBitmapCalls=0;
    int selectCalls=0;
    int deleteObjectCalls=0;
    int deleteDcCalls=0;
    std::set<uintptr_t> alive;
    std::vector<std::string> events;
};

static HDC FakeGdiCreateDc(void* context,HDC){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    ++state.createDcCalls;
    state.events.push_back("create-dc");
    return state.failCreateDc?nullptr:reinterpret_cast<HDC>(state.dc);
}

static HGDIOBJ FakeGdiGetCurrent(void* context,HDC,int){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    state.events.push_back("get-current");
    return state.failCurrentObject
        ?nullptr:reinterpret_cast<HGDIOBJ>(state.original);
}

static HBITMAP FakeGdiCreateBitmap(void* context,HDC,int,int){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    ++state.createBitmapCalls;
    state.events.push_back("create-bitmap");
    if(state.failCreateBitmap) return nullptr;
    const uintptr_t bitmap=state.nextBitmap++;
    state.alive.insert(bitmap);
    return reinterpret_cast<HBITMAP>(bitmap);
}

static HGDIOBJ FakeGdiSelect(void* context,HDC,HGDIOBJ object){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    ++state.selectCalls;
    state.events.push_back("select:"+
        std::to_string(reinterpret_cast<uintptr_t>(object)));
    if(state.failSelect ||
       (state.failSelectOnCall!=0 &&
        state.selectCalls==state.failSelectOnCall)){
        state.failSelect=false;
        return HGDI_ERROR;
    }
    const uintptr_t previous=state.selected;
    state.selected=reinterpret_cast<uintptr_t>(object);
    const uintptr_t reported=state.reportedPrevious;
    state.reportedPrevious=0;
    return reinterpret_cast<HGDIOBJ>(reported?reported:previous);
}

static BOOL FakeGdiDeleteObject(void* context,HGDIOBJ object){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    ++state.deleteObjectCalls;
    const uintptr_t value=reinterpret_cast<uintptr_t>(object);
    state.events.push_back("delete-object:"+std::to_string(value));
    if(state.selected==value) state.deletedWhileSelected=true;
    if(state.failDeleteObject) return FALSE;
    if(state.alive.erase(value)!=1) state.duplicateDelete=true;
    return TRUE;
}

static BOOL FakeGdiDeleteDc(void* context,HDC){
    FakeGdiBufferState& state=*static_cast<FakeGdiBufferState*>(context);
    ++state.deleteDcCalls;
    state.events.push_back("delete-dc");
    if(state.failDeleteDc) return FALSE;
    state.selected=0;
    return TRUE;
}

static GdiBufferOps FakeGdiOps(FakeGdiBufferState& state){
    GdiBufferOps ops;
    ops.context=&state;
    ops.createCompatibleDc=&FakeGdiCreateDc;
    ops.getCurrentObject=&FakeGdiGetCurrent;
    ops.createCompatibleBitmap=&FakeGdiCreateBitmap;
    ops.selectObject=&FakeGdiSelect;
    ops.deleteObject=&FakeGdiDeleteObject;
    ops.deleteDc=&FakeGdiDeleteDc;
    return ops;
}

static void test_gdi_buffer_fake_ownership_is_exact(){
    FakeGdiBufferState state;
    {
        GdiBuffer buffer(FakeGdiOps(state));
        const HDC reference=reinterpret_cast<HDC>(99);
        CHECK(buffer.ensure(reference,200,120));
        CHECK(buffer.ensure(reference,200,120));
        CHECK(state.createDcCalls==1 && state.createBitmapCalls==1);
        CHECK(buffer.ensure(reference,240,160));
        CHECK(state.selected==21);
        CHECK(state.deleteObjectCalls==1 && state.alive.count(20)==0);
        CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
        const std::vector<std::string> expected={
            "create-dc","get-current","create-bitmap","select:20",
            "create-bitmap","select:21","delete-object:20"};
        CHECK(state.events==expected);
        buffer.reset();
        CHECK(state.events.size()==10);
        CHECK(state.events[7]=="select:10");
        CHECK(state.events[8]=="delete-dc");
        CHECK(state.events[9]=="delete-object:21");
        const size_t resetEvents=state.events.size();
        buffer.reset();
        CHECK(state.events.size()==resetEvents);
    }
    CHECK(state.deleteObjectCalls==2 && state.deleteDcCalls==1);
    CHECK(state.alive.empty());
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
}

static void test_gdi_buffer_failures_are_atomic_and_cleanup_exact(){
    FakeGdiBufferState state;
    {
        GdiBuffer buffer(FakeGdiOps(state));
        const HDC reference=reinterpret_cast<HDC>(99);
        CHECK(buffer.ensure(reference,200,120));
        const HDC publishedDc=buffer.get();

        state.failCreateBitmap=true;
        CHECK(!buffer.ensure(reference,240,160));
        state.failCreateBitmap=false;
        CHECK(buffer.get()==publishedDc && state.selected==20);
        CHECK(state.deleteObjectCalls==0 && state.alive.count(20)==1);

        state.failSelect=true;
        CHECK(!buffer.ensure(reference,240,160));
        CHECK(buffer.get()==publishedDc && state.selected==20);
        CHECK(state.deleteObjectCalls==1 && state.alive.size()==1);
        CHECK(state.alive.count(20)==1 && !state.deletedWhileSelected);
    }
    CHECK(state.deleteObjectCalls==2 && state.deleteDcCalls==1);
    CHECK(state.alive.empty() && !state.duplicateDelete);

    FakeGdiBufferState noOriginal;
    noOriginal.failCurrentObject=true;
    {
        GdiBuffer buffer(FakeGdiOps(noOriginal));
        CHECK(!buffer.ensure(reinterpret_cast<HDC>(99),200,120));
        CHECK(buffer.get()==nullptr);
    }
    CHECK(noOriginal.createDcCalls==1);
    CHECK(noOriginal.createBitmapCalls==0);
    CHECK(noOriginal.deleteDcCalls==1);
    CHECK(noOriginal.deleteObjectCalls==0);

    FakeGdiBufferState noBitmap;
    noBitmap.failCreateBitmap=true;
    {
        GdiBuffer buffer(FakeGdiOps(noBitmap));
        CHECK(!buffer.ensure(reinterpret_cast<HDC>(99),200,120));
        CHECK(buffer.get()==nullptr);
    }
    CHECK(noBitmap.createDcCalls==1 && noBitmap.createBitmapCalls==1);
    CHECK(noBitmap.deleteDcCalls==1 && noBitmap.deleteObjectCalls==0);

    FakeGdiBufferState noSelection;
    noSelection.failSelect=true;
    {
        GdiBuffer buffer(FakeGdiOps(noSelection));
        CHECK(!buffer.ensure(reinterpret_cast<HDC>(99),200,120));
        CHECK(buffer.get()==nullptr);
    }
    CHECK(noSelection.createDcCalls==1 &&
          noSelection.createBitmapCalls==1);
    CHECK(noSelection.deleteDcCalls==1 &&
          noSelection.deleteObjectCalls==1);
    CHECK(noSelection.alive.empty());
}

static void test_gdi_buffer_failed_deselect_releases_dc_before_bitmap(){
    FakeGdiBufferState state;
    {
        GdiBuffer buffer(FakeGdiOps(state));
        CHECK(buffer.ensure(reinterpret_cast<HDC>(99),200,120));
        state.failSelect=true;
        buffer.reset();
        CHECK(state.events.size()==7);
        CHECK(state.events[4]=="select:10");
        CHECK(state.events[5]=="delete-dc");
        CHECK(state.events[6]=="delete-object:20");
        CHECK(!state.deletedWhileSelected);
    }
    CHECK(state.deleteDcCalls==1 && state.deleteObjectCalls==1);
    CHECK(state.alive.empty() && !state.duplicateDelete);
}

static void test_gdi_buffer_rejects_unexpected_previous_selection_atomically(){
    FakeGdiBufferState state;
    {
        GdiBuffer buffer(FakeGdiOps(state));
        const HDC reference=reinterpret_cast<HDC>(99);
        CHECK(buffer.ensure(reference,200,120));
        state.reportedPrevious=10;
        CHECK(!buffer.ensure(reference,240,160));
        CHECK(state.selected==20);
        CHECK(state.alive.size()==1 && state.alive.count(20)==1);
        CHECK(state.deleteObjectCalls==1);
        CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
        CHECK(buffer.ensure(reference,240,160));
        CHECK(state.selected==22);
        CHECK(state.alive.size()==1 && state.alive.count(22)==1);
    }
    CHECK(state.alive.empty());
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
}

static void test_gdi_buffer_retains_selected_bitmap_until_reset_can_retry(){
    FakeGdiBufferState state;
    GdiBuffer buffer(FakeGdiOps(state));
    CHECK(buffer.ensure(reinterpret_cast<HDC>(99),200,120));
    state.failSelect=true;
    state.failDeleteDc=true;
    buffer.reset();
    CHECK(buffer.get()!=nullptr);
    CHECK(state.deleteDcCalls==1 && state.deleteObjectCalls==0);
    CHECK(state.selected==20 && state.alive.count(20)==1);
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);

    state.failDeleteDc=false;
    buffer.reset();
    CHECK(buffer.get()==nullptr);
    CHECK(state.deleteDcCalls==2 && state.deleteObjectCalls==1);
    CHECK(state.alive.empty());
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
}

static void test_gdi_buffer_invalid_sizes_never_allocate(){
    FakeGdiBufferState state;
    GdiBuffer buffer(FakeGdiOps(state));
    const HDC reference=reinterpret_cast<HDC>(99);
    CHECK(!buffer.ensure(nullptr,200,120));
    CHECK(!buffer.ensure(reference,0,120));
    CHECK(!buffer.ensure(reference,-1,120));
    CHECK(!buffer.ensure(reference,200,0));
    CHECK(!buffer.ensure(reference,200,-1));
    CHECK(state.createDcCalls==0 && state.createBitmapCalls==0);
    CHECK(state.selectCalls==0 && state.deleteObjectCalls==0);
    CHECK(state.deleteDcCalls==0);
}

static void test_gdi_buffer_catastrophic_rollback_retains_both_bitmaps(){
    FakeGdiBufferState state;
    GdiBuffer buffer(FakeGdiOps(state));
    const HDC reference=reinterpret_cast<HDC>(99);
    CHECK(buffer.ensure(reference,200,120));
    state.reportedPrevious=10;
    state.failSelectOnCall=3;
    state.failDeleteDc=true;
    CHECK(!buffer.ensure(reference,240,160));
    CHECK(buffer.get()!=nullptr);
    CHECK(state.selected==21);
    CHECK(state.alive.size()==2 && state.alive.count(20)==1 &&
          state.alive.count(21)==1);
    CHECK(state.deleteObjectCalls==0 && state.deleteDcCalls==1);
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);

    state.failSelectOnCall=0;
    state.failDeleteDc=false;
    buffer.reset();
    CHECK(buffer.get()==nullptr);
    CHECK(state.deleteDcCalls==2 && state.deleteObjectCalls==2);
    CHECK(state.alive.empty());
    CHECK(!state.deletedWhileSelected && !state.duplicateDelete);
}

static void test_gdi_buffer_retains_dc_when_original_read_cleanup_fails(){
    FakeGdiBufferState state;
    state.failCurrentObject=true;
    state.failDeleteDc=true;
    GdiBuffer buffer(FakeGdiOps(state));
    CHECK(!buffer.ensure(reinterpret_cast<HDC>(99),200,120));
    CHECK(buffer.get()!=nullptr);
    CHECK(state.createDcCalls==1 && state.deleteDcCalls==1);
    CHECK(state.createBitmapCalls==0 && state.deleteObjectCalls==0);

    state.failDeleteDc=false;
    buffer.reset();
    CHECK(buffer.get()==nullptr && state.deleteDcCalls==2);
}

static void test_gdi_buffer_retains_every_failed_bitmap_delete_for_reset(){
    const HDC reference=reinterpret_cast<HDC>(99);

    FakeGdiBufferState rejectedCandidate;
    {
        GdiBuffer buffer(FakeGdiOps(rejectedCandidate));
        rejectedCandidate.failSelect=true;
        rejectedCandidate.failDeleteObject=true;
        CHECK(!buffer.ensure(reference,200,120));
        CHECK(rejectedCandidate.alive.size()==1);
        const int creates=rejectedCandidate.createDcCalls;
        CHECK(!buffer.ensure(reference,200,120));
        CHECK(rejectedCandidate.createDcCalls==creates);
        rejectedCandidate.failDeleteObject=false;
        buffer.reset();
        CHECK(rejectedCandidate.alive.empty());
    }
    CHECK(!rejectedCandidate.duplicateDelete &&
          !rejectedCandidate.deletedWhileSelected);

    FakeGdiBufferState rolledBackCandidate;
    {
        GdiBuffer buffer(FakeGdiOps(rolledBackCandidate));
        CHECK(buffer.ensure(reference,200,120));
        rolledBackCandidate.reportedPrevious=10;
        rolledBackCandidate.failDeleteObject=true;
        CHECK(!buffer.ensure(reference,240,160));
        CHECK(rolledBackCandidate.selected==20);
        CHECK(rolledBackCandidate.alive.size()==2);
        rolledBackCandidate.failDeleteObject=false;
        buffer.reset();
        CHECK(rolledBackCandidate.alive.empty());
    }
    CHECK(!rolledBackCandidate.duplicateDelete &&
          !rolledBackCandidate.deletedWhileSelected);

    FakeGdiBufferState retiredOld;
    {
        GdiBuffer buffer(FakeGdiOps(retiredOld));
        CHECK(buffer.ensure(reference,200,120));
        retiredOld.failDeleteObject=true;
        CHECK(buffer.ensure(reference,240,160));
        CHECK(retiredOld.selected==21 && retiredOld.alive.size()==2);
        const int creates=retiredOld.createBitmapCalls;
        CHECK(buffer.ensure(reference,240,160));
        CHECK(retiredOld.createBitmapCalls==creates);
        CHECK(!buffer.ensure(reference,260,180));
        CHECK(retiredOld.createBitmapCalls==creates);
        retiredOld.failDeleteObject=false;
        CHECK(buffer.ensure(reference,240,160));
        CHECK(retiredOld.alive.size()==1 && retiredOld.alive.count(21)==1);
        buffer.reset();
        CHECK(retiredOld.alive.empty());
    }
    CHECK(!retiredOld.duplicateDelete && !retiredOld.deletedWhileSelected);
}

struct FakeIconCacheState {
    uintptr_t nextOwned=1000;
    bool failCopy=false;
    bool throwCopy=false;
    bool throwDestroy=false;
    bool failDestroy=false;
    bool duplicateDestroy=false;
    int copyCalls=0;
    std::set<uintptr_t> alive;
    std::vector<uintptr_t> destroyed;
    std::vector<std::string> events;
};

static IconCacheOps FakeIconOps(FakeIconCacheState& state){
    IconCacheOps ops;
    ops.copy=[&](HICON borrowed)->HICON {
        ++state.copyCalls;
        state.events.push_back("copy:"+std::to_string(
            reinterpret_cast<uintptr_t>(borrowed)));
        if(state.throwCopy) throw std::runtime_error("copy failure");
        if(state.failCopy) return nullptr;
        const uintptr_t owned=state.nextOwned++;
        state.alive.insert(owned);
        return reinterpret_cast<HICON>(owned);
    };
    ops.destroy=[&](HICON icon)->bool {
        const uintptr_t owned=reinterpret_cast<uintptr_t>(icon);
        state.events.push_back("destroy:"+std::to_string(owned));
        if(state.throwDestroy) throw std::runtime_error("destroy failure");
        if(state.failDestroy) return false;
        state.destroyed.push_back(owned);
        if(state.alive.erase(owned)!=1) state.duplicateDestroy=true;
        return true;
    };
    return ops;
}

static void test_icon_cache_replacement_prune_clear_and_destructor_are_exact(){
    FakeIconCacheState state;
    {
        OwnedIconCache cache(3,FakeIconOps(state));
        HICON first=cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(11),1);
        HICON second=cache.insertBorrowed(
            "b",reinterpret_cast<HICON>(12),2);
        CHECK(first==reinterpret_cast<HICON>(1000));
        CHECK(second==reinterpret_cast<HICON>(1001));
        CHECK(cache.size()==2 && cache.peek("a")==first);
        CHECK(cache.getAndTouch("a",9)==first);

        HICON replacement=cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(13),10);
        CHECK(replacement==reinterpret_cast<HICON>(1002));
        CHECK(cache.peek("a")==replacement && cache.size()==2);
        CHECK(state.events[state.events.size()-2]=="copy:13");
        CHECK(state.events.back()=="destroy:1000");

        state.failCopy=true;
        CHECK(cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(14),11)==nullptr);
        state.failCopy=false;
        CHECK(cache.peek("a")==replacement && cache.size()==2);
        state.throwCopy=true;
        CHECK(cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(15),12)==nullptr);
        state.throwCopy=false;
        CHECK(cache.peek("a")==replacement && cache.size()==2);
        const int copiesBeforeNull=state.copyCalls;
        CHECK(cache.insertBorrowed("null",nullptr,13)==nullptr);
        CHECK(state.copyCalls==copiesBeforeNull && cache.size()==2);

        cache.pruneTo({"a"});
        CHECK(cache.size()==1 && cache.peek("b")==nullptr);
        CHECK(state.alive.size()==1 && state.alive.count(1002)==1);
        cache.clear();
        CHECK(cache.size()==0 && state.alive.empty());
        const size_t destroyed=state.destroyed.size();
        cache.clear();
        CHECK(state.destroyed.size()==destroyed);
    }
    CHECK(state.copyCalls==5);
    CHECK(state.destroyed.size()==3);
    CHECK(state.alive.empty() && !state.duplicateDestroy);
}

static void test_icon_cache_enforces_touch_lru_immediately(){
    FakeIconCacheState state;
    {
        OwnedIconCache cache(2,FakeIconOps(state));
        HICON a=cache.insertBorrowed("a",reinterpret_cast<HICON>(1),10);
        HICON b=cache.insertBorrowed("b",reinterpret_cast<HICON>(2),20);
        CHECK(a && b && cache.size()==2);
        CHECK(cache.getAndTouch("a",30)==a);
        HICON c=cache.insertBorrowed("c",reinterpret_cast<HICON>(3),40);
        CHECK(c && cache.size()==2);
        CHECK(cache.peek("a")==a && cache.peek("b")==nullptr);
        CHECK(cache.peek("c")==c);
        CHECK(state.destroyed.size()==1 && state.destroyed[0]==1001);
    }
    CHECK(state.alive.empty() && state.destroyed.size()==3);
    CHECK(!state.duplicateDestroy);

    FakeIconCacheState selfEvicted;
    {
        OwnedIconCache cache(1,FakeIconOps(selfEvicted));
        HICON retained=cache.insertBorrowed(
            "newer",reinterpret_cast<HICON>(4),100);
        CHECK(retained!=nullptr);
        CHECK(cache.insertBorrowed(
            "older",reinterpret_cast<HICON>(5),1)==nullptr);
        CHECK(cache.size()==1 && cache.peek("newer")==retained);
        CHECK(cache.peek("older")==nullptr);
    }
    CHECK(selfEvicted.alive.empty() && !selfEvicted.duplicateDestroy);

    FakeIconCacheState peekOnly;
    {
        OwnedIconCache cache(2,FakeIconOps(peekOnly));
        HICON a=cache.insertBorrowed("a",reinterpret_cast<HICON>(1),10);
        cache.insertBorrowed("b",reinterpret_cast<HICON>(2),20);
        CHECK(cache.peek("a")==a);
        cache.insertBorrowed("c",reinterpret_cast<HICON>(3),30);
        CHECK(cache.peek("a")==nullptr);
    }
    CHECK(peekOnly.alive.empty() && !peekOnly.duplicateDestroy);

    FakeIconCacheState tied;
    {
        OwnedIconCache cache(2,FakeIconOps(tied));
        cache.insertBorrowed("b",reinterpret_cast<HICON>(1),10);
        cache.insertBorrowed("a",reinterpret_cast<HICON>(2),10);
        cache.insertBorrowed("c",reinterpret_cast<HICON>(3),20);
        CHECK(cache.peek("a")==nullptr);
        CHECK(cache.peek("b")!=nullptr && cache.peek("c")!=nullptr);
    }
    CHECK(tied.alive.empty() && !tied.duplicateDestroy);
}

static void test_icon_cache_rejects_zero_limit_and_missing_ops(){
    FakeIconCacheState zeroState;
    {
        OwnedIconCache zero(0,FakeIconOps(zeroState));
        CHECK(zero.insertBorrowed(
            "a",reinterpret_cast<HICON>(1),1)==nullptr);
        CHECK(zero.size()==0 && zeroState.copyCalls==0);
    }
    CHECK(zeroState.destroyed.empty());

    FakeIconCacheState missingCopyState;
    IconCacheOps missingCopy=FakeIconOps(missingCopyState);
    missingCopy.copy={};
    OwnedIconCache withoutCopy(2,std::move(missingCopy));
    CHECK(withoutCopy.insertBorrowed(
        "a",reinterpret_cast<HICON>(1),1)==nullptr);
    CHECK(withoutCopy.size()==0 && missingCopyState.copyCalls==0);

    FakeIconCacheState missingDestroyState;
    IconCacheOps missingDestroy=FakeIconOps(missingDestroyState);
    missingDestroy.destroy={};
    OwnedIconCache withoutDestroy(2,std::move(missingDestroy));
    CHECK(withoutDestroy.insertBorrowed(
        "a",reinterpret_cast<HICON>(1),1)==nullptr);
    CHECK(withoutDestroy.size()==0 && missingDestroyState.copyCalls==0);
}

static void test_icon_cache_callback_exceptions_do_not_escape(){
    FakeIconCacheState state;
    {
        OwnedIconCache cache(2,FakeIconOps(state));
        CHECK(cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(1),1)!=nullptr);
        state.throwDestroy=true;
        CHECK(!cache.clear());
        CHECK(cache.size()==1 && state.alive.size()==1);
        state.throwDestroy=false;
        CHECK(cache.clear());
        CHECK(cache.size()==0 && state.alive.empty());
    }
    CHECK(state.destroyed.size()==1 && !state.duplicateDestroy);
}

static void test_icon_cache_destroy_failure_retains_bounded_ownership(){
    FakeIconCacheState replacement;
    {
        OwnedIconCache cache(1,FakeIconOps(replacement));
        HICON original=cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(1),10);
        replacement.failDestroy=true;
        CHECK(cache.insertBorrowed(
            "a",reinterpret_cast<HICON>(2),20)==nullptr);
        CHECK(cache.peek("a")==original && cache.size()==1);
        CHECK(replacement.alive.size()==2);
        const int copies=replacement.copyCalls;
        CHECK(cache.insertBorrowed(
            "b",reinterpret_cast<HICON>(3),30)==nullptr);
        CHECK(replacement.copyCalls==copies);
        replacement.failDestroy=false;
        CHECK(cache.clear());
        CHECK(cache.size()==0 && replacement.alive.empty());
    }
    CHECK(!replacement.duplicateDestroy);

    FakeIconCacheState pruning;
    {
        OwnedIconCache cache(2,FakeIconOps(pruning));
        cache.insertBorrowed("a",reinterpret_cast<HICON>(1),1);
        cache.insertBorrowed("b",reinterpret_cast<HICON>(2),2);
        pruning.failDestroy=true;
        CHECK(!cache.pruneTo({"b"}));
        CHECK(cache.size()==2 && cache.peek("a")!=nullptr);
        pruning.failDestroy=false;
        CHECK(cache.pruneTo({"b"}));
        CHECK(cache.size()==1 && cache.peek("a")==nullptr);
        CHECK(cache.clear());
    }
    CHECK(pruning.alive.empty() && !pruning.duplicateDestroy);
}

static void test_icon_cache_caps_257_live_keys_at_256_immediately(){
    FakeIconCacheState state;
    {
        OwnedIconCache cache(256,FakeIconOps(state));
        for(int index=0;index<257;++index){
            const std::string key="window-"+std::to_string(index);
            cache.insertBorrowed(key,
                reinterpret_cast<HICON>(static_cast<uintptr_t>(index+1)),
                static_cast<uint64_t>(index+1));
            CHECK(cache.size()<=256);
        }
        CHECK(cache.size()==256);
        CHECK(cache.peek("window-0")==nullptr);
        CHECK(state.destroyed.size()==1);
    }
    CHECK(state.copyCalls==257);
    CHECK(state.destroyed.size()==257);
    CHECK(state.alive.empty() && !state.duplicateDestroy);
}

static void test_icon_preload_gate_caps_work_and_skips_idle_refresh(){
    IconPreloadGate gate;
    gate.markDirty();
    const size_t available=256;
    size_t loaderCalls=0;
    size_t turns=0;
    IconPreloadTurn turn;
    do {
        turn=gate.runTurn(4,[&](size_t cursor){
            if(cursor>=available) return IconPreloadStep::Exhausted;
            ++loaderCalls;
            return IconPreloadStep::Miss;
        });
        ++turns;
        CHECK(turn.misses<=4);
    } while(!turn.complete && turns<100);
    CHECK(turn.complete && turns==65 && loaderCalls==256);

    const IconPreloadTurn idle=gate.runTurn(4,[&](size_t){
        ++loaderCalls;
        return IconPreloadStep::Miss;
    });
    CHECK(idle.complete && idle.misses==0 && loaderCalls==256);

    gate.markDirty();
    const IconPreloadTurn cached=gate.runTurn(4,[&](size_t cursor){
        if(cursor>=104) return IconPreloadStep::Exhausted;
        if(cursor<100) return IconPreloadStep::Cached;
        ++loaderCalls;
        return IconPreloadStep::Miss;
    });
    CHECK(!cached.complete && cached.visited==104 && cached.misses==4);
    gate.cancel();
    CHECK(!gate.dirty());
    const IconPreloadTurn hidden=gate.runTurn(4,[&](size_t){
        ++loaderCalls;
        return IconPreloadStep::Miss;
    });
    CHECK(hidden.complete && hidden.misses==0 && loaderCalls==260);
}

static void test_picker_scroll_clamp_reaches_rows_beyond_icon_budget(){
    std::vector<int> scrolls(100,999);
    std::vector<int> maxima(100,3);
    maxima.back()=1;
    for(size_t index=0;index<scrolls.size();++index)
        scrolls[index]=PickerVisibleScroll(scrolls[index],maxima[index]);
    CHECK(scrolls[63]==3);
    CHECK(scrolls.back()==1);
}

static void test_ordered_teardown_retries_without_destroying_dependencies(){
    OrderedTeardownGate gate;
    int windows=0,classes=0,resources=0;
    bool windowsReady=false,classesReady=false,resourcesReady=false;
    auto run=[&](){
        return gate.run(
            [&](){ ++windows; return windowsReady; },
            [&](){ ++classes; return classesReady; },
            [&](){ ++resources; return resourcesReady; });
    };

    CHECK(!run());
    CHECK(windows==1 && classes==0 && resources==0 && !gate.complete());
    windowsReady=true;
    CHECK(!run());
    CHECK(windows==2 && classes==1 && resources==0 && !gate.complete());
    classesReady=true;
    CHECK(!run());
    CHECK(windows==2 && classes==2 && resources==1 && !gate.complete());
    resourcesReady=true;
    CHECK(run() && gate.complete());
    CHECK(windows==2 && classes==2 && resources==2);
    CHECK(run());
    CHECK(windows==2 && classes==2 && resources==2);

    gate.reset();
    CHECK(!gate.complete());
}

static void test_fixed_icon_retirement_retains_failed_release_without_allocating(){
    FixedIconRetirement<3> retired;
    CHECK(retired.retain(reinterpret_cast<HICON>(1)));
    CHECK(retired.retain(reinterpret_cast<HICON>(2)));
    CHECK(retired.retain(reinterpret_cast<HICON>(3)));
    CHECK(!retired.retain(reinterpret_cast<HICON>(4)));
    CHECK(retired.size()==3);

    std::set<uintptr_t> alive={1,2,3};
    bool failSecond=true;
    CHECK(!retired.clear([&](HICON icon)->bool {
        const uintptr_t value=reinterpret_cast<uintptr_t>(icon);
        if(value==2 && failSecond) return false;
        return alive.erase(value)==1;
    }));
    CHECK(retired.size()==1 && alive.size()==1 && alive.count(2)==1);
    failSecond=false;
    CHECK(retired.clear([&](HICON icon)->bool {
        return alive.erase(reinterpret_cast<uintptr_t>(icon))==1;
    }));
    CHECK(retired.size()==0 && alive.empty());
    CHECK(retired.clear([](HICON)->bool { return false; }));

    CHECK(retired.retain(reinterpret_cast<HICON>(5)));
    CHECK(!retired.clear([](HICON)->bool {
        throw std::runtime_error("release failure");
    }));
    CHECK(retired.size()==1);
}

static void test_picker_valid_current_commits_only_with_model(){
    const GUID prior=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID observed=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<int> model={1,2};
    PickerState state;
    state.currentDesktop=prior;

    CHECK(!RunPickerRefreshWithCurrent(model,state,observed,
        [](std::vector<int>& staged,PickerState&)->bool {
            staged.push_back(9);
            return false;
        }));
    CHECK((model==std::vector<int>{1,2}));
    CHECK(IsCurrentDesktop(state,prior));

    CHECK(RunPickerRefreshWithCurrent(model,state,observed,
        [](std::vector<int>& staged,PickerState&){
            staged.push_back(7);
            return true;
        }));
    CHECK((model==std::vector<int>{7}));
    CHECK(IsCurrentDesktop(state,observed));

    CHECK(!RunPickerRefreshWithCurrent(model,state,GUID{},
        [](std::vector<int>& staged,PickerState&)->bool {
            staged.push_back(8);
            return false;
        }));
    CHECK((model==std::vector<int>{7}));
    CHECK(GuidIsZero(state.currentDesktop));
}

static void test_picker_paint_cache_failure_blocks_show_transactionally(){
    std::vector<int> cache={1,2};
    CHECK(!RunPickerPaintCacheTransaction(cache,
        [](std::vector<int>& staged){
            staged.push_back(9);
            return false;
        }));
    CHECK((cache==std::vector<int>{1,2}));
    CHECK(!PickerShowPreparationComplete(true,false));
    CHECK(!PickerShowPreparationComplete(false,true));

    CHECK(RunPickerPaintCacheTransaction(cache,
        [](std::vector<int>& staged){
            staged.push_back(7);
            return true;
        }));
    CHECK((cache==std::vector<int>{7}));
    CHECK(PickerShowPreparationComplete(true,true));
}

struct FakePickerComOutput {
    explicit FakePickerComOutput(int& releases):releases_(releases){}
    void Release(){ ++releases_; delete this; }
private:
    int& releases_;
};

static void test_picker_com_output_is_owned_even_on_failed_call(){
    int releases=0;
    {
        PickerScopedComOutput<FakePickerComOutput> output;
        *output.put()=new FakePickerComOutput(releases);
        const HRESULT simulatedResult=E_FAIL;
        CHECK(FAILED(simulatedResult));
        CHECK(static_cast<bool>(output));
    }
    CHECK(releases==1);
}

struct PickerTestPaintCache {
    int value=0;
    uint64_t generation=0;
    int* clock=nullptr;
    int* swapOrder=nullptr;
    void swap(PickerTestPaintCache& other) noexcept {
        if(clock && swapOrder) *swapOrder=++*clock;
        const int priorValue=value;
        value=other.value;
        other.value=priorValue;
        const uint64_t priorGeneration=generation;
        generation=other.generation;
        other.generation=priorGeneration;
    }
    void clear() noexcept {
        value=0;
        generation=0;
    }
};

static void test_picker_cache_publication_resets_hover_and_invalidates_failures(){
    PickerState state;
    const uint64_t firstGeneration=BeginPickerPaintRefresh(state);
    CHECK(firstGeneration!=0);
    CHECK(state.paintGeneration==firstGeneration);

    int clock=0,beforePublish=0,swapOrder=0,failures=0;
    PickerTestPaintCache cache;
    cache.value=1;
    cache.generation=firstGeneration;
    cache.clock=&clock;
    cache.swapOrder=&swapOrder;
    const uint64_t nextGeneration=BeginPickerPaintRefresh(state);
    CHECK(!PickerPaintCacheMatches(state,cache.generation));
    CHECK(RefreshPickerPaintCacheTransaction(cache,
        [&](PickerTestPaintCache& staged){
            staged.value=2;
            staged.generation=nextGeneration;
            return true;
        },
        [&]() noexcept { beforePublish=++clock; },
        [&](PickerTestPaintCache&) noexcept { ++failures; }));
    CHECK(beforePublish==1 && swapOrder==2 && failures==0);
    CHECK(cache.value==2 && cache.generation==nextGeneration);
    CHECK(PickerPaintCacheMatches(state,cache.generation));
    CHECK(PickerHoverPairMatches(4,nextGeneration,4,cache.generation));
    CHECK(!PickerHoverPairMatches(4,firstGeneration,4,cache.generation));

    state.paintGeneration=(std::numeric_limits<uint64_t>::max)();
    CHECK(BeginPickerPaintRefresh(state)==1);

    for(int callerCategory=0;callerCategory<5;++callerCategory){
        cache.value=10+callerCategory;
        cache.generation=BeginPickerPaintRefresh(state);
        failures=0;
        CHECK(!RefreshPickerPaintCacheTransaction(cache,
            [](PickerTestPaintCache& staged){
                staged.value=99;
                return false;
            },
            []() noexcept {},
            [&](PickerTestPaintCache& published) noexcept {
                published.value=0;
                published.generation=0;
                ++failures;
            }));
        CHECK(cache.value==0 && cache.generation==0 && failures==1);
        CHECK(!PickerPaintCacheMatches(state,cache.generation));
    }

    cache.value=77;
    cache.generation=BeginPickerPaintRefresh(state);
    failures=0;
    CHECK(!RefreshPickerPaintCacheTransaction(cache,
        [](PickerTestPaintCache&)->bool { throw std::bad_alloc(); },
        []() noexcept {},
        [&](PickerTestPaintCache& published) noexcept {
            published.value=0;
            published.generation=0;
            ++failures;
        }));
    CHECK(cache.value==0 && cache.generation==0 && failures==1);
}

static void test_picker_commit_requires_exact_active_identity(){
    const WindowIdentityKey active=IK(0x1234,77,9001);
    CHECK(PickerTargetMatchesActive(0x1234,active));
    CHECK(!PickerTargetMatchesActive(0x1235,active));
    CHECK(!PickerTargetMatchesActive(0,active));
    CHECK(PickerCommitIdentityAllowed(
        0x1234,active,active,WindowIdentityRecapture::Match));
    CHECK(!PickerCommitIdentityAllowed(
        0x1235,active,active,WindowIdentityRecapture::Match));
    CHECK(!PickerCommitIdentityAllowed(
        0x1234,active,IK(0x1234,78,9001),
        WindowIdentityRecapture::Match));
    CHECK(!PickerCommitIdentityAllowed(
        0x1234,active,IK(0x1234,77,9002),
        WindowIdentityRecapture::Match));
    CHECK(!PickerCommitIdentityAllowed(
        0x1234,active,active,WindowIdentityRecapture::Lost));
    CHECK(!PickerCommitIdentityAllowed(
        0x1234,active,active,WindowIdentityRecapture::Indeterminate));
}

static void test_picker_model_publish_invalidates_cache_before_reentry(){
    PickerState state;
    state.paintGeneration=41;
    std::vector<int> model={1};
    CHECK(RunPickerRefreshTransaction(model,state,
        [](std::vector<int>& staged,PickerState& next){
            staged.push_back(2);
            next.searchText=L"published";
            return true;
        }));
    PickerTestPaintCache cache;
    cache.value=7;
    cache.generation=41;
    CHECK(PickerPaintCacheMatches(state,cache.generation));

    bool reentrantAcceptedOldCache=true;
    InvalidatePickerPaintCacheState(state,cache,[&]() noexcept {
        reentrantAcceptedOldCache=
            PickerPaintCacheMatches(state,cache.generation);
    });
    CHECK(!reentrantAcceptedOldCache);
    CHECK(cache.value==0 && cache.generation==0);
    CHECK(state.paintGeneration==42);
}

static void test_picker_volatile_rows_skip_but_structural_failures_abort(){
    PickerState state;
    std::vector<int> model={9};
    const std::vector<PickerRowReadResult> volatileSequence={
        PickerRowReadResult::Success,
        PickerRowReadResult::IdentityUnavailable,
        PickerRowReadResult::DesktopUnavailable,
        PickerRowReadResult::TitleUnavailable,
        PickerRowReadResult::IdentityChanged,
        PickerRowReadResult::Success
    };
    CHECK(RunPickerRefreshTransaction(model,state,
        [&](std::vector<int>& staged,PickerState&){
            bool modelFailed=false;
            int row=0;
            for(PickerRowReadResult result : volatileSequence){
                if(result==PickerRowReadResult::Success)
                    staged.push_back(++row);
                else if(!ContinuePickerRowEnumeration(
                            result,modelFailed)) return false;
            }
            return !modelFailed;
        }));
    CHECK((model==std::vector<int>{1,2}));

    for(PickerRowReadResult fatal : {
            PickerRowReadResult::AllocationFailure,
            PickerRowReadResult::GlobalSnapshotFailure}){
        CHECK(!RunPickerRefreshTransaction(model,state,
            [&](std::vector<int>& staged,PickerState&){
                bool modelFailed=false;
                staged.push_back(7);
                return ContinuePickerRowEnumeration(fatal,modelFailed);
            }));
        CHECK((model==std::vector<int>{1,2}));
    }
}

static void test_picker_async_search_joins_by_full_identity(){
    const WindowIdentityKey row=IK(0x1234,77,9001);
    CHECK(PickerSearchResultMatches(row,row));
    CHECK(!PickerSearchResultMatches(row,IK(0x1235,77,9001)));
    CHECK(!PickerSearchResultMatches(row,IK(0x1234,78,9001)));
    CHECK(!PickerSearchResultMatches(row,IK(0x1234,77,9002)));
    CHECK(!PickerSearchResultMatches(row,WindowIdentityKey{}));
}

static void test_picker_state_whole_object_swap_includes_generation_sentinel(){
    const GUID leftDesktop=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    const GUID rightDesktop=G(
        L"{231A0000-0000-0000-0000-000000000002}");
    PickerState left,right;
    left.currentDesktop=leftDesktop;
    left.selectedDesktop=rightDesktop;
    left.selectedIndex=3;
    left.activeWindow=IK(11,12,13);
    left.searchText=L"left";
    left.searchActive=true;
    left.transition.phase=PickerPhase::TargetIssue;
    left.transition.runtimeKey="runtime-left";
    left.transition.capturedTitle=L"captured title";
    left.transition.capturedTitleComplete=true;
    left.scrollByDesktop[GuidKey(leftDesktop)]=4;
    left.paintGeneration=101;
    right.currentDesktop=rightDesktop;
    right.selectedDesktop=leftDesktop;
    right.selectedIndex=7;
    right.activeWindow=IK(21,22,23);
    right.searchText=L"right";
    right.scrollByDesktop[GuidKey(rightDesktop)]=8;
    right.paintGeneration=202;

    CHECK(noexcept(SwapPickerState(left,right)));
    CHECK(noexcept(left.swap(right)));
    SwapPickerState(left,right);
    CHECK(IsCurrentDesktop(left,rightDesktop));
    CHECK(left.selectedIndex==7 && left.searchText==L"right");
    CHECK(left.paintGeneration==202);
    CHECK(IsCurrentDesktop(right,leftDesktop));
    CHECK(right.selectedIndex==3 && right.searchText==L"left");
    CHECK(right.searchActive && right.controlledTransition());
    CHECK(right.transition.runtimeKey=="runtime-left");
    CHECK(right.transition.capturedTitle==L"captured title");
    CHECK(right.transition.capturedTitleComplete);
    CHECK(right.scrollByDesktop.at(GuidKey(leftDesktop))==4);
    CHECK(right.paintGeneration==101);
    SwapPickerState(left,right);
    CHECK(left.paintGeneration==101 && right.paintGeneration==202);
}

static PickerState PickerTransitionFixture(uint64_t generation=41){
    PickerState state;
    const GUID origin=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    const GUID destination=G(
        L"{231A0000-0000-0000-0000-000000000002}");
    state.currentDesktop=origin;
    CHECK(SetPickerSelection(state,1,destination));
    state.activeWindow=IK(0x1234,77,9001);
    state.transition.generation=generation;
    state.transition.reservationToken.owner=MoveOwner::Picker;
    state.transition.reservationToken.operationId=generation;
    state.transition.reservationToken.jobId=generation+1000;
    state.transition.target=state.activeWindow;
    state.transition.runtimeKey=RuntimeKey(state.activeWindow);
    state.transition.pendingRecordId=
        "{00000000-0000-0000-0000-000000001101}";
    state.transition.targetOrigin=origin;
    state.transition.popupOrigin=origin;
    state.transition.currentOrigin=origin;
    state.transition.destination=destination;
    return state;
}

static PickerObservation PickerObservationFor(
        const PickerEffect& effect,PickerEvent event){
    PickerObservation observation;
    observation.event=event;
    observation.generation=effect.generation;
    observation.effectKind=effect.kind;
    observation.effectSerial=effect.effectSerial;
    return observation;
}

static PickerEffect PickerAckPopupDesktop(
        PickerState& state,const PickerEffect& effect,
        PickerReadValidity validity,const GUID& actual){
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    observation.popupRead=validity;
    observation.actualPopupDesktop=actual;
    return AdvancePickerTransition(state,observation);
}

static PickerEffect PickerAckCurrentDesktop(
        PickerState& state,const PickerEffect& effect,
        PickerReadValidity validity,const GUID& actual){
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    observation.currentRead=validity;
    observation.actualCurrentDesktop=actual;
    return AdvancePickerTransition(state,observation);
}

static void CheckPickerEffect(const PickerState& state,
                              const PickerEffect& effect,
                              PickerEffectKind kind){
    CHECK(effect.kind==kind);
    CHECK(effect.generation==state.transition.generation);
    CHECK(effect.effectSerial!=0);
    CHECK(state.transition.pendingEffect==kind);
    CHECK(state.transition.effectSerial==effect.effectSerial);
    CHECK(state.controlledTransition());
}

static void test_picker_transition_success_has_exact_verified_effect_order(){
    PickerState state=PickerTransitionFixture();
    std::vector<PickerEffectKind> order;
    auto accept=[&](PickerEffect effect,PickerEffectKind expected){
        CheckPickerEffect(state,effect,expected);
        order.push_back(effect.kind);
        return effect;
    };

    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=state.transition.generation;
    PickerEffect effect=accept(
        AdvancePickerTransition(state,begin),PickerEffectKind::MoveTarget);
    CHECK(state.transition.phase==PickerPhase::TargetIssue);
    CHECK(state.transition.targetMayHaveMoved);
    CHECK(state.transition.forwardTargetAttempts==1);

    PickerObservation issued=PickerObservationFor(effect,PickerEvent::ApiCompleted);
    issued.identity=PickerIdentityValidity::Match;
    issued.apiInvoked=true;
    issued.apiAccepted=false;
    effect=accept(AdvancePickerTransition(state,issued),
                  PickerEffectKind::ReadTarget);
    CHECK(state.transition.phase==PickerPhase::TargetVerify);

    PickerObservation targetRead=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    targetRead.identity=PickerIdentityValidity::Match;
    targetRead.targetRead=PickerReadValidity::Valid;
    targetRead.actualTargetDesktop=state.transition.destination;
    effect=accept(AdvancePickerTransition(state,targetRead),
                  PickerEffectKind::ValidateTarget);
    CHECK(state.transition.phase==PickerPhase::IdentityVerifyBeforePopup);

    PickerObservation validated=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    validated.identity=PickerIdentityValidity::Match;
    effect=accept(AdvancePickerTransition(state,validated),
                  PickerEffectKind::MovePopup);
    issued=PickerObservationFor(effect,PickerEvent::ApiCompleted);
    issued.apiInvoked=true;
    issued.apiAccepted=false;
    effect=accept(AdvancePickerTransition(state,issued),
                  PickerEffectKind::ReadPopup);
    CHECK(state.transition.phase==PickerPhase::PopupVerify);

    PickerObservation popupRead=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    popupRead.popupRead=PickerReadValidity::Valid;
    popupRead.actualPopupDesktop=state.transition.destination;
    effect=accept(AdvancePickerTransition(state,popupRead),
                  PickerEffectKind::ValidateTarget);
    CHECK(state.transition.phase==PickerPhase::IdentityVerifyBeforeSwitch);

    validated=PickerObservationFor(effect,PickerEvent::EffectCompleted);
    validated.identity=PickerIdentityValidity::Match;
    effect=accept(AdvancePickerTransition(state,validated),
                  PickerEffectKind::SwitchDesktop);
    issued=PickerObservationFor(effect,PickerEvent::ApiCompleted);
    issued.identity=PickerIdentityValidity::Match;
    issued.apiInvoked=true;
    issued.apiAccepted=false;
    effect=accept(AdvancePickerTransition(state,issued),
                  PickerEffectKind::ReadCurrent);
    CHECK(state.transition.phase==PickerPhase::DestinationVerify);

    PickerObservation currentRead=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    currentRead.currentRead=PickerReadValidity::Valid;
    currentRead.actualCurrentDesktop=state.transition.destination;
    effect=accept(AdvancePickerTransition(state,currentRead),
                  PickerEffectKind::ReadPopup);
    popupRead=PickerObservationFor(effect,PickerEvent::ReadbackCompleted);
    popupRead.popupRead=PickerReadValidity::Valid;
    popupRead.actualPopupDesktop=state.transition.destination;
    effect=accept(AdvancePickerTransition(state,popupRead),
                  PickerEffectKind::SaveExactTarget);
    CHECK(state.transition.commitCutoffReached);

    PickerObservation saved=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    saved.identity=PickerIdentityValidity::Match;
    saved.saveStatus=PopupSaveStatus::Saved;
    effect=accept(AdvancePickerTransition(state,saved),
                  PickerEffectKind::Refresh);
    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    effect=accept(AdvancePickerTransition(state,refreshed),
                  PickerEffectKind::ShowAndFocus);
    PickerObservation focused=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    focused.popupIsForeground=true;
    const PickerEffect done=AdvancePickerTransition(state,focused);
    CHECK(done.kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(state.controlledTransition());
    CHECK(!PickerRuntimeTerminalizationReady(state.transition,false));
    CHECK(PickerRuntimeTerminalizationReady(state.transition,true));
    CHECK(DecidePickerTerminalGuardRelease(false,false,false)==
        PickerTerminalGuardReleaseAction::ResolvedAbsent);
    CHECK(DecidePickerTerminalGuardRelease(false,false,true)==
        PickerTerminalGuardReleaseAction::RetryExactOwner);
    CHECK(DecidePickerTerminalGuardRelease(true,false,true)==
        PickerTerminalGuardReleaseAction::RetryExactOwner);
    CHECK(DecidePickerTerminalGuardRelease(true,true,true)==
        PickerTerminalGuardReleaseAction::ConsumeExact);
    CHECK(DecidePickerTerminalNoProgressRoute(false,true)==
        PickerTerminalNoProgressRoute::DelayedTimer);
    CHECK(DecidePickerTerminalNoProgressRoute(false,false)==
        PickerTerminalNoProgressRoute::DurableExternalKick);
    CHECK(DecidePickerTerminalNoProgressRoute(true,true)==
        PickerTerminalNoProgressRoute::DurableExternalKick);
    CHECK(FinalizePickerTransition(state));
    CHECK(state.transition.phase==PickerPhase::Idle);
    CHECK(!state.controlledTransition());

    const std::vector<PickerEffectKind> expected={
        PickerEffectKind::MoveTarget,
        PickerEffectKind::ReadTarget,
        PickerEffectKind::ValidateTarget,
        PickerEffectKind::MovePopup,
        PickerEffectKind::ReadPopup,
        PickerEffectKind::ValidateTarget,
        PickerEffectKind::SwitchDesktop,
        PickerEffectKind::ReadCurrent,
        PickerEffectKind::ReadPopup,
        PickerEffectKind::SaveExactTarget,
        PickerEffectKind::Refresh,
        PickerEffectKind::ShowAndFocus
    };
    CHECK(order==expected);
    CHECK(state.transition.phase==PickerPhase::Idle);
    CHECK(!state.transition.targetMayHaveMoved);
}

static void test_picker_transition_rejects_stale_duplicate_and_wrong_effect_acks(){
    PickerState state=PickerTransitionFixture(82);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=82;
    PickerEffect move=AdvancePickerTransition(state,begin);
    CheckPickerEffect(state,move,PickerEffectKind::MoveTarget);
    const PickerPhase phase=state.transition.phase;
    const uint64_t serial=state.transition.effectSerial;

    PickerObservation stale=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    stale.generation=81;
    stale.identity=PickerIdentityValidity::Match;
    stale.apiInvoked=true;
    CHECK(AdvancePickerTransition(state,stale).kind==PickerEffectKind::None);
    CHECK(state.transition.phase==phase &&
          state.transition.effectSerial==serial);

    PickerObservation wrong=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    wrong.effectKind=PickerEffectKind::ReadTarget;
    wrong.identity=PickerIdentityValidity::Match;
    wrong.apiInvoked=true;
    CHECK(AdvancePickerTransition(state,wrong).kind==PickerEffectKind::None);
    CHECK(state.transition.pendingEffect==PickerEffectKind::MoveTarget);

    PickerObservation accepted=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    accepted.identity=PickerIdentityValidity::Match;
    accepted.apiInvoked=true;
    PickerEffect read=AdvancePickerTransition(state,accepted);
    CheckPickerEffect(state,read,PickerEffectKind::ReadTarget);
    CHECK(read.effectSerial>move.effectSerial);
    CHECK(AdvancePickerTransition(state,accepted).kind==PickerEffectKind::None);
    CHECK(state.transition.pendingEffect==PickerEffectKind::ReadTarget);

    PickerObservation timer;
    timer.event=PickerEvent::Timer;
    timer.generation=state.transition.generation;
    CHECK(AdvancePickerTransition(state,timer).kind==PickerEffectKind::None);
    CHECK(state.transition.pendingEffect==PickerEffectKind::ReadTarget);
}

static void test_picker_identity_loss_rolls_back_without_switch_or_save(){
    PickerState state=PickerTransitionFixture(90);
    std::vector<PickerEffectKind> effects;
    auto record=[&](PickerEffect effect){
        if(effect.kind!=PickerEffectKind::None) effects.push_back(effect.kind);
        return effect;
    };
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=90;
    PickerEffect effect=record(AdvancePickerTransition(state,begin));
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::ApiCompleted);
    observation.identity=PickerIdentityValidity::Match;
    observation.apiInvoked=true;
    effect=record(AdvancePickerTransition(state,observation));
    observation=PickerObservationFor(effect,PickerEvent::ReadbackCompleted);
    observation.identity=PickerIdentityValidity::Match;
    observation.targetRead=PickerReadValidity::Valid;
    observation.actualTargetDesktop=state.transition.destination;
    effect=record(AdvancePickerTransition(state,observation));
    CHECK(state.transition.phase==PickerPhase::IdentityVerifyBeforePopup);

    observation=PickerObservationFor(effect,PickerEvent::EffectCompleted);
    observation.identity=PickerIdentityValidity::Lost;
    effect=record(AdvancePickerTransition(state,observation));
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    CHECK(state.transition.targetIdentityUnusable);
    CHECK(state.transition.rollbackTargetAttempts==0);
    CHECK(state.transition.rollbackPopupAttempts==0);
    CHECK(state.transition.rollbackSwitchAttempts==0);
    effect=record(PickerAckPopupDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.currentOrigin));
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);
    effect=record(PickerAckCurrentDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.currentOrigin));
    CHECK(effect.kind==PickerEffectKind::Refresh);
    observation=PickerObservationFor(effect,PickerEvent::EffectCompleted);
    observation.apiAccepted=true;
    effect=record(AdvancePickerTransition(state,observation));
    CHECK(effect.kind==PickerEffectKind::ReportFailure);
    CHECK(std::count(effects.begin(),effects.end(),
                     PickerEffectKind::SwitchDesktop)==0);
    CHECK(std::count(effects.begin(),effects.end(),
                     PickerEffectKind::SaveExactTarget)==0);
}

static void test_picker_escape_hides_once_rolls_back_and_never_refocuses(){
    PickerState state=PickerTransitionFixture(101);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=101;
    PickerEffect move=AdvancePickerTransition(state,begin);
    CHECK(move.kind==PickerEffectKind::MoveTarget);

    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=101;
    PickerEffect hide=AdvancePickerTransition(state,cancel);
    CheckPickerEffect(state,hide,PickerEffectKind::Hide);
    CHECK(state.transition.dismissed && state.transition.cancelRequested);
    CHECK(AdvancePickerTransition(state,cancel).kind==PickerEffectKind::None);
    PickerObservation hidden=PickerObservationFor(
        hide,PickerEvent::EffectCompleted);
    PickerEffect effect=AdvancePickerTransition(state,hidden);
    CHECK(effect.kind==PickerEffectKind::MoveTarget);

    auto api=[&](PickerEffect current){
        PickerObservation acknowledged=PickerObservationFor(
            current,PickerEvent::ApiCompleted);
        acknowledged.identity=PickerIdentityValidity::Match;
        acknowledged.apiInvoked=true;
        acknowledged.apiAccepted=true;
        return AdvancePickerTransition(state,acknowledged);
    };
    auto readTarget=[&](PickerEffect current,const GUID& desktop){
        PickerObservation acknowledged=PickerObservationFor(
            current,PickerEvent::ReadbackCompleted);
        acknowledged.identity=PickerIdentityValidity::Match;
        acknowledged.targetRead=PickerReadValidity::Valid;
        acknowledged.actualTargetDesktop=desktop;
        return AdvancePickerTransition(state,acknowledged);
    };
    effect=api(effect);
    effect=readTarget(effect,state.transition.targetOrigin);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    effect=PickerAckPopupDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.currentOrigin);
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);
    effect=PickerAckCurrentDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.currentOrigin);
    CHECK(effect.kind==PickerEffectKind::Refresh);
    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    effect=AdvancePickerTransition(state,refreshed);
    CHECK(effect.kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(FinalizePickerTransition(state));
    CHECK(state.transition.phase==PickerPhase::Idle);
    CHECK(!state.controlledTransition());
}

static void test_picker_rollback_exhaustion_preserves_actual_readbacks(){
    PickerState state=PickerTransitionFixture(111);
    state.transition.phase=PickerPhase::TargetVerify;
    state.transition.pendingEffect=PickerEffectKind::ReadTarget;
    state.transition.effectSerial=70;
    state.transition.forwardTargetAttempts=4;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    const GUID targetActual=G(
        L"{231A0000-0000-0000-0000-000000000003}");
    PickerObservation failedRead;
    failedRead.event=PickerEvent::ReadbackCompleted;
    failedRead.generation=111;
    failedRead.effectKind=PickerEffectKind::ReadTarget;
    failedRead.effectSerial=70;
    failedRead.identity=PickerIdentityValidity::Match;
    failedRead.targetRead=PickerReadValidity::Valid;
    failedRead.actualTargetDesktop=targetActual;
    PickerEffect effect=AdvancePickerTransition(state,failedRead);
    CHECK(effect.kind==PickerEffectKind::MoveTarget);

    for(int attempt=0;attempt<4;++attempt){
        PickerObservation issued=PickerObservationFor(
            effect,PickerEvent::ApiCompleted);
        issued.identity=PickerIdentityValidity::Match;
        issued.apiInvoked=true;
        issued.apiAccepted=false;
        effect=AdvancePickerTransition(state,issued);
        CHECK(effect.kind==PickerEffectKind::ReadTarget);
        PickerObservation read=PickerObservationFor(
            effect,PickerEvent::ReadbackCompleted);
        read.identity=PickerIdentityValidity::Match;
        read.targetRead=PickerReadValidity::Valid;
        read.actualTargetDesktop=targetActual;
        effect=AdvancePickerTransition(state,read);
        if(attempt<3) CHECK(effect.kind==PickerEffectKind::MoveTarget);
    }
    CHECK(effect.kind==PickerEffectKind::MovePopup);
    for(int attempt=0;attempt<4;++attempt){
        PickerObservation issued=PickerObservationFor(
            effect,PickerEvent::ApiCompleted);
        issued.apiInvoked=true;
        issued.apiAccepted=false;
        effect=AdvancePickerTransition(state,issued);
        PickerObservation read=PickerObservationFor(
            effect,PickerEvent::ReadbackCompleted);
        read.popupRead=PickerReadValidity::Valid;
        read.actualPopupDesktop=state.transition.destination;
        effect=AdvancePickerTransition(state,read);
    }
    CHECK(effect.kind==PickerEffectKind::SwitchDesktop);
    for(int attempt=0;attempt<4;++attempt){
        PickerObservation issued=PickerObservationFor(
            effect,PickerEvent::ApiCompleted);
        issued.apiInvoked=true;
        issued.apiAccepted=false;
        effect=AdvancePickerTransition(state,issued);
        CHECK(effect.kind==PickerEffectKind::ReadCurrent);
        PickerObservation current=PickerObservationFor(
            effect,PickerEvent::ReadbackCompleted);
        current.currentRead=PickerReadValidity::Valid;
        current.actualCurrentDesktop=state.transition.destination;
        effect=AdvancePickerTransition(state,current);
    }
    CHECK(effect.kind==PickerEffectKind::Refresh);
    CHECK(GuidEq(state.transition.observedTargetDesktop,targetActual));
    CHECK(GuidEq(state.transition.observedPopupDesktop,
                 state.transition.destination));
    CHECK(GuidEq(state.transition.observedCurrentDesktop,
                 state.transition.destination));
    CHECK(state.transition.failed);
    CHECK(!state.transition.diagnostic.empty());
}

static void test_picker_ui_action_gate_and_auto_supersession_are_exact(){
    PickerState state=PickerTransitionFixture(121);
    for(PickerUiAction action : {
            PickerUiAction::CtrlMove,PickerUiAction::PlainSwitch,
            PickerUiAction::FooterNavigation,PickerUiAction::CloseOrReopen,
            PickerUiAction::SearchEdit})
        CHECK(PickerUiActionAllowed(state,action));
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=121;
    CHECK(AdvancePickerTransition(state,begin).kind==
          PickerEffectKind::MoveTarget);
    for(PickerUiAction action : {
            PickerUiAction::CtrlMove,PickerUiAction::PlainSwitch,
            PickerUiAction::FooterNavigation,PickerUiAction::CloseOrReopen,
            PickerUiAction::SearchEdit})
        CHECK(!PickerUiActionAllowed(state,action));

    CHECK(DecidePickerAutoSupersession(false,false,false)==
          PickerAutoSupersession::None);
    CHECK(DecidePickerAutoSupersession(true,false,false)==
          PickerAutoSupersession::CancelQueued);
    CHECK(DecidePickerAutoSupersession(true,true,false)==
          PickerAutoSupersession::WaitForIssuedReadback);
    CHECK(DecidePickerAutoSupersession(true,false,true)==
          PickerAutoSupersession::WaitForIssuedReadback);
}

static void test_picker_close_rejects_controlled_transition_without_cancel(){
    PickerState idle=PickerTransitionFixture(122);
    CHECK(RoutePickerClose(idle)==PickerCloseRoute::Hide);

    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=122;
    CHECK(AdvancePickerTransition(idle,begin).kind==
          PickerEffectKind::MoveTarget);
    CHECK(RoutePickerClose(idle)==PickerCloseRoute::Reject);
    CHECK(!idle.transition.cancelRequested);
    CHECK(!idle.transition.dismissed);
}

static void test_picker_observation_kick_survives_post_and_timer_failure_once(){
    PickerState state=PickerTransitionFixture(131);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=131;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    observation.identity=PickerIdentityValidity::Match;

    PickerKickState kick;
    CHECK(StagePickerObservationKick(kick,observation,true,false,true)==
          PickerKickRoute::Posted);
    PickerObservation later=observation;
    ++later.effectSerial;
    CHECK(StagePickerObservationKick(kick,later,true,true,true)==
          PickerKickRoute::PendingPreserved);
    PickerObservation consumed;
    CHECK(ConsumePickerObservationKick(kick,consumed));
    CHECK(consumed.effectSerial==observation.effectSerial);
    CHECK(!ConsumePickerObservationKick(kick,consumed));

    CHECK(StagePickerObservationKick(kick,observation,false,true,true)==
          PickerKickRoute::TimerArmed);
    CHECK(ConsumePickerObservationKick(kick,consumed));
    CHECK(StagePickerObservationKick(kick,observation,false,false,true)==
          PickerKickRoute::InlineFallback);
    CHECK(ConsumePickerObservationKick(kick,consumed));
    CHECK(StagePickerObservationKick(kick,observation,false,false,false)==
          PickerKickRoute::Teardown);
    CHECK(ConsumePickerObservationKick(kick,consumed));
}

static void test_picker_save_result_and_fresh_generation_callbacks_are_typed(){
    const WindowIdentityKey identity=IK(0x1234,77,9001);
    CHECK(PickerFreshRuntimeMatches(identity,91,identity,91));
    CHECK(!PickerFreshRuntimeMatches(identity,91,identity,90));
    CHECK(!PickerFreshRuntimeMatches(identity,91,IK(0x1234,77,9002),91));
    CHECK(PickerHasSafeOriginRecord(false,true));
    CHECK(PickerHasSafeOriginRecord(true,false));
    CHECK(!PickerHasSafeOriginRecord(false,false));
    CHECK(PickerMayReserveStableRecordId(true,true,true));
    CHECK(!PickerMayReserveStableRecordId(false,true,true));
    CHECK(!PickerMayReserveStableRecordId(true,false,true));
    CHECK(!PickerMayReserveStableRecordId(true,true,false));
    int callbacks=0;
    PopupSaveResult saved;
    saved.status=PopupSaveStatus::Saved;
    saved.app="firefox";
    CHECK(CompletePickerLifecycleForSave(saved,[&](const std::string& app){
        CHECK(app=="firefox");
        ++callbacks;
    }));
    CHECK(callbacks==1);
    PopupSaveResult ordinary;
    ordinary.status=PopupSaveStatus::NotTracked;
    CHECK(!CompletePickerLifecycleForSave(ordinary,[&](const std::string&){
        ++callbacks;
    }));
    PopupSaveResult failed;
    failed.status=PopupSaveStatus::Failed;
    failed.failure=PopupSaveFailure::StorageUnavailable;
    failed.app="firefox";
    CHECK(!CompletePickerLifecycleForSave(failed,[&](const std::string&){
        ++callbacks;
    }));
    CHECK(callbacks==1);
    CHECK(std::wstring(PickerSaveFailureDiagnostic(
              PopupSaveFailure::StorageUnavailable)).find(L"unavailable")!=
          std::wstring::npos);

    PickerState state=PickerTransitionFixture(139);
    state.transition.phase=PickerPhase::SaveExactTarget;
    state.transition.commitCutoffReached=true;
    state.transition.pendingEffect=PickerEffectKind::SaveExactTarget;
    state.transition.effectSerial=41;
    PickerEffect save;
    save.kind=PickerEffectKind::SaveExactTarget;
    save.generation=139;
    save.effectSerial=41;
    PickerObservation saveFailed=PickerObservationFor(
        save,PickerEvent::EffectCompleted);
    saveFailed.identity=PickerIdentityValidity::Match;
    saveFailed.saveStatus=PopupSaveStatus::Failed;
    saveFailed.saveFailure=PopupSaveFailure::StorageReadOnly;
    CHECK(AdvancePickerTransition(state,saveFailed).kind==
          PickerEffectKind::Refresh);
    CHECK(state.transition.diagnostic==
          PickerSaveFailureDiagnostic(PopupSaveFailure::StorageReadOnly));
}

static void test_ctrl_move_non_browser_never_mutates_auto_layout(){
    for(bool unrelatedFreshPresent : {false,true}){
        LayoutWin remembered;
        remembered.recordId="remembered-firefox";
        remembered.app="firefox";
        remembered.activeTitle="saved tab";
        std::vector<LayoutWin> automaticLayout{remembered};
        std::map<std::string,LayoutWin> acceptedFreshElsewhere;
        if(unrelatedFreshPresent)
            acceptedFreshElsewhere["other-runtime"]=remembered;
        const size_t recordsBefore=automaticLayout.size();
        const size_t freshBefore=acceptedFreshElsewhere.size();
        int mutations=0,writes=0,callbacks=0;

        const PopupSaveResult result=RunPickerPersistenceTransaction(
            PopupBrowserClassification::NotTracked,std::string(),
            PopupPersistenceReadiness::Ready,
            [&](const std::string& app){
                ++mutations;
                automaticLayout.push_back(remembered);
                acceptedFreshElsewhere.clear();
                ++writes;
                PopupSaveResult saved;
                saved.status=PopupSaveStatus::Saved;
                saved.app=app;
                return saved;
            });
        CHECK(result.status==PopupSaveStatus::NotTracked);
        CHECK(result.failure==PopupSaveFailure::None);
        CHECK(result.app.empty());
        CHECK(!CompletePickerLifecycleForSave(
            result,[&](const std::string&){ ++callbacks; }));
        CHECK(mutations==0 && writes==0 && callbacks==0);
        CHECK(automaticLayout.size()==recordsBefore);
        CHECK(automaticLayout[0].recordId=="remembered-firefox");
        CHECK(acceptedFreshElsewhere.size()==freshBefore);
        if(unrelatedFreshPresent)
            CHECK(acceptedFreshElsewhere.count("other-runtime")==1);
    }
}

static void test_ctrl_move_tracked_browser_unwritable_reports_failed(){
    LayoutWin remembered;
    remembered.recordId="tracked-firefox";
    remembered.app="firefox";
    remembered.activeTitle="saved tab";
    std::vector<LayoutWin> automaticLayout{remembered};
    const LayoutWin before=automaticLayout[0];
    int mutations=0,writes=0,callbacks=0;

    const PopupSaveResult result=RunPickerPersistenceTransaction(
        PopupBrowserClassification::Tracked,"firefox",
        PopupPersistenceReadiness::ReadOnly,
        [&](const std::string& app){
            ++mutations;
            automaticLayout[0].desktop=
                G(L"{231A0000-0000-0000-0000-000000000099}");
            ++writes;
            PopupSaveResult saved;
            saved.status=PopupSaveStatus::Saved;
            saved.app=app;
            return saved;
        });
    CHECK(result.status==PopupSaveStatus::Failed);
    CHECK(result.failure==PopupSaveFailure::StorageReadOnly);
    CHECK(result.app=="firefox");
    CHECK(!CompletePickerLifecycleForSave(
        result,[&](const std::string&){ ++callbacks; }));
    CHECK(mutations==0 && writes==0 && callbacks==0);
    CHECK(automaticLayout.size()==1);
    CHECK(automaticLayout[0].recordId==before.recordId);
    CHECK(GuidEq(automaticLayout[0].desktop,before.desktop));
}

static void test_picker_persistence_app_staging_contains_allocation_failure(){
    PopupSaveResult result;
    const std::string app(256,'f');
    int assignments=0;
    CHECK(!TryStagePickerPersistenceAppNoThrow(
        result,app,[&](std::string&,const std::string&){
            ++assignments;
            throw std::bad_alloc();
        }));
    CHECK(assignments==1 && result.app.empty());
    CHECK(TryStagePickerPersistenceAppNoThrow(
        result,app,[&](std::string& output,const std::string& value){
            ++assignments;
            output=value;
        }));
    CHECK(assignments==2 && result.app==app);

    const PopupSaveResult failed=RunPickerPersistenceTransaction(
        PopupBrowserClassification::Tracked,app,
        PopupPersistenceReadiness::Ready,
        [&](const std::string&)->PopupSaveResult {
            throw std::bad_alloc();
        });
    CHECK(failed.status==PopupSaveStatus::Failed);
    CHECK(failed.failure==PopupSaveFailure::Unexpected);
    CHECK(failed.app==app);

    int committedWithoutApp=0;
    const PopupSaveResult unstaged=RunPickerPersistenceTransaction(
        PopupBrowserClassification::Tracked,app,
        PopupPersistenceReadiness::Ready,
        [&](const std::string&)->PopupSaveResult {
            ++committedWithoutApp;
            PopupSaveResult saved;
            saved.status=PopupSaveStatus::Saved;
            return saved;
        });
    CHECK(committedWithoutApp==1);
    CHECK(unstaged.status==PopupSaveStatus::Failed);
    CHECK(unstaged.failure==PopupSaveFailure::Unexpected);
    CHECK(unstaged.app.empty());
}

static void test_picker_api_ack_distinguishes_invocation_and_identity_quality(){
    PickerState state=PickerTransitionFixture(141);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=141;
    PickerEffect move=AdvancePickerTransition(state,begin);
    CHECK(move.kind==PickerEffectKind::MoveTarget);
    CHECK(state.transition.targetMayHaveMoved);
    CHECK(state.transition.forwardTargetAttempts==1);

    PickerObservation notInvoked=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    notInvoked.identity=PickerIdentityValidity::Lost;
    notInvoked.apiInvoked=false;
    notInvoked.apiAccepted=false;
    PickerEffect effect=AdvancePickerTransition(state,notInvoked);
    CHECK(effect.kind==PickerEffectKind::Refresh);
    CHECK(!state.transition.targetMayHaveMoved);
    CHECK(state.transition.targetIdentityUnusable);

    state=PickerTransitionFixture(142);
    begin.generation=142;
    move=AdvancePickerTransition(state,begin);
    PickerObservation invoked=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    invoked.identity=PickerIdentityValidity::Match;
    invoked.apiInvoked=true;
    invoked.apiAccepted=false;
    effect=AdvancePickerTransition(state,invoked);
    CHECK(effect.kind==PickerEffectKind::ReadTarget);
    CHECK(state.transition.targetMayHaveMoved);

    PickerObservation unknown=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    unknown.identity=PickerIdentityValidity::Indeterminate;
    unknown.targetRead=PickerReadValidity::Unavailable;
    effect=AdvancePickerTransition(state,unknown);
    CHECK(state.transition.targetIdentityUnusable);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    CHECK(state.transition.rollbackTargetAttempts==0);
}

static void test_picker_post_switch_reads_current_then_popup_and_requires_both(){
    PickerState state=PickerTransitionFixture(151);
    state.transition.phase=PickerPhase::SwitchIssue;
    state.transition.forwardSwitchAttempts=1;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.pendingEffect=PickerEffectKind::SwitchDesktop;
    state.transition.effectSerial=50;
    PickerObservation switched;
    switched.event=PickerEvent::ApiCompleted;
    switched.generation=151;
    switched.effectKind=PickerEffectKind::SwitchDesktop;
    switched.effectSerial=50;
    switched.identity=PickerIdentityValidity::Match;
    switched.apiInvoked=true;
    switched.apiAccepted=true;
    PickerEffect effect=AdvancePickerTransition(state,switched);
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);

    PickerObservation current=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    current.currentRead=PickerReadValidity::Valid;
    current.actualCurrentDesktop=state.transition.destination;
    effect=AdvancePickerTransition(state,current);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    PickerObservation popup=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    popup.popupRead=PickerReadValidity::Unavailable;
    effect=AdvancePickerTransition(state,popup);
    CHECK(effect.kind!=PickerEffectKind::SaveExactTarget);
    CHECK(state.transition.failed ||
          state.transition.phase==PickerPhase::IdentityVerifyBeforePopup);
}

static void test_picker_escape_after_save_emission_is_a_commit_cutoff(){
    PickerState state=PickerTransitionFixture(161);
    state.transition.phase=PickerPhase::SaveExactTarget;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    state.transition.pendingEffect=PickerEffectKind::SaveExactTarget;
    state.transition.effectSerial=80;
    state.transition.commitCutoffReached=true;
    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=161;
    CHECK(AdvancePickerTransition(state,cancel).kind==PickerEffectKind::None);
    CHECK(state.transition.pendingEffect==PickerEffectKind::SaveExactTarget);
    PickerEffect save;
    save.kind=PickerEffectKind::SaveExactTarget;
    save.generation=161;
    save.effectSerial=80;
    PickerObservation saved=PickerObservationFor(
        save,PickerEvent::EffectCompleted);
    saved.identity=PickerIdentityValidity::Match;
    saved.saveStatus=PopupSaveStatus::Saved;
    PickerEffect hide=AdvancePickerTransition(state,saved);
    CHECK(hide.kind==PickerEffectKind::Hide);
    PickerObservation hidden=PickerObservationFor(
        hide,PickerEvent::EffectCompleted);
    PickerEffect effect=AdvancePickerTransition(state,hidden);
    CHECK(effect.kind==PickerEffectKind::Refresh);
    CHECK(state.transition.phase==PickerPhase::RefreshModel);
    CHECK(state.transition.rollbackTargetAttempts==0);
    CHECK(state.transition.rollbackPopupAttempts==0);
    CHECK(state.transition.rollbackSwitchAttempts==0);
    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    CHECK(AdvancePickerTransition(state,refreshed).kind==
          PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(FinalizePickerTransition(state));
    CHECK(state.transition.phase==PickerPhase::Idle);
}

static void test_picker_cancel_discards_only_matching_unissued_non_save_effect(){
    PickerState state=PickerTransitionFixture(162);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=162;
    PickerEffect scheduled=AdvancePickerTransition(state,begin);
    bool hasScheduled=true;
    CHECK(DiscardPickerUnissuedEffectForCancel(
        scheduled,hasScheduled,state.transition));
    CHECK(!hasScheduled && scheduled.kind==PickerEffectKind::None);
    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=162;
    cancel.unissuedEffectCancelled=true;
    PickerEffect hide=AdvancePickerTransition(state,cancel);
    CHECK(hide.kind==PickerEffectKind::Hide);
    CHECK(!state.transition.targetMayHaveMoved);
    PickerEffect refresh=AdvancePickerTransition(
        state,PickerObservationFor(hide,PickerEvent::EffectCompleted));
    CHECK(refresh.kind==PickerEffectKind::Refresh);

    state=PickerTransitionFixture(163);
    state.transition.phase=PickerPhase::SaveExactTarget;
    state.transition.commitCutoffReached=true;
    state.transition.pendingEffect=PickerEffectKind::SaveExactTarget;
    state.transition.effectSerial=81;
    scheduled.kind=PickerEffectKind::SaveExactTarget;
    scheduled.generation=163;
    scheduled.effectSerial=81;
    hasScheduled=true;
    CHECK(!DiscardPickerUnissuedEffectForCancel(
        scheduled,hasScheduled,state.transition));
    CHECK(hasScheduled && scheduled.kind==PickerEffectKind::SaveExactTarget);

    PickerEffect stale=scheduled;
    --stale.effectSerial;
    CHECK(!DiscardPickerUnissuedEffectForCancel(
        stale,hasScheduled,state.transition));
    CHECK(hasScheduled);

    state=PickerTransitionFixture(164);
    state.transition.phase=PickerPhase::RefreshModel;
    state.transition.pendingEffect=PickerEffectKind::Refresh;
    state.transition.effectSerial=82;
    scheduled.kind=PickerEffectKind::Refresh;
    scheduled.generation=164;
    scheduled.effectSerial=82;
    hasScheduled=true;
    CHECK(!DiscardPickerUnissuedEffectForCancel(
        scheduled,hasScheduled,state.transition));
    cancel=PickerObservation{};
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=164;
    CHECK(AdvancePickerTransition(state,cancel).kind==PickerEffectKind::None);
    CHECK(state.transition.pendingEffect==PickerEffectKind::Refresh);
    PickerObservation refreshed=PickerObservationFor(
        scheduled,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    hide=AdvancePickerTransition(state,refreshed);
    CHECK(hide.kind==PickerEffectKind::Hide);
    CHECK(AdvancePickerTransition(
        state,PickerObservationFor(hide,PickerEvent::EffectCompleted)).kind==
        PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);

    scheduled=hide;
    hasScheduled=true;
    CHECK(!DiscardPickerUnissuedEffectForCancel(
        scheduled,hasScheduled,state.transition));
    CHECK(hasScheduled);
}

static void test_picker_reservation_filter_and_owner_replacement_are_exact(){
    FastWin exact;
    exact.hwnd=reinterpret_cast<HWND>(0x1234);
    exact.pid=77;
    exact.processStart=9001;
    FastWin reused=exact;
    reused.processStart=9002;
    std::map<std::string,WindowIdentityKey> pickerReservations;
    pickerReservations[RuntimeKey(exact)]=IdentityOf(exact);
    CHECK(PickerReservationBlocks(exact,pickerReservations));
    CHECK(!PickerReservationBlocks(reused,pickerReservations));
    CHECK(PickerReservationReplacementAllowed(
        MoveOwner::AutoReconcile,MoveOwner::Picker));
    CHECK(!PickerReservationReplacementAllowed(
        MoveOwner::ManualTray,MoveOwner::Picker));
    CHECK(!PickerReservationReplacementAllowed(
        MoveOwner::Picker,MoveOwner::ManualTray));
    CHECK(!PickerReservationReplacementAllowed(
        MoveOwner::Picker,MoveOwner::AutoReconcile));
    CHECK(!PickerReservationReplacementAllowed(
        MoveOwner::Picker,MoveOwner::Picker));
}

static void test_picker_raw_edit_and_tab_cache_are_model_generation_scoped(){
    PickerState state;
    state.searchActive=true;
    CHECK(SetPickerSearchText(state,L"GitHub PR",L"github pr"));
    CHECK(state.searchEditText==L"GitHub PR");
    CHECK(state.searchText==L"github pr");
    CHECK(state.searchActive);
    state.searchActive=false;
    CHECK(SetPickerSearchText(state,L"GitHub PR",L"github pr"));
    CHECK(!state.searchActive);
    state.searchActive=true;
    CHECK(SetPickerSearchText(state,L"",L""));
    CHECK(state.searchActive);
    PickerState preserved=PreservePickerUi(state);
    CHECK(preserved.searchEditText.empty());
    CHECK(preserved.searchText.empty());
    CHECK(preserved.searchActive);

    PickerTabSearchCacheState cache;
    cache.modelGeneration=7;
    cache.query=L"github pr";
    cache.ready=true;
    CHECK(PickerTabSearchCacheUsable(cache,7,L"github pr"));
    CHECK(!PickerTabSearchCacheUsable(cache,8,L"github pr"));
    CHECK(!PickerTabSearchCacheUsable(cache,7,L"mail"));
    InvalidatePickerTabSearchCache(cache,8);
    CHECK(cache.modelGeneration==8 && !cache.ready && cache.query.empty());

    CHECK(BeginPickerTabSearchAttempt(cache,901,9,L"tab only"));
    CHECK(PickerTabSearchAttemptMatches(cache,901,9,L"tab only"));
    CHECK(!PickerTabSearchCacheUsable(cache,9,L"tab only"));
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,901,9,L"tab only","firefox"));
    CHECK(CompletePickerTabSearchAttempt(cache,901,9,L"tab only"));
    CHECK(cache.retryNeeded && !cache.pending && !cache.ready);
    CHECK(!PickerTabSearchRetryPostNeeded(cache,9,L"tab only"));
    for(const char* unrelated : {"chrome","edge","other","chrome"}){
        NotePickerTabSearchRouteFreed(cache,unrelated);
        CHECK(!PickerTabSearchRetryPostNeeded(cache,9,L"tab only"));
        CHECK(cache.retryAttempts==0);
    }
    NotePickerTabSearchRouteFreed(cache,"firefox");
    CHECK(PickerTabSearchRetryPostNeeded(cache,9,L"tab only"));
    CHECK(MarkPickerTabSearchRetryPosted(cache,9,L"tab only"));
    CHECK(!PickerTabSearchRetryPostNeeded(cache,9,L"tab only"));
    CHECK(AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        cache,false,9,L"tab only"));
    CHECK(!cache.retryPosted && cache.retryDeliveryPending &&
          cache.routeFreed);
    CHECK(BeginPickerTabSearchAttempt(cache,902,9,L"tab only"));
    CHECK(!cache.retryDeliveryPending && !cache.routeFreed);
    CHECK(CompletePickerTabSearchAttempt(cache,902,9,L"tab only"));
    CHECK(PickerTabSearchCacheUsable(cache,9,L"tab only"));

    CHECK(BeginPickerTabSearchAttempt(cache,1001,10,L"query a"));
    CHECK(BeginPickerTabSearchAttempt(cache,1002,10,L"query b"));
    CHECK(BeginPickerTabSearchAttempt(cache,1003,10,L"query a"));
    CHECK(PickerTabSearchAttemptMatches(cache,1003,10,L"query a"));
    CHECK(cache.pending);
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1003,10,L"query a","firefox"));
    CHECK(!MarkPickerTabSearchRetryNeeded(
        cache,1001,10,L"query a","firefox",
        PickerTabSearchRetryTrigger::Immediate));
    CHECK(!CompletePickerTabSearchAttempt(cache,1001,10,L"query a"));
    CHECK(cache.pending &&
          PickerTabSearchAttemptMatches(cache,1003,10,L"query a"));
    CHECK(!cache.routeFreed);
    CHECK(CompletePickerTabSearchAttempt(cache,1003,10,L"query a"));
    CHECK(!PickerTabSearchCacheUsable(cache,10,L"query a"));
    CHECK(!PickerTabSearchRetryPostNeeded(cache,10,L"query a"));
    NotePickerTabSearchRouteFreed(cache,"firefox");
    CHECK(PickerTabSearchRetryPostNeeded(cache,10,L"query a"));

    InvalidatePickerTabSearchCache(cache,11);
    CHECK(BeginPickerTabSearchAttempt(cache,1101,11,L"capacity"));
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1101,11,L"capacity","firefox",
        PickerTabSearchRetryTrigger::AnyRoute));
    CHECK(CompletePickerTabSearchAttempt(cache,1101,11,L"capacity"));
    NotePickerTabSearchRouteFreed(cache,"chrome");
    CHECK(PickerTabSearchRetryPostNeeded(cache,11,L"capacity"));

    InvalidatePickerTabSearchCache(cache,12);
    CHECK(BeginPickerTabSearchAttempt(cache,1201,12,L"failure"));
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1201,12,L"failure","firefox",
        PickerTabSearchRetryTrigger::Immediate));
    CHECK(CompletePickerTabSearchAttempt(cache,1201,12,L"failure"));
    CHECK(PickerTabSearchRetryPostNeeded(cache,12,L"failure"));
    CHECK(MarkPickerTabSearchRetryPosted(cache,12,L"failure"));
    CHECK(!AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        cache,true,12,L"failure"));
    CHECK(cache.retryPosted && cache.routeFreed);
    CHECK(!AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        cache,false,13,L"failure"));
    CHECK(cache.retryPosted && cache.routeFreed);
    CHECK(AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        cache,false,12,L"failure"));
    CHECK(!cache.retryPosted && cache.retryDeliveryPending &&
          cache.routeFreed);

    InvalidatePickerTabSearchCache(cache,13);
    CHECK(BeginPickerTabSearchAttempt(cache,1301,13,L"durable"));
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1301,13,L"durable","firefox",
        PickerTabSearchRetryTrigger::Immediate));
    CHECK(CompletePickerTabSearchAttempt(cache,1301,13,L"durable"));
    CHECK(MarkPickerTabSearchRetryDeliveryFailed(
        cache,13,L"durable"));
    CHECK(PickerTabSearchRetryDeliveryKickNeeded(
        cache,13,L"durable"));
    CHECK(!PickerTabSearchRetryDeliveryReadyWhenIdle(
        cache,true,13,L"durable"));
    CHECK(!PickerTabSearchRetryDeliveryReadyWhenIdle(
        cache,false,14,L"durable"));
    CHECK(PickerTabSearchRetryDeliveryKickNeeded(
        cache,13,L"durable"));
    CHECK(PickerTabSearchRetryDeliveryReadyWhenIdle(
        cache,false,13,L"durable"));
    CHECK(PickerTabSearchRetryDeliveryKickNeeded(
        cache,13,L"durable"));
    CHECK(!cache.retryPosted && cache.routeFreed &&
          cache.retryAttempts==0);
    CHECK(BeginPickerTabSearchAttempt(cache,1302,13,L"durable"));
    CHECK(!cache.retryDeliveryPending && !cache.routeFreed);
    CHECK(cache.retryAttempts==1);
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1302,13,L"durable","firefox",
        PickerTabSearchRetryTrigger::Immediate));
    CHECK(CompletePickerTabSearchAttempt(cache,1302,13,L"durable"));
    CHECK(MarkPickerTabSearchRetryDeliveryFailed(
        cache,13,L"durable"));
    CHECK(!cache.retryPosted && cache.retryAttempts==1);
    CHECK(!MarkPickerTabSearchRetryDeliveryFailed(
        cache,14,L"durable"));
    CHECK(PickerTabSearchRetryDeliveryKickNeeded(
        cache,13,L"durable"));
    CHECK(MarkPickerTabSearchRetryPosted(cache,13,L"durable"));
    CHECK(!PickerTabSearchRetryDeliveryKickNeeded(
        cache,13,L"durable"));
    CHECK(cache.retryPosted && cache.retryAttempts==1);
    InvalidatePickerTabSearchCache(cache,14);
    CHECK(!cache.retryDeliveryPending);
}

static void test_picker_tab_retry_delivery_is_failure_atomic(){
    PickerTabSearchCacheState cache;
    CHECK(BeginPickerTabSearchAttempt(cache,1401,14,L"atomic"));
    CHECK(MarkPickerTabSearchRetryNeeded(
        cache,1401,14,L"atomic","firefox",
        PickerTabSearchRetryTrigger::Immediate));
    CHECK(CompletePickerTabSearchAttempt(cache,1401,14,L"atomic"));
    CHECK(MarkPickerTabSearchRetryPosted(cache,14,L"atomic"));
    CHECK(AcquirePickerTabSearchRetryPostLeaseWhenIdle(
        cache,false,14,L"atomic"));

    bool operationPublished=false;
    bool partialRoute=false;
    int cleanups=0;
    PickerTabSearchEnsureOutcome outcome=RunPickerTabSearchEnsureAttempt(
        cache,1402,14,L"atomic",
        [&]()->bool {
            operationPublished=true;
            throw std::bad_alloc();
        },
        [&](){ partialRoute=true; return true; },
        [&]() noexcept {
            operationPublished=false;
            partialRoute=false;
            ++cleanups;
        });
    CHECK(outcome==PickerTabSearchEnsureOutcome::RetryPreserved);
    CHECK(cleanups==1 && !operationPublished && !partialRoute);
    CHECK(!cache.pending && cache.retryNeeded && cache.routeFreed &&
          cache.retryDeliveryPending && cache.retryAttempts==0);

    outcome=RunPickerTabSearchEnsureAttempt(
        cache,1403,14,L"atomic",
        [&](){ operationPublished=true; return true; },
        [&]()->bool {
            partialRoute=true;
            throw std::bad_alloc();
        },
        [&]() noexcept {
            operationPublished=false;
            partialRoute=false;
            ++cleanups;
        });
    CHECK(outcome==PickerTabSearchEnsureOutcome::RetryPreserved);
    CHECK(cleanups==2 && !operationPublished && !partialRoute);
    CHECK(!cache.pending && cache.retryNeeded && cache.routeFreed &&
          cache.retryDeliveryPending && cache.retryAttempts==1);

    outcome=RunPickerTabSearchEnsureAttempt(
        cache,1404,14,L"atomic",
        [&](){ operationPublished=true; return true; },
        [&](){
            CHECK(PickerTabSearchAttemptMatches(
                cache,1404,14,L"atomic"));
            CHECK(CompletePickerTabSearchAttempt(
                cache,1404,14,L"atomic"));
            operationPublished=false;
            return true;
        },
        [&]() noexcept {
            operationPublished=false;
            ++cleanups;
        });
    CHECK(outcome==PickerTabSearchEnsureOutcome::AttemptCommitted);
    CHECK(cleanups==2 && !operationPublished);
    CHECK(PickerTabSearchCacheUsable(cache,14,L"atomic"));
    CHECK(!cache.retryDeliveryPending && !cache.routeFreed);

    bool foreignOperation=true;
    int ownedCleanups=0;
    CHECK(!CleanupPickerTabSearchPublishedOperation(
        false,[&](){ foreignOperation=false; ++ownedCleanups; }));
    CHECK(foreignOperation && ownedCleanups==0);
    CHECK(CleanupPickerTabSearchPublishedOperation(
        true,[&](){ ++ownedCleanups; }));
    CHECK(foreignOperation && ownedCleanups==1);
}

static void test_picker_nonidle_gate_includes_all_tray_mutators(){
    PickerState state=PickerTransitionFixture(171);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=171;
    CHECK(AdvancePickerTransition(state,begin).kind==
          PickerEffectKind::MoveTarget);
    for(PickerUiAction action : {
            PickerUiAction::ManualSave,PickerUiAction::ManualRestore,
            PickerUiAction::Settings,PickerUiAction::About,
            PickerUiAction::Help})
        CHECK(!PickerUiActionAllowed(state,action));
    CHECK(PickerUiActionAllowed(state,PickerUiAction::TrayExit));
    CHECK(RoutePickerShutdown(state)==
          PickerShutdownRoute::CancelThenProceed);
    PickerState idle;
    CHECK(RoutePickerShutdown(idle)==PickerShutdownRoute::Proceed);

    int cancels=0,pumps=0;
    CHECK(RunPickerShutdownDrain(
        idle,[&](){ ++cancels; },[&](){ ++pumps; }));
    CHECK(cancels==0 && pumps==0);

    PickerState draining=PickerTransitionFixture(172);
    draining.transition.phase=PickerPhase::TargetIssue;
    CHECK(RunPickerShutdownDrain(
        draining,[&](){ ++cancels; },[&](){
            ++pumps;
            draining.transition.pendingEffect=PickerEffectKind::None;
            draining.transition.terminalAcknowledged=true;
            CHECK(FinalizePickerTransition(draining));
        }));
    CHECK(cancels==1 && pumps==1 && !draining.controlledTransition());

    PickerState blocked=PickerTransitionFixture(173);
    blocked.transition.phase=PickerPhase::PopupVerify;
    CHECK(!RunPickerShutdownDrain(
        blocked,[&](){ ++cancels; },[&](){ ++pumps; }));
    CHECK(blocked.controlledTransition());
    CHECK(cancels==2 && pumps==5);
    CHECK(blocked.transition.reservationToken.owner==MoveOwner::Picker);
    CHECK(blocked.transition.reservationToken.operationId==173);
    CHECK(blocked.transition.reservationToken.jobId==1173);
}

static void test_picker_shutdown_driver_preserves_delays_and_skips_ui_work(){
    for(PickerEffectKind read : {
            PickerEffectKind::ReadTarget,PickerEffectKind::ReadPopup,
            PickerEffectKind::ReadCurrent}){
        CHECK(PickerEffectRequiresSettlingDelay(read));
        CHECK(RoutePickerEffectExecution(read,true)==
              PickerEffectExecutionRoute::DeferUntilDue);
    }

    CHECK(RoutePickerEffectExecution(PickerEffectKind::Refresh,true)==
          PickerEffectExecutionRoute::AcknowledgeWithoutUi);
    CHECK(RoutePickerEffectExecution(PickerEffectKind::ShowAndFocus,true)==
          PickerEffectExecutionRoute::AcknowledgeWithoutUi);
    CHECK(RoutePickerEffectExecution(PickerEffectKind::ReportFailure,true)==
          PickerEffectExecutionRoute::AcknowledgeWithoutUi);
    CHECK(RoutePickerEffectExecution(PickerEffectKind::Hide,true)==
          PickerEffectExecutionRoute::Execute);
    CHECK(RoutePickerEffectExecution(PickerEffectKind::MoveTarget,true)==
          PickerEffectExecutionRoute::Execute);
    CHECK(!PickerEffectRequiresSettlingDelay(PickerEffectKind::Refresh));
    CHECK(!PickerEffectRequiresSettlingDelay(PickerEffectKind::MoveTarget));

    const uint64_t notBefore=PickerSettlingNotBeforeMs(1000,150);
    CHECK(notBefore==1150);
    CHECK(PickerSettlingDelayRemainingMs(1000,notBefore)==150);
    CHECK(PickerSettlingDelayRemainingMs(1149,notBefore)==1);
    CHECK(PickerSettlingDelayRemainingMs(1150,notBefore)==0);
    CHECK(PickerSettlingDelayRemainingMs(1200,notBefore)==0);
    CHECK(PickerSettlingNotBeforeMs(
        (std::numeric_limits<uint64_t>::max)()-10,150)==
        (std::numeric_limits<uint64_t>::max)());
    CHECK(!PickerPumpImmediateKickAllowed(true));
    CHECK(PickerPumpImmediateKickAllowed(false));
    CHECK(!PickerDurableKickRequiredAfterDefer(true,false));
    CHECK(PickerDurableKickRequiredAfterDefer(true,true));
    CHECK(PickerDurableKickRequiredAfterDefer(false,false));
    CHECK(PickerDurableKickRequiredAfterDefer(false,true));

    for(PickerEffectKind effect : {
            PickerEffectKind::ReadTarget,PickerEffectKind::ReadPopup,
            PickerEffectKind::ReadCurrent,PickerEffectKind::Refresh,
            PickerEffectKind::ShowAndFocus,PickerEffectKind::ReportFailure,
            PickerEffectKind::Hide,PickerEffectKind::MoveTarget})
        CHECK(RoutePickerEffectExecution(effect,false)==
              PickerEffectExecutionRoute::Execute);
}

static PickerEffect PickerAckApi(PickerState& state,const PickerEffect& effect,
                                 bool invoked=true,
                                 PickerIdentityValidity identity=
                                     PickerIdentityValidity::Match){
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::ApiCompleted);
    observation.identity=identity;
    observation.apiInvoked=invoked;
    observation.apiAccepted=false;
    return AdvancePickerTransition(state,observation);
}

static PickerEffect PickerAckTarget(PickerState& state,
                                    const PickerEffect& effect,
                                    PickerReadValidity validity,
                                    const GUID& actual,
                                    PickerIdentityValidity identity=
                                        PickerIdentityValidity::Match){
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    observation.identity=identity;
    observation.targetRead=validity;
    observation.actualTargetDesktop=actual;
    return AdvancePickerTransition(state,observation);
}

static PickerEffect PickerAckIdentity(PickerState& state,
                                      const PickerEffect& effect,
                                      PickerIdentityValidity identity=
                                          PickerIdentityValidity::Match){
    PickerObservation observation=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    observation.identity=identity;
    return AdvancePickerTransition(state,observation);
}

static void test_picker_forward_attempt_matrix_has_independent_exact_budgets(){
    PickerState state=PickerTransitionFixture(181);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=181;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    for(int attempt=1;attempt<=4;++attempt){
        CHECK(effect.kind==PickerEffectKind::MoveTarget);
        effect=PickerAckApi(state,effect);
        CHECK(effect.kind==PickerEffectKind::ReadTarget);
        const GUID actual=attempt==4 ? state.transition.destination
                                    : state.transition.targetOrigin;
        effect=PickerAckTarget(
            state,effect,PickerReadValidity::Valid,actual);
    }
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);
    CHECK(state.transition.forwardTargetAttempts==4);

    effect=PickerAckIdentity(state,effect);
    for(int attempt=1;attempt<=4;++attempt){
        CHECK(effect.kind==PickerEffectKind::MovePopup);
        effect=PickerAckApi(state,effect);
        CHECK(effect.kind==PickerEffectKind::ReadPopup);
        PickerObservation popup=PickerObservationFor(
            effect,PickerEvent::ReadbackCompleted);
        popup.popupRead=PickerReadValidity::Valid;
        popup.actualPopupDesktop=attempt==4
            ? state.transition.destination : state.transition.currentOrigin;
        effect=AdvancePickerTransition(state,popup);
        if(attempt<4) effect=PickerAckIdentity(state,effect);
    }
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);
    CHECK(state.transition.forwardPopupAttempts==4);

    effect=PickerAckIdentity(state,effect);
    for(int attempt=1;attempt<=4;++attempt){
        CHECK(effect.kind==PickerEffectKind::SwitchDesktop);
        effect=PickerAckApi(state,effect);
        CHECK(effect.kind==PickerEffectKind::ReadCurrent);
        PickerObservation current=PickerObservationFor(
            effect,PickerEvent::ReadbackCompleted);
        current.currentRead=PickerReadValidity::Valid;
        current.actualCurrentDesktop=attempt==4
            ? state.transition.destination : state.transition.currentOrigin;
        effect=AdvancePickerTransition(state,current);
        if(attempt<4) effect=PickerAckIdentity(state,effect);
    }
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    CHECK(state.transition.forwardSwitchAttempts==4);
    CHECK(state.transition.rollbackTargetAttempts==0);
    CHECK(state.transition.rollbackPopupAttempts==0);
    CHECK(state.transition.rollbackSwitchAttempts==0);
}

static void test_picker_unavailable_target_read_exhausts_then_uses_rollback_budget(){
    PickerState state=PickerTransitionFixture(182);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=182;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    for(int attempt=1;attempt<=4;++attempt){
        effect=PickerAckApi(state,effect);
        effect=PickerAckTarget(
            state,effect,PickerReadValidity::Unavailable,GUID{});
    }
    CHECK(effect.kind==PickerEffectKind::MoveTarget);
    CHECK(state.transition.forwardTargetAttempts==4);
    CHECK(state.transition.rollbackTargetAttempts==1);
    CHECK(state.transition.targetMayHaveMoved);
}

static void test_picker_wrong_event_and_serial_never_consume_pending_lane(){
    const PickerEffectKind kinds[]={
        PickerEffectKind::MoveTarget,PickerEffectKind::MovePopup,
        PickerEffectKind::SwitchDesktop,PickerEffectKind::ReadTarget,
        PickerEffectKind::ReadPopup,PickerEffectKind::ReadCurrent,
        PickerEffectKind::ValidateTarget,PickerEffectKind::SaveExactTarget,
        PickerEffectKind::Refresh,PickerEffectKind::ShowAndFocus,
        PickerEffectKind::Hide,PickerEffectKind::ReportFailure
    };
    for(PickerEffectKind kind : kinds){
        PickerState state=PickerTransitionFixture(190+
            static_cast<uint64_t>(kind));
        state.transition.phase=PickerPhase::FocusRestore;
        state.transition.pendingEffect=kind;
        state.transition.effectSerial=91;
        PickerObservation wrong;
        wrong.event=PickerEvent::Timer;
        wrong.generation=state.transition.generation;
        wrong.effectKind=kind;
        wrong.effectSerial=91;
        CHECK(AdvancePickerTransition(state,wrong).kind==PickerEffectKind::None);
        CHECK(state.transition.pendingEffect==kind);
        wrong.event=PickerEvent::EffectCompleted;
        wrong.effectSerial=90;
        CHECK(AdvancePickerTransition(state,wrong).kind==PickerEffectKind::None);
        CHECK(state.transition.pendingEffect==kind);
        wrong.effectSerial=91;
        wrong.generation=state.transition.generation-1;
        CHECK(AdvancePickerTransition(state,wrong).kind==PickerEffectKind::None);
        CHECK(state.transition.pendingEffect==kind);
    }
}

static void test_picker_cancel_partial_matrix_stops_forward_and_hides_once(){
    for(unsigned mask=0;mask<8;++mask){
        PickerState state=PickerTransitionFixture(220+mask);
        state.transition.phase=PickerPhase::DestinationVerify;
        state.transition.pendingEffect=PickerEffectKind::ReadPopup;
        state.transition.effectSerial=60;
        state.transition.targetMayHaveMoved=(mask&1)!=0;
        state.transition.popupMayHaveMoved=(mask&2)!=0;
        state.transition.switchMayHaveChanged=(mask&4)!=0;
        PickerObservation cancel;
        cancel.event=PickerEvent::CancelRequested;
        cancel.generation=state.transition.generation;
        PickerEffect hide=AdvancePickerTransition(state,cancel);
        CHECK(hide.kind==PickerEffectKind::Hide);
        CHECK(AdvancePickerTransition(state,cancel).kind==PickerEffectKind::None);
        PickerObservation stale;
        stale.event=PickerEvent::ReadbackCompleted;
        stale.generation=state.transition.generation;
        stale.effectKind=PickerEffectKind::ReadPopup;
        stale.effectSerial=60;
        CHECK(AdvancePickerTransition(state,stale).kind==PickerEffectKind::None);
        PickerObservation hidden=PickerObservationFor(
            hide,PickerEvent::EffectCompleted);
        PickerEffect rollback=AdvancePickerTransition(state,hidden);
        const PickerEffectKind expected=(mask&1)
            ? PickerEffectKind::MoveTarget : mask!=0
            ? PickerEffectKind::ReadTarget : PickerEffectKind::Refresh;
        CHECK(rollback.kind==expected);
        CHECK(rollback.kind!=PickerEffectKind::SaveExactTarget);
        CHECK(rollback.kind!=PickerEffectKind::ShowAndFocus);
    }
}

static void test_picker_focus_budget_terminates_without_report_loop(){
    PickerState state=PickerTransitionFixture(241);
    state.transition.phase=PickerPhase::FocusRestore;
    state.transition.focusAttempts=1;
    state.transition.pendingEffect=PickerEffectKind::ShowAndFocus;
    state.transition.effectSerial=1;
    PickerEffect effect;
    for(int attempt=1;attempt<=4;++attempt){
        PickerObservation focus;
        focus.event=PickerEvent::EffectCompleted;
        focus.generation=241;
        focus.effectKind=PickerEffectKind::ShowAndFocus;
        focus.effectSerial=state.transition.effectSerial;
        focus.popupIsForeground=false;
        effect=AdvancePickerTransition(state,focus);
        if(attempt<4) CHECK(effect.kind==PickerEffectKind::ShowAndFocus);
    }
    CHECK(effect.kind==PickerEffectKind::ReportFailure);
    PickerObservation reported=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    CHECK(AdvancePickerTransition(state,reported).kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(state.transition.failureReported);
    CHECK(AdvancePickerTransition(state,reported).kind==PickerEffectKind::None);
    CHECK(FinalizePickerTransition(state));
}

static void test_picker_begin_gate_rejects_incomplete_or_busy_state_atomically(){
    for(int invalid=0;invalid<13;++invalid){
        PickerState state=PickerTransitionFixture(251+invalid);
        if(invalid==0) state.transition.target=WindowIdentityKey{};
        if(invalid==1) state.transition.targetOrigin=GUID{};
        if(invalid==2) state.transition.popupOrigin=GUID{};
        if(invalid==3) state.transition.currentOrigin=GUID{};
        if(invalid==4) state.transition.destination=GUID{};
        if(invalid==5) state.transition.reservationToken.jobId=0;
        if(invalid==6) state.transition.reservationToken.operationId=0;
        if(invalid==7)
            state.transition.reservationToken.owner=MoveOwner::ManualTray;
        if(invalid==8) state.selectedIndex=-1;
        if(invalid==9) state.selectedDesktop=state.transition.currentOrigin;
        if(invalid==10) state.currentDesktop=state.transition.destination;
        if(invalid==11) state.activeWindow=IK(0x1234,77,9002);
        if(invalid==12) state.transition.runtimeKey.clear();
        PickerObservation begin;
        begin.event=PickerEvent::Begin;
        begin.generation=state.transition.generation;
        CHECK(AdvancePickerTransition(state,begin).kind==PickerEffectKind::None);
        CHECK(state.transition.phase==PickerPhase::Idle);
        CHECK(state.transition.pendingEffect==PickerEffectKind::None);
    }
    PickerState busy=PickerTransitionFixture(260);
    busy.transition.phase=PickerPhase::RefreshModel;
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=260;
    CHECK(AdvancePickerTransition(busy,begin).kind==PickerEffectKind::None);
    CHECK(busy.transition.phase==PickerPhase::RefreshModel);
}

static void test_picker_ui_preservation_prunes_scroll_and_adopts_only_safe_idle_target(){
    PickerState state;
    const GUID first=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID second=G(L"{231A0000-0000-0000-0000-000000000002}");
    state.scrollByDesktop[GuidKey(first)]=4;
    state.scrollByDesktop[GuidKey(second)]=8;
    PrunePickerScrollState(state,std::vector<GUID>{second});
    CHECK(state.scrollByDesktop.size()==1);
    CHECK(state.scrollByDesktop.at(GuidKey(second))==8);

    const WindowIdentityKey opening=IK(0x1111,1,11);
    const WindowIdentityKey candidate=IK(0x2222,2,22);
    state.activeWindow=opening;
    state.transition.phase=PickerPhase::TargetVerify;
    CHECK(!AdoptPickerIdleActiveIdentity(
        state,candidate,0x3333,0x4444,true));
    CHECK(SameIdentity(state.activeWindow,opening));
    state.transition.phase=PickerPhase::Idle;
    CHECK(!AdoptPickerIdleActiveIdentity(
        state,IK(0x3333,3,33),0x3333,0x4444,true));
    CHECK(!AdoptPickerIdleActiveIdentity(
        state,IK(0x4444,4,44),0x3333,0x4444,true));
    CHECK(!AdoptPickerIdleActiveIdentity(
        state,candidate,0x3333,0x4444,false));
    CHECK(AdoptPickerIdleActiveIdentity(
        state,candidate,0x3333,0x4444,true));
    CHECK(SameIdentity(state.activeWindow,candidate));
}

static void test_picker_lightweight_refresh_is_single_snapshot_cache_only(){
    PickerState state;
    int snapshots=0,publications=0;
    CHECK(RunPickerLightweightRefresh(
        state,
        [&](){ ++snapshots; return 7; },
        [&](int snapshot,PickerState& staged){
            ++publications;
            staged.paintGeneration=static_cast<uint64_t>(snapshot);
            return true;
        }));
    CHECK(snapshots==1 && publications==1 && state.paintGeneration==7);
    state.transition.phase=PickerPhase::TargetIssue;
    CHECK(!RunPickerLightweightRefresh(
        state,
        [&](){ ++snapshots; return 8; },
        [&](int,PickerState&){ ++publications; return true; }));
    CHECK(snapshots==1 && publications==1);
}

static void test_picker_lightweight_snapshot_clears_stale_truth_exactly(){
    PickerState state;
    const GUID oldDesktop=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    const GUID newDesktop=G(
        L"{231A0000-0000-0000-0000-000000000002}");
    const WindowIdentityKey opening=IK(0x1111,11,111);
    const WindowIdentityKey external=IK(0x2222,22,222);
    state.currentDesktop=oldDesktop;
    state.activeWindow=opening;

    CHECK(ApplyPickerLightweightHighlightSnapshot(
        state,PickerReadValidity::Unavailable,GUID{},
        PickerForegroundObservation::Popup,WindowIdentityKey{},
        PickerIdentityValidity::Match,0x3333,0x3333,true)==
        PickerLightweightActiveUpdate::Preserved);
    CHECK(GuidIsZero(state.currentDesktop));
    CHECK(SameIdentity(state.activeWindow,opening));

    CHECK(ApplyPickerLightweightHighlightSnapshot(
        state,PickerReadValidity::Valid,newDesktop,
        PickerForegroundObservation::Unavailable,WindowIdentityKey{},
        PickerIdentityValidity::Indeterminate,0x3333,0x3333,true)==
        PickerLightweightActiveUpdate::Preserved);
    CHECK(GuidEq(state.currentDesktop,newDesktop));
    CHECK(SameIdentity(state.activeWindow,opening));

    CHECK(ApplyPickerLightweightHighlightSnapshot(
        state,PickerReadValidity::Valid,newDesktop,
        PickerForegroundObservation::Popup,WindowIdentityKey{},
        PickerIdentityValidity::Lost,0x3333,0x3333,true)==
        PickerLightweightActiveUpdate::Cleared);
    CHECK(!SameIdentity(state.activeWindow,state.activeWindow));

    state.activeWindow=opening;
    CHECK(ApplyPickerLightweightHighlightSnapshot(
        state,PickerReadValidity::Valid,newDesktop,
        PickerForegroundObservation::UnusableExternal,WindowIdentityKey{},
        PickerIdentityValidity::Match,0x3333,0x3333,true)==
        PickerLightweightActiveUpdate::Cleared);
    CHECK(!SameIdentity(state.activeWindow,state.activeWindow));

    state.activeWindow=opening;
    CHECK(ApplyPickerLightweightHighlightSnapshot(
        state,PickerReadValidity::Valid,newDesktop,
        PickerForegroundObservation::ValidExternal,external,
        PickerIdentityValidity::Lost,0x3333,0x3333,true)==
        PickerLightweightActiveUpdate::Adopted);
    CHECK(SameIdentity(state.activeWindow,external));
}

static void test_picker_later_noninvocation_preserves_prior_unresolved_move(){
    PickerState state=PickerTransitionFixture(271);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=271;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    effect=PickerAckApi(state,effect);
    const GUID third=G(
        L"{231A0000-0000-0000-0000-000000000003}");
    effect=PickerAckTarget(state,effect,PickerReadValidity::Valid,third);
    CHECK(effect.kind==PickerEffectKind::MoveTarget);
    effect=PickerAckApi(state,effect,false,PickerIdentityValidity::Match);
    CHECK(state.transition.targetMayHaveMoved);
    CHECK(effect.kind==PickerEffectKind::MoveTarget);
    CHECK(state.transition.phase==PickerPhase::RollbackTargetIssue);
}

static void test_picker_popup_recovery_after_fourth_switch_saves_without_fifth(){
    PickerState state=PickerTransitionFixture(272);
    state.transition.phase=PickerPhase::DestinationVerify;
    state.transition.forwardTargetAttempts=1;
    state.transition.forwardPopupAttempts=1;
    state.transition.forwardSwitchAttempts=4;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    state.transition.observedCurrentValidity=PickerReadValidity::Valid;
    state.transition.observedCurrentDesktop=state.transition.destination;
    state.transition.pendingEffect=PickerEffectKind::ReadPopup;
    state.transition.effectSerial=30;
    PickerObservation missing;
    missing.event=PickerEvent::ReadbackCompleted;
    missing.generation=272;
    missing.effectKind=PickerEffectKind::ReadPopup;
    missing.effectSerial=30;
    missing.popupRead=PickerReadValidity::Unavailable;
    PickerEffect effect=AdvancePickerTransition(state,missing);
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);
    effect=PickerAckIdentity(state,effect);
    CHECK(effect.kind==PickerEffectKind::MovePopup);
    effect=PickerAckApi(state,effect);
    PickerObservation popup=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    popup.popupRead=PickerReadValidity::Valid;
    popup.actualPopupDesktop=state.transition.destination;
    effect=AdvancePickerTransition(state,popup);
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);
    CHECK(state.transition.phase==PickerPhase::DestinationVerify);
    effect=PickerAckCurrentDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.destination);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    effect=PickerAckPopupDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.destination);
    CHECK(effect.kind==PickerEffectKind::SaveExactTarget);
    CHECK(state.transition.forwardSwitchAttempts==4);
}

static void test_picker_popup_repair_rechecks_current_before_save(){
    PickerState state=PickerTransitionFixture(2721);
    state.transition.phase=PickerPhase::DestinationVerify;
    state.transition.forwardTargetAttempts=1;
    state.transition.forwardPopupAttempts=1;
    state.transition.forwardSwitchAttempts=3;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    state.transition.rollbackVerificationRequired=true;
    state.transition.observedCurrentValidity=PickerReadValidity::Valid;
    state.transition.observedCurrentDesktop=state.transition.destination;
    state.transition.pendingEffect=PickerEffectKind::ReadPopup;
    state.transition.effectSerial=31;

    PickerEffect readPopup{PickerEffectKind::ReadPopup,2721,31,GUID{}};
    PickerEffect effect=PickerAckPopupDesktop(
        state,readPopup,PickerReadValidity::Unavailable,GUID{});
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);
    effect=PickerAckIdentity(state,effect);
    CHECK(effect.kind==PickerEffectKind::MovePopup);
    effect=PickerAckApi(state,effect);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    effect=PickerAckPopupDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.destination);
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);

    // The current desktop changed while the popup was being repaired.  The
    // stale pre-repair destination read must never allow Save.
    effect=PickerAckCurrentDesktop(
        state,effect,PickerReadValidity::Valid,
        state.transition.currentOrigin);
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);
    CHECK(effect.kind!=PickerEffectKind::SaveExactTarget);
}

static void test_picker_cancel_during_exhausted_rollback_cannot_strand(){
    PickerState state=PickerTransitionFixture(273);
    state.transition.phase=PickerPhase::RollbackTargetVerify;
    state.transition.rollbackTargetAttempts=4;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.pendingEffect=PickerEffectKind::ReadTarget;
    state.transition.effectSerial=40;
    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=273;
    PickerEffect hide=AdvancePickerTransition(state,cancel);
    CHECK(hide.kind==PickerEffectKind::Hide);
    PickerObservation hidden=PickerObservationFor(
        hide,PickerEvent::EffectCompleted);
    PickerEffect effect=AdvancePickerTransition(state,hidden);
    CHECK(effect.kind==PickerEffectKind::ReadTarget ||
          effect.kind==PickerEffectKind::MovePopup ||
          effect.kind==PickerEffectKind::Refresh);
    CHECK(effect.kind!=PickerEffectKind::None);
}

static void test_picker_failed_current_rollback_suppresses_invisible_focus(){
    PickerState state=PickerTransitionFixture(274);
    state.transition.phase=PickerPhase::OriginVerify;
    state.transition.failed=true;
    state.transition.rollbackSwitchAttempts=4;
    state.transition.popupMayHaveMoved=false;
    state.transition.switchMayHaveChanged=true;
    state.transition.pendingEffect=PickerEffectKind::ReadCurrent;
    state.transition.effectSerial=50;
    PickerObservation current;
    current.event=PickerEvent::ReadbackCompleted;
    current.generation=274;
    current.effectKind=PickerEffectKind::ReadCurrent;
    current.effectSerial=50;
    current.currentRead=PickerReadValidity::Unavailable;
    PickerEffect effect=AdvancePickerTransition(state,current);
    CHECK(effect.kind==PickerEffectKind::Refresh);
    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    effect=AdvancePickerTransition(state,refreshed);
    CHECK(effect.kind==PickerEffectKind::ReportFailure);
    PickerObservation reported=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    effect=AdvancePickerTransition(state,reported);
    CHECK(effect.kind==PickerEffectKind::None);
    CHECK(state.transition.suppressFocus);
    CHECK(state.transition.terminalAcknowledged);
}

static void test_picker_effect_serial_exhaustion_becomes_terminal_not_stranded(){
    PickerState state=PickerTransitionFixture(275);
    state.transition.effectSerial=(std::numeric_limits<uint64_t>::max)();
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=275;
    CHECK(AdvancePickerTransition(state,begin).kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(state.transition.failed);
    CHECK(FinalizePickerTransition(state));
}

static void test_picker_unknown_identity_never_allows_future_target_api(){
    PickerState state=PickerTransitionFixture(276);
    state.transition.phase=PickerPhase::TargetVerify;
    state.transition.targetMayHaveMoved=true;
    state.transition.pendingEffect=PickerEffectKind::ReadTarget;
    state.transition.effectSerial=71;
    PickerObservation unknown;
    unknown.event=PickerEvent::ReadbackCompleted;
    unknown.generation=276;
    unknown.effectKind=PickerEffectKind::ReadTarget;
    unknown.effectSerial=71;
    unknown.identity=PickerIdentityValidity::Unknown;
    PickerEffect effect=AdvancePickerTransition(state,unknown);
    CHECK(state.transition.targetIdentityUnusable);
    CHECK(state.transition.rollbackTargetAttempts==0);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
}

static void test_picker_cancel_terminal_effects_never_reemit_or_refocus(){
    struct Case { PickerPhase phase; PickerEffectKind pending; };
    const Case cases[]={
        {PickerPhase::RefreshModel,PickerEffectKind::Refresh},
        {PickerPhase::FailureReport,PickerEffectKind::ReportFailure},
        {PickerPhase::FocusRestore,PickerEffectKind::ShowAndFocus}
    };
    for(size_t index=0;index<3;++index){
        PickerState state=PickerTransitionFixture(280+index);
        state.transition.phase=cases[index].phase;
        state.transition.pendingEffect=cases[index].pending;
        state.transition.effectSerial=80;
        PickerObservation cancel;
        cancel.event=PickerEvent::CancelRequested;
        cancel.generation=state.transition.generation;
        PickerEffect hide=AdvancePickerTransition(state,cancel);
        if(cases[index].pending==PickerEffectKind::Refresh){
            CHECK(hide.kind==PickerEffectKind::None);
            PickerEffect refresh;
            refresh.kind=PickerEffectKind::Refresh;
            refresh.generation=state.transition.generation;
            refresh.effectSerial=80;
            PickerObservation refreshed=PickerObservationFor(
                refresh,PickerEvent::EffectCompleted);
            refreshed.apiAccepted=true;
            hide=AdvancePickerTransition(state,refreshed);
        }
        CHECK(hide.kind==PickerEffectKind::Hide);
        PickerObservation hidden=PickerObservationFor(
            hide,PickerEvent::EffectCompleted);
        PickerEffect effect=AdvancePickerTransition(state,hidden);
        CHECK(effect.kind==PickerEffectKind::None);
        CHECK(state.transition.terminalAcknowledged);
        CHECK(state.transition.dismissed);
        CHECK(FinalizePickerTransition(state));
    }
}

static void test_picker_invoked_identity_loss_keeps_unknown_displacement(){
    PickerState state=PickerTransitionFixture(290);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=290;
    PickerEffect move=AdvancePickerTransition(state,begin);
    PickerObservation lost=PickerObservationFor(
        move,PickerEvent::ApiCompleted);
    lost.apiInvoked=true;
    lost.identity=PickerIdentityValidity::Lost;
    PickerEffect effect=AdvancePickerTransition(state,lost);
    CHECK(state.transition.targetMayHaveMoved);
    CHECK(state.transition.targetIdentityUnusable);
    CHECK(state.transition.observedTargetValidity==
          PickerReadValidity::Unavailable);
    CHECK(GuidIsZero(state.transition.observedTargetDesktop));
    CHECK(state.transition.rollbackTargetAttempts==0);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
}

static void test_picker_noninvoked_rollback_apis_still_require_readback(){
    struct Case {
        PickerPhase phase;
        PickerEffectKind api;
        PickerEffectKind read;
    };
    const Case cases[]={
        {PickerPhase::RollbackTargetIssue,PickerEffectKind::MoveTarget,
         PickerEffectKind::ReadTarget},
        {PickerPhase::RollbackPopupIssue,PickerEffectKind::MovePopup,
         PickerEffectKind::ReadPopup},
        {PickerPhase::RollbackSwitchIssue,PickerEffectKind::SwitchDesktop,
         PickerEffectKind::ReadCurrent}
    };
    for(size_t index=0;index<3;++index){
        PickerState state=PickerTransitionFixture(291+index);
        state.transition.phase=cases[index].phase;
        state.transition.failed=true;
        state.transition.targetMayHaveMoved=true;
        state.transition.popupMayHaveMoved=true;
        state.transition.switchMayHaveChanged=true;
        state.transition.pendingEffect=cases[index].api;
        state.transition.effectSerial=90;
        PickerObservation api;
        api.event=PickerEvent::ApiCompleted;
        api.generation=state.transition.generation;
        api.effectKind=cases[index].api;
        api.effectSerial=90;
        api.identity=PickerIdentityValidity::Match;
        api.apiInvoked=false;
        PickerEffect effect=AdvancePickerTransition(state,api);
        CHECK(effect.kind==cases[index].read);
        CHECK(state.controlledTransition());
    }
}

static void test_picker_cancel_all_displaced_rolls_back_in_exact_order(){
    PickerState state=PickerTransitionFixture(294);
    state.transition.phase=PickerPhase::DestinationVerify;
    state.transition.pendingEffect=PickerEffectKind::ReadPopup;
    state.transition.effectSerial=100;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    std::vector<PickerEffectKind> effects;
    auto record=[&](const PickerEffect& effect){
        if(effect.kind!=PickerEffectKind::None) effects.push_back(effect.kind);
        return effect;
    };

    PickerObservation cancel;
    cancel.event=PickerEvent::CancelRequested;
    cancel.generation=state.transition.generation;
    PickerEffect effect=record(AdvancePickerTransition(state,cancel));
    CHECK(effect.kind==PickerEffectKind::Hide);
    effect=record(AdvancePickerTransition(
        state,PickerObservationFor(effect,PickerEvent::EffectCompleted)));
    CHECK(effect.kind==PickerEffectKind::MoveTarget);

    PickerObservation api=PickerObservationFor(
        effect,PickerEvent::ApiCompleted);
    api.identity=PickerIdentityValidity::Match;
    api.apiInvoked=true;
    effect=record(AdvancePickerTransition(state,api));
    CHECK(effect.kind==PickerEffectKind::ReadTarget);
    effect=record(PickerAckTarget(
        state,effect,PickerReadValidity::Valid,
        state.transition.targetOrigin));
    CHECK(effect.kind==PickerEffectKind::MovePopup);

    api=PickerObservationFor(effect,PickerEvent::ApiCompleted);
    api.apiInvoked=true;
    effect=record(AdvancePickerTransition(state,api));
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    PickerObservation popup=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    popup.popupRead=PickerReadValidity::Valid;
    popup.actualPopupDesktop=state.transition.currentOrigin;
    effect=record(AdvancePickerTransition(state,popup));
    CHECK(effect.kind==PickerEffectKind::SwitchDesktop);

    api=PickerObservationFor(effect,PickerEvent::ApiCompleted);
    api.apiInvoked=true;
    effect=record(AdvancePickerTransition(state,api));
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);
    PickerObservation current=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    current.currentRead=PickerReadValidity::Valid;
    current.actualCurrentDesktop=state.transition.currentOrigin;
    effect=record(AdvancePickerTransition(state,current));
    CHECK(effect.kind==PickerEffectKind::Refresh);

    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    effect=record(AdvancePickerTransition(state,refreshed));
    CHECK(effect.kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(state.transition.dismissed);
    CHECK(!state.transition.targetMayHaveMoved);
    CHECK(!state.transition.popupMayHaveMoved);
    CHECK(!state.transition.switchMayHaveChanged);
    CHECK(FinalizePickerTransition(state));
    const std::vector<PickerEffectKind> expected={
        PickerEffectKind::Hide,
        PickerEffectKind::MoveTarget,PickerEffectKind::ReadTarget,
        PickerEffectKind::MovePopup,PickerEffectKind::ReadPopup,
        PickerEffectKind::SwitchDesktop,PickerEffectKind::ReadCurrent,
        PickerEffectKind::Refresh
    };
    CHECK(effects==expected);
    CHECK(std::count(effects.begin(),effects.end(),
                     PickerEffectKind::ShowAndFocus)==0);
}

static void test_picker_accepted_partial_rollback_reads_all_actual_components(){
    PickerState state=PickerTransitionFixture(2941);
    state.transition.phase=PickerPhase::TargetVerify;
    state.transition.forwardTargetAttempts=4;
    state.transition.targetMayHaveMoved=true;
    state.transition.rollbackVerificationRequired=true;
    state.transition.pendingEffect=PickerEffectKind::ReadTarget;
    state.transition.effectSerial=100;
    PickerEffect readTarget{PickerEffectKind::ReadTarget,2941,100,GUID{}};

    // The fourth forward read proves that the target is back at its origin,
    // but an earlier accepted API still requires a fresh rollback snapshot of
    // target, popup, and current before Refresh can publish actual truth.
    PickerEffect effect=PickerAckTarget(
        state,readTarget,PickerReadValidity::Valid,
        state.transition.targetOrigin);
    CHECK(effect.kind==PickerEffectKind::ReadTarget);
    CHECK(state.transition.phase==PickerPhase::RollbackTargetVerify);
    CHECK(state.transition.rollbackTargetAttempts==0);

    effect=PickerAckTarget(
        state,effect,PickerReadValidity::Valid,
        state.transition.targetOrigin);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    CHECK(state.transition.phase==PickerPhase::RollbackPopupVerify);
    CHECK(state.transition.rollbackPopupAttempts==0);

    PickerObservation popup=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    popup.popupRead=PickerReadValidity::Unavailable;
    effect=AdvancePickerTransition(state,popup);
    CHECK(effect.kind==PickerEffectKind::MovePopup);
    CHECK(state.transition.observedPopupValidity==
          PickerReadValidity::Unavailable);

    PickerObservation popupMove=PickerObservationFor(
        effect,PickerEvent::ApiCompleted);
    popupMove.apiInvoked=true;
    effect=AdvancePickerTransition(state,popupMove);
    CHECK(effect.kind==PickerEffectKind::ReadPopup);
    popup=PickerObservationFor(effect,PickerEvent::ReadbackCompleted);
    popup.popupRead=PickerReadValidity::Valid;
    popup.actualPopupDesktop=state.transition.currentOrigin;
    effect=AdvancePickerTransition(state,popup);
    CHECK(effect.kind==PickerEffectKind::ReadCurrent);
    CHECK(state.transition.phase==PickerPhase::OriginVerify);
    CHECK(state.transition.rollbackSwitchAttempts==0);

    PickerObservation current=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    current.currentRead=PickerReadValidity::Valid;
    current.actualCurrentDesktop=state.transition.currentOrigin;
    effect=AdvancePickerTransition(state,current);
    CHECK(effect.kind==PickerEffectKind::Refresh);
    CHECK(state.transition.observedTargetValidity==PickerReadValidity::Valid);
    CHECK(state.transition.observedPopupValidity==PickerReadValidity::Valid);
    CHECK(state.transition.observedCurrentValidity==PickerReadValidity::Valid);
}

static void test_picker_forward_api_ack_records_rollback_verification_need(){
    PickerState invoked=PickerTransitionFixture(2942);
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=invoked.transition.generation;
    PickerEffect move=AdvancePickerTransition(invoked,begin);
    CHECK(!invoked.transition.rollbackVerificationRequired);
    CHECK(PickerAckApi(invoked,move,true).kind==PickerEffectKind::ReadTarget);
    CHECK(invoked.transition.rollbackVerificationRequired);

    PickerState notInvoked=PickerTransitionFixture(2943);
    begin.generation=notInvoked.transition.generation;
    move=AdvancePickerTransition(notInvoked,begin);
    CHECK(PickerAckApi(notInvoked,move,false).kind==
          PickerEffectKind::Refresh);
    CHECK(!notInvoked.transition.rollbackVerificationRequired);
}

static void test_picker_identity_loss_matrix_never_touches_target_again(){
    {
        PickerState state=PickerTransitionFixture(295);
        state.transition.phase=PickerPhase::IdentityVerifyBeforeSwitch;
        state.transition.pendingEffect=PickerEffectKind::ValidateTarget;
        state.transition.effectSerial=101;
        state.transition.targetMayHaveMoved=true;
        state.transition.popupMayHaveMoved=true;
        PickerEffect validate;
        validate.kind=PickerEffectKind::ValidateTarget;
        validate.generation=295;
        validate.effectSerial=101;
        PickerEffect effect=PickerAckIdentity(
            state,validate,PickerIdentityValidity::Lost);
        CHECK(state.transition.targetIdentityUnusable);
        CHECK(state.transition.observedTargetValidity==
              PickerReadValidity::Unavailable);
        CHECK(effect.kind==PickerEffectKind::MovePopup);
        CHECK(effect.kind!=PickerEffectKind::MoveTarget);
        CHECK(effect.kind!=PickerEffectKind::SwitchDesktop);
        CHECK(effect.kind!=PickerEffectKind::SaveExactTarget);
    }
    {
        PickerState state=PickerTransitionFixture(296);
        state.transition.phase=PickerPhase::RollbackTargetVerify;
        state.transition.pendingEffect=PickerEffectKind::ReadTarget;
        state.transition.effectSerial=102;
        state.transition.targetMayHaveMoved=true;
        state.transition.popupMayHaveMoved=true;
        PickerEffect read;
        read.kind=PickerEffectKind::ReadTarget;
        read.generation=296;
        read.effectSerial=102;
        PickerEffect effect=PickerAckTarget(
            state,read,PickerReadValidity::Unavailable,GUID{},
            PickerIdentityValidity::Indeterminate);
        CHECK(state.transition.targetIdentityUnusable);
        CHECK(state.transition.observedTargetValidity==
              PickerReadValidity::Unavailable);
        CHECK(effect.kind==PickerEffectKind::MovePopup);
        CHECK(effect.kind!=PickerEffectKind::MoveTarget);
    }
    {
        PickerState state=PickerTransitionFixture(297);
        state.transition.phase=PickerPhase::SaveExactTarget;
        state.transition.commitCutoffReached=true;
        state.transition.pendingEffect=PickerEffectKind::SaveExactTarget;
        state.transition.effectSerial=103;
        PickerEffect save;
        save.kind=PickerEffectKind::SaveExactTarget;
        save.generation=297;
        save.effectSerial=103;
        PickerObservation lost=PickerObservationFor(
            save,PickerEvent::EffectCompleted);
        lost.identity=PickerIdentityValidity::Lost;
        lost.saveStatus=PopupSaveStatus::Failed;
        PickerEffect effect=AdvancePickerTransition(state,lost);
        CHECK(state.transition.targetIdentityUnusable);
        CHECK(state.transition.observedTargetValidity==
              PickerReadValidity::Unavailable);
        CHECK(effect.kind==PickerEffectKind::Refresh);
        CHECK(effect.kind!=PickerEffectKind::MoveTarget);
    }
}

static void test_picker_popup_and_switch_retry_boundaries_are_exact(){
    const GUID third=G(
        L"{231A0000-0000-0000-0000-000000000003}");
    struct ReadCase {
        PickerReadValidity validity;
        int actualKind;
        int attempts;
    };
    const ReadCase cases[]={
        {PickerReadValidity::Valid,0,1},
        {PickerReadValidity::Valid,0,4},
        {PickerReadValidity::Valid,1,1},
        {PickerReadValidity::Valid,1,4},
        {PickerReadValidity::Unavailable,2,1},
        {PickerReadValidity::Unavailable,2,4}
    };
    for(size_t index=0;index<6;++index){
        const ReadCase& test=cases[index];
        PickerState popupState=PickerTransitionFixture(300+index);
        popupState.transition.phase=PickerPhase::PopupVerify;
        popupState.transition.pendingEffect=PickerEffectKind::ReadPopup;
        popupState.transition.effectSerial=110;
        popupState.transition.forwardPopupAttempts=test.attempts;
        PickerEffect read;
        read.kind=PickerEffectKind::ReadPopup;
        read.generation=popupState.transition.generation;
        read.effectSerial=110;
        PickerObservation popup=PickerObservationFor(
            read,PickerEvent::ReadbackCompleted);
        popup.popupRead=test.validity;
        popup.actualPopupDesktop=test.actualKind==0
            ? popupState.transition.currentOrigin
            : test.actualKind==1 ? third : GUID{};
        PickerEffect popupNext=AdvancePickerTransition(popupState,popup);
        if(test.attempts<4)
            CHECK(popupNext.kind==PickerEffectKind::ValidateTarget);
        else if(test.actualKind==0)
            CHECK(popupNext.kind==PickerEffectKind::Refresh);
        else
            CHECK(popupNext.kind==PickerEffectKind::ReadTarget);

        PickerState switchState=PickerTransitionFixture(310+index);
        switchState.transition.phase=PickerPhase::DestinationVerify;
        switchState.transition.pendingEffect=PickerEffectKind::ReadCurrent;
        switchState.transition.effectSerial=111;
        switchState.transition.forwardSwitchAttempts=test.attempts;
        read.kind=PickerEffectKind::ReadCurrent;
        read.generation=switchState.transition.generation;
        read.effectSerial=111;
        PickerObservation current=PickerObservationFor(
            read,PickerEvent::ReadbackCompleted);
        current.currentRead=test.validity;
        current.actualCurrentDesktop=test.actualKind==0
            ? switchState.transition.currentOrigin
            : test.actualKind==1 ? third : GUID{};
        PickerEffect switchNext=AdvancePickerTransition(switchState,current);
        if(test.attempts<4)
            CHECK(switchNext.kind==PickerEffectKind::ValidateTarget);
        else if(test.actualKind==0)
            CHECK(switchNext.kind==PickerEffectKind::Refresh);
        else
            CHECK(switchNext.kind==PickerEffectKind::ReadTarget);
    }
}

static void test_picker_fourth_rollback_readback_and_focus_success_terminate(){
    {
        PickerState state=PickerTransitionFixture(320);
        state.transition.phase=PickerPhase::RollbackTargetVerify;
        state.transition.pendingEffect=PickerEffectKind::ReadTarget;
        state.transition.effectSerial=120;
        state.transition.rollbackTargetAttempts=4;
        state.transition.targetMayHaveMoved=true;
        PickerEffect read{PickerEffectKind::ReadTarget,320,120,GUID{}};
        PickerEffect effect=PickerAckTarget(
            state,read,PickerReadValidity::Valid,
            state.transition.targetOrigin);
        CHECK(!state.transition.targetMayHaveMoved);
        CHECK(effect.kind==PickerEffectKind::Refresh);
    }
    {
        PickerState state=PickerTransitionFixture(321);
        state.transition.phase=PickerPhase::RollbackPopupVerify;
        state.transition.pendingEffect=PickerEffectKind::ReadPopup;
        state.transition.effectSerial=121;
        state.transition.rollbackPopupAttempts=4;
        state.transition.popupMayHaveMoved=true;
        PickerEffect read{PickerEffectKind::ReadPopup,321,121,GUID{}};
        PickerObservation popup=PickerObservationFor(
            read,PickerEvent::ReadbackCompleted);
        popup.popupRead=PickerReadValidity::Valid;
        popup.actualPopupDesktop=state.transition.currentOrigin;
        PickerEffect effect=AdvancePickerTransition(state,popup);
        CHECK(!state.transition.popupMayHaveMoved);
        CHECK(effect.kind==PickerEffectKind::Refresh);
    }
    {
        PickerState state=PickerTransitionFixture(322);
        state.transition.phase=PickerPhase::OriginVerify;
        state.transition.pendingEffect=PickerEffectKind::ReadCurrent;
        state.transition.effectSerial=122;
        state.transition.rollbackSwitchAttempts=4;
        state.transition.switchMayHaveChanged=true;
        PickerEffect read{PickerEffectKind::ReadCurrent,322,122,GUID{}};
        PickerObservation current=PickerObservationFor(
            read,PickerEvent::ReadbackCompleted);
        current.currentRead=PickerReadValidity::Valid;
        current.actualCurrentDesktop=state.transition.currentOrigin;
        PickerEffect effect=AdvancePickerTransition(state,current);
        CHECK(!state.transition.switchMayHaveChanged);
        CHECK(effect.kind==PickerEffectKind::Refresh);
    }
    for(int attempt : {1,4}){
        PickerState state=PickerTransitionFixture(330+attempt);
        state.transition.phase=PickerPhase::FocusRestore;
        state.transition.pendingEffect=PickerEffectKind::ShowAndFocus;
        state.transition.effectSerial=123;
        state.transition.focusAttempts=attempt;
        PickerEffect focus{PickerEffectKind::ShowAndFocus,
                           state.transition.generation,123,GUID{}};
        PickerObservation foreground=PickerObservationFor(
            focus,PickerEvent::EffectCompleted);
        foreground.popupIsForeground=true;
        CHECK(AdvancePickerTransition(state,foreground).kind==
              PickerEffectKind::None);
        CHECK(state.transition.terminalAcknowledged);
        CHECK(FinalizePickerTransition(state));
    }
}

static MoveJob MJ(MoveOwner owner, uint64_t operationId,
                  uint64_t jobId, const char* runtimeKey){
    MoveJob job;
    job.token={owner,operationId,jobId,0};
    job.runtimeKey=runtimeKey;
    job.recordId="record-"+std::to_string(jobId);
    job.destination=G(L"{231A0000-0000-0000-0000-000000000001}");
    return job;
}

static void check_move_result(const MoveResult& result,MoveTerminal terminal,
                              const MoveJob& job,int attempts){
    CHECK(result.completed);
    CHECK(result.terminal==terminal);
    CHECK(result.attempts==attempts);
    CHECK(result.token.owner==job.token.owner);
    CHECK(result.token.operationId==job.token.operationId);
    CHECK(result.token.jobId==job.token.jobId);
    CHECK(result.token.itemIndex==job.token.itemIndex);
    CHECK(result.runtimeKey==job.runtimeKey);
    CHECK(result.recordId==job.recordId);
}

static void check_empty_move_result(const MoveResult& result){
    CHECK(!result.completed);
    CHECK(result.terminal==MoveTerminal::None);
    CHECK(result.attempts==0);
    CHECK(result.token.owner==MoveOwner::AutoReconcile);
    CHECK(result.token.operationId==0);
    CHECK(result.token.jobId==0);
    CHECK(result.token.itemIndex==0);
    CHECK(result.runtimeKey.empty());
    CHECK(result.recordId.empty());
}

static_assert(std::is_same<decltype(std::declval<MoveQueue&>().front()),
                           const MoveJob*>::value,
              "MoveQueue::front must expose read-only state");

static void test_move_queue_alternates_issue_verify_and_succeeds(){
    MoveQueue queue;
    CHECK(queue.empty());
    CHECK(queue.front()==nullptr);
    CHECK(queue.nextAction()==MoveAction::None);
    check_empty_move_result(queue.onIssued(MoveAttemptOutcome::Accepted));
    check_empty_move_result(queue.onVerified(MoveAttemptOutcome::OnDestination));
    check_empty_move_result(queue.cancelJob(77));

    MoveJob job=MJ(MoveOwner::Picker,101,1001,"picker-runtime");
    job.token.itemIndex=7;
    job.recordId="{00000000-0000-0000-0000-000000001001}";
    CHECK(queue.enqueue(job));
    CHECK(!queue.empty());
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1001);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult issued=queue.onIssued(MoveAttemptOutcome::Accepted);
    CHECK(!issued.completed);
    CHECK(issued.terminal==MoveTerminal::None);
    CHECK(issued.attempts==1);
    CHECK(issued.token.owner==MoveOwner::Picker);
    CHECK(issued.token.operationId==101);
    CHECK(issued.token.jobId==1001);
    CHECK(issued.token.itemIndex==7);
    CHECK(issued.runtimeKey==job.runtimeKey);
    CHECK(issued.recordId==job.recordId);
    CHECK(queue.front()!=nullptr && queue.front()->attempts==1);
    CHECK(queue.front()!=nullptr && queue.front()->waitingForVerify);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult verified=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(verified,MoveTerminal::Succeeded,job,1);
    CHECK(queue.empty());
    CHECK(queue.front()==nullptr);
    CHECK(queue.nextAction()==MoveAction::None);
}

static void test_move_queue_enqueue_validates_identity_state_and_copies_guid(){
    MoveQueue queue;
    MoveJob valid=MJ(MoveOwner::AutoReconcile,111,1101,"");
    valid.recordId.clear();
    GUID expectedDestination=valid.destination;
    CHECK(queue.enqueue(valid));
    valid.token.operationId=999;
    valid.token.jobId=999;
    valid.destination=GUID{};
    CHECK(queue.front()!=nullptr && queue.front()->token.operationId==111);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);
    CHECK(queue.front()!=nullptr && GuidEq(queue.front()->destination,expectedDestination));
    CHECK(queue.front()!=nullptr && queue.front()->runtimeKey.empty());
    CHECK(queue.front()!=nullptr && queue.front()->recordId.empty());

    MoveJob zeroOperation=MJ(MoveOwner::ManualTray,0,1102,"runtime");
    MoveJob zeroJob=MJ(MoveOwner::ManualTray,112,0,"runtime");
    MoveJob invalidOwner=MJ(static_cast<MoveOwner>(-1),112,1102,"runtime");
    MoveJob zeroDestination=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    zeroDestination.destination=GUID{};
    MoveJob attempted=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    attempted.attempts=1;
    MoveJob waiting=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    waiting.waitingForVerify=true;
    MoveJob duplicateId=MJ(MoveOwner::Picker,999,1101,"different-runtime");
    CHECK(!queue.enqueue(zeroOperation));
    CHECK(!queue.enqueue(zeroJob));
    CHECK(!queue.enqueue(invalidOwner));
    CHECK(!queue.enqueue(zeroDestination));
    CHECK(!queue.enqueue(attempted));
    CHECK(!queue.enqueue(waiting));
    CHECK(!queue.enqueue(duplicateId));
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);

    MoveResult cancelled=queue.cancelJob(1101);
    CHECK(cancelled.completed && cancelled.terminal==MoveTerminal::Cancelled);
    CHECK(queue.enqueue(duplicateId)); // uniqueness is required among live jobs
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);
}

static void test_move_queue_allows_bounded_auto_with_manual_and_picker_jobs(){
    MoveQueue queue;
    const size_t autoJobCount=4096;
    bool acceptedAllAuto=true;
    for(size_t i=0;i<autoJobCount;++i){
        MoveJob job=MJ(MoveOwner::AutoReconcile,151,1501+i,"");
        job.recordId.clear();
        if(!queue.enqueue(job)) acceptedAllAuto=false;
    }
    CHECK(acceptedAllAuto);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1501);

    MoveJob manual=MJ(MoveOwner::ManualTray,152,1000001,"shared-runtime");
    MoveJob picker=MJ(MoveOwner::Picker,153,1000002,"shared-runtime");
    const bool manualAccepted=queue.enqueue(manual);
    const bool pickerAccepted=queue.enqueue(picker);
    CHECK(manualAccepted);
    CHECK(pickerAccepted);
    if(manualAccepted){
        MoveResult cancelled=queue.cancelJob(manual.token.jobId);
        check_move_result(cancelled,MoveTerminal::Cancelled,manual,0);
    }
    if(pickerAccepted){
        MoveResult cancelled=queue.cancelJob(picker.token.jobId);
        check_move_result(cancelled,MoveTerminal::Cancelled,picker,0);
    }
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1501);
}

static void test_move_queue_phase_guards_and_issue_outcomes(){
    MoveQueue queue;
    MoveJob job=MJ(MoveOwner::AutoReconcile,121,1201,"phase-runtime");
    CHECK(queue.enqueue(job));
    CHECK(queue.nextAction()==MoveAction::Issue);
    CHECK(queue.nextAction()==MoveAction::Issue);
    check_empty_move_result(queue.onVerified(MoveAttemptOutcome::OnDestination));
    CHECK(queue.front()!=nullptr && queue.front()->attempts==0);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult issued=queue.onIssued(MoveAttemptOutcome::TransientFailure);
    CHECK(!issued.completed && issued.attempts==1);
    CHECK(queue.nextAction()==MoveAction::Verify);
    CHECK(queue.nextAction()==MoveAction::Verify);
    check_empty_move_result(queue.onIssued(MoveAttemptOutcome::Accepted));
    CHECK(queue.front()!=nullptr && queue.front()->attempts==1);
    CHECK(queue.front()!=nullptr && queue.front()->waitingForVerify);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult retry=queue.onVerified(MoveAttemptOutcome::TransientFailure);
    CHECK(!retry.completed && retry.attempts==1);
    CHECK(queue.nextAction()==MoveAction::Issue);
    MoveResult alreadyThere=queue.onIssued(MoveAttemptOutcome::OnDestination);
    check_move_result(alreadyThere,MoveTerminal::Succeeded,job,2);
    CHECK(queue.empty());
}

static void test_move_queue_four_transient_issues_still_receive_four_verifies(){
    MoveQueue queue;
    MoveJob job=MJ(MoveOwner::AutoReconcile,131,1301,"ambiguous-issue");
    CHECK(queue.enqueue(job));
    for(int attempt=1;attempt<=4;++attempt){
        CHECK(queue.nextAction()==MoveAction::Issue);
        MoveResult issued=queue.onIssued(MoveAttemptOutcome::TransientFailure);
        CHECK(!issued.completed && issued.attempts==attempt);
        CHECK(queue.nextAction()==MoveAction::Verify);
        MoveResult verified=queue.onVerified(MoveAttemptOutcome::TransientFailure);
        if(attempt<4){
            CHECK(!verified.completed && verified.attempts==attempt);
            CHECK(queue.nextAction()==MoveAction::Issue);
        }else{
            check_move_result(verified,MoveTerminal::Exhausted,job,4);
        }
    }
    CHECK(queue.empty());
}

static void test_move_queue_invalid_outcomes_fail_closed(){
    MoveQueue queue;
    MoveJob invalidIssue=MJ(MoveOwner::AutoReconcile,141,1401,"invalid-issue");
    MoveJob acceptedVerify=MJ(MoveOwner::ManualTray,142,1402,"accepted-verify");
    MoveJob invalidVerify=MJ(MoveOwner::Picker,143,1403,"invalid-verify");
    CHECK(queue.enqueue(invalidIssue));
    CHECK(queue.enqueue(acceptedVerify));
    CHECK(queue.enqueue(invalidVerify));

    MoveResult first=queue.onIssued(static_cast<MoveAttemptOutcome>(-1));
    check_move_result(first,MoveTerminal::PermanentFailure,invalidIssue,1);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult second=queue.onVerified(MoveAttemptOutcome::Accepted);
    check_move_result(second,MoveTerminal::PermanentFailure,acceptedVerify,1);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult third=queue.onVerified(static_cast<MoveAttemptOutcome>(99));
    check_move_result(third,MoveTerminal::PermanentFailure,invalidVerify,1);
    CHECK(queue.empty());
}

static void test_move_queue_four_transient_cycles_exhaust_and_unblock_next(){
    MoveQueue queue;
    MoveJob failed=MJ(MoveOwner::AutoReconcile,201,2001,"failed-runtime");
    failed.token.itemIndex=3;
    MoveJob next=MJ(MoveOwner::AutoReconcile,201,2002,"healthy-runtime");
    CHECK(queue.enqueue(failed));
    CHECK(queue.enqueue(next));

    for(int attempt=1;attempt<=4;++attempt){
        CHECK(queue.nextAction()==MoveAction::Issue);
        MoveResult issued=queue.onIssued(MoveAttemptOutcome::Accepted);
        CHECK(!issued.completed && issued.attempts==attempt);
        CHECK(queue.nextAction()==MoveAction::Verify);
        MoveResult verified=queue.onVerified(MoveAttemptOutcome::TransientFailure);
        if(attempt<4){
            CHECK(!verified.completed && verified.attempts==attempt);
            CHECK(queue.nextAction()==MoveAction::Issue);
        }else{
            check_move_result(verified,MoveTerminal::Exhausted,failed,4);
        }
    }

    CHECK(!queue.empty());
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==next.token.jobId);
    CHECK(queue.nextAction()==MoveAction::Issue);
    MoveResult nextIssued=queue.onIssued(MoveAttemptOutcome::Accepted);
    CHECK(!nextIssued.completed && nextIssued.attempts==1);
    MoveResult nextVerified=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(nextVerified,MoveTerminal::Succeeded,next,1);
    CHECK(queue.empty());
}

static void test_move_queue_permanent_failure_finishes_and_unblocks_next(){
    MoveQueue queue;
    MoveJob invalidDestination=MJ(MoveOwner::ManualTray,301,3001,"bad-destination");
    MoveJob invalidIdentity=MJ(MoveOwner::Picker,302,3002,"bad-identity");
    MoveJob healthy=MJ(MoveOwner::AutoReconcile,303,3003,"healthy");
    CHECK(queue.enqueue(invalidDestination));
    CHECK(queue.enqueue(invalidIdentity));
    CHECK(queue.enqueue(healthy));

    MoveResult first=queue.onIssued(MoveAttemptOutcome::PermanentFailure);
    check_move_result(first,MoveTerminal::PermanentFailure,invalidDestination,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==invalidIdentity.token.jobId);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult second=queue.onVerified(MoveAttemptOutcome::PermanentFailure);
    check_move_result(second,MoveTerminal::PermanentFailure,invalidIdentity,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==healthy.token.jobId);

    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult third=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(third,MoveTerminal::Succeeded,healthy,1);
    CHECK(queue.empty());
}

static void test_move_queue_cancel_job_is_identity_safe_during_verify(){
    MoveQueue queue;
    MoveJob automatic=MJ(MoveOwner::AutoReconcile,401,4001,"shared-runtime");
    MoveJob manual=MJ(MoveOwner::ManualTray,402,4002,"shared-runtime");
    CHECK(queue.enqueue(automatic));
    CHECK(queue.enqueue(manual));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult cancelled=queue.cancelJob(automatic.token.jobId);
    check_move_result(cancelled,MoveTerminal::Cancelled,automatic,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==manual.token.jobId);
    CHECK(queue.front()!=nullptr && queue.front()->attempts==0);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult duplicate=queue.cancelJob(automatic.token.jobId);
    CHECK(!duplicate.completed && duplicate.terminal==MoveTerminal::None);
    MoveResult manualCancelled=queue.cancelJob(manual.token.jobId);
    check_move_result(manualCancelled,MoveTerminal::Cancelled,manual,0);
    CHECK(queue.empty());
}

static void test_move_queue_cancel_operation_is_owner_scoped_and_fifo(){
    MoveQueue queue;
    MoveJob autoCurrent=MJ(MoveOwner::AutoReconcile,501,5001,"same-runtime");
    MoveJob manualSameOperation=MJ(MoveOwner::ManualTray,501,5002,"same-runtime");
    MoveJob autoLater=MJ(MoveOwner::AutoReconcile,501,5003,"another-runtime");
    MoveJob autoOtherOperation=MJ(MoveOwner::AutoReconcile,502,5004,"same-runtime");
    MoveJob pickerSameOperation=MJ(MoveOwner::Picker,501,5005,"same-runtime");
    CHECK(queue.enqueue(autoCurrent));
    CHECK(queue.enqueue(manualSameOperation));
    CHECK(queue.enqueue(autoLater));
    CHECK(queue.enqueue(autoOtherOperation));
    CHECK(queue.enqueue(pickerSameOperation));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    std::vector<MoveResult> cancelled=queue.cancelOperation(MoveOwner::AutoReconcile,501);
    CHECK(cancelled.size()==2);
    if(cancelled.size()==2){
        check_move_result(cancelled[0],MoveTerminal::Cancelled,autoCurrent,1);
        check_move_result(cancelled[1],MoveTerminal::Cancelled,autoLater,0);
    }
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==manualSameOperation.token.jobId);
    CHECK(queue.cancelOperation(MoveOwner::AutoReconcile,501).empty());

    MoveResult manual=queue.cancelJob(manualSameOperation.token.jobId);
    check_move_result(manual,MoveTerminal::Cancelled,manualSameOperation,0);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==autoOtherOperation.token.jobId);
    MoveResult other=queue.cancelJob(autoOtherOperation.token.jobId);
    check_move_result(other,MoveTerminal::Cancelled,autoOtherOperation,0);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==pickerSameOperation.token.jobId);
    MoveResult picker=queue.cancelJob(pickerSameOperation.token.jobId);
    check_move_result(picker,MoveTerminal::Cancelled,pickerSameOperation,0);
    CHECK(queue.empty());
}

struct FakeMoveOperationKey {
    MoveOwner owner=MoveOwner::AutoReconcile;
    uint64_t operationId=0;
    bool operator<(const FakeMoveOperationKey& other) const {
        if(owner!=other.owner) return static_cast<int>(owner)<static_cast<int>(other.owner);
        return operationId<other.operationId;
    }
};

struct FakeMoveOperationState {
    size_t outstanding=0;
    size_t delivered=0;
    std::set<uint64_t> liveJobIds;
};

struct FakeMoveOwnerDispatcher {
    std::map<FakeMoveOperationKey,FakeMoveOperationState> operations;
    bool dispatch(const MoveResult& result){
        if(!result.completed) return false;
        FakeMoveOperationKey key{result.token.owner,result.token.operationId};
        auto operation=operations.find(key);
        if(operation==operations.end()) return false;
        if(operation->second.liveJobIds.erase(result.token.jobId)!=1) return false;
        if(operation->second.outstanding==0) return false;
        --operation->second.outstanding;
        ++operation->second.delivered;
        return true;
    }
};

static void test_move_queue_duplicate_owner_delivery_is_harmless(){
    MoveQueue queue;
    MoveJob automatic=MJ(MoveOwner::AutoReconcile,601,6001,"shared-runtime");
    MoveJob manual=MJ(MoveOwner::ManualTray,601,6002,"shared-runtime");
    CHECK(queue.enqueue(automatic));
    CHECK(queue.enqueue(manual));

    FakeMoveOwnerDispatcher dispatcher;
    FakeMoveOperationKey automaticKey{MoveOwner::AutoReconcile,601};
    FakeMoveOperationKey manualKey{MoveOwner::ManualTray,601};
    dispatcher.operations[automaticKey].outstanding=1;
    dispatcher.operations[automaticKey].liveJobIds.insert(automatic.token.jobId);
    dispatcher.operations[manualKey].outstanding=1;
    dispatcher.operations[manualKey].liveJobIds.insert(manual.token.jobId);

    MoveResult cancelled=queue.cancelJob(automatic.token.jobId);
    CHECK(dispatcher.dispatch(cancelled));
    CHECK(!dispatcher.dispatch(cancelled));
    CHECK(dispatcher.operations[automaticKey].outstanding==0);
    CHECK(dispatcher.operations[automaticKey].delivered==1);
    CHECK(dispatcher.operations[manualKey].outstanding==1);
    CHECK(dispatcher.operations[manualKey].delivered==0);
    CHECK(dispatcher.operations[manualKey].liveJobIds.count(manual.token.jobId)==1);

    MoveResult manualCancelled=queue.cancelJob(manual.token.jobId);
    CHECK(dispatcher.dispatch(manualCancelled));
    CHECK(dispatcher.operations[manualKey].outstanding==0);
    CHECK(dispatcher.operations[manualKey].delivered==1);
}

static MoveResult TerminalMoveResult(const MoveToken& token,MoveTerminal terminal){
    MoveResult result;
    result.completed=true;
    result.terminal=terminal;
    result.token=token;
    return result;
}

static void test_move_operation_dispatcher_is_job_and_owner_scoped(){
    MoveOperationDispatcher dispatcher;
    MoveToken automaticA{MoveOwner::AutoReconcile,701,7001,0};
    MoveToken automaticB{MoveOwner::AutoReconcile,701,7002,1};
    MoveToken manual{MoveOwner::ManualTray,701,7003,0};
    CHECK(dispatcher.begin(MoveOwner::AutoReconcile,701,{automaticA,automaticB}));
    CHECK(dispatcher.begin(MoveOwner::ManualTray,701,{manual}));

    MoveOperationSummary completion;
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticA,MoveTerminal::Succeeded),completion)==
        MoveDispatchDisposition::Accepted);
    MoveOperationSummary partial;
    CHECK(dispatcher.lookup(MoveOwner::AutoReconcile,701,partial));
    CHECK(partial.outstanding==1 && partial.succeeded==1 && !partial.complete());

    MoveToken forged=automaticB;
    forged.owner=MoveOwner::ManualTray;
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(forged,MoveTerminal::Cancelled),completion)==
        MoveDispatchDisposition::Stale);
    CHECK(dispatcher.containsJob(automaticB.jobId));

    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticB,MoveTerminal::PermanentFailure),completion)==
        MoveDispatchDisposition::OperationCompleted);
    CHECK(completion.expected==2 && completion.outstanding==0);
    CHECK(completion.succeeded==1 && completion.permanentFailures==1);
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticB,MoveTerminal::PermanentFailure),completion)==
        MoveDispatchDisposition::Stale);

    CHECK(dispatcher.dispatch(
        TerminalMoveResult(manual,MoveTerminal::Succeeded),completion)==
        MoveDispatchDisposition::OperationCompleted);
    CHECK(completion.expected==1 && completion.succeeded==1);
}

static void test_move_operation_dispatcher_cancellation_completes_each_job_once(){
    MoveOperationDispatcher dispatcher;
    MoveToken first{MoveOwner::Picker,702,7011,0};
    MoveToken second{MoveOwner::Picker,702,7012,1};
    CHECK(dispatcher.begin(MoveOwner::Picker,702,{first,second}));
    std::vector<uint64_t> cancelledJobs;
    MoveOperationSummary completion;
    CHECK(dispatcher.cancelOperation(
        MoveOwner::Picker,702,cancelledJobs,completion));
    CHECK(cancelledJobs.size()==2);
    CHECK(cancelledJobs[0]==first.jobId && cancelledJobs[1]==second.jobId);
    CHECK(completion.complete() && completion.cancelled==2);
    CHECK(!dispatcher.cancelOperation(
        MoveOwner::Picker,702,cancelledJobs,completion));
    CHECK(cancelledJobs.empty());
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(first,MoveTerminal::Cancelled),completion)==
        MoveDispatchDisposition::Stale);

    MoveToken duplicateItemA{MoveOwner::AutoReconcile,703,7021,0};
    MoveToken duplicateItemB{MoveOwner::AutoReconcile,703,7022,0};
    CHECK(!dispatcher.begin(MoveOwner::AutoReconcile,703,
        {duplicateItemA,duplicateItemB}));
    std::vector<MoveToken> oversized;
    for(size_t i=0;i<4097;++i)
        oversized.push_back(MoveToken{
            MoveOwner::AutoReconcile,704,7100+i,i});
    CHECK(!dispatcher.begin(MoveOwner::AutoReconcile,704,oversized));
}

static void test_move_reservation_replacement_requires_exact_terminal_token(){
    MoveReservationBook reservations;
    WindowIdentityKey identity;
    identity.hwnd=0x701;
    identity.pid=1701;
    identity.processStart=2701;

    MoveReservation automatic;
    automatic.token=MoveToken{MoveOwner::AutoReconcile,801,8001,2};
    automatic.identity=identity;
    automatic.boundRecordId="{00000000-0000-0000-0000-000000008001}";
    automatic.hasProvisionalOriginRecord=true;
    automatic.provisionalOriginRecord.recordId=
        "{00000000-0000-0000-0000-000000008002}";
    automatic.provisionalOriginRecord.app="firefox";
    automatic.provisionalOriginRecord.desktop=
        G(L"{231A0000-0000-0000-0000-000000000801}");

    MoveReservation displaced;
    CHECK(reservations.reserve(automatic,&displaced)==
        MoveReservationUpdate::Inserted);
    MoveReservation stored;
    CHECK(reservations.lookup(identity,stored));
    CHECK(SameMoveToken(stored.token,automatic.token));
    CHECK(stored.identity.processStart==2701);
    CHECK(stored.boundRecordId==automatic.boundRecordId);
    CHECK(stored.hasProvisionalOriginRecord &&
        stored.provisionalOriginRecord.recordId==
            automatic.provisionalOriginRecord.recordId);

    MoveReservation manual=automatic;
    manual.token=MoveToken{MoveOwner::ManualTray,802,8002,0};
    manual.boundRecordId.clear();
    manual.hasProvisionalOriginRecord=false;
    CHECK(reservations.reserve(manual,&displaced)==
        MoveReservationUpdate::Replaced);
    CHECK(SameMoveToken(displaced.token,automatic.token));
    CHECK(reservations.size()==1);
    std::vector<MoveReservation> snapshot;
    CHECK(reservations.snapshot(snapshot));
    CHECK(snapshot.size()==1 && SameMoveToken(snapshot[0].token,manual.token));

    CHECK(!reservations.erase(identity,automatic.token));
    MoveToken wrongGeneration=manual.token;
    ++wrongGeneration.operationId;
    CHECK(!reservations.erase(identity,wrongGeneration));
    CHECK(reservations.lookup(identity,stored) &&
        SameMoveToken(stored.token,manual.token));
    CHECK(reservations.erase(identity,manual.token));
    CHECK(!reservations.erase(identity,manual.token));
    CHECK(reservations.empty());
}

static void test_issued_reservation_transfer_has_no_checkpoint_gap(){
    CheckpointController checkpoint;
    int heartbeatCalls=0;
    bool protectionVisible=false;
    auto heartbeat=[&](CheckpointReason reason){
        CHECK(reason==CheckpointReason::Heartbeat);
        CHECK(protectionVisible);
        ++heartbeatCalls;
        return true;
    };

    MoveReservationBook reservations;
    MoveReservation old;
    old.token.owner=MoveOwner::AutoReconcile;
    old.token.operationId=6101;
    old.token.jobId=6102;
    old.identity={0x6103,6104,6105};
    old.boundRecordId="{00000000-0000-0000-0000-000000006101}";
    old.originDesktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Inserted);
    protectionVisible=true;
    CHECK(checkpoint.dispatch(
        CheckpointReason::Heartbeat,true,true,true,heartbeat));
    CHECK(checkpoint.heartbeatDeferred && heartbeatCalls==0);

    MoveReservation replacement=old;
    replacement.token.owner=MoveOwner::ManualTray;
    replacement.token.operationId=6201;
    replacement.token.jobId=6202;
    MoveReservation displaced;
    CHECK(reservations.reserve(replacement,&displaced)==
          MoveReservationUpdate::Replaced);
    CHECK(SameMoveToken(displaced.token,old.token));
    CHECK(!reservations.erase(old.token));
    CHECK(checkpoint.acknowledgeReservationBeforeRelease(
        false,reservations.size()==1,true,true,heartbeat));
    CHECK(checkpoint.heartbeatDeferred && heartbeatCalls==0);

    // The successor completes with an exact readback.  Normal completion
    // publishes owner state first, erases the guard, then runs the heartbeat.
    CHECK(reservations.erase(replacement.token));
    protectionVisible=false;
    bool ownerStateSafe=true;
    CHECK(checkpoint.reservationTerminated(
        true,!reservations.empty(),true,true,[&](CheckpointReason reason){
            CHECK(reason==CheckpointReason::Heartbeat);
            CHECK(ownerStateSafe && !protectionVisible);
            ++heartbeatCalls;
            return true;
        }));
    CHECK(heartbeatCalls==1);
}

static void test_successor_handoff_publishes_before_issued_displaced_cancel(){
    MoveQueue queue;
    MoveJob displaced=MJ(MoveOwner::AutoReconcile,6151,6152,"same-runtime");
    MoveJob successor=MJ(MoveOwner::Picker,6161,6162,"same-runtime");
    CHECK(queue.enqueue(displaced));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveToken visibleGuard=displaced.token;
    bool displacedOwnerAlive=true,successorOwner=false,successorRuntime=false;
    bool successorQueued=false,displacedRetiring=false;
    int cancellationCalls=0,rollbackCalls=0;

    // Owner publication failure is entirely pre-commit.  The issued job,
    // owner, and old guard remain byte-for-byte intact.
    CHECK(!RunSuccessorFirstReservationHandoff([&](){
        successorRuntime=true;
        return false;
    },[&]() noexcept {
        visibleGuard=successor.token;
    },[&](){ ++cancellationCalls; },[&](){
        ++rollbackCalls;
        successorRuntime=false;
    }));
    CHECK(rollbackCalls==1 && cancellationCalls==0 && !successorRuntime);
    CHECK(displacedOwnerAlive && SameMoveToken(visibleGuard,displaced.token));
    CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token) &&
          queue.nextAction()==MoveAction::Verify);

    // A failed final enqueue has the same property: cancellation is not even
    // requested until the successor is wholly published.
    CHECK(!RunSuccessorFirstReservationHandoff([&](){
        successorRuntime=true;
        successorOwner=true;
        return false; // injected enqueue=false
    },[&]() noexcept {
        visibleGuard=successor.token;
    },[&](){ ++cancellationCalls; },[&](){
        ++rollbackCalls;
        successorOwner=false;
        successorRuntime=false;
    }));
    CHECK(rollbackCalls==2 && cancellationCalls==0 && !successorOwner &&
          !successorRuntime && SameMoveToken(visibleGuard,displaced.token));
    CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token) &&
          queue.nextAction()==MoveAction::Verify);

    std::vector<std::string> events;
    CHECK(RunSuccessorFirstReservationHandoff([&](){
        events.push_back("owner");
        successorOwner=true;
        successorRuntime=true;
        CHECK(queue.enqueue(successor));
        successorQueued=true;
        events.push_back("enqueue");
        return true;
    },[&]() noexcept {
        CHECK(successorOwner && successorRuntime && successorQueued);
        visibleGuard=successor.token;
        events.push_back("guard");
    },[&](){
        ++cancellationCalls;
        CHECK(SameMoveToken(visibleGuard,successor.token));
        CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token));
        CHECK(queue.nextAction()==MoveAction::Verify);
        displacedRetiring=true;
        events.push_back("cancel");
    },[&](){ ++rollbackCalls; }));
    CHECK((events==std::vector<std::string>{"owner","enqueue","guard","cancel"}));
    CHECK(cancellationCalls==1 && rollbackCalls==2 && displacedRetiring);
    CHECK(displacedOwnerAlive && successorOwner && successorRuntime &&
          successorQueued && SameMoveToken(visibleGuard,successor.token));
}

static void test_issued_reservation_rollback_waits_for_terminal_ack(){
    CHECK(MoveCancellationDispositionFor(false,true)==
          MoveCancellationDisposition::CancelImmediately);
    CHECK(MoveCancellationDispositionFor(true,false)==
          MoveCancellationDisposition::CancelImmediately);
    CHECK(MoveCancellationDispositionFor(true,true)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(true,WindowIdentityRecapture::Match)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(
              true,WindowIdentityRecapture::Indeterminate)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(true,WindowIdentityRecapture::Lost)==
          MoveCancellationDisposition::CancelImmediately);

    IdentityRecaptureRetryBudget unknownIdentity;
    for(unsigned check=1;
        check<IdentityRecaptureRetryBudget::kMaxIndeterminateChecks;++check)
        CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
              IdentityRecaptureRetryAction::Retry);
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
          IdentityRecaptureRetryAction::RetireCancelled);
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Match)==
          IdentityRecaptureRetryAction::Continue);
    unknownIdentity.reset();
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
          IdentityRecaptureRetryAction::Retry);

    MoveQueue unknownQueue;
    MoveJob unknownFirst,unknownSecond;
    unknownFirst.token={MoveOwner::ManualTray,6250,6251,0};
    unknownFirst.runtimeKey="unknown-first";
    unknownFirst.recordId="record-first";
    unknownFirst.destination=G(L"{231A0000-0000-0000-0000-000000000001}");
    unknownSecond=unknownFirst;
    unknownSecond.token.jobId=6252;
    unknownSecond.token.itemIndex=1;
    unknownSecond.runtimeKey="unknown-second";
    unknownSecond.recordId="record-second";
    CHECK(unknownQueue.enqueue(unknownFirst));
    CHECK(unknownQueue.enqueue(unknownSecond));
    IdentityRecaptureRetryBudget firstBudget;
    for(unsigned check=0;
        check<IdentityRecaptureRetryBudget::kMaxIndeterminateChecks;++check){
        const IdentityRecaptureRetryAction action=
            firstBudget.observe(WindowIdentityRecapture::Indeterminate);
        if(action==IdentityRecaptureRetryAction::RetireCancelled){
            MoveResult retiredUnknown=unknownQueue.cancelJob(
                unknownFirst.token.jobId);
            CHECK(retiredUnknown.completed &&
                  retiredUnknown.terminal==MoveTerminal::Cancelled);
        }
    }
    CHECK(unknownQueue.front() &&
          unknownQueue.front()->token.jobId==unknownSecond.token.jobId);

    CheckpointController checkpoint;
    MoveReservationBook reservations;
    MoveReservation old;
    old.token.owner=MoveOwner::AutoReconcile;
    old.token.operationId=6301;
    old.token.jobId=6302;
    old.identity={0x6303,6304,6305};
    old.boundRecordId="{00000000-0000-0000-0000-000000006301}";
    old.originDesktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    old.hasProvisionalOriginRecord=true;
    old.provisionalOriginRecord.recordId=old.boundRecordId;
    old.provisionalOriginRecord.app="firefox";
    old.provisionalOriginRecord.desktop=old.originDesktop;
    old.provisionalOriginRecord.lastSeenUtc=1700000000;
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Inserted);
    CHECK(checkpoint.dispatch(CheckpointReason::Heartbeat,true,true,true,
        [](CheckpointReason){ return false; }));

    MoveReservation replacement=old;
    replacement.token.owner=MoveOwner::Picker;
    replacement.token.operationId=6401;
    replacement.token.jobId=6402;
    CHECK(reservations.reserve(replacement)==MoveReservationUpdate::Replaced);
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Replaced);

    int heartbeatCalls=0;
    CHECK(checkpoint.acknowledgeReservationBeforeRelease(
        true,reservations.size()==1,true,true,[&](CheckpointReason){
            MoveReservation protectedOrigin;
            CHECK(reservations.lookup(old.identity,protectedOrigin));
            CHECK(protectedOrigin.hasProvisionalOriginRecord);
            CHECK(GuidEq(protectedOrigin.provisionalOriginRecord.desktop,
                         old.originDesktop));
            ++heartbeatCalls;
            return true;
        }));
    CHECK(heartbeatCalls==1 && reservations.erase(old.token));
    CHECK(!reservations.erase(old.token));

    // Failed/throwing checkpoint callbacks are consumed exactly once so the
    // terminal acknowledgement can always release its guard.
    for(int mode=0;mode!=2;++mode){
        CheckpointController failed;
        MoveReservationBook protectedReservations;
        CHECK(protectedReservations.reserve(old)==MoveReservationUpdate::Inserted);
        CHECK(failed.dispatch(CheckpointReason::Heartbeat,true,true,true,
            [](CheckpointReason){ return true; }));
        int calls=0;
        const bool saved=failed.acknowledgeReservationBeforeRelease(
            true,true,true,true,[&](CheckpointReason)->bool{
                ++calls;
                if(mode) throw std::runtime_error("injected checkpoint failure");
                return false;
            });
        CHECK(!saved && calls==1 && !failed.heartbeatDeferred);
        CHECK(protectedReservations.erase(old.token));
        CHECK(failed.acknowledgeReservationBeforeRelease(
            false,true,true,true,[&](CheckpointReason){ ++calls; return true; }));
        CHECK(calls==1);
    }

    MoveQueue queue;
    MoveReservationBook retiringReservations;
    CHECK(retiringReservations.reserve(old)==MoveReservationUpdate::Inserted);
    CheckpointController retiringCheckpoint;
    int retiringHeartbeatCalls=0;
    CHECK(retiringCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[&](CheckpointReason){
            ++retiringHeartbeatCalls;
            return true;
        }));
    MoveJob issued;
    issued.token=old.token;
    issued.runtimeKey=RuntimeKey(old.identity);
    issued.recordId=old.boundRecordId;
    issued.destination=old.originDesktop;
    CHECK(queue.enqueue(issued));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);
    IssuedMoveRetirementTracker retirement;
    for(unsigned check=1;
        check<IssuedMoveRetirementTracker::kMaxUnresolvedReadbacks;++check){
        CHECK(retirement.observe(true,MoveAttemptOutcome::TransientFailure)==
              IssuedMoveRetirementAction::WaitForReadback);
        CHECK(queue.nextAction()==MoveAction::Verify);
        CHECK(queue.front() && queue.front()->attempts==1);
        MoveReservation stillProtected;
        CHECK(retiringReservations.lookup(old.identity,stillProtected));
        CHECK(retiringHeartbeatCalls==0 &&
              retiringCheckpoint.heartbeatDeferred);
    }
    CHECK(retirement.observe(true,MoveAttemptOutcome::TransientFailure)==
          IssuedMoveRetirementAction::ConsumeProtectedCheckpointAndCancel);
    CHECK(retiringCheckpoint.acknowledgeReservationBeforeRelease(
        true,retiringReservations.size()==1,true,true,[&](CheckpointReason){
            MoveReservation protectedOrigin;
            CHECK(retiringReservations.lookup(old.identity,protectedOrigin));
            CHECK(GuidEq(protectedOrigin.originDesktop,old.originDesktop));
            ++retiringHeartbeatCalls;
            return true;
        }));
    MoveResult retired=queue.cancelJob(old.token.jobId);
    CHECK(retired.completed && retired.terminal==MoveTerminal::Cancelled);
    CHECK(retiringReservations.erase(old.token));
    CHECK(queue.empty() && retiringHeartbeatCalls==1);

    IssuedMoveRetirementTracker exact;
    CHECK(exact.observe(true,MoveAttemptOutcome::OnDestination)==
          IssuedMoveRetirementAction::CancelAfterSafeReadback);
    IssuedMoveRetirementTracker vanished;
    CHECK(vanished.observe(false,MoveAttemptOutcome::TransientFailure)==
          IssuedMoveRetirementAction::CancelAfterSafeReadback);

    // A timer-arm failure uses the same protected terminal path: the guard is
    // present during the single deferred checkpoint and always removed.
    MoveReservationBook timerReservations;
    CHECK(timerReservations.reserve(old)==MoveReservationUpdate::Inserted);
    CheckpointController timerCheckpoint;
    int timerHeartbeatCalls=0,timerCancelCalls=0;
    CHECK(timerCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[](CheckpointReason){ return true; }));
    CHECK(!ArmMoveWorkOrCancel(true,[](){ return false; },[&](){
        ++timerCancelCalls;
        CHECK(timerCheckpoint.acknowledgeReservationBeforeRelease(
            true,timerReservations.size()==1,true,true,[&](CheckpointReason){
                MoveReservation protectedOrigin;
                CHECK(timerReservations.lookup(old.identity,protectedOrigin));
                ++timerHeartbeatCalls;
                return true;
            }));
        CHECK(timerReservations.erase(old.token));
    }));
    CHECK(timerCancelCalls==1 && timerHeartbeatCalls==1 &&
          timerReservations.empty());

    // A failed terminal cannot drop its origin merely because an unrelated
    // reservation remains.  Consume the one deferred checkpoint while both
    // guards are visible; later terminals must not replay it.
    MoveReservation sibling=old;
    sibling.token.operationId=6501;
    sibling.token.jobId=6502;
    sibling.identity.hwnd=0x6503;
    MoveReservationBook multiple;
    CHECK(multiple.reserve(old)==MoveReservationUpdate::Inserted);
    CHECK(multiple.reserve(sibling)==MoveReservationUpdate::Inserted);
    CheckpointController multiCheckpoint;
    CHECK(multiCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[](CheckpointReason){ return true; }));
    int multiCalls=0;
    CHECK(multiCheckpoint.acknowledgeReservationBeforeRelease(
        true,false,true,true,[&](CheckpointReason){
            MoveReservation first,second;
            CHECK(multiple.lookup(old.identity,first));
            CHECK(multiple.lookup(sibling.identity,second));
            ++multiCalls;
            return true;
        }));
    CHECK(multiple.erase(old.token));
    CHECK(multiCheckpoint.reservationTerminated(
        false,true,true,true,[&](CheckpointReason){ ++multiCalls; return true; }));
    CHECK(multiple.erase(sibling.token));
    CHECK(multiCheckpoint.reservationTerminated(
        true,false,true,true,[&](CheckpointReason){ ++multiCalls; return true; }));
    CHECK(multiCalls==1);
}

static AsyncSessionRoute SessionRoute(uint64_t requestId,uint64_t operationId,
        const char* app,SessionPurpose purpose,uint64_t generation,
        uint64_t deadlineMs){
    AsyncSessionRoute route;
    route.requestId=requestId;
    route.operationId=operationId;
    route.app=app;
    route.purpose=purpose;
    route.identityGeneration=generation;
    route.deadlineMs=deadlineMs;
    return route;
}

static void test_async_session_route_protects_manual_work_and_retires_once(){
    AsyncSessionRouteGate routes;
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRoute save=SessionRoute(
        9001,901,"firefox",SessionPurpose::ManualSave,11,1000);
    CHECK(routes.submit(save,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(retired.empty() && routes.outstanding()==1);

    AsyncSessionRoute probe=SessionRoute(
        9002,902,"firefox",SessionPurpose::MetadataProbe,11,1000);
    CHECK(routes.submit(probe,100,retired)==
        AsyncRouteAdmission::RejectedProtected);
    CHECK(retired.size()==1 && retired[0].route.requestId==probe.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(routes.outstanding()==1);
    CHECK(routes.submit(probe,100,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty());

    AsyncSessionRoute restore=SessionRoute(
        9003,903,"firefox",SessionPurpose::ManualRestore,12,1200);
    CHECK(routes.submit(restore,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(retired.size()==1 && retired[0].route.requestId==save.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Superseded);
    CHECK(!routes.retire(save.requestId,save.operationId,
        save.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.empty());
    CHECK(routes.retire(restore.requestId,restore.operationId,
        restore.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.size()==1 && retired[0].reason==AsyncRetirementReason::Completed);
    CHECK(!routes.retire(restore.requestId,restore.operationId,
        restore.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.empty() && routes.outstanding()==0);
}

static void test_async_session_route_timeout_and_cancel_are_exact(){
    AsyncSessionRouteGate routes;
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRoute firefox=SessionRoute(
        9101,911,"firefox",SessionPurpose::AutoReconcile,21,200);
    AsyncSessionRoute chrome=SessionRoute(
        9102,911,"chrome",SessionPurpose::AutoReconcile,22,300);
    AsyncSessionRoute edgeSearch=SessionRoute(
        9103,911,"msedge",SessionPurpose::Search,23,400);
    CHECK(routes.submit(firefox,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.submit(chrome,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.submit(edgeSearch,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.expire(250,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==firefox.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::TimedOut);
    CHECK(routes.expire(250,retired)==0 && retired.empty());
    CHECK(routes.cancelOperation(
        SessionPurpose::AutoReconcile,911,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==chrome.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Cancelled);
    CHECK(routes.cancelOperation(
        SessionPurpose::AutoReconcile,911,retired)==0 && retired.empty());
    CHECK(routes.outstanding()==1);
    CHECK(routes.cancelOperation(SessionPurpose::Search,911,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==edgeSearch.requestId);
    CHECK(!routes.retire(chrome.requestId,chrome.operationId,
        chrome.identityGeneration,AsyncRetirementReason::Completed,retired));
}

static void test_async_session_route_bounds_deadlines_and_retires_capacity(){
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRouteGate deadlines;
    const uint64_t now=100;
    AsyncSessionRoute boundary=SessionRoute(
        9201,921,"firefox",SessionPurpose::MetadataProbe,31,
        now+AsyncSessionRouteGate::maxLifetimeMs());
    CHECK(deadlines.submit(boundary,now,retired)==AsyncRouteAdmission::Accepted);
    CHECK(deadlines.cancelOperation(
        SessionPurpose::MetadataProbe,921,retired)==1);
    AsyncSessionRoute farRoute=SessionRoute(
        9202,922,"firefox",SessionPurpose::MetadataProbe,31,
        (std::numeric_limits<uint64_t>::max)());
    CHECK(deadlines.submit(farRoute,now,retired)==
        AsyncRouteAdmission::RejectedDeadline);
    CHECK(retired.size()==1 && retired[0].route.requestId==farRoute.requestId &&
        retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(deadlines.submit(farRoute,now,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty());

    AsyncSessionRouteGate capacity;
    for(uint64_t i=0;i<16;++i){
        const std::string app="bounded-app-"+std::to_string(i);
        CHECK(capacity.submit(SessionRoute(
            9300+i,9400+i,app.c_str(),SessionPurpose::MetadataProbe,
            40+i,now+1000),now,retired)==AsyncRouteAdmission::Accepted);
    }
    AsyncSessionRoute overflow=SessionRoute(
        9316,9416,"bounded-app-overflow",SessionPurpose::MetadataProbe,
        56,now+1000);
    CHECK(capacity.submit(overflow,now,retired)==
        AsyncRouteAdmission::RejectedCapacity);
    CHECK(retired.size()==1 &&
        retired[0].route.requestId==overflow.requestId &&
        retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(capacity.submit(overflow,now,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty() && capacity.outstanding()==16);
}

static void test_dirty_flush_is_coalesced_bounded_and_retries_without_spin(){
    DirtyFlushController dirty;
    dirty.markDirty(100);
    CHECK(dirty.dirty() && dirty.dueAtMs()==600);
    dirty.markDirty(400);
    CHECK(dirty.dueAtMs()==600);
    int writes=0;
    CHECK(dirty.flush(599,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==0);
    CHECK(dirty.flush(600,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==1 && dirty.dirty() && dirty.dueAtMs()==1100);
    CHECK(dirty.flush(600,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==1);

    dirty.setConflict(true,1000);
    CHECK(dirty.flush(1100,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::ConflictSuppressed);
    CHECK(writes==1 && dirty.dirty());
    CHECK(dirty.flush(1100,true,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==2 && dirty.dirty() && dirty.conflicted());
    CHECK(dirty.flush(1200,true,[&]{ ++writes; return true; })==
        DirtyFlushResult::Succeeded);
    CHECK(writes==3 && !dirty.dirty() && !dirty.conflicted());
}

static void test_move_timer_failure_cancels_accepted_work_once(){
    CHECK(ShouldCancelMoveBeforeIssuedReadback(true,false,false));
    CHECK(ShouldCancelMoveBeforeIssuedReadback(true,false,true));
    CHECK(!ShouldCancelMoveBeforeIssuedReadback(true,true,true));
    CHECK(!ShouldCancelMoveBeforeIssuedReadback(false,true,true));

    // A failed timer arm may discover an already-issued front while a second
    // job is being queued.  Cancellation stays pending to drive the retry,
    // but it must not bypass the issued readback or release the exact guard.
    MoveQueue issuedFront;
    const MoveJob first=MJ(MoveOwner::AutoReconcile,9801,9802,"issued-first");
    const MoveJob second=MJ(MoveOwner::AutoReconcile,9803,9804,"queued-second");
    CHECK(issuedFront.enqueue(first));
    CHECK(issuedFront.enqueue(second));
    CHECK(!issuedFront.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(issuedFront.nextAction()==MoveAction::Verify);
    bool cancelRequested=true,retireAfterVerify=true,guardProtected=true;
    CHECK(!ShouldCancelMoveBeforeIssuedReadback(
        cancelRequested,retireAfterVerify,
        issuedFront.nextAction()==MoveAction::Verify));
    IssuedMoveRetirementTracker retirement;
    CHECK(retirement.observe(true,MoveAttemptOutcome::OnDestination)==
          IssuedMoveRetirementAction::CancelAfterSafeReadback);
    CHECK(guardProtected && issuedFront.front() &&
          issuedFront.front()->token.jobId==first.token.jobId);
    const MoveResult retired=issuedFront.cancelJob(first.token.jobId);
    CHECK(retired.completed && retired.terminal==MoveTerminal::Cancelled);
    CHECK(guardProtected);
    guardProtected=false;
    CHECK(!guardProtected && issuedFront.front() &&
          issuedFront.front()->token.jobId==second.token.jobId);

    int armCalls=0,cancelCalls=0;
    CHECK(!ArmMoveWorkOrCancel(true,[&](){ ++armCalls; return false; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==1 && cancelCalls==1);
    CHECK(ArmMoveWorkOrCancel(false,[&](){ ++armCalls; return false; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==1 && cancelCalls==1);
    CHECK(ArmMoveWorkOrCancel(true,[&](){ ++armCalls; return true; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==2 && cancelCalls==1);

    int queued=2,cancelAttempts=0,rearmCalls=0;
    bool injectThrow=true;
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        if(injectThrow){ injectThrow=false; throw std::bad_alloc(); }
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return true; })==
        MoveArmFailureCleanup::Rearmed);
    CHECK(queued==2 && cancelAttempts==1 && rearmCalls==1);
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return true; })==
        MoveArmFailureCleanup::Rearmed);
    CHECK(queued==1 && rearmCalls==2);
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return false; })==
        MoveArmFailureCleanup::Completed);
    CHECK(queued==0 && rearmCalls==2);

    CHECK(RecoverMoveArmFailure([](){ throw std::bad_alloc(); return false; },
        [](){ return false; })==MoveArmFailureCleanup::Unresolved);

    MoveToken retainedToken{MoveOwner::ManualTray,9901,9902,0};
    MoveOperationDispatcher retainedOwner;
    CHECK(retainedOwner.begin(MoveOwner::ManualTray,9901,{retainedToken}));
    CHECK(RecoverMoveArmFailure([](){
        throw std::bad_alloc();
        return false;
    },[](){ return true; })==MoveArmFailureCleanup::Rearmed);
    MoveOperationSummary retainedSummary;
    CHECK(retainedOwner.lookup(MoveOwner::ManualTray,9901,retainedSummary));
    CHECK(retainedSummary.outstanding==1 &&
          retainedOwner.containsJob(retainedToken.jobId));
    MoveResult exactCancellation;
    exactCancellation.completed=true;
    exactCancellation.terminal=MoveTerminal::Cancelled;
    exactCancellation.token=retainedToken;
    CHECK(retainedOwner.dispatch(exactCancellation,retainedSummary)==
          MoveDispatchDisposition::OperationCompleted);
    CHECK(!retainedOwner.lookup(MoveOwner::ManualTray,9901,retainedSummary));
}

static void test_move_cancellation_gate_precedes_fallible_cleanup(){
    bool cancellationPending=false;
    bool cancelRequested[3]={false,false,false};
    const uint64_t jobIds[3]={11001,11002,11003};
    bool threw=false;
    try {
        CHECK(PublishMoveCancellationIntent(
            cancellationPending,jobIds,3,[&](uint64_t jobId) noexcept {
                CHECK(jobId>=11001 && jobId<=11003);
                cancelRequested[jobId-11001]=true;
            }));
        throw std::bad_alloc(); // injected route/payload cleanup failure
    } catch(const std::bad_alloc&) { threw=true; }
    CHECK(threw && cancellationPending);
    CHECK(cancelRequested[0] && cancelRequested[1] && cancelRequested[2]);

    cancellationPending=false;
    CHECK(!PublishMoveCancellationIntent(
        cancellationPending,nullptr,1,[](uint64_t) noexcept {}));
    CHECK(!cancellationPending);
}

static void test_move_terminal_state_is_prepared_before_publication(){
    MoveTerminalOutcomes outcomes;
    CHECK(outcomes.initialize(3));
    CHECK(outcomes.size()==3 && !outcomes.succeeded(0));
    CHECK(outcomes.markSucceeded(1));
    CHECK(outcomes.succeeded(1) && !outcomes.succeeded(2));
    CHECK(!outcomes.markSucceeded(3));

    MoveToken token{MoveOwner::ManualTray,12001,12002,0};
    MoveResult output;
    output.token.jobId=999;
    int assignments=0;
    CHECK(!PrepareCancelledMoveResult(token,"runtime","record",output,
        [&](std::string& destination,const std::string& source){
            if(++assignments==2) throw std::bad_alloc();
            destination=source;
        }));
    CHECK(output.token.jobId==999); // transactional preparation
    CHECK(PrepareCancelledMoveResult(token,"runtime","record",output));
    CHECK(output.completed && output.terminal==MoveTerminal::Cancelled &&
          SameMoveToken(output.token,token) && output.runtimeKey=="runtime" &&
          output.recordId=="record");

    int failureCompletions=0;
    CHECK(!RunTerminalCompletionOrFail([&](){
        throw std::bad_alloc();
    },[&]() noexcept { ++failureCompletions; }));
    CHECK(failureCompletions==1);
    CHECK(!RunTerminalCompletionOrFail([&](){
        throw std::length_error("injected terminal commit fault");
    },[&](){ ++failureCompletions; throw std::bad_alloc(); }));
    CHECK(failureCompletions==2);
}

static void test_move_setup_rolls_back_provisional_and_queue_state(){
    std::map<std::string,std::string> provisional;
    std::set<uint64_t> runtimes,reservations,queued;
    int rollbacks=0;
    auto rollback=[&]{
        ++rollbacks;
        provisional.erase("runtime");
        runtimes.erase(77);
        reservations.erase(77);
        queued.erase(77);
    };
    CHECK(!RunFailureAtomicMoveSetup([&]{
        provisional["runtime"]="record";
        runtimes.insert(77);
        reservations.insert(77);
        queued.insert(77);
        return false;
    },rollback));
    CHECK(rollbacks==1 && provisional.empty() && runtimes.empty() &&
          reservations.empty() && queued.empty());

    CHECK(!RunFailureAtomicMoveSetup([&]()->bool{
        provisional["runtime"]="record";
        runtimes.insert(77);
        throw std::bad_alloc();
    },rollback));
    CHECK(rollbacks==2 && provisional.empty() && runtimes.empty());

    CHECK(RunFailureAtomicMoveSetup([&]{
        provisional["runtime"]="record";
        runtimes.insert(77);
        reservations.insert(77);
        queued.insert(77);
        return true;
    },rollback));
    CHECK(rollbacks==2 && provisional.size()==1 && runtimes.count(77)==1 &&
          reservations.count(77)==1 && queued.count(77)==1);

    // Owner membership is staged before queue publication.  If the final
    // enqueue throws, rollback never needs the allocating cancel primitive.
    bool runtimePublished=false,reservationPublished=false,ownerPublished=false;
    bool queuePublished=false;
    int queueCancelCalls=0;
    CHECK(!RunFailureAtomicMoveSetup([&]()->bool{
        runtimePublished=true;
        reservationPublished=true;
        ownerPublished=true;
        throw std::bad_alloc(); // injected final enqueue failure
    },[&](){
        if(queuePublished) ++queueCancelCalls;
        ownerPublished=false;
        reservationPublished=false;
        runtimePublished=false;
    }));
    CHECK(!runtimePublished && !reservationPublished && !ownerPublished &&
          !queuePublished && queueCancelCalls==0);
}

static void test_unbound_manual_reservation_uses_provisional_origin_id(){
    LayoutWin origin;
    origin.recordId="{00000000-0000-0000-0000-000000000771}";
    origin.app="firefox";
    origin.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    origin.lastSeenUtc=1700000000;
    std::string reservationId=
        "{00000000-0000-0000-0000-000000000772}";
    CHECK(BindReservationToProvisionalOrigin(origin,reservationId));
    CHECK(reservationId==origin.recordId);

    LayoutWin invalid=origin;
    invalid.recordId.clear();
    const std::string before=reservationId;
    CHECK(!BindReservationToProvisionalOrigin(invalid,reservationId));
    CHECK(reservationId==before);
}

static void test_auto_restore_failure_never_completes_as_success(){
    CHECK(AutoRestoreCompletionOutcome(false,false)==
          LcRestoreOutcome::Success);
    CHECK(AutoRestoreCompletionOutcome(true,false)==
          LcRestoreOutcome::Exhausted);
    CHECK(AutoRestoreCompletionOutcome(false,true)==
          LcRestoreOutcome::Exhausted);
    CHECK(AutoRestoreCompletionOutcome(true,true)==
          LcRestoreOutcome::Exhausted);
}

static void test_identity_guard_recaptures_immediately_before_issue_or_verify(){
    int captures=0,actions=0;
    HRESULT result=E_UNEXPECTED;
    CHECK(!RunIdentityGuardedComCall([&](){ ++captures; return false; },
        [&](){ ++actions; return S_OK; },result));
    CHECK(captures==1 && actions==0 && result==E_UNEXPECTED);
    CHECK(RunIdentityGuardedComCall([&](){ ++captures; return true; },
        [&](){ ++actions; return S_FALSE; },result));
    CHECK(captures==2 && actions==1 && result==S_FALSE);
}

static void test_fast_window_publication_requires_exact_final_identity(){
    CHECK(FinalFastWindowIdentityCanPublish(
        WindowIdentityRecapture::Match));
    CHECK(!FinalFastWindowIdentityCanPublish(
        WindowIdentityRecapture::Lost));
    CHECK(!FinalFastWindowIdentityCanPublish(
        WindowIdentityRecapture::Indeterminate));
}

static void test_fast_window_identity_failure_invalidates_all_enabled_profiles(){
    for(const WindowIdentityRecapture recapture : {
            WindowIdentityRecapture::Lost,
            WindowIdentityRecapture::Indeterminate}){
        CHECK(FinalFastWindowIdentityFailureInvalidatesEveryProfile(
            recapture));
    }
    CHECK(!FinalFastWindowIdentityFailureInvalidatesEveryProfile(
        WindowIdentityRecapture::Match));

    const std::vector<AppProfile> profiles=BuiltinProfiles(true,true,true);
    std::map<std::string,AppFastSnapshot> snapshots;
    snapshots["firefox"];
    snapshots["chrome"];
    snapshots["msedge"];
    MarkFastSnapshotCaptureIncomplete(profiles,snapshots);
    CHECK(!snapshots["firefox"].enumerationComplete);
    CHECK(!snapshots["chrome"].enumerationComplete);
    CHECK(!snapshots["msedge"].enumerationComplete);
}

static void test_desktop_services_require_documented_manager(){
    CHECK(DesktopServicesReady(true,true,true));
    CHECK(!DesktopServicesReady(true,true,false));
    CHECK(!DesktopServicesReady(true,false,true));
    CHECK(!DesktopServicesReady(false,true,true));
}

static void test_failed_com_out_pointer_is_released(){
    int releases=0;
    CHECK(!ValidateComOutPointerOrRelease(
        E_FAIL,true,[&](){ ++releases; }));
    CHECK(releases==1);
    CHECK(!ValidateComOutPointerOrRelease(
        E_FAIL,false,[&](){ ++releases; }));
    CHECK(!ValidateComOutPointerOrRelease(
        S_OK,false,[&](){ ++releases; }));
    CHECK(releases==1);
    CHECK(ValidateComOutPointerOrRelease(
        S_OK,true,[&](){ ++releases; }));
    CHECK(releases==1);
}

struct FakeDesktopLookupDesktop {
    GUID id={0};
    HRESULT idResult=S_OK;
    int releases=0;
};

struct FakeDesktopLookupArray {
    int releases=0;
};

struct FakeDesktopLookupOps {
    FakeDesktopLookupArray array;
    HRESULT desktopsResult=S_OK;
    bool returnArray=true;
    HRESULT countResult=S_OK;
    UINT count=0;
    std::vector<FakeDesktopLookupDesktop*> desktops;
    std::vector<HRESULT> atResults;
    bool returnDesktopOnFailedGetAt=false;
    int getCountCalls=0;
    int getAtCalls=0;
    int getIdCalls=0;

    HRESULT getDesktops(FakeDesktopLookupArray** output){
        if(returnArray) *output=&array;
        return desktopsResult;
    }

    HRESULT getCount(FakeDesktopLookupArray*,UINT* output){
        ++getCountCalls;
        *output=count;
        return countResult;
    }

    HRESULT getAt(FakeDesktopLookupArray*,UINT index,
                  FakeDesktopLookupDesktop** output){
        ++getAtCalls;
        const HRESULT result=index<atResults.size()
            ? atResults[index] : E_FAIL;
        FakeDesktopLookupDesktop* desktop=index<desktops.size()
            ? desktops[index] : nullptr;
        if(desktop && (SUCCEEDED(result) || returnDesktopOnFailedGetAt))
            *output=desktop;
        return result;
    }

    HRESULT getId(FakeDesktopLookupDesktop* desktop,GUID* output){
        ++getIdCalls;
        *output=desktop->id;
        return desktop->idResult;
    }

    void releaseArray(FakeDesktopLookupArray* value){
        ++value->releases;
    }

    void releaseDesktop(FakeDesktopLookupDesktop* value){
        ++value->releases;
    }
};

struct FakeDesktopLookupOwner {
    explicit FakeDesktopLookupOwner(FakeDesktopLookupOps& operations)
        :ops(&operations){}
    ~FakeDesktopLookupOwner(){ reset(); }
    FakeDesktopLookupOwner(const FakeDesktopLookupOwner&)=delete;
    FakeDesktopLookupOwner& operator=(const FakeDesktopLookupOwner&)=delete;

    void reset(FakeDesktopLookupDesktop* replacement=nullptr) noexcept {
        if(value) ops->releaseDesktop(value);
        value=replacement;
    }

    FakeDesktopLookupOps* ops=nullptr;
    FakeDesktopLookupDesktop* value=nullptr;
};

static bool RunFakeDesktopLookup(
        const DesktopCollectionLookupRequest& request,
        FakeDesktopLookupOps& ops,FakeDesktopLookupOwner& output,
        int& index){
    return LookupDesktopCollectionOwned<
        FakeDesktopLookupArray,FakeDesktopLookupDesktop>(
            request,ops,output,index);
}

static bool RunFakeDesktopSnapshot(
        FakeDesktopLookupOps& ops,
        std::vector<DesktopCollectionEntry>& output){
    return SnapshotDesktopCollectionOwned<
        FakeDesktopLookupArray,FakeDesktopLookupDesktop>(ops,output);
}

static void test_desktop_lookup_releases_failed_getdesktops_output(){
    FakeDesktopLookupOps ops;
    ops.desktopsResult=E_FAIL;
    ops.count=1;
    FakeDesktopLookupOwner output(ops);
    int index=17;
    CHECK(!RunFakeDesktopLookup(
        DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
    CHECK(ops.array.releases==1);
    CHECK(ops.getCountCalls==0);
    CHECK(output.value==nullptr && index==-1);
}

static void test_desktop_lookup_rejects_failed_or_oversized_count(){
    {
        FakeDesktopLookupOps ops;
        ops.countResult=E_FAIL;
        ops.count=1;
        FakeDesktopLookupOwner output(ops);
        int index=17;
        CHECK(!RunFakeDesktopLookup(
            DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
        CHECK(ops.array.releases==1);
        CHECK(ops.getCountCalls==1 && ops.getAtCalls==0);
        CHECK(output.value==nullptr && index==-1);
    }
    {
        FakeDesktopLookupOps ops;
        ops.count=65;
        FakeDesktopLookupOwner output(ops);
        int index=17;
        CHECK(!RunFakeDesktopLookup(
            DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
        CHECK(ops.array.releases==1);
        CHECK(ops.getCountCalls==1 && ops.getAtCalls==0);
        CHECK(output.value==nullptr && index==-1);
    }
    {
        FakeDesktopLookupDesktop desktop;
        desktop.id=G(L"{231A0000-0000-0000-0000-000000000070}");
        FakeDesktopLookupOps ops;
        ops.count=MAX_VIRTUAL_DESKTOPS;
        ops.desktops.resize(MAX_VIRTUAL_DESKTOPS,&desktop);
        ops.atResults.resize(MAX_VIRTUAL_DESKTOPS,S_OK);
        int index=-1;
        {
            FakeDesktopLookupOwner output(ops);
            CHECK(RunFakeDesktopLookup(
                DesktopCollectionLookupRequest::ByIndex(
                    MAX_VIRTUAL_DESKTOPS-1),ops,output,index));
            CHECK(index==static_cast<int>(MAX_VIRTUAL_DESKTOPS-1) &&
                  output.value==&desktop);
        }
        CHECK(ops.array.releases==1 && desktop.releases==1);
    }
}

static void test_desktop_lookup_releases_failed_getat_output(){
    FakeDesktopLookupDesktop desktop;
    desktop.id=G(L"{231A0000-0000-0000-0000-000000000071}");
    FakeDesktopLookupOps ops;
    ops.count=1;
    ops.desktops.push_back(&desktop);
    ops.atResults.push_back(E_FAIL);
    ops.returnDesktopOnFailedGetAt=true;
    FakeDesktopLookupOwner output(ops);
    int index=17;
    CHECK(!RunFakeDesktopLookup(
        DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
    CHECK(ops.array.releases==1 && desktop.releases==1);
    CHECK(ops.getAtCalls==1 && ops.getIdCalls==0);
    CHECK(output.value==nullptr && index==-1);
}

static void test_desktop_lookup_rejects_failed_or_zero_getid(){
    {
        FakeDesktopLookupDesktop desktop;
        desktop.id=G(L"{231A0000-0000-0000-0000-000000000072}");
        desktop.idResult=E_FAIL;
        FakeDesktopLookupOps ops;
        ops.count=1;
        ops.desktops.push_back(&desktop);
        ops.atResults.push_back(S_OK);
        FakeDesktopLookupOwner output(ops);
        int index=17;
        CHECK(!RunFakeDesktopLookup(
            DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
        CHECK(ops.array.releases==1 && desktop.releases==1);
        CHECK(output.value==nullptr && index==-1);
    }
    {
        FakeDesktopLookupDesktop desktop;
        FakeDesktopLookupOps ops;
        ops.count=1;
        ops.desktops.push_back(&desktop);
        ops.atResults.push_back(S_OK);
        FakeDesktopLookupOwner output(ops);
        int index=17;
        CHECK(!RunFakeDesktopLookup(
            DesktopCollectionLookupRequest::ByIndex(0),ops,output,index));
        CHECK(ops.array.releases==1 && desktop.releases==1);
        CHECK(output.value==nullptr && index==-1);
    }
}

static void test_desktop_lookup_returns_only_valid_owned_matches(){
    const GUID firstId=G(L"{231A0000-0000-0000-0000-000000000073}");
    const GUID secondId=G(L"{231A0000-0000-0000-0000-000000000074}");
    {
        FakeDesktopLookupDesktop first,second;
        first.id=firstId;
        second.id=secondId;
        FakeDesktopLookupOps ops;
        ops.count=2;
        ops.desktops={&first,&second};
        ops.atResults={S_OK,S_OK};
        int index=-1;
        {
            FakeDesktopLookupOwner output(ops);
            CHECK(RunFakeDesktopLookup(
                DesktopCollectionLookupRequest::ByIndex(1),
                ops,output,index));
            CHECK(output.value==&second && index==1);
            CHECK(first.releases==0 && second.releases==0);
        }
        CHECK(ops.array.releases==1 && second.releases==1);
    }
    {
        FakeDesktopLookupDesktop first,second;
        first.id=firstId;
        second.id=secondId;
        FakeDesktopLookupOps ops;
        ops.count=2;
        ops.desktops={&first,&second};
        ops.atResults={S_OK,S_OK};
        int index=-1;
        {
            FakeDesktopLookupOwner output(ops);
            CHECK(RunFakeDesktopLookup(
                DesktopCollectionLookupRequest::ByGuid(secondId),
                ops,output,index));
            CHECK(output.value==&second && index==1);
            CHECK(first.releases==1 && second.releases==0);
        }
        CHECK(ops.array.releases==1 && second.releases==1);
    }
    {
        FakeDesktopLookupDesktop first;
        first.id=firstId;
        FakeDesktopLookupOps ops;
        ops.count=1;
        ops.desktops={&first};
        ops.atResults={S_OK};
        FakeDesktopLookupOwner output(ops);
        int index=0;
        CHECK(!RunFakeDesktopLookup(
            DesktopCollectionLookupRequest::ByGuid(secondId),
            ops,output,index));
        CHECK(output.value==nullptr && index==-1);
        CHECK(ops.array.releases==1 && first.releases==1);
    }
}

static void test_desktop_snapshot_rechecks_count_after_prior_success(){
    FakeDesktopLookupDesktop desktop;
    desktop.id=G(L"{231A0000-0000-0000-0000-000000000075}");
    FakeDesktopLookupOps ops;
    ops.count=1;
    ops.desktops={&desktop};
    ops.atResults={S_OK};
    std::vector<DesktopCollectionEntry> snapshot;
    CHECK(RunFakeDesktopSnapshot(ops,snapshot));
    CHECK(snapshot.size()==1 && snapshot[0].index==0 &&
          GuidEq(snapshot[0].guid,desktop.id));

    ops.countResult=E_FAIL;
    CHECK(!RunFakeDesktopSnapshot(ops,snapshot));
    CHECK(snapshot.size()==1 && GuidEq(snapshot[0].guid,desktop.id));
    CHECK(ops.getCountCalls==2 && ops.getAtCalls==1);
    CHECK(ops.array.releases==2 && desktop.releases==1);

    FakeDesktopLookupOps oversized;
    oversized.count=UINT_MAX;
    std::vector<DesktopCollectionEntry> sentinel={{7,desktop.id}};
    CHECK(!RunFakeDesktopSnapshot(oversized,sentinel));
    CHECK(sentinel.size()==1 && sentinel[0].index==7 &&
          GuidEq(sentinel[0].guid,desktop.id));
    CHECK(oversized.getAtCalls==0 && oversized.array.releases==1);
}

static void test_desktop_snapshot_fails_atomically_on_collection_errors(){
    const GUID sentinelId=
        G(L"{231A0000-0000-0000-0000-000000000076}");
    const auto unchanged=[&](const std::vector<DesktopCollectionEntry>& value){
        return value.size()==1 && value[0].index==9 &&
            GuidEq(value[0].guid,sentinelId);
    };
    {
        FakeDesktopLookupOps ops;
        ops.desktopsResult=E_FAIL;
        std::vector<DesktopCollectionEntry> output={{9,sentinelId}};
        CHECK(!RunFakeDesktopSnapshot(ops,output));
        CHECK(unchanged(output) && ops.array.releases==1);
    }
    {
        FakeDesktopLookupDesktop desktop;
        desktop.id=sentinelId;
        FakeDesktopLookupOps ops;
        ops.count=1;
        ops.desktops={&desktop};
        ops.atResults={E_FAIL};
        ops.returnDesktopOnFailedGetAt=true;
        std::vector<DesktopCollectionEntry> output={{9,sentinelId}};
        CHECK(!RunFakeDesktopSnapshot(ops,output));
        CHECK(unchanged(output));
        CHECK(ops.array.releases==1 && desktop.releases==1);
    }
    {
        FakeDesktopLookupDesktop first,second;
        first.id=G(L"{231A0000-0000-0000-0000-000000000077}");
        second.id=G(L"{231A0000-0000-0000-0000-000000000078}");
        second.idResult=E_FAIL;
        FakeDesktopLookupOps ops;
        ops.count=2;
        ops.desktops={&first,&second};
        ops.atResults={S_OK,S_OK};
        std::vector<DesktopCollectionEntry> output={{9,sentinelId}};
        CHECK(!RunFakeDesktopSnapshot(ops,output));
        CHECK(unchanged(output));
        CHECK(ops.array.releases==1 && first.releases==1 &&
              second.releases==1);
    }
}

static void test_service_initialization_releases_every_failed_partial_state(){
    int initialized=0,sanityChecks=0,releases=0;
    CHECK(!InitializeServicesWithRollback([&]{ ++initialized; return false; },
        [&]{ ++sanityChecks; return true; },[&]{ ++releases; }));
    CHECK(initialized==1 && sanityChecks==0 && releases==1);
    CHECK(!InitializeServicesWithRollback([&]{ ++initialized; return true; },
        [&]{ ++sanityChecks; return false; },[&]{ ++releases; }));
    CHECK(initialized==2 && sanityChecks==1 && releases==2);
    CHECK(InitializeServicesWithRollback([&]{ ++initialized; return true; },
        [&]{ ++sanityChecks; return true; },[&]{ ++releases; }));
    CHECK(initialized==3 && sanityChecks==2 && releases==2);
    CHECK(!InitializeServicesWithRollback([&]()->bool{ throw std::bad_alloc(); },
        [&]{ return true; },[&]{ ++releases; }));
    CHECK(releases==3);
}

static void test_reconcile_deadline_retires_dropped_operation_exactly_once(){
    AsyncReconcileDeadlineGate deadlines;
    CHECK(deadlines.begin(101,1000));
    CHECK(deadlines.begin(101,1300));
    CHECK(deadlines.pending(101)==2);
    CHECK(deadlines.dueAt(101)==
          1000+AsyncReconcileDeadlineGate::maxLifetimeMs());
    std::vector<uint64_t> expired;
    CHECK(deadlines.expire(
        1000+AsyncReconcileDeadlineGate::maxLifetimeMs()-1,expired)==0);
    CHECK(deadlines.complete(101) && deadlines.pending(101)==1);
    CHECK(deadlines.expire(
        1000+AsyncReconcileDeadlineGate::maxLifetimeMs(),expired)==1);
    CHECK(expired.size()==1 && expired[0]==101 && deadlines.empty());
    CHECK(!deadlines.complete(101));

    CHECK(deadlines.begin(102,2000));
    CHECK(deadlines.cancel(102));
    CHECK(deadlines.expire(UINT64_MAX,expired)==0 && expired.empty());
}

static void test_dirty_flush_preserves_mutation_during_write_and_limits_errors(){
    DirtyFlushController dirty;
    dirty.markDirty(0);
    CHECK(dirty.flush(500,false,[&]{
        dirty.markDirty(500);
        return true;
    })==DirtyFlushResult::SucceededDirtyAgain);
    CHECK(dirty.dirty() && dirty.dueAtMs()==1000);

    CHECK(dirty.shouldReportError("layout locked",10));
    CHECK(!dirty.shouldReportError("layout locked",300009));
    CHECK(dirty.shouldReportError("layout locked",300010));
    CHECK(dirty.shouldReportError("different",300011));
    dirty.markDirty(600);
    CHECK(dirty.shouldReportError("different",300012));

    DirtyFlushController rollbackErrors;
    CHECK(rollbackErrors.shouldReportError("same",100));
    CHECK(!rollbackErrors.shouldReportError("same",90));
    CHECK(!rollbackErrors.shouldReportError("same",89));
    CHECK(rollbackErrors.shouldReportError("same",300089));

    DirtyFlushController disabledDuringWrite;
    disabledDuringWrite.markDirty(0);
    DirtyFlushResult nested=DirtyFlushResult::Failed;
    int nestedWrites=0;
    CHECK(disabledDuringWrite.flush(500,false,[&]{
        disabledDuringWrite.clearDirty();
        nested=disabledDuringWrite.flush(500,true,[&]{
            ++nestedWrites;
            return true;
        });
        return false;
    })==DirtyFlushResult::Cleared);
    CHECK(nested==DirtyFlushResult::Deferred && nestedWrites==0);
    CHECK(!disabledDuringWrite.dirty());
}

static void test_dirty_flush_clock_ceiling_never_spins(){
    DirtyFlushController dirty;
    const uint64_t ceiling=(std::numeric_limits<uint64_t>::max)();
    int writes=0;
    dirty.markDirty(ceiling);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==1);
    CHECK(dirty.flush(ceiling,true,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==2);
    dirty.markDirty(ceiling);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==3);

    const uint64_t rolledBack=ceiling-1000;
    CHECK(dirty.flush(rolledBack,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(dirty.dueAtMs()==rolledBack+500 && writes==3);
    CHECK(dirty.flush(rolledBack+500,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Succeeded);
    CHECK(writes==4 && !dirty.dirty());

    DirtyFlushController saturated;
    int saturatedWrites=0;
    saturated.markDirty(ceiling-600);
    CHECK(saturated.flush(ceiling-100,false,[&]{
        ++saturatedWrites;
        return false;
    })==DirtyFlushResult::Failed);
    CHECK(saturated.flush(ceiling,false,[&]{
        ++saturatedWrites;
        return true;
    })==DirtyFlushResult::Deferred);
    CHECK(saturatedWrites==1);
}

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
    CHECK(w.size()==1); CHECK(w[0].app=="firefox");
    CHECK(w[0].lastSeenUtc==1800000000);
    CHECK(w[0].tabCount==5); CHECK(w[0].counts["docs.python.org"]==1);
}

static std::string V4Line(const char* guid, const char* recordId, const char* lastSeen, const char* missing){
    return std::string("W\tfirefox\t") + recordId + "\t0\t" + guid + "\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t" + lastSeen + "\t" + missing + "\n";
}

static LayoutWin Identified(const char* recordId,const char* app,int deskIndex,
        const std::map<std::string,int>& counts,const char* activeTitle){
    LayoutWin record;
    record.recordId=recordId;
    record.app=app;
    record.deskIndex=deskIndex;
    record.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    record.activeTitle=activeTitle;
    record.activeDomain=counts.empty() ? "" : counts.begin()->first;
    record.counts=counts;
    for(const auto& count : counts) record.tabCount+=count.second;
    record.lastSeenUtc=1900000000;
    return record;
}

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

static void test_auto_restore_destination_uses_preserved_prepare_desktops(){
    LayoutWin saved=Identified(
        "{00000000-0000-0000-0000-000000000408}","firefox",0,
        {{"a.com",1}},"A");
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000008}");
    const std::vector<DeskRec> prepared={
        {8,saved.desktop,L"remembered"}
    };
    CHECK(SavedRestoreDestinationAvailable(
        saved,saved.desktop,prepared));
    CHECK(!SavedRestoreDestinationAvailable(
        saved,saved.desktop,{}));
    CHECK(!SavedRestoreDestinationAvailable(
        saved,G(L"{231A0000-0000-0000-0000-000000000009}"),prepared));
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

static void test_missing_bookkeeping_is_copy_transactional_and_prunes_globally(){
    const UnixSeconds now=2000000000;
    LayoutWin firefox=Identified(
        "{00000000-0000-0000-0000-000000000404}","firefox",0,
        {{"a.com",1}},"A");
    LayoutWin chrome=Identified(
        "{00000000-0000-0000-0000-000000000405}","chrome",0,
        {{"b.com",1}},"B");
    LayoutWin edge=Identified(
        "{00000000-0000-0000-0000-000000000406}","msedge",0,
        {{"c.com",1}},"C");
    LayoutWin expired=Identified(
        "{00000000-0000-0000-0000-000000000407}","firefox",0,
        {{"d.com",1}},"D");
    firefox.lastSeenUtc=now-100;
    chrome.lastSeenUtc=now-50;
    edge.lastSeenUtc=now-25;
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    const std::vector<LayoutWin> input={firefox,chrome,edge,expired};

    const std::vector<LayoutWin> output=MarkOnlyObservedAppsMissing(
        input,{edge.recordId},{"chrome","msedge"},now);

    CHECK(output.size()==3);
    CHECK(output[0].recordId==firefox.recordId && output[0].missingSinceUtc==0);
    CHECK(output[1].recordId==chrome.recordId &&
          output[1].missingSinceUtc==chrome.lastSeenUtc);
    CHECK(output[2].recordId==edge.recordId && output[2].missingSinceUtc==0);
    CHECK(input.size()==4 && input[1].missingSinceUtc==0 &&
          input[3].missingSinceUtc==expired.missingSinceUtc);
}

static void test_title_only_provisional_cannot_steal_established_restore(){
    const UnixSeconds now=2000000000;
    LayoutWin established=Identified(
        "{00000000-0000-0000-0000-000000000409}","firefox",1,
        {{"a.com",1}},"Old title");
    established.desktop=G(L"{231A0000-0000-0000-0000-000000000011}");
    LayoutWin provisional=Identified(
        "{00000000-0000-0000-0000-000000000410}","firefox",0,
        {},"Current title");
    provisional.desktop=G(L"{231A0000-0000-0000-0000-000000000010}");
    provisional.provisional=true;
    LayoutWin live=Identified(
        "{00000000-0000-0000-0000-000000000411}","firefox",0,
        {{"a.com",1}},"Current title");
    live.desktop=provisional.desktop;

    const ReconcilePlan plan=PlanAppReconcile(
        {established,provisional},{live},"firefox",now);

    CHECK(!plan.deferred);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==0 &&
          plan.matches[0].liveIndex==0);
    CHECK(plan.restores.size()==1 && plan.restores[0].savedIndex==0 &&
          GuidEq(plan.restores[0].destination,established.desktop));
}

static void test_distinct_title_only_provisionals_adopt_rich_live_rows(){
    const UnixSeconds now=2000000000;
    LayoutWin alpha=Identified(
        "{00000000-0000-0000-0000-000000000414}","firefox",0,{},"  Alpha ");
    LayoutWin beta=Identified(
        "{00000000-0000-0000-0000-000000000415}","firefox",0,{},"BETA");
    alpha.provisional=true;
    beta.provisional=true;
    LayoutWin liveBeta=Identified(
        "{00000000-0000-0000-0000-000000000416}","firefox",0,
        {{"beta.example",1}}," beta ");
    LayoutWin liveAlpha=Identified(
        "{00000000-0000-0000-0000-000000000417}","firefox",0,
        {{"alpha.example",1}},"alpha");

    const ReconcilePlan plan=PlanAppReconcile(
        {alpha,beta},{liveBeta,liveAlpha},"firefox",now);

    CHECK(!plan.deferred && plan.matches.size()==2);
    const auto alphaMatch=std::find_if(
        plan.matches.begin(),plan.matches.end(),
        [](const LayoutMatch& match){ return match.savedIndex==0; });
    const auto betaMatch=std::find_if(
        plan.matches.begin(),plan.matches.end(),
        [](const LayoutMatch& match){ return match.savedIndex==1; });
    CHECK(alphaMatch!=plan.matches.end() && alphaMatch->liveIndex==1);
    CHECK(betaMatch!=plan.matches.end() && betaMatch->liveIndex==0);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
}

static void test_disjoint_residual_titles_do_not_block_safe_adoption(){
    const UnixSeconds now=2000000000;
    LayoutWin alpha=Identified(
        "{00000000-0000-0000-0000-000000000430}","firefox",0,{},"Alpha");
    LayoutWin beta=Identified(
        "{00000000-0000-0000-0000-000000000431}","firefox",0,{},"Beta");
    alpha.provisional=true;
    beta.provisional=true;
    LayoutWin liveAlpha=Identified(
        "{00000000-0000-0000-0000-000000000432}","firefox",0,
        {{"alpha.example",1}},"Alpha");
    LayoutWin liveGamma=Identified(
        "{00000000-0000-0000-0000-000000000433}","firefox",0,
        {{"gamma.example",1}},"Gamma");

    const ReconcilePlan plan=PlanAppReconcile(
        {alpha,beta},{liveAlpha,liveGamma},"firefox",now);

    CHECK(!plan.deferred);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==0 &&
          plan.matches[0].liveIndex==0);
    CHECK(plan.missingSavedIndices.size()==1 &&
          plan.missingSavedIndices[0]==1);
    CHECK(plan.newRecords.size()==1 && plan.newRecords[0].liveIndex==1);
}

static void test_duplicate_title_only_provisionals_remain_deferred(){
    const UnixSeconds now=2000000000;
    LayoutWin first=Identified(
        "{00000000-0000-0000-0000-000000000418}","firefox",0,{},"Same");
    LayoutWin second=Identified(
        "{00000000-0000-0000-0000-000000000419}","firefox",0,{}," same ");
    first.provisional=true;
    second.provisional=true;
    LayoutWin liveFirst=Identified(
        "{00000000-0000-0000-0000-000000000420}","firefox",0,
        {{"first.example",1}},"Same");
    LayoutWin liveSecond=Identified(
        "{00000000-0000-0000-0000-000000000421}","firefox",0,
        {{"second.example",1}},"Same");

    const ReconcilePlan plan=PlanAppReconcile(
        {first,second},{liveFirst,liveSecond},"firefox",now);

    CHECK(plan.deferred);
    CHECK(plan.matches.empty() && plan.restores.empty());
}

static void test_title_only_provisional_cannot_steal_title_only_live(){
    const UnixSeconds now=2000000000;
    LayoutWin established=Identified(
        "{00000000-0000-0000-0000-000000000422}","firefox",1,
        {{"remembered.example",1}},"Remembered");
    LayoutWin provisional=Identified(
        "{00000000-0000-0000-0000-000000000423}","firefox",0,{},"Current");
    provisional.provisional=true;
    LayoutWin live=Identified(
        "{00000000-0000-0000-0000-000000000424}","firefox",0,{},"Current");

    const ReconcilePlan plan=PlanAppReconcile(
        {established,provisional},{live},"firefox",now);

    CHECK(plan.deferred);
    CHECK(plan.matches.empty() && plan.restores.empty());

    LayoutWin unrelated=Identified(
        "{00000000-0000-0000-0000-000000000425}","firefox",0,
        {{"unrelated.example",1}},"Current");
    const ReconcilePlan unrelatedPlan=PlanAppReconcile(
        {established,provisional},{unrelated},"firefox",now);
    CHECK(!unrelatedPlan.deferred);
    CHECK(unrelatedPlan.matches.size()==1 &&
          unrelatedPlan.matches[0].savedIndex==1 &&
          unrelatedPlan.matches[0].liveIndex==0);

    LayoutWin otherProvisional=Identified(
        "{00000000-0000-0000-0000-000000000434}","firefox",0,{},"Other");
    otherProvisional.provisional=true;
    LayoutWin titleOnlyGamma=Identified(
        "{00000000-0000-0000-0000-000000000435}","firefox",0,{},"Gamma");
    LayoutWin titleOnlyDelta=Identified(
        "{00000000-0000-0000-0000-000000000436}","firefox",0,{},"Delta");
    const ReconcilePlan disjointTitleOnly=PlanAppReconcile(
        {established,provisional,otherProvisional},
        {titleOnlyGamma,titleOnlyDelta},"firefox",now);
    CHECK(disjointTitleOnly.deferred);
    CHECK(disjointTitleOnly.matches.empty());
}

static void test_auto_cli_restore_uses_reconcile_semantics(){
    const UnixSeconds now=2000000000;
    LayoutWin provisional=Identified(
        "{00000000-0000-0000-0000-000000000426}","firefox",0,{}," CLI ");
    provisional.provisional=true;
    LayoutWin live=Identified(
        "{00000000-0000-0000-0000-000000000427}","firefox",0,
        {{"cli.example",1}},"cli");
    const CliRestoreMatchPlan adopted=PlanCliCheckpointRestoreMatches(
        false,{provisional},{live},{"firefox"},now);
    CHECK(adopted.status==CliRestoreMatchStatus::Ready);
    CHECK(adopted.matches.size()==1 && adopted.matches[0].savedIndex==0 &&
          adopted.matches[0].liveIndex==0);

    LayoutWin duplicate=provisional;
    duplicate.recordId="{00000000-0000-0000-0000-000000000428}";
    LayoutWin duplicateLive=live;
    const CliRestoreMatchPlan ambiguous=PlanCliCheckpointRestoreMatches(
        false,{provisional,duplicate},{live,duplicateLive},{"firefox"},now);
    CHECK(ambiguous.status==CliRestoreMatchStatus::Deferred);
    CHECK(ambiguous.matches.empty());

    LayoutWin expired=Identified(
        "{00000000-0000-0000-0000-000000000429}","firefox",1,
        {{"expired.example",1}},"Expired");
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    LayoutWin expiredLive=expired;
    expiredLive.recordId.clear();
    expiredLive.missingSinceUtc=0;
    const CliRestoreMatchPlan autoExpired=PlanCliCheckpointRestoreMatches(
        false,{expired},{expiredLive},{"firefox"},now);
    CHECK(autoExpired.status==CliRestoreMatchStatus::Ready);
    CHECK(autoExpired.matches.empty());
    const CliRestoreMatchPlan manualExpired=PlanCliCheckpointRestoreMatches(
        true,{expired},{expiredLive},{"firefox"},now);
    CHECK(manualExpired.status==CliRestoreMatchStatus::Ready);
    CHECK(manualExpired.matches.size()==1);
}

static void test_final_provisional_binding_yields_to_unresolved_established_record(){
    const UnixSeconds now=2000000000;
    LayoutWin established=Identified(
        "{00000000-0000-0000-0000-000000000412}","firefox",1,
        {{"a.com",1}},"Remembered");
    LayoutWin provisional=Identified(
        "{00000000-0000-0000-0000-000000000413}","firefox",0,
        {},"Observed");
    provisional.provisional=true;
    const std::vector<LayoutWin> records={established,provisional};

    CHECK(!CanBindFinalProvisional(
        records,{},provisional,false,false,now));
    CHECK(CanBindFinalProvisional(
        records,{established.recordId},provisional,false,false,now));
    CHECK(!CanBindFinalProvisional(
        records,{established.recordId},provisional,true,false,now));
    CHECK(!CanBindFinalProvisional(
        records,{established.recordId},provisional,false,true,now));
    established.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    CHECK(CanBindFinalProvisional(
        {established,provisional},{},provisional,false,false,now));
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
    sentinelWin.lastSeenUtc=1700000077; sentinelWin.missingSinceUtc=1700000088;
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
}

static LayoutWin OldStyleRecord(){
    LayoutWin w;
    w.app="firefox"; w.deskIndex=0;
    w.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    w.activeTitle="Inbox"; w.activeDomain="mail.example"; w.tabCount=1;
    w.counts={{"mail.example",1}};
    return w;
}

static LayoutWin StrictV4Record(){
    LayoutWin w=OldStyleRecord();
    w.recordId="{00000000-0000-0000-0000-000000000101}";
    w.lastSeenUtc=1700000000;
    return w;
}

static void test_layout_provisional_marker_roundtrips_strict_v4(){
    LayoutWin provisional=StrictV4Record();
    provisional.provisional=true;
    const std::string marker="P\t"+provisional.recordId+"\n";

    const std::string serialized=SerializeLayout({}, {provisional});
    CHECK(serialized.find(marker)!=std::string::npos);
    CHECK(serialized.find(marker)==serialized.rfind(marker));

    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error="stale";
    int sourceVersion=0;
    CHECK(ParseLayout(serialized,desks,records,1800000000,&error,
                      &sourceVersion));
    CHECK(error.empty() && sourceVersion==4);
    CHECK(records.size()==1 && records[0].provisional);
    CHECK(records.size()==1 && records[0].recordId==provisional.recordId);
}

static void test_layout_noncanonical_record_id_is_published_canonically(){
    const std::string raw="abcdefab-cdef-abcd-efab-cdefabcdefab";
    const std::string marker="{abcdefab-cdef-abcd-efab-cdefabcdefab}";
    const std::string canonical=
        "{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB}";
    const std::string data="# VDE snapshot v4\n"+
        V4Line("{231A0000-0000-0000-0000-000000000001}",
               raw.c_str(),"1700000000","0")+
        "P\t"+marker+"\n";
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error;
    int sourceVersion=0;
    CHECK(ParseLayout(data,desks,records,1800000000,&error,&sourceVersion));
    CHECK(error.empty() && sourceVersion==4 && records.size()==1);
    CHECK(records.size()==1 && records[0].recordId==canonical &&
          records[0].provisional);
    const std::string serialized=SerializeLayout(desks,records);
    CHECK(serialized.find("W\tfirefox\t"+canonical+"\t")!=
          std::string::npos);
    CHECK(serialized.find("P\t"+canonical+"\n")!=std::string::npos);
    CHECK(serialized.find(raw)==std::string::npos);
}

static void test_v4_provisional_extension_preserves_base_window_row(){
    LayoutWin ordinary=StrictV4Record();
    LayoutWin provisional=ordinary;
    provisional.provisional=true;
    const std::string ordinaryBytes=SerializeLayout({}, {ordinary});
    const std::string provisionalBytes=SerializeLayout({}, {provisional});
    const size_t ordinaryWindow=ordinaryBytes.find("W\t");
    const size_t ordinaryEnd=ordinaryBytes.find('\n',ordinaryWindow);
    const size_t provisionalWindow=provisionalBytes.find("W\t");
    const size_t provisionalEnd=provisionalBytes.find('\n',provisionalWindow);
    CHECK(ordinaryWindow!=std::string::npos &&
          provisionalWindow!=std::string::npos);
    CHECK(ordinaryBytes.substr(ordinaryWindow,ordinaryEnd-ordinaryWindow)==
          provisionalBytes.substr(
              provisionalWindow,provisionalEnd-provisionalWindow));
    CHECK(ordinaryBytes.find("\nP\t")==std::string::npos);
    CHECK(provisionalBytes.find("\nP\t"+ordinary.recordId+"\n")!=
          std::string::npos);

    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    CHECK(ParseLayout(ordinaryBytes,desks,records,1800000000));
    CHECK(records.size()==1 && !records[0].provisional);
    CHECK(ParseLayout(provisionalBytes,desks,records,1800000000));
    CHECK(records.size()==1 && records[0].provisional);
}

static void test_layout_provisional_marker_is_strict_and_transactional(){
    const std::string desktop=
        "{231A0000-0000-0000-0000-000000000001}";
    const std::string recordId=
        "{00000000-0000-0000-0000-000000000101}";
    const std::string window=V4Line(
        desktop.c_str(),recordId.c_str(),"1700000000","0");
    const std::vector<std::string> invalid={
        "# VDE snapshot v4\nP\t"+recordId+"\n"+window,
        "# VDE snapshot v4\n"+window+"P\t"+recordId+"\textra\n",
        "# VDE snapshot v4\n"+window+
            "P\t{00000000-0000-0000-0000-000000000102}\n",
        "# VDE snapshot v4\n"+window+"P\t"+recordId+"\nP\t"+
            recordId+"\n"
    };
    for(const std::string& data : invalid){
        DeskRec sentinelDesk{};
        sentinelDesk.index=77;
        LayoutWin sentinel=StrictV4Record();
        sentinel.activeTitle="sentinel";
        std::vector<DeskRec> desks={sentinelDesk};
        std::vector<LayoutWin> records={sentinel};
        std::string error;
        CHECK(!ParseLayout(data,desks,records,1800000000,&error));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77);
        CHECK(records.size()==1 && records[0].activeTitle=="sentinel");
        CHECK(!records[0].provisional);
    }
}

static void test_layout_legacy_migration_never_invents_provisional_marker(){
    const std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" +
        b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\t0\n";
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error;
    int sourceVersion=0;
    CHECK(ParseLayout(data,desks,records,1800000000,&error,&sourceVersion));
    CHECK(error.empty() && sourceVersion==3 && records.size()==1);
    CHECK(records.size()==1 && !records[0].provisional);
    CHECK(SerializeLayout(desks,records).find("\nP\t")==std::string::npos);
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

static void test_startup_expiry_partitions_every_app_transactionally(){
    const UnixSeconds now=1900000000;
    LayoutWin expiredFirefox=OldStyleRecord();
    expiredFirefox.recordId="expired-enabled-firefox";
    expiredFirefox.app="firefox";
    expiredFirefox.lastSeenUtc=now-WINDOW_RETENTION_SECONDS-10;
    expiredFirefox.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;

    LayoutWin recentChrome=OldStyleRecord();
    recentChrome.recordId="recent-chrome";
    recentChrome.app="chrome";
    recentChrome.lastSeenUtc=now-WINDOW_RETENTION_SECONDS+1;
    recentChrome.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;

    LayoutWin expiredDisabledEdge=OldStyleRecord();
    expiredDisabledEdge.recordId="expired-disabled-edge";
    expiredDisabledEdge.app="msedge";
    expiredDisabledEdge.lastSeenUtc=now-WINDOW_RETENTION_SECONDS-20;
    expiredDisabledEdge.missingSinceUtc=now-WINDOW_RETENTION_SECONDS-5;

    LayoutWin liveChrome=OldStyleRecord();
    liveChrome.recordId="live-chrome";
    liveChrome.app="chrome";
    liveChrome.lastSeenUtc=now-1;
    liveChrome.missingSinceUtc=0;

    const std::vector<LayoutWin> input={
        expiredFirefox,recentChrome,expiredDisabledEdge,liveChrome
    };
    ExpiredLayoutPartition partition;
    CHECK(PartitionExpiredLayoutRecords(input,now,partition));
    CHECK(partition.expired.size()==2 &&
          partition.expired[0].recordId=="expired-enabled-firefox" &&
          partition.expired[1].recordId=="expired-disabled-edge");
    CHECK(partition.retained.size()==2 &&
          partition.retained[0].recordId=="recent-chrome" &&
          partition.retained[1].recordId=="live-chrome");
    CHECK(input.size()==4 && input[0].recordId=="expired-enabled-firefox" &&
          input[1].recordId=="recent-chrome" &&
          input[2].recordId=="expired-disabled-edge" &&
          input[3].recordId=="live-chrome");

    ExpiredLayoutPartition untouched;
    LayoutWin retainedSentinel=OldStyleRecord();
    retainedSentinel.recordId="retained-sentinel";
    LayoutWin expiredSentinel=OldStyleRecord();
    expiredSentinel.recordId="expired-sentinel";
    untouched.retained.push_back(retainedSentinel);
    untouched.expired.push_back(expiredSentinel);
    int copies=0;
    CHECK(!PartitionExpiredLayoutRecords(
        input,now,untouched,
        [&](std::vector<LayoutWin>& destination,const LayoutWin& record){
            if(++copies==2) throw std::bad_alloc();
            destination.push_back(record);
            return true;
        }));
    CHECK(copies==2);
    CHECK(untouched.retained.size()==1 &&
          untouched.retained[0].recordId=="retained-sentinel");
    CHECK(untouched.expired.size()==1 &&
          untouched.expired[0].recordId=="expired-sentinel");
    CHECK(input.size()==4 && input[0].app=="firefox" &&
          input[1].app=="chrome" && input[2].app=="msedge" &&
          input[3].missingSinceUtc==0);
}

static LayoutWin MatchRecord(const char* app, const char* title, const char* domain, int tabs,
        const std::map<std::string,int>& counts){
    LayoutWin w=OldStyleRecord();
    w.app=app; w.activeTitle=title; w.activeDomain=domain; w.tabCount=tabs; w.counts=counts;
    return w;
}

static void test_reconcile_restores_saved_a_and_creates_new_b(){
    const UnixSeconds now=2000000000;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000301}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    liveA.deskIndex=3;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({savedA},{liveA,liveB},"firefox",now);
    CHECK(plan.restores.size()==1);
    CHECK(plan.restores[0].liveIndex==0);
    CHECK(plan.newRecords.size()==1 && plan.newRecords[0].liveIndex==1);
    CHECK(!plan.newRecords[0].recordId.empty());

    std::vector<LayoutWin> committed=CommitAppReconcile({savedA},{liveA,liveB},plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(GuidEq(committed[0].desktop,savedA.desktop));
    CHECK(committed[0].missingSinceUtc==0);
    CHECK(committed[1].activeTitle=="B");
    CHECK(GuidEq(committed[1].desktop,liveB.desktop));
}

static void test_expired_reappearance_is_new_not_restored(){
    const UnixSeconds now=2000000000;
    LayoutWin expired=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    expired.recordId="{00000000-0000-0000-0000-000000000305}";
    expired.deskIndex=0;
    expired.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    LayoutWin live=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    live.deskIndex=4;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({expired},{live},"firefox",now);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.size()==1);
    CHECK(plan.newRecords[0].recordId!=expired.recordId);

    std::vector<LayoutWin> committed=CommitAppReconcile({expired},{live},plan,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==plan.newRecords[0].recordId);
    CHECK(GuidEq(committed[0].desktop,live.desktop));
}

static void test_cached_stale_edge_preserves_match_and_defers_unmatched(){
    const UnixSeconds now=2000000000;
    LayoutWin saved=MatchRecord("msedge","Saved","saved.example",2,{{"saved.example",2}});
    saved.recordId="{00000000-0000-0000-0000-000000000307}";
    saved.deskIndex=0;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin matched=MatchRecord("msedge","Saved","stale.example",9,{{"saved.example",2}});
    matched.deskIndex=0;
    matched.desktop=saved.desktop;
    LayoutWin unmatched=MatchRecord("msedge","New","new.example",1,{{"new.example",1}});
    unmatched.deskIndex=1;
    unmatched.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ReconcilePlan plan=PlanAppReconcile(
        {saved},{matched,unmatched},"msedge",now,{},ReconcileFreshness::CachedStale);
    CHECK(plan.matches.size()==1);
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());

    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{matched,unmatched},plan,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].counts==saved.counts);
    CHECK(committed[0].activeTitle==saved.activeTitle);
    CHECK(committed[0].activeDomain==saved.activeDomain);
    CHECK(committed[0].tabCount==saved.tabCount);
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

static std::string DeterministicRecordId(size_t ordinal){
    GUID id{};
    id.Data1=static_cast<unsigned long>(ordinal+1);
    return W2U8(GuidToString(id));
}

static size_t& CountingRecordIdGeneratorCalls(){
    static size_t calls=0;
    return calls;
}

static void ResetCountingRecordIdGenerator(){
    CountingRecordIdGeneratorCalls()=0;
}

static std::string CountingRecordIdGenerator(){
    const size_t call=CountingRecordIdGeneratorCalls()++;
    return DeterministicRecordId(MAX_LAYOUT_RECORDS+100+call);
}

static size_t& CountingReconcileMatcherCalls(){
    static size_t calls=0;
    return calls;
}

static void ResetCountingReconcileMatcher(){
    CountingReconcileMatcherCalls()=0;
}

static std::vector<LayoutMatch> CountingReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    ++CountingReconcileMatcherCalls();
    return MatchOneToOne(saved,live,acceptScore,tooComplex);
}

static std::vector<LayoutMatch> DuplicateSavedReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    (void)saved; (void)live; (void)acceptScore;
    if(tooComplex) *tooComplex=false;
    return {Candidate(0,0,0.9),Candidate(0,1,0.9)};
}

static std::vector<LayoutMatch> DuplicateLiveReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    (void)saved; (void)live; (void)acceptScore;
    if(tooComplex) *tooComplex=false;
    return {Candidate(0,0,0.9),Candidate(1,0,0.9)};
}

enum class InjectedMatchMode {
    SavedOutOfRange, LiveOutOfRange, OtherAppLive, NotANumber,
    Infinity, BelowThreshold, TooMany, Valid
};

static InjectedMatchMode& CurrentInjectedMatchMode(){
    static InjectedMatchMode mode=InjectedMatchMode::Valid;
    return mode;
}

static std::vector<LayoutMatch> ConfigurableReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    if(tooComplex) *tooComplex=false;
    switch(CurrentInjectedMatchMode()){
    case InjectedMatchMode::SavedOutOfRange:
        return {Candidate(saved.size(),0,acceptScore)};
    case InjectedMatchMode::LiveOutOfRange:
        return {Candidate(0,live.size(),acceptScore)};
    case InjectedMatchMode::OtherAppLive:
        return {Candidate(0,1,acceptScore)};
    case InjectedMatchMode::NotANumber:
        return {Candidate(0,0,std::numeric_limits<double>::quiet_NaN())};
    case InjectedMatchMode::Infinity:
        return {Candidate(0,0,std::numeric_limits<double>::infinity())};
    case InjectedMatchMode::BelowThreshold:
        return {Candidate(0,0,acceptScore-0.01)};
    case InjectedMatchMode::TooMany:
        return std::vector<LayoutMatch>(
            MAX_LAYOUT_RECORDS+1,Candidate(0,0,acceptScore));
    case InjectedMatchMode::Valid:
        return {Candidate(0,0,acceptScore)};
    }
    return {};
}

static bool SameLayoutWinFields(const LayoutWin& left, const LayoutWin& right){
    return left.recordId==right.recordId && left.app==right.app &&
        left.deskIndex==right.deskIndex && GuidEq(left.desktop,right.desktop) &&
        left.activeTitle==right.activeTitle && left.activeDomain==right.activeDomain &&
        left.tabCount==right.tabCount && left.counts==right.counts &&
        left.lastSeenUtc==right.lastSeenUtc &&
        left.missingSinceUtc==right.missingSinceUtc &&
        left.provisional==right.provisional;
}

static bool SameLayoutWinVectors(const std::vector<LayoutWin>& left,
        const std::vector<LayoutWin>& right){
    if(left.size()!=right.size()) return false;
    for(size_t i=0;i<left.size();++i)
        if(!SameLayoutWinFields(left[i],right[i])) return false;
    return true;
}

static LayoutWin ReconcileTestRecord(const std::string& recordId, const char* app,
        const char* title, const char* domain, int deskIndex, const GUID& desktop,
        UnixSeconds lastSeenUtc){
    LayoutWin record=MatchRecord(app,title,domain,1,{{domain,1}});
    record.recordId=recordId;
    record.deskIndex=deskIndex;
    record.desktop=desktop;
    record.lastSeenUtc=lastSeenUtc;
    return record;
}

static ReconcilePlan ValidCommitPlan(const std::string& app, UnixSeconds nowUtc,
        ReconcileFreshness freshness=ReconcileFreshness::Fresh){
    ReconcilePlan plan;
    plan.app=app;
    plan.nowUtc=nowUtc;
    plan.freshness=freshness;
    return plan;
}

static NewRecordRequest PlannedNewRecord(size_t liveIndex, const std::string& recordId){
    NewRecordRequest request;
    request.liveIndex=liveIndex;
    request.recordId=recordId;
    return request;
}

static RestoreRequest PlannedRestore(size_t savedIndex, size_t liveIndex,
        const GUID& destination){
    RestoreRequest request;
    request.savedIndex=savedIndex;
    request.liveIndex=liveIndex;
    request.destination=destination;
    return request;
}

static LayoutWin PersistedProvisionalRecord(
        const std::string& recordId,const GUID& desktop,UnixSeconds lastSeenUtc){
    LayoutWin record;
    record.recordId=recordId;
    record.app="firefox";
    record.deskIndex=1;
    record.desktop=desktop;
    record.lastSeenUtc=lastSeenUtc;
    record.provisional=true;
    return record;
}

static void test_layout_provisional_companions_do_not_consume_record_cap(){
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    std::vector<LayoutWin> records;
    records.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=ReconcileTestRecord(
            DeterministicRecordId(14000+i),"firefox","Provisional",
            "provisional.example",1,desktop,1700000000);
        record.provisional=true;
        records.push_back(record);
    }
    std::string bytes;
    std::string error="stale";
    CHECK(BuildCheckedLayoutSnapshot({},records,1700000000,bytes,&error));
    CHECK(error.empty());

    std::vector<DeskRec> parsedDesks;
    std::vector<LayoutWin> parsedRecords;
    int sourceVersion=0;
    CHECK(ParseLayout(bytes,parsedDesks,parsedRecords,1800000000,&error,
                      &sourceVersion));
    CHECK(error.empty() && sourceVersion==4);
    CHECK(parsedRecords.size()==MAX_LAYOUT_RECORDS);
    CHECK(parsedRecords.size()==MAX_LAYOUT_RECORDS &&
          parsedRecords.front().provisional && parsedRecords.back().provisional);

    LayoutWin overflow=records.back();
    overflow.recordId=DeterministicRecordId(14000+MAX_LAYOUT_RECORDS);
    const std::string overflowSnapshot=SerializeLayout({}, {overflow});
    const std::string overflowBody=overflowSnapshot.substr(
        std::string("# VDE snapshot v4\n").size());
    DeskRec sentinelDesk{};
    sentinelDesk.index=77;
    LayoutWin sentinel=StrictV4Record();
    sentinel.activeTitle="sentinel";
    parsedDesks={sentinelDesk};
    parsedRecords={sentinel};
    CHECK(!ParseLayout(bytes+overflowBody,parsedDesks,parsedRecords,
                       1800000000,&error));
    CHECK(!error.empty());
    CHECK(parsedDesks.size()==1 && parsedDesks[0].index==77);
    CHECK(parsedRecords.size()==1 &&
          parsedRecords[0].activeTitle=="sentinel");
}

static void test_fresh_reconcile_adopts_one_persisted_provisional_with_same_id(){
    const UnixSeconds now=2000000050;
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin saved=PersistedProvisionalRecord(
        "{00000000-0000-0000-0000-000000000391}",desktop,now-50);
    LayoutWin live=MatchRecord(
        "firefox","fresh identity","fresh.example",2,
        {{"fresh.example",2}});
    live.deskIndex=1;
    live.desktop=desktop;
    ResetCountingRecordIdGenerator();

    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);

    CHECK(!plan.deferred);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==0 &&
          plan.matches[0].liveIndex==0);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {saved},{live},plan,{},now);
    CHECK(committed.size()==1 && committed[0].recordId==saved.recordId);
    CHECK(committed.size()==1 && !committed[0].provisional);
    CHECK(committed.size()==1 && committed[0].activeTitle==live.activeTitle &&
          committed[0].counts==live.counts);
    ResetCountingRecordIdGenerator();
}

static void test_fresh_reconcile_clears_multiple_matched_provisionals(){
    const UnixSeconds now=2000000060;
    const GUID desktopA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID desktopB=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin savedA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000000392}","firefox","Alpha",
        "alpha.example",1,desktopA,now-50);
    LayoutWin savedB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000000393}","firefox","Beta",
        "beta.example",2,desktopB,now-50);
    savedA.provisional=true;
    savedB.provisional=true;
    LayoutWin liveA=savedA;
    LayoutWin liveB=savedB;
    liveA.recordId.clear();
    liveB.recordId.clear();
    liveA.provisional=false;
    liveB.provisional=false;

    ReconcilePlan plan=PlanAppReconcile(
        {savedA,savedB},{liveB,liveA},"firefox",now);
    CHECK(!plan.deferred && plan.matches.size()==2);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {savedA,savedB},{liveB,liveA},plan,{},now);
    CHECK(committed.size()==2);
    CHECK(committed.size()==2 && committed[0].recordId==savedA.recordId &&
          committed[1].recordId==savedB.recordId);
    CHECK(committed.size()==2 && !committed[0].provisional &&
          !committed[1].provisional);
}

static void test_fresh_reconcile_defers_ambiguous_provisional_adoption(){
    const UnixSeconds now=2000000070;
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    const std::pair<size_t,size_t> cardinalities[]={{1,2},{2,1},{2,2}};
    for(const auto& cardinality : cardinalities){
        std::vector<LayoutWin> saved;
        std::vector<LayoutWin> live;
        for(size_t i=0;i<cardinality.first;++i)
            saved.push_back(PersistedProvisionalRecord(
                DeterministicRecordId(9300+i),desktop,now-50));
        for(size_t i=0;i<cardinality.second;++i){
            LayoutWin observed=MatchRecord(
                "firefox",("Live "+std::to_string(i)).c_str(),
                ("live"+std::to_string(i)+".example").c_str(),1,{});
            observed.deskIndex=1;
            observed.desktop=desktop;
            live.push_back(observed);
        }
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            saved,live,"firefox",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty() && plan.restores.empty());
        CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
    }
    ResetCountingRecordIdGenerator();
}

static void test_failed_chrome_restore_retains_saved_destination_and_marks_seen(){
    const UnixSeconds now=2000000100;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000401}";
    saved.deskIndex=1;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    saved.lastSeenUtc=now-50;
    saved.missingSinceUtc=now-20;
    LayoutWin live=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    live.deskIndex=4;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({saved},{live},"chrome",now);
    CHECK(plan.restores.size()==1);
    CHECK(plan.restores[0].savedIndex==0 && plan.restores[0].liveIndex==0);
    CHECK(GuidEq(plan.restores[0].destination,saved.desktop));
    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{live},plan,{},now);

    CHECK(committed.size()==1);
    CHECK(GuidEq(committed[0].desktop,saved.desktop));
    CHECK(committed[0].deskIndex==saved.deskIndex);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(committed[0].lastSeenUtc==now && committed[0].missingSinceUtc==0);
}

static void test_empty_chrome_reconcile_marks_only_chrome_missing(){
    const UnixSeconds now=2000000200;
    LayoutWin firefox=MatchRecord("firefox","Firefox","ff.example",3,{{"ff.example",3}});
    firefox.recordId="{00000000-0000-0000-0000-000000000402}";
    firefox.deskIndex=0;
    firefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    firefox.lastSeenUtc=now-100;
    LayoutWin chrome=MatchRecord("chrome","Chrome","chrome.example",2,{{"chrome.example",2}});
    chrome.recordId="{00000000-0000-0000-0000-000000000403}";
    chrome.deskIndex=1;
    chrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    chrome.lastSeenUtc=now-25;

    ReconcilePlan plan=PlanAppReconcile({firefox,chrome},{},"chrome",now);
    CHECK(plan.matches.empty() && plan.restores.empty() && plan.newRecords.empty());
    CHECK(plan.missingSavedIndices==std::vector<size_t>({1}));
    std::vector<LayoutWin> committed=CommitAppReconcile({firefox,chrome},{},plan,{},now);

    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],firefox));
    CHECK(committed[1].missingSinceUtc==chrome.lastSeenUtc);
    CHECK(committed[1].lastSeenUtc==chrome.lastSeenUtc);
}

static void test_reserved_chrome_record_cannot_be_stolen_by_duplicate(){
    const UnixSeconds now=2000000300;
    LayoutWin bound=MatchRecord("chrome","Same","same.com",1,{{"same.com",1}});
    bound.recordId="{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB}";
    bound.deskIndex=0;
    bound.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    bound.lastSeenUtc=now-10;
    LayoutWin duplicate=MatchRecord("chrome","Same","same.com",1,{{"same.com",1}});
    duplicate.deskIndex=2;
    duplicate.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    const std::set<std::string> reserved={"abcdefab-cdef-abcd-efab-cdefabcdefab"};

    ReconcilePlan plan=PlanAppReconcile(
        {bound},{duplicate},"chrome",now,reserved,ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(plan.newRecords.size()==1 && plan.newRecords[0].liveIndex==0);
    CHECK(plan.newRecords[0].recordId==ConstantRecordIdGenerator());
    std::vector<LayoutWin> committed=CommitAppReconcile({bound},{duplicate},plan,{},now);

    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],bound));
    CHECK(committed[1].recordId==ConstantRecordIdGenerator());
    CHECK(committed[1].deskIndex==duplicate.deskIndex);
    CHECK(GuidEq(committed[1].desktop,duplicate.desktop));
}

static void test_same_desktop_match_learns_live_index_without_restore(){
    const UnixSeconds now=2000000400;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000405}";
    saved.deskIndex=2;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin live=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    live.deskIndex=8;
    live.desktop=saved.desktop;

    ReconcilePlan plan=PlanAppReconcile({saved},{live},"chrome",now);
    CHECK(plan.matches.size()==1);
    CHECK(plan.restores.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{live},plan,{},now);

    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(GuidEq(committed[0].desktop,live.desktop));
    CHECK(committed[0].deskIndex==live.deskIndex);
    CHECK(committed[0].lastSeenUtc==now);
}

static void test_late_window_after_first_wave_restores_before_save(){
    LcState state;
    CHECK(LcObserve(state,true,1,10,100,1,0).action==LcAction::None);
    LcDecision first=LcObserve(state,true,1,10,100,1,1);
    CHECK(first.action==LcAction::BeginRestore && first.generation!=0);
    LcRestoreCompleted(state,first.generation,LcRestoreOutcome::Success,100,1,2);
    CHECK(LcObserve(state,true,3,30,100,1,3).action==LcAction::None);
    LcDecision late=LcObserve(state,true,3,30,100,1,4);
    CHECK(late.action==LcAction::BeginRestore && late.generation!=first.generation);
    CHECK(LcObserve(state,true,3,30,100,1,5).action==LcAction::None);

    const UnixSeconds now=2000000500;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000406}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedA.lastSeenUtc=now-10;
    LayoutWin savedB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    savedB.recordId="{00000000-0000-0000-0000-000000000407}";
    savedB.deskIndex=1;
    savedB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    savedB.lastSeenUtc=now-100;
    savedB.missingSinceUtc=now-100;
    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    const std::set<std::string> reserved={savedA.recordId};

    ReconcilePlan plan=PlanAppReconcile({savedA,savedB},{liveB},"firefox",now,reserved);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==1 &&
        plan.matches[0].liveIndex==0);
    CHECK(plan.restores.size()==1 && plan.restores[0].savedIndex==1 &&
        plan.restores[0].liveIndex==0);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {savedA,savedB},{liveB},plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],savedA));
    CHECK(GuidEq(committed[1].desktop,savedB.desktop));

    LcRestoreCompleted(state,late.generation,LcRestoreOutcome::Success,100,1,6);
    CHECK(LcObserve(state,true,3,30,100,1,7).action==LcAction::None);
    CHECK(LcObserve(state,true,3,30,100,1,8).action==LcAction::None);
}

static void test_edge_retention_is_independent_while_firefox_stays_open(){
    const UnixSeconds now=2000000600;
    LayoutWin firefoxA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    firefoxA.recordId="{00000000-0000-0000-0000-000000000408}";
    firefoxA.deskIndex=0;
    firefoxA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    firefoxA.lastSeenUtc=now-50;
    LayoutWin firefoxB=MatchRecord("firefox","B","b.com",2,{{"b.com",2}});
    firefoxB.recordId="{00000000-0000-0000-0000-000000000409}";
    firefoxB.deskIndex=1;
    firefoxB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    firefoxB.lastSeenUtc=now-40;
    std::vector<LayoutWin> firefoxRecords={firefoxA,firefoxB};
    const std::vector<LayoutWin> originalFirefox=firefoxRecords;

    LayoutWin edge=MatchRecord("msedge","Edge","edge.example",1,{{"edge.example",1}});
    edge.recordId="{00000000-0000-0000-0000-000000000410}";
    edge.deskIndex=2;
    edge.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    edge.lastSeenUtc=now-10;
    std::vector<LayoutWin> edgeRecords={edge};
    LcState firefoxState,edgeState;

    CHECK(LcObserve(firefoxState,true,10,10,10,10,0).action==LcAction::None);
    LcDecision edgeMissing=LcObserve(edgeState,false,0,0,20,20,0);
    CHECK(edgeMissing.action==LcAction::MarkMissingFromLastSeen);
    ReconcilePlan absentPlan=PlanAppReconcile(edgeRecords,{},"msedge",now);
    edgeRecords=CommitAppReconcile(edgeRecords,{},absentPlan,{},now);
    CHECK(edgeRecords.size()==1 && edgeRecords[0].missingSinceUtc==edge.lastSeenUtc);
    CHECK(SameLayoutWinVectors(firefoxRecords,originalFirefox));

    LcDecision firefoxWave=LcObserve(firefoxState,true,10,10,10,10,1);
    CHECK(firefoxWave.action==LcAction::BeginRestore);
    LcRestoreCompleted(firefoxState,firefoxWave.generation,LcRestoreOutcome::Success,10,10,2);
    CHECK(LcObserve(edgeState,false,0,0,20,20,1).action==LcAction::None);
    CHECK(LcObserve(firefoxState,true,10,10,10,10,2).action==LcAction::None);
    CHECK(LcObserve(edgeState,true,30,30,20,21,2).action==LcAction::None);

    LayoutWin liveEdge=MatchRecord("msedge","Edge","edge.example",1,{{"edge.example",1}});
    liveEdge.deskIndex=4;
    liveEdge.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan returnPlan=PlanAppReconcile(edgeRecords,{liveEdge},"msedge",now+1);
    CHECK(returnPlan.restores.size()==1);
    edgeRecords=CommitAppReconcile(edgeRecords,{liveEdge},returnPlan,{0},now+1);
    LcDecision edgeReturn=LcObserve(edgeState,true,30,30,20,21,3);
    CHECK(edgeReturn.action==LcAction::BeginRestore);
    LcRestoreCompleted(edgeState,edgeReturn.generation,LcRestoreOutcome::Success,20,21,4);
    CHECK(LcObserve(firefoxState,true,10,10,10,10,3).action==LcAction::None);
    CHECK(SameLayoutWinVectors(firefoxRecords,originalFirefox));
}

static void test_firefox_sibling_reappears_while_first_window_stays_open(){
    const UnixSeconds now=2000000700;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000411}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedA.lastSeenUtc=now-20;
    LayoutWin savedB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    savedB.recordId="{00000000-0000-0000-0000-000000000412}";
    savedB.deskIndex=1;
    savedB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    savedB.lastSeenUtc=now-10;
    const std::set<std::string> reserved={savedA.recordId};

    ReconcilePlan missingPlan=PlanAppReconcile({savedA,savedB},{},"firefox",now,reserved);
    CHECK(missingPlan.missingSavedIndices==std::vector<size_t>({1}));
    std::vector<LayoutWin> afterMissing=CommitAppReconcile(
        {savedA,savedB},{},missingPlan,{},now);
    CHECK(afterMissing.size()==2);
    CHECK(SameLayoutWinFields(afterMissing[0],savedA));
    CHECK(afterMissing[1].missingSinceUtc==savedB.lastSeenUtc);

    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan returnPlan=PlanAppReconcile(
        afterMissing,{liveB},"firefox",now+1,reserved);
    CHECK(returnPlan.matches.size()==1 && returnPlan.matches[0].savedIndex==1);
    CHECK(returnPlan.restores.size()==1 && returnPlan.restores[0].savedIndex==1 &&
        returnPlan.restores[0].liveIndex==0);
    CHECK(returnPlan.newRecords.empty() && returnPlan.missingSavedIndices.empty());
    std::vector<LayoutWin> failed=CommitAppReconcile(
        afterMissing,{liveB},returnPlan,{},now+1);

    CHECK(failed.size()==2);
    CHECK(SameLayoutWinFields(failed[0],savedA));
    CHECK(GuidEq(failed[1].desktop,savedB.desktop));
    CHECK(failed[1].deskIndex==savedB.deskIndex);
    CHECK(failed[1].lastSeenUtc==now+1 && failed[1].missingSinceUtc==0);
}

static void test_reconcile_plan_and_commit_preserve_input_vectors(){
    const UnixSeconds now=2000000800;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000413}";
    saved.deskIndex=0;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    saved.lastSeenUtc=now-30;
    saved.missingSinceUtc=now-20;
    LayoutWin liveA=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    liveA.deskIndex=3;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    LayoutWin liveB=MatchRecord("chrome","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    std::vector<LayoutWin> existing={saved};
    std::vector<LayoutWin> live={liveA,liveB};
    const std::vector<LayoutWin> originalExisting=existing;
    const std::vector<LayoutWin> originalLive=live;

    ReconcilePlan plan=PlanAppReconcile(
        existing,live,"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(SameLayoutWinVectors(existing,originalExisting));
    CHECK(SameLayoutWinVectors(live,originalLive));
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,live,plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(SameLayoutWinVectors(existing,originalExisting));
    CHECK(SameLayoutWinVectors(live,originalLive));
}

static void test_reconcile_empty_generator_defers_transactionally(){
    const UnixSeconds now=2000000900;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000414}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};

    ReconcilePlan plan=PlanAppReconcile(
        existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,
        FailingRecordIdGenerator);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,{liveChrome},plan,{},now);
    CHECK(SameLayoutWinVectors(committed,existing));
}

static void test_reconcile_invalid_generators_defer_transactionally(){
    const UnixSeconds now=2000001000;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000415}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};
    const RecordIdGenerator generators[]={MalformedRecordIdGenerator,ZeroRecordIdGenerator};

    for(RecordIdGenerator generator : generators){
        ReconcilePlan plan=PlanAppReconcile(
            existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,generator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        std::vector<LayoutWin> committed=CommitAppReconcile(
            existing,{liveChrome},plan,{},now);
        CHECK(SameLayoutWinVectors(committed,existing));
    }
}

static void test_reconcile_generator_collision_with_any_existing_record_defers(){
    const UnixSeconds now=2000001100;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=4;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    for(int variant=0;variant<3;++variant){
        LayoutWin existing=MatchRecord(
            "chrome","Existing","existing.example",1,{{"existing.example",1}});
        existing.recordId="00000000-0000-0000-0000-000000000099";
        existing.deskIndex=0;
        existing.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
        existing.lastSeenUtc=now-10;
        std::set<std::string> reserved;
        if(variant==0) existing.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
        if(variant==1) existing.app="firefox";
        if(variant==2) reserved.insert(existing.recordId);

        ReconcilePlan plan=PlanAppReconcile(
            {existing},{liveChrome},"chrome",now,reserved,ReconcileFreshness::Fresh,
            ConstantRecordIdGenerator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
    }
}

static void test_reconcile_duplicate_generated_ids_defer_transactionally(){
    const UnixSeconds now=2000001200;
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=0;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=1;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ReconcilePlan plan=PlanAppReconcile(
        {},{liveA,liveB},"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
}

static void test_reconcile_unique_generated_id_commits_strict_v4(){
    const UnixSeconds now=2000001300;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=0;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");

    ReconcilePlan plan=PlanAppReconcile(
        {},{live},"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(!plan.deferred);
    CHECK(plan.matches.empty() && plan.restores.empty() &&
        plan.missingSavedIndices.empty());
    CHECK(plan.newRecords.size()==1 &&
        plan.newRecords[0].recordId==ConstantRecordIdGenerator());
    std::vector<LayoutWin> committed=CommitAppReconcile({}, {live}, plan, {}, now);
    CHECK(committed.size()==1);

    std::string snapshot,error;
    CHECK(BuildCheckedLayoutSnapshot({},committed,now,snapshot,&error));
    CHECK(error.empty());
    std::vector<DeskRec> parsedDesks;
    std::vector<LayoutWin> parsed;
    int version=0;
    CHECK(ParseLayout(snapshot,parsedDesks,parsed,now,&error,&version));
    CHECK(error.empty() && version==4 && parsedDesks.empty());
    CHECK(parsed.size()==1 && SameLayoutWinFields(parsed[0],committed[0]));
}

static void test_reconcile_null_generator_defers_transactionally(){
    const UnixSeconds now=2000001400;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000416}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};
    RecordIdGenerator generator=nullptr;

    ReconcilePlan plan=PlanAppReconcile(
        existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,generator);
    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,{liveChrome},plan,{},now);
    CHECK(SameLayoutWinVectors(committed,existing));
}

static void test_reconcile_match_preflight_too_complex_defers_cleanly(){
    const UnixSeconds now=2000001500;
    LayoutWin savedBase=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    savedBase.deskIndex=0;
    savedBase.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedBase.lastSeenUtc=now-10;
    std::vector<LayoutWin> saved;
    saved.reserve(1001);
    for(size_t i=0;i<1001;++i){
        LayoutWin record=savedBase;
        record.recordId=DeterministicRecordId(i);
        saved.push_back(record);
    }
    LayoutWin liveBase=MatchRecord("chrome","Live","other.example",1,{{"other.example",1}});
    liveBase.deskIndex=1;
    liveBase.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<LayoutWin> live(1000,liveBase);

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        saved,live,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(saved,live,plan,{},now);
    CHECK(SameLayoutWinVectors(committed,saved));
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_window_caps_defer_before_generation(){
    const UnixSeconds now=2000001600;
    LayoutWin existingBase=MatchRecord(
        "chrome","Existing","existing.example",1,{{"existing.example",1}});
    existingBase.deskIndex=0;
    existingBase.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingBase.lastSeenUtc=now-10;
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=existingBase;
        record.recordId=DeterministicRecordId(i);
        existing.push_back(record);
    }
    std::vector<LayoutWin> strictExisting=existing;
    std::string snapshot,error;
    CHECK(BuildCheckedLayoutSnapshot({},strictExisting,now,snapshot,&error));
    CHECK(error.empty());

    LayoutWin liveFirefox=MatchRecord(
        "firefox","New","new.example",1,{{"new.example",1}});
    liveFirefox.deskIndex=1;
    liveFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ReconcilePlan fullOutput=PlanAppReconcile(
        existing,{liveFirefox},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(fullOutput.deferred);
    CHECK(fullOutput.matches.empty());
    CHECK(fullOutput.restores.empty());
    CHECK(fullOutput.newRecords.empty());
    CHECK(fullOutput.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,{liveFirefox},fullOutput,{},now),existing));

    std::vector<LayoutWin> oversizedExisting=existing;
    LayoutWin extra=existingBase;
    extra.recordId=DeterministicRecordId(MAX_LAYOUT_RECORDS);
    oversizedExisting.push_back(extra);
    ResetCountingRecordIdGenerator();
    ReconcilePlan existingOverflow=PlanAppReconcile(
        oversizedExisting,{liveFirefox},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(existingOverflow.deferred);
    CHECK(existingOverflow.matches.empty());
    CHECK(existingOverflow.restores.empty());
    CHECK(existingOverflow.newRecords.empty());
    CHECK(existingOverflow.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);

    std::vector<LayoutWin> oversizedLive(MAX_LAYOUT_RECORDS+1,liveFirefox);
    ResetCountingRecordIdGenerator();
    ReconcilePlan liveOverflow=PlanAppReconcile(
        {},oversizedLive,"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(liveOverflow.deferred);
    CHECK(liveOverflow.matches.empty());
    CHECK(liveOverflow.restores.empty());
    CHECK(liveOverflow.newRecords.empty());
    CHECK(liveOverflow.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_malformed_reserved_id_defers_before_work(){
    const UnixSeconds now=2000001650;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(17000),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"chrome",now,{"not-a-guid"},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);

    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_reserved_id_cap_is_fail_closed_at_boundary(){
    const UnixSeconds now=2000001675;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::set<std::string> oversized;
    for(size_t i=0;i<MAX_LAYOUT_RECORDS+1;++i)
        oversized.insert(DeterministicRecordId(18000+i));

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan rejected=PlanAppReconcile(
        {},{live},"chrome",now,oversized,ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(rejected.app=="chrome" && rejected.nowUtc==now &&
        rejected.freshness==ReconcileFreshness::Fresh);
    CHECK(rejected.deferred);
    CHECK(rejected.matches.empty());
    CHECK(rejected.restores.empty());
    CHECK(rejected.newRecords.empty());
    CHECK(rejected.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    LayoutWin bound=ReconcileTestRecord(
        "{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAC}","chrome","Same","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin duplicate=MatchRecord("chrome","Same","same.example",1,{{"same.example",1}});
    duplicate.deskIndex=2;
    duplicate.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    std::set<std::string> exact={"abcdefab-cdef-abcd-efab-cdefabcdefac"};
    for(size_t i=0;i<MAX_LAYOUT_RECORDS-1;++i)
        exact.insert(DeterministicRecordId(23000+i));
    CHECK(exact.size()==MAX_LAYOUT_RECORDS);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan accepted=PlanAppReconcile(
        {bound},{duplicate},"chrome",now,exact,ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!accepted.deferred);
    CHECK(accepted.matches.empty());
    CHECK(accepted.restores.empty());
    CHECK(accepted.missingSavedIndices.empty());
    CHECK(accepted.newRecords.size()==1 && accepted.newRecords[0].liveIndex==0);
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==1);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_guaranteed_capacity_defers_before_matcher(){
    const UnixSeconds now=2000001685;
    const size_t chromeSavedCount=64;
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        const bool chrome=i>=MAX_LAYOUT_RECORDS-chromeSavedCount;
        LayoutWin record=ReconcileTestRecord(
            DeterministicRecordId(28000+i),chrome ? "chrome" : "firefox",
            chrome ? "Chrome" : "Firefox",chrome ? "same.example" : "ff.example",0,
            G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
        existing.push_back(record);
    }
    LayoutWin liveChrome=MatchRecord(
        "chrome","Chrome","same.example",1,{{"same.example",1}});
    liveChrome.deskIndex=0;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    std::vector<LayoutWin> overflowLive(chromeSavedCount+1,liveChrome);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan overflow=PlanAppReconcile(
        existing,overflowLive,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(overflow.app=="chrome" && overflow.nowUtc==now &&
        overflow.freshness==ReconcileFreshness::Fresh);
    CHECK(overflow.deferred);
    CHECK(overflow.matches.empty());
    CHECK(overflow.restores.empty());
    CHECK(overflow.newRecords.empty());
    CHECK(overflow.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    std::vector<LayoutWin> exactLive(chromeSavedCount,liveChrome);
    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan exact=PlanAppReconcile(
        existing,exactLive,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!exact.deferred);
    CHECK(exact.matches.size()==chromeSavedCount);
    CHECK(exact.restores.empty());
    CHECK(exact.newRecords.empty());
    CHECK(exact.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan noMatcher=PlanAppReconcile(
        {},{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,nullptr);
    CHECK(noMatcher.deferred);
    CHECK(noMatcher.matches.empty());
    CHECK(noMatcher.restores.empty());
    CHECK(noMatcher.newRecords.empty());
    CHECK(noMatcher.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static std::vector<LayoutWin> MissingProjectionCapacityRecords(UnixSeconds now){
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS-1;++i){
        existing.push_back(ReconcileTestRecord(
            DeterministicRecordId(40000+i),"firefox","Firefox","ff.example",0,
            G(L"{231A0000-0000-0000-0000-000000000001}"),now-10));
    }
    existing.push_back(ReconcileTestRecord(
        DeterministicRecordId(45000),"chrome","Old","old.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS));
    return existing;
}

static bool HasRecordId(const std::vector<LayoutWin>& records,
        const std::string& recordId){
    for(const LayoutWin& record : records)
        if(record.recordId==recordId) return true;
    return false;
}

static void test_reconcile_projects_mark_missing_expiration_before_capacity(){
    const UnixSeconds now=2100000000;
    const size_t oldIndex=MAX_LAYOUT_RECORDS-1;
    const std::string newId=DeterministicRecordId(MAX_LAYOUT_RECORDS+100);
    std::vector<LayoutWin> existing=MissingProjectionCapacityRecords(now);
    const std::vector<LayoutWin> originalExisting=existing;
    const std::string oldId=existing[oldIndex].recordId;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan planned=PlanAppReconcile(
        existing,{live},"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!planned.deferred);
    CHECK(planned.matches.empty() && planned.restores.empty());
    CHECK(planned.missingSavedIndices==std::vector<size_t>({oldIndex}));
    CHECK(planned.newRecords.size()==1 && planned.newRecords[0].liveIndex==0 &&
        planned.newRecords[0].recordId==newId);
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==1);
    CHECK(SameLayoutWinVectors(existing,originalExisting));

    std::vector<LayoutWin> committed=CommitAppReconcile(
        existing,{live},planned,{},now);
    CHECK(committed.size()==MAX_LAYOUT_RECORDS);
    CHECK(!HasRecordId(committed,oldId));
    CHECK(HasRecordId(committed,newId));

    ReconcilePlan handcrafted=ValidCommitPlan("chrome",now);
    handcrafted.missingSavedIndices.push_back(oldIndex);
    handcrafted.newRecords.push_back(PlannedNewRecord(0,newId));
    std::vector<LayoutWin> handcraftedCommit=CommitAppReconcile(
        existing,{live},handcrafted,{},now);
    CHECK(SameLayoutWinVectors(handcraftedCommit,committed));
    CHECK(handcraftedCommit.size()==MAX_LAYOUT_RECORDS);
    CHECK(!HasRecordId(handcraftedCommit,oldId));
    CHECK(HasRecordId(handcraftedCommit,newId));
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();

    struct RetainedControl {
        UnixSeconds lastSeenUtc;
        UnixSeconds missingSinceUtc;
    };
    const RetainedControl controls[]={
        {now-WINDOW_RETENTION_SECONDS+1,0},
        {0,0},
        {now-WINDOW_RETENTION_SECONDS,now-WINDOW_RETENTION_SECONDS+1}
    };
    for(const RetainedControl& control : controls){
        std::vector<LayoutWin> retained=existing;
        retained[oldIndex].lastSeenUtc=control.lastSeenUtc;
        retained[oldIndex].missingSinceUtc=control.missingSinceUtc;
        ResetCountingRecordIdGenerator();
        ResetCountingReconcileMatcher();
        ReconcilePlan rejected=PlanAppReconcile(
            retained,{live},"chrome",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator,CountingReconcileMatcher);
        CHECK(rejected.deferred);
        CHECK(rejected.matches.empty());
        CHECK(rejected.restores.empty());
        CHECK(rejected.newRecords.empty());
        CHECK(rejected.missingSavedIndices.empty());
        CHECK(CountingReconcileMatcherCalls()==1);
        CHECK(CountingRecordIdGeneratorCalls()==0);

        ReconcilePlan manualControl=ValidCommitPlan("chrome",now);
        manualControl.missingSavedIndices.push_back(oldIndex);
        manualControl.newRecords.push_back(PlannedNewRecord(0,newId));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(retained,{live},manualControl,{},now),retained));
    }
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_projected_retained_count_rejects_mismatched_flags(){
    const UnixSeconds now=2100000100;
    LayoutWin record=OldStyleRecord();
    record.lastSeenUtc=now-1;
    const std::vector<LayoutWin> existing={record};

    size_t retained=17;
    CHECK(!ProjectedRetainedExistingCount(
        existing,std::vector<bool>(),now,retained));
    CHECK(retained==17);

    retained=23;
    CHECK(!ProjectedRetainedExistingCount(
        existing,std::vector<bool>({false,true}),now,retained));
    CHECK(retained==23);
}

static void test_reconcile_duplicate_injected_match_ownership_defers_cleanly(){
    const UnixSeconds now=2000001690;
    LayoutWin savedA=ReconcileTestRecord(
        DeterministicRecordId(33000),"chrome","A","a.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin savedB=ReconcileTestRecord(
        DeterministicRecordId(33001),"chrome","B","b.example",1,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-10);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=savedA.deskIndex;
    liveA.desktop=savedA.desktop;
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=savedB.deskIndex;
    liveB.desktop=savedB.desktop;
    const ReconcileMatcher invalidMatchers[]={
        DuplicateSavedReconcileMatcher,DuplicateLiveReconcileMatcher
    };

    for(ReconcileMatcher matcher : invalidMatchers){
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            {savedA,savedB},{liveA,liveB},"chrome",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator,matcher);
        CHECK(plan.app=="chrome" && plan.nowUtc==now &&
            plan.freshness==ReconcileFreshness::Fresh);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
        ResetCountingRecordIdGenerator();
    }
}

static void test_reconcile_rejects_all_malformed_injected_matches(){
    const UnixSeconds now=2000001695;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(33002),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveChrome=MatchRecord(
        "chrome","Live","same.example",1,{{"same.example",1}});
    liveChrome.deskIndex=saved.deskIndex;
    liveChrome.desktop=saved.desktop;
    LayoutWin liveFirefox=MatchRecord(
        "firefox","Other","other.example",1,{{"other.example",1}});
    liveFirefox.deskIndex=1;
    liveFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const InjectedMatchMode invalidModes[]={
        InjectedMatchMode::SavedOutOfRange,InjectedMatchMode::LiveOutOfRange,
        InjectedMatchMode::OtherAppLive,InjectedMatchMode::NotANumber,
        InjectedMatchMode::Infinity,InjectedMatchMode::BelowThreshold,
        InjectedMatchMode::TooMany
    };

    for(InjectedMatchMode mode : invalidModes){
        CurrentInjectedMatchMode()=mode;
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            {saved},{liveChrome,liveFirefox},"chrome",now,{},
            ReconcileFreshness::Fresh,CountingRecordIdGenerator,
            ConfigurableReconcileMatcher);
        CHECK(plan.app=="chrome" && plan.nowUtc==now &&
            plan.freshness==ReconcileFreshness::Fresh);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
        ResetCountingRecordIdGenerator();
    }

    CurrentInjectedMatchMode()=InjectedMatchMode::Valid;
    ResetCountingRecordIdGenerator();
    ReconcilePlan valid=PlanAppReconcile(
        {saved},{liveChrome,liveFirefox},"chrome",now,{},
        ReconcileFreshness::Fresh,CountingRecordIdGenerator,
        ConfigurableReconcileMatcher);
    CHECK(!valid.deferred);
    CHECK(valid.matches.size()==1 && valid.matches[0].savedIndex==0 &&
        valid.matches[0].liveIndex==0 && valid.matches[0].score==0.55);
    CHECK(valid.restores.empty());
    CHECK(valid.newRecords.empty());
    CHECK(valid.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {saved},{liveChrome,liveFirefox},valid,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(committed[0].activeTitle==liveChrome.activeTitle);
    CHECK(committed[0].lastSeenUtc==now && committed[0].missingSinceUtc==0);
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_unsupported_app_defers_without_generation(){
    const UnixSeconds now=2000001700;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(10000),"firefox","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveOpera=MatchRecord("opera","New","new.example",1,{{"new.example",1}});
    liveOpera.deskIndex=1;
    liveOpera.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        {existing},{liveOpera},"opera",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(plan.app=="opera" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_commit_reconcile_rejects_out_of_range_mixed_plan_atomically(){
    const UnixSeconds now=2000001800;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10001),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=saved.deskIndex;
    live.desktop=saved.desktop;
    const std::vector<LayoutWin> existing={saved};
    const std::vector<LayoutWin> liveRecords={live};

    for(int variant=0;variant<3;++variant){
        ReconcilePlan plan=ValidCommitPlan("chrome",now);
        plan.matches.push_back(Candidate(0,0,1.0));
        if(variant==0) plan.missingSavedIndices.push_back(existing.size());
        if(variant==1) plan.newRecords.push_back(
            PlannedNewRecord(liveRecords.size(),DeterministicRecordId(11000)));
        if(variant==2) plan.matches.push_back(Candidate(existing.size(),0,1.0));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existing,liveRecords,plan,{},now),existing));
    }
}

static void test_commit_reconcile_rejects_malformed_restore_sets_atomically(){
    const UnixSeconds now=2000001850;
    LayoutWin savedA=ReconcileTestRecord(
        DeterministicRecordId(10010),"chrome","A","a.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin savedB=ReconcileTestRecord(
        DeterministicRecordId(10011),"chrome","B","b.example",1,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-20);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=2;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=3;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    const std::vector<LayoutWin> existing={savedA,savedB};
    const std::vector<LayoutWin> live={liveA,liveB};

    ReconcilePlan baseline=ValidCommitPlan("chrome",now);
    baseline.matches.push_back(Candidate(0,0,1.0));
    baseline.restores.push_back(PlannedRestore(0,0,savedA.desktop));
    std::vector<LayoutWin> accepted=CommitAppReconcile(existing,live,baseline,{0},now);
    CHECK(accepted.size()==2 && accepted[0].lastSeenUtc==now);

    for(int variant=0;variant<7;++variant){
        ReconcilePlan malformed=baseline;
        std::set<size_t> successful;
        if(variant==0) malformed.restores[0].savedIndex=existing.size();
        if(variant==1) malformed.restores[0].liveIndex=live.size();
        if(variant==2)
            malformed.restores[0]=PlannedRestore(1,1,savedB.desktop);
        if(variant==3) malformed.restores[0].destination=savedB.desktop;
        if(variant==4) malformed.restores.push_back(malformed.restores[0]);
        if(variant==5) malformed.restores.clear();
        if(variant==6) successful.insert(1);
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existing,live,malformed,successful,now),existing));
    }
    CHECK(SameLayoutWinVectors(existing,{savedA,savedB}));
    CHECK(SameLayoutWinVectors(live,{liveA,liveB}));
}

static void test_commit_reconcile_rejects_duplicate_match_ownership(){
    const UnixSeconds now=2000001900;
    LayoutWin first=ReconcileTestRecord(
        DeterministicRecordId(10002),"chrome","Same","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin second=first;
    second.recordId=DeterministicRecordId(10003);
    LayoutWin liveFirst=MatchRecord("chrome","Same","same.example",1,{{"same.example",1}});
    liveFirst.deskIndex=first.deskIndex;
    liveFirst.desktop=first.desktop;
    LayoutWin liveSecond=liveFirst;
    const std::vector<LayoutWin> existing={first,second};
    const std::vector<LayoutWin> live={liveFirst,liveSecond};

    ReconcilePlan duplicateSaved=ValidCommitPlan("chrome",now);
    duplicateSaved.matches={Candidate(0,0,1.0),Candidate(0,1,1.0)};
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,live,duplicateSaved,{},now),existing));

    ReconcilePlan duplicateLive=ValidCommitPlan("chrome",now);
    duplicateLive.matches={Candidate(0,0,1.0),Candidate(1,0,1.0)};
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,live,duplicateLive,{},now),existing));
}

static void test_commit_reconcile_rejects_app_mismatches(){
    const UnixSeconds now=2000002000;
    LayoutWin chromeSaved=ReconcileTestRecord(
        DeterministicRecordId(10004),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin chromeLive=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    chromeLive.deskIndex=chromeSaved.deskIndex;
    chromeLive.desktop=chromeSaved.desktop;
    LayoutWin firefoxSaved=chromeSaved;
    firefoxSaved.recordId=DeterministicRecordId(10005);
    firefoxSaved.app="firefox";
    LayoutWin firefoxLive=chromeLive;
    firefoxLive.app="firefox";

    ReconcilePlan planAppMismatch=ValidCommitPlan("firefox",now);
    planAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({chromeSaved},{chromeLive},planAppMismatch,{},now),
        {chromeSaved}));

    ReconcilePlan liveAppMismatch=ValidCommitPlan("chrome",now);
    liveAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({chromeSaved},{firefoxLive},liveAppMismatch,{},now),
        {chromeSaved}));

    ReconcilePlan existingAppMismatch=ValidCommitPlan("chrome",now);
    existingAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({firefoxSaved},{chromeLive},existingAppMismatch,{},now),
        {firefoxSaved}));
}

static void test_commit_reconcile_rejects_invalid_new_record_requests(){
    const UnixSeconds now=2000002100;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(0xABCDEF),"firefox","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=1;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=2;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    const std::vector<LayoutWin> existingRecords={existing};

    const std::string invalidIds[]={"not-a-guid","{00000000-0000-0000-0000-000000000000}"};
    for(const std::string& invalidId : invalidIds){
        ReconcilePlan plan=ValidCommitPlan("chrome",now);
        plan.newRecords.push_back(PlannedNewRecord(0,invalidId));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existingRecords,{liveA},plan,{},now),existingRecords));
    }

    ReconcilePlan collision=ValidCommitPlan("chrome",now);
    collision.newRecords.push_back(
        PlannedNewRecord(0,existing.recordId.substr(1,existing.recordId.size()-2)));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA},collision,{},now),existingRecords));

    ReconcilePlan duplicateIds=ValidCommitPlan("chrome",now);
    duplicateIds.newRecords={
        PlannedNewRecord(0,DeterministicRecordId(12000)),
        PlannedNewRecord(1,DeterministicRecordId(12000))
    };
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA,liveB},duplicateIds,{},now),
        existingRecords));

    ReconcilePlan duplicateLive=ValidCommitPlan("chrome",now);
    duplicateLive.newRecords={
        PlannedNewRecord(0,DeterministicRecordId(12001)),
        PlannedNewRecord(0,DeterministicRecordId(12002))
    };
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA,liveB},duplicateLive,{},now),
        existingRecords));
}

static void test_commit_reconcile_rejects_cached_stale_actions(){
    const UnixSeconds now=2000002200;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(10006),"chrome","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existingRecords={existing};

    ReconcilePlan staleNew=ValidCommitPlan(
        "chrome",now,ReconcileFreshness::CachedStale);
    staleNew.newRecords.push_back(PlannedNewRecord(0,DeterministicRecordId(12003)));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{live},staleNew,{},now),existingRecords));

    ReconcilePlan staleMissing=ValidCommitPlan(
        "chrome",now,ReconcileFreshness::CachedStale);
    staleMissing.missingSavedIndices.push_back(0);
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{},staleMissing,{},now),existingRecords));
}

static void test_commit_reconcile_rejects_planning_clock_mismatch(){
    const UnixSeconds now=2000002300;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10007),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=saved.deskIndex;
    live.desktop=saved.desktop;
    ReconcilePlan plan=ValidCommitPlan("chrome",now-1);
    plan.matches.push_back(Candidate(0,0,1.0));

    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({saved},{live},plan,{},now),{saved}));
}

static void test_reconcile_rejects_nonpositive_planning_clocks(){
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=0;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    const UnixSeconds invalidTimes[]={0,-1};

    for(UnixSeconds invalidNow : invalidTimes){
        ResetCountingRecordIdGenerator();
        ReconcilePlan planned=PlanAppReconcile(
            {},{live},"chrome",invalidNow,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator);
        CHECK(planned.app=="chrome" && planned.nowUtc==invalidNow &&
            planned.freshness==ReconcileFreshness::Fresh);
        CHECK(planned.deferred);
        CHECK(planned.matches.empty());
        CHECK(planned.restores.empty());
        CHECK(planned.newRecords.empty());
        CHECK(planned.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);

        LayoutWin saved=ReconcileTestRecord(
            DeterministicRecordId(10008),"chrome","Saved","same.example",0,
            live.desktop,2000002300);
        ReconcilePlan manual=ValidCommitPlan("chrome",invalidNow);
        manual.matches.push_back(Candidate(0,0,1.0));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile({saved},{live},manual,{},invalidNow),{saved}));
        ResetCountingRecordIdGenerator();
    }
}

static void test_reconcile_rejects_invalid_freshness_before_planning(){
    const UnixSeconds now=2000002350;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10009),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const ReconcileFreshness invalid=static_cast<ReconcileFreshness>(99);

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"chrome",now,{},invalid,CountingRecordIdGenerator);

    CHECK(plan.app=="chrome" && plan.nowUtc==now && plan.freshness==invalid);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_commit_reconcile_rejects_projected_output_overflow(){
    const UnixSeconds now=2000002400;
    LayoutWin base=ReconcileTestRecord(
        DeterministicRecordId(13000),"chrome","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=base;
        record.recordId=DeterministicRecordId(13000+i);
        existing.push_back(record);
    }
    LayoutWin live=MatchRecord("firefox","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    ReconcilePlan plan=ValidCommitPlan("firefox",now);
    plan.newRecords.push_back(
        PlannedNewRecord(0,DeterministicRecordId(13000+MAX_LAYOUT_RECORDS)));

    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,{live},plan,{},now),existing));
}

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
    std::vector<LayoutWin> acceptedWins={StrictV4Record()};
    std::string output="sentinel", error="stale";
    CHECK(BuildCheckedLayoutSnapshot(acceptedDesks,acceptedWins,1700000000,output,&error));
    CHECK(error.empty()); CHECK(output.find("# VDE snapshot v4\n")==0);
    CHECK(acceptedWins[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(acceptedWins[0].lastSeenUtc==1700000000);

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
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].recordId="not-a-guid";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="not-a-guid" && wins[0].lastSeenUtc==1700000000);
}

static void test_checked_snapshot_rejects_empty_id_and_zero_last_seen(){
    std::vector<LayoutWin> wins={OldStyleRecord()};
    wins[0].lastSeenUtc=1700000000;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty() && output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId.empty() &&
          wins[0].lastSeenUtc==1700000000);

    wins={StrictV4Record()};
    wins[0].lastSeenUtc=0;
    error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty() && output=="prior snapshot bytes");
    CHECK(wins[0].recordId=="{00000000-0000-0000-0000-000000000101}" &&
          wins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_zero_record_id_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].recordId="{00000000-0000-0000-0000-000000000000}";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000000}" && wins[0].lastSeenUtc==1700000000);
}

static void test_checked_snapshot_rejects_duplicate_record_ids_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record(),StrictV4Record()};
    wins[0].recordId="{AAAAAAAA-BBBB-CCCC-DDDD-000000000001}";
    wins[1].recordId="aaaaaaaa-bbbb-cccc-dddd-000000000001";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==2 && wins[0].lastSeenUtc==1700000000 && wins[1].lastSeenUtc==1700000000);
    CHECK(wins[0].recordId.front()=='{' && wins[1].recordId.front()=='a');
}

static void test_checked_snapshot_rejects_negative_missing_since_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].missingSinceUtc=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000101}" &&
          wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==-1);
}

static void test_checked_snapshot_accepts_supported_browser_apps(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].app=app;
        std::string output="sentinel", error="stale";
        CHECK(BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
        CHECK(error.empty()); CHECK(output.find(std::string("W\t")+app+"\t")!=std::string::npos);
    }
}

static void test_checked_snapshot_rejects_unsupported_app_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].app="opera";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_negative_tab_count_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].tabCount=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_invalid_counts_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].counts={{"mail.example",0}};
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].counts={{"mail.example,evil",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].counts={{"mail.example\tinjected",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].activeDomain="mail.example\tinjected";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].activeDomain="mail.example\ninjected"; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
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

static void test_lc_deferred_backoff_distinguishes_exact_max_from_overflow(){
    const uint64_t exactStart=UINT64_MAX-30000;
    LcState exact;
    LcDecision exactWave=lc_begin_initial(exact,1,10,100,20,0);
    LcRestoreCompleted(exact,exactWave.generation,LcRestoreOutcome::Deferred,
                       100,20,exactStart);
    CHECK(exact.retryNotBeforeMs==UINT64_MAX &&
          !exact.deferredUntilInputChanges);
    CHECK(LcObserve(exact,true,1,10,100,20,UINT64_MAX-1).action==LcAction::None);
    CHECK(LcObserve(exact,true,1,10,100,20,UINT64_MAX).action==LcAction::BeginRestore);

    const uint64_t overflowStart=exactStart+1;
    LcState overflow;
    LcDecision overflowWave=lc_begin_initial(overflow,1,10,100,20,0);
    LcRestoreCompleted(overflow,overflowWave.generation,LcRestoreOutcome::Deferred,
                       100,20,overflowStart);
    CHECK(LcObserve(overflow,true,1,10,100,20,UINT64_MAX).action==LcAction::None);

    LcState released;
    LcDecision releasedWave=lc_begin_initial(released,1,10,100,20,0);
    LcRestoreCompleted(released,releasedWave.generation,LcRestoreOutcome::Deferred,
                       100,20,overflowStart);
    CHECK(LcObserve(released,true,2,11,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(released,true,2,11,100,20,UINT64_MAX).action==LcAction::BeginRestore);
}

static void test_lc_deferred_rollback_to_unrepresentable_deadline_fails_closed(){
    const uint64_t exactStart=UINT64_MAX-30000;
    const uint64_t rolledBackOrigin=exactStart+1;
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                       100,20,exactStart);
    CHECK(LcObserve(s,true,1,10,100,20,UINT64_MAX-1).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,rolledBackOrigin).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(s,true,2,11,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(s,true,2,11,100,20,UINT64_MAX).action==LcAction::BeginRestore);
}

static void test_lc_deferred_budget_resets_on_new_source_stamp(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    uint64_t completedAt=100;
    const uint64_t sources[4]={99,100,100,100};
    const int expectedAttempts[4]={1,1,2,3};
    for(int attempt=0;attempt<4;++attempt){
        LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                           100,sources[attempt],completedAt);
        CHECK(s.sessionStampSignature==20 &&
              s.deferredAttempts==expectedAttempts[attempt]);
        CHECK(s.deferredWindowSetSignature==1 &&
              s.deferredSessionStampSignature==sources[attempt]);
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

static void test_lc_cancelled_stale_reconcile_retires_exact_flight_and_rearms(){
    LcState state;
    LcDecision wave=lc_begin_initial(state,1,10,100,20,0);
    CHECK(!LcCancelRestore(state,wave.generation+1,2,true));
    CHECK(state.restoreInFlight && state.inFlightGeneration==wave.generation);
    CHECK(LcCancelRestore(state,wave.generation,3,true));
    CHECK(!state.restoreInFlight && state.inFlightGeneration==0);
    CHECK(state.restorePending && state.stableSnapshots==0);
    CHECK(LcObserve(state,true,1,10,100,20,4).action==LcAction::None);
    LcDecision rearmed=LcObserve(state,true,1,10,100,20,5);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=wave.generation);

    CHECK(LcCancelRestore(state,rearmed.generation,6,false));
    CHECK(!state.restoreInFlight && !state.restorePending);
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

static void test_lc_explicit_save_completion_commits_captured_layout_only(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save101=LcObserve(s,true,1,10,101,20,3);
    CHECK(save101.action==LcAction::SaveLayout &&
          s.saveRequestedLayoutSignature==101);
    CHECK(LcObserve(s,true,1,10,102,20,4).action==LcAction::None);
    LcExplicitSaveCompleted(s,save101.generation,102,20,5);
    LcDecision save102=LcObserve(s,true,1,10,102,20,6);
    CHECK(save102.action==LcAction::SaveLayout &&
          save102.generation!=save101.generation);
}

static void test_lc_explicit_save_completion_rebases_pending_wave_on_rollback(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save=LcObserve(s,true,1,10,101,20,1000);
    CHECK(save.action==LcAction::SaveLayout);
    CHECK(LcObserve(s,true,2,20,101,21,100000).action==LcAction::None);
    CHECK(s.saveInFlight && s.restorePending);
    LcExplicitSaveCompleted(s,save.generation,101,21,0);
    CHECK(LcObserve(s,true,2,21,101,21,19999).action==LcAction::None);
    LcDecision timeout=LcObserve(s,true,2,22,101,21,20000);
    CHECK(timeout.action==LcAction::BeginRestore && timeout.generation!=0);
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

    RestoreBudgets prepared;
    CHECK(prepared.prepareTerminalInsert());
    RestoreBudgetKey preparedKey{"record-prepared","runtime-prepared","desktop"};
    CHECK(prepared.markExhaustedPrepared(std::move(preparedKey)));
    CHECK(!prepared.mayAttempt(
        {"record-prepared","runtime-prepared","desktop"}));
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

static void test_restore_budgets_new_key_copy_failure_is_transactional(){
    for(int fault=0;fault<2;++fault){
        bool inject=true;
        const RestoreBudgetKey rejected=numbered_budget_key(256);
        RestoreBudgetOps ops;
        ops.copyKey=[&](const RestoreBudgetKey& key)->RestoreBudgetKey{
            if(inject && key==rejected){
                if(fault==0) throw std::bad_alloc();
                throw std::length_error("injected restore-budget copy fault");
            }
            return key;
        };
        RestoreBudgets budgets(ops);
        for(int i=0;i<256;++i) budgets.markExhausted(numbered_budget_key(i));
        CHECK(!budgets.mayAttempt(numbered_budget_key(0))); // LRU is now key 1

        bool caught=false;
        try {
            budgets.markExhausted(rejected);
        } catch(const std::bad_alloc&) {
            caught=fault==0;
        } catch(const std::length_error&) {
            caught=fault==1;
        } catch(...) {
            caught=false;
        }
        CHECK(caught && budgets.size()==256);
        CHECK(!budgets.mayAttempt(numbered_budget_key(1))); // old key survived
        CHECK(budgets.mayAttempt(rejected));                // new key was not committed

        inject=false;
        budgets.markExhausted(numbered_budget_key(257));
        CHECK(budgets.size()==256);
        CHECK(budgets.mayAttempt(numbered_budget_key(2)));  // order survived; key 1 was touched
        CHECK(!budgets.mayAttempt(numbered_budget_key(3)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(0)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(1)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(257)));
    }
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

    HANDLE delay=CreateWaitableTimerW(nullptr,TRUE,nullptr);
    CHECK(delay!=nullptr);
    if(delay){
        LARGE_INTEGER due{};
        due.QuadPart=-20LL*10000LL;
        CHECK(SetWaitableTimer(delay,&due,0,nullptr,nullptr,FALSE)!=FALSE);
        CHECK(WaitForSingleObject(delay,1000)==WAIT_OBJECT_0);
        CloseHandle(delay);
    }
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

static void test_manual_operation_profiles_remain_captured_across_settings_changes(){
    AppProfile original=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    original.classNames.push_back(L"CapturedClass");
    original.titleSuffixes.push_back(L" CapturedSuffix");
    original.userDataDir=L"captured-data";
    std::vector<AppProfile> active={original};
    OperationAppProfiles captured(active);

    active[0].classNames.clear();
    active[0].exeName=L"changed.exe";
    active[0].titleSuffixes.clear();
    active[0].session=AppProfile::NONE;
    active[0].userDataDir=L"changed-data";
    active.clear();

    const AppProfile* retained=captured.find("chrome");
    CHECK(retained && SessionProfilesEqual(*retained,original));
    CHECK(captured.find("firefox")==nullptr);
    CHECK(captured.all().size()==1);
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

static void test_picker_uses_self_contained_gdi_buffer(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    CHECK(source.find("#include \"gdi_buffer.hpp\"")!=std::string::npos);
    CHECK(source.find("static GdiBuffer g_pickerBuffer;")!=std::string::npos);
    CHECK(source.find("class PickerBackBuffer")==std::string::npos);
    CHECK(source.find("CreateCompatibleBitmap")==std::string::npos);
}

static std::string SourceSection(const std::string& source,
                                 const std::string& begin,
                                 const std::string& end){
    const size_t first=source.find(begin);
    if(first==std::string::npos) return {};
    const size_t last=source.find(end,first+begin.size());
    if(last==std::string::npos || last<=first) return {};
    return source.substr(first,last-first);
}

static void test_picker_icon_loading_is_bounded_and_outside_paint(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    CHECK(source.find("#include \"icon_cache.hpp\"")!=std::string::npos);
    CHECK(source.find("static OwnedIconCache g_windowIconCache(256")!=
          std::string::npos);
    CHECK(source.find("static HICON WindowIcon")==std::string::npos);
    CHECK(source.find("SMTO_ABORTIFHUNG,200")==std::string::npos);

    const std::string row=SourceSection(
        source,"struct WinItem {","struct Tile {");
    CHECK(!row.empty());
    CHECK(row.find("std::string runtimeKey")!=std::string::npos);
    CHECK(row.find("HICON icon")==std::string::npos);

    const std::string enumerate=SourceSection(
        source,"static BOOL CALLBACK EnumAll(",
        "static bool PopulatePickerFilteredRows(");
    CHECK(!enumerate.empty());
    CHECK(enumerate.find("RuntimeKey(fast)")!=std::string::npos);
    CHECK(enumerate.find("liveKeys")!=std::string::npos);
    CHECK(enumerate.find("WM_GETICON")==std::string::npos);
    CHECK(enumerate.find("GetClassLongPtrW")==std::string::npos);
    CHECK(enumerate.find("LoadWindowIconOutsidePaint")==std::string::npos);

    const std::string loader=SourceSection(
        source,"static HICON LoadWindowIconOutsidePaint(",
        "static HICON CachedWindowIcon(");
    CHECK(!loader.empty());
    CHECK(loader.find("getAndTouch")!=std::string::npos);
    CHECK(loader.find("GetClassLongPtrW")!=std::string::npos);
    CHECK(loader.find("WM_GETICON,ICON_SMALL2")!=std::string::npos);
    CHECK(loader.find("SMTO_ABORTIFHUNG|SMTO_BLOCK,25")!=
          std::string::npos);
    const size_t firstIdentity=loader.find(
        "RecaptureGenericWindowIdentity");
    const size_t timeout=loader.find("SendMessageTimeoutW");
    const size_t secondIdentity=loader.find(
        "RecaptureGenericWindowIdentity",firstIdentity+1);
    const size_t publish=loader.find("insertBorrowed");
    CHECK(firstIdentity!=std::string::npos && timeout!=std::string::npos &&
          secondIdentity!=std::string::npos && publish!=std::string::npos);
    CHECK(firstIdentity<timeout && timeout<secondIdentity &&
          secondIdentity<publish);

    const std::string paint=SourceSection(
        source,"static void Paint(","static void TipDeactivate(");
    CHECK(!paint.empty());
    CHECK(paint.find("CachedWindowIcon(window.runtimeKey)")!=
          std::string::npos);
    CHECK(paint.find("LoadWindowIconOutsidePaint")==std::string::npos);
    CHECK(paint.find("RuntimeKey(")==std::string::npos);
    CHECK(paint.find("SendMessageTimeoutW")==std::string::npos);
    CHECK(paint.find("GetClassLongPtrW")==std::string::npos);
    CHECK(paint.find("getAndTouch")==std::string::npos);
    CHECK(paint.find("insertBorrowed")==std::string::npos);
    CHECK(paint.find("LowerW(")==std::string::npos);
    CHECK(paint.find("EnsureTabSearch")==std::string::npos);

    const std::string build=SourceSection(
        source,"static bool BuildModel(",
        "static bool SetPickerSelectionCurrent(");
    CHECK(!build.empty());
    CHECK(build.find("if(published) PruneIconCache(liveKeys);")!=
          std::string::npos);
}

static void test_picker_preloads_only_laid_out_visible_rows(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    const std::string preload=SourceSection(
        source,"static void PreloadVisiblePickerIcons(",
        "class ScopedPickerMeasureDc");
    CHECK(!preload.empty());
    CHECK(preload.find("LoadWindowIconOutsidePaint(window)")!=
          std::string::npos);
    CHECK(preload.find("g_pickerIconPreloadGate.runTurn(")!=
          std::string::npos);
    CHECK(preload.find("PICKER_ICON_PRELOAD_MISS_BUDGET")!=
          std::string::npos);

    const std::string queued=SourceSection(
        source,"static bool BuildPickerIconPreloadQueue() noexcept {",
        "static void SchedulePickerIconPreloadContinuation() noexcept {");
    CHECK(!queued.empty());
    CHECK(queued.find("tile.filtered[position]")!=std::string::npos);
    CHECK(queued.find("PickerTileVisibleRows(tile)")!=std::string::npos);
    CHECK(queued.find("PickerVisibleScroll(")!=std::string::npos);
    CHECK(queued.find("PICKER_ICON_CACHE_LIMIT")!=std::string::npos);

    const std::string refresh=SourceSection(
        source,"static bool RefreshPickerPaintCache(\n"
               "        bool allowHiddenPreparation) noexcept {",
        "struct PickerLightweightSnapshot");
    CHECK(!refresh.empty());
    const size_t layout=refresh.find("LayoutTiles(client.right)");
    const size_t clamp=refresh.find("ClampAllPickerScrolls()");
    const size_t preloaded=refresh.find("PreloadVisiblePickerIcons()");
    const size_t rebuild=refresh.find("RebuildPickerPaintCache(");
    CHECK(layout!=std::string::npos && clamp!=std::string::npos &&
          preloaded!=std::string::npos && rebuild!=std::string::npos);
    CHECK(layout<clamp && clamp<preloaded && preloaded<rebuild);
    CHECK(source.find("bool allowHiddenPreparation=false")!=
          std::string::npos);
    CHECK(refresh.find("IsWindowVisible(g_main)")!=std::string::npos);
    CHECK(refresh.find("CancelPickerIconPreload(g_main)")!=
          std::string::npos);

    const std::string clampRows=SourceSection(
        source,"static void ClampAllPickerScrolls() noexcept {",
        "static bool BuildPickerIconPreloadQueue() noexcept {");
    CHECK(!clampRows.empty());
    CHECK(clampRows.find(
        "for(size_t index=0;index<g_tiles.size();++index)")!=
          std::string::npos);
    CHECK(clampRows.find("RememberPickerScroll(tile,clamped)")!=
          std::string::npos);
    CHECK(clampRows.find("PICKER_ICON_PRELOAD_MISS_BUDGET")==
          std::string::npos);

    const std::string filtering=SourceSection(
        source,"static bool RebuildPickerFilteredRows() noexcept {",
        "static void ResetPickerHoverTooltip()");
    CHECK(filtering.find("MarkPickerIconPreloadDirty()")!=
          std::string::npos);
    const std::string model=SourceSection(
        source,"static bool BuildModel(",
        "static bool SetPickerSelectionCurrent(");
    CHECK(model.find("MarkPickerIconPreloadDirty()")!=
          std::string::npos);
    const std::string wheel=SourceSection(
        source,"case WM_MOUSEWHEEL:","case WM_COMMAND:");
    CHECK(wheel.find("MarkPickerIconPreloadDirty(")!=
          std::string::npos);

    const std::string selection=SourceSection(
        source,"static bool SetPickerSelectionCurrent(",
        "static bool RememberPickerScroll(");
    CHECK(selection.find("MarkPickerIconPreloadDirty(index)")!=
          std::string::npos);

    const std::string timers=SourceSection(
        source,"case WM_TIMER:","case WM_QUERYENDSESSION:");
    CHECK(timers.find("TIMER_PICKER_ICON_PRELOAD")!=std::string::npos);
    CHECK(timers.find("PreloadVisiblePickerIcons(true)")!=
          std::string::npos);
    CHECK(timers.find("IsWindowVisible(hwnd)")!=std::string::npos);
    CHECK(timers.find("CancelPickerIconPreload(hwnd)")!=
          std::string::npos);

    const std::string cancellation=SourceSection(
        source,"static void CancelPickerIconPreload(HWND window) noexcept {",
        "static bool QuiesceRuntime(");
    CHECK(!cancellation.empty());
    CHECK(cancellation.find("KillTimer(window,TIMER_PICKER_ICON_PRELOAD)")!=
          std::string::npos);
    CHECK(cancellation.find("g_pickerIconPreloadGate.cancel()")!=
          std::string::npos);
    CHECK(cancellation.find("g_pickerIconPreloadQueue.clear()")!=
          std::string::npos);
    const std::string quiesce=SourceSection(
        source,"static bool QuiesceRuntime(","static int TILE_W=");
    CHECK(quiesce.find("CancelPickerIconPreload(messageWindow)")!=
          std::string::npos);
    const std::string controlledHide=SourceSection(
        source,"case PickerEffectKind::Hide:",
        "case PickerEffectKind::ReportFailure:");
    CHECK(controlledHide.find("CancelPickerIconPreload(g_main)")!=
          std::string::npos);
    const std::string idleHide=SourceSection(
        source,"static void HidePicker(){","// Search EDIT subclass");
    CHECK(idleHide.find("CancelPickerIconPreload(g_main)")!=
          std::string::npos);

    const std::string show=SourceSection(
        source,"static void ShowPicker(","static void MoveSel(");
    const size_t showDirty=show.rfind("MarkPickerIconPreloadDirty(");
    const size_t showRefresh=show.rfind("RefreshPickerPaintCache(true)");
    CHECK(showDirty!=std::string::npos && showRefresh!=std::string::npos &&
          showDirty<showRefresh);
}

static void test_picker_wm_paint_requires_the_owned_buffer(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    const std::string message=SourceSection(
        source,"case WM_PAINT:","case WM_ERASEBKGND:");
    CHECK(!message.empty());
    CHECK(message.find("RECT client={0,0,0,0};")!=std::string::npos);
    CHECK(message.find("if(!GetClientRect(hwnd,&client)) return 0;")!=
          std::string::npos);
    CHECK(message.find(
        "if(!g_pickerBuffer.ensure(target,client.right,client.bottom))")!=
          std::string::npos);
    CHECK(message.find("Paint(target,g_pickerBuffer.get(),client);")!=
          std::string::npos);
    CHECK(message.find("HDC canvas=target")==std::string::npos);
}

static size_t CountSourceText(const std::string& source,
                              const std::string& needle){
    size_t count=0;
    size_t position=0;
    while((position=source.find(needle,position))!=std::string::npos){
        ++count;
        position+=needle.size();
    }
    return count;
}

static void test_cli_list_uses_one_atomic_desktop_snapshot(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    const std::string cli=SourceSection(
        source,"static int CliRun(const std::wstring& cmd){",
        "// ================================ GUI: picker");
    CHECK(!cli.empty());
    CHECK(CountSourceText(cli,"CurrentDesktops(")==1);
    CHECK(cli.find("g_vdmi->GetCount(")==std::string::npos);
    CHECK(cli.find("GetDesktopByIndex(")==std::string::npos);
    CHECK(cli.find("->GetID(")==std::string::npos);
    const size_t snapshot=cli.find(
        "if(!CurrentDesktops(desks,&desktopError))");
    const size_t output=cli.find("printf(\"Virtual desktops: %u\\n\"");
    CHECK(snapshot!=std::string::npos && output!=std::string::npos &&
          snapshot<output);
    CHECK(cli.find("return 1;",snapshot)<output);
}

static void test_ui_resources_have_one_owned_cleanup_path(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    CHECK(source.find("static HBRUSH g_searchBrush=nullptr;")!=
          std::string::npos);
    CHECK(source.find("static HBRUSH sbr")==std::string::npos);
    CHECK(source.find("static std::vector<HICON> g_ownedIcons;")!=
          std::string::npos);
    CHECK(CountSourceText(source,"InitMetrics();")==1);
    CHECK(source.find("std::terminate()") == std::string::npos);
    const std::string metrics=SourceSection(
        source,"static void InitMetrics(){","// ---- picker search");
    CHECK(metrics.find("g_ownedIcons.reserve(MAX_OWNED_APP_ICONS)")!=
          std::string::npos);

    const std::string track=SourceSection(
        source,"static HICON TrackOwnedAppIcon(",
        "static HICON LoadAppIcon(");
    CHECK(!track.empty());
    CHECK(track.find("g_ownedIcons.push_back(icon)")!=std::string::npos);
    CHECK(track.find("DestroyIcon(icon)")!=std::string::npos);
    CHECK(track.find("g_failedOwnedIconReleases.retain(icon)")!=
          std::string::npos);

    const std::string load=SourceSection(
        source,"static HICON LoadAppIcon(","static void TrayAdd(");
    CHECK(!load.empty());
    CHECK(CountSourceText(load,"TrackOwnedAppIcon(")>=3);
    CHECK(load.find("CopyIcon(fallback)")!=std::string::npos);
    CHECK(load.find("return LoadIconW(")==std::string::npos);
    const size_t capacityCheck=load.find("OwnedAppIconCapacityAvailable()");
    const size_t firstLoad=load.find("LoadImageW(");
    CHECK(capacityCheck!=std::string::npos && firstLoad!=std::string::npos &&
          capacityCheck<firstLoad);

    const std::string cleanup=SourceSection(
        source,"static bool CleanupUiResources() noexcept {",
        "static bool DestroyUiWindow(HWND& window) noexcept {");
    CHECK(!cleanup.empty());
    const size_t buffer=cleanup.find("g_pickerBuffer.reset()");
    const size_t iconCache=cleanup.find("ClearWindowIconCache()");
    const size_t fonts=cleanup.find("HFONT* fonts[]");
    const size_t brush=cleanup.find("DeleteObject(g_searchBrush)");
    const size_t icons=cleanup.find("for(HICON icon : g_ownedIcons)");
    CHECK(buffer!=std::string::npos && iconCache!=std::string::npos &&
          fonts!=std::string::npos && brush!=std::string::npos &&
          icons!=std::string::npos);
    CHECK(buffer<iconCache && iconCache<fonts && fonts<brush && brush<icons);
    CHECK(cleanup.find("if(!g_pickerBuffer.released()) return false;")!=
          std::string::npos);
    CHECK(cleanup.find("if(!ClearWindowIconCache()) return false;")!=
          std::string::npos);
    CHECK(cleanup.find("g_ownedIcons.clear()")!=std::string::npos);
    CHECK(cleanup.find("g_failedOwnedIconReleases.clear(")!=
          std::string::npos);
    CHECK(cleanup.find("g_nid.hIcon=nullptr")!=std::string::npos);

    const std::string teardown=SourceSection(
        source,"static bool DestroyUiWindow(HWND& window) noexcept {",
        "class ScopedUiShutdown");
    CHECK(!teardown.empty());
    CHECK(teardown.find("!DestroyWindow(owned)")!=
          std::string::npos);
    const size_t destroySettings=teardown.find("DestroyUiWindow(g_settings)");
    const size_t destroyMain=teardown.find("DestroyUiWindow(g_main)");
    const size_t unregisterSettings=teardown.find(
        "UnregisterUiClass(L\"VdeSettings\"");
    const size_t unregisterMain=teardown.find(
        "UnregisterUiClass(L\"VdeWindow\"");
    const size_t cleanupCall=teardown.find("CleanupUiResources()");
    CHECK(destroySettings!=std::string::npos &&
          destroyMain!=std::string::npos &&
          unregisterSettings!=std::string::npos &&
          unregisterMain!=std::string::npos &&
          cleanupCall!=std::string::npos);
    CHECK(destroySettings<destroyMain && destroyMain<unregisterSettings &&
          unregisterSettings<unregisterMain && unregisterMain<cleanupCall);
    CHECK(CountSourceText(source,"CleanupUiResources();")==1);
    CHECK(teardown.find("g_uiTeardown.run(")!=std::string::npos);
    CHECK(teardown.find("g_uiShutdownComplete=true;")==std::string::npos);
}

static void test_message_pump_failure_uses_shared_teardown(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    const std::string run=SourceSection(
        source,"static int RunGui(HINSTANCE hInst)","int WINAPI wWinMain(");
    CHECK(!run.empty());
    CHECK(run.find("ScopedUiShutdown shutdown;")!=std::string::npos);
    CHECK(run.find("if(wait==WAIT_FAILED){")!=std::string::npos);
    CHECK(run.find("GetLastError()")!=std::string::npos);
    CHECK(run.find("runResult=4")!=std::string::npos);
    CHECK(run.find("if(g_uiFont)DeleteObject(g_uiFont)")==
          std::string::npos);

    const std::string mainProcedure=SourceSection(
        source,"static LRESULT CALLBACK WndProcImpl(",
        "static LRESULT CALLBACK WndProc(");
    CHECK(!mainProcedure.empty());
    CHECK(mainProcedure.find("case WM_DESTROY:")!=std::string::npos);
    CHECK(mainProcedure.find("g_pickerBuffer.reset()")==std::string::npos);
}

static void test_visible_branding_and_help_retention_are_exact(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    const std::string retention=
        "A closed Firefox, Chrome, or Edge window keeps its remembered virtual desktop for 30 days. If it reappears before expiry, VDE restores it before updating the saved layout.";
    CHECK(!source.empty());
    CHECK(source.find(retention)!=std::string::npos);
    CHECK(source.find("a few runs")==std::string::npos);
    CHECK(source.find("Virtual Desktop Extension")!=std::string::npos);
    CHECK(source.find("Virtual Desktops Extension")==std::string::npos);
    CHECK(source.find("Virtual Desktops Extention")==std::string::npos);
    CHECK(source.find("Virtual Desktop Extention")==std::string::npos);
}

static void test_picker_search_retry_uses_a_distinct_timer_channel(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    CHECK(source.find("#define TIMER_PICKER_SEARCH_RETRY 7")!=
          std::string::npos);
    CHECK(source.find("SetTimer(g_main,TIMER_PICKER_SEARCH_RETRY,1,nullptr)")!=
          std::string::npos);
    CHECK(source.find("wp==TIMER_PICKER_SEARCH_RETRY")!=
          std::string::npos);
    CHECK(source.find("KillTimer(messageWindow,TIMER_PICKER_SEARCH_RETRY)")!=
          std::string::npos);
    CHECK(source.find("RunPickerTabSearchEnsureAttempt(")!=
          std::string::npos);
    CHECK(source.find("AcquirePickerTabSearchRetryPostLeaseWhenIdle(")!=
          std::string::npos);
    CHECK(source.find("PickerTabSearchRetryDeliveryReadyWhenIdle(")!=
          std::string::npos);
}

static void test_picker_close_route_is_used_by_the_window_procedure(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    CHECK(source.find("RoutePickerClose(g_picker)")!=std::string::npos);
    CHECK(source.find(
        "case WM_CLOSE:\r\n        if(g_picker.controlledTransition())")==
        std::string::npos);
    CHECK(source.find(
        "case WM_CLOSE:\n        if(g_picker.controlledTransition())")==
        std::string::npos);
}

static void test_picker_persistence_transaction_is_used_by_save(){
    const std::string source=ReadRawFile(L"src\\vde.cpp");
    const std::string header=ReadRawFile(L"src\\picker_state.hpp");
    CHECK(!source.empty());
    CHECK(!header.empty());
    CHECK(source.find("RunPickerPersistenceTransaction(")!=
          std::string::npos);
    CHECK(source.find("CleanupPickerTabSearchPublishedOperation(")!=
          std::string::npos);
    CHECK(header.find("TryStagePickerPersistenceAppNoThrow(")!=
          std::string::npos);
    CHECK(header.find("TryStagePickerPersistenceAppNoThrow(result,app")!=
          std::string::npos);
    const size_t transaction=source.find(
        "return RunPickerPersistenceTransaction(");
    const size_t appStaged=source.find(
        "TryStagePickerPersistenceAppNoThrow(result,exactApp)",
        transaction);
    const size_t firstMutationBoundary=source.find(
        "const std::string runtimeKey=RuntimeKey(identity);",transaction);
    CHECK(transaction!=std::string::npos);
    CHECK(appStaged!=std::string::npos);
    CHECK(firstMutationBoundary!=std::string::npos);
    CHECK(transaction<appStaged && appStaged<firstMutationBoundary);
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

static void test_durable_write_captures_revision_without_post_publish_read(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string bytes=ValidLayoutBytes("captured-revision");
    std::string error;
    LayoutFsOps ops;
    const auto realRead=ops.readFile;
    bool atomicReturned=false;
    int readsAfterAtomicReturn=0;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,
                     DWORD& read)->BOOL {
        if(atomicReturned){
            ++readsAfterAtomicReturn;
            throw std::bad_alloc();
        }
        return realRead(handle,buffer,requested,read);
    };
    LayoutRevision current;
    current.sourcePath=L"old-revision";
    current.exists=true;
    bool dirty=true;
    bool pendingPublication=false;
    CHECK(PublishLayoutWithCapturedRevision(
        [&](LayoutRevision& published){
            const bool wrote=AtomicWriteText(
                primary,bytes,&error,false,ops,&published);
            atomicReturned=wrote;
            return wrote;
        },
        [&](LayoutRevision&) noexcept { pendingPublication=true; },
        [&](LayoutRevision& published) noexcept {
            CommitPublishedLayoutRevisionNoThrow(current,published);
            dirty=false;
        })==CapturedLayoutPublishResult::Succeeded);
    CHECK(atomicReturned && error.empty() && !dirty && !pendingPublication);
    CHECK(readsAfterAtomicReturn==0);
    const LayoutRevision actual=ReadLayoutRevisionLocked(primary);
    CHECK(SameRevision(current,actual));
    CHECK(ReadRawFile(primary)==bytes);
}

static void test_durable_publish_exception_adopts_revision_then_retries(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string prior=ValidLayoutBytes("prior-captured");
    const std::string next=ValidLayoutBytes("next-captured");
    std::string error;
    CHECK(AtomicWriteText(primary,prior,&error));
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    bool dirty=true,pendingAdopted=false;
    LayoutFsOps throwingOps;
    throwingOps.deleteFile=[](const std::wstring&)->BOOL {
        throw std::bad_alloc();
    };
    const CapturedLayoutPublishResult first=
        PublishLayoutWithCapturedRevision(
            [&](LayoutRevision& published){
                return AtomicWriteText(
                    primary,next,&error,true,throwingOps,&published);
            },
            [&](LayoutRevision& published) noexcept {
                pendingAdopted=true;
                CommitPublishedLayoutRevisionNoThrow(current,published);
            },
            [&](LayoutRevision&) noexcept { dirty=false; });
    CHECK(first==CapturedLayoutPublishResult::PublishedNeedsRetry);
    CHECK(pendingAdopted && dirty && ReadRawFile(primary)==next);
    CHECK(SameRevision(current,ReadLayoutRevisionLocked(primary)));

    const CapturedLayoutPublishResult retry=
        PublishLayoutWithCapturedRevision(
            [&](LayoutRevision& published){
                return AtomicWriteText(
                    primary,next,&error,true,&published);
            },
            [&](LayoutRevision& published) noexcept {
                CommitPublishedLayoutRevisionNoThrow(current,published);
            },
            [&](LayoutRevision& published) noexcept {
                CommitPublishedLayoutRevisionNoThrow(current,published);
                dirty=false;
            });
    CHECK(retry==CapturedLayoutPublishResult::Succeeded);
    CHECK(!dirty && SameRevision(current,ReadLayoutRevisionLocked(primary)));
}

static void test_first_post_publish_verify_throw_recovers_from_armed_candidate(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string prior=ValidLayoutBytes("candidate-prior");
    const std::string next=ValidLayoutBytes("candidate-next");
    std::string error;
    CHECK(AtomicWriteText(primary,prior,&error));
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    const LayoutRevision priorRevision=current;
    LayoutPublishCandidate candidate;
    CHECK(BuildLayoutPublishCandidate(primary,next,candidate));
    CHECK(candidate.armed);

    LayoutFsOps ops;
    const auto realOpen=ops.openFile;
    int primaryReadOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                     DWORD creation,DWORD flags)->HANDLE {
        if(opened==primary && creation==OPEN_EXISTING &&
           ++primaryReadOpens==2) throw std::bad_alloc();
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutRevision uncaptured;
    bool returned=false;
    try {
        returned=AtomicWriteText(
            primary,next,&error,true,ops,&uncaptured);
    } catch(...) { returned=false; }
    CHECK(!returned && !uncaptured.exists);
    CHECK(ReadRawFile(primary)==next);

    LayoutRevision unavailable;
    unavailable.sourcePath=primary;
    unavailable.exists=true;
    CHECK(ObserveLayoutPublishCandidateNoThrow(
              candidate,LayoutRevisionReadStatus::Unavailable,
              unavailable,current)==
          LayoutPublishCandidateObservation::RetainedUnavailable);
    CHECK(candidate.armed && SameRevision(current,priorRevision));

    LayoutRevision exact=ReadLayoutRevisionLocked(primary);
    CHECK(ObserveLayoutPublishCandidateNoThrow(
              candidate,LayoutRevisionReadStatus::Present,exact,current)==
          LayoutPublishCandidateObservation::Adopted);
    CHECK(!candidate.armed &&
          SameRevision(current,ReadLayoutRevisionLocked(primary)));

    LayoutRevision retried;
    CHECK(AtomicWriteText(primary,next,&error,true,&retried));
    CommitPublishedLayoutRevisionNoThrow(current,retried);
    CHECK(SameRevision(current,ReadLayoutRevisionLocked(primary)));
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

static std::string LegacyV3LayoutBytes(){
    return std::string("# VDE snapshot v3\n")+
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t"+b64enc("Legacy")+"\n"+
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t"+
        b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\t0\n";
}

static void test_legacy_migration_failure_preserves_source_and_publishes_nothing(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string legacyBytes=LegacyV3LayoutBytes();
    CHECK(WriteRawFile(legacy,legacyBytes));

    LayoutFsOps ops;
    ops.writeFile=[](HANDLE,const void*,DWORD,DWORD& written)->BOOL {
        written=0;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    };
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Failed);
    CHECK(!result.error.empty());
    CHECK(ReadRawFile(legacy)==legacyBytes);
    CHECK(!FileExists(automatic));
    CHECK(!FileExists(automatic+L".tmp"));
    CHECK(!FileExists(automatic+L".bak"));
    CHECK(!FileExists(automatic+L".rollback"));
}

static void test_legacy_migration_parses_before_publishing(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string invalid="# VDE snapshot v3\nW\tfirefox\tinvalid\n";
    CHECK(WriteRawFile(legacy,invalid));

    LayoutFsOps ops;
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Failed);
    CHECK(!result.error.empty());
    CHECK(ReadRawFile(legacy)==invalid);
    CHECK(!FileExists(automatic));
    CHECK(!FileExists(automatic+L".tmp"));
}

static void test_legacy_migration_never_overlays_recoverable_target(){
    const wchar_t* recoverySuffixes[]={L".rollback",L".bak"};
    for(const wchar_t* suffix : recoverySuffixes){
        LayoutTempDir temp;
        std::wstring legacy=temp.file(L"layout.txt");
        std::wstring automatic=temp.file(L"layout-auto.txt");
        std::wstring recovery=automatic+suffix;
        std::string legacyBytes=LegacyV3LayoutBytes();
        std::string recoveryBytes=ValidLayoutBytes(
            wcscmp(suffix,L".rollback")==0 ? "rollback" : "backup");
        CHECK(WriteRawFile(legacy,legacyBytes));
        CHECK(WriteRawFile(recovery,recoveryBytes));

        LayoutFsOps ops;
        LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
            legacy,automatic,1700000000,ops);

        CHECK(result.status==LegacyLayoutMigrationStatus::NotNeeded);
        CHECK(result.target.status==LayoutLoadStatus::Recovered);
        CHECK(result.target.revision.sourcePath==recovery);
        CHECK(ReadRawFile(legacy)==legacyBytes);
        CHECK(ReadRawFile(recovery)==recoveryBytes);
        CHECK(!FileExists(automatic));
        CHECK(!FileExists(legacy+L".migrated"));
    }
}

static void test_legacy_migration_installs_checked_v4_then_retires_source(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string legacyBytes=LegacyV3LayoutBytes();
    CHECK(WriteRawFile(legacy,legacyBytes));

    LayoutFsOps ops;
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Migrated);
    CHECK(result.error.empty());
    CHECK(result.target.status==LayoutLoadStatus::Valid);
    CHECK(result.target.sourceVersion==4);
    CHECK(result.target.revision.sourcePath==automatic);
    CHECK(result.target.revision.exists);
    CHECK(!FileExists(legacy));
    CHECK(ReadRawFile(legacy+L".migrated")==legacyBytes);

    std::string installed=ReadRawFile(automatic);
    CHECK(installed.find("# VDE snapshot v4\n")==0);
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string error;
    int version=0;
    CHECK(ParseLayout(installed,desks,wins,1700000000,&error,&version));
    CHECK(version==4 && desks.size()==1 && wins.size()==1);
    CHECK(!wins[0].recordId.empty());
    CHECK(wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==0);
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

static void test_missing_primary_corrupt_recovery_revision_allows_empty_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        backup=primary+L".bak";
    const std::string corruptRollback="corrupt rollback",
        corruptBackup="corrupt backup", emptyLayout=ValidLayoutBytes("empty-after-corruption");
    CHECK(WriteRawFile(rollback,corruptRollback));
    CHECK(WriteRawFile(backup,corruptBackup));

    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    LayoutRevision canonical=ReadLayoutRevisionLocked(primary);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed);
    CHECK(!canonical.exists && canonical.sourcePath==primary);
    CHECK(SameRevision(loaded.revision,canonical));
    CHECK(WriteIfCanonicalRevisionUnchanged(primary,loaded.revision,emptyLayout));
    CHECK(ReadRawFile(primary)==emptyLayout);
    CHECK(ReadRawFile(rollback)==corruptRollback && ReadRawFile(backup)==corruptBackup);

    LayoutTempDir tempOnly;
    std::wstring tempPrimary=tempOnly.file(L"layout.txt"), committedTemp=tempPrimary+L".tmp";
    CHECK(WriteRawFile(committedTemp,"corrupt committed temporary"));
    LayoutLoadResult tempLoaded=LoadLayoutWithBackupLocked(tempPrimary,1700000000);
    LayoutRevision tempCanonical=ReadLayoutRevisionLocked(tempPrimary);
    CHECK(tempLoaded.status==LayoutLoadStatus::CorruptPreserved && tempLoaded.writesAllowed);
    CHECK(!RawFileExists(committedTemp));
    CHECK(SameRevision(tempLoaded.revision,tempCanonical));
    CHECK(WriteIfCanonicalRevisionUnchanged(tempPrimary,tempLoaded.revision,emptyLayout));
    CHECK(ReadRawFile(tempPrimary)==emptyLayout);
}

static void test_recovered_conflict_preserves_valid_backup_before_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak";
    const std::string initial=ValidLayoutBytes("initial"),
        recovery=ValidLayoutBytes("external-recovery"),
        merged=ValidLayoutBytes("merged-after-recovery");
    std::string error;
    CHECK(AtomicWriteText(primary,initial,&error));
    LayoutLoadResult original=LoadLayoutWithBackupLocked(primary,1700000000);
    bool preserveBackup=PreserveExistingBackupForPublish(false,original.status);
    CHECK(original.status==LayoutLoadStatus::Valid && !preserveBackup);

    CHECK(WriteRawFile(primary,"external corrupt primary"));
    CHECK(WriteRawFile(backup,recovery));
    LayoutLoadResult latest=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(latest.status==LayoutLoadStatus::Recovered && latest.revision.sourcePath==backup);
    preserveBackup=PreserveExistingBackupForPublish(preserveBackup,latest.status);
    CHECK(preserveBackup);
    CHECK(AtomicWriteText(primary,merged,&error,preserveBackup));
    CHECK(ReadRawFile(primary)==merged);
    CHECK(ReadRawFile(backup)==recovery);
    CHECK(PreserveExistingBackupForPublish(true,LayoutLoadStatus::Valid));
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

static void test_window_identity_requires_full_nonzero_process_identity(){
    FastWin window;
    window.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    window.pid=77;
    window.processStart=9001;
    WindowIdentityKey key=IdentityOf(window);
    CHECK(key.hwnd==0x1234 && key.pid==77 && key.processStart==9001);
    CHECK(SameIdentity(key,key));

    WindowIdentityKey changed=key;
    changed.processStart=9002;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.pid=78;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.hwnd=0x1235;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.processStart=0;
    CHECK(!SameIdentity(changed,changed));
    changed=key; changed.hwnd=0;
    CHECK(!SameIdentity(changed,changed));
    changed=key; changed.pid=0;
    CHECK(!SameIdentity(changed,changed));
    CHECK(RuntimeKey(window)==RuntimeKey(key));
    CHECK(RuntimeKey(key)=="4660:77:9001");
}

static void test_snapshot_versions_change_only_for_changed_inputs(){
    SnapshotVersionTracker tracker;
    SnapshotVersions first=tracker.observe("firefox",11,21);
    CHECK(first.identityGeneration!=0 && first.contentGeneration!=0);
    SnapshotVersions unchanged=tracker.observe("firefox",11,21);
    CHECK(unchanged.identityGeneration==first.identityGeneration);
    CHECK(unchanged.contentGeneration==first.contentGeneration);

    SnapshotVersions content=tracker.observe("firefox",11,22);
    CHECK(content.identityGeneration==first.identityGeneration);
    CHECK(content.contentGeneration>first.contentGeneration);

    SnapshotVersions identity=tracker.observe("firefox",12,23);
    CHECK(identity.identityGeneration>content.contentGeneration);
    CHECK(identity.contentGeneration>identity.identityGeneration);

    SnapshotVersions other=tracker.observe("chrome",11,21);
    CHECK(other.identityGeneration>identity.contentGeneration);
    CHECK(other.contentGeneration>other.identityGeneration);
}

static void test_snapshot_signatures_are_delimiter_safe(){
    SnapshotSignatureBuilder left;
    left.addString("a").addString("bc");
    SnapshotSignatureBuilder right;
    right.addString("ab").addString("c");
    CHECK(left.value()!=right.value());

    SnapshotVersionTracker tracker;
    SnapshotVersions before=tracker.observe("firefox",1,left.value());
    SnapshotVersions after=tracker.observe("firefox",1,right.value());
    CHECK(after.identityGeneration==before.identityGeneration);
    CHECK(after.contentGeneration>before.contentGeneration);
}

static void test_snapshot_generation_wrap_restarts_without_zero(){
    SnapshotVersionTracker tracker(UINT64_MAX);
    SnapshotVersions first=tracker.observe("firefox",1,2);
    CHECK(first.identityGeneration==1);
    CHECK(first.contentGeneration==2);
    SnapshotVersions same=tracker.observe("firefox",1,2);
    CHECK(same.identityGeneration==1 && same.contentGeneration==2);

    SnapshotVersionTracker oneSlot(UINT64_MAX-1);
    SnapshotVersions nearWrap=oneSlot.observe("firefox",1,2);
    CHECK(nearWrap.identityGeneration==UINT64_MAX-1);
    CHECK(nearWrap.contentGeneration==UINT64_MAX);
    SnapshotVersions restarted=oneSlot.observe("firefox",1,2);
    CHECK(restarted.identityGeneration==1 && restarted.contentGeneration==2);
}

static FastWin SnapshotWindow(uintptr_t hwnd,DWORD pid,uint64_t started,
        const wchar_t* title,const GUID& desktop){
    FastWin window;
    window.app="firefox";
    window.hwnd=reinterpret_cast<HWND>(hwnd);
    window.pid=pid;
    window.processStart=started;
    window.title=title;
    window.desktop=desktop;
    return window;
}

static void test_save_observed_bound_app_updates_only_exact_bound_identities(){
    const UnixSeconds now=2000000500;
    const GUID desktopA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID movedA=G(L"{231A0000-0000-0000-0000-000000000002}");
    const GUID savedB=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin recordA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009101}","firefox",
        "A","a.example",0,desktopA,now-100);
    LayoutWin recordB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009102}","firefox",
        "B","b.example",2,savedB,now-100);
    recordA.missingSinceUtc=now-50;

    BoundSaveObservation bound;
    bound.window=SnapshotWindow(101,1001,10001,L"changed title",movedA);
    bound.hasBinding=true;
    bound.expectedIdentity=IdentityOf(bound.window);
    bound.recordId=recordA.recordId;
    bound.deskIndex=1;
    bound.causalGeneration=81;
    BoundSaveObservation unbound;
    unbound.window=SnapshotWindow(102,1002,10002,L"B",desktopA);
    unbound.hasBinding=false;
    unbound.deskIndex=0;

    const std::vector<LayoutWin> input={recordA,recordB};
    SaveObservedAppResult saved=ApplyObservedBoundRecords(
        input,"firefox",{bound,unbound},true,now);
    CHECK(saved.valid && saved.needsReconcile);
    CHECK(saved.records.size()==2 && saved.updates.size()==1);
    CHECK(GuidEq(saved.records[0].desktop,movedA) &&
          saved.records[0].deskIndex==1);
    CHECK(saved.records[0].activeTitle==recordA.activeTitle &&
          saved.records[0].activeDomain==recordA.activeDomain);
    CHECK(saved.records[0].lastSeenUtc==now &&
          saved.records[0].missingSinceUtc==0);
    CHECK(SameLayoutWinFields(saved.records[1],recordB));
    CHECK(saved.updates[0].semanticChanged &&
          saved.updates[0].causalGeneration==81 &&
          saved.updates[0].after.recordId==recordA.recordId);

    BoundSaveObservation reused=bound;
    reused.expectedIdentity.processStart++;
    SaveObservedAppResult stale=ApplyObservedBoundRecords(
        input,"firefox",{reused},true,now);
    CHECK(stale.valid && stale.needsReconcile && stale.updates.empty());
    CHECK(SameLayoutWinVectors(stale.records,input));

    SaveObservedAppResult incomplete=ApplyObservedBoundRecords(
        input,"firefox",{bound},false,now);
    CHECK(!incomplete.valid && input.size()==2 &&
          SameLayoutWinFields(input[0],recordA));
}

static void test_explicit_save_with_unbound_sibling_rearms_reconcile(){
    LcState state;
    CHECK(LcObserve(state,true,1,1,10,1,0).action==LcAction::None);
    LcDecision restore=LcObserve(state,true,1,1,10,1,1);
    CHECK(restore.action==LcAction::BeginRestore);
    LcRestoreCompleted(state,restore.generation,LcRestoreOutcome::Success,
                       10,1,2);
    LcDecision save=LcObserve(state,true,1,1,11,1,3);
    CHECK(save.action==LcAction::SaveLayout && state.saveInFlight);
    CHECK(LcExplicitSaveNeedsReconcile(
        state,save.generation,11,1,4));
    CHECK(!state.saveInFlight && state.restorePending &&
          !state.restoreInFlight);
    CHECK(!LcExplicitSaveNeedsReconcile(
        state,save.generation,11,1,5));
}

static void test_fast_snapshot_versions_are_order_independent_and_quality_aware(){
    SnapshotVersionTracker tracker;
    AppFastSnapshot first;
    first.windows.push_back(SnapshotWindow(
        2,20,200,L"",G(L"{231A0000-0000-0000-0000-000000000002}")));
    first.windows.push_back(SnapshotWindow(
        1,10,100,L"Inbox",G(L"{231A0000-0000-0000-0000-000000000001}")));
    FinalizeFastSnapshot("firefox",91,tracker,first);
    CHECK(first.windows.size()==2 && first.windows[0].title==L"Inbox");
    CHECK(first.identityGeneration!=0 && first.generation!=0);
    CHECK(FastSnapshotCanObserve(first) && FastSnapshotCanPersistAll(first));

    AppFastSnapshot reordered;
    reordered.windows.push_back(first.windows[1]);
    reordered.windows.push_back(first.windows[0]);
    FinalizeFastSnapshot("firefox",91,tracker,reordered);
    CHECK(reordered.identityGeneration==first.identityGeneration);
    CHECK(reordered.generation==first.generation);

    reordered.windows[0].title=L"Changed";
    FinalizeFastSnapshot("firefox",91,tracker,reordered);
    CHECK(reordered.identityGeneration==first.identityGeneration);
    CHECK(reordered.generation>first.generation);

    AppFastSnapshot desktopOnly=reordered;
    desktopOnly.windows[0].desktop=
        G(L"{231A0000-0000-0000-0000-000000000003}");
    FinalizeFastSnapshot("firefox",91,tracker,desktopOnly);
    CHECK(desktopOnly.identityGeneration==reordered.identityGeneration);
    CHECK(desktopOnly.generation>reordered.generation);

    AppFastSnapshot configOnly=desktopOnly;
    FinalizeFastSnapshot("firefox",92,tracker,configOnly);
    CHECK(configOnly.identityGeneration>desktopOnly.identityGeneration);
    CHECK(configOnly.generation>configOnly.identityGeneration);

    AppFastSnapshot incomplete=reordered;
    incomplete.enumerationComplete=false;
    FinalizeFastSnapshot("firefox",91,tracker,incomplete);
    CHECK(incomplete.identityGeneration>reordered.identityGeneration);
    CHECK(incomplete.generation>incomplete.identityGeneration);
    CHECK(!FastSnapshotCanObserve(incomplete));

    AppFastSnapshot desktopFailed=reordered;
    desktopFailed.desktopLookupsComplete=false;
    desktopFailed.windows[0].desktop=GUID{};
    FinalizeFastSnapshot("firefox",91,tracker,desktopFailed);
    CHECK(FastSnapshotCanObserve(desktopFailed));
    CHECK(!FastSnapshotCanPersistAll(desktopFailed));
}

static void test_resolve_saved_desktop_uses_guid_only(){
    LayoutWin saved;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    saved.deskIndex=0;
    std::vector<DeskRec> current={
        {7,G(L"{231A0000-0000-0000-0000-000000000001}"),L"one"},
        {9,G(L"{231A0000-0000-0000-0000-000000000002}"),L"two"}
    };
    CHECK(ResolveSavedDesktop(saved,current)==9);
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000099}");
    CHECK(ResolveSavedDesktop(saved,current)==-1);
    saved.desktop=GUID{};
    CHECK(ResolveSavedDesktop(saved,current)==-1);
}

static LayoutRevision RebaseRevision(const wchar_t* path,uint64_t hash){
    LayoutRevision revision;
    revision.sourcePath=path;
    revision.exists=true;
    revision.size=100;
    revision.mtime=200;
    revision.contentHash=hash;
    return revision;
}

static RecordDelta RebaseUpsert(const LayoutWin& base,const LayoutWin& desired,
        const LayoutRevision& baseRevision,UnixSeconds changedUtc,uint64_t generation){
    RecordDelta delta;
    delta.kind=RecordDeltaKind::ValidatedRuntimeUpsert;
    delta.record=desired;
    delta.baseRevision=baseRevision;
    delta.baseRecordPresent=true;
    delta.baseRecord=base;
    delta.changedUtc=changedUtc;
    delta.causalGeneration=generation;
    return delta;
}

static void test_rebase_merges_different_ids_and_preserves_external_records(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",1);
    LayoutRevision latestRevision=RebaseRevision(L"layout",2);
    LayoutWin baseA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009001}","firefox","A","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin desiredA=baseA;
    desiredA.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    desiredA.deskIndex=2;
    desiredA.lastSeenUtc=300;
    LayoutWin externalB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009002}","chrome","B","b.test",3,
        G(L"{231A0000-0000-0000-0000-000000000003}"),250);
    std::map<std::string,RecordDelta> deltas;
    deltas[baseA.recordId]=RebaseUpsert(baseA,desiredA,baseRevision,300,7);
    RebaseResult result=RebaseRecordDeltas(
        {baseA,externalB},latestRevision,deltas,400);
    CHECK(result.deferredConflictRecordIds.empty());
    CHECK(result.records.size()==2);
    CHECK(SameLayoutWinFields(result.records[0],desiredA));
    CHECK(SameLayoutWinFields(result.records[1],externalB));
}

static void test_rebase_same_id_newer_validated_upsert_wins(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",11);
    LayoutRevision latestRevision=RebaseRevision(L"layout",12);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009011}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=200;
    LayoutWin local=base;
    local.activeTitle="local";
    local.lastSeenUtc=301;
    std::map<std::string,RecordDelta> deltas;
    deltas[base.recordId]=RebaseUpsert(base,local,baseRevision,301,9);
    RebaseResult result=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(result.deferredConflictRecordIds.empty());
    CHECK(result.records.size()==1 && SameLayoutWinFields(result.records[0],local));
}

static void test_durable_candidate_delta_is_satisfied_by_external_unrelated_revision(){
    LayoutRevision memoryRevision=RebaseRevision(L"layout",13);
    LayoutRevision capturedRevision=RebaseRevision(L"layout",14);
    const LayoutRevision externalRevision=RebaseRevision(L"layout",15);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009013}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin desired=base;
    desired.activeTitle="durable-C";
    desired.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    desired.deskIndex=2;
    desired.lastSeenUtc=300;
    const RecordDelta delta=RebaseUpsert(
        base,desired,memoryRevision,300,77);
    const std::map<std::string,RecordDelta> retainedDirty={
        {desired.recordId,delta}};
    ValidatedRecordTouch touch;
    touch.recordId=desired.recordId;
    touch.lastSeenUtc=desired.lastSeenUtc;
    touch.causalGeneration=77;

    // C was durable, but the cleanup path retained the A-based journal while
    // adopting C.  An external D then preserves our row and adds another ID.
    CommitPublishedLayoutRevisionNoThrow(memoryRevision,capturedRevision);
    LayoutWin external=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009014}","chrome","external-D","d.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),301);
    const std::vector<std::map<std::string,uint64_t> > causalStates={
        {{touch.recordId,77}},
        {},
        {{touch.recordId,88}}
    };
    for(int newerTouch=0;newerTouch<2;++newerTouch){
        LayoutWin diskDesired=desired;
        diskDesired.lastSeenUtc+=newerTouch;
        for(const auto& causal : causalStates){
            RecordDeltaRebasePreparation prepared;
            CHECK(PrepareRecordDeltasForRebase(
                {diskDesired,external},retainedDirty,causal,prepared));
            CHECK(prepared.deltas.empty());
            CHECK(prepared.deferredRecordIds.empty());

            RebaseResult rebased=RebaseRecordDeltas(
                {diskDesired,external},externalRevision,
                prepared.deltas,400);
            CHECK(rebased.deferredConflictRecordIds.empty());
            CHECK(rebased.records.size()==2);
            CHECK(SameLayoutWinFields(rebased.records[0],diskDesired));
            CHECK(SameLayoutWinFields(rebased.records[1],external));

            TouchRebaseResult touched=ReapplyValidatedTouches(
                rebased.records,{{touch.recordId,touch}},causal);
            CHECK(touched.deferredRecordIds.empty());
            CHECK(touched.records.size()==2);
            CHECK(SameLayoutWinFields(touched.records[0],diskDesired));
            CHECK(SameLayoutWinFields(touched.records[1],external));
        }
    }

    RecordDeltaRebasePreparation blocked;
    CHECK(PrepareRecordDeltasForRebase(
        {base,external},retainedDirty,{},blocked));
    CHECK(blocked.deltas.empty());
    CHECK(blocked.deferredRecordIds.count(desired.recordId)==1);

    // Publish D and its residual journal as one transaction.  A forced write
    // failure after adoption must not leave the old A base behind.  A later
    // MissingMark then rebases cleanly over unrelated external F.
    const std::map<std::string,ValidatedRecordTouch> retainedTouches={
        {touch.recordId,touch}};
    RecordDeltaRebasePreparation prepared;
    CHECK(PrepareRecordDeltasForRebase(
        {desired,external},retainedDirty,{},prepared));
    CHECK(prepared.satisfiedRecordIds.count(desired.recordId)==1);
    RebaseResult rebased=RebaseRecordDeltas(
        {desired,external},externalRevision,prepared.deltas,400);
    TouchRebaseResult touched=ReapplyValidatedTouches(
        rebased.records,retainedTouches,{});
    CHECK(touched.satisfiedRecordIds.count(desired.recordId)==1);
    RebasedResidualJournal residual;
    CHECK(BuildRebasedResidualJournal(
        retainedDirty,retainedTouches,prepared,rebased,touched,
        {desired,external},externalRevision,residual));
    CHECK(residual.deltas.empty() && residual.touches.empty());

    RebasedAutoLayoutPublication publication;
    CHECK(BuildRebasedAutoLayoutPublication(
        touched.records,{},externalRevision,{},residual.deltas,
        residual.touches,publication));
    std::vector<LayoutWin> inMemory={base};
    std::map<std::string,RecordDelta> journal=retainedDirty;
    std::map<std::string,ValidatedRecordTouch> touchJournal=retainedTouches;
    std::map<std::string,DeferredRecordConflict> conflicts;
    inMemory.swap(publication.records);
    journal.swap(publication.deltas);
    touchJournal.swap(publication.touches);
    SwapLayoutRevisionNoThrow(memoryRevision,publication.revision);
    conflicts.swap(publication.conflicts);
    CHECK(SameRevision(memoryRevision,externalRevision));
    CHECK(journal.empty() && touchJournal.empty() && conflicts.empty());

    LayoutWin missing=inMemory[0];
    missing.missingSinceUtc=500;
    RecordDelta missingDelta;
    missingDelta.kind=RecordDeltaKind::MissingMark;
    missingDelta.record=missing;
    missingDelta.baseRevision=memoryRevision;
    missingDelta.baseRecordPresent=true;
    missingDelta.baseRecord=inMemory[0];
    missingDelta.changedUtc=500;
    missingDelta.causalGeneration=88;
    std::vector<LayoutWin> stagedRecords;
    std::map<std::string,RecordDelta> stagedDeltas;
    std::map<std::string,DeferredRecordConflict> stagedConflicts;
    CHECK(StageRecordDeltaMutation(
        inMemory,journal,conflicts,missingDelta,true,
        stagedRecords,stagedDeltas,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(SameRevision(
        stagedDeltas.at(missing.recordId).baseRevision,externalRevision));

    LayoutWin externalF=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009015}","edge","external-F","f.test",5,
        G(L"{231A0000-0000-0000-0000-000000000005}"),501);
    const LayoutRevision revisionF=RebaseRevision(L"layout",16);
    RebaseResult overF=RebaseRecordDeltas(
        {desired,external,externalF},revisionF,stagedDeltas,600);
    CHECK(overF.deferredConflictRecordIds.empty());
    CHECK(overF.records.size()==3);
    CHECK(overF.records[0].missingSinceUtc==500);
    CHECK(SameLayoutWinFields(overF.records[1],external));
    CHECK(SameLayoutWinFields(overF.records[2],externalF));
}

static void test_rebase_tied_or_older_upsert_and_stale_tombstone_defer(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",21);
    LayoutRevision latestRevision=RebaseRevision(L"layout",22);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009021}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=250;
    LayoutWin local=base;
    local.activeTitle="local";
    local.lastSeenUtc=250;
    std::map<std::string,RecordDelta> deltas;
    deltas[base.recordId]=RebaseUpsert(base,local,baseRevision,250,10);
    RebaseResult tied=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(tied.records.size()==1 && SameLayoutWinFields(tied.records[0],disk));
    CHECK(tied.deferredConflictRecordIds.count(base.recordId)==1);

    deltas[base.recordId].changedUtc=249;
    RebaseResult older=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(older.records.size()==1 && SameLayoutWinFields(older.records[0],disk));
    CHECK(older.deferredConflictRecordIds.count(base.recordId)==1);

    RecordDelta missing=deltas[base.recordId];
    missing.kind=RecordDeltaKind::MissingMark;
    missing.record=local;
    missing.record.missingSinceUtc=249;
    deltas[base.recordId]=missing;
    RebaseResult missingResult=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(missingResult.records.size()==1 && SameLayoutWinFields(missingResult.records[0],disk));
    CHECK(missingResult.deferredConflictRecordIds.count(base.recordId)==1);

    RecordDelta tombstone=missing;
    tombstone.kind=RecordDeltaKind::ExpireDelete;
    tombstone.erase=true;
    deltas[base.recordId]=tombstone;
    RebaseResult staleDelete=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(staleDelete.records.size()==1 && SameLayoutWinFields(staleDelete.records[0],disk));
    CHECK(staleDelete.deferredConflictRecordIds.count(base.recordId)==1);
}

static void test_rebase_expiry_delete_requires_latest_independently_expired(){
    const UnixSeconds now=WINDOW_RETENTION_SECONDS+1000;
    LayoutRevision baseRevision=RebaseRevision(L"layout",31);
    LayoutRevision latestRevision=RebaseRevision(L"layout",32);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009031}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="changed";
    disk.missingSinceUtc=1;
    RecordDelta tombstone;
    tombstone.kind=RecordDeltaKind::ExpireDelete;
    tombstone.erase=true;
    tombstone.record=base;
    tombstone.baseRevision=baseRevision;
    tombstone.baseRecordPresent=true;
    tombstone.baseRecord=base;
    tombstone.changedUtc=now;
    tombstone.causalGeneration=1;
    std::map<std::string,RecordDelta> deltas={{base.recordId,tombstone}};
    RebaseResult expired=RebaseRecordDeltas({disk},latestRevision,deltas,now);
    CHECK(expired.records.empty());
    CHECK(expired.deferredConflictRecordIds.empty());

    disk.missingSinceUtc=now-1;
    RebaseResult retained=RebaseRecordDeltas({disk},latestRevision,deltas,now);
    CHECK(retained.records.size()==1 && SameLayoutWinFields(retained.records[0],disk));
    CHECK(retained.deferredConflictRecordIds.count(base.recordId)==1);
}

static void test_record_delta_chaining_preserves_first_disk_base(){
    LayoutRevision firstRevision=RebaseRevision(L"layout",41);
    LayoutRevision accidentalLocalRevision=RebaseRevision(L"layout",42);
    LayoutWin diskBase=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009041}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin firstDesired=diskBase;
    firstDesired.activeTitle="first";
    firstDesired.lastSeenUtc=200;
    RecordDelta first=RebaseUpsert(
        diskBase,firstDesired,firstRevision,200,3);
    LayoutWin secondDesired=firstDesired;
    secondDesired.activeTitle="second";
    secondDesired.lastSeenUtc=300;
    RecordDelta later=RebaseUpsert(
        firstDesired,secondDesired,accidentalLocalRevision,300,4);
    RecordDelta chained=ChainRecordDelta(first,later);
    CHECK(SameRevision(chained.baseRevision,firstRevision));
    CHECK(chained.baseRecordPresent);
    CHECK(SameLayoutWinFields(chained.baseRecord,diskBase));
    CHECK(SameLayoutWinFields(chained.record,secondDesired));
    CHECK(chained.changedUtc==300 && chained.causalGeneration==4);
}

static void test_deferred_conflict_survives_repeated_publish_until_newer_causal_upsert(){
    const LayoutRevision baseRevision=RebaseRevision(L"layout",51);
    const LayoutRevision adoptedRevision=RebaseRevision(L"layout",52);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009051}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=250;
    LayoutWin stale=base;
    stale.activeTitle="stale";
    stale.lastSeenUtc=250;
    std::map<std::string,RecordDelta> dirty;
    dirty[base.recordId]=RebaseUpsert(base,stale,baseRevision,250,11);

    const RebaseResult first=RebaseRecordDeltas(
        {disk},adoptedRevision,dirty,300);
    CHECK(first.records.size()==1 && SameLayoutWinFields(first.records[0],disk));
    CHECK(first.deferredConflictRecordIds.count(base.recordId)==1);

    std::map<std::string,DeferredRecordConflict> conflicts;
    CHECK(BuildDeferredRecordConflicts(
        first.deferredConflictRecordIds,first.records,adoptedRevision,conflicts));
    int serializations=0,writes=0;
    auto persist=[&](){
        if(DeferredRecordConflictsBlockPublish(conflicts,adoptedRevision))
            return false;
        ++serializations;
        ++writes;
        return true;
    };
    CHECK(!persist());
    CHECK(!persist());
    CHECK(serializations==0 && writes==0);

    const std::vector<LayoutWin> adoptedRecords=first.records;
    const std::map<std::string,RecordDelta> originalDirty=dirty;
    const std::map<std::string,DeferredRecordConflict> originalConflicts=conflicts;
    std::vector<LayoutWin> stagedRecords;
    std::map<std::string,RecordDelta> stagedDirty;
    std::map<std::string,DeferredRecordConflict> stagedConflicts;

    RecordDelta tied=RebaseUpsert(
        disk,stale,adoptedRevision,250,12);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,tied,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    CHECK(stagedRecords.empty() && stagedDirty.empty() && stagedConflicts.empty());
    CHECK(SameLayoutWinFields(adoptedRecords[0],disk));
    CHECK(SameLayoutWinFields(dirty.at(base.recordId).record,
                              originalDirty.at(base.recordId).record));
    CHECK(SameRevision(conflicts.at(base.recordId).adoptedRevision,
                       originalConflicts.at(base.recordId).adoptedRevision));
    CHECK(DeferredRecordConflictsBlockPublish(conflicts,adoptedRevision));

    RecordDelta older=tied;
    older.changedUtc=249;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,older,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    RecordDelta destructive=tied;
    destructive.kind=RecordDeltaKind::ExpireDelete;
    destructive.erase=true;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,destructive,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);

    LayoutWin unrelated=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009052}","chrome","other","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),251);
    RecordDelta unrelatedDelta;
    unrelatedDelta.kind=RecordDeltaKind::ExplicitUpsert;
    unrelatedDelta.record=unrelated;
    unrelatedDelta.baseRevision=adoptedRevision;
    unrelatedDelta.changedUtc=251;
    unrelatedDelta.causalGeneration=14;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,unrelatedDelta,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(stagedRecords.size()==2 && stagedConflicts.size()==1);
    CHECK(DeferredRecordConflictsBlockPublish(
        stagedConflicts,adoptedRevision));

    LayoutWin newer=disk;
    newer.activeTitle="newer";
    newer.lastSeenUtc=251;
    RecordDelta accepted=RebaseUpsert(
        disk,newer,adoptedRevision,251,13);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,accepted,false,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,accepted,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(stagedRecords.size()==1 && SameLayoutWinFields(stagedRecords[0],newer));
    CHECK(stagedConflicts.empty());
    CHECK(stagedDirty.size()==1);
    const RecordDelta& replacement=stagedDirty.at(base.recordId);
    CHECK(SameRevision(replacement.baseRevision,adoptedRevision));
    CHECK(replacement.baseRecordPresent &&
          SameLayoutWinFields(replacement.baseRecord,disk));
    CHECK(SameLayoutWinFields(replacement.record,newer));
    CHECK(!DeferredRecordConflictsBlockPublish(
        stagedConflicts,adoptedRevision));
}

static void test_rebased_publication_captures_adopted_disk_before_any_swap(){
    const LayoutRevision adoptedRevision=RebaseRevision(L"layout-B",62);
    LayoutWin adopted=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009061}","firefox","disk-B","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),620);
    DeskRec desktop;
    desktop.index=2;
    desktop.guid=adopted.desktop;
    desktop.name=L"B";
    RebasedAutoLayoutPublication publication;
    CHECK(BuildRebasedAutoLayoutPublication(
        {adopted},{desktop},adoptedRevision,{adopted.recordId},publication));
    CHECK(publication.records.size()==1 &&
          SameLayoutWinFields(publication.records[0],adopted));
    CHECK(publication.desktops.size()==1 &&
          GuidEq(publication.desktops[0].guid,desktop.guid));
    CHECK(SameRevision(publication.revision,adoptedRevision));
    CHECK(publication.conflicts.size()==1);
    const DeferredRecordConflict& conflict=
        publication.conflicts.find(adopted.recordId)->second;
    CHECK(conflict.adoptedRecordPresent &&
          SameLayoutWinFields(conflict.adoptedRecord,adopted));
    CHECK(SameRevision(conflict.adoptedRevision,adoptedRevision));

    RebasedAutoLayoutPublication sentinel=publication;
    sentinel.records[0].activeTitle="sentinel";
    CHECK(!BuildRebasedAutoLayoutPublication(
        {adopted},{desktop},adoptedRevision,{adopted.recordId},sentinel,
        [](){ throw std::bad_alloc(); }));
    CHECK(sentinel.records[0].activeTitle=="sentinel");
    CHECK(SameRevision(sentinel.revision,adoptedRevision));
}

static void test_durable_publish_commits_revision_without_copy_or_rewrite(){
    LayoutRevision current=RebaseRevision(L"layout-A",71);
    LayoutRevision published=RebaseRevision(L"layout-B",72);
    const LayoutRevision expected=published;
    bool dirty=true;
    int writes=0;
    auto persist=[&]{
        if(!dirty) return true;
        ++writes;
        CommitPublishedLayoutRevisionNoThrow(current,published);
        dirty=false;
        return true;
    };
    CHECK(noexcept(CommitPublishedLayoutRevisionNoThrow(current,published)));
    CHECK(persist());
    CHECK(SameRevision(current,expected));
    CHECK(!dirty && writes==1);
    CHECK(persist());
    CHECK(writes==1);
}

static void test_final_checkpoint_mutation_is_transactional_across_fault_matrix(){
    const LayoutRevision revision=RebaseRevision(L"layout",61);
    const UnixSeconds now=2000000600;
    LayoutWin kept=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009061}","firefox","old","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    LayoutWin erased=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009062}","chrome","gone","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-100);
    LayoutWin changed=kept;
    changed.activeTitle="new";
    changed.lastSeenUtc=now;
    LayoutWin added=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009063}","firefox","added","c.test",3,
        G(L"{231A0000-0000-0000-0000-000000000003}"),now);
    ValidatedRecordTouch touch;
    touch.recordId=changed.recordId;
    touch.lastSeenUtc=now;
    touch.causalGeneration=61;

    FinalCheckpointMutationState sentinel;
    sentinel.records.push_back(erased);
    RecordDelta sentinelDelta=RebaseUpsert(
        erased,erased,revision,now-1,60);
    sentinel.deltas[erased.recordId]=sentinelDelta;
    sentinel.touches[erased.recordId]=touch;
    sentinel.provisionalRecordByRuntime["sentinel-runtime"]=erased.recordId;
    const std::map<std::string,std::string> finalProvisional={
        {"new-runtime",added.recordId}};
    const std::vector<FinalCheckpointFaultPoint> faults={
        FinalCheckpointFaultPoint::InitialCopies,
        FinalCheckpointFaultPoint::RecordIndexes,
        FinalCheckpointFaultPoint::EraseDelta,
        FinalCheckpointFaultPoint::UpsertDelta,
        FinalCheckpointFaultPoint::ValidatedTouches,
        FinalCheckpointFaultPoint::FinalRecords,
        FinalCheckpointFaultPoint::Publish
    };
    for(FinalCheckpointFaultPoint failedAt : faults){
        FinalCheckpointMutationState output=sentinel;
        CHECK(!BuildFinalCheckpointMutation(
            {kept,erased},{changed,added},{}, {}, {}, {touch},
            finalProvisional,revision,now,61,output,
            [&](FinalCheckpointFaultPoint point){
                if(point==failedAt) throw std::bad_alloc();
            }));
        CHECK(output.records.size()==1 &&
              SameLayoutWinFields(output.records[0],erased));
        CHECK(output.deltas.size()==1 &&
              output.deltas.count(erased.recordId)==1);
        CHECK(output.touches.size()==1 &&
              output.touches.count(erased.recordId)==1);
        CHECK(output.provisionalRecordByRuntime.size()==1 &&
              output.provisionalRecordByRuntime.count("sentinel-runtime")==1);
    }

    FinalCheckpointMutationState staged;
    CHECK(BuildFinalCheckpointMutation(
        {kept,erased},{changed,added},{}, {}, {}, {touch},finalProvisional,
        revision,now,61,staged));
    CHECK(staged.records.size()==2 &&
          SameLayoutWinFields(staged.records[0],changed) &&
          SameLayoutWinFields(staged.records[1],added));
    CHECK(staged.deltas.size()==3);
    CHECK(staged.deltas.at(erased.recordId).erase);
    CHECK(SameLayoutWinFields(staged.deltas.at(changed.recordId).record,changed));
    CHECK(SameLayoutWinFields(staged.deltas.at(added.recordId).record,added));
    CHECK(staged.touches.size()==1 &&
          staged.touches.at(changed.recordId).causalGeneration==61);
    CHECK(staged.provisionalRecordByRuntime==finalProvisional);
}

static void test_expire_delete_discards_validated_touch_before_external_rebase(){
    const LayoutRevision revisionA=RebaseRevision(L"layout",64);
    const LayoutRevision revisionD=RebaseRevision(L"layout",65);
    const UnixSeconds now=2000000650;
    LayoutWin erased=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009064}","firefox","gone","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    ValidatedRecordTouch staleTouch;
    staleTouch.recordId=erased.recordId;
    staleTouch.lastSeenUtc=now-50;
    staleTouch.causalGeneration=64;
    const std::map<std::string,ValidatedRecordTouch> originalTouches={
        {erased.recordId,staleTouch}};

    FinalCheckpointMutationState checkpoint;
    CHECK(BuildFinalCheckpointMutation(
        {erased},{},{},originalTouches,{}, {}, {},revisionA,now,64,checkpoint));
    CHECK(checkpoint.records.empty());
    CHECK(checkpoint.deltas.size()==1 &&
          checkpoint.deltas.at(erased.recordId).erase);
    CHECK(checkpoint.touches.count(erased.recordId)==0);

    LayoutWin external=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009065}","chrome","external-D","d.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now);
    RecordDeltaRebasePreparation prepared;
    CHECK(PrepareRecordDeltasForRebase(
        {erased,external},checkpoint.deltas,{},prepared));
    RebaseResult rebased=RebaseRecordDeltas(
        {erased,external},revisionD,prepared.deltas,now+1);
    CHECK(rebased.deferredConflictRecordIds.empty());
    CHECK(rebased.appliedDeleteRecordIds.count(erased.recordId)==1);
    CHECK(rebased.records.size()==1 &&
          SameLayoutWinFields(rebased.records[0],external));

    std::map<std::string,ValidatedRecordTouch> touchesForRebase;
    CHECK(PrepareValidatedTouchesForRebase(
        originalTouches,rebased.appliedDeleteRecordIds,touchesForRebase));
    CHECK(touchesForRebase.empty());
    TouchRebaseResult touched=ReapplyValidatedTouches(
        rebased.records,touchesForRebase,{});
    CHECK(touched.deferredRecordIds.empty());

    RebasedResidualJournal residual;
    CHECK(BuildRebasedResidualJournal(
        checkpoint.deltas,originalTouches,prepared,rebased,touched,
        {erased,external},revisionD,residual));
    CHECK(residual.touches.count(erased.recordId)==0);
}

static void test_final_observation_provisional_map_stages_before_global_publish(){
    const std::map<std::string,std::string> current={
        {"old-runtime","{00000000-0000-0000-0000-000000009071}"}};
    std::map<std::string,std::string> outputMap={
        {"sentinel-runtime","{00000000-0000-0000-0000-000000009072}"}};
    FinalAppObservation sentinel;
    sentinel.app="sentinel";
    std::vector<FinalAppObservation> output={sentinel};
    CHECK(!StageFinalObservationsAndProvisionals(
        current,output,outputMap,
        [](std::map<std::string,std::string>& staged,
           std::vector<FinalAppObservation>& observations)->bool {
            staged["new-runtime"]=
                "{00000000-0000-0000-0000-000000009073}";
            FinalAppObservation app;
            app.app="firefox";
            observations.push_back(app);
            throw std::bad_alloc();
        }));
    CHECK(current.size()==1 && current.count("old-runtime")==1);
    CHECK(output.size()==1 && output[0].app=="sentinel");
    CHECK(outputMap.size()==1 && outputMap.count("sentinel-runtime")==1);

    CHECK(StageFinalObservationsAndProvisionals(
        current,output,outputMap,
        [](std::map<std::string,std::string>& staged,
           std::vector<FinalAppObservation>& observations)->bool {
            staged["new-runtime"]=
                "{00000000-0000-0000-0000-000000009073}";
            FinalAppObservation app;
            app.app="firefox";
            observations.push_back(app);
            return true;
        }));
    CHECK(current.size()==1 && current.count("old-runtime")==1);
    CHECK(output.size()==1 && output[0].app=="firefox");
    CHECK(outputMap.size()==2 && outputMap.count("old-runtime")==1 &&
          outputMap.count("new-runtime")==1);
}

struct ReconcileResultSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<ReconcileResult> > results;
    std::atomic<bool> wrongMessage{false};

    bool post(HWND,UINT message,WPARAM,LPARAM value){
        if(message!=WM_RECONCILE_RESULT) wrongMessage=true;
        std::unique_ptr<ReconcileResult> owned(
            reinterpret_cast<ReconcileResult*>(value));
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(owned));
        }
        changed.notify_all();
        return true;
    }

    std::unique_ptr<ReconcileResult> waitFor(
            uint64_t operationId,
            std::chrono::milliseconds timeout=std::chrono::seconds(5)){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock,timeout,[&]{
            for(const auto& result : results)
                if(result->operationId==operationId) return true;
            return false;
        });
        for(auto it=results.begin();it!=results.end();++it){
            if((*it)->operationId!=operationId) continue;
            std::unique_ptr<ReconcileResult> found=std::move(*it);
            results.erase(it);
            return found;
        }
        return std::unique_ptr<ReconcileResult>();
    }
};

struct BlockingReconcilePlanner {
    std::mutex mutex;
    std::condition_variable changed;
    bool firstEntered=false;
    bool released=false;
    int calls=0;
    std::thread::id workerThread;

    ReconcilePlan plan(const ReconcileRequest& request){
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        workerThread=std::this_thread::get_id();
        if(calls==1){
            firstEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return released; });
        }
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        return output;
    }

    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{
            return firstEntered;
        });
    }

    void release(){
        std::lock_guard<std::mutex> lock(mutex);
        released=true;
        changed.notify_all();
    }
};

static ReconcileRequest WorkerReconcileRequest(uint64_t operationId){
    ReconcileRequest request;
    request.operationId=operationId;
    request.app="firefox";
    request.identityGeneration=11;
    request.contentGeneration=21+operationId;
    request.sessionRequestId=31+operationId;
    request.sessionDataGeneration=41+operationId;
    request.nowUtc=1700000000;
    request.freshness=ReconcileFreshness::Fresh;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009101}","firefox","A","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),1699999900);
    request.saved.push_back(saved);
    request.live.push_back(saved);
    return request;
}

static ReconcileRequest WorkerPreparedRequest(uint64_t operationId,
                                               const wchar_t* firstTitle){
    ReconcileRequest request=WorkerReconcileRequest(operationId);
    request.live.clear();
    request.buildLiveFromInputs=true;
    request.workMode=ReconcileWorkMode::Plan;
    DeskRec firstDesktop;
    firstDesktop.index=7;
    firstDesktop.guid=G(L"{231A0000-0000-0000-0000-000000009201}");
    firstDesktop.name=L"Seven";
    DeskRec secondDesktop;
    secondDesktop.index=9;
    secondDesktop.guid=G(L"{231A0000-0000-0000-0000-000000009202}");
    secondDesktop.name=L"Nine";
    request.desktops={firstDesktop,secondDesktop};
    request.titleSuffixes={L" - Browser"};
    request.fastWindows={
        SnapshotWindow(0x9201,9201,19201,firstTitle,firstDesktop.guid),
        SnapshotWindow(0x9202,9202,19202,L"Other - Browser",secondDesktop.guid)
    };
    std::shared_ptr<std::vector<WinFp> > session(new std::vector<WinFp>());
    WinFp other;
    other.activeTitle="Other";
    other.activeDomain="other.test";
    other.tabCount=2;
    other.tabsBlob="other-tabs";
    WinFp first;
    first.activeTitle=W2U8(std::wstring(firstTitle).substr(
        0,std::wstring(firstTitle).size()-std::wstring(L" - Browser").size()));
    first.activeDomain="first.test";
    first.tabCount=3;
    first.tabsBlob="first-tabs";
    session->push_back(other);
    session->push_back(first);
    request.sessionWindows=session;
    return request;
}

static void test_reconcile_live_preparation_is_ordered_and_search_ready(){
    ReconcileRequest request=WorkerPreparedRequest(140,L"Inbox - Browser");
    PreparedReconcileLive prepared;
    CHECK(BuildReconcileLivePreparation(request,prepared));
    CHECK(prepared.live.size()==2);
    CHECK(prepared.sessionIndexByFast.size()==2 &&
        prepared.sessionIndexByFast[0]==1 && prepared.sessionIndexByFast[1]==0);
    CHECK(request.sessionWindows->at(
        static_cast<size_t>(prepared.sessionIndexByFast[0])).tabsBlob=="first-tabs");
    CHECK(prepared.live[0].activeTitle=="Inbox" &&
        prepared.live[0].activeDomain=="first.test" &&
        prepared.live[0].tabCount==3 && prepared.live[0].deskIndex==7);
    CHECK(prepared.live[1].activeTitle=="Other" &&
        prepared.live[1].activeDomain=="other.test" &&
        prepared.live[1].deskIndex==9);
    CHECK(reinterpret_cast<uintptr_t>(request.fastWindows[0].hwnd)==0x9201);
    CHECK(request.sessionWindows && request.sessionWindows->at(1).tabsBlob=="first-tabs");
}

static void test_picker_accepts_fresh_cache_only_for_exact_session_rows(){
    ReconcileResult result;
    result.status=ReconcileResultStatus::Completed;
    result.app="firefox";
    result.identityGeneration=141;
    result.freshness=ReconcileFreshness::Fresh;
    result.workMode=ReconcileWorkMode::PrepareLiveOnly;
    result.buildLiveFromInputs=true;
    result.fastWindows.push_back(SnapshotWindow(
        14101,14102,14103,L"Matched - Browser",
        G(L"{231A0000-0000-0000-0000-000000000001}")));
    result.fastWindows.push_back(SnapshotWindow(
        14111,14112,14113,L"",
        G(L"{231A0000-0000-0000-0000-000000000001}")));
    result.live.resize(2);
    result.live[0].app="firefox";
    result.live[0].activeTitle="Matched";
    result.live[0].activeDomain="matched.test";
    result.live[0].tabCount=1;
    result.live[0].counts={{"matched.test",1}};
    result.live[1].app="firefox";
    std::shared_ptr<std::vector<WinFp> > sessions(
        new std::vector<WinFp>(1));
    sessions->at(0).activeTitle="Matched";
    sessions->at(0).activeDomain="matched.test";
    sessions->at(0).tabCount=1;
    sessions->at(0).counts={{"matched.test",1}};
    result.sessionWindows=sessions;
    result.sessionIndexByFast={0,-1};

    const auto usable=[&](size_t index){
        const WinFp* session=ReconcileSessionForFast(result,index);
        const LayoutWin& live=result.live[index];
        const bool associationMatches=session &&
            live.activeDomain==session->activeDomain &&
            live.tabCount==session->tabCount &&
            live.counts==session->counts &&
            (session->activeTitle.empty() ||
             live.activeTitle==session->activeTitle);
        return PickerAcceptedFreshRowUsable(
            result.freshness==ReconcileFreshness::Fresh,
            associationMatches,IdentityOf(result.fastWindows[index]),
            live.app==result.app,live.activeTitle,live.counts);
    };

    CHECK(usable(0));
    CHECK(!usable(1));

    // An unmatched nonempty HWND remains a title-only provisional; app-level
    // Freshness must not promote it to an exact session fingerprint either.
    result.live[1].activeTitle="Captured title";
    CHECK(!usable(1));

    // Association alone is not enough when the resulting row is blank, but
    // a captured fallback title makes the exact associated row matchable.
    result.sessionIndexByFast[1]=0;
    sessions->at(0)=WinFp{};
    CHECK(usable(1));
    result.live[1].activeTitle.clear();
    CHECK(!usable(1));
    sessions->at(0).counts={{"fallback.test",1}};
    result.live[1].counts={{"fallback.test",1}};
    CHECK(usable(1));
}

static void test_unusable_fresh_rows_cannot_corrupt_or_create_records(){
    CHECK(PickerRowUsesFreshFingerprint(true,true));
    CHECK(!PickerRowUsesFreshFingerprint(true,false));
    CHECK(!PickerRowUsesFreshFingerprint(false,true));
    CHECK(PickerUnboundRowEligibleForReconcilePlan(true,true));
    CHECK(!PickerUnboundRowEligibleForReconcilePlan(true,false));
    CHECK(PickerUnboundRowEligibleForReconcilePlan(false,false));
    CHECK(PickerUnboundPlanUsesFreshness(true,false));
    CHECK(!PickerUnboundPlanUsesFreshness(true,true));

    const GUID desktop=
        G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin existing=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000001421}","firefox",
        "Good","good.test",2,desktop,1700000000);
    FastWin fast=SnapshotWindow(
        14201,14202,14203,L"",desktop);
    fast.app="firefox";
    LayoutWin blank;
    blank.app="firefox";
    blank.desktop=desktop;
    LayoutWin committed;
    std::map<std::string,std::string> provisionalByRuntime;
    CHECK(CommitBoundRecordRefresh(
        existing,fast,blank,
        PickerRowUsesFreshFingerprint(true,false)
            ? ReconcileFreshness::Fresh
            : ReconcileFreshness::CachedStale,
        1700000001,
        RuntimeKey(fast),provisionalByRuntime,
        [&](const LayoutWin& desired){ committed=desired; return true; }));
    CHECK(committed.activeTitle==existing.activeTitle &&
          committed.activeDomain==existing.activeDomain &&
          committed.tabCount==existing.tabCount &&
          committed.counts==existing.counts);

    std::vector<LayoutWin> planLive;
    if(PickerUnboundRowEligibleForReconcilePlan(true,false))
        planLive.push_back(blank);
    const ReconcilePlan plan=PlanAppReconcile(
        {},planLive,"firefox",1700000001,{},ReconcileFreshness::Fresh);
    CHECK(plan.newRecords.empty());
}

static void test_picker_title_only_provisionals_share_reconcile_normalization(){
    const std::vector<std::wstring> suffixes={L" - Browser"};
    LayoutWin first,second;
    first.app=second.app="firefox";
    first.activeTitle=W2U8(StripReconcileTitleSuffix(
        L"First - Browser",suffixes));
    second.activeTitle=W2U8(StripReconcileTitleSuffix(
        L"Second - Browser",suffixes));
    CHECK(PickerTitleOnlyProvisionalFieldsUsable(
        first.app,first.activeTitle));
    CHECK(PickerTitleOnlyProvisionalFieldsUsable(
        second.app,second.activeTitle));
    first.provisional=second.provisional=true;
    CHECK(first.app=="firefox" && first.activeTitle=="First" &&
          first.provisional);
    CHECK(second.app=="firefox" && second.activeTitle=="Second" &&
          second.provisional);

    first.recordId="{00000000-0000-0000-0000-000000001411}";
    second.recordId="{00000000-0000-0000-0000-000000001412}";
    first.desktop=second.desktop=
        G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveFirst=first,liveSecond=second;
    liveFirst.provisional=liveSecond.provisional=false;
    liveFirst.recordId.clear();
    liveSecond.recordId.clear();
    const ReconcilePlan plan=PlanAppReconcile(
        {first,second},{liveFirst,liveSecond},"firefox",1700000000,{},
        ReconcileFreshness::Fresh);
    CHECK(!plan.deferred && plan.matches.size()==2 &&
          plan.newRecords.empty());
}

static void test_cli_profile_batch_aborts_transactionally_on_first_prep_failure(){
    ReconcileRequest firefox=WorkerPreparedRequest(146,L"Firefox - Browser");
    firefox.app="firefox";
    ReconcileRequest chrome=WorkerPreparedRequest(147,L"Chrome - Browser");
    chrome.app="chrome";
    std::vector<ReconcileRequest> requests={firefox,chrome};

    PreparedCliProfileBatch output;
    LayoutWin retainedLive;
    retainedLive.app="retained";
    retainedLive.activeTitle="manual-before";
    output.live.push_back(retainedLive);
    FastWin retainedFast;
    retainedFast.app="retained";
    retainedFast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0xCAFE));
    output.fastWindows.push_back(retainedFast);

    int preparationCalls=0;
    std::string manualBytes="manual-before";
    size_t moveCount=0;
    bool prepared=BuildCliProfileBatch(
        requests,output,
        [&](const ReconcileRequest& request,PreparedReconcileLive& built){
            ++preparationCalls;
            if(request.app=="firefox") return false;
            return BuildReconcileLivePreparation(request,built);
        });
    if(prepared){
        manualBytes="manual-after";
        moveCount=output.fastWindows.size();
    }

    CHECK(!prepared);
    CHECK(preparationCalls==1);
    CHECK(output.live.size()==1 && output.live[0].app=="retained" &&
          output.live[0].activeTitle=="manual-before");
    CHECK(output.fastWindows.size()==1 &&
          reinterpret_cast<uintptr_t>(output.fastWindows[0].hwnd)==0xCAFE);
    CHECK(manualBytes=="manual-before");
    CHECK(moveCount==0);
}

static void test_cli_loads_settings_before_selecting_active_profiles(){
    bool firefox=true;
    bool chrome=false;
    bool edge=true;
    bool settingsLoaded=false;
    int dispatches=0;

    const int result=RunCliWithLoadedSettings(
        [&]{
            firefox=false;
            chrome=true;
            edge=false;
            settingsLoaded=true;
        },
        [&]{
            ++dispatches;
            CHECK(settingsLoaded);
            const std::vector<AppProfile> profiles=
                BuiltinProfiles(firefox,chrome,edge);
            CHECK(profiles.size()==1 && profiles[0].id=="chrome");
            return 37;
        });

    CHECK(result==37);
    CHECK(dispatches==1);
}

static void test_cli_save_revalidates_snapshot_and_desktops_before_publish(){
    FastWin fast;
    fast.app="firefox";
    fast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9541));
    fast.pid=9541;
    fast.processStart=954100;
    fast.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    fast.title=L"Inbox";
    AppFastSnapshot snapshot;
    snapshot.windows.push_back(fast);
    snapshot.identityGeneration=41;
    snapshot.generation=42;
    std::map<std::string,AppFastSnapshot> captured={{"firefox",snapshot}};
    std::map<std::string,AppFastSnapshot> current=captured;
    DeskRec desktop;
    desktop.index=0;
    desktop.guid=fast.desktop;
    std::vector<DeskRec> desktops={desktop};
    CHECK(CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));

    current["firefox"].generation=43;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    current["firefox"].desktopLookupsComplete=false;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    current["chrome"]=snapshot;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    std::vector<DeskRec> changedDesktops=desktops;
    changedDesktops[0].guid=
        G(L"{231A0000-0000-0000-0000-000000000002}");
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,changedDesktops));
}

static void test_manual_save_incomplete_snapshot_keeps_prior_bytes_without_write(){
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000009401}");
    const std::vector<DeskRec> desktops={{0,desktop,L"one"}};
    SnapshotVersionTracker tracker;
    AppFastSnapshot capturedSnapshot;
    capturedSnapshot.windows.push_back(
        SnapshotWindow(9401,9402,9403,L"",desktop));
    FinalizeFastSnapshot("firefox",1,tracker,capturedSnapshot);
    std::map<std::string,AppFastSnapshot> captured={
        {"firefox",capturedSnapshot}};
    std::map<std::string,AppFastSnapshot> current=captured;
    std::string manualBytes="prior-manual-layout-bytes";
    const std::string candidate="new-checked-v4-bytes";
    int writes=0;
    auto write=[&](const std::string& bytes){
        ++writes;
        manualBytes=bytes;
        return true;
    };

    current["firefox"].enumerationComplete=false;
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,current,desktops,desktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    current=captured;
    current["firefox"].desktopLookupsComplete=false;
    current["firefox"].windows[0].desktop=GUID{};
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,current,desktops,desktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    std::vector<DeskRec> changedDesktops=desktops;
    changedDesktops[0].guid=G(L"{231A0000-0000-0000-0000-000000009402}");
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,captured,desktops,changedDesktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    CHECK(PublishManualSnapshotIfCurrent(
        captured,captured,desktops,desktops,candidate,write));
    CHECK(writes==1 && manualBytes==candidate);
}

static void test_cli_status_keeps_fast_rows_when_fingerprints_unavailable(){
    FastWin fast;
    fast.app="firefox";
    fast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9551));
    fast.pid=9551;
    fast.processStart=955100;
    fast.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    fast.title=L"Known title";
    AppFastSnapshot snapshot;
    snapshot.windows.push_back(fast);
    snapshot.identityGeneration=51;
    snapshot.generation=52;
    DeskRec desktop;
    desktop.index=7;
    desktop.guid=fast.desktop;
    std::vector<CliStatusRow> rows;
    CHECK(BuildCliStatusRows(snapshot,{desktop},nullptr,false,rows));
    CHECK(rows.size()==1 && SameIdentity(
        IdentityOf(rows[0].window),IdentityOf(fast)));
    CHECK(rows[0].deskIndex==7 && rows[0].activeTitle=="Known title");
    CHECK(!rows[0].fingerprintAvailable && rows[0].tabCount==-1);
}

static void test_reconcile_copied_text_budget_covers_every_owned_string(){
    ReconcileRequest request;
    request.app=std::string(1,'a');
    LayoutWin saved;
    saved.recordId=std::string(2,'b');
    saved.app=std::string(3,'c');
    saved.activeTitle=std::string(4,'d');
    saved.activeDomain=std::string(5,'e');
    saved.counts[std::string(6,'f')]=1;
    request.saved.push_back(saved);
    LayoutWin live;
    live.recordId=std::string(7,'g');
    live.app=std::string(8,'h');
    live.activeTitle=std::string(9,'i');
    live.activeDomain=std::string(10,'j');
    live.counts[std::string(11,'k')]=1;
    request.live.push_back(live);
    request.reservedRecordIds.insert(std::string(12,'l'));
    FastWin fast;
    fast.app=std::string(13,'m');
    fast.title=std::wstring(14,L'n');
    request.fastWindows.push_back(fast);
    DeskRec desktop;
    desktop.name=std::wstring(15,L'o');
    request.desktops.push_back(desktop);
    request.titleSuffixes.push_back(std::wstring(16,L'p'));
    const size_t exactBytes=91+45*sizeof(wchar_t);
    CHECK(ReconcileRequestTextWithinBudget(request,exactBytes));
    CHECK(!ReconcileRequestTextWithinBudget(request,exactBytes-1));

    ReconcileRequest oversized=WorkerPreparedRequest(145,L"Bounded - Browser");
    oversized.titleSuffixes[0].assign(
        MAX_RECONCILE_TITLE_SUFFIX_CHARS+1,L'x');
    PreparedReconcileLive untouched;
    untouched.sessionIndexByFast.push_back(42);
    CHECK(!BuildReconcileLivePreparation(oversized,untouched));
    CHECK(untouched.live.empty() && untouched.sessionIndexByFast.size()==1 &&
        untouched.sessionIndexByFast[0]==42);
}

struct BlockingLivePreparer {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false;
    bool released=false;
    int calls=0;
    std::thread::id workerThread;

    bool build(const ReconcileRequest& request,PreparedReconcileLive& output){
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++calls;
            workerThread=std::this_thread::get_id();
            if(calls==1){
                entered=true;
                changed.notify_all();
                changed.wait(lock,[&]{ return released; });
            }
        }
        return BuildReconcileLivePreparation(request,output);
    }

    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }

    void release(){
        std::lock_guard<std::mutex> lock(mutex);
        released=true;
        changed.notify_all();
    }
};

static void test_reconcile_worker_prepares_live_inputs_off_thread_and_coalesces(){
    ReconcileResultSink sink;
    BlockingLivePreparer blocker;
    std::atomic<int> planned{0};
    std::atomic<bool> plannerSawAligned{true};
    ReconcileWorkerOps ops;
    ops.buildLive=[&](const ReconcileRequest& request,PreparedReconcileLive& output){
        return blocker.build(request,output);
    };
    ops.plan=[&](const ReconcileRequest& request){
        ++planned;
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        if(request.live.size()!=request.fastWindows.size())
            plannerSawAligned.store(false);
        return output;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    const std::thread::id caller=std::this_thread::get_id();
    ReconcileRequest firstRequest=WorkerPreparedRequest(141,L"First - Browser");
    const auto firstSession=firstRequest.sessionWindows;
    CHECK(worker.Request(firstRequest));
    firstRequest.fastWindows[0].title=L"caller-mutated";
    firstRequest.titleSuffixes[0]=L"caller-mutated";
    firstRequest.desktops[0].index=99;
    CHECK(blocker.waitEntered());
    CHECK(worker.Request(WorkerPreparedRequest(142,L"Pending - Browser")));
    CHECK(worker.Request(WorkerPreparedRequest(143,L"Newest - Browser")));
    CHECK(worker.ActiveCount()==1 && worker.PendingCount()==1 &&
        worker.OutstandingForApp("firefox")==2);
    std::unique_ptr<ReconcileResult> superseded=sink.waitFor(142);
    CHECK(superseded && superseded->status==ReconcileResultStatus::Superseded);
    CHECK(superseded && superseded->fastWindows.size()==2 &&
        superseded->fastWindows[0].title==L"Pending - Browser");
    blocker.release();
    std::unique_ptr<ReconcileResult> first=sink.waitFor(141);
    std::unique_ptr<ReconcileResult> newest=sink.waitFor(143);
    CHECK(first && first->status==ReconcileResultStatus::Completed);
    CHECK(newest && newest->status==ReconcileResultStatus::Completed);
    CHECK(first && first->fastWindows.size()==2 &&
        first->fastWindows[0].title==L"First - Browser" &&
        first->desktops[0].index==7 &&
        first->titleSuffixes[0]==L" - Browser");
    CHECK(first && first->sessionWindows.get()==firstSession.get());
    CHECK(first && first->live.size()==2 &&
        first->sessionIndexByFast.size()==2);
    const WinFp* firstBound=first ? ReconcileSessionForFast(*first,0) : nullptr;
    CHECK(firstBound && firstBound->tabsBlob=="first-tabs");
    CHECK(blocker.workerThread!=caller && planned.load()==2 && blocker.calls==2 &&
        plannerSawAligned.load());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_prepare_only_skips_planner_for_search(){
    ReconcileResultSink sink;
    std::atomic<int> planned{0};
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest&)->ReconcilePlan{
        ++planned;
        throw std::runtime_error("planner must not run");
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    ReconcileRequest request=WorkerPreparedRequest(144,L"Search - Browser");
    request.workMode=ReconcileWorkMode::PrepareLiveOnly;
    CHECK(worker.Request(request));
    std::unique_ptr<ReconcileResult> result=sink.waitFor(144);
    CHECK(result && result->status==ReconcileResultStatus::Completed);
    CHECK(result && result->workMode==ReconcileWorkMode::PrepareLiveOnly);
    CHECK(result && result->fastWindows.size()==2 && result->live.size()==2);
    const WinFp* bound=result ? ReconcileSessionForFast(*result,0) : nullptr;
    CHECK(bound && bound->tabsBlob=="first-tabs");
    CHECK(planned.load()==0);
    CHECK(worker.Stop());
}

static void test_reconcile_worker_is_bounded_coalesced_and_nonblocking(){
    ReconcileResultSink sink;
    BlockingReconcilePlanner blocker;
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest& request){ return blocker.plan(request); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    const std::thread::id caller=std::this_thread::get_id();
    CHECK(worker.Request(WorkerReconcileRequest(100)));
    CHECK(blocker.waitEntered());
    CHECK(worker.Request(WorkerReconcileRequest(101)));
    CHECK(worker.Request(WorkerReconcileRequest(102)));
    CHECK(worker.ActiveCount()==1);
    CHECK(worker.PendingCount()==1);
    CHECK(worker.OutstandingForApp("firefox")==2);

    std::unique_ptr<ReconcileResult> superseded=sink.waitFor(101);
    CHECK(superseded && superseded->status==ReconcileResultStatus::Superseded);
    CHECK(superseded && superseded->contentGeneration==122);
    blocker.release();
    std::unique_ptr<ReconcileResult> first=sink.waitFor(100);
    std::unique_ptr<ReconcileResult> newest=sink.waitFor(102);
    CHECK(first && first->status==ReconcileResultStatus::Completed);
    CHECK(newest && newest->status==ReconcileResultStatus::Completed);
    CHECK(first && first->saved.size()==1 && first->live.size()==1);
    CHECK(newest && newest->saved.size()==1 && newest->live.size()==1);
    CHECK(blocker.workerThread!=caller);
    CHECK(!sink.wrongMessage.load());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_planner_failure_is_owned_and_thread_survives(){
    ReconcileResultSink sink;
    std::atomic<int> calls{0};
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest& request)->ReconcilePlan{
        if(++calls==1) throw std::bad_alloc();
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        return output;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    CHECK(worker.Request(WorkerReconcileRequest(110)));
    std::unique_ptr<ReconcileResult> failed=sink.waitFor(110);
    CHECK(failed && failed->status==ReconcileResultStatus::Failed);
    CHECK(worker.Request(WorkerReconcileRequest(111)));
    std::unique_ptr<ReconcileResult> recovered=sink.waitFor(111);
    CHECK(recovered && recovered->status==ReconcileResultStatus::Completed);
    CHECK(worker.Stop());
}

static void test_reconcile_worker_accepted_request_owns_failure_result(){
    ReconcileResultSink sink;
    std::atomic<int> allocations{0};
    ReconcileWorkerOps ops;
    ops.makeResult=[&](){
        if(++allocations!=1) return std::unique_ptr<ReconcileResult>();
        return std::unique_ptr<ReconcileResult>(new ReconcileResult());
    };
    ops.plan=[](const ReconcileRequest&)->ReconcilePlan{
        throw std::bad_alloc();
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    CHECK(worker.Request(WorkerReconcileRequest(112)));
    std::unique_ptr<ReconcileResult> failed=
        sink.waitFor(112,std::chrono::milliseconds(250));
    CHECK(failed && failed->status==ReconcileResultStatus::Failed);
    CHECK(allocations.load()==1);
    CHECK(worker.Stop());
}

static void test_reconcile_post_failure_deadlines_all_operation_owners(){
    std::mutex mutex;
    std::condition_variable changed;
    size_t postAttempts=0;
    ReconcileWorkerOps ops;
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++postAttempts;
        }
        changed.notify_all();
        return false;
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    AsyncReconcileDeadlineGate deadlines;
    // Auto, manual save, manual restore and search each own a distinct
    // operation id even when the bounded worker coalesces by application.
    const uint64_t operationIds[]={113,114,115,116};
    for(size_t index=0;index<4;++index){
        CHECK(deadlines.begin(operationIds[index],1000+index));
        CHECK(worker.Request(WorkerReconcileRequest(operationIds[index])));
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock,std::chrono::seconds(5),[&]{
            return postAttempts>=index+1;
        }));
    }
    for(uint64_t operationId : operationIds)
        CHECK(deadlines.pending(operationId)==1);

    std::vector<uint64_t> expired;
    CHECK(deadlines.expire(1003+AsyncReconcileDeadlineGate::maxLifetimeMs(),
                           expired)==4);
    std::sort(expired.begin(),expired.end());
    CHECK(expired==std::vector<uint64_t>({113,114,115,116}));
    for(uint64_t operationId : operationIds)
        CHECK(!deadlines.complete(operationId));
    CHECK(deadlines.expire(UINT64_MAX,expired)==0 && expired.empty());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_rejects_invalid_or_oversized_requests(){
    ReconcileWorker worker(reinterpret_cast<HWND>(1));
    ReconcileRequest request=WorkerReconcileRequest(120);
    request.operationId=0;
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(121);
    request.contentGeneration=0;
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(122);
    request.app="unsupported";
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(123);
    request.saved.resize(MAX_LAYOUT_RECORDS+1);
    CHECK(!worker.Request(request));
    CHECK(worker.Stop());
}

static std::vector<LayoutMatch> TooComplexReconcileMatcher(
        const std::vector<LayoutWin>&,const std::vector<LayoutWin>&,
        double,bool* tooComplex){
    if(tooComplex) *tooComplex=true;
    return {};
}

static void test_reconcile_plan_distinguishes_complexity_deferral(){
    ReconcileRequest request=WorkerReconcileRequest(130);
    ReconcilePlan plan=PlanAppReconcile(
        request.saved,request.live,request.app,request.nowUtc,{},
        request.freshness,NewRecordId,TooComplexReconcileMatcher);
    CHECK(plan.deferred);
    CHECK(plan.tooComplex);
}

static void test_posted_reconcile_results_are_drained(){
    HWND window=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,
                                nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr);
    if(!window) return;
    ReconcileResult* first=new ReconcileResult();
    ReconcileResult* second=new ReconcileResult();
    CHECK(PostMessageW(window,WM_RECONCILE_RESULT,0,
                       reinterpret_cast<LPARAM>(first))!=FALSE);
    CHECK(PostMessageW(window,WM_RECONCILE_RESULT,0,
                       reinterpret_cast<LPARAM>(second))!=FALSE);
    CHECK(DrainPostedReconcileResults(window)==2);
    CHECK(DrainPostedReconcileResults(window)==0);
    CHECK(DestroyWindow(window)!=FALSE);
}

static void test_reconcile_consumer_ignores_stale_content_generation(){
    ReconcileResultConsumerKey expected;
    expected.operationId=9301;
    expected.app="firefox";
    expected.workMode=ReconcileWorkMode::Plan;
    expected.identityGeneration=9302;
    expected.contentGeneration=9303;
    expected.sessionRequestId=9304;
    expected.sessionDataGeneration=9305;

    ReconcileResult result;
    result.status=ReconcileResultStatus::Completed;
    result.operationId=expected.operationId;
    result.app=expected.app;
    result.workMode=expected.workMode;
    result.identityGeneration=expected.identityGeneration;
    result.contentGeneration=expected.contentGeneration;
    result.sessionRequestId=expected.sessionRequestId;
    result.sessionDataGeneration=expected.sessionDataGeneration;
    int mutations=0;
    auto stale=[&](const ReconcileResult& candidate){
        const int before=mutations;
        CHECK(!ConsumeReconcileResultIfCurrent(
            candidate,expected,[&](){ ++mutations; }));
        CHECK(mutations==before);
    };
    ReconcileResult changed=result;
    changed.operationId++;
    stale(changed);
    changed=result; changed.app="chrome"; stale(changed);
    changed=result; changed.workMode=ReconcileWorkMode::PrepareLiveOnly; stale(changed);
    changed=result; changed.identityGeneration++; stale(changed);
    changed=result; changed.contentGeneration++; stale(changed);
    changed=result; changed.sessionRequestId++; stale(changed);
    changed=result; changed.sessionDataGeneration++; stale(changed);
    changed=result; changed.status=ReconcileResultStatus::Superseded; stale(changed);

    CHECK(ConsumeReconcileResultIfCurrent(
        result,expected,[&](){ ++mutations; }));
    CHECK(mutations==1);
    CHECK(!ConsumeReconcileResultIfCurrent(
        result,expected,[&](){ ++mutations; throw std::bad_alloc(); }));
    CHECK(mutations==2);
}

static FinalWindowObservation FinalObserved(const char* app,const char* title,
        const GUID& desktop,const std::string& provisionalId){
    FinalWindowObservation observation;
    observation.observed=MatchRecord(app,title,"",0,{});
    observation.observed.app=app;
    observation.observed.desktop=desktop;
    observation.observed.deskIndex=1;
    observation.desktopValid=!GuidIsZero(desktop);
    observation.provisionalRecordId=provisionalId;
    return observation;
}

static void test_final_snapshot_captures_immediately_opened_new_window(){
    const UnixSeconds now=1700001000;
    FinalWindowObservation opened=FinalObserved(
        "firefox","opened-now",
        G(L"{231A0000-0000-0000-0000-000000000002}"),
        "{00000000-0000-0000-0000-000000009201}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(opened);
    FinalSnapshotResult result=CommitFinalSnapshotRecords({}, {app}, now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==opened.provisionalRecordId);
    CHECK(result.records[0].activeTitle=="opened-now");
    CHECK(GuidEq(result.records[0].desktop,opened.observed.desktop));
    CHECK(result.records[0].lastSeenUtc==now &&
          result.records[0].missingSinceUtc==0);
}

static void test_final_snapshot_marks_unbound_additions_provisional_independent_of_title(){
    const UnixSeconds now=1700001500;
    FinalWindowObservation opened=FinalObserved(
        "firefox","already titled",
        G(L"{231A0000-0000-0000-0000-000000000002}"),
        "{00000000-0000-0000-0000-000000009206}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(opened);

    FinalSnapshotResult result=CommitFinalSnapshotRecords({}, {app}, now);

    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].provisional);
}

static void test_final_snapshot_failed_reappeared_keeps_destination_and_adds_sibling(){
    const UnixSeconds now=1700002000;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009211}","firefox","saved","a.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-100);
    saved.missingSinceUtc=now-50;
    FinalWindowObservation failed=FinalObserved(
        "firefox","saved",
        G(L"{231A0000-0000-0000-0000-000000000009}"),"");
    failed.pendingRecordId=saved.recordId;
    FinalWindowObservation sibling=FinalObserved(
        "firefox","new sibling",
        G(L"{231A0000-0000-0000-0000-000000000003}"),
        "{00000000-0000-0000-0000-000000009212}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={failed,sibling};
    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);
    CHECK(result.valid && result.records.size()==2);
    CHECK(result.records[0].recordId==saved.recordId);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(result.records[0].lastSeenUtc==now &&
          result.records[0].missingSinceUtc==0);
    CHECK(result.records[1].recordId==sibling.provisionalRecordId);
    CHECK(GuidEq(result.records[1].desktop,sibling.observed.desktop));
}

static void test_final_snapshot_zero_live_marks_and_prunes_from_last_seen(){
    const UnixSeconds now=1700003000;
    LayoutWin recent=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009221}","firefox","recent","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    LayoutWin expired=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009222}","firefox","expired","b.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS);
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    FinalSnapshotResult result=CommitFinalSnapshotRecords(
        {recent,expired},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==recent.recordId);
    CHECK(result.records[0].missingSinceUtc==recent.lastSeenUtc);
    CHECK(result.erasedRecordIds.count(expired.recordId)==1);
}

static void test_final_snapshot_incomplete_profile_is_byte_preserved(){
    const UnixSeconds now=1700004000;
    LayoutWin old=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009231}","firefox","old","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS-1);
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Incomplete;
    FinalSnapshotResult result=CommitFinalSnapshotRecords({old},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(SameLayoutWinFields(result.records[0],old));
    CHECK(result.changedRecordIds.empty() && result.erasedRecordIds.empty());
}

static void test_final_snapshot_failed_desktop_lookup_preserves_saved_guid(){
    const UnixSeconds now=1700005000;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009241}","firefox","bound","a.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),now-20);
    FinalWindowObservation observed=FinalObserved("firefox","bound",GUID{},"");
    observed.boundRecordId=saved.recordId;
    observed.desktopValid=false;
    observed.fingerprintFresh=true;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(observed);
    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(!GuidIsZero(result.records[0].desktop));
    CHECK(result.records[0].lastSeenUtc==now);
}

static void test_final_snapshot_stale_pending_uses_unique_title_and_preserves_destination(){
    const UnixSeconds now=1700005500;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009246}","firefox","stable","a.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),now-20);
    FinalWindowObservation observed=FinalObserved(
        "firefox","stable",
        G(L"{231A0000-0000-0000-0000-000000000009}"),
        "{00000000-0000-0000-0000-000000009248}");
    observed.pendingRecordId=
        "{00000000-0000-0000-0000-000000009247}";
    observed.fingerprintFresh=true;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(observed);

    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);

    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==saved.recordId);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(result.records[0].deskIndex==saved.deskIndex);
    CHECK(result.records[0].lastSeenUtc==now);
    CHECK(result.records[0].missingSinceUtc==0);
}

static void test_final_snapshot_reservations_preserve_bound_and_provisional_origin(){
    const UnixSeconds now=1700006000;
    LayoutWin bound=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009251}","firefox","bound","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    FinalWindowObservation boundMove=FinalObserved(
        "firefox","bound",
        G(L"{231A0000-0000-0000-0000-000000000009}"),"");
    boundMove.boundRecordId=bound.recordId;
    boundMove.reserved=true;

    FinalWindowObservation unboundMove=FinalObserved(
        "firefox","new",
        G(L"{231A0000-0000-0000-0000-000000000009}"),
        "{00000000-0000-0000-0000-000000009252}");
    unboundMove.reserved=true;
    unboundMove.hasProvisionalOriginRecord=true;
    unboundMove.provisionalOriginRecord=unboundMove.observed;
    unboundMove.provisionalOriginRecord.recordId=unboundMove.provisionalRecordId;
    unboundMove.provisionalOriginRecord.desktop=
        G(L"{231A0000-0000-0000-0000-000000000002}");
    unboundMove.provisionalOriginRecord.deskIndex=2;
    unboundMove.provisionalOriginRecord.lastSeenUtc=now;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={boundMove,unboundMove};
    FinalSnapshotResult result=CommitFinalSnapshotRecords({bound},{app},now);
    CHECK(result.valid && result.records.size()==2);
    CHECK(SameLayoutWinFields(result.records[0],bound));
    CHECK(result.records[1].recordId==unboundMove.provisionalRecordId);
    CHECK(result.records[1].provisional);
    CHECK(GuidEq(result.records[1].desktop,
                 unboundMove.provisionalOriginRecord.desktop));
    FinalSnapshotResult repeated=CommitFinalSnapshotRecords(
        result.records,{app},now+1);
    CHECK(repeated.valid && repeated.records.size()==2);
}

static void test_initial_partial_enumeration_suppresses_lifecycle_missing_and_write(){
    const UnixSeconds now=1700006500;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009261}","firefox","saved","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-50);
    AppFastSnapshot partial;
    partial.windows.push_back(SnapshotWindow(
        9261,9262,9263,L"",saved.desktop)); // empty title itself is valid
    partial.enumerationComplete=false;       // EnumWindows failed after it
    partial.desktopLookupsComplete=true;
    SnapshotVersionTracker tracker;
    FinalizeFastSnapshot("firefox",1,tracker,partial);

    LcState lifecycle;
    std::vector<LayoutWin> records={saved};
    int lifecycleCalls=0,missingCalls=0,writeCalls=0;
    auto monitorTick=[&](){
        if(!FastSnapshotCanObserve(partial)) return;
        ++lifecycleCalls;
        LcDecision decision=LcObserve(lifecycle,!partial.windows.empty(),
            partial.windowSetSignature,partial.settleSignature,
            partial.layoutSignature,0,0);
        if(decision.action==LcAction::MarkMissingFromLastSeen) ++missingCalls;
        if(decision.action==LcAction::SaveLayout) ++writeCalls;
    };
    monitorTick();
    CHECK(lifecycleCalls==0 && missingCalls==0 && writeCalls==0);
    CHECK(records.size()==1 && SameLayoutWinFields(records[0],saved));

    FinalAppObservation incomplete;
    incomplete.app="firefox";
    incomplete.quality=FinalProfileQuality::Incomplete;
    FinalSnapshotResult final=CommitFinalSnapshotRecords(records,{incomplete},now);
    CHECK(final.valid && final.records.size()==1 &&
          SameLayoutWinFields(final.records[0],saved));
    CHECK(final.changedRecordIds.empty() && final.erasedRecordIds.empty());
}

static void test_bound_a_save_then_unbound_b_reconcile_never_moves_a(){
    const UnixSeconds now=1700006600;
    const GUID originA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID movedA=G(L"{231A0000-0000-0000-0000-000000000002}");
    const GUID destinationB=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin savedA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009271}","firefox","A","a.test",0,
        originA,now-100);
    LayoutWin savedB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009272}","firefox","B","b.test",2,
        destinationB,now-100);
    BoundSaveObservation observedA;
    observedA.window=SnapshotWindow(9271,9272,9273,L"A",movedA);
    observedA.hasBinding=true;
    observedA.expectedIdentity=IdentityOf(observedA.window);
    observedA.recordId=savedA.recordId;
    observedA.deskIndex=1;
    observedA.causalGeneration=9274;
    BoundSaveObservation unboundB;
    unboundB.window=SnapshotWindow(9275,9276,9277,L"B",originA);
    unboundB.deskIndex=0;

    SaveObservedAppResult saved=ApplyObservedBoundRecords(
        {savedA,savedB},"firefox",{observedA,unboundB},true,now);
    CHECK(saved.valid && saved.needsReconcile && saved.updates.size()==1);
    CHECK(GuidEq(saved.records[0].desktop,movedA));

    LayoutWin liveB=savedB;
    liveB.desktop=originA;
    liveB.deskIndex=0;
    liveB.lastSeenUtc=now;
    ReconcilePlan plan=PlanAppReconcile(
        saved.records,{liveB},"firefox",now,{savedA.recordId},
        ReconcileFreshness::Fresh);
    CHECK(!plan.deferred && plan.restores.size()==1);
    CHECK(plan.restores[0].savedIndex==1 && plan.restores[0].liveIndex==0);
    for(const RestoreRequest& restore : plan.restores)
        CHECK(restore.savedIndex!=0);
    CHECK(plan.newRecords.empty());
}

static void test_query_end_destroy_full_final_snapshot_chains(){
    const UnixSeconds now=1700006700;
    const GUID first=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID second=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<DeskRec> desks={{0,first,L"one"},{1,second,L"two"}};

    auto run=[&](std::vector<LayoutWin> initial,
                 const std::vector<FinalAppObservation>& observations,
                 const std::vector<LayoutWin>& expected){
        CheckpointController controller;
        std::vector<LayoutWin> current=initial;
        std::string bytes;
        int writes=0;
        auto checkpoint=[&](CheckpointReason){
            FinalSnapshotResult committed=CommitFinalSnapshotRecords(
                current,observations,now);
            CHECK(committed.valid);
            if(!committed.valid) return false;
            current=committed.records;
            bytes=SerializeLayout(desks,current);
            ++writes;
            return true;
        };
        CHECK(controller.dispatch(
            CheckpointReason::QueryEndSession,true,true,false,checkpoint));
        CHECK(!controller.finalization.finished && writes==1);
        CHECK(controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint));
        CHECK(controller.finalization.finished && writes==2);
        const std::string afterEnd=bytes;
        CHECK(controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint));
        CHECK(writes==2 && bytes==afterEnd);
        CHECK(bytes==SerializeLayout(desks,expected));
    };

    FinalWindowObservation opened=FinalObserved(
        "firefox","opened",second,
        "{00000000-0000-0000-0000-000000009281}");
    FinalAppObservation openedApp;
    openedApp.app="firefox";
    openedApp.quality=FinalProfileQuality::Complete;
    openedApp.windows={opened};
    LayoutWin expectedOpened=opened.observed;
    expectedOpened.recordId=opened.provisionalRecordId;
    expectedOpened.lastSeenUtc=now;
    expectedOpened.missingSinceUtc=0;
    expectedOpened.provisional=true;
    run({}, {openedApp}, {expectedOpened});

    LayoutWin moved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009282}","firefox","moved","a.test",0,
        first,now-100);
    FinalWindowObservation movedObservation=FinalObserved(
        "firefox","moved",second,"");
    movedObservation.boundRecordId=moved.recordId;
    movedObservation.fingerprintFresh=false;
    FinalAppObservation movedApp;
    movedApp.app="firefox";
    movedApp.quality=FinalProfileQuality::Complete;
    movedApp.windows={movedObservation};
    LayoutWin expectedMoved=moved;
    expectedMoved.desktop=second;
    expectedMoved.deskIndex=1;
    expectedMoved.lastSeenUtc=now;
    expectedMoved.missingSinceUtc=0;
    run({moved},{movedApp},{expectedMoved});

    LayoutWin absent=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009283}","firefox","absent","b.test",0,
        first,now-100);
    FinalAppObservation emptyApp;
    emptyApp.app="firefox";
    emptyApp.quality=FinalProfileQuality::Complete;
    LayoutWin expectedAbsent=absent;
    expectedAbsent.missingSinceUtc=absent.lastSeenUtc;
    run({absent},{emptyApp},{expectedAbsent});

    CheckpointController retry;
    int finalizeCalls=0;
    CHECK(retry.dispatch(CheckpointReason::QueryEndSession,true,true,false,
        [](CheckpointReason){ return true; }));
    CHECK(!retry.dispatch(CheckpointReason::Finalize,true,true,false,
        [&](CheckpointReason){ ++finalizeCalls; return false; }));
    CHECK(!retry.finalization.finished);
    CHECK(retry.dispatch(CheckpointReason::Finalize,true,true,false,
        [&](CheckpointReason){ ++finalizeCalls; return true; }));
    CHECK(finalizeCalls==2 && retry.finalization.finished);
}

static void test_issued_move_heartbeat_then_session_end_preserves_origin_and_sibling(){
    const UnixSeconds now=1700006800;
    const GUID origin=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID target=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin sibling=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009291}","firefox","sibling","s.test",0,
        origin,now-100);
    LayoutWin provisional=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009292}","firefox","new","",0,
        origin,now);
    provisional.provisional=true;

    MoveJob issued=MJ(MoveOwner::Picker,9291,9292,"issued-runtime");
    issued.recordId=provisional.recordId;
    issued.destination=target;
    MoveQueue queue;
    CHECK(queue.enqueue(issued));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);
    MoveReservation reservation;
    reservation.token=issued.token;
    reservation.identity={0x9293,9294,9295};
    reservation.originDesktop=origin;
    reservation.provisionalOriginRecord=provisional;
    reservation.hasProvisionalOriginRecord=true;
    MoveReservationBook reservations;
    CHECK(reservations.reserve(reservation)==MoveReservationUpdate::Inserted);

    FinalWindowObservation siblingObserved=FinalObserved(
        "firefox","sibling",target,"");
    siblingObserved.boundRecordId=sibling.recordId;
    FinalWindowObservation moving=FinalObserved(
        "firefox","new",target,provisional.recordId);
    moving.reserved=true;
    moving.hasProvisionalOriginRecord=true;
    moving.provisionalOriginRecord=provisional;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={siblingObserved,moving};

    CheckpointController checkpoint;
    int checkpointCalls=0;
    std::vector<LayoutWin> persisted;
    auto save=[&](CheckpointReason){
        MoveReservation visible;
        CHECK(reservations.lookup(reservation.identity,visible));
        CHECK(queue.nextAction()==MoveAction::Verify);
        FinalSnapshotResult result=CommitFinalSnapshotRecords({sibling},{app},now);
        CHECK(result.valid);
        persisted=result.records;
        ++checkpointCalls;
        return result.valid;
    };
    CHECK(checkpoint.dispatch(
        CheckpointReason::Heartbeat,true,true,true,save));
    CHECK(checkpoint.heartbeatDeferred && checkpointCalls==0);
    CHECK(checkpoint.dispatch(
        CheckpointReason::QueryEndSession,true,true,true,save));
    CHECK(checkpointCalls==1 && persisted.size()==2);
    CHECK(GuidEq(persisted[0].desktop,target));
    CHECK(persisted[1].recordId==provisional.recordId &&
          persisted[1].provisional && GuidEq(persisted[1].desktop,origin));
    CHECK(queue.nextAction()==MoveAction::Verify && reservations.size()==1);
}

static void test_manual_restore_keeps_fixture_bytes_and_reports_once(){
    LayoutTempDir temp;
    const std::wstring path=temp.file(L"manual-restore.vde");
    const std::string prior="manual-layout-byte-for-byte-sentinel";
    CHECK(WriteRawFile(path,prior));

    MoveOperationDispatcher dispatcher;
    const MoveToken first{MoveOwner::ManualTray,9391,9392,0};
    const MoveToken second{MoveOwner::ManualTray,9391,9393,1};
    const MoveToken third{MoveOwner::ManualTray,9391,9394,2};
    CHECK(dispatcher.begin(MoveOwner::ManualTray,9391,{first,second,third}));
    MoveOperationSummary summary;
    int reports=0;
    auto deliver=[&](const MoveToken& token,MoveTerminal terminal){
        MoveDispatchDisposition disposition=dispatcher.dispatch(
            TerminalMoveResult(token,terminal),summary);
        if(disposition==MoveDispatchDisposition::OperationCompleted) ++reports;
        return disposition;
    };
    CHECK(deliver(first,MoveTerminal::Succeeded)==MoveDispatchDisposition::Accepted);
    CHECK(deliver(second,MoveTerminal::PermanentFailure)==MoveDispatchDisposition::Accepted);
    CHECK(deliver(third,MoveTerminal::Exhausted)==
          MoveDispatchDisposition::OperationCompleted);
    CHECK(reports==1 && summary.succeeded==1 &&
          summary.permanentFailures==1 && summary.exhausted==1);
    CHECK(deliver(third,MoveTerminal::Exhausted)==MoveDispatchDisposition::Stale);
    CHECK(reports==1 && ReadRawFile(path)==prior);
}

static void test_auto_load_retry_uses_capped_backoff_and_initializes_once(){
    AutoLoadRetryState state;
    CHECK(state.due(0));
    state.failed(0);
    CHECK(state.nextAttemptMs==1000 && !state.due(999) && state.due(1000));
    state.failed(1000);
    CHECK(state.nextAttemptMs==3000 && !state.due(2999) && state.due(3000));
    for(unsigned attempt=0;attempt<8;++attempt)
        state.failed(state.nextAttemptMs);
    CHECK(state.nextAttemptMs-state.lastAttemptMs==60000);
}

static void test_corrective_initial_observation_is_transactional_before_async_work(){
    std::vector<AppProfile> profiles;
    profiles.push_back(sessionTestProfile("firefox"));
    profiles.push_back(sessionTestProfile("chrome",AppProfile::CHROMIUM));
    SnapshotVersionTracker tracker;
    std::map<std::string,AppFastSnapshot> snapshots;
    AppFastSnapshot firefox;
    firefox.windows.push_back(SnapshotWindow(
        11,101,1001,L"A",G(L"{231A0000-0000-0000-0000-000000000001}")));
    FinalizeFastSnapshot("firefox",1,tracker,firefox);
    snapshots["firefox"]=firefox;
    AppFastSnapshot chrome;
    chrome.windows.push_back(SnapshotWindow(
        22,202,2002,L"B",G(L"{231A0000-0000-0000-0000-000000000002}")));
    FinalizeFastSnapshot("chrome",2,tracker,chrome);
    snapshots["chrome"]=chrome;

    InitialLifecyclePreparation prepared;
    prepared.states["prior"].initialized=true;
    prepared.signatures["prior"]=77;
    int preparedApps=0;
    const bool first=PrepareInitialLifecycleStates(
        profiles,snapshots,500,prepared,
        [&](InitialLifecycleFaultPoint point){
            if(point==InitialLifecycleFaultPoint::AfterApp &&
               ++preparedApps==2) throw 7;
        });
    CHECK(!first && preparedApps==2);
    CHECK(prepared.states.size()==1 && prepared.states.count("prior")==1 &&
          prepared.signatures.size()==1 && prepared.signatures["prior"]==77);
    // The first attempt staged A locally, but no owner/route/wave was
    // published, so a hypothetical late generation cannot match anything.
    CHECK(prepared.states.count("firefox")==0);

    CHECK(PrepareInitialLifecycleStates(profiles,snapshots,600,prepared));
    CHECK(prepared.states.size()==2 && prepared.signatures.size()==2 &&
          prepared.states.count("firefox")==1 &&
          prepared.states.count("chrome")==1 &&
          prepared.states.count("prior")==0);

    std::map<std::string,uint64_t> routes;
    std::vector<std::string> owners;
    LcState& state=prepared.states["firefox"];
    const AppFastSnapshot& current=snapshots["firefox"];
    const LcDecision wave=LcObserve(
        state,true,current.windowSetSignature,current.settleSignature,
        current.layoutSignature,0,601);
    if(wave.action==LcAction::BeginRestore){
        routes["firefox"]=wave.generation;
        owners.push_back("firefox");
    }
    const LcDecision duplicate=LcObserve(
        state,true,current.windowSetSignature,current.settleSignature,
        current.layoutSignature,0,602);
    CHECK(wave.action==LcAction::BeginRestore && wave.generation!=0);
    CHECK(duplicate.action==LcAction::None && routes.size()==1 &&
          routes["firefox"]==wave.generation && owners.size()==1);
}

static void test_corrective_fresh_bound_refresh_promotes_exact_provisional(){
    const UnixSeconds now=1700007000;
    LayoutWin existing=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009301}","firefox",
        "old","old.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-50);
    existing.provisional=true;
    FastWin fast=SnapshotWindow(9301,9302,9303,L"fresh",
        G(L"{231A0000-0000-0000-0000-000000000002}"));
    LayoutWin live=existing;
    live.activeTitle="fresh";
    live.activeDomain="fresh.example";
    live.tabCount=3;
    live.counts={{"fresh.example",3}};
    live.deskIndex=1;

    std::map<std::string,std::string> provisionals;
    const std::string runtime=RuntimeKey(fast);
    provisionals[runtime]=existing.recordId;
    provisionals["other-runtime"]=existing.recordId;
    LayoutWin committed;
    int writes=0;
    CHECK(CommitBoundRecordRefresh(existing,fast,live,
        ReconcileFreshness::Fresh,now,runtime,provisionals,
        [&](const LayoutWin& desired){ committed=desired; ++writes; return true; }));
    CHECK(writes==1 && committed.recordId==existing.recordId);
    CHECK(!committed.provisional && committed.activeTitle=="fresh" &&
          committed.activeDomain=="fresh.example" && committed.tabCount==3 &&
          committed.counts==live.counts && committed.lastSeenUtc==now &&
          committed.missingSinceUtc==0);
    CHECK(GuidEq(committed.desktop,fast.desktop) && committed.deskIndex==1);
    CHECK(provisionals.count(runtime)==0 &&
          provisionals.count("other-runtime")==1);

    LayoutWin cached;
    provisionals[runtime]=existing.recordId;
    CHECK(CommitBoundRecordRefresh(existing,fast,live,
        ReconcileFreshness::CachedStale,now+1,runtime,provisionals,
        [&](const LayoutWin& desired){ cached=desired; return true; }));
    CHECK(cached.provisional && cached.activeTitle==existing.activeTitle &&
          provisionals[runtime]==existing.recordId);

    provisionals[runtime]="{00000000-0000-0000-0000-000000009302}";
    CHECK(CommitBoundRecordRefresh(existing,fast,live,
        ReconcileFreshness::Fresh,now+2,runtime,provisionals,
        [](const LayoutWin&){ return true; }));
    CHECK(provisionals[runtime]==
          "{00000000-0000-0000-0000-000000009302}");
    CHECK(!CommitBoundRecordRefresh(existing,fast,live,
        ReconcileFreshness::Fresh,now+3,runtime,provisionals,
        [](const LayoutWin&){ return false; }));
    CHECK(provisionals[runtime]==
          "{00000000-0000-0000-0000-000000009302}");

    std::vector<LayoutWin> backing(1,existing);
    const std::string stableRecordId=backing[0].recordId;
    provisionals[runtime]=stableRecordId;
    std::vector<LayoutWin> displaced;
    CHECK(CommitBoundRecordRefresh(backing[0],fast,live,
        ReconcileFreshness::Fresh,now+4,runtime,provisionals,
        [&](const LayoutWin& desired){
            std::vector<LayoutWin> replacement(1,desired);
            backing.swap(replacement);
            displaced.swap(replacement);
            displaced[0].recordId=
                "{00000000-0000-0000-0000-000000009399}";
            return true;
        }));
    CHECK(provisionals.count(runtime)==0);
}

static void test_corrective_startup_complete_empty_marks_missing_transactionally(){
    const UnixSeconds now=1700007100;
    std::vector<AppProfile> profiles;
    profiles.push_back(sessionTestProfile("firefox"));
    profiles.push_back(sessionTestProfile("chrome",AppProfile::CHROMIUM));
    profiles.push_back(sessionTestProfile("edge",AppProfile::CHROMIUM));
    SnapshotVersionTracker tracker;
    std::map<std::string,AppFastSnapshot> snapshots;
    AppFastSnapshot firefox;
    FinalizeFastSnapshot("firefox",1,tracker,firefox);
    snapshots["firefox"]=firefox;
    AppFastSnapshot chrome;
    chrome.windows.push_back(SnapshotWindow(9311,9312,9313,L"live",
        G(L"{231A0000-0000-0000-0000-000000000003}")));
    FinalizeFastSnapshot("chrome",2,tracker,chrome);
    snapshots["chrome"]=chrome;
    AppFastSnapshot edge;
    edge.enumerationComplete=false;
    FinalizeFastSnapshot("edge",3,tracker,edge);
    snapshots["edge"]=edge;

    InitialLifecyclePreparation sentinel;
    sentinel.states["sentinel"].initialized=true;
    sentinel.signatures["sentinel"]=77;
    sentinel.missingApps.push_back({"sentinel",77});
    InitialLifecyclePreparation prepared=sentinel;
    int injected=0;
    CHECK(!PrepareInitialLifecycleStates(
        profiles,snapshots,500,prepared,[&](InitialLifecycleFaultPoint point){
            if(point==InitialLifecycleFaultPoint::BeforePublish && ++injected==1)
                throw std::bad_alloc();
        }));
    CHECK(prepared.states.size()==1 && prepared.states.count("sentinel")==1);
    CHECK(prepared.signatures.size()==1 && prepared.signatures["sentinel"]==77);
    CHECK(prepared.missingApps.size()==1 &&
          prepared.missingApps[0].app=="sentinel");

    CHECK(PrepareInitialLifecycleStates(profiles,snapshots,600,prepared));
    CHECK(prepared.states.size()==2 && prepared.states.count("firefox")==1 &&
          prepared.states.count("chrome")==1 && prepared.states.count("edge")==0);
    CHECK(!prepared.states["firefox"].present &&
          prepared.states["chrome"].present);
    CHECK(prepared.missingApps.size()==1 &&
          prepared.missingApps[0].app=="firefox" &&
          prepared.missingApps[0].generation!=0);

    LayoutWin recent=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009311}","firefox",
        "recent","a.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin expired=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009312}","firefox",
        "expired","b.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS-1);
    LayoutWin incomplete=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009313}","edge",
        "keep","c.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS-1);
    ValidatedRecordTouch expiredTouch;
    expiredTouch.recordId=expired.recordId;
    expiredTouch.lastSeenUtc=expired.lastSeenUtc;
    expiredTouch.causalGeneration=88;
    FinalCheckpointMutationState mutationSentinel;
    mutationSentinel.records.push_back(incomplete);
    mutationSentinel.provisionalRecordByRuntime["sentinel"]=incomplete.recordId;
    FinalCheckpointMutationState mutation=mutationSentinel;
    const std::map<std::string,uint64_t> missing={{"firefox",91}};
    const std::map<std::string,std::string> provisionals={
        {"expired-runtime",expired.recordId},{"edge-runtime",incomplete.recordId}};
    int perRecord=0;
    CHECK(!BuildInitialMissingMutation(
        {recent,expired,incomplete},{},{{expired.recordId,expiredTouch}}, {},
        provisionals,LayoutRevision(),missing,now,mutation,
        [&](InitialMissingFaultPoint point){
            if(point==InitialMissingFaultPoint::PerRecord && ++perRecord==1)
                throw std::bad_alloc();
        }));
    CHECK(mutation.records.size()==1 &&
          mutation.records[0].recordId==incomplete.recordId &&
          mutation.provisionalRecordByRuntime.count("sentinel")==1);

    CHECK(BuildInitialMissingMutation(
        {recent,expired,incomplete},{},{{expired.recordId,expiredTouch}}, {},
        provisionals,LayoutRevision(),missing,now,mutation));
    CHECK(mutation.records.size()==2);
    const LayoutWin* marked=nullptr;
    const LayoutWin* kept=nullptr;
    for(const LayoutWin& record : mutation.records){
        if(record.recordId==recent.recordId) marked=&record;
        if(record.recordId==incomplete.recordId) kept=&record;
    }
    CHECK(marked && marked->missingSinceUtc==recent.lastSeenUtc);
    CHECK(kept && SameLayoutWinFields(*kept,incomplete));
    CHECK(mutation.deltas.count(recent.recordId)==1 &&
          mutation.deltas.at(recent.recordId).kind==RecordDeltaKind::MissingMark);
    CHECK(mutation.deltas.count(expired.recordId)==1 &&
          mutation.deltas.at(expired.recordId).kind==RecordDeltaKind::ExpireDelete);
    CHECK(mutation.touches.count(expired.recordId)==0 &&
          mutation.provisionalRecordByRuntime.count("expired-runtime")==0 &&
          mutation.provisionalRecordByRuntime.count("edge-runtime")==1);
}

static void test_corrective_monitor_arm_failure_backs_off_before_loading(){
    AutoLoadRetryState state;
    int monitorAttempts=0,loadAttempts=0,initializations=0;
    int heartbeatAttempts=0,alternatePosts=0;
    bool monitorAvailable=false;
    const auto attempt=[&](uint64_t nowMs){
        return AdvanceAutoRuntimeStart(state,nowMs,
            [&](){ ++monitorAttempts; return monitorAvailable; },
            [&](){ ++loadAttempts; return true; },
            [&](){ ++initializations; return true; },
            [&](){ ++heartbeatAttempts; return true; },
            [&](){ ++alternatePosts; return true; });
    };

    CHECK(attempt(100)==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(!state.loaded && !state.monitorStarted &&
          !state.layoutPrepared && !state.heartbeatStarted);
    CHECK(state.nextAttemptMs==1100 && alternatePosts==1);
    CHECK(loadAttempts==0 && initializations==0 && heartbeatAttempts==0);

    monitorAvailable=true;
    CHECK(attempt(100)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(state.monitorStarted && !state.loaded && loadAttempts==0);
    CHECK(attempt(1099)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==0);
    CHECK(attempt(1100)==AutoRuntimeStartResult::Ready);
    CHECK(state.loaded && state.heartbeatStarted &&
          loadAttempts==1 && initializations==1 && heartbeatAttempts==1);
}

static void test_corrective_monitor_alternate_rearm_is_bounded(){
    AutoLoadRetryState state;
    int monitorAttempts=0,alternatePosts=0;
    const auto attempt=[&](){
        return AdvanceAutoRuntimeStart(state,0,
            [&](){ ++monitorAttempts; return false; },
            [](){ return true; },[](){ return true; },
            [](){ return true; },
            [&](){ ++alternatePosts; return true; });
    };
    for(unsigned index=0;
        index<AutoLoadRetryState::kMaxAlternateMonitorRetries+3;++index)
        CHECK(attempt()==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(alternatePosts==
          static_cast<int>(AutoLoadRetryState::kMaxAlternateMonitorRetries));
    CHECK(monitorAttempts==
          static_cast<int>(AutoLoadRetryState::kMaxAlternateMonitorRetries+3));
    CHECK(!state.loaded && !state.monitorStarted);
}

static void test_corrective_failed_monitor_retry_post_remains_unready(){
    AutoLoadRetryState state;
    int postAttempts=0;
    const AutoRuntimeStartResult result=AdvanceAutoRuntimeStart(
        state,50,[](){ return false; },[](){ return true; },
        [](){ return true; },[](){ return true; },
        [&](){ ++postAttempts; return false; });
    CHECK(result==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(postAttempts==1 && !state.loaded && !state.monitorStarted &&
          !state.layoutPrepared && !state.initialized &&
          !state.heartbeatStarted);
    CHECK(state.nextAttemptMs==1050 && !state.due(1049));
}

static void test_corrective_unavailable_load_then_heartbeat_retry_initializes_once(){
    AutoLoadRetryState state;
    std::string bytes="prior-valid-bytes";
    int monitorStarts=0,loadAttempts=0,initializations=0;
    int heartbeatAttempts=0,heartbeatStarts=0,alternatePosts=0;
    const auto attempt=[&](uint64_t nowMs){
        return AdvanceAutoRuntimeStart(state,nowMs,
            [&](){ ++monitorStarts; return true; },
            [&](){
                ++loadAttempts;
                if(loadAttempts<3) return false;
                bytes="new-valid-bytes";
                return true;
            },
            [&](){ ++initializations; return true; },
            [&](){
                ++heartbeatAttempts;
                if(heartbeatAttempts==1) return false;
                ++heartbeatStarts;
                return true;
            },
            [&](){ ++alternatePosts; return true; });
    };

    CHECK(attempt(0)==AutoRuntimeStartResult::LoadUnavailable);
    CHECK(bytes=="prior-valid-bytes" && loadAttempts==1);
    CHECK(attempt(999)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==1 && bytes=="prior-valid-bytes");
    CHECK(attempt(1000)==AutoRuntimeStartResult::LoadUnavailable);
    CHECK(loadAttempts==2 && bytes=="prior-valid-bytes");
    CHECK(attempt(3000)==AutoRuntimeStartResult::HeartbeatUnavailable);
    CHECK(bytes=="new-valid-bytes" && state.layoutPrepared &&
          !state.initialized && !state.loaded && !state.heartbeatStarted);
    CHECK(initializations==0 && heartbeatAttempts==1);
    CHECK(attempt(3999)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==3 && initializations==0 && heartbeatAttempts==1);
    CHECK(attempt(4000)==AutoRuntimeStartResult::Ready);
    CHECK(state.loaded && state.heartbeatStarted &&
          monitorStarts==1 && loadAttempts==3 && initializations==1 &&
          heartbeatAttempts==2 && heartbeatStarts==1 && alternatePosts==0);
}

static void test_corrective_move_cancellation_retry_progresses_beyond_eight_jobs(){
    MoveCancellationRetryState retry;
    size_t remaining=12;
    int queuedMessages=0,maxQueuedMessages=0,timerAttempts=0;
    auto request=[&](){
        return retry.request(remaining!=0,
            [&](){ ++timerAttempts; return false; },
            [&](){ ++queuedMessages; maxQueuedMessages=(std::max)(
                maxQueuedMessages,queuedMessages); return true; });
    };
    CHECK(request() && queuedMessages==1);
    size_t retired=0;
    while(queuedMessages){
        --queuedMessages;
        const size_t before=remaining;
        if(remaining){ --remaining; ++retired; }
        retry.completePostedAttempt(before,remaining);
        CHECK(request());
    }
    CHECK(retired==12 && remaining==0 && maxQueuedMessages==1 &&
          timerAttempts==12 && !retry.postOutstanding());

    remaining=1;
    CHECK(retry.request(true,[](){ return false; },[](){ return false; })==false);
    CHECK(!retry.postOutstanding());
    CHECK(retry.request(true,[](){ return false; },[](){ return true; }));
    for(unsigned attempt=0;
        attempt<MoveCancellationRetryState::kMaxConsecutiveNoProgress;
        ++attempt){
        retry.completePostedAttempt(1,1);
        if(attempt+1<MoveCancellationRetryState::kMaxConsecutiveNoProgress)
            CHECK(retry.request(true,[](){ return false; },[](){ return true; }));
    }
    CHECK(!retry.request(true,[](){ return false; },[](){ return true; }));
    retry.completePostedAttempt(1,0);
    CHECK(retry.consecutiveNoProgress()==0);
}

static void test_corrective_monitor_retry_deadline_survives_post_cap(){
    AutoLoadRetryState state;
    uint64_t now=0;
    int monitorAttempts=0,loads=0,initializations=0,heartbeats=0;
    for(int failure=0;failure<7;++failure){
        CHECK(AdvanceAutoRuntimeStart(state,now,
            [&](){ ++monitorAttempts; return false; },
            [&](){ ++loads; return true; },
            [&](){ ++initializations; return true; },
            [&](){ ++heartbeats; return true; },[](){ return false; })==
            AutoRuntimeStartResult::MonitorUnavailable);
        uint32_t delay=0;
        CHECK(state.monitorRetryDelayMs(now,delay) && delay>0);
        uint32_t early=0;
        CHECK(state.monitorRetryDelayMs(now+delay-1,early) && early==1);
        now+=delay;
    }
    CHECK(monitorAttempts==7 && loads==0 && initializations==0 && heartbeats==0);
    CHECK(AdvanceAutoRuntimeStart(state,now,
        [&](){ ++monitorAttempts; return true; },
        [&](){ ++loads; return true; },
        [&](){ ++initializations; return true; },
        [&](){ ++heartbeats; return true; },[](){ return false; })==
        AutoRuntimeStartResult::Ready);
    CHECK(state.loaded && monitorAttempts==8 && loads==1 &&
          initializations==1 && heartbeats==1);
    uint32_t disabled=123;
    CHECK(!state.monitorRetryDelayMs(now,disabled));

    AutoLoadRetryState saturated;
    saturated.nextAttemptMs=UINT64_MAX;
    uint32_t delay=0;
    CHECK(saturated.monitorRetryDelayMs(0,delay) && delay==UINT32_MAX-1);
    saturated.lastAttemptMs=100;
    saturated.nextAttemptMs=200;
    CHECK(saturated.monitorRetryDelayMs(300,delay) && delay==0);
}

static void test_corrective_stable_monitor_rearms_dirty_flush_after_timer_failure(){
    bool dirty=true,armed=false,eligible=true;
    int armAttempts=0;
    auto maintain=[&](bool armSucceeds){
        if(!ShouldMaintainDirtyFlush(eligible,dirty,armed)) return;
        ++armAttempts;
        if(armSucceeds) armed=true;
    };
    maintain(false);
    CHECK(dirty && !armed && armAttempts==1);
    maintain(true); // next otherwise-stable monitor tick
    CHECK(dirty && armed && armAttempts==2);
    maintain(true);
    CHECK(armAttempts==2);
    armed=false; dirty=false; maintain(true);
    CHECK(armAttempts==2);
    dirty=true; eligible=false; maintain(true);
    CHECK(armAttempts==2);
}

static void test_checkpoint_controller_heartbeat_and_session_end_chain(){
    CheckpointController controller;
    int calls=0;
    std::vector<CheckpointReason> reasons;
    auto success=[&](CheckpointReason reason){
        ++calls;
        reasons.push_back(reason);
        return true;
    };
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,false,false,false,success));
    CHECK(calls==0);
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,false,success));
    CHECK(calls==1 && reasons.back()==CheckpointReason::Heartbeat);
    CHECK(controller.dispatch(CheckpointReason::QueryEndSession,true,true,false,success));
    CHECK(calls==2 && !controller.finalization.finished);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,false,success));
    CHECK(calls==3 && controller.finalization.finished);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,false,success));
    CHECK(calls==3);
}

static void test_checkpoint_failed_end_is_retryable_at_destroy(){
    CheckpointController controller;
    int calls=0;
    auto failThenPass=[&](CheckpointReason){ return ++calls>1; };
    CHECK(!controller.dispatch(
        CheckpointReason::Finalize,true,true,false,failThenPass));
    CHECK(!controller.finalization.running && !controller.finalization.finished);
    CHECK(controller.dispatch(
        CheckpointReason::Finalize,true,true,false,failThenPass));
    CHECK(calls==2 && controller.finalization.finished);
}

static void test_tray_exit_requires_successful_finalize_before_destroy(){
    CheckpointController controller;
    int writes=0,destroys=0;
    auto checkpoint=[&](CheckpointReason){
        ++writes;
        return writes>1;
    };
    CHECK(!RunTrayExit(
        [&](){ return controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint); },
        [&](){ ++destroys; return true; },
        [&](){ controller.finalization.reopen(); }));
    CHECK(writes==1 && destroys==0 && !controller.finalization.finished);
    CHECK(RunTrayExit(
        [&](){ return controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint); },
        [&](){ ++destroys; return true; },
        [&](){ controller.finalization.reopen(); }));
    CHECK(writes==2 && destroys==1 && controller.finalization.finished);

    CheckpointController throwing;
    writes=0; destroys=0;
    CHECK(!RunTrayExit(
        [&](){
            return throwing.dispatch(CheckpointReason::Finalize,true,true,false,
                [&](CheckpointReason)->bool{ ++writes; throw std::bad_alloc(); });
        },[&](){ ++destroys; return true; },
        [&](){ throwing.finalization.reopen(); }));
    CHECK(writes==1 && destroys==0 && !throwing.finalization.finished);
    CHECK(RunTrayExit(
        [&](){ return throwing.dispatch(
            CheckpointReason::Finalize,true,true,false,
            [&](CheckpointReason){ ++writes; return true; }); },
        [&](){ ++destroys; return true; },
        [&](){ throwing.finalization.reopen(); }));
    CHECK(writes==2 && destroys==1 && throwing.finalization.finished);

    CheckpointController alreadySaved;
    writes=0; destroys=0;
    CHECK(RunTrayExit(
        [&](){ return alreadySaved.dispatch(
            CheckpointReason::Finalize,true,true,false,
            [&](CheckpointReason){ ++writes; return true; }); },
        [&](){
            ++destroys;
            CHECK(alreadySaved.dispatch(
                CheckpointReason::Finalize,true,true,false,
                [&](CheckpointReason){ ++writes; return true; }));
            return true;
        },[&](){ alreadySaved.finalization.reopen(); }));
    CHECK(writes==1 && destroys==1);

    CheckpointController destroyRetry;
    writes=0; destroys=0;
    auto finalizeDestroyRetry=[&](){
        return destroyRetry.dispatch(
            CheckpointReason::Finalize,true,true,false,
            [&](CheckpointReason){ ++writes; return true; });
    };
    CHECK(!RunTrayExit(
        finalizeDestroyRetry,[&](){ ++destroys; return false; },
        [&](){ destroyRetry.finalization.reopen(); }));
    CHECK(writes==1 && destroys==1 && !destroyRetry.finalization.finished);
    CHECK(RunTrayExit(
        finalizeDestroyRetry,[&](){ ++destroys; return true; },
        [&](){ destroyRetry.finalization.reopen(); }));
    CHECK(writes==2 && destroys==2 && destroyRetry.finalization.finished);
}

static void test_corrective_successful_session_end_quiesces_late_work(){
    RuntimeQuiescenceState cancelled;
    int cancelledFinalizations=0,cancelledQuiesces=0;
    CHECK(!FinalizeSessionAndQuiesce(false,
        [&](){ ++cancelledFinalizations; return true; },
        [&](){ return RunRuntimeQuiescence(cancelled,
            [&](){ ++cancelledQuiesces; }); }));
    CHECK(cancelled.acceptsDispatch() && cancelledFinalizations==0 &&
          cancelledQuiesces==0);

    RuntimeQuiescenceState failedSave;
    int failedFinalizations=0,failedQuiesces=0;
    CHECK(FinalizeSessionAndQuiesce(true,
        [&](){ ++failedFinalizations; return false; },
        [&](){ return RunRuntimeQuiescence(failedSave,
            [&](){ ++failedQuiesces; }); }));
    CHECK(!failedSave.acceptsDispatch() && failedSave.quiesced() &&
          failedFinalizations==1 && failedQuiesces==1);

    RuntimeQuiescenceState state;
    int finalizations=0,quiesces=0,timerCallbacks=0,resultCallbacks=0;
    CHECK(FinalizeSessionAndQuiesce(true,
        [&](){ ++finalizations; return true; },
        [&](){ return RunRuntimeQuiescence(state,[&](){ ++quiesces; }); }));
    CHECK(!state.acceptsDispatch() && state.quiesced() &&
          finalizations==1 && quiesces==1);
    if(state.acceptsDispatch()) ++timerCallbacks;
    if(state.acceptsDispatch()) ++resultCallbacks;
    CHECK(timerCallbacks==0 && resultCallbacks==0);
    CHECK(RunRuntimeQuiescence(state,[&](){ ++quiesces; }));
    CHECK(quiesces==1);

    RuntimeQuiescenceState retry;
    int cleanupAttempts=0;
    CHECK(!RunRuntimeQuiescence(retry,[&](){
        ++cleanupAttempts;
        throw std::bad_alloc();
    }));
    CHECK(!retry.acceptsDispatch() && !retry.quiesced());
    CHECK(RunRuntimeQuiescence(retry,[&](){ ++cleanupAttempts; }));
    CHECK(retry.quiesced() && cleanupAttempts==2);
}

static void test_corrective_message_routes_are_no_throw_and_retire_exactly_once(){
    int retired[6]={0,0,0,0,0,0};
    int finallyCalls[6]={0,0,0,0,0,0};
    for(int route=0;route<6;++route){
        CHECK(!RunMessageRouteNoThrow(
            [route](){ if(route>=0) throw std::bad_alloc(); },
            [&](){ ++retired[route]; if(route==5) throw 7; },
            [&](){ ++finallyCalls[route]; }));
    }
    for(int route=0;route<6;++route)
        CHECK(retired[route]==1 && finallyCalls[route]==1);

    int unrelatedRetired=0,destructions=0;
    struct OwnedProbe {
        int* destructions;
        ~OwnedProbe(){ ++*destructions; }
    };
    {
        std::unique_ptr<OwnedProbe> payload(new OwnedProbe{&destructions});
        CHECK(!RunMessageRouteNoThrow(
            [&](){ CHECK(payload.get()!=nullptr); throw std::bad_alloc(); },
            [&](){ ++retired[2]; },[](){}));
    }
    CHECK(destructions==1 && retired[2]==2 && unrelatedRetired==0);
    CHECK(RunMessageRouteNoThrow(
        [](){},[&](){ ++unrelatedRetired; },[](){}));
    CHECK(unrelatedRetired==0);

    AsyncSessionRouteGate routes;
    std::vector<AsyncSessionRetirement> events;
    AsyncSessionRoute target=SessionRoute(
        99301,99302,"firefox",SessionPurpose::ManualSave,99303,99400);
    AsyncSessionRoute sibling=SessionRoute(
        99304,99305,"chrome",SessionPurpose::Search,99306,99400);
    CHECK(routes.submit(target,99000,events)==AsyncRouteAdmission::Accepted);
    CHECK(routes.submit(sibling,99000,events)==AsyncRouteAdmission::Accepted);
    CHECK(!routes.abandon(target.requestId,target.operationId,
                          target.identityGeneration+1));
    CHECK(routes.outstanding()==2);
    CHECK(routes.abandon(target.requestId,target.operationId,
                         target.identityGeneration));
    CHECK(routes.outstanding()==1);
    CHECK(!routes.abandon(target.requestId,target.operationId,
                          target.identityGeneration));
    CHECK(routes.retire(sibling.requestId,sibling.operationId,
        sibling.identityGeneration,AsyncRetirementReason::Completed,events));
}

static void test_settings_checkpoint_rejects_enabled_unloaded_and_preserves_state(){
    CheckpointController controller;
    bool recoveryPending=true;
    bool settingEnabled=true;
    int checkpointCalls=0;
    const bool accepted=controller.dispatch(
        CheckpointReason::SettingsChange,true,false,false,
        [&](CheckpointReason){
            ++checkpointCalls;
            recoveryPending=false;
            return true;
        });
    if(accepted) settingEnabled=false;
    CHECK(!accepted);
    CHECK(checkpointCalls==0);
    CHECK(recoveryPending && settingEnabled);
}

static void test_settings_transaction_rolls_back_and_cancels_only_auto_owner(){
    SettingsRuntimeSnapshot current;
    current.hotkeyVk='D';
    current.hotkeyMods=MOD_CONTROL|MOD_ALT;
    current.autoFix=true;
    current.runAtLogon=false;
    current.firefox=true;
    current.chrome=true;
    current.edge=false;
    SettingsRuntimeSnapshot requested=current;
    requested.hotkeyVk='K';
    requested.hotkeyMods=MOD_SHIFT;
    requested.autoFix=false;
    requested.runAtLogon=true;
    requested.chrome=false;

    bool dialogOpen=true,autoOperation=true,manualOperation=true;
    bool pickerOperation=true,moveTimerArmed=true;
    int checkpoints=0,cancellations=0;
    CHECK(!ApplySettingsRuntimeTransaction(current,requested,
        [&](){ ++checkpoints; return false; },
        [&](){ ++cancellations; autoOperation=false; return true; }));
    CHECK(checkpoints==1 && cancellations==0 && dialogOpen);
    CHECK(current.hotkeyVk=='D' && current.hotkeyMods==(MOD_CONTROL|MOD_ALT) &&
          current.autoFix && !current.runAtLogon && current.firefox &&
          current.chrome && !current.edge);
    CHECK(autoOperation && manualOperation && pickerOperation && moveTimerArmed);

    CHECK(ApplySettingsRuntimeTransaction(current,requested,
        [&](){ ++checkpoints; return true; },
        [&](){
            ++cancellations;
            autoOperation=false;
            return true;
        }));
    CHECK(checkpoints==2 && cancellations==1 && !autoOperation);
    CHECK(manualOperation && pickerOperation && moveTimerArmed);
    CHECK(current.hotkeyVk=='K' && current.hotkeyMods==MOD_SHIFT &&
          !current.autoFix && current.runAtLogon && current.firefox &&
          !current.chrome && !current.edge);

    SettingsRuntimeSnapshot enabled=current;
    enabled.autoFix=true;
    enabled.chrome=true;
    int loadStarts=0;
    CHECK(ApplySettingsRuntimeTransaction(current,enabled,
        [&](){ ++checkpoints; return true; },
        [&](){ ++cancellations; return true; }));
    if(current.autoFix) ++loadStarts;
    CHECK(current.autoFix && current.chrome && loadStarts==1);
    CHECK(checkpoints==2); // enabling a disabled snapshot needs no save

    SettingsRuntimeSnapshot disabled=current;
    disabled.autoFix=false;
    CHECK(ApplySettingsRuntimeTransaction(current,disabled,
        [&](){ ++checkpoints; return true; },
        [&](){ ++cancellations; return true; }));
    CHECK(!current.autoFix && checkpoints==3); // disable only after checkpoint
    CHECK(manualOperation && pickerOperation && moveTimerArmed);
}

static void test_checkpoint_reservation_defers_one_heartbeat_but_not_final(){
    CheckpointController controller;
    int heartbeatCalls=0,finalCalls=0;
    auto count=[&](CheckpointReason reason){
        if(reason==CheckpointReason::Heartbeat) ++heartbeatCalls;
        if(reason==CheckpointReason::Finalize) ++finalCalls;
        return true;
    };
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(heartbeatCalls==0 && controller.heartbeatDeferred);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,true,count));
    CHECK(finalCalls==1 && controller.finalization.finished);

    CheckpointController running;
    CHECK(running.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(running.runDeferredHeartbeat(true,true,false,count));
    CHECK(heartbeatCalls==1 && !running.heartbeatDeferred);
    CHECK(running.runDeferredHeartbeat(true,true,false,count));
    CHECK(heartbeatCalls==1);

    CheckpointController terminal;
    CHECK(terminal.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(terminal.reservationTerminated(false,false,true,true,count));
    CHECK(heartbeatCalls==1 && terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(true,true,true,true,count));
    CHECK(heartbeatCalls==1 && terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(true,false,true,true,count));
    CHECK(heartbeatCalls==2 && !terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(false,false,true,true,count));
    CHECK(heartbeatCalls==2);
}

static void test_tray_instance_scope_is_gui_only_and_covers_work_lifetime(){
    std::vector<std::string> events;
    auto acquire=[&](){
        events.push_back("acquire");
        return TrayInstanceAcquireStatus::Acquired;
    };
    auto body=[&](){ events.push_back("body"); return 17; };
    auto release=[&](){ events.push_back("release"); };

    CHECK(RunWithTrayInstanceScope(false,acquire,body,release)==17);
    CHECK((events==std::vector<std::string>{"acquire","body","release"}));

    events.clear();
    CHECK(RunWithTrayInstanceScope(true,acquire,body,release)==17);
    CHECK((events==std::vector<std::string>{"body"}));

    events.clear();
    auto already=[&](){
        events.push_back("already");
        return TrayInstanceAcquireStatus::AlreadyRunning;
    };
    CHECK(RunWithTrayInstanceScope(false,already,body,release)==0);
    CHECK((events==std::vector<std::string>{"already"}));
}

static void test_browser_classifier_requires_enabled_class_and_exact_executable_basename(){
    std::vector<AppProfile> enabled=BuiltinProfiles(true,true,true);
    const AppProfile* chrome=ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        enabled);
    CHECK(chrome && chrome->id=="chrome");
    CHECK(ClassifyBrowserCandidate(
        L"MozillaWindowClass",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        enabled)==nullptr);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Temp\\notchrome.exe",enabled)==nullptr);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Temp\\chrome.exe.backup",enabled)==nullptr);

    std::vector<AppProfile> firefoxOnly=BuiltinProfiles(true,false,false);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        firefoxOnly)==nullptr);
    const AppProfile* firefox=ClassifyBrowserCandidate(
        L"MozillaWindowClass",L"C:\\Program Files\\Mozilla Firefox\\FIREFOX.EXE",
        firefoxOnly);
    CHECK(firefox && firefox->id=="firefox");
}

static void test_class_lookup_failure_marks_enabled_profiles_incomplete_but_empty_title_is_valid(){
    const std::vector<AppProfile> profiles=BuiltinProfiles(true,true,false);
    std::map<std::string,AppFastSnapshot> snapshots;
    snapshots["firefox"];
    snapshots["chrome"];
    FastWin emptyTitle=SnapshotWindow(
        931,9301,93001,L"",
        G(L"{231A0000-0000-0000-0000-000000000001}"));
    emptyTitle.app="firefox";
    snapshots["firefox"].windows.push_back(emptyTitle);

    CHECK(AcceptFastClassNameRead(18,profiles,snapshots));
    CHECK(snapshots["firefox"].enumerationComplete);
    CHECK(snapshots["chrome"].enumerationComplete);
    CHECK(snapshots["firefox"].windows.size()==1 &&
          snapshots["firefox"].windows[0].title.empty());

    CHECK(!AcceptFastClassNameRead(0,profiles,snapshots));
    CHECK(!snapshots["firefox"].enumerationComplete);
    CHECK(!snapshots["chrome"].enumerationComplete);
    CHECK(snapshots["firefox"].windows.size()==1 &&
          snapshots["firefox"].windows[0].title.empty());
}

static void test_popup_persistence_recaptures_before_classification_and_reports_storage(){
    const WindowIdentityKey captured{0x941,9401,94001};
    int recaptures=0,classifications=0,readinessChecks=0,writes=0;
    auto lostRecapture=[&](const WindowIdentityKey& expected){
        ++recaptures;
        CHECK(SameIdentity(expected,captured));
        return WindowIdentityRecapture::Lost; // same HWND/PID, reused process start
    };
    auto classify=[&](const WindowIdentityKey&){
        ++classifications;
        return PopupBrowserClassification::Tracked;
    };
    auto readiness=[&](){
        ++readinessChecks;
        return PopupPersistenceReadiness::Ready;
    };
    auto persist=[&](){ ++writes; return true; };

    CHECK(CompletePopupMovePersistence(
              captured,lostRecapture,classify,readiness,persist)==
          PopupPersistenceResult::IdentityLost);
    CHECK(recaptures==1 && classifications==0 && readinessChecks==0 && writes==0);

    auto matchingRecapture=[&](const WindowIdentityKey&){
        ++recaptures;
        return WindowIdentityRecapture::Match;
    };
    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,
              [&](const WindowIdentityKey&){
                  ++classifications;
                  return PopupBrowserClassification::NotTracked;
              },readiness,persist)==PopupPersistenceResult::NotTracked);
    CHECK(recaptures==2 && classifications==1 && readinessChecks==0 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,
              [&](){
                  ++readinessChecks;
                  return PopupPersistenceReadiness::Unavailable;
              },persist)==PopupPersistenceResult::StorageUnavailable);
    CHECK(recaptures==3 && classifications==2 && readinessChecks==1 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,
              [&](){
                  ++readinessChecks;
                  return PopupPersistenceReadiness::ReadOnly;
              },persist)==PopupPersistenceResult::StorageReadOnly);
    CHECK(recaptures==4 && classifications==3 && readinessChecks==2 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,readiness,persist)==
          PopupPersistenceResult::Saved);
    CHECK(recaptures==5 && classifications==4 && readinessChecks==3 && writes==1);
    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,readiness,[&](){ ++writes; return false; })==
          PopupPersistenceResult::SaveFailed);
    CHECK(recaptures==6 && classifications==5 && readinessChecks==4 && writes==2);
}

static void test_popup_saved_only_completes_exact_lifecycle_save_generation(){
    LcState state;
    state.saveInFlight=true;
    state.saveGeneration=91;
    state.saveRequestedLayoutSignature=901;
    int completions=0;
    const auto complete=[&](uint64_t generation){
        return CompletePopupLifecycleAfterPersistence(
            PopupPersistenceResult::Saved,[&]{
                ++completions;
                LcExplicitSaveCompleted(state,generation,902,903,904);
            });
    };
    const PopupPersistenceResult failures[]={
        PopupPersistenceResult::NotTracked,
        PopupPersistenceResult::IdentityLost,
        PopupPersistenceResult::IdentityIndeterminate,
        PopupPersistenceResult::ClassificationFailed,
        PopupPersistenceResult::StorageUnavailable,
        PopupPersistenceResult::StorageReadOnly,
        PopupPersistenceResult::SaveFailed
    };
    for(PopupPersistenceResult result : failures){
        CHECK(!CompletePopupLifecycleAfterPersistence(result,[&]{
            ++completions;
            LcExplicitSaveCompleted(state,91,902,903,904);
        }));
    }
    CHECK(completions==0 && state.saveInFlight && state.saveGeneration==91);
    CHECK(complete(90));
    CHECK(completions==1 && state.saveInFlight && state.saveGeneration==91);
    CHECK(complete(91));
    CHECK(completions==2 && !state.saveInFlight && state.saveGeneration==0);
}

static void test_popup_uses_exact_pending_saved_id_before_new_provisional(){
    const WindowIdentityKey identity{0x942,9402,94002};
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009402}";
    const std::map<std::string,std::string> pending={
        {RuntimeKey(identity),savedId}};
    std::string selected="sentinel";
    int validations=0;
    auto validate=[&](const std::string& candidate,
                      const std::string& app,std::string& canonical){
        ++validations;
        if(candidate!=savedId || app!="firefox") return false;
        canonical=candidate;
        return true;
    };
    CHECK(SelectPendingPopupRecordId(
        identity,"firefox",pending,validate,selected));
    CHECK(selected==savedId && validations==1);

    WindowIdentityKey reused=identity;
    ++reused.processStart;
    selected="unchanged";
    CHECK(!SelectPendingPopupRecordId(
        reused,"firefox",pending,validate,selected));
    CHECK(selected=="unchanged" && validations==1);
    CHECK(!SelectPendingPopupRecordId(
        identity,"chrome",pending,validate,selected));
    CHECK(selected=="unchanged" && validations==2);
}

static void test_popup_pending_id_bypasses_title_and_origin_provisional_gates(){
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009403}";
    for(int failure=0;failure<2;++failure){
        int pendingCalls=0,provisionalCalls=0;
        std::string selected="sentinel";
        const bool titleComplete=failure!=0;
        const bool originDesktopValid=failure!=1;
        const PopupReservationRecordSource source=
            SelectPopupReservationRecord(
                true,true,titleComplete,originDesktopValid,
                [&](std::string& output){
                    ++pendingCalls;
                    output=savedId;
                    return true;
                },
                [&](std::string& output){
                    ++provisionalCalls;
                    output="{00000000-0000-0000-0000-000000009404}";
                    return true;
                },selected);
        CHECK(source==PopupReservationRecordSource::Pending);
        CHECK(selected==savedId && pendingCalls==1 && provisionalCalls==0);
    }

    int provisionalCalls=0;
    std::string selected="unchanged";
    CHECK(SelectPopupReservationRecord(
        true,true,false,true,
        [](std::string&){ return false; },
        [&](std::string&){ ++provisionalCalls; return true; },selected)==
        PopupReservationRecordSource::None);
    CHECK(selected=="unchanged" && provisionalCalls==0);
    CHECK(SelectPopupReservationRecord(
        true,true,true,false,
        [](std::string&){ return false; },
        [&](std::string&){ ++provisionalCalls; return true; },selected)==
        PopupReservationRecordSource::None);
    CHECK(selected=="unchanged" && provisionalCalls==0);
}

static void test_picker_inflight_accepted_match_reuses_one_exact_saved_id(){
    const WindowIdentityKey target{0x9407,9407,94007};
    WindowIdentityKey sibling{0x9408,9408,94008};
    const std::string savedA=
        "{00000000-0000-0000-0000-000000009407}";
    const std::string savedB=
        "{00000000-0000-0000-0000-000000009408}";
    std::string selected;
    bool found=false;

    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",sibling,"firefox",savedB,selected,found)==
        PickerAcceptedPlanRecordResult::Unrelated);
    CHECK(!found && selected.empty());
    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",target,"firefox",savedA,selected,found)==
        PickerAcceptedPlanRecordResult::Selected);
    CHECK(found && selected==savedA);
    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",target,"firefox",savedA,selected,found)==
        PickerAcceptedPlanRecordResult::Selected);
    CHECK(found && selected==savedA);

    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",target,"firefox",savedB,selected,found)==
        PickerAcceptedPlanRecordResult::Rejected);
    CHECK(found && selected==savedA);

    std::string rejected;
    bool rejectedFound=false;
    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",target,"chrome",savedA,rejected,rejectedFound)==
        PickerAcceptedPlanRecordResult::Rejected);
    CHECK(!rejectedFound && rejected.empty());
    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",target,"firefox","",rejected,rejectedFound)==
        PickerAcceptedPlanRecordResult::Rejected);
    CHECK(!rejectedFound && rejected.empty());

    // A transient class/image lookup failure leaves the capture app empty.
    // An already accepted exact plan is still authoritative and atomically
    // supplies both its canonical app and saved record ID.
    std::string adoptedApp,adoptedRecord;
    bool adopted=false;
    CHECK(AccumulatePickerAcceptedPlanRecordAdoptingApp(
        target,"",target,"firefox",savedA,
        adoptedApp,adoptedRecord,adopted)==
        PickerAcceptedPlanRecordResult::Selected);
    CHECK(adopted && adoptedApp=="firefox" && adoptedRecord==savedA);
    CHECK(AccumulatePickerAcceptedPlanRecordAdoptingApp(
        target,"",target,"chrome",savedB,
        adoptedApp,adoptedRecord,adopted)==
        PickerAcceptedPlanRecordResult::Rejected);
    CHECK(adoptedApp=="firefox" && adoptedRecord==savedA);

    std::string unrelatedApp,unrelatedRecord;
    bool unrelatedFound=false;
    CHECK(AccumulatePickerAcceptedPlanRecordAdoptingApp(
        target,"",sibling,"firefox",savedB,
        unrelatedApp,unrelatedRecord,unrelatedFound)==
        PickerAcceptedPlanRecordResult::Unrelated);
    CHECK(!unrelatedFound && unrelatedApp.empty() &&
          unrelatedRecord.empty());

    WindowIdentityKey reused=target;
    ++reused.processStart;
    CHECK(AccumulatePickerAcceptedPlanRecord(
        target,"firefox",reused,"firefox",savedA,rejected,rejectedFound)==
        PickerAcceptedPlanRecordResult::Unrelated);
    CHECK(!rejectedFound && rejected.empty());

    std::map<std::string,std::string> pending={
        {RuntimeKey(target),savedB}};
    std::map<std::string,std::string> staged;
    CHECK(StagePickerAcceptedPlanPendingAssociation(
        pending,RuntimeKey(target),savedA,staged));
    pending.swap(staged);

    // The Picker guard/transition may now be cancelled and released.  The
    // canonical accepted-plan association must remain independently durable,
    // so the next explicit move reuses A rather than allocating B again.
    std::string afterCancel="unchanged";
    CHECK(SelectPendingPopupRecordId(
        target,"firefox",pending,
        [&](const std::string& candidate,const std::string& app,
            std::string& canonical){
            if(candidate!=savedA || app!="firefox") return false;
            canonical=candidate;
            return true;
        },afterCancel));
    CHECK(afterCancel==savedA);
    CHECK(pending.size()==1 && pending.begin()->second==savedA);

    std::map<std::string,PickerOperationLifetimeClaim> operationClaims;
    std::map<std::string,PickerOperationLifetimeClaim> stagedClaims;
    CHECK(StagePickerOperationLifetimeClaim(
        operationClaims,RuntimeKey(target),savedA,true,stagedClaims));
    operationClaims.swap(stagedClaims);
    // The transition/guard can later be fully gone while a slow sibling still
    // owns the accepted auto operation.  The terminal Picker outcome decides
    // whether that stale Finish protects A or rearms its displaced restore.
    CHECK(PickerOperationLifetimeClaimMatches(
        operationClaims,target,savedA));
    CHECK(!PickerOperationLifetimeClaimMatches(
        operationClaims,target,savedB));
    CHECK(!PickerOperationLifetimeClaimMatches(
        operationClaims,reused,savedA));
    // The accepted restore T->A can still have a slow sibling.  Merely
    // adopting A is not publication: Esc before Save must rearm T instead of
    // letting the sibling report the operation restored.
    CHECK(!operationClaims.begin()->second.pickerPublished);
    CHECK(!MarkPickerOperationLifetimeClaimPublished(
        operationClaims,RuntimeKey(target),savedB,
        PopupSaveStatus::Saved,PopupSaveFailure::None));
    CHECK(!MarkPickerOperationLifetimeClaimPublished(
        operationClaims,RuntimeKey(reused),savedA,
        PopupSaveStatus::Saved,PopupSaveFailure::None));
    CHECK(!PickerOperationLifetimeClaimMustProtect(
        operationClaims.begin()->second,false));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,false,true)==
        PickerOperationLifetimeClaimReleaseAction::RearmRestore);

    CHECK(MarkPickerOperationLifetimeClaimPublished(
        operationClaims,RuntimeKey(target),savedA,
        PopupSaveStatus::Saved,PopupSaveFailure::None));
    CHECK(operationClaims.begin()->second.pickerPublished &&
          operationClaims.begin()->second.pickerEpisodePublished);
    CHECK(MarkPickerOperationLifetimeClaimPublished(
        operationClaims,RuntimeKey(target),savedA,
        PopupSaveStatus::Saved,PopupSaveFailure::None));
    CHECK(PickerOperationLifetimeClaimMustProtect(
        operationClaims.begin()->second,false));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,false,true)==
        PickerOperationLifetimeClaimReleaseAction::ProtectPublished);
    // A second Picker transition for the same exact A is a new ownership
    // episode.  It must not inherit P1's Saved bit while a slow sibling keeps
    // the accepted automatic operation alive.
    std::map<std::string,PickerOperationLifetimeClaim> repeatedClaims;
    CHECK(StagePickerOperationLifetimeClaim(
        operationClaims,RuntimeKey(target),savedA,true,repeatedClaims));
    operationClaims.swap(repeatedClaims);
    CHECK(operationClaims.begin()->second.pickerPublished);
    CHECK(!operationClaims.begin()->second.pickerEpisodePublished);
    CHECK(!operationClaims.begin()->second.pickerTerminalObserved);
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::RearmOperation);
    CHECK(MarkPickerOperationLifetimeClaimTerminalOutcome(
        operationClaims,RuntimeKey(target),savedA,true));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::ProtectPublished);
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,true,true)==
        PickerOperationLifetimeClaimReleaseAction::ProtectPublished);
    CHECK(StagePickerOperationLifetimeClaim(
        operationClaims,RuntimeKey(target),savedA,true,repeatedClaims));
    operationClaims.swap(repeatedClaims);
    CHECK(MarkPickerOperationLifetimeClaimTerminalOutcome(
        operationClaims,RuntimeKey(target),savedA,false));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::RearmOperation);
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        operationClaims.begin()->second,true,true)==
        PickerOperationLifetimeClaimReleaseAction::RearmOperation);

    std::map<std::string,PickerOperationLifetimeClaim> newClaims;
    CHECK(StagePickerOperationLifetimeClaim(
        newClaims,RuntimeKey(target),savedB,false,stagedClaims));
    newClaims.swap(stagedClaims);
    // A cancelled/rolled-back Picker never published new C, so the deferred
    // auto Finish may publish it.  Existence alone is not publication.
    CHECK(!PickerOperationLifetimeClaimMustProtect(
        newClaims.begin()->second,false));
    CHECK(!PickerOperationLifetimeClaimMustProtect(
        newClaims.begin()->second,true));
    CHECK(MarkPickerOperationLifetimeClaimTerminalOutcome(
        newClaims,RuntimeKey(target),savedB,true));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        newClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::ReleaseToPlan);

    // Flush failure happens after the delta/record/binding swap.  It is a
    // queued Picker publication and must retain ownership against stale auto
    // completion just like Saved.
    CHECK(MarkPickerOperationLifetimeClaimPublished(
        newClaims,RuntimeKey(target),savedB,
        PopupSaveStatus::Failed,PopupSaveFailure::FlushFailed));
    CHECK(PickerOperationLifetimeClaimMustProtect(
        newClaims.begin()->second,true));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        newClaims.begin()->second,true,true)==
        PickerOperationLifetimeClaimReleaseAction::ProtectPublished);

    std::map<std::string,PickerOperationLifetimeClaim> cancelledClaims;
    CHECK(StagePickerOperationLifetimeClaim(
        cancelledClaims,RuntimeKey(target),savedA,true,stagedClaims));
    cancelledClaims.swap(stagedClaims);
    CHECK(!MarkPickerOperationLifetimeClaimPublished(
        cancelledClaims,RuntimeKey(target),savedA,
        PopupSaveStatus::Failed,PopupSaveFailure::StorageReadOnly));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        cancelledClaims.begin()->second,true,true)==
        PickerOperationLifetimeClaimReleaseAction::RearmRestore);
    // A no-restore accepted match is releasable only after the Picker proves
    // the target returned to its captured origin.  Unknown/partial rollback
    // must rearm the whole reconcile wave instead of publishing stale success.
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        cancelledClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::RearmOperation);
    CHECK(MarkPickerOperationLifetimeClaimTerminalOutcome(
        cancelledClaims,RuntimeKey(target),savedA,true));
    CHECK(DecidePickerOperationLifetimeClaimRelease(
        cancelledClaims.begin()->second,true,false)==
        PickerOperationLifetimeClaimReleaseAction::ReleaseToPlan);

    PickerTransition restoredTarget;
    restoredTarget.targetOrigin=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    restoredTarget.observedTargetDesktop=restoredTarget.targetOrigin;
    restoredTarget.observedTargetValidity=PickerReadValidity::Valid;
    CHECK(PickerTransitionTargetRestoredToOrigin(restoredTarget));
    restoredTarget.targetMayHaveMoved=true;
    CHECK(!PickerTransitionTargetRestoredToOrigin(restoredTarget));
    restoredTarget.targetMayHaveMoved=false;
    restoredTarget.targetIdentityUnusable=true;
    CHECK(!PickerTransitionTargetRestoredToOrigin(restoredTarget));

    CHECK(DecidePickerOperationClaimQueuePublication(false)==
        PickerOperationClaimQueueAction::RetainClaimAndRearmOperation);
    CHECK(DecidePickerOperationClaimQueuePublication(true)==
        PickerOperationClaimQueueAction::EraseClaimAndAwaitMove);
    CHECK(DecidePickerRearmedMoveArmResult(false)==
        PickerRearmedMoveArmAction::RearmOperation);
    CHECK(DecidePickerRearmedMoveArmResult(true)==
        PickerRearmedMoveArmAction::AwaitMove);

    PickerOperationClaimRearmControl firstThenFailure;
    CHECK(ObservePickerOperationClaimQueuePublication(
        firstThenFailure,true)==
        PickerOperationClaimQueueAction::EraseClaimAndAwaitMove);
    CHECK(firstThenFailure.moveQueued &&
          !PickerOperationClaimRearmRequiresRetry(firstThenFailure));
    CHECK(ObservePickerOperationClaimQueuePublication(
        firstThenFailure,false)==
        PickerOperationClaimQueueAction::RetainClaimAndRearmOperation);
    CHECK(PickerOperationClaimRearmRequiresRetry(firstThenFailure));

    PickerOperationClaimRearmControl armFailure;
    CHECK(ObservePickerOperationClaimQueuePublication(
        armFailure,true)==
        PickerOperationClaimQueueAction::EraseClaimAndAwaitMove);
    BeginPickerOperationClaimMoveArm(armFailure);
    CHECK(armFailure.armPending &&
          PickerOperationClaimRearmRequiresRetry(armFailure));
    CHECK(CompletePickerOperationClaimMoveArm(armFailure,false)==
        PickerRearmedMoveArmAction::RearmOperation);
    CHECK(PickerOperationClaimRearmRequiresRetry(armFailure));

    PickerOperationClaimRearmControl armed;
    CHECK(ObservePickerOperationClaimQueuePublication(armed,true)==
        PickerOperationClaimQueueAction::EraseClaimAndAwaitMove);
    BeginPickerOperationClaimMoveArm(armed);
    CHECK(CompletePickerOperationClaimMoveArm(armed,true)==
        PickerRearmedMoveArmAction::AwaitMove);
    CHECK(!PickerOperationClaimRearmRequiresRetry(armed));

    CHECK(DecidePickerAutoCancelledMoveOwnerAction(false)==
        PickerAutoCancelledMoveOwnerAction::IgnoreSupersession);
    CHECK(DecidePickerAutoCancelledMoveOwnerAction(true)==
        PickerAutoCancelledMoveOwnerAction::RearmOperation);

    // No map iterator may survive FinishAutoOperation: an arm-failure callback
    // can synchronously erase the previously "next" operation.
    std::map<uint64_t,int> operationCursor={
        {101,1},{202,2},{303,3}};
    auto cursor=PickerOperationCursorNext(operationCursor,false,0);
    CHECK(cursor!=operationCursor.end() && cursor->first==101);
    const uint64_t completedOperation=cursor->first;
    operationCursor.erase(202);
    cursor=PickerOperationCursorNext(
        operationCursor,true,completedOperation);
    CHECK(cursor!=operationCursor.end() && cursor->first==303);
    operationCursor.erase(303);
    CHECK(PickerOperationCursorNext(
        operationCursor,true,completedOperation)==operationCursor.end());
}

static void test_picker_save_accepts_only_exact_same_generation_late_fresh(){
    const WindowIdentityKey target{0x9409,9409,94009};
    WindowIdentityKey reused=target;
    ++reused.processStart;
    CHECK(SelectPickerFreshRecordSource(
        target,71,false,WindowIdentityKey{},0,
        true,target,71)==PickerFreshRecordSource::AcceptedAfterEntry);
    CHECK(SelectPickerFreshRecordSource(
        target,71,false,WindowIdentityKey{},0,
        true,target,70)==PickerFreshRecordSource::None);
    CHECK(SelectPickerFreshRecordSource(
        target,71,false,WindowIdentityKey{},0,
        true,reused,71)==PickerFreshRecordSource::None);
    CHECK(SelectPickerFreshRecordSource(
        target,71,true,target,71,
        false,WindowIdentityKey{},0)==PickerFreshRecordSource::Reserved);
    CHECK(SelectPickerFreshRecordSource(
        target,71,true,target,71,
        true,target,71)==PickerFreshRecordSource::AcceptedAfterEntry);
    CHECK(SelectPickerFreshRecordSource(
        target,0,true,target,0,
        true,target,0)==PickerFreshRecordSource::None);
}

static void test_picker_controlled_edit_allows_readback_but_not_mutation(){
    CHECK(PickerControlledEditMessageAllowed(WM_GETTEXT));
    CHECK(PickerControlledEditMessageAllowed(WM_GETTEXTLENGTH));
    CHECK(PickerControlledEditMessageAllowed(WM_PAINT));
    CHECK(PickerControlledEditMessageAllowed(WM_PRINTCLIENT));
    CHECK(PickerControlledEditMessageAllowed(WM_SETFOCUS));
    CHECK(PickerControlledEditMessageAllowed(WM_KILLFOCUS));
    CHECK(PickerControlledEditMessageAllowed(WM_NCDESTROY));
    CHECK(!PickerControlledEditMessageAllowed(WM_SETTEXT));
    CHECK(!PickerControlledEditMessageAllowed(WM_KEYDOWN));
    CHECK(!PickerControlledEditMessageAllowed(WM_LBUTTONDOWN));
    CHECK(!PickerControlledEditMessageAllowed(WM_CHAR));
    CHECK(!PickerControlledEditMessageAllowed(WM_PASTE));

    CHECK(RoutePickerIdleEditInput(
        WM_KEYDOWN,VK_SPACE,true)==PickerIdleEditInputRoute::Grid);
    CHECK(RoutePickerIdleEditInput(
        WM_CHAR,L' ',true)==PickerIdleEditInputRoute::Swallow);
    CHECK(RoutePickerIdleEditInput(
        WM_KEYDOWN,VK_SPACE,false)==PickerIdleEditInputRoute::Edit);
    CHECK(RoutePickerIdleEditInput(
        WM_CHAR,L' ',false)==PickerIdleEditInputRoute::Edit);
    CHECK(RoutePickerIdleEditInput(
        WM_KEYDOWN,VK_RETURN,false)==PickerIdleEditInputRoute::Grid);
}

static void test_picker_inflight_plan_gate_and_late_handoff_cutoff_are_exact(){
    CHECK(DecidePickerInFlightPlanEntry(
        false,false,false,false)==PickerInFlightPlanEntryAction::Allow);
    CHECK(DecidePickerInFlightPlanEntry(
        true,false,false,false)==PickerInFlightPlanEntryAction::Wait);
    CHECK(DecidePickerInFlightPlanEntry(
        true,true,false,false)==PickerInFlightPlanEntryAction::Allow);
    CHECK(DecidePickerInFlightPlanEntry(
        true,true,true,false)==PickerInFlightPlanEntryAction::Wait);
    CHECK(DecidePickerInFlightPlanEntry(
        true,true,true,true)==PickerInFlightPlanEntryAction::ReuseAccepted);

    CHECK(DecidePickerLatePlanHandoff(
        false,false,false)==PickerLatePlanHandoffAction::Ignore);
    CHECK(DecidePickerLatePlanHandoff(
        true,true,false)==PickerLatePlanHandoffAction::TransferBeforeSave);
    CHECK(DecidePickerLatePlanHandoff(
        true,true,true)==PickerLatePlanHandoffAction::RejectPlan);
    CHECK(DecidePickerLatePlanHandoff(
        true,false,false)==PickerLatePlanHandoffAction::RejectPlan);

    RestoreBudgets budgets;
    const RestoreBudgetKey oldB{"record-b","runtime","desktop"};
    const RestoreBudgetKey acceptedA{"record-a","runtime","desktop"};
    budgets.markExhausted(oldB);
    budgets.markExhausted(acceptedA);
    bool published=false;
    CHECK(CommitPickerAcceptedPlanRecordTransfer(
        [&](){
            budgets.clearForExplicitRetry("record-a");
            return true;
        },[&]() noexcept { published=true; }));
    CHECK(published && budgets.mayAttempt(acceptedA));
    CHECK(!budgets.mayAttempt(oldB));
    published=false;
    CHECK(!CommitPickerAcceptedPlanRecordTransfer(
        [](){ return false; },[&]() noexcept { published=true; }));
    CHECK(!published);
}

static void test_picker_failure_refresh_selection_uses_actual_readback(){
    const GUID origin=G(L"{231A0000-0000-0000-0000-000000000031}");
    const GUID destination=G(L"{231A0000-0000-0000-0000-000000000032}");
    PickerState state=PickerTransitionFixture(94010);
    state.currentDesktop=origin;
    CHECK(SetPickerSelection(state,1,destination));
    state.transition.failed=true;
    state.transition.observedCurrentValidity=PickerReadValidity::Valid;
    state.transition.observedCurrentDesktop=origin;
    CHECK(PreparePickerRefreshSelectionFromActual(state));
    CHECK(ResolvePickerSelection(state,{origin,destination}));
    CHECK(GuidEq(state.currentDesktop,origin));
    CHECK(GuidEq(state.selectedDesktop,origin));
    CHECK(state.selectedIndex==0);

    state=PickerTransitionFixture(94011);
    state.currentDesktop=origin;
    CHECK(SetPickerSelection(state,1,destination));
    state.transition.cancelRequested=true;
    state.transition.observedCurrentValidity=PickerReadValidity::Unavailable;
    CHECK(PreparePickerRefreshSelectionFromActual(state));
    CHECK(ResolvePickerSelection(state,{origin,destination}));
    CHECK(GuidEq(state.selectedDesktop,origin));

    state=PickerTransitionFixture(94012);
    state.currentDesktop=origin;
    CHECK(SetPickerSelection(state,1,destination));
    CHECK(!PreparePickerRefreshSelectionFromActual(state));
    CHECK(ResolvePickerSelection(state,{origin,destination}));
    CHECK(GuidEq(state.selectedDesktop,destination));
}

static void test_picker_post_save_identity_diagnostics_preserve_save_truth(){
    for(PopupSaveStatus status : {
            PopupSaveStatus::Saved,PopupSaveStatus::NotTracked}){
        PickerState state=PickerTransitionFixture(
            status==PopupSaveStatus::Saved?94013:94014);
        state.transition.phase=PickerPhase::SaveExactTarget;
        state.transition.commitCutoffReached=true;
        state.transition.pendingEffect=PickerEffectKind::SaveExactTarget;
        state.transition.effectSerial=5;
        PickerEffect save;
        save.kind=PickerEffectKind::SaveExactTarget;
        save.generation=state.transition.generation;
        save.effectSerial=5;
        PickerObservation completed=PickerObservationFor(
            save,PickerEvent::EffectCompleted);
        completed.identity=PickerIdentityValidity::Lost;
        completed.saveStatus=status;
        completed.saveFailure=PopupSaveFailure::None;
        CHECK(AdvancePickerTransition(state,completed).kind==
              PickerEffectKind::Refresh);
        CHECK(state.transition.failed);
        const std::wstring diagnostic=state.transition.diagnostic;
        CHECK(diagnostic.find(L"could not be saved")==std::wstring::npos);
        CHECK(diagnostic.find(L"remains unsaved")==std::wstring::npos);
        if(status==PopupSaveStatus::Saved)
            CHECK(diagnostic.find(L"was saved")!=std::wstring::npos);
        else
            CHECK(diagnostic.find(L"window identity")!=std::wstring::npos);
    }
}

static void test_picker_switch_effect_revalidates_target_at_invocation_boundary(){
    CHECK(PickerForwardSwitchInvocationAllowed(
        PickerIdentityValidity::Match,true));
    CHECK(!PickerForwardSwitchInvocationAllowed(
        PickerIdentityValidity::Lost,true));
    CHECK(!PickerForwardSwitchInvocationAllowed(
        PickerIdentityValidity::Indeterminate,true));
    CHECK(!PickerForwardSwitchInvocationAllowed(
        PickerIdentityValidity::Match,false));

    PickerState state=PickerTransitionFixture(94015);
    state.transition.phase=PickerPhase::SwitchIssue;
    state.transition.targetMayHaveMoved=true;
    state.transition.popupMayHaveMoved=true;
    state.transition.switchMayHaveChanged=true;
    state.transition.switchUnresolvedBeforeIssue=false;
    state.transition.pendingEffect=PickerEffectKind::SwitchDesktop;
    state.transition.effectSerial=6;
    PickerEffect switchEffect;
    switchEffect.kind=PickerEffectKind::SwitchDesktop;
    switchEffect.generation=state.transition.generation;
    switchEffect.effectSerial=6;
    PickerObservation lost=PickerObservationFor(
        switchEffect,PickerEvent::ApiCompleted);
    lost.identity=PickerIdentityValidity::Lost;
    lost.apiInvoked=false;
    const PickerEffect rollback=AdvancePickerTransition(state,lost);
    CHECK(rollback.kind==PickerEffectKind::MovePopup);
    CHECK(state.transition.targetIdentityUnusable);
    CHECK(!state.transition.switchMayHaveChanged);
    CHECK(!state.transition.commitCutoffReached);
    CHECK(state.transition.phase==PickerPhase::RollbackPopupIssue);
}

static void test_popup_post_classification_reuses_pending_id_after_initial_untracked_capture(){
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009405}";
    const std::string generatedId=
        "{00000000-0000-0000-0000-000000009406}";
    std::string selected="sentinel";
    int pendingCalls=0,generatorCalls=0;

    CHECK(PickerTerminalReservationAppAllowed("","firefox"));
    CHECK(PickerTerminalReservationAppAllowed("firefox","firefox"));
    CHECK(!PickerTerminalReservationAppAllowed("chrome","firefox"));
    CHECK(!PickerTerminalReservationAppAllowed("", ""));

    // Capture-time class lookup failed, so the generic picker reservation has
    // no record ID.  Terminal identity recapture and classification succeeded:
    // the exact runtime's saved pending ID must win before allocation.
    CHECK(SelectPopupPersistRecordId(
        "",
        [&](std::string& output){
            ++pendingCalls;
            output=savedId;
            return true;
        },
        [&](std::string& output){
            ++generatorCalls;
            output=generatedId;
            return true;
        },selected));
    CHECK(selected==savedId && pendingCalls==1 && generatorCalls==0);

    // A reused process-start cannot select the old runtime's pending row; the
    // terminal path instead creates one new ID and never steals savedId.
    selected="sentinel";
    CHECK(SelectPopupPersistRecordId(
        "",
        [&](std::string&){ ++pendingCalls; return false; },
        [&](std::string& output){
            ++generatorCalls;
            output=generatedId;
            return true;
        },selected));
    CHECK(selected==generatedId && pendingCalls==2 && generatorCalls==1);
}

static void test_validated_touch_rebase_preserves_external_semantics(){
    LayoutWin disk=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009301}","firefox","external","a.test",8,
        G(L"{231A0000-0000-0000-0000-000000000008}"),100);
    disk.missingSinceUtc=90;
    ValidatedRecordTouch touch;
    touch.recordId=disk.recordId;
    touch.lastSeenUtc=300;
    touch.causalGeneration=7;
    std::map<std::string,uint64_t> current={{disk.recordId,7}};
    TouchRebaseResult result=ReapplyValidatedTouches({disk},{{disk.recordId,touch}},current);
    CHECK(result.deferredRecordIds.empty() && result.records.size()==1);
    CHECK(result.records[0].activeTitle=="external");
    CHECK(result.records[0].deskIndex==8 &&
          GuidEq(result.records[0].desktop,disk.desktop));
    CHECK(result.records[0].lastSeenUtc==300 &&
          result.records[0].missingSinceUtc==0);

    current[disk.recordId]=8;
    TouchRebaseResult stale=ReapplyValidatedTouches({disk},{{disk.recordId,touch}},current);
    CHECK(stale.records.size()==1 && SameLayoutWinFields(stale.records[0],disk));
    CHECK(stale.deferredRecordIds.count(disk.recordId)==1);
}

int main(){
    test_picker_uses_self_contained_gdi_buffer();
    test_picker_icon_loading_is_bounded_and_outside_paint();
    test_picker_preloads_only_laid_out_visible_rows();
    test_picker_wm_paint_requires_the_owned_buffer();
    test_cli_list_uses_one_atomic_desktop_snapshot();
    test_ui_resources_have_one_owned_cleanup_path();
    test_message_pump_failure_uses_shared_teardown();
    test_footer_literal_and_links_are_exact();
    test_footer_minimum_size_is_one_line_and_dpi_scaled();
    test_footer_geometry_hit_hover_cursor_and_open_result_seams();
    test_footer_cache_and_activation_are_transactional();
    test_composite_picker_cache_cannot_omit_footer_state();
    test_footer_first_route_consumes_before_search_and_tiles();
    test_footer_hover_event_state_covers_every_reset_path();
    test_footer_mousemove_resets_tooltip_only_once_per_transition();
    test_visible_branding_and_help_retention_are_exact();
    test_picker_search_retry_uses_a_distinct_timer_channel();
    test_picker_close_route_is_used_by_the_window_procedure();
    test_picker_persistence_transaction_is_used_by_save();
    test_picker_distinguishes_current_selected_and_active();
    test_picker_zero_guids_and_partial_identities_never_highlight();
    test_picker_active_identity_rejects_hwnd_reuse();
    test_picker_selection_resolution_preserves_then_falls_back();
    test_picker_selection_updates_pair_without_touching_current();
    test_picker_refresh_preserves_search_scroll_and_identity();
    test_picker_refresh_transaction_publishes_only_complete_stage();
    test_blend_color_respects_channels_and_alpha_endpoints();
    test_picker_dim_search_keeps_current_and_selection_distinct();
    test_picker_visible_scroll_clamps_without_mutating_saved_value();
    test_picker_wheel_scroll_saturates_at_integer_bounds();
    test_picker_target_failed_recapture_clears_entire_capture();
    test_picker_row_requires_complete_stable_identity();
    test_picker_failed_current_read_clears_only_current();
    test_picker_desktop_snapshot_rejects_zero_and_duplicates();
    test_picker_filter_cache_is_transactional_and_precomputed();
    test_picker_bitmap_replacement_deselects_before_delete();
    test_gdi_buffer_resize_does_not_leak_selected_bitmaps();
    test_gdi_buffer_is_noncopyable();
    test_gdi_buffer_fake_ownership_is_exact();
    test_gdi_buffer_failures_are_atomic_and_cleanup_exact();
    test_gdi_buffer_failed_deselect_releases_dc_before_bitmap();
    test_gdi_buffer_rejects_unexpected_previous_selection_atomically();
    test_gdi_buffer_retains_selected_bitmap_until_reset_can_retry();
    test_gdi_buffer_invalid_sizes_never_allocate();
    test_gdi_buffer_catastrophic_rollback_retains_both_bitmaps();
    test_gdi_buffer_retains_dc_when_original_read_cleanup_fails();
    test_gdi_buffer_retains_every_failed_bitmap_delete_for_reset();
    test_icon_cache_replacement_prune_clear_and_destructor_are_exact();
    test_icon_cache_enforces_touch_lru_immediately();
    test_icon_cache_rejects_zero_limit_and_missing_ops();
    test_icon_cache_callback_exceptions_do_not_escape();
    test_icon_cache_destroy_failure_retains_bounded_ownership();
    test_icon_cache_caps_257_live_keys_at_256_immediately();
    test_icon_preload_gate_caps_work_and_skips_idle_refresh();
    test_picker_scroll_clamp_reaches_rows_beyond_icon_budget();
    test_ordered_teardown_retries_without_destroying_dependencies();
    test_fixed_icon_retirement_retains_failed_release_without_allocating();
    test_picker_valid_current_commits_only_with_model();
    test_picker_paint_cache_failure_blocks_show_transactionally();
    test_picker_com_output_is_owned_even_on_failed_call();
    test_picker_cache_publication_resets_hover_and_invalidates_failures();
    test_picker_commit_requires_exact_active_identity();
    test_picker_model_publish_invalidates_cache_before_reentry();
    test_picker_volatile_rows_skip_but_structural_failures_abort();
    test_picker_async_search_joins_by_full_identity();
    test_picker_state_whole_object_swap_includes_generation_sentinel();
    test_picker_transition_success_has_exact_verified_effect_order();
    test_picker_transition_rejects_stale_duplicate_and_wrong_effect_acks();
    test_picker_identity_loss_rolls_back_without_switch_or_save();
    test_picker_escape_hides_once_rolls_back_and_never_refocuses();
    test_picker_rollback_exhaustion_preserves_actual_readbacks();
    test_picker_ui_action_gate_and_auto_supersession_are_exact();
    test_picker_close_rejects_controlled_transition_without_cancel();
    test_picker_observation_kick_survives_post_and_timer_failure_once();
    test_picker_save_result_and_fresh_generation_callbacks_are_typed();
    test_ctrl_move_non_browser_never_mutates_auto_layout();
    test_ctrl_move_tracked_browser_unwritable_reports_failed();
    test_picker_persistence_app_staging_contains_allocation_failure();
    test_picker_api_ack_distinguishes_invocation_and_identity_quality();
    test_picker_post_switch_reads_current_then_popup_and_requires_both();
    test_picker_escape_after_save_emission_is_a_commit_cutoff();
    test_picker_cancel_discards_only_matching_unissued_non_save_effect();
    test_picker_reservation_filter_and_owner_replacement_are_exact();
    test_picker_raw_edit_and_tab_cache_are_model_generation_scoped();
    test_picker_tab_retry_delivery_is_failure_atomic();
    test_picker_nonidle_gate_includes_all_tray_mutators();
    test_picker_shutdown_driver_preserves_delays_and_skips_ui_work();
    test_picker_forward_attempt_matrix_has_independent_exact_budgets();
    test_picker_unavailable_target_read_exhausts_then_uses_rollback_budget();
    test_picker_wrong_event_and_serial_never_consume_pending_lane();
    test_picker_cancel_partial_matrix_stops_forward_and_hides_once();
    test_picker_focus_budget_terminates_without_report_loop();
    test_picker_begin_gate_rejects_incomplete_or_busy_state_atomically();
    test_picker_ui_preservation_prunes_scroll_and_adopts_only_safe_idle_target();
    test_picker_lightweight_refresh_is_single_snapshot_cache_only();
    test_picker_lightweight_snapshot_clears_stale_truth_exactly();
    test_picker_later_noninvocation_preserves_prior_unresolved_move();
    test_picker_popup_recovery_after_fourth_switch_saves_without_fifth();
    test_picker_popup_repair_rechecks_current_before_save();
    test_picker_cancel_during_exhausted_rollback_cannot_strand();
    test_picker_failed_current_rollback_suppresses_invisible_focus();
    test_picker_effect_serial_exhaustion_becomes_terminal_not_stranded();
    test_picker_unknown_identity_never_allows_future_target_api();
    test_picker_cancel_terminal_effects_never_reemit_or_refocus();
    test_picker_invoked_identity_loss_keeps_unknown_displacement();
    test_picker_noninvoked_rollback_apis_still_require_readback();
    test_picker_cancel_all_displaced_rolls_back_in_exact_order();
    test_picker_accepted_partial_rollback_reads_all_actual_components();
    test_picker_forward_api_ack_records_rollback_verification_need();
    test_picker_identity_loss_matrix_never_touches_target_again();
    test_picker_popup_and_switch_retry_boundaries_are_exact();
    test_picker_fourth_rollback_readback_and_focus_success_terminate();
    test_finalization_runs_once();
    test_window_identity_requires_full_nonzero_process_identity();
    test_snapshot_versions_change_only_for_changed_inputs();
    test_snapshot_signatures_are_delimiter_safe();
    test_snapshot_generation_wrap_restarts_without_zero();
    test_save_observed_bound_app_updates_only_exact_bound_identities();
    test_explicit_save_with_unbound_sibling_rearms_reconcile();
    test_fast_snapshot_versions_are_order_independent_and_quality_aware();
    test_resolve_saved_desktop_uses_guid_only();
    test_unknown_desktop_guid_is_not_index_zero();
    test_auto_restore_destination_uses_preserved_prepare_desktops();
    test_rebase_merges_different_ids_and_preserves_external_records();
    test_rebase_same_id_newer_validated_upsert_wins();
    test_durable_candidate_delta_is_satisfied_by_external_unrelated_revision();
    test_rebase_tied_or_older_upsert_and_stale_tombstone_defer();
    test_rebase_expiry_delete_requires_latest_independently_expired();
    test_record_delta_chaining_preserves_first_disk_base();
    test_deferred_conflict_survives_repeated_publish_until_newer_causal_upsert();
    test_rebased_publication_captures_adopted_disk_before_any_swap();
    test_durable_publish_commits_revision_without_copy_or_rewrite();
    test_final_checkpoint_mutation_is_transactional_across_fault_matrix();
    test_expire_delete_discards_validated_touch_before_external_rebase();
    test_final_observation_provisional_map_stages_before_global_publish();
    test_reconcile_worker_is_bounded_coalesced_and_nonblocking();
    test_reconcile_live_preparation_is_ordered_and_search_ready();
    test_picker_accepts_fresh_cache_only_for_exact_session_rows();
    test_unusable_fresh_rows_cannot_corrupt_or_create_records();
    test_picker_title_only_provisionals_share_reconcile_normalization();
    test_cli_profile_batch_aborts_transactionally_on_first_prep_failure();
    test_cli_loads_settings_before_selecting_active_profiles();
    test_cli_save_revalidates_snapshot_and_desktops_before_publish();
    test_manual_save_incomplete_snapshot_keeps_prior_bytes_without_write();
    test_cli_status_keeps_fast_rows_when_fingerprints_unavailable();
    test_reconcile_copied_text_budget_covers_every_owned_string();
    test_reconcile_worker_prepares_live_inputs_off_thread_and_coalesces();
    test_reconcile_worker_prepare_only_skips_planner_for_search();
    test_reconcile_worker_planner_failure_is_owned_and_thread_survives();
    test_reconcile_worker_accepted_request_owns_failure_result();
    test_reconcile_post_failure_deadlines_all_operation_owners();
    test_reconcile_worker_rejects_invalid_or_oversized_requests();
    test_reconcile_plan_distinguishes_complexity_deferral();
    test_posted_reconcile_results_are_drained();
    test_reconcile_consumer_ignores_stale_content_generation();
    test_final_snapshot_captures_immediately_opened_new_window();
    test_final_snapshot_marks_unbound_additions_provisional_independent_of_title();
    test_final_snapshot_failed_reappeared_keeps_destination_and_adds_sibling();
    test_final_snapshot_zero_live_marks_and_prunes_from_last_seen();
    test_final_snapshot_incomplete_profile_is_byte_preserved();
    test_final_snapshot_failed_desktop_lookup_preserves_saved_guid();
    test_final_snapshot_stale_pending_uses_unique_title_and_preserves_destination();
    test_final_snapshot_reservations_preserve_bound_and_provisional_origin();
    test_initial_partial_enumeration_suppresses_lifecycle_missing_and_write();
    test_bound_a_save_then_unbound_b_reconcile_never_moves_a();
    test_query_end_destroy_full_final_snapshot_chains();
    test_issued_move_heartbeat_then_session_end_preserves_origin_and_sibling();
    test_manual_restore_keeps_fixture_bytes_and_reports_once();
    test_auto_load_retry_uses_capped_backoff_and_initializes_once();
    test_corrective_initial_observation_is_transactional_before_async_work();
    test_corrective_fresh_bound_refresh_promotes_exact_provisional();
    test_corrective_startup_complete_empty_marks_missing_transactionally();
    test_corrective_monitor_arm_failure_backs_off_before_loading();
    test_corrective_monitor_alternate_rearm_is_bounded();
    test_corrective_failed_monitor_retry_post_remains_unready();
    test_corrective_unavailable_load_then_heartbeat_retry_initializes_once();
    test_corrective_move_cancellation_retry_progresses_beyond_eight_jobs();
    test_corrective_monitor_retry_deadline_survives_post_cap();
    test_corrective_stable_monitor_rearms_dirty_flush_after_timer_failure();
    test_checkpoint_controller_heartbeat_and_session_end_chain();
    test_checkpoint_failed_end_is_retryable_at_destroy();
    test_tray_exit_requires_successful_finalize_before_destroy();
    test_corrective_successful_session_end_quiesces_late_work();
    test_corrective_message_routes_are_no_throw_and_retire_exactly_once();
    test_settings_checkpoint_rejects_enabled_unloaded_and_preserves_state();
    test_settings_transaction_rolls_back_and_cancels_only_auto_owner();
    test_checkpoint_reservation_defers_one_heartbeat_but_not_final();
    test_tray_instance_scope_is_gui_only_and_covers_work_lifetime();
    test_browser_classifier_requires_enabled_class_and_exact_executable_basename();
    test_class_lookup_failure_marks_enabled_profiles_incomplete_but_empty_title_is_valid();
    test_popup_persistence_recaptures_before_classification_and_reports_storage();
    test_popup_saved_only_completes_exact_lifecycle_save_generation();
    test_popup_uses_exact_pending_saved_id_before_new_provisional();
    test_popup_pending_id_bypasses_title_and_origin_provisional_gates();
    test_picker_inflight_accepted_match_reuses_one_exact_saved_id();
    test_picker_save_accepts_only_exact_same_generation_late_fresh();
    test_picker_controlled_edit_allows_readback_but_not_mutation();
    test_picker_inflight_plan_gate_and_late_handoff_cutoff_are_exact();
    test_picker_failure_refresh_selection_uses_actual_readback();
    test_picker_post_save_identity_diagnostics_preserve_save_truth();
    test_picker_switch_effect_revalidates_target_at_invocation_boundary();
    test_popup_post_classification_reuses_pending_id_after_initial_untracked_capture();
    test_validated_touch_rebase_preserves_external_semantics();
    test_etld1();
    test_b64();
    test_b64_long_roundtrip();
    test_strict_integer_parsing();
    test_strict_base64_parsing();
    test_strict_counts_parsing();
    test_move_queue_alternates_issue_verify_and_succeeds();
    test_move_queue_enqueue_validates_identity_state_and_copies_guid();
    test_move_queue_allows_bounded_auto_with_manual_and_picker_jobs();
    test_move_queue_phase_guards_and_issue_outcomes();
    test_move_queue_four_transient_issues_still_receive_four_verifies();
    test_move_queue_invalid_outcomes_fail_closed();
    test_move_queue_four_transient_cycles_exhaust_and_unblock_next();
    test_move_queue_permanent_failure_finishes_and_unblocks_next();
    test_move_queue_cancel_job_is_identity_safe_during_verify();
    test_move_queue_cancel_operation_is_owner_scoped_and_fifo();
    test_move_queue_duplicate_owner_delivery_is_harmless();
    test_move_operation_dispatcher_is_job_and_owner_scoped();
    test_move_operation_dispatcher_cancellation_completes_each_job_once();
    test_move_reservation_replacement_requires_exact_terminal_token();
    test_issued_reservation_transfer_has_no_checkpoint_gap();
    test_successor_handoff_publishes_before_issued_displaced_cancel();
    test_issued_reservation_rollback_waits_for_terminal_ack();
    test_async_session_route_protects_manual_work_and_retires_once();
    test_async_session_route_timeout_and_cancel_are_exact();
    test_async_session_route_bounds_deadlines_and_retires_capacity();
    test_dirty_flush_is_coalesced_bounded_and_retries_without_spin();
    test_move_timer_failure_cancels_accepted_work_once();
    test_move_cancellation_gate_precedes_fallible_cleanup();
    test_move_terminal_state_is_prepared_before_publication();
    test_move_setup_rolls_back_provisional_and_queue_state();
    test_unbound_manual_reservation_uses_provisional_origin_id();
    test_auto_restore_failure_never_completes_as_success();
    test_identity_guard_recaptures_immediately_before_issue_or_verify();
    test_fast_window_publication_requires_exact_final_identity();
    test_fast_window_identity_failure_invalidates_all_enabled_profiles();
    test_desktop_services_require_documented_manager();
    test_failed_com_out_pointer_is_released();
    test_desktop_lookup_releases_failed_getdesktops_output();
    test_desktop_lookup_rejects_failed_or_oversized_count();
    test_desktop_lookup_releases_failed_getat_output();
    test_desktop_lookup_rejects_failed_or_zero_getid();
    test_desktop_lookup_returns_only_valid_owned_matches();
    test_desktop_snapshot_rechecks_count_after_prior_success();
    test_desktop_snapshot_fails_atomically_on_collection_errors();
    test_service_initialization_releases_every_failed_partial_state();
    test_reconcile_deadline_retires_dropped_operation_exactly_once();
    test_dirty_flush_preserves_mutation_during_write_and_limits_errors();
    test_dirty_flush_clock_ceiling_never_spins();
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
    test_manual_operation_profiles_remain_captured_across_settings_changes();
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
    test_layout_provisional_marker_roundtrips_strict_v4();
    test_layout_noncanonical_record_id_is_published_canonically();
    test_v4_provisional_extension_preserves_base_window_row();
    test_layout_provisional_marker_is_strict_and_transactional();
    test_layout_legacy_migration_never_invents_provisional_marker();
    test_layout_provisional_companions_do_not_consume_record_cap();
    test_layout_parse_v2();
    test_layout_rejects_invalid_base64();
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
    test_startup_expiry_partitions_every_app_transactionally();
    test_reconcile_restores_saved_a_and_creates_new_b();
    test_fresh_reconcile_adopts_one_persisted_provisional_with_same_id();
    test_fresh_reconcile_clears_multiple_matched_provisionals();
    test_fresh_reconcile_defers_ambiguous_provisional_adoption();
    test_expired_reappearance_is_new_not_restored();
    test_cached_stale_edge_preserves_match_and_defers_unmatched();
    test_failed_chrome_restore_retains_saved_destination_and_marks_seen();
    test_empty_chrome_reconcile_marks_only_chrome_missing();
    test_disabled_app_is_not_marked_newly_missing();
    test_missing_bookkeeping_is_copy_transactional_and_prunes_globally();
    test_title_only_provisional_cannot_steal_established_restore();
    test_distinct_title_only_provisionals_adopt_rich_live_rows();
    test_disjoint_residual_titles_do_not_block_safe_adoption();
    test_duplicate_title_only_provisionals_remain_deferred();
    test_title_only_provisional_cannot_steal_title_only_live();
    test_auto_cli_restore_uses_reconcile_semantics();
    test_final_provisional_binding_yields_to_unresolved_established_record();
    test_reserved_chrome_record_cannot_be_stolen_by_duplicate();
    test_same_desktop_match_learns_live_index_without_restore();
    test_late_window_after_first_wave_restores_before_save();
    test_edge_retention_is_independent_while_firefox_stays_open();
    test_firefox_sibling_reappears_while_first_window_stays_open();
    test_reconcile_plan_and_commit_preserve_input_vectors();
    test_reconcile_empty_generator_defers_transactionally();
    test_reconcile_invalid_generators_defer_transactionally();
    test_reconcile_generator_collision_with_any_existing_record_defers();
    test_reconcile_duplicate_generated_ids_defer_transactionally();
    test_reconcile_unique_generated_id_commits_strict_v4();
    test_reconcile_null_generator_defers_transactionally();
    test_reconcile_match_preflight_too_complex_defers_cleanly();
    test_reconcile_window_caps_defer_before_generation();
    test_reconcile_malformed_reserved_id_defers_before_work();
    test_reconcile_reserved_id_cap_is_fail_closed_at_boundary();
    test_reconcile_guaranteed_capacity_defers_before_matcher();
    test_reconcile_projects_mark_missing_expiration_before_capacity();
    test_projected_retained_count_rejects_mismatched_flags();
    test_reconcile_duplicate_injected_match_ownership_defers_cleanly();
    test_reconcile_rejects_all_malformed_injected_matches();
    test_reconcile_unsupported_app_defers_without_generation();
    test_commit_reconcile_rejects_out_of_range_mixed_plan_atomically();
    test_commit_reconcile_rejects_malformed_restore_sets_atomically();
    test_commit_reconcile_rejects_duplicate_match_ownership();
    test_commit_reconcile_rejects_app_mismatches();
    test_commit_reconcile_rejects_invalid_new_record_requests();
    test_commit_reconcile_rejects_cached_stale_actions();
    test_commit_reconcile_rejects_planning_clock_mismatch();
    test_reconcile_rejects_nonpositive_planning_clocks();
    test_reconcile_rejects_invalid_freshness_before_planning();
    test_commit_reconcile_rejects_projected_output_overflow();
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
    test_checked_snapshot_rejects_empty_id_and_zero_last_seen();
    test_checked_snapshot_rejects_zero_record_id_transactionally();
    test_checked_snapshot_rejects_duplicate_record_ids_transactionally();
    test_checked_snapshot_rejects_negative_missing_since_transactionally();
    test_checked_snapshot_accepts_supported_browser_apps();
    test_checked_snapshot_rejects_unsupported_app_transactionally();
    test_checked_snapshot_rejects_negative_tab_count_transactionally();
    test_checked_snapshot_rejects_invalid_counts_transactionally();
    test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally();
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
    test_lc_deferred_backoff_distinguishes_exact_max_from_overflow();
    test_lc_deferred_rollback_to_unrepresentable_deadline_fails_closed();
    test_lc_deferred_budget_resets_on_new_source_stamp();
    test_lc_cancelled_stale_reconcile_retires_exact_flight_and_rearms();
    test_lc_all_completion_outcomes_honor_one_queued_rearm();
    test_lc_exhausted_records_actual_layout_without_save_loop();
    test_lc_explicit_save_completion_is_generation_safe();
    test_lc_explicit_save_completion_commits_captured_layout_only();
    test_lc_explicit_save_completion_rebases_pending_wave_on_rollback();
    test_restore_budgets_isolate_siblings_runtime_and_destination();
    test_restore_budgets_prune_only_dead_runtime_identities();
    test_restore_budgets_cap_uses_deterministic_touch_lru();
    test_restore_budgets_new_key_copy_failure_is_transactional();
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
    test_durable_write_captures_revision_without_post_publish_read();
    test_durable_publish_exception_adopts_revision_then_retries();
    test_first_post_publish_verify_throw_recovers_from_armed_candidate();
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
    test_legacy_migration_failure_preserves_source_and_publishes_nothing();
    test_legacy_migration_parses_before_publishing();
    test_legacy_migration_never_overlays_recoverable_target();
    test_legacy_migration_installs_checked_v4_then_retires_source();
    test_same_revision_compares_every_field();
    test_missing_primary_corrupt_recovery_revision_allows_empty_publish();
    test_recovered_conflict_preserves_valid_backup_before_publish();
    test_two_actor_stale_save_is_rejected_without_overwrite();
    test_two_actor_recovered_source_stale_save_is_rejected();
    test_layout_mutex_zero_timeout_and_acquisition_after_release();
    test_layout_mutex_treats_abandoned_as_acquired();
    test_layout_fixture_removes_only_its_unique_tree();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
