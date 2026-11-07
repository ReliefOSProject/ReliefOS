#include "recovery_flow.h"
#include <assert.h>
#include <stdint.h>

using rm::RecoveryFlow;

static void saved_recovery_waits_through_conflict_retries() {
    const uint64_t selected_id = 0x51a7;
    const RecoveryFlow::SaveResult retry_choices[]={
        RecoveryFlow::SaveResult::SaveAsRetry,
        RecoveryFlow::SaveResult::OverwriteRetry
    };

    for (RecoveryFlow::SaveResult retry : retry_choices) {
        RecoveryFlow flow;
        assert(flow.select(selected_id));
        assert(flow.wait_for_save());
        assert(!flow.select(selected_id+1));
        assert(flow.save_result(retry) == 0);
        assert(flow.save_result(RecoveryFlow::SaveResult::Succeeded) == selected_id);
        assert(flow.finish_load() == selected_id);
        assert(flow.finish_load() == 0);
    }
}

static void edits_after_the_save_snapshot_do_not_get_replaced_by_recovery() {
    RecoveryFlow flow;
    assert(flow.select(707));
    assert(flow.wait_for_save());
    assert(flow.save_result(RecoveryFlow::SaveResult::Succeeded,true)==0);
    assert(flow.waiting_for_save());
    assert(flow.save_result(RecoveryFlow::SaveResult::Succeeded,false)==707);
    assert(flow.finish_load()==707);

    RecoveryFlow discard_flow;
    assert(discard_flow.select(808));
    assert(discard_flow.wait_for_save());
    assert(discard_flow.save_result(RecoveryFlow::SaveResult::Succeeded,true)==0);
    assert(discard_flow.accept_after_dirty_check()==808);
    assert(discard_flow.finish_load()==808);
}

static void failed_or_cancelled_save_cannot_recover_a_stale_id() {
    const RecoveryFlow::SaveResult abort_results[]={
        RecoveryFlow::SaveResult::Failed,
        RecoveryFlow::SaveResult::Cancelled
    };

    for (RecoveryFlow::SaveResult result : abort_results) {
        RecoveryFlow flow;
        assert(flow.select(101));
        assert(flow.wait_for_save());
        assert(flow.save_result(result) == 0);
        assert(flow.begin_load() == 0);

        assert(flow.select(202));
        assert(flow.begin_load() == 202);
        assert(flow.finish_load() == 202);
    }
}

static void save_as_cancel_and_load_enqueue_failure_clear_only_pending_choice() {
    RecoveryFlow flow;
    assert(flow.select(303));
    flow.abandon();
    assert(flow.begin_load() == 0);

    assert(flow.select(404));
    assert(flow.begin_load() == 404);
    flow.load_start_failed();
    assert(flow.finish_load() == 0);

    assert(flow.select(505));
    assert(flow.begin_load() == 505);
    assert(flow.finish_load() == 505);
}

static void save_completion_does_not_clear_dirty_snapshots_or_race_a_continuation() {
    using Followup=RecoveryFlow::SaveFollowup;
    assert(RecoveryFlow::after_save(false,false,false,true)==Followup::ClearSnapshots);
    assert(RecoveryFlow::after_save(false,false,true,true)==Followup::UpdateDirtySnapshot);
    assert(RecoveryFlow::after_save(false,true,true,true)==Followup::ContinueCommand);
    assert(RecoveryFlow::after_save(true,true,true,true)==Followup::LoadRecovery);
    assert(RecoveryFlow::after_save(false,false,false,false)==Followup::None);
}

static void clean_save_cleanup_selects_only_recovered_then_own_snapshot() {
    using Kind=RecoveryFlow::CleanupTarget::Kind;
    RecoveryFlow::CleanupTarget target=RecoveryFlow::next_cleanup_target(606,true);
    assert(target.kind==Kind::Recovered&&target.id==606);

    target=RecoveryFlow::next_cleanup_target(0,true);
    assert(target.kind==Kind::Own&&target.id==0);

    target=RecoveryFlow::next_cleanup_target(0,false);
    assert(target.kind==Kind::None&&target.id==0);
}

int main() {
    saved_recovery_waits_through_conflict_retries();
    edits_after_the_save_snapshot_do_not_get_replaced_by_recovery();
    failed_or_cancelled_save_cannot_recover_a_stale_id();
    save_as_cancel_and_load_enqueue_failure_clear_only_pending_choice();
    save_completion_does_not_clear_dirty_snapshots_or_race_a_continuation();
    clean_save_cleanup_selects_only_recovered_then_own_snapshot();
}
