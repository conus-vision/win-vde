// appprofile.hpp — describes a trackable multi-window app (window class + exe +
// title suffixes + where its session data lives). Firefox uses its sessionstore;
// Chrome/Edge use SNSS (session.hpp). Windows with no session match fall back to
// title-only fingerprints.
#pragma once
#include "str_util.hpp"

struct AppProfile {
    std::string id;                          // "firefox" | "chrome" | "msedge"
    std::vector<std::wstring> classNames;    // acceptable window classes
    std::wstring exeName;                    // process image basename
    std::vector<std::wstring> titleSuffixes; // stripped from window title to get the active-tab title
    enum Sess { NONE, FIREFOX, CHROMIUM } session;
    std::wstring userDataDir;                // for CHROMIUM (SNSS lives under <userDataDir>\Default\Sessions)
};

inline std::wstring LocalAppData(){ wchar_t b[MAX_PATH]={0}; GetEnvironmentVariableW(L"LOCALAPPDATA",b,MAX_PATH); return b; }

inline std::vector<AppProfile> BuiltinProfiles(bool ff, bool cr, bool ed){
    std::vector<AppProfile> v;
    if(ff) v.push_back({ "firefox", { L"MozillaWindowClass" }, L"firefox.exe",
        { L" \x2014 Mozilla Firefox (Private Browsing)", L" - Mozilla Firefox (Private Browsing)",
          L" \x2014 Mozilla Firefox", L" - Mozilla Firefox" },
        AppProfile::FIREFOX, L"" });
    if(cr) v.push_back({ "chrome", { L"Chrome_WidgetWin_1" }, L"chrome.exe",
        { L" - Google Chrome", L" \x2013 Google Chrome", L" \x2014 Google Chrome" },
        AppProfile::CHROMIUM, LocalAppData()+L"\\Google\\Chrome\\User Data" });
    if(ed) v.push_back({ "msedge", { L"Chrome_WidgetWin_1" }, L"msedge.exe",
        { L" - Microsoft Edge", L" \x2013 Microsoft Edge", L" \x2014 Microsoft Edge" },
        AppProfile::CHROMIUM, LocalAppData()+L"\\Microsoft\\Edge\\User Data" });
    return v;
}
