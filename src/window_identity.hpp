#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

struct WindowIdentityKey {
    uintptr_t hwnd = 0;
    DWORD pid = 0;
    uint64_t processStart = 0;
};

enum class WindowIdentityRecapture {
    Match,
    Lost,
    Indeterminate
};

inline bool FinalFastWindowIdentityCanPublish(
        WindowIdentityRecapture recapture) noexcept {
    return recapture==WindowIdentityRecapture::Match;
}

inline bool FinalFastWindowIdentityFailureInvalidatesEveryProfile(
        WindowIdentityRecapture recapture) noexcept {
    return recapture!=WindowIdentityRecapture::Match;
}

enum class PopupBrowserClassification {
    Tracked,
    NotTracked,
    Failed
};

enum class PopupPersistenceReadiness {
    Ready,
    Unavailable,
    ReadOnly
};

enum class PopupPersistenceResult {
    Saved,
    NotTracked,
    IdentityLost,
    IdentityIndeterminate,
    ClassificationFailed,
    StorageUnavailable,
    StorageReadOnly,
    SaveFailed
};

enum class PopupReservationRecordSource {
    None,
    Pending,
    Provisional
};

struct FastWin {
    std::string app;
    HWND hwnd = nullptr;
    DWORD pid = 0;
    uint64_t processStart = 0;
    GUID desktop = {0};
    std::wstring title;
};

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

inline WindowIdentityKey IdentityOf(const FastWin& window){
    WindowIdentityKey key;
    key.hwnd = reinterpret_cast<uintptr_t>(window.hwnd);
    key.pid = window.pid;
    key.processStart = window.processStart;
    return key;
}

inline bool SameIdentity(const WindowIdentityKey& left,
                         const WindowIdentityKey& right){
    return left.hwnd != 0 && left.pid != 0 && left.processStart != 0 &&
           left.hwnd == right.hwnd && left.pid == right.pid &&
           left.processStart == right.processStart;
}

template<class Recapture,class Classify,class Readiness,class Persist>
inline PopupPersistenceResult CompletePopupMovePersistence(
        const WindowIdentityKey& expected,Recapture&& recapture,
        Classify&& classify,Readiness&& readiness,Persist&& persist) noexcept {
    WindowIdentityRecapture identity=WindowIdentityRecapture::Indeterminate;
    try { identity=recapture(expected); }
    catch(...) { return PopupPersistenceResult::IdentityIndeterminate; }
    if(identity==WindowIdentityRecapture::Lost)
        return PopupPersistenceResult::IdentityLost;
    if(identity!=WindowIdentityRecapture::Match)
        return PopupPersistenceResult::IdentityIndeterminate;

    PopupBrowserClassification browser=PopupBrowserClassification::Failed;
    try { browser=classify(expected); }
    catch(...) { return PopupPersistenceResult::ClassificationFailed; }
    if(browser==PopupBrowserClassification::NotTracked)
        return PopupPersistenceResult::NotTracked;
    if(browser!=PopupBrowserClassification::Tracked)
        return PopupPersistenceResult::ClassificationFailed;

    PopupPersistenceReadiness storage=PopupPersistenceReadiness::Unavailable;
    try { storage=readiness(); }
    catch(...) { return PopupPersistenceResult::StorageUnavailable; }
    if(storage==PopupPersistenceReadiness::Unavailable)
        return PopupPersistenceResult::StorageUnavailable;
    if(storage==PopupPersistenceReadiness::ReadOnly)
        return PopupPersistenceResult::StorageReadOnly;
    if(storage!=PopupPersistenceReadiness::Ready)
        return PopupPersistenceResult::StorageUnavailable;

    try {
        return persist() ? PopupPersistenceResult::Saved
                         : PopupPersistenceResult::SaveFailed;
    } catch(...) { return PopupPersistenceResult::SaveFailed; }
}

template<class Complete>
inline bool CompletePopupLifecycleAfterPersistence(
        PopupPersistenceResult result,Complete&& complete) noexcept {
    if(result!=PopupPersistenceResult::Saved) return false;
    try {
        complete();
        return true;
    } catch(...) { return false; }
}

inline bool operator<(const WindowIdentityKey& left,
                      const WindowIdentityKey& right){
    if(left.hwnd != right.hwnd) return left.hwnd < right.hwnd;
    if(left.pid != right.pid) return left.pid < right.pid;
    return left.processStart < right.processStart;
}

inline std::string RuntimeKey(const WindowIdentityKey& key){
    return std::to_string(static_cast<unsigned long long>(key.hwnd)) + ":" +
           std::to_string(static_cast<unsigned long long>(key.pid)) + ":" +
           std::to_string(static_cast<unsigned long long>(key.processStart));
}

inline std::string RuntimeKey(const FastWin& window){
    return RuntimeKey(IdentityOf(window));
}

