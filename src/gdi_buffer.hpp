#pragma once

#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

struct GdiBufferOps {
    // Injected operations must report failure through their return value and
    // must not throw after changing GDI selection or ownership state.
    void* context=nullptr;
    HDC (*createCompatibleDc)(void*,HDC)=nullptr;
    HGDIOBJ (*getCurrentObject)(void*,HDC,int)=nullptr;
    HBITMAP (*createCompatibleBitmap)(void*,HDC,int,int)=nullptr;
    HGDIOBJ (*selectObject)(void*,HDC,HGDIOBJ)=nullptr;
    BOOL (*deleteObject)(void*,HGDIOBJ)=nullptr;
    BOOL (*deleteDc)(void*,HDC)=nullptr;

    bool valid() const noexcept {
        return createCompatibleDc && getCurrentObject &&
            createCompatibleBitmap && selectObject &&
            deleteObject && deleteDc;
    }
};

inline HDC GdiBufferCreateCompatibleDc(void*,HDC reference) noexcept {
    return CreateCompatibleDC(reference);
}

inline HGDIOBJ GdiBufferGetCurrentObject(void*,HDC dc,int kind) noexcept {
    return GetCurrentObject(dc,kind);
}

inline HBITMAP GdiBufferCreateCompatibleBitmap(
        void*,HDC reference,int width,int height) noexcept {
    return CreateCompatibleBitmap(reference,width,height);
}

inline HGDIOBJ GdiBufferSelectObject(
        void*,HDC dc,HGDIOBJ object) noexcept {
    return SelectObject(dc,object);
}

inline BOOL GdiBufferDeleteObject(void*,HGDIOBJ object) noexcept {
    return DeleteObject(object);
}

inline BOOL GdiBufferDeleteDc(void*,HDC dc) noexcept {
    return DeleteDC(dc);
}

inline GdiBufferOps DefaultGdiBufferOps() noexcept {
    GdiBufferOps ops;
    ops.createCompatibleDc=&GdiBufferCreateCompatibleDc;
    ops.getCurrentObject=&GdiBufferGetCurrentObject;
    ops.createCompatibleBitmap=&GdiBufferCreateCompatibleBitmap;
    ops.selectObject=&GdiBufferSelectObject;
    ops.deleteObject=&GdiBufferDeleteObject;
    ops.deleteDc=&GdiBufferDeleteDc;
    return ops;
}

class GdiBuffer {
    GdiBufferOps ops_=DefaultGdiBufferOps();
    HDC dc_=nullptr;
    HBITMAP bitmap_=nullptr;
    HBITMAP pendingBitmap_=nullptr;
    HBITMAP retiredBitmap_=nullptr;
    HBITMAP original_=nullptr;
    int width_=0;
    int height_=0;
    bool cleanupPending_=false;

    bool destroyBitmap(HBITMAP& bitmap) noexcept {
        if(!bitmap) return true;
        try {
            if(!ops_.deleteObject(ops_.context,bitmap)) return false;
        } catch(...) { return false; }
        bitmap=nullptr;
        return true;
    }

    bool deleteDc() noexcept {
        if(!dc_) return true;
        try { return ops_.deleteDc(ops_.context,dc_)!=FALSE; }
        catch(...) { return false; }
    }

public:
    GdiBuffer()=default;
    explicit GdiBuffer(GdiBufferOps ops) noexcept :ops_(ops){}
    GdiBuffer(const GdiBuffer&)=delete;
    GdiBuffer& operator=(const GdiBuffer&)=delete;

    ~GdiBuffer() noexcept { reset(); }

