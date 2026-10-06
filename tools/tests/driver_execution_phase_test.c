#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <reliefnt/audio.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>

static __thread uint32_t fixture_cpu;
static __thread uint64_t fixture_irq_flags = 1ULL << 9;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static uint32_t prepare_entered;
static uint32_t prepare_finished;
static uint32_t manager_entered;
static uint32_t manager_wait_irq_enabled;
static uint32_t manager_resume_irq_disabled;
static uint32_t manager_exit_irq_enabled;
static uint32_t worker_attempted;
static uint32_t task_progress;
static uint32_t init_entered;
static uint32_t init_completed;
static uint32_t abort_prepare;
static uint32_t fixture_stream;
static uint32_t legacy_pin_entered;
static uint32_t legacy_callback_release;
static uint32_t legacy_callback_completed;
static uint32_t fault_mode;

uint32_t smp_current_cpu(void) { return fixture_cpu; }
void smp_membarrier_poll(void) { }

uint64_t kernel_irq_save(void)
{
    uint64_t saved = fixture_irq_flags;
    fixture_irq_flags &= ~(1ULL << 9);
    return saved;
}

void kernel_irq_restore(uint64_t flags)
{
    fixture_irq_flags = flags & (1ULL << 9);
}

#include "../../kernel/reliefnt/kernel/reliefnt/lock.c"
#include "../../kernel/reliefnt/kernel/reliefnt/driver_manager_phase.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/core.c"
#include "../../kernel/reliefnt/kernel/reliefnt/audio/pcm.c"

/* The phase fixture links the PCM core without the ALSA timer module. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

int time_clock_get(int32_t clock, struct linux_timespec *value)
{
    *value=(struct linux_timespec){.tv_sec=100+clock};return 0;
}

/* This pthread fixture has no sched task waiters; wakeup semantics are tested
 * by the PCM/control fixtures. Keep the real core's disconnect path linked. */
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{
    assert(queue != NULL && queue->count == 0u);
    return 0u;
}

static int fixture_caps(void *opaque, uint32_t device, enum audio_direction direction,
                        struct audio_caps *out)
{
    (void)opaque; (void)device; (void)direction; (void)out;
    return 0;
}
static int fixture_open(void *opaque, uint32_t device, enum audio_direction direction,
                        struct audio_hw_stream *out)
{
    (void)opaque; (void)device; (void)direction; out->id = 17;
    return 0;
}
static int fixture_prepare(void *opaque, struct audio_hw_stream *stream,
                           const struct audio_params *params, const struct audio_dma *dma)
{
    (void)opaque; (void)stream; (void)params; (void)dma;
    pthread_mutex_lock(&state_mutex);
    prepare_entered = 1;
    pthread_cond_broadcast(&state_changed);
    while (!task_progress && !abort_prepare)
        pthread_cond_wait(&state_changed, &state_mutex);
    prepare_finished = task_progress;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    return 0;
}
static int fixture_trigger(void *opaque, struct audio_hw_stream *stream,
                           enum audio_trigger trigger)
{
    (void)opaque; (void)stream; (void)trigger;
    if(fault_mode){
        assert(fixture_irq_flags&(1ULL<<9));
        manager_wait_irq_enabled=1;
    }
    return 0;
}
static int fixture_pointer(void *opaque, struct audio_hw_stream *stream, uint64_t *out)
{
    (void)opaque; (void)stream; (void)out;
    return 0;
}
static void fixture_close(void *opaque, struct audio_hw_stream *stream)
{
    (void)opaque; (void)stream;
}
static uint32_t fixture_control_count(void *opaque)
{
    (void)opaque;
    return 0;
}

static void *prepare_thread(void *opaque)
{
    (void)opaque;
    fixture_cpu = 1;
    struct audio_params params = {0};
    struct audio_dma dma = {0};
    assert(!audio_stream_prepare(fixture_stream, &params, &dma));
    return NULL;
}

