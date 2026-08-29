// binding_store.hpp — persisted window→record bindings and the rule that
// decides when VDE may move a window back.
//
// While VDE runs, a window is identified exactly by (HWND, PID, process start
// time); a bound window keeps its record no matter how its tabs change.  The
// content fingerprint is only needed to *re-find* a window whose identity is
// gone — that is, after the browser restarted.
//
// Those bindings used to live only in memory, so restarting VDE looked exactly
// like restarting the browser: every window became unknown and was re-placed
// from its fingerprint, even though nothing had moved.  Persisting them makes
// the two cases distinguishable:
//
//   * the identity still matches a live window  -> the same physical window,
//     bind it silently and never move it;
//   * no identity matches                       -> the browser really did
//     restart, so search by tabs and restore.
//
// Pure logic (no COM, no GUI, no file system) so it can be unit-tested; see
// tests/vdtest.cpp.
#pragma once

#include "layout.hpp"

#include <cstdint>
#include <string>
#include <vector>

static const size_t MAX_PERSISTED_BINDINGS = 4096;
static const unsigned long long MAX_BINDING_FILE_BYTES = 1024ULL * 1024ULL;

// How long after a browser's windows first appear VDE still treats a newly
// seen window as "part of the restart".  Firefox restores some windows lazily,
// several seconds apart; without this grace the late ones would be classified
// as user-opened and left wherever Windows put them.
static const uint64_t RESTORE_GRACE_MS = 60ULL * 1000ULL;

struct PersistedBinding {
    std::string app;
    std::string recordId;
    uint64_t hwnd = 0;
    uint64_t pid = 0;
    uint64_t processStart = 0;
};

inline bool ValidPersistedBinding(const PersistedBinding& binding){
    if(!IsSupportedLayoutApp(binding.app)) return false;
    if(binding.hwnd == 0 || binding.pid == 0 || binding.processStart == 0)
        return false;
    GUID parsed{};
    return ParseNonzeroLayoutGuid(binding.recordId, parsed);
}

//   # VDE bindings v1
//   B <app> <recordId> <hwnd> <pid> <processStart>
inline std::string SerializeBindings(
        const std::vector<PersistedBinding>& bindings){
    std::string out = "# VDE bindings v1\n";
    for(size_t i = 0; i < bindings.size(); ++i){
        const PersistedBinding& binding = bindings[i];
        if(!ValidPersistedBinding(binding)) continue;
        out += "B\t"; out += binding.app; out += "\t";
        out += binding.recordId; out += "\t";
        out += std::to_string(binding.hwnd); out += "\t";
        out += std::to_string(binding.pid); out += "\t";
        out += std::to_string(binding.processStart); out += "\n";
    }
    return out;
}