template<class Validate>
inline bool SelectPendingPopupRecordId(
        const WindowIdentityKey& identity,const std::string& app,
        const std::map<std::string,std::string>& pending,
        Validate&& validate,std::string& output) noexcept {
    if(identity.hwnd==0 || identity.pid==0 || identity.processStart==0 ||
       app.empty()) return false;
    try {
        const auto found=pending.find(RuntimeKey(identity));
        if(found==pending.end()) return false;
        std::string canonical;
        if(!validate(found->second,app,canonical) || canonical.empty())
            return false;
        output.swap(canonical);
        return true;
    } catch(...) { return false; }
}

template<class SelectPending,class SelectProvisional>
inline PopupReservationRecordSource SelectPopupReservationRecord(
        bool tracked,bool persistenceReady,bool titleComplete,
        bool originDesktopValid,SelectPending&& selectPending,
        SelectProvisional&& selectProvisional,
        std::string& output) noexcept {
    if(!tracked || !persistenceReady)
        return PopupReservationRecordSource::None;
    try {
        std::string selected;
        if(selectPending(selected) && !selected.empty()){
            output.swap(selected);
            return PopupReservationRecordSource::Pending;
        }
        if(!titleComplete || !originDesktopValid)
            return PopupReservationRecordSource::None;
        selected.clear();
        if(!selectProvisional(selected) || selected.empty())
            return PopupReservationRecordSource::None;
        output.swap(selected);
        return PopupReservationRecordSource::Provisional;
    } catch(...) { return PopupReservationRecordSource::None; }
}

template<class SelectPending,class Generate>
inline bool SelectPopupPersistRecordId(
        const std::string& reservedRecordId,SelectPending&& selectPending,
        Generate&& generate,std::string& output) noexcept {
    try {
        std::string selected;
        if(!reservedRecordId.empty()){
            selected=reservedRecordId;
        } else {
            if(!selectPending(selected) || selected.empty()){
                selected.clear();
                if(!generate(selected) || selected.empty()) return false;
            }
        }
        output.swap(selected);
        return true;
    } catch(...) { return false; }
}

inline std::wstring ExecutableBaseName(const std::wstring& image){
    const size_t separator=image.find_last_of(L"\\/");
    return separator==std::wstring::npos ? image : image.substr(separator+1);
}

inline bool ExactExecutableBaseName(const std::wstring& image,
                                    const std::wstring& expected){
    const std::wstring base=ExecutableBaseName(image);
    return base.size()==expected.size() &&
        CompareStringOrdinal(base.c_str(),static_cast<int>(base.size()),
                             expected.c_str(),static_cast<int>(expected.size()),
                             TRUE)==CSTR_EQUAL;
}

template<class Profile>
inline const Profile* ClassifyBrowserCandidate(
        const wchar_t* className,const std::wstring& image,
        const std::vector<Profile>& profiles){
    if(!className || image.empty()) return nullptr;
    for(const Profile& profile : profiles){
        const bool classMatches=std::find(
            profile.classNames.begin(),profile.classNames.end(),className)!=
            profile.classNames.end();
        if(classMatches && ExactExecutableBaseName(image,profile.exeName))
            return &profile;
    }
    return nullptr;
}

template<class Profile>
inline void MarkFastSnapshotCaptureIncomplete(
        const std::vector<Profile>& enabledProfiles,
        std::map<std::string,AppFastSnapshot>& snapshots) noexcept {
    for(const Profile& profile : enabledProfiles){
        const auto found=snapshots.find(profile.id);
        if(found!=snapshots.end()) found->second.enumerationComplete=false;
    }
}

template<class Profile>
inline bool AcceptFastClassNameRead(
        int copiedCharacters,const std::vector<Profile>& enabledProfiles,
        std::map<std::string,AppFastSnapshot>& snapshots) noexcept {
    if(copiedCharacters>0) return true;
    MarkFastSnapshotCaptureIncomplete(enabledProfiles,snapshots);
    return false;
}

class SnapshotSignatureBuilder {
public:
    SnapshotSignatureBuilder() : hash_(1469598103934665603ULL) {}

    SnapshotSignatureBuilder& addBytes(const void* value, size_t size){
        addUnsigned(static_cast<uint64_t>(size));
        const unsigned char* bytes = static_cast<const unsigned char*>(value);
        for(size_t index = 0; index < size; ++index){
            hash_ ^= bytes[index];
            hash_ *= 1099511628211ULL;
        }
        return *this;
    }

    SnapshotSignatureBuilder& addString(const std::string& value){
        return addBytes(value.data(), value.size());
    }

    SnapshotSignatureBuilder& addWideString(const std::wstring& value){
        return addBytes(value.data(), value.size() * sizeof(wchar_t));
    }

    SnapshotSignatureBuilder& addUnsigned(uint64_t value){
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(&value);
        for(size_t index = 0; index < sizeof(value); ++index){
            hash_ ^= bytes[index];
            hash_ *= 1099511628211ULL;
        }
        return *this;
    }

    uint64_t value() const { return hash_; }

private:
    uint64_t hash_;
};

