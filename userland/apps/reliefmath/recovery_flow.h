#ifndef RELIEFMATH_RECOVERY_FLOW_H
#define RELIEFMATH_RECOVERY_FLOW_H
#include <stdint.h>

namespace rm {
/** Tracks one chosen recovery snapshot across dirty-document save decisions. */
class RecoveryFlow {
public:
    enum class SaveResult { Succeeded, SaveAsRetry, OverwriteRetry, Failed, Cancelled };
    enum class SaveFollowup { None, LoadRecovery, ContinueCommand, UpdateDirtySnapshot, ClearSnapshots };
    struct CleanupTarget {
        enum class Kind { None, Recovered, Own };
        Kind kind=Kind::None;
        uint64_t id=0;
    };

private:
    enum class Stage { Idle, Selected, WaitingForSave, Loading };
    Stage stage_=Stage::Idle;
    uint64_t id_=0;

    void clear() { stage_=Stage::Idle; id_=0; }

public:
    bool select(uint64_t id) {
        if (!id || stage_!=Stage::Idle) return false;
        id_=id;
        stage_=Stage::Selected;
        return true;
    }

    bool wait_for_save() {
        if (stage_!=Stage::Selected) return false;
        stage_=Stage::WaitingForSave;
        return true;
    }

    bool waiting_for_save() const { return stage_==Stage::WaitingForSave; }

    uint64_t begin_load() {
        if (stage_!=Stage::Selected) return 0;
        stage_=Stage::Loading;
        return id_;
    }

    uint64_t save_result(SaveResult result, bool document_dirty=false) {
        if (stage_!=Stage::WaitingForSave) return 0;
        switch (result) {
        case SaveResult::Succeeded:
            if (document_dirty) return 0;
            stage_=Stage::Loading;
            return id_;
        case SaveResult::SaveAsRetry:
        case SaveResult::OverwriteRetry:
            return 0;
        case SaveResult::Failed:
        case SaveResult::Cancelled:
            clear();
            return 0;
        }
        clear();
        return 0;
    }

    uint64_t accept_after_dirty_check() {
        if (stage_!=Stage::WaitingForSave) return 0;
        stage_=Stage::Loading;
        return id_;
    }

    uint64_t finish_load() {
        if (stage_!=Stage::Loading) return 0;
        uint64_t id=id_;
        clear();
        return id;
    }

    void load_start_failed() {
        if (stage_==Stage::Loading) clear();
    }

    void abandon() { clear(); }

    static SaveFollowup after_save(bool recovery_load, bool continuation,
                                   bool dirty, bool has_snapshots) {
        if (recovery_load) return SaveFollowup::LoadRecovery;
        if (continuation) return SaveFollowup::ContinueCommand;
        if (dirty) return SaveFollowup::UpdateDirtySnapshot;
        if (has_snapshots) return SaveFollowup::ClearSnapshots;
        return SaveFollowup::None;
    }

    static CleanupTarget next_cleanup_target(uint64_t recovered_id, bool has_own) {
        if (recovered_id) return {CleanupTarget::Kind::Recovered,recovered_id};
        if (has_own) return {CleanupTarget::Kind::Own,0};
        return {};
    }
};
}

#endif
