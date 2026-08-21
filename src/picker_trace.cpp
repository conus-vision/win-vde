#include "picker_trace.hpp"

static bool PickerTraceIsCliCommand(const std::wstring& command) noexcept {
    return command==L"save" || command==L"restore" ||
        command==L"restore-auto" || command==L"status" ||
        command==L"list" || command==L"-h" ||
        command==L"--help" || command==L"/?";
}

VdeLaunchOptions ParseVdeLaunchOptions(
        int argc,const wchar_t* const* argv) noexcept {
    VdeLaunchOptions result;
    try {
        if(argc>=2 && argv && argv[1]) result.command=argv[1];
        result.cli=PickerTraceIsCliCommand(result.command);
        result.tracePicker=argc==2 && argv && argv[1] &&
            result.command==L"--trace-picker";
    } catch(...) {
        result=VdeLaunchOptions{};
    }
    return result;
}
