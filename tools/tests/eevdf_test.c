#include <assert.h>
#include <stdio.h>
#include <reliefnt/eevdf.h>

#ifdef RELIEFNT_EEVDF_RBTREE
static struct eevdf_node node_a, node_b;
#endif

static void reset_pair(struct eevdf_queue *q, struct eevdf_entity *a,
                       struct eevdf_entity *b)
{
    *q = (struct eevdf_queue){0};
    *a = (struct eevdf_entity){0};
    *b = (struct eevdf_entity){0};
#ifdef RELIEFNT_EEVDF_RBTREE
    eevdf_bind(a, &node_a);
    eevdf_bind(b, &node_b);
#endif
}

int main(void)
{
    struct eevdf_queue q = {0};
    struct eevdf_entity a = {0}, b = {0};
    reset_pair(&q, &a, &b);
    eevdf_enqueue(&q, &a, 0);
    eevdf_enqueue(&q, &b, 0);
    for (unsigned i = 0; i < 10000; ++i) {
        struct eevdf_entity *e = eevdf_pick(&q);
        assert(e);
        eevdf_account(e, 1000);
    }
    assert(a.runtime > 4900000 && a.runtime < 5100000);
    assert(b.runtime > 4900000 && b.runtime < 5100000);
    puts("PASS EEVDF equal-weight service");

    reset_pair(&q, &a, &b);
    eevdf_enqueue(&q, &a, 0);
    eevdf_enqueue(&q, &b, 5);
    for (unsigned i = 0; i < 200000; ++i) eevdf_account(eevdf_pick(&q), 100);
    assert(a.runtime > 14900000 && a.runtime < 15200000);
    assert(b.runtime > 4800000 && b.runtime < 5100000);
    assert(eevdf_weight(-20) == 88761 && eevdf_weight(19) == 15);
    puts("PASS EEVDF nice 0:5 proportional service, neither task starves");

    reset_pair(&q, &a, &b);
    eevdf_enqueue(&q, &a, 0); eevdf_enqueue(&q, &b, 0);
    a.vruntime = 0; a.deadline = 20000;
    b.vruntime = 1000; b.deadline = 100;
#ifdef RELIEFNT_EEVDF_RBTREE
    eevdf_refresh(&a);
    eevdf_refresh(&b);
#endif
    assert(eevdf_pick(&q) == &a); /* earliest deadline alone is insufficient */
    eevdf_account(&a, 2000);
    assert(eevdf_pick(&q) == &b);
    eevdf_sleep(&q, &a);
    assert(a.queued && a.delayed);
    assert(eevdf_pick(&q) == &b);
    eevdf_account(&b, 3000);
    assert(eevdf_pick(&q) == &b);
    assert(!a.queued && a.lag == 0);
    eevdf_enqueue(&q, &a, 0);
    assert(a.vruntime == b.vruntime);
    a.deadline = b.deadline - 1;
#ifdef RELIEFNT_EEVDF_RBTREE
    eevdf_refresh(&a);
#endif
    assert(eevdf_pick(&q) == &a);
    eevdf_yield(&a);
    assert(eevdf_pick(&q) == &b);
    eevdf_dequeue(&q, &a); eevdf_dequeue(&q, &b);
    assert(!q.head && !eevdf_pick(&q));
    puts("PASS EEVDF eligibility, delayed sleep debt, wakeup and yield");

    reset_pair(&q, &a, &b);
    eevdf_enqueue(&q, &a, 0); eevdf_enqueue(&q, &b, 0);
    eevdf_account(&b, 2000);
    eevdf_sleep(&q, &a);
    assert(!a.queued && a.lag == 1000);
    eevdf_account(&b, 10000);
    eevdf_enqueue(&q, &a, 0);
    uint64_t load;
    assert(eevdf_average(&q, &load) - a.vruntime == 1000);
    puts("PASS EEVDF wake placement preserves bounded positive lag");
    int64_t request = a.deadline - a.vruntime;
    eevdf_reweight(&q, &a, 5);
    assert(a.weight == 335 && a.queued);
    assert(a.deadline - a.vruntime == request * 1024 / 335);
    a.runtime = b.runtime = 0;
    for (unsigned i = 0; i < 200000; ++i) eevdf_account(eevdf_pick(&q), 100);
    assert(b.runtime > 14900000 && b.runtime < 15200000);
    puts("PASS EEVDF reweight preserves remaining request and changes service ratio");
    eevdf_dequeue(&q, &a);
    a.lag = 1000;
    eevdf_reweight(&q, &a, -20);
    assert(!a.queued && a.weight == 88761);
    assert(a.lag == 1000 * 335 / 88761);
    puts("PASS sleeping entity reweight preserves service debt without waking it");
}
