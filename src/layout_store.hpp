// layout_store.hpp — bounded, recoverable layout persistence.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "layout.hpp"
#include "str_util.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

static const unsigned long long MAX_LAYOUT_FILE_BYTES=16ULL*1024ULL*1024ULL;

enum class LayoutLoadStatus {
    Missing, Valid, Recovered, CorruptPreserved, Unavailable
};

struct LayoutRevision {
    std::wstring sourcePath;
    unsigned long long size=0;
    unsigned long long mtime=0;
    uint64_t contentHash=0;
    bool exists=false;
};

struct LayoutLoadResult {
    LayoutLoadStatus status=LayoutLoadStatus::Unavailable;
    bool writesAllowed=false;
    int sourceVersion=0;
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    LayoutRevision revision;
    std::string error;
    bool usable() const {
        return status==LayoutLoadStatus::Valid || status==LayoutLoadStatus::Recovered;
    }
};

class ScopedLayoutLock {
public:
    explicit ScopedLayoutLock(DWORD timeoutMs=0){
        handle_=CreateMutexW(nullptr,FALSE,L"Local\\win-vde.layout-store.v1");
        if(!handle_) return;
        DWORD waited=WaitForSingleObject(handle_,timeoutMs);
        acquired_=waited==WAIT_OBJECT_0 || waited==WAIT_ABANDONED;
    }
    ~ScopedLayoutLock(){
        if(acquired_) ReleaseMutex(handle_);
        if(handle_) CloseHandle(handle_);
    }
    bool acquired() const { return acquired_; }
    ScopedLayoutLock(const ScopedLayoutLock&)=delete;
    ScopedLayoutLock& operator=(const ScopedLayoutLock&)=delete;
private:
    HANDLE handle_=nullptr;
    bool acquired_=false;
};

inline bool SameRevision(const LayoutRevision& a,const LayoutRevision& b){
    return a.sourcePath==b.sourcePath && a.exists==b.exists && a.size==b.size &&
        a.mtime==b.mtime && a.contentHash==b.contentHash;
}

