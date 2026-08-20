#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <windows.h>

struct IconCacheOps {
    // Callbacks must report ownership failure by null/false and must not throw
    // after creating or releasing an icon. Exceptions before a side effect are
    // caught and treated as failure so the prior published entry is preserved.
    std::function<HICON(HICON)> copy;
    std::function<bool(HICON)> destroy;
};

enum class IconPreloadStep { Cached,Miss,Exhausted };

struct IconPreloadTurn {
    size_t visited=0;
    size_t misses=0;
    bool complete=true;
};

class IconPreloadGate {
    bool dirty_=false;
    size_t cursor_=0;
public:
    void markDirty() noexcept {
        dirty_=true;
        cursor_=0;
    }
    void cancel() noexcept {
        dirty_=false;
        cursor_=0;
    }
    bool dirty() const noexcept { return dirty_; }

    template<class Next>
    IconPreloadTurn runTurn(size_t missLimit,Next&& next) noexcept {
        IconPreloadTurn turn;
        if(!dirty_) return turn;
        turn.complete=false;
        if(missLimit==0) return turn;
        try {
            while(turn.misses<missLimit){
                const IconPreloadStep step=next(cursor_);
                if(step==IconPreloadStep::Exhausted){
                    cancel();
                    turn.complete=true;
                    return turn;
                }
                ++cursor_;
                ++turn.visited;
                if(step==IconPreloadStep::Miss) ++turn.misses;
            }
        } catch(...) {}
        return turn;
    }
};

class OrderedTeardownGate {
    enum class Stage { Windows,Classes,Resources,Complete };
    Stage stage_=Stage::Windows;
public:
    void reset() noexcept { stage_=Stage::Windows; }
    bool complete() const noexcept { return stage_==Stage::Complete; }

    template<class DestroyWindows,class UnregisterClasses,
             class DestroyResources>
    bool run(DestroyWindows&& destroyWindows,
             UnregisterClasses&& unregisterClasses,
             DestroyResources&& destroyResources) noexcept {
        try {
            if(stage_==Stage::Windows){
                if(!destroyWindows()) return false;
                stage_=Stage::Classes;
            }
            if(stage_==Stage::Classes){
                if(!unregisterClasses()) return false;
                stage_=Stage::Resources;
            }
            if(stage_==Stage::Resources){
                if(!destroyResources()) return false;
                stage_=Stage::Complete;
            }
            return true;
        } catch(...) { return false; }
    }
};

template<size_t Capacity>
class FixedIconRetirement {
    std::array<HICON,Capacity> icons_{};
    size_t size_=0;
public:
    FixedIconRetirement()=default;
    FixedIconRetirement(const FixedIconRetirement&)=delete;
    FixedIconRetirement& operator=(const FixedIconRetirement&)=delete;

    bool retain(HICON icon) noexcept {
        if(!icon || size_>=Capacity) return false;
        icons_[size_++]=icon;
        return true;
    }

    template<class Destroy>
    bool clear(Destroy&& destroy) noexcept {
        size_t retained=0;
        for(size_t index=0;index<size_;++index){
            bool released=false;
            try { released=destroy(icons_[index]); }
            catch(...) { released=false; }
            if(!released) icons_[retained++]=icons_[index];
        }
        for(size_t index=retained;index<size_;++index)
            icons_[index]=nullptr;
        size_=retained;
        return size_==0;
    }

    size_t size() const noexcept { return size_; }
};

class OwnedIconCache {
    struct Entry {
        HICON owned=nullptr;
        uint64_t touched=0;
    };

    size_t limit_=0;
    IconCacheOps ops_;
    bool valid_=false;
    std::map<std::string,Entry> entries_;
    HICON pendingDestroy_=nullptr;

    bool destroyNoThrow(HICON icon) noexcept {
        if(!icon) return true;
        if(!ops_.destroy) return false;
        try { return ops_.destroy(icon); }
        catch(...) { return false; }
    }

    bool drainPending() noexcept {
        if(!pendingDestroy_) return true;
        if(!destroyNoThrow(pendingDestroy_)) return false;
        pendingDestroy_=nullptr;
        return true;
    }