static void *manager_thread(void *opaque)
{
    (void)opaque;
    fixture_cpu = 0;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    if(fault_mode){
        pthread_mutex_lock(&state_mutex);
        manager_entered=1;pthread_cond_broadcast(&state_changed);
        pthread_mutex_unlock(&state_mutex);
        driver_manager_process_audio_faults();
        manager_resume_irq_disabled=!(fixture_irq_flags&(1ULL<<9));
        kernel_execution_unlock_irqrestore(flags);
        manager_exit_irq_enabled=(fixture_irq_flags&(1ULL<<9))!=0;
        return NULL;
    }
    assert(!driver_manager_phase_try_enter());
    struct driver_manager_wait_scope scope = {0};
    assert(!driver_manager_wait_begin(&scope));
    manager_wait_irq_enabled = (fixture_irq_flags & (1ULL << 9)) != 0;
    pthread_mutex_lock(&state_mutex);
    manager_entered = 1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    int ret = audio_unregister_owner(37, 1);
    assert(ret == 0);
    assert(!driver_manager_wait_end(&scope));
    manager_resume_irq_disabled = !(fixture_irq_flags & (1ULL << 9));
    driver_manager_phase_leave();
    kernel_execution_unlock_irqrestore(flags);
    manager_exit_irq_enabled = (fixture_irq_flags & (1ULL << 9)) != 0;
    return NULL;
}

static void *callback_resume_thread(void *opaque)
{
    (void)opaque;
    fixture_cpu = 2;
    pthread_mutex_lock(&state_mutex);
    worker_attempted = 1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    pthread_mutex_lock(&state_mutex);
    task_progress = 1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    kernel_execution_unlock_irqrestore(flags);
    return NULL;
}