namespace layout_store_detail {

inline uint64_t HashLayoutBytes(const std::string& bytes){
    uint64_t hash=14695981039346656037ULL;
    for(unsigned char byte : bytes){ hash^=(uint64_t)byte; hash*=1099511628211ULL; }
    return hash;
}

inline DWORD LastErrorOr(DWORD fallback){
    DWORD error=GetLastError();
    return error==ERROR_SUCCESS ? fallback : error;
}

inline std::string Win32Failure(const std::string& operation,DWORD error){
    return operation+" (Win32 error "+std::to_string((unsigned long long)error)+")";
}

inline bool SetFailure(std::string* errorOut,const std::string& message){
    if(errorOut) *errorOut=message.empty() ? "layout storage operation failed" : message;
    return false;
}

inline LayoutRevision RevisionFromRead(const std::wstring& path,const FileReadMetadata& metadata,
        const std::string& bytes){
    LayoutRevision revision;
    revision.sourcePath=path;
    revision.exists=true;
    revision.size=metadata.size;
    revision.mtime=metadata.mtime;
    revision.contentHash=HashLayoutBytes(bytes);
    return revision;
}

enum class PathState { Missing, File, Unavailable };

inline PathState QueryPath(const std::wstring& path,const LayoutFsOps& ops,std::string* errorOut){
    DWORD attributes=ops.getAttributes(path);
    if(attributes==INVALID_FILE_ATTRIBUTES){
        DWORD error=LastErrorOr(ERROR_GEN_FAILURE);
        if(str_util_detail::IsMissingFileError(error)) return PathState::Missing;
        if(errorOut) *errorOut=Win32Failure("GetFileAttributesW failed",error);
        return PathState::Unavailable;
    }
    if(attributes&FILE_ATTRIBUTE_DIRECTORY){
        if(errorOut) *errorOut="layout path names a directory";
        return PathState::Unavailable;
    }
    return PathState::File;
}

inline bool ReadExactBytes(const std::wstring& path,const LayoutFsOps& ops,std::string& bytes,
        LayoutRevision* revisionOut,std::string* errorOut){
    FileReadMetadata metadata;
    FileReadResult read=ReadFileBytesBoundedWithMetadata(path,MAX_LAYOUT_FILE_BYTES,ops,&metadata);
    if(read.status!=FileReadStatus::Ok){
        if(errorOut) *errorOut=read.error.empty() ? "layout file is unreadable" : read.error;
        return false;
    }
    if(revisionOut) *revisionOut=RevisionFromRead(path,metadata,read.bytes);
    bytes.swap(read.bytes);
    return true;
}

inline bool VerifyExactFile(const std::wstring& path,const std::string& expected,
        const LayoutFsOps& ops,std::string* errorOut){
    std::string actual;
    LayoutRevision revision;
    std::string readError;
    if(!ReadExactBytes(path,ops,actual,&revision,&readError)){
        if(errorOut) *errorOut="verification read failed for "+W2U8(path)+": "+readError;
        return false;
    }
    uint64_t expectedHash=HashLayoutBytes(expected);
    if(revision.size!=(unsigned long long)expected.size() || revision.contentHash!=expectedHash || actual!=expected){
        if(errorOut) *errorOut="verification mismatch for "+W2U8(path);
        return false;
    }
    return true;
}

struct OptionalBytes {
    std::wstring path;
    bool exists=false;
    std::string bytes;
};

enum class ExactFileState { Exact, Missing, Mismatch, Unavailable };

inline ExactFileState InspectExactFile(const std::wstring& path,const std::string& expected,
        const LayoutFsOps& ops,std::string* errorOut){
    FileReadResult read=ReadFileBytesBounded(path,MAX_LAYOUT_FILE_BYTES,ops);
    if(read.status==FileReadStatus::Missing) return ExactFileState::Missing;
    if(read.status!=FileReadStatus::Ok){
        if(errorOut) *errorOut="verification read failed for "+W2U8(path)+": "+
            (read.error.empty() ? "file is unavailable" : read.error);
        return ExactFileState::Unavailable;
    }
    if(read.bytes.size()==expected.size() && HashLayoutBytes(read.bytes)==HashLayoutBytes(expected) &&
       read.bytes==expected)
        return ExactFileState::Exact;
    if(errorOut) *errorOut="verification mismatch for "+W2U8(path);
    return ExactFileState::Mismatch;
}

inline bool DeleteArtifactChecked(const std::wstring& path,const char* label,
        const LayoutFsOps& ops,std::string* errorOut){
    if(!ops.deleteFile(path))
        return SetFailure(errorOut,Win32Failure(std::string("DeleteFileW ")+label+" failed",
            LastErrorOr(ERROR_ACCESS_DENIED)));
    std::string queryError;
    PathState state=QueryPath(path,ops,&queryError);
    if(state==PathState::Unavailable)
        return SetFailure(errorOut,std::string("cannot verify ")+label+" cleanup: "+queryError);
    if(state!=PathState::Missing)
        return SetFailure(errorOut,std::string("DeleteFileW reported success but ")+label+" remains");
    return true;
}

// A promotion marker is authoritative only after a verified non-authoritative
// stage has been renamed into place.  A failed CopyFile may leave arbitrary
// bytes at the stage path, but can never publish those bytes as transaction
// intent.
inline bool EnsurePromotionMarker(const std::wstring& rollback,const std::wstring& promote,
        const std::string& expected,const LayoutFsOps& ops,std::string* errorOut){
    const std::wstring stage=promote+L".stage";
    std::string error;
    ExactFileState markerState=InspectExactFile(promote,expected,ops,&error);
    if(markerState==ExactFileState::Unavailable)
        return SetFailure(errorOut,"cannot inspect rollback-promotion marker: "+error);
    if(markerState==ExactFileState::Mismatch)
        return SetFailure(errorOut,"rollback-promotion marker conflicts with captured rollback");
    if(markerState==ExactFileState::Exact){
        PathState stageState=QueryPath(stage,ops,&error);
        if(stageState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot inspect obsolete rollback-promotion stage: "+error);
        if(stageState==PathState::File){
            if(!VerifyExactFile(promote,expected,ops,&error))
                return SetFailure(errorOut,
                    "cannot verify promotion marker before obsolete stage cleanup: "+error);
            if(!DeleteArtifactChecked(stage,"obsolete rollback-promotion stage",ops,errorOut))
                return false;
            if(!VerifyExactFile(promote,expected,ops,&error))
                return SetFailure(errorOut,
                    "promotion marker changed during obsolete stage cleanup: "+error);
        }
        return true;
    }

    ExactFileState rollbackState=InspectExactFile(rollback,expected,ops,&error);
    if(rollbackState!=ExactFileState::Exact)
        return SetFailure(errorOut,"cannot verify rollback before promotion-marker creation: "+error);

    PathState staleStageState=QueryPath(stage,ops,&error);
    if(staleStageState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot inspect rollback-promotion stage: "+error);
    if(staleStageState==PathState::File){
        if(!DeleteArtifactChecked(stage,"uncommitted rollback-promotion stage",ops,errorOut))
            return false;
        if(!VerifyExactFile(rollback,expected,ops,&error))
            return SetFailure(errorOut,
                "rollback changed during uncommitted promotion-stage cleanup: "+error);
    }
    if(!ops.copyFile(rollback,stage,TRUE))
        return SetFailure(errorOut,Win32Failure("CopyFileW rollback-promotion stage failed",
            LastErrorOr(ERROR_GEN_FAILURE)));
    ExactFileState committedStageState=InspectExactFile(stage,expected,ops,&error);
    if(committedStageState!=ExactFileState::Exact)
        return SetFailure(errorOut,"rollback-promotion stage verification failed: "+error);
    if(!ops.moveFile(stage,promote,MOVEFILE_WRITE_THROUGH))
        return SetFailure(errorOut,Win32Failure("MoveFileExW rollback-promotion marker publish failed",
            LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
    markerState=InspectExactFile(promote,expected,ops,&error);
    if(markerState!=ExactFileState::Exact)
        return SetFailure(errorOut,"rollback-promotion marker verification failed: "+error);
    return true;
}

inline bool EnsureExactFromMarker(const std::wstring& marker,const std::wstring& target,
        const std::string& expected,const LayoutFsOps& ops,bool& repaired,std::string* errorOut){
    repaired=false;
    std::string error;
    ExactFileState markerState=InspectExactFile(marker,expected,ops,&error);
    if(markerState!=ExactFileState::Exact)
        return SetFailure(errorOut,"recovery marker is not readable and exact: "+error);
    ExactFileState targetState=InspectExactFile(target,expected,ops,&error);
    if(targetState==ExactFileState::Exact) return true;
    if(targetState==ExactFileState::Unavailable)
        return SetFailure(errorOut,"recovery target is transiently unavailable; marker retained: "+error);
    if(!ops.copyFile(marker,target,FALSE))
        return SetFailure(errorOut,Win32Failure("CopyFileW recovery-target repair failed",
            LastErrorOr(ERROR_GEN_FAILURE)));
    targetState=InspectExactFile(target,expected,ops,&error);
    if(targetState!=ExactFileState::Exact)
        return SetFailure(errorOut,"recovery-target repair verification failed: "+error);
    repaired=true;
    return true;
}

inline bool CaptureOptional(const std::wstring& path,const LayoutFsOps& ops,OptionalBytes& captured,
        std::string* errorOut){
    captured=OptionalBytes();
    captured.path=path;
    std::string queryError;
    PathState state=QueryPath(path,ops,&queryError);
    if(state==PathState::Missing) return true;
    if(state==PathState::Unavailable){
        if(errorOut) *errorOut=queryError;
        return false;
    }
    LayoutRevision ignored;
    if(!ReadExactBytes(path,ops,captured.bytes,&ignored,errorOut)) return false;
    captured.exists=true;
    return true;
}

inline bool VerifyCaptured(const OptionalBytes& captured,const LayoutFsOps& ops,std::string* errorOut){
    return !captured.exists || VerifyExactFile(captured.path,captured.bytes,ops,errorOut);
}

inline bool ResolveWriteTempArtifacts(const std::wstring& primary,OptionalBytes& currentPrimary,
        bool allowCommittedRecovery,const LayoutFsOps& ops,std::string* errorOut){
    const std::wstring temp=primary+L".tmp";
    const std::wstring stage=temp+L".stage";
    OptionalBytes committed;
    std::string error;
    if(!CaptureOptional(temp,ops,committed,&error))
        return SetFailure(errorOut,"cannot inspect committed temporary layout: "+error);
    PathState stageState=QueryPath(stage,ops,&error);
    if(stageState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot inspect temporary write stage: "+error);

    if(currentPrimary.exists){
        if(!committed.exists && stageState==PathState::Missing) return true;
        if(!VerifyCaptured(currentPrimary,ops,&error))
            return SetFailure(errorOut,"cannot verify primary before temporary cleanup: "+error);
        if(stageState==PathState::File &&
           !DeleteArtifactChecked(stage,"stale temporary write stage",ops,errorOut))
            return false;
        if(committed.exists &&
           !DeleteArtifactChecked(temp,"stale committed temporary layout",ops,errorOut))
            return false;
        if(!VerifyCaptured(currentPrimary,ops,&error))
            return SetFailure(errorOut,"primary changed during temporary cleanup: "+error);
        return true;
    }

    if(!allowCommittedRecovery){
        if(committed.exists && !VerifyCaptured(committed,ops,&error))
            return SetFailure(errorOut,"noncanonical committed temporary changed before cleanup: "+error);
        if(stageState==PathState::File &&
           !DeleteArtifactChecked(stage,"noncanonical temporary write stage",ops,errorOut))
            return false;
        if(committed.exists &&
           !DeleteArtifactChecked(temp,"noncanonical committed temporary layout",ops,errorOut))
            return false;
        return true;
    }

    if(!committed.exists){
        if(stageState==PathState::File)
            return DeleteArtifactChecked(stage,"uncommitted temporary write stage",ops,errorOut);
        return true;
    }
    if(!VerifyCaptured(committed,ops,&error))
        return SetFailure(errorOut,"committed temporary layout changed before recovery: "+error);
    if(stageState==PathState::File &&
       !DeleteArtifactChecked(stage,"obsolete temporary write stage",ops,errorOut))
        return false;
    if(!ops.moveFile(temp,primary,MOVEFILE_WRITE_THROUGH))
        return SetFailure(errorOut,Win32Failure("MoveFileExW committed temporary recovery failed",
            LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
    if(!VerifyExactFile(primary,committed.bytes,ops,&error))
        return SetFailure(errorOut,"committed temporary recovery verification failed: "+error);
    PathState tempState=QueryPath(temp,ops,&error);
    if(tempState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot verify committed temporary consumption: "+error);
    if(tempState!=PathState::Missing)
        return SetFailure(errorOut,"MoveFileExW reported success but committed temporary remains");
    currentPrimary.path=primary;
    currentPrimary.exists=true;
    currentPrimary.bytes=committed.bytes;
    return true;
}

inline bool ResolveDisplacedForRecovery(const std::wstring& primary,const std::wstring& displaced,
        const std::string& currentPrimary,const std::string& requested,
        OptionalBytes& preservedBackup,OptionalBytes& preservedRollback,
        const LayoutFsOps& ops,bool& requestAlreadyPublished,std::string* errorOut){
    requestAlreadyPublished=false;
    OptionalBytes staleDisplaced;
    std::string error;
    if(!CaptureOptional(displaced,ops,staleDisplaced,&error))
        return SetFailure(errorOut,"cannot inspect prior displaced artifact: "+error);
    if(!staleDisplaced.exists) return true;
    if(!VerifyExactFile(primary,currentPrimary,ops,&error) ||
       !VerifyCaptured(staleDisplaced,ops,&error) ||
       !VerifyCaptured(preservedBackup,ops,&error) ||
       !VerifyCaptured(preservedRollback,ops,&error))
        return SetFailure(errorOut,"cannot verify survivors before displaced cleanup: "+error);

    // A preserve-mode ReplaceFile backup is itself the captured prior stream.
    // If no named recovery survived and another payload is requested, first
    // make that stream canonical at .bak before consuming .displaced.
    if(currentPrimary!=requested && !preservedBackup.exists && !preservedRollback.exists){
        if(!ops.moveFile(displaced,preservedBackup.path,MOVEFILE_WRITE_THROUGH))
            return SetFailure(errorOut,Win32Failure(
                "MoveFileExW displaced-to-backup recovery failed",
                LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
        if(!VerifyExactFile(preservedBackup.path,staleDisplaced.bytes,ops,&error))
            return SetFailure(errorOut,"displaced-to-backup recovery verification failed: "+error);
        PathState displacedState=QueryPath(displaced,ops,&error);
        if(displacedState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot verify displaced-to-backup consumption: "+error);
        if(displacedState!=PathState::Missing)
            return SetFailure(errorOut,
                "MoveFileExW reported success but displaced artifact remains");
        if(!VerifyExactFile(primary,currentPrimary,ops,&error))
            return SetFailure(errorOut,
                "primary changed during displaced-to-backup recovery: "+error);
        preservedBackup.exists=true;
        preservedBackup.bytes=staleDisplaced.bytes;
        return true;
    }
    if(!DeleteArtifactChecked(displaced,"prior displaced artifact",ops,errorOut)) return false;
    if(!VerifyExactFile(primary,currentPrimary,ops,&error) ||
       !VerifyCaptured(preservedBackup,ops,&error) ||
       !VerifyCaptured(preservedRollback,ops,&error))
        return SetFailure(errorOut,"survivor verification failed after displaced cleanup: "+error);
    requestAlreadyPublished=currentPrimary==requested;
    return true;
}

inline bool ResolveRedundantDisplaced(const std::wstring& primary,const std::wstring& displaced,
        const std::string& currentPrimary,const LayoutFsOps& ops,std::string* errorOut){
    OptionalBytes staleDisplaced;
    std::string error;
    if(!CaptureOptional(displaced,ops,staleDisplaced,&error))
        return SetFailure(errorOut,"cannot inspect displaced artifact beside valid primary: "+error);
    if(!staleDisplaced.exists) return true;
    if(!VerifyExactFile(primary,currentPrimary,ops,&error) ||
       !VerifyCaptured(staleDisplaced,ops,&error))
        return SetFailure(errorOut,
            "cannot verify valid primary before displaced cleanup: "+error);
    if(!DeleteArtifactChecked(displaced,"redundant displaced artifact",ops,errorOut)) return false;
    if(!VerifyExactFile(primary,currentPrimary,ops,&error))
        return SetFailure(errorOut,
            "primary changed during redundant displaced cleanup: "+error);
    return true;
}

inline bool ResolveDisplacedWithoutPrimary(const std::wstring& displaced,
        OptionalBytes& preservedBackup,OptionalBytes& preservedRollback,
        const LayoutFsOps& ops,OptionalBytes& cleanupPending,std::string* errorOut){
    cleanupPending=OptionalBytes();
    cleanupPending.path=displaced;
    OptionalBytes staleDisplaced;
    std::string error;
    if(!CaptureOptional(displaced,ops,staleDisplaced,&error))
        return SetFailure(errorOut,"cannot inspect displaced artifact beside missing primary: "+error);
    if(!staleDisplaced.exists) return true;
    if(!VerifyCaptured(staleDisplaced,ops,&error) ||
       !VerifyCaptured(preservedBackup,ops,&error) || !VerifyCaptured(preservedRollback,ops,&error))
        return SetFailure(errorOut,"cannot verify recovery beside displaced artifact: "+error);
    if(!preservedBackup.exists && !preservedRollback.exists){
        if(!ops.moveFile(displaced,preservedBackup.path,MOVEFILE_WRITE_THROUGH))
            return SetFailure(errorOut,Win32Failure(
                "MoveFileExW displaced-only recovery failed",
                LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
        if(!VerifyExactFile(preservedBackup.path,staleDisplaced.bytes,ops,&error))
            return SetFailure(errorOut,"displaced-only recovery verification failed: "+error);
        PathState displacedState=QueryPath(displaced,ops,&error);
        if(displacedState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot verify displaced-only consumption: "+error);
        if(displacedState!=PathState::Missing)
            return SetFailure(errorOut,
                "MoveFileExW reported success but displaced-only artifact remains");
        preservedBackup.exists=true;
        preservedBackup.bytes=staleDisplaced.bytes;
        return true;
    }
    cleanupPending=staleDisplaced;
    return true;
}

inline std::string DescribeReplaceFailureRecovery(const std::wstring& primary,
        const std::wstring& recoveryArtifact,const std::string& priorPrimary,const LayoutFsOps& ops){
    std::string error;
    if(VerifyExactFile(primary,priorPrimary,ops,&error))
        return "prior primary verified at canonical path";
    std::string primaryError=error;
    if(VerifyExactFile(recoveryArtifact,priorPrimary,ops,&error))
        return "prior primary verified at recovery artifact";
    return "prior primary recovery could not be verified (primary: "+primaryError+"; artifact: "+error+")";
}

inline bool ResolveStaleBakPrevious(const OptionalBytes& currentPrimary,
        const std::wstring& rollback,const std::wstring& backup,const std::wstring& previous,
        const LayoutFsOps& ops,std::string* errorOut){
    const std::wstring restore=previous+L".restore";
    const std::wstring promote=previous+L".promote";
    const std::wstring promoteStage=promote+L".stage";
    const std::wstring stagingTemp=previous+L".stage";
    std::string error;
    auto deleteChecked=[&](const std::wstring& path,const char* label)->bool{
        if(!ops.deleteFile(path))
            return SetFailure(errorOut,Win32Failure(std::string("DeleteFileW ")+label+" failed",
                LastErrorOr(ERROR_ACCESS_DENIED)));
        std::string queryError;
        PathState state=QueryPath(path,ops,&queryError);
        if(state==PathState::Unavailable)
            return SetFailure(errorOut,std::string("cannot verify ")+label+" cleanup: "+queryError);
        if(state!=PathState::Missing)
            return SetFailure(errorOut,std::string("DeleteFileW reported success but ")+label+" remains");
        return true;
    };

    OptionalBytes staged;
    if(!CaptureOptional(previous,ops,staged,&error))
        return SetFailure(errorOut,"cannot inspect staged prior backup: "+error);
    PathState orphanStageState=QueryPath(stagingTemp,ops,&error);
    if(orphanStageState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot inspect prior-backup staging temp: "+error);
    if(!staged.exists){
        OptionalBytes currentRollback,currentBackup,promotionCopy,restoreCopy;
        if(!CaptureOptional(rollback,ops,currentRollback,&error) ||
           !CaptureOptional(backup,ops,currentBackup,&error) ||
           !CaptureOptional(promote,ops,promotionCopy,&error) ||
           !CaptureOptional(restore,ops,restoreCopy,&error))
            return SetFailure(errorOut,"cannot inspect recovery staging without prior backup: "+error);
        PathState promotionStageState=QueryPath(promoteStage,ops,&error);
        if(promotionStageState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot inspect rollback-promotion stage: "+error);
        if(promotionStageState==PathState::File){
            if(promotionCopy.exists){
                if(!VerifyCaptured(promotionCopy,ops,&error))
                    return SetFailure(errorOut,
                        "cannot verify promotion marker before stage cleanup: "+error);
                if(!deleteChecked(promoteStage,"obsolete rollback-promotion stage")) return false;
                if(!VerifyCaptured(promotionCopy,ops,&error))
                    return SetFailure(errorOut,
                        "promotion marker changed during stage cleanup: "+error);
            } else if(currentRollback.exists){
                if(!EnsurePromotionMarker(rollback,promote,currentRollback.bytes,ops,errorOut)) return false;
                promotionCopy.path=promote;
                promotionCopy.exists=true;
                promotionCopy.bytes=currentRollback.bytes;
            } else {
                // The .promote.stage name is deliberately non-authoritative:
                // CopyFile may have returned FALSE after leaving arbitrary
                // bytes there.  Once canonical survivors are exact it can be
                // discarded without interpreting its contents as intent.
                if(!currentPrimary.exists && !currentBackup.exists)
                    return SetFailure(errorOut,
                        "cannot discard rollback-promotion stage without a canonical stream");
                if(!VerifyCaptured(currentPrimary,ops,&error) ||
                   !VerifyCaptured(currentBackup,ops,&error))
                    return SetFailure(errorOut,
                        "cannot verify canonical state before rollback-promotion stage cleanup: "+error);
                if(!deleteChecked(promoteStage,"orphan rollback-promotion stage")) return false;
                if(!VerifyCaptured(currentPrimary,ops,&error) ||
                   !VerifyCaptured(currentBackup,ops,&error))
                    return SetFailure(errorOut,
                        "canonical state changed during rollback-promotion stage cleanup: "+error);
            }
        }
        if(promotionCopy.exists){
            if(currentRollback.exists && currentRollback.bytes!=promotionCopy.bytes){
                if(!ops.copyFile(promote,rollback,FALSE))
                    return SetFailure(errorOut,Win32Failure(
                        "CopyFileW live rollback repair from promotion staging failed",
                        LastErrorOr(ERROR_GEN_FAILURE)));
                if(!VerifyExactFile(rollback,promotionCopy.bytes,ops,&error))
                    return SetFailure(errorOut,"live rollback repair verification failed: "+error);
                currentRollback.bytes=promotionCopy.bytes;
            }
            if(currentRollback.exists){
                if(!ops.moveFile(rollback,backup,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                    return SetFailure(errorOut,Win32Failure(
                        "MoveFileExW pending rollback promotion failed",LastErrorOr(ERROR_ACCESS_DENIED)));
                currentRollback.exists=false;
                currentRollback.bytes.clear();
            }
            bool repaired=false;
            if(!EnsureExactFromMarker(promote,backup,promotionCopy.bytes,ops,repaired,errorOut))
                return false;
            currentBackup.path=backup;
            currentBackup.exists=true;
            currentBackup.bytes=promotionCopy.bytes;
            if(!deleteChecked(promote,"rollback-promotion staging")) return false;
            if(restoreCopy.exists && !deleteChecked(restore,"obsolete backup-restoration staging"))
                return false;
            restoreCopy.exists=false;
        } else if(restoreCopy.exists){
            bool repaired=false;
            if(!EnsureExactFromMarker(restore,backup,restoreCopy.bytes,ops,repaired,errorOut)) return false;
            currentBackup.path=backup;
            currentBackup.exists=true;
            currentBackup.bytes=restoreCopy.bytes;
            if(!deleteChecked(restore,"backup-restoration staging")) return false;
        }
        if(orphanStageState!=PathState::File) return true;
        if(!currentPrimary.exists && !currentRollback.exists && !currentBackup.exists)
            return SetFailure(errorOut,"cannot discard orphan staging without a canonical layout stream");
        if(!VerifyCaptured(currentPrimary,ops,&error) || !VerifyCaptured(currentRollback,ops,&error) ||
           !VerifyCaptured(currentBackup,ops,&error))
            return SetFailure(errorOut,"cannot verify recovery before orphan staging cleanup: "+error);
        if(!deleteChecked(stagingTemp,"orphan prior-backup staging temp")) return false;
        if(!VerifyCaptured(currentPrimary,ops,&error) || !VerifyCaptured(currentRollback,ops,&error) ||
           !VerifyCaptured(currentBackup,ops,&error))
            return SetFailure(errorOut,"recovery changed during orphan staging cleanup: "+error);
        return true;
    }

    OptionalBytes currentRollback,currentBackup,restoreCopy,promotionCopy;
    if(!CaptureOptional(rollback,ops,currentRollback,&error) ||
       !CaptureOptional(backup,ops,currentBackup,&error) ||
       !CaptureOptional(restore,ops,restoreCopy,&error) ||
       !CaptureOptional(promote,ops,promotionCopy,&error))
        return SetFailure(errorOut,"cannot inspect recovery streams beside staged backup: "+error);
    PathState promotionStageState=QueryPath(promoteStage,ops,&error);
    if(promotionStageState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot inspect rollback-promotion stage: "+error);

    if(promotionStageState==PathState::File){
        if(promotionCopy.exists){
            if(!VerifyCaptured(promotionCopy,ops,&error))
                return SetFailure(errorOut,
                    "cannot verify promotion marker before stage cleanup: "+error);
            if(!deleteChecked(promoteStage,"obsolete rollback-promotion stage")) return false;
            if(!VerifyCaptured(promotionCopy,ops,&error))
                return SetFailure(errorOut,
                    "promotion marker changed during stage cleanup: "+error);
        } else if(currentRollback.exists){
            if(!EnsurePromotionMarker(rollback,promote,currentRollback.bytes,ops,errorOut)) return false;
            promotionCopy.path=promote;
            promotionCopy.exists=true;
            promotionCopy.bytes=currentRollback.bytes;
        } else {
            if(!VerifyCaptured(currentPrimary,ops,&error) ||
               !VerifyCaptured(currentBackup,ops,&error) ||
               !VerifyCaptured(staged,ops,&error))
                return SetFailure(errorOut,
                    "cannot verify canonical state before rollback-promotion stage cleanup: "+error);
            if(!deleteChecked(promoteStage,"orphan rollback-promotion stage")) return false;
            if(!VerifyCaptured(currentPrimary,ops,&error) ||
               !VerifyCaptured(currentBackup,ops,&error) ||
               !VerifyCaptured(staged,ops,&error))
                return SetFailure(errorOut,
                    "canonical state changed during rollback-promotion stage cleanup: "+error);
        }
    }

    if(restoreCopy.exists && restoreCopy.bytes!=staged.bytes){
        if(!deleteChecked(restore,"poisoned backup-restoration staging")) return false;
        return SetFailure(errorOut,
            "backup-restoration staging readback mismatch removed; verified prior backup retained");
    }

    if(promotionCopy.exists && currentRollback.exists &&
       promotionCopy.bytes!=currentRollback.bytes){
        if(!ops.copyFile(promote,rollback,FALSE))
            return SetFailure(errorOut,Win32Failure(
                "CopyFileW live rollback repair from promotion staging failed",
                LastErrorOr(ERROR_GEN_FAILURE)));
        if(!VerifyExactFile(rollback,promotionCopy.bytes,ops,&error))
            return SetFailure(errorOut,"live rollback repair verification failed: "+error);
        currentRollback.bytes=promotionCopy.bytes;
    }

    // Reconcile the prior transaction independently of the next requested
    // payload.  A separately verified promotion marker makes partial-effect
    // and post-move verification failures retryable without trusting .bak.
    if(currentRollback.exists){
        if(!promotionCopy.exists){
            if(!EnsurePromotionMarker(rollback,promote,currentRollback.bytes,ops,errorOut)) return false;
            promotionCopy.path=promote;
            promotionCopy.exists=true;
            promotionCopy.bytes=currentRollback.bytes;
        }
        if(!ops.moveFile(rollback,backup,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            return SetFailure(errorOut,Win32Failure("MoveFileExW pending rollback promotion failed",
                LastErrorOr(ERROR_ACCESS_DENIED)));
        bool repaired=false;
        if(!EnsureExactFromMarker(promote,backup,promotionCopy.bytes,ops,repaired,errorOut)) return false;
        currentBackup.path=backup;
        currentBackup.exists=true;
        currentBackup.bytes=promotionCopy.bytes;
        currentRollback.exists=false;
        currentRollback.bytes.clear();
        if(!deleteChecked(promote,"rollback-promotion staging")) return false;
        promotionCopy.exists=false;
        if(restoreCopy.exists && !deleteChecked(restore,"obsolete backup-restoration staging")) return false;
    } else if(promotionCopy.exists){
        bool repaired=false;
        if(!EnsureExactFromMarker(promote,backup,promotionCopy.bytes,ops,repaired,errorOut)) return false;
        currentBackup.path=backup;
        currentBackup.exists=true;
        currentBackup.bytes=promotionCopy.bytes;
        if(!deleteChecked(promote,"rollback-promotion staging")) return false;
        promotionCopy.exists=false;
        if(restoreCopy.exists && !deleteChecked(restore,"obsolete backup-restoration staging")) return false;
    } else if(!currentBackup.exists){
        if(!restoreCopy.exists){
            if(!ops.copyFile(previous,restore,TRUE))
                return SetFailure(errorOut,Win32Failure("CopyFileW backup-restoration staging failed",
                    LastErrorOr(ERROR_GEN_FAILURE)));
            if(!VerifyExactFile(restore,staged.bytes,ops,&error)){
                std::string mismatch=error;
                if(!deleteChecked(restore,"poisoned backup-restoration staging")) return false;
                return SetFailure(errorOut,"backup-restoration staging readback mismatch: "+mismatch+
                    "; verified prior backup retained");
            }
            restoreCopy.path=restore;
            restoreCopy.exists=true;
            restoreCopy.bytes=staged.bytes;
        }
        bool repaired=false;
        if(!EnsureExactFromMarker(restore,backup,staged.bytes,ops,repaired,errorOut)) return false;
        currentBackup.path=backup;
        currentBackup.exists=true;
        currentBackup.bytes=staged.bytes;
        if(!deleteChecked(restore,"backup-restoration staging")) return false;
    } else if(restoreCopy.exists){
        // A side-effecting CopyFile failure may have installed .bak while the
        // verified restore marker survived.  The marker defines exact intent.
        if(currentBackup.bytes!=staged.bytes){
            bool repaired=false;
            if(!EnsureExactFromMarker(restore,backup,staged.bytes,ops,repaired,errorOut)) return false;
            currentBackup.bytes=staged.bytes;
        }
        if(!deleteChecked(restore,"backup-restoration staging")) return false;
    }

    if(!currentBackup.exists)
        return SetFailure(errorOut,"cannot resolve staged backup without a canonical backup");
    if(!VerifyCaptured(currentPrimary,ops,&error) || !VerifyCaptured(currentBackup,ops,&error) ||
       !VerifyCaptured(staged,ops,&error))
        return SetFailure(errorOut,"cannot verify canonical state before staged-backup cleanup: "+error);
    if(orphanStageState==PathState::File){
        if(!deleteChecked(stagingTemp,"orphan prior-backup staging temp")) return false;
        if(!VerifyCaptured(currentPrimary,ops,&error) ||
           !VerifyCaptured(currentBackup,ops,&error) ||
           !VerifyCaptured(staged,ops,&error))
            return SetFailure(errorOut,
                "canonical state changed during prior-backup stage cleanup: "+error);
    }
    if(!deleteChecked(previous,"staged prior backup")) return false;
    if(!VerifyCaptured(currentPrimary,ops,&error) || !VerifyCaptured(currentBackup,ops,&error))
        return SetFailure(errorOut,"canonical state verification failed after staged-backup cleanup: "+error);
    return true;
}

inline bool StagePriorBackup(const std::wstring& backup,const std::wstring& previous,
        const OptionalBytes& oldBackup,const LayoutFsOps& ops,std::string* errorOut){
    if(!oldBackup.exists) return true;
    const std::wstring stage=previous+L".stage";
    std::string error;
    auto deleteChecked=[&](const std::wstring& path,const char* label)->bool{
        if(!ops.deleteFile(path))
            return SetFailure(errorOut,Win32Failure(std::string("DeleteFileW ")+label+" failed",
                LastErrorOr(ERROR_ACCESS_DENIED)));
        std::string queryError;
        PathState state=QueryPath(path,ops,&queryError);
        if(state==PathState::Unavailable)
            return SetFailure(errorOut,std::string("cannot verify ")+label+" cleanup: "+queryError);
        if(state!=PathState::Missing)
            return SetFailure(errorOut,std::string("DeleteFileW reported success but ")+label+" remains");
        return true;
    };
    PathState staleStageState=QueryPath(stage,ops,&error);
    if(staleStageState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot inspect prior-backup staging temp: "+error);
    if(staleStageState==PathState::File){
        if(!VerifyCaptured(oldBackup,ops,&error))
            return SetFailure(errorOut,"cannot verify backup before stale staging cleanup: "+error);
        if(!deleteChecked(stage,"stale prior-backup staging temp")) return false;
        if(!VerifyCaptured(oldBackup,ops,&error))
            return SetFailure(errorOut,"backup changed during stale staging cleanup: "+error);
    }
    if(!ops.copyFile(backup,stage,TRUE))
        return SetFailure(errorOut,Win32Failure("CopyFileW prior-backup staging temp failed",
            LastErrorOr(ERROR_GEN_FAILURE)));
    if(!VerifyExactFile(stage,oldBackup.bytes,ops,&error)){
        std::string mismatch=error;
        if(!deleteChecked(stage,"poisoned prior-backup staging temp")) return false;
        return SetFailure(errorOut,"prior-backup staging temp readback mismatch: "+mismatch);
    }
    if(!ops.copyFile(stage,previous,TRUE))
        return SetFailure(errorOut,Win32Failure("CopyFileW prior-backup staging publish failed",
            LastErrorOr(ERROR_GEN_FAILURE)));
    if(!VerifyExactFile(previous,oldBackup.bytes,ops,&error)){
        std::string mismatch=error;
        if(!deleteChecked(previous,"poisoned staged prior backup")) return false;
        return SetFailure(errorOut,"staged prior-backup verification failed: "+mismatch);
    }
    if(!deleteChecked(stage,"prior-backup staging temp")) return false;
    if(!VerifyExactFile(previous,oldBackup.bytes,ops,&error))
        return SetFailure(errorOut,"staged prior-backup changed after temp cleanup: "+error);
    return true;
}

inline bool CleanupStagedPriorBackup(const std::wstring& previous,const OptionalBytes& oldBackup,
        const LayoutFsOps& ops,std::string* errorOut){
    if(!oldBackup.exists) return true;
    if(!ops.deleteFile(previous))
        return SetFailure(errorOut,Win32Failure("DeleteFileW prior-backup staging cleanup failed",
            LastErrorOr(ERROR_ACCESS_DENIED)));
    std::string error;
    PathState previousState=QueryPath(previous,ops,&error);
    if(previousState==PathState::Unavailable)
        return SetFailure(errorOut,"cannot verify prior-backup staging cleanup: "+error);
    if(previousState!=PathState::Missing)
        return SetFailure(errorOut,"DeleteFileW reported success but staged prior backup remains");
    return true;
}

inline bool RestoreRollbackPromotion(const std::wstring& rollback,const std::wstring& backup,
        const std::wstring& previous,const OptionalBytes& oldRollback,const OptionalBytes& oldBackup,
        const LayoutFsOps& ops,const std::string& promotionError,std::string* errorOut){
    if(!ops.moveFile(backup,rollback,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return SetFailure(errorOut,promotionError+"; rollback-name restoration failed: "+
            Win32Failure("MoveFileExW",LastErrorOr(ERROR_ACCESS_DENIED)));
    // Copy rather than move so a failed restoration never consumes the last
    // verified copy held at .bak.previous.  The next transaction resolves it.
    if(oldBackup.exists && !ops.copyFile(previous,backup,TRUE))
        return SetFailure(errorOut,promotionError+"; prior backup-name restoration failed: "+
            Win32Failure("CopyFileW",LastErrorOr(ERROR_GEN_FAILURE)));
    std::string verifyError;
    if(!VerifyExactFile(rollback,oldRollback.bytes,ops,&verifyError))
        return SetFailure(errorOut,promotionError+"; restored rollback verification failed: "+verifyError);
    if(oldBackup.exists){
        if(!VerifyExactFile(backup,oldBackup.bytes,ops,&verifyError))
            return SetFailure(errorOut,promotionError+"; restored backup verification failed: "+verifyError);
    } else {
        PathState backupState=QueryPath(backup,ops,&verifyError);
        if(backupState!=PathState::Missing)
            return SetFailure(errorOut,promotionError+"; backup absence could not be restored");
    }
    if(oldBackup.exists){
        std::string cleanupError;
        if(!CleanupStagedPriorBackup(previous,oldBackup,ops,&cleanupError))
            return SetFailure(errorOut,promotionError+"; restored names but staged-backup cleanup failed: "+
                cleanupError);
    }
    return SetFailure(errorOut,promotionError+"; original rollback and backup names restored");
}

inline bool PromoteRollbackChecked(const std::wstring& rollback,const std::wstring& backup,
        const std::wstring& previous,const OptionalBytes& oldRollback,const OptionalBytes& oldBackup,
        const LayoutFsOps& ops,std::string* errorOut){
    const std::wstring promote=previous+L".promote";
    std::string error;
    auto deleteMarker=[&]()->bool{
        if(!ops.deleteFile(promote))
            return SetFailure(errorOut,Win32Failure("DeleteFileW rollback-promotion staging cleanup failed",
                LastErrorOr(ERROR_ACCESS_DENIED)));
        std::string queryError;
        PathState state=QueryPath(promote,ops,&queryError);
        if(state==PathState::Unavailable)
            return SetFailure(errorOut,"cannot verify rollback-promotion staging cleanup: "+queryError);
        if(state!=PathState::Missing)
            return SetFailure(errorOut,
                "DeleteFileW reported success but rollback-promotion staging remains");
        return true;
    };
    if(!EnsurePromotionMarker(rollback,promote,oldRollback.bytes,ops,errorOut)) return false;
    if(!ops.moveFile(rollback,backup,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return SetFailure(errorOut,Win32Failure("MoveFileExW prior rollback promotion failed",
            LastErrorOr(ERROR_ACCESS_DENIED)));
    if(!VerifyExactFile(backup,oldRollback.bytes,ops,&error))
        return RestoreRollbackPromotion(rollback,backup,previous,oldRollback,oldBackup,ops,
            "promoted rollback verification failed: "+error,errorOut);
    if(!deleteMarker()) return false;
    if(!VerifyExactFile(backup,oldRollback.bytes,ops,&error))
        return SetFailure(errorOut,"promoted rollback changed after staging cleanup: "+error);
    return true;
}

inline bool WriteTempFile(const std::wstring& tempPath,const std::string& data,
        const LayoutFsOps& ops,std::string* errorOut){
    HANDLE file=ops.openFile(tempPath,GENERIC_WRITE,FILE_SHARE_READ,CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH);
    if(file==INVALID_HANDLE_VALUE)
        return SetFailure(errorOut,Win32Failure("CreateFileW for temporary layout failed",LastErrorOr(ERROR_OPEN_FAILED)));
    size_t offset=0;
    while(offset<data.size()){
        DWORD requested=(DWORD)(std::min)(data.size()-offset,(size_t)(std::numeric_limits<DWORD>::max)());
        DWORD written=0;
        if(!ops.writeFile(file,data.data()+offset,requested,written)){
            DWORD error=LastErrorOr(ERROR_WRITE_FAULT);
            ops.closeHandle(file);
            return SetFailure(errorOut,Win32Failure("WriteFile failed",error));
        }
        if(written==0 || written>requested){
            ops.closeHandle(file);
            return SetFailure(errorOut,"WriteFile returned an invalid byte count");
        }
        offset+=written;
    }
    if(!ops.flushFile(file)){
        DWORD error=LastErrorOr(ERROR_WRITE_FAULT);
        ops.closeHandle(file);
        return SetFailure(errorOut,Win32Failure("FlushFileBuffers failed",error));
    }
    if(!ops.closeHandle(file))
        return SetFailure(errorOut,Win32Failure("CloseHandle for temporary layout failed",
            LastErrorOr(ERROR_INVALID_HANDLE)));
    return true;
}

enum class CandidateState { Missing, Valid, Corrupt, Unavailable };

struct LayoutCandidate {
    CandidateState state=CandidateState::Unavailable;
    std::wstring path;
    std::string bytes;
    int sourceVersion=0;
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    LayoutRevision revision;
    std::string error;
};

inline LayoutCandidate ReadCandidate(const std::wstring& path,UnixSeconds nowUtc,const LayoutFsOps& ops){
    LayoutCandidate candidate;
    candidate.path=path;
    candidate.revision.sourcePath=path;
    FileReadMetadata metadata;
    FileReadResult read=ReadFileBytesBoundedWithMetadata(path,MAX_LAYOUT_FILE_BYTES,ops,&metadata);
    if(read.status==FileReadStatus::Missing){
        candidate.state=CandidateState::Missing;
        candidate.revision.exists=false;
        return candidate;
    }
    if(read.status!=FileReadStatus::Ok){
        candidate.state=CandidateState::Unavailable;
        candidate.revision.exists=true;
        candidate.error=read.error.empty() ? "layout stream is unavailable" : read.error;
        return candidate;
    }
    candidate.bytes=read.bytes;
    candidate.revision=RevisionFromRead(path,metadata,candidate.bytes);
    std::string parseError;
    if(!ParseLayout(candidate.bytes,candidate.desks,candidate.wins,nowUtc,&parseError,&candidate.sourceVersion)){
        candidate.state=CandidateState::Corrupt;
        candidate.error=parseError.empty() ? "layout stream is corrupt" : parseError;
        candidate.desks.clear();
        candidate.wins.clear();
        candidate.sourceVersion=0;
        return candidate;
    }
    candidate.state=CandidateState::Valid;
    return candidate;
}

inline bool PreserveCorruptCandidate(const LayoutCandidate& candidate,UnixSeconds nowUtc,
        const LayoutFsOps& ops,std::string* errorOut){
    if(candidate.state!=CandidateState::Corrupt)
        return SetFailure(errorOut,"attempted to preserve a non-corrupt layout stream");
    const unsigned long long maxAttempts=100000ULL;
    for(unsigned long long counter=0;counter<maxAttempts;++counter){
        std::wstring diagnostic=candidate.path+L".corrupt."+std::to_wstring(nowUtc)+L"."+
            std::to_wstring(counter);
        if(!ops.copyFile(candidate.path,diagnostic,TRUE)){
            DWORD error=LastErrorOr(ERROR_GEN_FAILURE);
            if(error==ERROR_FILE_EXISTS || error==ERROR_ALREADY_EXISTS) continue;
            return SetFailure(errorOut,Win32Failure("CopyFileW diagnostic preservation failed",error));
        }
        std::string copied;
        LayoutRevision copiedRevision;
        std::string readError;
        if(!ReadExactBytes(diagnostic,ops,copied,&copiedRevision,&readError))
            return SetFailure(errorOut,"diagnostic copy readback failed: "+readError);
        if(copiedRevision.size!=candidate.revision.size ||
           copiedRevision.contentHash!=candidate.revision.contentHash || copied!=candidate.bytes)
            return SetFailure(errorOut,"diagnostic copy readback mismatch");
        return true;
    }
    return SetFailure(errorOut,"could not allocate a collision-free diagnostic filename");
}

inline LayoutLoadResult UnavailableLoad(const std::wstring& source,const std::string& error){
    LayoutLoadResult result;
    result.status=LayoutLoadStatus::Unavailable;
    result.writesAllowed=false;
    result.revision.sourcePath=source;
    result.error=error.empty() ? "layout storage is unavailable" : error;
    return result;
}

inline LayoutLoadResult ValidLoad(const LayoutCandidate& candidate,LayoutLoadStatus status){
    LayoutLoadResult result;
    result.status=status;
    result.writesAllowed=true;
    result.sourceVersion=candidate.sourceVersion;
    result.desks=candidate.desks;
    result.wins=candidate.wins;
    result.revision=candidate.revision;
    return result;
}

inline bool PreserveAll(const std::vector<const LayoutCandidate*>& corrupt,UnixSeconds nowUtc,
        const LayoutFsOps& ops,std::string* errorOut){
    for(const LayoutCandidate* candidate : corrupt)
        if(!PreserveCorruptCandidate(*candidate,nowUtc,ops,errorOut)) return false;
    return true;
}

inline bool NoUnresolvedTransactionArtifacts(const std::wstring& path,const LayoutFsOps& ops,
        std::string* errorOut,bool includeTemp=true){
    const std::wstring previous=path+L".bak.previous";
    const std::wstring artifacts[]={
        path+L".tmp",path+L".tmp.stage",path+L".displaced",previous,previous+L".stage",
        previous+L".restore",previous+L".promote",previous+L".promote.stage"
    };
    for(const std::wstring& artifact : artifacts){
        if(!includeTemp && (artifact==path+L".tmp" || artifact==path+L".tmp.stage")) continue;
        std::string queryError;
        PathState state=QueryPath(artifact,ops,&queryError);
        if(state==PathState::Unavailable)
            return SetFailure(errorOut,"cannot inspect internal recovery artifact "+W2U8(artifact)+
                ": "+queryError);
        if(state==PathState::File)
            return SetFailure(errorOut,"unresolved internal recovery artifact: "+W2U8(artifact));
    }
    return true;
}

inline bool TryRecoverSoleTemp(const std::wstring& path,UnixSeconds nowUtc,const LayoutFsOps& ops,
        bool& handled,LayoutLoadResult& result){
    handled=false;
    const std::wstring temp=path+L".tmp";
    const std::wstring stage=temp+L".stage";
    std::string stageError;
    PathState stageState=QueryPath(stage,ops,&stageError);
    if(stageState==PathState::Unavailable){
        handled=true;
        result=UnavailableLoad(stage,"temporary write stage unavailable: "+stageError);
        return true;
    }
    LayoutCandidate candidate=ReadCandidate(temp,nowUtc,ops);
    if(candidate.state==CandidateState::Missing){
        if(stageState==PathState::Missing) return true;
        handled=true;
        if(!DeleteArtifactChecked(stage,"uncommitted temporary write stage",ops,&stageError)){
            result=UnavailableLoad(stage,stageError);
            return true;
        }
        result.status=LayoutLoadStatus::Missing;
        result.writesAllowed=true;
        result.revision.sourcePath=path;
        result.revision.exists=false;
        return true;
    }
    handled=true;
    if(candidate.state==CandidateState::Unavailable){
        result=UnavailableLoad(temp,"temporary layout unavailable: "+candidate.error);
        return true;
    }
    if(stageState==PathState::File){
        std::string verifyError;
        if(!VerifyExactFile(temp,candidate.bytes,ops,&verifyError) ||
           !DeleteArtifactChecked(stage,"obsolete temporary write stage",ops,&verifyError)){
            result=UnavailableLoad(temp,"cannot reconcile temporary write stage: "+verifyError);
            return true;
        }
    }
    if(candidate.state==CandidateState::Corrupt){
        std::string error;
        if(!PreserveCorruptCandidate(candidate,nowUtc,ops,&error) ||
           !DeleteArtifactChecked(temp,"preserved corrupt temporary layout",ops,&error)){
            result=UnavailableLoad(temp,error);
            return true;
        }
        result.status=LayoutLoadStatus::CorruptPreserved;
        result.writesAllowed=true;
        result.revision=candidate.revision;
        result.error="corrupt temporary layout was preserved diagnostically";
        return true;
    }
    std::string verifyError;
    if(!VerifyExactFile(temp,candidate.bytes,ops,&verifyError)){
        result=UnavailableLoad(temp,"temporary layout changed before recovery: "+verifyError);
        return true;
    }
    if(!ops.moveFile(temp,path,MOVEFILE_WRITE_THROUGH)){
        result=UnavailableLoad(temp,Win32Failure("MoveFileExW temporary layout recovery failed",
            LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
        return true;
    }
    LayoutCandidate promoted=ReadCandidate(path,nowUtc,ops);
    if(promoted.state!=CandidateState::Valid || promoted.bytes!=candidate.bytes){
        result=UnavailableLoad(path,"promoted temporary layout verification failed");
        return true;
    }
    std::string queryError;
    PathState tempState=QueryPath(temp,ops,&queryError);
    if(tempState==PathState::Unavailable){
        result=UnavailableLoad(temp,"cannot verify temporary layout promotion: "+queryError);
        return true;
    }
    if(tempState==PathState::File &&
       !DeleteArtifactChecked(temp,"duplicate temporary layout after promotion",ops,&queryError)){
        result=UnavailableLoad(temp,queryError);
        return true;
    }
    if(!VerifyExactFile(path,candidate.bytes,ops,&queryError)){
        result=UnavailableLoad(path,"temporary recovery survivor verification failed: "+queryError);
        return true;
    }
    result=ValidLoad(promoted,LayoutLoadStatus::Recovered);
    result.error="layout recovered from a flushed first-publish temporary file";
    return true;
}

} // namespace layout_store_detail

inline LayoutRevision ReadLayoutRevisionLocked(const std::wstring& path,const LayoutFsOps& ops){
    LayoutRevision revision;
    revision.sourcePath=path;
    FileReadMetadata metadata;
    FileReadResult read=ReadFileBytesBoundedWithMetadata(path,MAX_LAYOUT_FILE_BYTES,ops,&metadata);
    if(read.status==FileReadStatus::Ok) return layout_store_detail::RevisionFromRead(path,metadata,read.bytes);
    revision.exists=read.status!=FileReadStatus::Missing;
    return revision;
}

inline LayoutRevision ReadLayoutRevisionLocked(const std::wstring& path){
    LayoutFsOps ops;
    return ReadLayoutRevisionLocked(path,ops);
}

inline bool AtomicWriteText(const std::wstring& path,const std::string& data,std::string* errorOut,
        bool preserveExistingBackup,const LayoutFsOps& ops){
    using namespace layout_store_detail;
    if((unsigned long long)data.size()>MAX_LAYOUT_FILE_BYTES)
        return SetFailure(errorOut,"layout data exceeds the 16 MiB limit");

    const std::wstring temp=path+L".tmp";
    const std::wstring tempStage=temp+L".stage";
    const std::wstring rollback=path+L".rollback";
    const std::wstring backup=path+L".bak";
    const std::wstring previousBackup=backup+L".previous";
    const std::wstring promotionMarker=previousBackup+L".promote";
    const std::wstring displaced=path+L".displaced";
    std::string queryError;
    PathState primaryState=QueryPath(path,ops,&queryError);
    if(primaryState==PathState::Unavailable) return SetFailure(errorOut,queryError);

    std::string priorPrimary;
    OptionalBytes currentPrimary;
    currentPrimary.path=path;
    OptionalBytes preservedBackup, preservedRollback;
    bool requestAlreadyPublished=false;
    OptionalBytes displacedCleanupPending;
    if(primaryState==PathState::File){
        LayoutRevision ignored;
        if(!ReadExactBytes(path,ops,priorPrimary,&ignored,&queryError))
            return SetFailure(errorOut,"cannot read current primary before replacement: "+queryError);
        currentPrimary.exists=true;
        currentPrimary.bytes=priorPrimary;
    }

    if(!ResolveStaleBakPrevious(currentPrimary,rollback,backup,previousBackup,ops,&queryError))
        return SetFailure(errorOut,queryError);

    bool hasCanonicalRecovery=false;
    if(!currentPrimary.exists){
        PathState rollbackState=QueryPath(rollback,ops,&queryError);
        if(rollbackState==PathState::Unavailable) return SetFailure(errorOut,queryError);
        PathState backupState=QueryPath(backup,ops,&queryError);
        if(backupState==PathState::Unavailable) return SetFailure(errorOut,queryError);
        hasCanonicalRecovery=rollbackState==PathState::File || backupState==PathState::File;
    }
    if(!ResolveWriteTempArtifacts(path,currentPrimary,!hasCanonicalRecovery,ops,&queryError))
        return SetFailure(errorOut,queryError);
    if(currentPrimary.exists){
        primaryState=PathState::File;
        priorPrimary=currentPrimary.bytes;
    }

    if(!preserveExistingBackup){
        if(primaryState==PathState::File){
            if(!ResolveRedundantDisplaced(path,displaced,priorPrimary,ops,&queryError))
                return SetFailure(errorOut,queryError);
        } else {
            if(!CaptureOptional(backup,ops,preservedBackup,&queryError))
                return SetFailure(errorOut,"cannot inspect backup beside missing primary: "+queryError);
            if(!CaptureOptional(rollback,ops,preservedRollback,&queryError))
                return SetFailure(errorOut,"cannot inspect rollback beside missing primary: "+queryError);
            if(!ResolveDisplacedWithoutPrimary(displaced,preservedBackup,preservedRollback,ops,
                    displacedCleanupPending,&queryError))
                return SetFailure(errorOut,queryError);
        }
    } else {
        if(!CaptureOptional(backup,ops,preservedBackup,&queryError))
            return SetFailure(errorOut,"cannot preserve existing backup: "+queryError);
        if(!CaptureOptional(rollback,ops,preservedRollback,&queryError))
            return SetFailure(errorOut,"cannot preserve existing rollback: "+queryError);
        if(primaryState==PathState::File){
            if(!ResolveDisplacedForRecovery(path,displaced,priorPrimary,data,preservedBackup,
                    preservedRollback,ops,requestAlreadyPublished,&queryError))
                return SetFailure(errorOut,queryError);
            if(requestAlreadyPublished){
                if(errorOut) errorOut->clear();
                return true;
            }
        } else {
            if(!ResolveDisplacedWithoutPrimary(displaced,preservedBackup,preservedRollback,ops,
                    displacedCleanupPending,&queryError))
                return SetFailure(errorOut,queryError);
        }
    }

    OptionalBytes oldRollback,oldBackup;
    if(!preserveExistingBackup && primaryState==PathState::File){
        if(!CaptureOptional(rollback,ops,oldRollback,&queryError))
            return SetFailure(errorOut,"cannot inspect prior rollback: "+queryError);
        if(!CaptureOptional(backup,ops,oldBackup,&queryError))
            return SetFailure(errorOut,"cannot inspect existing backup before replacement: "+queryError);
    }

    if(primaryState==PathState::File && priorPrimary==data){
        if(preserveExistingBackup){
            if(!VerifyExactFile(path,data,ops,&queryError) ||
               !VerifyCaptured(preservedBackup,ops,&queryError) ||
               !VerifyCaptured(preservedRollback,ops,&queryError))
                return SetFailure(errorOut,"idempotent recovery verification failed: "+queryError);
        } else if(oldRollback.exists){
            // ReplaceFile already published these bytes on a prior attempt,
            // but rollback-to-backup promotion did not complete.  Finish that
            // pending phase directly; replacing data with itself would rotate
            // the new bytes into .bak and lose the real prior primary.
            if(!StagePriorBackup(backup,previousBackup,oldBackup,ops,&queryError))
                return SetFailure(errorOut,queryError);
            if(!PromoteRollbackChecked(rollback,backup,previousBackup,oldRollback,oldBackup,
                    ops,&queryError))
                return SetFailure(errorOut,queryError);
            if(!VerifyExactFile(path,data,ops,&queryError) ||
               !VerifyExactFile(backup,oldRollback.bytes,ops,&queryError))
                return SetFailure(errorOut,"idempotent promotion verification failed: "+queryError);
            if(!CleanupStagedPriorBackup(previousBackup,oldBackup,ops,&queryError))
                return SetFailure(errorOut,queryError);
        } else if(!VerifyExactFile(path,data,ops,&queryError) ||
                  !VerifyCaptured(oldBackup,ops,&queryError)){
            return SetFailure(errorOut,"idempotent write verification failed: "+queryError);
        }
        if(errorOut) errorOut->clear();
        return true;
    }

    const std::wstring& writePath=primaryState==PathState::Missing ? tempStage : temp;
    if(!WriteTempFile(writePath,data,ops,errorOut)) return false;

    if(primaryState==PathState::Missing){
        if(!ops.moveFile(tempStage,temp,MOVEFILE_WRITE_THROUGH))
            return SetFailure(errorOut,Win32Failure("MoveFileExW temporary commit failed",
                LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
        if(!VerifyExactFile(temp,data,ops,&queryError))
            return SetFailure(errorOut,"committed temporary verification failed: "+queryError);
    }

    if(primaryState==PathState::Missing){
        if(!ops.moveFile(temp,path,MOVEFILE_WRITE_THROUGH))
            return SetFailure(errorOut,Win32Failure("MoveFileExW first layout publish failed",
                LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT)));
        if(!VerifyExactFile(path,data,ops,&queryError)) return SetFailure(errorOut,queryError);
        PathState committedState=QueryPath(temp,ops,&queryError);
        if(committedState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot verify committed temporary consumption: "+queryError);
        if(committedState!=PathState::Missing)
            return SetFailure(errorOut,"MoveFileExW reported success but committed temporary remains");
        if(!VerifyCaptured(preservedBackup,ops,&queryError) ||
           !VerifyCaptured(preservedRollback,ops,&queryError))
            return SetFailure(errorOut,queryError);
        if(displacedCleanupPending.exists){
            if(!VerifyExactFile(path,data,ops,&queryError) ||
               !VerifyCaptured(displacedCleanupPending,ops,&queryError) ||
               !VerifyCaptured(preservedBackup,ops,&queryError) ||
               !VerifyCaptured(preservedRollback,ops,&queryError))
                return SetFailure(errorOut,
                    "survivor verification failed before displaced cleanup: "+queryError);
            if(!DeleteArtifactChecked(displaced,"displaced artifact after first publish",ops,errorOut))
                return false;
            if(!VerifyExactFile(path,data,ops,&queryError) ||
               !VerifyCaptured(preservedBackup,ops,&queryError) ||
               !VerifyCaptured(preservedRollback,ops,&queryError))
                return SetFailure(errorOut,"survivor verification failed after displaced cleanup: "+queryError);
        }
        if(errorOut) errorOut->clear();
        return true;
    }

    if(preserveExistingBackup){
        if(!ops.replaceFile(path,temp,displaced,REPLACEFILE_WRITE_THROUGH)){
            DWORD replaceError=LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
            std::string recovery=DescribeReplaceFailureRecovery(path,displaced,priorPrimary,ops);
            std::string preservedError;
            bool survivors=VerifyCaptured(preservedBackup,ops,&preservedError) &&
                VerifyCaptured(preservedRollback,ops,&preservedError);
            if(!survivors) recovery+="; preserved recovery verification failed: "+preservedError;
            return SetFailure(errorOut,Win32Failure("ReplaceFileW recovery publish failed",replaceError)+
                "; "+recovery);
        }
        if(!VerifyExactFile(path,data,ops,&queryError)) return SetFailure(errorOut,queryError);
        if(!VerifyCaptured(preservedBackup,ops,&queryError) ||
           !VerifyCaptured(preservedRollback,ops,&queryError))
            return SetFailure(errorOut,queryError);
        if(!ops.deleteFile(displaced))
            return SetFailure(errorOut,Win32Failure("DeleteFileW displaced cleanup failed",
                LastErrorOr(ERROR_ACCESS_DENIED)));
        PathState displacedState=QueryPath(displaced,ops,&queryError);
        if(displacedState==PathState::Unavailable)
            return SetFailure(errorOut,"cannot verify displaced cleanup: "+queryError);
        if(displacedState!=PathState::Missing)
            return SetFailure(errorOut,"DeleteFileW reported success but displaced artifact remains");
        if(!VerifyExactFile(path,data,ops,&queryError) ||
           !VerifyCaptured(preservedBackup,ops,&queryError) ||
           !VerifyCaptured(preservedRollback,ops,&queryError))
            return SetFailure(errorOut,"survivor verification failed after displaced cleanup: "+queryError);
        if(errorOut) errorOut->clear();
        return true;
    }

    if(!StagePriorBackup(backup,previousBackup,oldBackup,ops,&queryError))
        return SetFailure(errorOut,queryError);
    if(oldRollback.exists &&
       !PromoteRollbackChecked(rollback,backup,previousBackup,oldRollback,oldBackup,ops,&queryError))
        return SetFailure(errorOut,queryError);
    if(!ops.replaceFile(path,temp,rollback,REPLACEFILE_WRITE_THROUGH)){
        DWORD replaceError=LastErrorOr(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
        std::string recovery=DescribeReplaceFailureRecovery(path,rollback,priorPrimary,ops);
        return SetFailure(errorOut,Win32Failure("ReplaceFileW layout publish failed",replaceError)+
            "; "+recovery);
    }
    if(!EnsurePromotionMarker(rollback,promotionMarker,priorPrimary,ops,errorOut)) return false;
    if(!ops.moveFile(rollback,backup,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return SetFailure(errorOut,Win32Failure("MoveFileExW rollback-to-backup promotion failed",
            LastErrorOr(ERROR_ACCESS_DENIED)));
    if(!VerifyExactFile(path,data,ops,&queryError)) return SetFailure(errorOut,queryError);
    if(!VerifyExactFile(backup,priorPrimary,ops,&queryError)) return SetFailure(errorOut,queryError);
    if(!DeleteArtifactChecked(promotionMarker,"rollback-promotion marker",ops,errorOut)) return false;
    if(!VerifyExactFile(path,data,ops,&queryError) ||
       !VerifyExactFile(backup,priorPrimary,ops,&queryError))
        return SetFailure(errorOut,"survivor verification failed after promotion staging cleanup: "+queryError);
    if(!CleanupStagedPriorBackup(previousBackup,oldBackup,ops,&queryError))
        return SetFailure(errorOut,queryError);
    if(errorOut) errorOut->clear();
    return true;
}

inline bool AtomicWriteText(const std::wstring& path,const std::string& data,
        std::string* errorOut=nullptr,bool preserveExistingBackup=false){
    LayoutFsOps ops;
    return AtomicWriteText(path,data,errorOut,preserveExistingBackup,ops);
}

inline LayoutLoadResult LoadLayoutWithBackupLocked(const std::wstring& path,UnixSeconds nowUtc,
        const LayoutFsOps& ops){
    using namespace layout_store_detail;
    LayoutCandidate primary=ReadCandidate(path,nowUtc,ops);
    const std::wstring promotionPath=path+L".bak.previous.promote";
    const std::wstring promotionStagePath=promotionPath+L".stage";
    const std::wstring previousBackupPath=path+L".bak.previous";
    if(primary.state==CandidateState::Valid){
        std::string displacedError;
        if(!ResolveRedundantDisplaced(path,path+L".displaced",primary.bytes,ops,&displacedError))
            return UnavailableLoad(path,
                "cannot reconcile displaced artifact beside valid primary: "+displacedError);
        // A committed promotion marker represents an unfinished transaction,
        // even though the new primary is already parse-valid.  Settle it
        // before advertising a writable Valid state so the prior generation
        // cannot remain stranded at rollback/marker paths.
        LayoutCandidate pendingPromotion=ReadCandidate(promotionPath,nowUtc,ops);
        if(pendingPromotion.state==CandidateState::Unavailable)
            return UnavailableLoad(promotionPath,
                "rollback-promotion marker unavailable: "+pendingPromotion.error);
        if(pendingPromotion.state==CandidateState::Corrupt)
            return UnavailableLoad(promotionPath,
                "authoritative rollback-promotion marker is corrupt: "+pendingPromotion.error);
        std::string artifactError;
        bool hasResolverArtifact=pendingPromotion.state==CandidateState::Valid;
        std::vector<std::wstring> resolverArtifacts;
        resolverArtifacts.push_back(promotionStagePath);
        resolverArtifacts.push_back(previousBackupPath);
        resolverArtifacts.push_back(previousBackupPath+L".restore");
        resolverArtifacts.push_back(previousBackupPath+L".stage");
        for(const std::wstring& artifact:resolverArtifacts){
            PathState state=QueryPath(artifact,ops,&artifactError);
            if(state==PathState::Unavailable)
                return UnavailableLoad(artifact,
                    "layout transaction artifact unavailable: "+artifactError);
            if(state==PathState::File) hasResolverArtifact=true;
        }
        if(hasResolverArtifact){
            OptionalBytes currentPrimary;
            currentPrimary.path=path;
            currentPrimary.exists=true;
            currentPrimary.bytes=primary.bytes;
            if(!ResolveStaleBakPrevious(currentPrimary,path+L".rollback",path+L".bak",
                    previousBackupPath,ops,&artifactError))
                return UnavailableLoad(path,"cannot reconcile layout transaction artifacts: "+artifactError);
            LayoutCandidate settled=ReadCandidate(path,nowUtc,ops);
            if(settled.state!=CandidateState::Valid || settled.bytes!=primary.bytes)
                return UnavailableLoad(path,
                    "primary layout changed while transaction artifacts were reconciled");
            return ValidLoad(settled,LayoutLoadStatus::Valid);
        }
        return ValidLoad(primary,LayoutLoadStatus::Valid);
    }
    if(primary.state==CandidateState::Unavailable)
        return UnavailableLoad(path,"primary layout unavailable: "+primary.error);

    std::vector<const LayoutCandidate*> corrupt;
    if(primary.state==CandidateState::Corrupt) corrupt.push_back(&primary);

    LayoutCandidate promotion=ReadCandidate(promotionPath,nowUtc,ops);
    if(promotion.state==CandidateState::Unavailable)
        return UnavailableLoad(promotionPath,"rollback-promotion marker unavailable: "+promotion.error);
    if(promotion.state==CandidateState::Corrupt){
        corrupt.push_back(&promotion);
        std::string preserveError;
        if(!PreserveAll(corrupt,nowUtc,ops,&preserveError))
            return UnavailableLoad(promotionPath,preserveError);
        return UnavailableLoad(promotionPath,"authoritative rollback-promotion marker is corrupt");
    }
    const bool promotionActive=promotion.state==CandidateState::Valid;

    if(primary.state==CandidateState::Missing && !promotionActive){
        const std::wstring displacedPath=path+L".displaced";
        std::string displacedError;
        PathState displacedState=QueryPath(displacedPath,ops,&displacedError);
        if(displacedState==PathState::Unavailable)
            return UnavailableLoad(displacedPath,
                "cannot inspect displaced recovery artifact: "+displacedError);
        if(displacedState==PathState::File){
            PathState rollbackState=QueryPath(path+L".rollback",ops,&displacedError);
            if(rollbackState==PathState::Unavailable)
                return UnavailableLoad(path+L".rollback",displacedError);
            PathState backupState=QueryPath(path+L".bak",ops,&displacedError);
            if(backupState==PathState::Unavailable)
                return UnavailableLoad(path+L".bak",displacedError);
            if(rollbackState==PathState::Missing && backupState==PathState::Missing){
                OptionalBytes missingBackup,missingRollback,pendingCleanup;
                missingBackup.path=path+L".bak";
                missingRollback.path=path+L".rollback";
                if(!ResolveDisplacedWithoutPrimary(displacedPath,missingBackup,missingRollback,
                        ops,pendingCleanup,&displacedError))
                    return UnavailableLoad(displacedPath,
                        "cannot canonicalize sole displaced recovery: "+displacedError);
            }
        }
    }

    LayoutCandidate rollback=ReadCandidate(path+L".rollback",nowUtc,ops);
    if(rollback.state==CandidateState::Unavailable)
        return UnavailableLoad(rollback.path,"rollback layout unavailable: "+rollback.error);
    if(rollback.state==CandidateState::Valid &&
       (!promotionActive || rollback.bytes==promotion.bytes)){
        std::string preserveError;
        if(!PreserveAll(corrupt,nowUtc,ops,&preserveError)) return UnavailableLoad(path,preserveError);
        LayoutLoadResult result=ValidLoad(rollback,LayoutLoadStatus::Recovered);
        result.error=promotionActive ?
            "layout recovered from rollback verified by promotion marker" :
            "layout recovered from rollback";
        return result;
    }
    if(rollback.state==CandidateState::Corrupt) corrupt.push_back(&rollback);

    LayoutCandidate backup=ReadCandidate(path+L".bak",nowUtc,ops);
    if(backup.state==CandidateState::Unavailable)
        return UnavailableLoad(backup.path,"backup layout unavailable: "+backup.error);
    if(backup.state==CandidateState::Valid &&
       (!promotionActive || backup.bytes==promotion.bytes)){
        std::string preserveError;
        if(!PreserveAll(corrupt,nowUtc,ops,&preserveError)) return UnavailableLoad(path,preserveError);
        LayoutLoadResult result=ValidLoad(backup,LayoutLoadStatus::Recovered);
        result.error=promotionActive ?
            "layout recovered from backup verified by promotion marker" :
            "layout recovered from backup";
        return result;
    }
    if(backup.state==CandidateState::Corrupt) corrupt.push_back(&backup);

    if(promotionActive){
        std::string preserveError;
        if(!PreserveAll(corrupt,nowUtc,ops,&preserveError))
            return UnavailableLoad(path,preserveError);
        bool repaired=false;
        std::string repairError;
        if(!EnsureExactFromMarker(promotion.path,backup.path,promotion.bytes,ops,repaired,&repairError))
            return UnavailableLoad(backup.path,"cannot reconcile authoritative promotion marker: "+repairError);
        backup=ReadCandidate(backup.path,nowUtc,ops);
        if(backup.state!=CandidateState::Valid || backup.bytes!=promotion.bytes)
            return UnavailableLoad(backup.path,"reconciled promotion backup is not a valid exact layout");
        LayoutLoadResult result=ValidLoad(backup,LayoutLoadStatus::Recovered);
        result.error="layout recovered by reconciling the rollback-promotion marker";
        return result;
    }

    std::string preserveError;
    if(!corrupt.empty() && !PreserveAll(corrupt,nowUtc,ops,&preserveError))
        return UnavailableLoad(path,preserveError);
    std::string transactionError;
    if(!NoUnresolvedTransactionArtifacts(path,ops,&transactionError,false))
        return UnavailableLoad(path,transactionError);
    if(corrupt.empty()){
        bool tempHandled=false;
        LayoutLoadResult tempResult;
        if(!TryRecoverSoleTemp(path,nowUtc,ops,tempHandled,tempResult))
            return UnavailableLoad(path,"temporary layout recovery failed");
        if(tempHandled) return tempResult;
        LayoutLoadResult result;
        result.status=LayoutLoadStatus::Missing;
        result.writesAllowed=true;
        result.revision.sourcePath=path;
        result.revision.exists=false;
        return result;
    }
    OptionalBytes noPrimary;
    noPrimary.path=path;
    if(!ResolveWriteTempArtifacts(path,noPrimary,false,ops,&transactionError))
        return UnavailableLoad(path,transactionError);
    LayoutLoadResult result;
    result.status=LayoutLoadStatus::CorruptPreserved;
    result.writesAllowed=true;
    result.revision=corrupt.front()->revision;
    result.error="corrupt layout streams were preserved diagnostically";
    return result;
}

inline LayoutLoadResult LoadLayoutWithBackupLocked(const std::wstring& path,UnixSeconds nowUtc){
    LayoutFsOps ops;
    return LoadLayoutWithBackupLocked(path,nowUtc,ops);
}

inline LayoutLoadResult LoadLayoutWithBackup(const std::wstring& path,UnixSeconds nowUtc,
        DWORD lockTimeoutMs=0){
    ScopedLayoutLock lock(lockTimeoutMs);
    if(!lock.acquired()) return layout_store_detail::UnavailableLoad(path,"layout mutex is busy or unavailable");
    return LoadLayoutWithBackupLocked(path,nowUtc);
}