    void destroyCopiedOrRetain(HICON copied) noexcept {
        if(!copied || destroyNoThrow(copied)) return;
        pendingDestroy_=copied;
    }

    typename std::map<std::string,Entry>::iterator oldestEntry() noexcept {
        auto oldest=entries_.begin();
        for(auto candidate=entries_.begin();candidate!=entries_.end();
            ++candidate){
            if(candidate->second.touched<oldest->second.touched)
                oldest=candidate;
        }
        return oldest;
    }

public:
    OwnedIconCache(size_t limit,IconCacheOps ops)
        :limit_(limit),ops_(std::move(ops)),
         valid_(limit_!=0 && static_cast<bool>(ops_.copy) &&
                static_cast<bool>(ops_.destroy)){}

    ~OwnedIconCache() noexcept { (void)clear(); }

    OwnedIconCache(const OwnedIconCache&)=delete;
    OwnedIconCache& operator=(const OwnedIconCache&)=delete;

    HICON getAndTouch(const std::string& key,uint64_t touch) noexcept {
        if(!valid_) return nullptr;
        try {
            auto found=entries_.find(key);
            if(found==entries_.end()) return nullptr;
            found->second.touched=touch;
            return found->second.owned;
        } catch(...) { return nullptr; }
    }

    HICON peek(const std::string& key) const noexcept {
        if(!valid_) return nullptr;
        try {
            auto found=entries_.find(key);
            return found==entries_.end()?nullptr:found->second.owned;
        } catch(...) { return nullptr; }
    }

    HICON insertBorrowed(const std::string& key,HICON borrowed,
                         uint64_t touch) noexcept {
        if(!valid_ || !borrowed) return nullptr;
        if(!drainPending()) return nullptr;
        HICON copied=nullptr;
        try { copied=ops_.copy(borrowed); }
        catch(...) { return nullptr; }
        if(!copied) return nullptr;

        try {
            auto found=entries_.find(key);
            if(found!=entries_.end()){
                HICON replaced=found->second.owned;
                if(!destroyNoThrow(replaced)){
                    destroyCopiedOrRetain(copied);
                    return nullptr;
                }
                found->second.owned=copied;
                found->second.touched=touch;
                return copied;
            }

            if(entries_.size()>=limit_){
                auto oldest=oldestEntry();
                const bool copiedIsOldest=
                    touch<oldest->second.touched ||
                    (touch==oldest->second.touched && key<oldest->first);
                if(copiedIsOldest){
                    destroyCopiedOrRetain(copied);
                    return nullptr;
                }

                const std::string oldestKey=oldest->first;
                const auto inserted=entries_.emplace(key,Entry{copied,touch});
                if(!inserted.second){
                    destroyCopiedOrRetain(copied);
                    return nullptr;
                }
                oldest=entries_.find(oldestKey);
                if(oldest==entries_.end() ||
                   !destroyNoThrow(oldest->second.owned)){
                    entries_.erase(inserted.first);
                    destroyCopiedOrRetain(copied);
                    return nullptr;
                }
                entries_.erase(oldest);
                return copied;
            }

            const auto inserted=entries_.emplace(key,Entry{copied,touch});
            if(!inserted.second){
                destroyCopiedOrRetain(copied);
                return nullptr;
            }
            return copied;
        } catch(...) {
            destroyCopiedOrRetain(copied);
            return nullptr;
        }
    }

    bool pruneTo(const std::set<std::string>& liveKeys) noexcept {
        if(!valid_) return entries_.empty() && !pendingDestroy_;
        if(!drainPending()) return false;
        try {
            auto entry=entries_.begin();
            while(entry!=entries_.end()){
                if(liveKeys.count(entry->first)!=0){ ++entry; continue; }
                if(!destroyNoThrow(entry->second.owned)) return false;
                entry=entries_.erase(entry);
            }
        } catch(...) { return false; }
        return true;
    }

    bool clear() noexcept {
        if(!drainPending()) return false;
        try {
            auto entry=entries_.begin();
            while(entry!=entries_.end()){
                if(!destroyNoThrow(entry->second.owned)) return false;
                entry=entries_.erase(entry);
            }
        } catch(...) { return false; }
        return true;
    }

    size_t size() const noexcept { return entries_.size(); }
};