    bool ensure(HDC reference,int width,int height) noexcept {
        if(!reference || width<=0 || height<=0 || !ops_.valid()) return false;
        if(cleanupPending_ || pendingBitmap_ || (bitmap_ && !dc_) ||
           (dc_ && !original_)) return false;
        if(retiredBitmap_ && !destroyBitmap(retiredBitmap_))
            return bitmap_ && dc_ && width_==width && height_==height;
        bool createdDc=false;
        if(!dc_){
            HDC created=nullptr;
            try { created=ops_.createCompatibleDc(ops_.context,reference); }
            catch(...) { return false; }
            if(!created) return false;
            HBITMAP original=nullptr;
            try {
                original=static_cast<HBITMAP>(
                    ops_.getCurrentObject(ops_.context,created,OBJ_BITMAP));
            } catch(...) {}
            if(!original){
                dc_=created;
                cleanupPending_=true;
                if(deleteDc()){
                    dc_=nullptr;
                    cleanupPending_=false;
                }
                return false;
            }
            dc_=created;
            original_=original;
            createdDc=true;
        }
        if(bitmap_ && width_==width && height_==height) return true;
        HBITMAP replacement=nullptr;
        try {
            replacement=ops_.createCompatibleBitmap(
                ops_.context,reference,width,height);
        } catch(...) {}
        if(!replacement){
            if(createdDc) reset();
            return false;
        }
        HGDIOBJ previous=HGDI_ERROR;
        try {
            previous=ops_.selectObject(ops_.context,dc_,replacement);
        } catch(...) {}
        if(!previous || previous==HGDI_ERROR){
            if(!destroyBitmap(replacement)){
                pendingBitmap_=replacement;
                cleanupPending_=true;
            }
            if(createdDc) reset();
            return false;
        }
        HGDIOBJ expected=bitmap_
            ?static_cast<HGDIOBJ>(bitmap_)
            :static_cast<HGDIOBJ>(original_);
        if(previous!=expected){
            HGDIOBJ rollback=HGDI_ERROR;
            try {
                rollback=ops_.selectObject(ops_.context,dc_,expected);
            } catch(...) {}
            if(!rollback || rollback==HGDI_ERROR || rollback!=replacement){
                pendingBitmap_=replacement;
                cleanupPending_=true;
                const bool dcDestroyed=deleteDc();
                if(!dcDestroyed) return false;
                dc_=nullptr;
                original_=nullptr;
                destroyBitmap(pendingBitmap_);
                if(destroyBitmap(bitmap_)){
                    width_=0;
                    height_=0;
                }
                destroyBitmap(retiredBitmap_);
                cleanupPending_=dc_ || bitmap_ || pendingBitmap_ ||
                    retiredBitmap_;
                return false;
            }
            if(!destroyBitmap(replacement)){
                pendingBitmap_=replacement;
                cleanupPending_=true;
            }
            if(createdDc) reset();
            return false;
        }
        HBITMAP old=bitmap_;
        bitmap_=replacement;
        width_=width;
        height_=height;
        if(old && !destroyBitmap(old)){
            retiredBitmap_=old;
            return true;
        }
        return true;
    }

    void reset() noexcept {
        cleanupPending_=true;
        const bool hasOwned=bitmap_ || pendingBitmap_;
        bool bitmapsKnownUnselected=!dc_ || !hasOwned;
        if(dc_ && original_ && hasOwned){
            HGDIOBJ previous=HGDI_ERROR;
            try {
                previous=ops_.selectObject(
                    ops_.context,dc_,original_);
            } catch(...) {}
            bitmapsKnownUnselected=previous && previous!=HGDI_ERROR;
        }
        bool dcDestroyed=!dc_;
        if(dc_) dcDestroyed=deleteDc();
        if(!dcDestroyed && !bitmapsKnownUnselected) return;

        if(dcDestroyed){
            dc_=nullptr;
            original_=nullptr;
        }
        destroyBitmap(pendingBitmap_);
        destroyBitmap(retiredBitmap_);
        if(destroyBitmap(bitmap_)){
            width_=0;
            height_=0;
        }
        cleanupPending_=dc_ || bitmap_ || pendingBitmap_ || retiredBitmap_;
    }

    HDC get() const noexcept { return dc_; }
    bool released() const noexcept {
        return !dc_ && !bitmap_ && !pendingBitmap_ && !retiredBitmap_;
    }
};
