// tab_overlay.hpp - pure logic of the picker's tab list overlay (Ctrl+click a
// window row): scrolling, hit testing and keyboard selection. No Win32 calls.
#pragma once

#include <cstddef>

// First row that may be scrolled to so that the last page stays full.
inline int TabOverlayMaxScroll(size_t rowCount,int visibleRows) noexcept {
    if(visibleRows<=0 || rowCount<=static_cast<size_t>(visibleRows)) return 0;
    const size_t maximum=rowCount-static_cast<size_t>(visibleRows);
    return maximum>0x7fffffff ? 0x7fffffff : static_cast<int>(maximum);
}

inline int TabOverlayClampScroll(long long scroll,size_t rowCount,
                                 int visibleRows) noexcept {
    const int maximum=TabOverlayMaxScroll(rowCount,visibleRows);
    if(scroll<0) return 0;
    return scroll>maximum ? maximum : static_cast<int>(scroll);
}

// Scrolls as little as possible so that `row` is on screen.
inline int TabOverlayScrollToShow(int scroll,int row,size_t rowCount,
                                  int visibleRows) noexcept {
    if(row<0 || visibleRows<=0)
        return TabOverlayClampScroll(scroll,rowCount,visibleRows);
    long long next=scroll;
    if(row<scroll) next=row;
    else if(row>=scroll+visibleRows) next=static_cast<long long>(row)-visibleRows+1;
    return TabOverlayClampScroll(next,rowCount,visibleRows);
}

// Row under a y coordinate inside the list, or -1.
inline int TabOverlayHitRow(int y,int listTop,int rowHeight,int scroll,
                            size_t rowCount,int visibleRows) noexcept {
    if(rowHeight<=0 || y<listTop) return -1;
    const int slot=(y-listTop)/rowHeight;
    if(slot>=visibleRows) return -1;
    const long long row=static_cast<long long>(scroll)+slot;
    if(row<0 || row>=static_cast<long long>(rowCount)) return -1;
    return static_cast<int>(row);
}

enum class TabOverlayKey { Up, Down, PageUp, PageDown, Home, End };

// New selected row after a navigation key; -1 stays -1 only for an empty list.
inline int TabOverlayMoveSelection(int selected,TabOverlayKey key,
                                   size_t rowCount,int visibleRows) noexcept {
    if(rowCount==0) return -1;
    const long long last=static_cast<long long>(rowCount)-1;
    const long long page=visibleRows>1 ? visibleRows-1 : 1;
    long long next=selected<0 ? -1 : selected;
    switch(key){
    case TabOverlayKey::Up:       next=selected<0 ? last : next-1; break;
    case TabOverlayKey::Down:     next=next+1; break;
    case TabOverlayKey::PageUp:   next=selected<0 ? 0 : next-page; break;
    case TabOverlayKey::PageDown: next=selected<0 ? page : next+page; break;
    case TabOverlayKey::Home:     next=0; break;
    case TabOverlayKey::End:      next=last; break;
    }
    if(next<0) next=0;
    if(next>last) next=last;
    return static_cast<int>(next);
}