struct SnapshotVersions {
    uint64_t identityGeneration = 0;
    uint64_t contentGeneration = 0;
};

class SnapshotVersionTracker {
public:
    explicit SnapshotVersionTracker(uint64_t nextGeneration = 1)
        : nextGeneration_(nextGeneration == 0 ? 1 : nextGeneration) {}

    SnapshotVersions observe(const std::string& app,
                             uint64_t identityQualityConfigSignature,
                             uint64_t fullContentSignature){
        if(nextGeneration_ == 0){
            entries_.clear();
            nextGeneration_ = 1;
        }

        Entry* entry = &entries_[app];
        bool identityChanged = !entry->initialized ||
            entry->identitySignature != identityQualityConfigSignature;
        bool contentChanged = !entry->initialized || identityChanged ||
            entry->contentSignature != fullContentSignature;
        unsigned allocations = (identityChanged ? 1U : 0U) +
                               (contentChanged ? 1U : 0U);
        if(allocations == 2U &&
           nextGeneration_ == (std::numeric_limits<uint64_t>::max)()){
            entries_.clear();
            nextGeneration_ = 1;
            entry = &entries_[app];
            identityChanged = true;
            contentChanged = true;
        }

        if(identityChanged)
            entry->versions.identityGeneration = takeGeneration();
        if(contentChanged)
            entry->versions.contentGeneration = takeGeneration();
        entry->identitySignature = identityQualityConfigSignature;
        entry->contentSignature = fullContentSignature;
        entry->initialized = true;
        return entry->versions;
    }

private:
    struct Entry {
        bool initialized = false;
        uint64_t identitySignature = 0;
        uint64_t contentSignature = 0;
        SnapshotVersions versions;
    };

    uint64_t takeGeneration(){
        uint64_t value = nextGeneration_;
        nextGeneration_ = value == (std::numeric_limits<uint64_t>::max)()
            ? 0 : value + 1;
        return value;
    }

    uint64_t nextGeneration_;
    std::map<std::string, Entry> entries_;
};

inline bool FastSnapshotCanObserve(const AppFastSnapshot& snapshot){
    return snapshot.enumerationComplete;
}

inline bool FastSnapshotCanPersistAll(const AppFastSnapshot& snapshot){
    if(!snapshot.enumerationComplete || !snapshot.desktopLookupsComplete)
        return false;
    for(const FastWin& window : snapshot.windows){
        const GUID& guid=window.desktop;
        bool zero=guid.Data1==0 && guid.Data2==0 && guid.Data3==0;
        for(size_t index=0;zero && index<sizeof(guid.Data4);++index)
            zero=guid.Data4[index]==0;
        if(zero) return false;
    }
    return true;
}

inline void FinalizeFastSnapshot(const std::string& app,
                                 uint64_t profileConfigSignature,
                                 SnapshotVersionTracker& tracker,
                                 AppFastSnapshot& snapshot){
    std::sort(snapshot.windows.begin(),snapshot.windows.end(),
        [](const FastWin& left,const FastWin& right){
            const WindowIdentityKey a=IdentityOf(left),b=IdentityOf(right);
            if(a<b) return true;
            if(b<a) return false;
            return left.app<right.app;
        });

    SnapshotSignatureBuilder windowSet;
    windowSet.addString(app).addUnsigned(snapshot.windows.size());
    for(const FastWin& window : snapshot.windows){
        const WindowIdentityKey identity=IdentityOf(window);
        windowSet.addUnsigned(static_cast<uint64_t>(identity.hwnd))
                 .addUnsigned(static_cast<uint64_t>(identity.pid))
                 .addUnsigned(identity.processStart);
    }
    snapshot.windowSetSignature=windowSet.value();

    SnapshotSignatureBuilder settle;
    settle.addUnsigned(snapshot.windowSetSignature);
    SnapshotSignatureBuilder layout;
    layout.addUnsigned(snapshot.windowSetSignature);
    for(const FastWin& window : snapshot.windows){
        settle.addWideString(window.title);
        layout.addBytes(&window.desktop,sizeof(window.desktop));
    }
    snapshot.settleSignature=settle.value();
    snapshot.layoutSignature=layout.value();

    SnapshotSignatureBuilder identityQualityConfig;
    identityQualityConfig.addString(app)
        .addUnsigned(profileConfigSignature)
        .addUnsigned(snapshot.enumerationComplete ? 1 : 0)
        .addUnsigned(snapshot.desktopLookupsComplete ? 1 : 0)
        .addUnsigned(snapshot.windowSetSignature);
    SnapshotSignatureBuilder content;
    content.addUnsigned(identityQualityConfig.value())
        .addUnsigned(snapshot.settleSignature)
        .addUnsigned(snapshot.layoutSignature);
    const SnapshotVersions versions=tracker.observe(
        app,identityQualityConfig.value(),content.value());
    snapshot.identityGeneration=versions.identityGeneration;
    snapshot.generation=versions.contentGeneration;
}