static int waitable_module_init_probe(void)
{
    pthread_mutex_lock(&state_mutex);
    init_entered = 1;
    pthread_cond_broadcast(&state_changed);
    while (!task_progress)
        pthread_cond_wait(&state_changed, &state_mutex);
    init_completed = 1;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

static void waitable_init_phase_allows_task_progress(void)
{
    fixture_cpu = 3;
    fixture_irq_flags = 1ULL << 9;
    task_progress = 0;
    init_entered = 0;
    init_completed = 0;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    assert(!driver_manager_phase_try_enter());
    struct driver_manager_wait_scope scope = {0};
    assert(!driver_manager_wait_begin(&scope));
    assert(fixture_irq_flags & (1ULL << 9));
    pthread_t progress;
    assert(!pthread_create(&progress, NULL, callback_resume_thread, NULL));
    assert(!waitable_module_init_probe());
    assert(!driver_manager_wait_end(&scope));
    uint32_t resumed_irq_disabled = !(fixture_irq_flags & (1ULL << 9));
    assert(resumed_irq_disabled);
    driver_manager_phase_leave();
    kernel_execution_unlock_irqrestore(flags);
    pthread_join(progress, NULL);
    assert(init_entered && init_completed && resumed_irq_disabled);
    assert(fixture_irq_flags & (1ULL << 9));
    puts("waitable_module_init_releases_execution_phase PASS");
}

static void *held_legacy_callback_thread(void *opaque)
{
    (void)opaque;
    fixture_cpu = 5;
    assert(driver_manager_service_try_pin(7));
    pthread_mutex_lock(&state_mutex);
    legacy_pin_entered = 1;
    pthread_cond_broadcast(&state_changed);
    while (!legacy_callback_release)
        pthread_cond_wait(&state_changed, &state_mutex);
    pthread_mutex_unlock(&state_mutex);
    pthread_mutex_lock(&state_mutex);
    legacy_callback_completed = 1; /* Callback work is complete before the final pin drop. */
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    driver_manager_service_unpin(7);
    return NULL;
}

static void *release_legacy_callback_progress_thread(void *opaque)
{
    (void)opaque;
    fixture_cpu = 6;
    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    pthread_mutex_lock(&state_mutex);
    legacy_callback_release = 1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    kernel_execution_unlock_irqrestore(flags);
    return NULL;
}

static void legacy_admission_drain_and_stop_failure_reopen(void)
{
    fixture_cpu = 3;
    fixture_irq_flags = 1ULL << 9;
    driver_manager_service_enable(7);
    pthread_t callback;
    assert(!pthread_create(&callback, NULL, held_legacy_callback_thread, NULL));
    pthread_mutex_lock(&state_mutex);
    while (!legacy_pin_entered)
        pthread_cond_wait(&state_changed, &state_mutex);
    pthread_mutex_unlock(&state_mutex);

    uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    assert(!driver_manager_phase_try_enter());
    driver_manager_service_close(7);
    assert(!driver_manager_service_try_pin(7));
    struct driver_manager_wait_scope scope = {0};
    assert(!driver_manager_wait_begin(&scope));
    assert(fixture_irq_flags & (1ULL << 9));
    pthread_t progress;
    assert(!pthread_create(&progress, NULL, release_legacy_callback_progress_thread, NULL));
    driver_manager_service_drain(7);
    pthread_mutex_lock(&state_mutex);
    assert(legacy_callback_completed);
    pthread_mutex_unlock(&state_mutex);
    assert(!driver_manager_wait_end(&scope));
    driver_manager_phase_leave();

    /* This is the retained-module branch used when audio STOP refuses unload. */
    driver_manager_service_reopen(7);
    assert(driver_manager_service_try_pin(7));
    driver_manager_service_unpin(7);
    driver_manager_service_disable(7);
    assert(!driver_manager_service_try_pin(7));
    kernel_execution_unlock_irqrestore(flags);
    pthread_join(callback, NULL);
    pthread_join(progress, NULL);
    assert(fixture_irq_flags & (1ULL << 9));
    puts("legacy_service_close_drain_and_retained_reopen PASS");
}

static void boot_wait_phase_uses_owned_transaction(void)
{
    fixture_cpu = 3;
    const uint64_t caller_irq_states[] = {0, 1ULL << 9};
    for (uint32_t index = 0; index < sizeof(caller_irq_states) / sizeof(caller_irq_states[0]); ++index) {
        fixture_irq_flags = caller_irq_states[index];
        assert(!driver_manager_phase_try_enter());
        uint64_t flags;
        kernel_execution_lock_irqsave(&flags);
        struct driver_manager_wait_scope scope = {0};
        assert(!driver_manager_wait_begin(&scope));
        assert(fixture_irq_flags & (1ULL << 9));
        assert(!driver_manager_wait_end(&scope));
        assert(!(fixture_irq_flags & (1ULL << 9)));
        driver_manager_phase_leave();
        kernel_execution_unlock_irqrestore(flags);
        assert(fixture_irq_flags == caller_irq_states[index]);
    }
    puts("boot_wait_phase_preserves_irq_state PASS");
}

static void suspend_rejects_nested_and_unowned_contexts(void)
{
    fixture_cpu = 4;
    fixture_irq_flags = 1ULL << 9;
    uint64_t outer_flags, nested_flags;
    kernel_execution_lock_irqsave(&outer_flags);
    kernel_execution_lock_irqsave(&nested_flags);
    struct kernel_execution_suspend_token nested = {0};
    assert(kernel_execution_suspend_irqrestore(&nested) == -35);
    assert(!nested.active);
    assert(!(fixture_irq_flags & (1ULL << 9)));
    kernel_execution_unlock_irqrestore(nested_flags);
    kernel_execution_unlock_irqrestore(outer_flags);
    assert(fixture_irq_flags & (1ULL << 9));

    struct kernel_execution_suspend_token unowned = {0};
    assert(kernel_execution_suspend_irqrestore(&unowned) == -1);
    assert(!unowned.active);
    assert(fixture_irq_flags & (1ULL << 9));
    puts("suspend_rejects_nested_and_unowned_contexts PASS");
}

static struct timespec deadline_after_one_second(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    ++deadline.tv_sec;
    return deadline;
}

static unsigned deferred_task_calls;
static uint32_t waitable_card_task(void *opaque, uint32_t budget)
{
    (void)opaque;
    assert(budget && (fixture_irq_flags & (1ULL << 9)));
    assert(driver_manager_phase_try_enter() == -16);
    pthread_t progress;
    assert(!pthread_create(&progress, NULL, callback_resume_thread, NULL));
    assert(!waitable_module_init_probe());
    pthread_join(progress, NULL);
    ++deferred_task_calls;
    return 1;
}

static void deferred_card_task_uses_wait_phase(struct audio_card_ops *base,
                                              const struct audio_card_identity *identity)
{
    fixture_cpu = 3;
    fixture_irq_flags = 1ULL << 9;
    struct audio_card_ops ops = *base;
    ops.task_service = waitable_card_task;
    uint32_t card;
    assert(!audio_register_card_owned(identity, &ops, NULL, 37, &card));
    audio_service_tick();
    assert(!deferred_task_calls && audio_task_work_pending());
    /* No transaction and a nested transaction must preserve work for retry. */
    driver_manager_process_audio_faults();
    assert(!deferred_task_calls && audio_task_work_pending());
    uint64_t flags, nested_flags;
    kernel_execution_lock_irqsave(&flags);
    kernel_execution_lock_irqsave(&nested_flags);
    driver_manager_process_audio_faults();
    assert(!deferred_task_calls && audio_task_work_pending());
    assert(!(fixture_irq_flags & (1ULL << 9)));
    kernel_execution_unlock_irqrestore(nested_flags);
    assert(!driver_manager_phase_try_enter());
    driver_manager_process_audio_faults();
    assert(!deferred_task_calls && audio_task_work_pending());
    driver_manager_phase_leave();
    task_progress = init_entered = init_completed = 0;
    driver_manager_process_audio_faults();
    assert(deferred_task_calls == 1 && !audio_task_work_pending());
    assert(init_entered && init_completed && task_progress);
    assert(!(fixture_irq_flags & (1ULL << 9)));
    assert(!driver_manager_phase_try_enter());
    driver_manager_phase_leave();
    kernel_execution_unlock_irqrestore(flags);
    assert(fixture_irq_flags & (1ULL << 9));
    assert(!audio_unregister_card(card, 0));
    puts("deferred_card_task_manager_gate_nested_retry_IRQ_execution_progress PASS");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct audio_card_identity identity = {.id = "phase", .name = "phase fixture"};
    struct audio_card_ops ops = {
        .version = 1, .size = sizeof(ops), .pcm_caps = fixture_caps,
        .open = fixture_open, .prepare = fixture_prepare, .trigger = fixture_trigger,
        .pointer = fixture_pointer, .close = fixture_close,
        .control_count = fixture_control_count,
    };
    uint32_t card;
    assert(!audio_register_card_owned(&identity, &ops, NULL, 37, &card));
    assert(!audio_stream_open(card, 0, AUDIO_PLAYBACK, &fixture_stream));
    boot_wait_phase_uses_owned_transaction();
    suspend_rejects_nested_and_unowned_contexts();
    waitable_init_phase_allows_task_progress();
    legacy_admission_drain_and_stop_failure_reopen();

    for(fault_mode=0;fault_mode<2;++fault_mode){
    if(fault_mode){
        assert(!audio_register_card_owned(&identity,&ops,NULL,37,&card));
        assert(!audio_stream_open(card,0,AUDIO_PLAYBACK,&fixture_stream));
    }
    prepare_entered=prepare_finished=abort_prepare=0;
    manager_wait_irq_enabled=manager_resume_irq_disabled=manager_exit_irq_enabled=0;
    task_progress = 0;
    worker_attempted = 0;
    manager_entered = 0;
    pthread_t callback, manager, resume;
    assert(!pthread_create(&callback, NULL, prepare_thread, NULL));
    pthread_mutex_lock(&state_mutex);
    while (!prepare_entered)
        pthread_cond_wait(&state_changed, &state_mutex);
    pthread_mutex_unlock(&state_mutex);
    if(fault_mode)assert(!audio_request_controller_disconnect(card));

    assert(!pthread_create(&manager, NULL, manager_thread, NULL));
    pthread_mutex_lock(&state_mutex);
    while (!manager_entered)
        pthread_cond_wait(&state_changed, &state_mutex);
    pthread_mutex_unlock(&state_mutex);
    for (;;) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&audio_lock, &flags);
        uint32_t closing = cards[card & 255U].closing;
        kernel_spin_unlock_irqrestore(&audio_lock, flags);
        if (closing && __atomic_load_n(&manager_admitted,__ATOMIC_ACQUIRE)) break;
        __asm__ volatile("pause");
    }

    assert(driver_manager_phase_try_enter() == -16);
    assert(!pthread_create(&resume, NULL, callback_resume_thread, NULL));
    pthread_mutex_lock(&state_mutex);
    while (!worker_attempted)
        pthread_cond_wait(&state_changed, &state_mutex);
    struct timespec deadline = deadline_after_one_second();
    int wait_ret = 0;
    while (!prepare_finished && wait_ret == 0)
        wait_ret = pthread_cond_timedwait(&state_changed, &state_mutex, &deadline);
    int progressed_while_unload_waited = prepare_finished && task_progress;
    if (!progressed_while_unload_waited) {
        puts("RED: pinned prepare callback could not resume while manager waited for its pin");
        abort_prepare = 1;
        pthread_cond_broadcast(&state_changed);
    }
    pthread_mutex_unlock(&state_mutex);

    pthread_join(callback, NULL);
    pthread_join(resume, NULL);
    pthread_join(manager, NULL);
    assert(manager_wait_irq_enabled);
    assert(manager_resume_irq_disabled);
    assert(manager_exit_irq_enabled);
    assert(progressed_while_unload_waited);
    assert(!audio_stream_close(fixture_stream));
    assert(!audio_disconnect_work_pending());
    puts(fault_mode ? "deferred_fatal_wrapper_releases_execution_IRQ_manager_gate_and_pin_drain PASS" :
         "execution_phase_allows_pinned_task_callback_progress PASS");
    }
    deferred_card_task_uses_wait_phase(&ops, &identity);
    return 0;
}
