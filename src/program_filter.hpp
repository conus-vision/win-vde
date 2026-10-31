// program_filter.hpp -- pure logic of the picker's program filter: which
// programs get a check box under the search box, what they are called, how the
// boxes wrap, and how the unchecked ones are stored.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

// A program is known by the lowercase base name of its executable, so every
// window of firefox.exe shares one check box whatever its install path.
inline std::wstring ProgramKeyFromImagePath(const std::wstring& path){
    const size_t slash=path.find_last_of(L"\\/");
    std::wstring key=slash==std::wstring::npos ? path : path.substr(slash+1);
    for(wchar_t& c : key) c=static_cast<wchar_t>(std::towlower(c));
    const std::wstring extension=L".exe";
    if(key.size()>extension.size() &&
       key.compare(key.size()-extension.size(),extension.size(),extension)==0)
        key.resize(key.size()-extension.size());
    return key;
}

// The label is only the program's name: Firefox, Opera, Word.  Executables
// whose file name says little get a table entry; the rest are capitalized.
inline std::wstring ProgramDisplayName(const std::wstring& key){
    struct KnownProgram { const wchar_t* key; const wchar_t* name; };
    static const KnownProgram known[]={
        {L"applicationframehost",L"Windows apps"},
        {L"acrobat",L"Acrobat"},{L"acrord32",L"Acrobat Reader"},
        {L"code",L"VS Code"},{L"code - insiders",L"VS Code Insiders"},
        {L"devenv",L"Visual Studio"},{L"excel",L"Excel"},
        {L"iexplore",L"Internet Explorer"},{L"ms-teams",L"Teams"},
        {L"msedge",L"Edge"},{L"msteams",L"Teams"},
        {L"notepad++",L"Notepad++"},{L"obs64",L"OBS"},{L"olk",L"Outlook"},
        {L"onenote",L"OneNote"},{L"powerpnt",L"PowerPoint"},
        {L"windowsterminal",L"Terminal"},{L"winword",L"Word"},
    };
    for(const KnownProgram& entry : known)
        if(key==entry.key) return entry.name;
    std::wstring name=key;
    const size_t digits=name.size()>2 ? name.size()-2 : 0;
    if(digits>0 && (name.compare(digits,2,L"64")==0 ||
                    name.compare(digits,2,L"32")==0))
        name.resize(digits);
    if(!name.empty()) name[0]=static_cast<wchar_t>(std::towupper(name[0]));
    return name;
}

struct PickerProgramChip {
    std::wstring key;
    std::wstring name;
    bool hidden=false;
    RECT rect={0,0,0,0};
};

// A check box appears once a program has more than one window.  A program the
// user unchecked keeps its box down to its last window: without the box that
// window could never be shown again.
inline std::vector<PickerProgramChip> BuildPickerProgramChips(
        const std::map<std::wstring,size_t>& windowsByProgram,
        const std::set<std::wstring>& hiddenPrograms){
    std::vector<PickerProgramChip> chips;
    for(const std::pair<const std::wstring,size_t>& program : windowsByProgram){
        if(program.first.empty() || program.second==0) continue;
        const bool hidden=hiddenPrograms.count(program.first)!=0;
        if(program.second<2 && !hidden) continue;
        PickerProgramChip chip;
        chip.key=program.first;
        chip.name=ProgramDisplayName(program.first);
        chip.hidden=hidden;
        chips.push_back(std::move(chip));
    }
    std::sort(chips.begin(),chips.end(),
        [](const PickerProgramChip& left,const PickerProgramChip& right){
            return left.name<right.name;
        });
    return chips;
}

// Lays the boxes out left to right and wraps them.  Returns the number of rows
// used.  With a row limit, a box that does not fit keeps an empty rect, which
// hit tests and painting skip.
inline int LayoutPickerProgramChips(
        std::vector<PickerProgramChip>& chips,const std::vector<int>& widths,
        int left,int top,int right,int chipHeight,int gapX,int gapY,
        int maxRows) noexcept {
    int rows=0;
    int x=left;
    bool full=maxRows<=0;
    for(size_t index=0;index<chips.size();++index){
        chips[index].rect=RECT{0,0,0,0};
        if(full || index>=widths.size()) continue;
        if(rows==0) rows=1;
        const int width=widths[index];
        if(x>left && x+width>right){
            if(rows>=maxRows){ full=true; continue; }
            ++rows;
            x=left;
        }
        const int y=top+(rows-1)*(chipHeight+gapY);
        chips[index].rect=RECT{x,y,x+width,y+chipHeight};
        x+=width+gapX;
    }
    return rows;
}

inline int PickerProgramChipAt(const std::vector<PickerProgramChip>& chips,
                               POINT point) noexcept {
    for(size_t index=0;index<chips.size();++index){
        const RECT& rect=chips[index].rect;
        if(rect.right>rect.left && point.x>=rect.left && point.x<rect.right &&
           point.y>=rect.top && point.y<rect.bottom)
            return static_cast<int>(index);
    }
    return -1;
}

// Stored as one "|"-separated string; a file name cannot contain "|".
inline std::wstring FormatHiddenPrograms(const std::set<std::wstring>& programs){
    std::wstring text;
    for(const std::wstring& program : programs){
        if(program.empty()) continue;
        if(!text.empty()) text+=L'|';
        text+=program;
    }
    return text;
}

inline std::set<std::wstring> ParseHiddenPrograms(const std::wstring& text){
    std::set<std::wstring> programs;
    size_t begin=0;
    while(begin<=text.size()){
        const size_t end=text.find(L'|',begin);
        const size_t stop=end==std::wstring::npos ? text.size() : end;
        if(stop>begin) programs.insert(text.substr(begin,stop-begin));
        if(end==std::wstring::npos) break;
        begin=end+1;
    }
    return programs;
}
