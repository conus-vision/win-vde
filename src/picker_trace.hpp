#pragma once

#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "picker_state.hpp"

struct VdeLaunchOptions {
    std::wstring command;
    bool cli=false;
    bool tracePicker=false;
};

VdeLaunchOptions ParseVdeLaunchOptions(
    int argc,const wchar_t* const* argv) noexcept;