inline bool ParseBindings(const std::string& data,
                          std::vector<PersistedBinding>& output,
                          std::string* errorOut = nullptr){
    std::vector<PersistedBinding> parsed;
    bool headerSeen = false;
    size_t position = 0, lineNumber = 0;
    auto fail = [&](const std::string& message)->bool {
        if(errorOut) *errorOut = "line " + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    if((unsigned long long)data.size() > MAX_BINDING_FILE_BYTES){
        if(errorOut) *errorOut = "binding file is too large";
        return false;
    }
    try {
        while(position < data.size()){
            size_t newline = data.find('\n', position);
            std::string line = data.substr(
                position, (newline == std::string::npos ? data.size() : newline) - position);
            position = newline == std::string::npos ? data.size() : newline + 1;
            ++lineNumber;
            if(!line.empty() && line.back() == '\r') line.pop_back();
            if(line.empty()) continue;
            if(line[0] == '#'){
                if(headerSeen) return fail("duplicate header");
                if(line != "# VDE bindings v1") return fail("unsupported binding header");
                headerSeen = true;
                continue;
            }
            if(!headerSeen) return fail("record before header");
            std::vector<std::string> fields;
            for(size_t fieldPos = 0;;){
                size_t tab = line.find('\t', fieldPos);
                fields.push_back(line.substr(
                    fieldPos, (tab == std::string::npos ? line.size() : tab) - fieldPos));
                if(tab == std::string::npos) break;
                fieldPos = tab + 1;
            }
            if(fields[0] != "B") return fail("unknown record type");
            if(fields.size() != 6) return fail("binding record needs 6 fields");
            if(parsed.size() >= MAX_PERSISTED_BINDINGS) return fail("too many bindings");
            PersistedBinding binding;
            binding.app = fields[1];
            GUID id{};
            std::string canonical;
            if(!ParseNonzeroLayoutGuid(fields[2], id, &canonical))
                return fail("invalid record ID");
            binding.recordId = canonical;
            unsigned long long hwnd = 0, pid = 0, started = 0;
            if(!ParseU64Strict(fields[3], hwnd) || hwnd == 0) return fail("invalid window handle");
            if(!ParseU64Strict(fields[4], pid) || pid == 0) return fail("invalid process ID");
            if(!ParseU64Strict(fields[5], started) || started == 0)
                return fail("invalid process start time");
            binding.hwnd = (uint64_t)hwnd;
            binding.pid = (uint64_t)pid;
            binding.processStart = (uint64_t)started;
            if(!ValidPersistedBinding(binding)) return fail("unusable binding");
            parsed.push_back(std::move(binding));
        }
    } catch(...) {
        if(errorOut) *errorOut = "out of memory parsing bindings";
        return false;
    }
    if(!headerSeen){
        if(errorOut) *errorOut = "bindings file is missing its header";
        return false;
    }
    output = std::move(parsed);
    if(errorOut) errorOut->clear();
    return true;
}

// Cheap change detector so the file is only rewritten when the binding set
// actually changed (it is rewritten from the observation loop, not from the
// layout transaction).
inline uint64_t BindingsSignature(const std::vector<PersistedBinding>& bindings){
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](uint64_t value){
        for(int byte = 0; byte < 8; ++byte){
            hash ^= (value >> (byte * 8)) & 0xffULL;
            hash *= 1099511628211ULL;
        }
    };
    auto mixText = [&](const std::string& text){
        for(size_t i = 0; i < text.size(); ++i){
            hash ^= (unsigned char)text[i];
            hash *= 1099511628211ULL;
        }
        mix(text.size());
    };
    mix(bindings.size());
    for(size_t i = 0; i < bindings.size(); ++i){
        mixText(bindings[i].app);
        mixText(bindings[i].recordId);
        mix(bindings[i].hwnd);
        mix(bindings[i].pid);
        mix(bindings[i].processStart);
    }
    return hash;
}

// ---------------------------------------------------------- restore gate ----
// One gate per tracked app.  "Cold" means VDE holds no live binding for that
// app: either it has just started and nothing matched, or the browser really
// did restart.  Only a cold app (plus a short grace after it warms up) may have
// its windows moved back; once warm, an appearing window is the user's doing
// and is only recorded.
struct RestoreGate {
    uint64_t warmSinceMs = 0;      // 0 = cold
};

enum class RestoreDisposition { Restore, RecordOnly };

inline void ObserveRestoreGate(RestoreGate& gate, bool hasLiveBindings,
                               uint64_t nowMs){
    if(!hasLiveBindings){
        gate.warmSinceMs = 0;
        return;
    }
    if(gate.warmSinceMs == 0) gate.warmSinceMs = nowMs != 0 ? nowMs : 1;
    else if(nowMs < gate.warmSinceMs) gate.warmSinceMs = nowMs;   // clock rollback
}

inline RestoreDisposition GateDisposition(const RestoreGate& gate, uint64_t nowMs,
                                          uint64_t graceMs = RESTORE_GRACE_MS){
    if(gate.warmSinceMs == 0) return RestoreDisposition::Restore;
    if(nowMs >= gate.warmSinceMs && nowMs - gate.warmSinceMs < graceMs)
        return RestoreDisposition::Restore;
    return RestoreDisposition::RecordOnly;
}
