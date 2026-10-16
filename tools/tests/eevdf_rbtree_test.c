#include <stdio.h>

#include <reliefnt/eevdf.h>

#ifndef RELIEFNT_EEVDF_RBTREE
int main(void)
{
    fputs("RED: EEVDF has no indexed red-black tree yet\n", stderr);
    return 1;
}
#else
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "eevdf_reference.h"

#define ENTITY_COUNT 2048
#define RANDOM_STEPS 7000

static struct eevdf_queue queue;
static struct eevdf_ref_queue reference_queue;
static struct eevdf_entity entities[ENTITY_COUNT];
static struct eevdf_node nodes[ENTITY_COUNT];
static struct eevdf_ref_entity reference_entities[ENTITY_COUNT];
static uint32_t random_state = 0x7a31d2c5u;

static uint32_t next_random(void)
{
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    random_state = x;
    return x;
}

static unsigned choose_entity(void)
{ return next_random() % ENTITY_COUNT; }

static int entity_index(const struct eevdf_entity *entity)
{
    assert(entity >= entities && entity < entities + ENTITY_COUNT);
    return (int)(entity - entities);
}

static int64_t minimum(int64_t a, int64_t b)
{ return a < b ? a : b; }

struct tree_summary {
    unsigned count;
    unsigned delayed_count;
    unsigned black_height;
    unsigned height;
    int64_t min_vruntime;
    int64_t min_delayed_vruntime;
};

static struct tree_summary verify_subtree(struct eevdf_node *node,
                                          struct eevdf_node *parent,
                                          int *inorder, unsigned *inorder_count)
{
    if (!node) return (struct tree_summary){
        .black_height = 1,
        .height = 0,
        .min_vruntime = INT64_MAX,
        .min_delayed_vruntime = INT64_MAX,
    };

    assert(node->parent == parent);
    assert(node->entity);
    assert(node->queue == &queue);
    int current_index = entity_index(node->entity);
    assert(node->cached_vruntime == node->entity->vruntime);
    assert(node->cached_deadline == node->entity->deadline);
    assert(node->cached_weight == node->entity->weight);
    assert(node->cached_delayed == node->entity->delayed);
    assert(node->entity->queued);
    assert(node->entity->node == node);
    if (node->red) {
        assert(!node->left || !node->left->red);
        assert(!node->right || !node->right->red);
    }

    struct tree_summary left = verify_subtree(node->left, node, inorder,
                                               inorder_count);
    assert(*inorder_count < ENTITY_COUNT);
    inorder[(*inorder_count)++] = current_index;
    struct tree_summary right = verify_subtree(node->right, node, inorder,
                                                inorder_count);
    assert(left.black_height == right.black_height);
    struct tree_summary result = {
        .count = left.count + right.count + 1,
        .delayed_count = left.delayed_count + right.delayed_count +
            (node->entity->delayed ? 1u : 0u),
        .black_height = left.black_height + (node->red ? 0u : 1u),
        .height = (left.height > right.height ? left.height : right.height) + 1,
        .min_vruntime = minimum(node->entity->vruntime,
            minimum(left.min_vruntime, right.min_vruntime)),
        .min_delayed_vruntime = minimum(
            node->entity->delayed ? node->entity->vruntime : INT64_MAX,
            minimum(left.min_delayed_vruntime, right.min_delayed_vruntime)),
    };
    assert(node->min_vruntime == result.min_vruntime);
    assert(node->min_delayed_vruntime == result.min_delayed_vruntime);
    assert(node->delayed_count == result.delayed_count);
    return result;
}

static void reset_world(void)
{
    memset(&queue, 0, sizeof(queue));
    memset(&reference_queue, 0, sizeof(reference_queue));
    for (unsigned i = 0; i < ENTITY_COUNT; ++i) {
        memset(&entities[i], 0, sizeof(entities[i]));
        memset(&nodes[i], 0, sizeof(nodes[i]));
        memset(&reference_entities[i], 0, sizeof(reference_entities[i]));
        eevdf_bind(&entities[i], &nodes[i]);
    }
}

static void compare_policy_state(void)
{
    assert(queue.clock == reference_queue.clock);
    uint64_t actual_load = 0, reference_load = 0;
    int64_t actual_average = eevdf_average(&queue, &actual_load);
    int64_t reference_average = eevdf_ref_average(&reference_queue,
                                                  &reference_load);
    assert(actual_average == reference_average);
    assert(actual_load == reference_load);

    for (unsigned i = 0; i < ENTITY_COUNT; ++i) {
        struct eevdf_entity *a = &entities[i];
        struct eevdf_ref_entity *r = &reference_entities[i];
        assert(a->vruntime == r->vruntime);
        assert(a->deadline == r->deadline);
        assert(a->lag == r->lag);
        assert(a->remainder == r->remainder);
        assert(a->runtime == r->runtime);
        assert(a->weight == r->weight);
        assert(a->cpu == r->cpu);
        assert(a->initialized == r->initialized);
        assert(a->queued == r->queued);
        assert(a->delayed == r->delayed);
    }
}

static void verify_index(void)
{
    assert(!queue.root || !queue.root->parent);
    assert(!queue.root || !queue.root->red);
    int inorder[ENTITY_COUNT];
    unsigned inorder_count = 0;
    unsigned active_count = 0;
    uint64_t expected_load = 0;
    int64_t expected_weighted_sum = 0;
    bool seen_order[ENTITY_COUNT] = {0};

    /* Check the independent newest-first membership list. */
    struct eevdf_entity *actual = queue.head;
    struct eevdf_ref_entity *reference = reference_queue.head;
    struct eevdf_node *previous = NULL, *last = NULL;
    unsigned list_count = 0;
    while (actual || reference) {
        assert(actual && reference);
        int index = entity_index(actual);
        assert(&reference_entities[index] == reference);
        struct eevdf_node *node = actual->node;
        assert(node && node->previous == previous);
        assert(node->queue == &queue && node->entity == actual);
        if (previous) assert(previous->next == node);
        if (!actual->delayed) ++active_count;
        expected_load += actual->weight;
        expected_weighted_sum += (actual->vruntime - queue.clock) *
            (int64_t)actual->weight;
        previous = node;
        last = node;
        actual = node->next ? node->next->entity : NULL;
        reference = reference->next;
        ++list_count;
        assert(list_count <= ENTITY_COUNT);
    }
    assert(queue.tail == last);
    assert(queue.nr_queued == list_count);
    assert(queue.nr_active == active_count);
    assert(queue.load == expected_load);
    assert(queue.weighted_sum == expected_weighted_sum);

    struct tree_summary summary = verify_subtree(queue.root, NULL, inorder,
                                                 &inorder_count);
    assert(summary.count == list_count);
    assert(inorder_count == list_count);
    assert(summary.delayed_count == list_count - active_count);
    unsigned ceil_log2 = 0;
    for (unsigned capacity = 1; capacity < list_count + 1; capacity <<= 1)
        ++ceil_log2;
    assert(summary.height <= 2 * ceil_log2);

    for (unsigned i = 0; i < inorder_count; ++i) {
        struct eevdf_node *node = entities[inorder[i]].node;
        assert(!seen_order[inorder[i]]);
        seen_order[inorder[i]] = true;
        if (i) {
            struct eevdf_node *before = entities[inorder[i - 1]].node;
            assert(before->entity->deadline <= node->entity->deadline);
            if (before->entity->deadline == node->entity->deadline)
                assert(before->order > node->order);
        }
    }
    for (unsigned i = 0; i < ENTITY_COUNT; ++i)
        assert(seen_order[i] == entities[i].queued);
}

static void check_all(void)
{
    compare_policy_state();
    verify_index();
}

static bool subtree_can_find(const struct eevdf_node *node, int64_t average,
                             bool delayed_only)
{
    return node && (delayed_only
        ? node->delayed_count && node->min_delayed_vruntime <= average
        : node->min_vruntime <= average);
}

static unsigned find_path_steps(int64_t average, bool delayed_only)
{
    struct eevdf_node *node = queue.root;
    unsigned steps = 0;
    while (subtree_can_find(node, average, delayed_only)) {
        ++steps;
        if (subtree_can_find(node->left, average, delayed_only)) {
            node = node->left;
        } else if (node->entity->vruntime <= average &&
                   (!delayed_only || node->entity->delayed)) {
            break;
        } else {
            node = node->right;
        }
    }
    return steps;
}

static struct eevdf_ref_entity *reference_find(int64_t average,
                                               bool delayed_only)
{
    struct eevdf_ref_entity *best = NULL;
    for (struct eevdf_ref_entity *entity = reference_queue.head;
         entity; entity = entity->next) {
        if (entity->vruntime > average ||
            (delayed_only && !entity->delayed)) continue;
        if (!best || entity->deadline < best->deadline) best = entity;
    }
    return best;
}

static void check_tree_search(int64_t average, bool delayed_only,
                              unsigned max_steps)
{
    unsigned steps = find_path_steps(average, delayed_only);
    assert(steps <= max_steps);
    struct eevdf_ref_entity *expected = reference_find(average, delayed_only);
    struct eevdf_entity *actual = eevdf_tree_find(&queue, average, delayed_only);
    assert((!expected && !actual) ||
           (expected && actual == &entities[expected - reference_entities]));
}

static void enqueue(unsigned index, int nice)
{
    eevdf_enqueue(&queue, &entities[index], nice);
    eevdf_ref_enqueue(&reference_queue, &reference_entities[index], nice);
}

static void dequeue(unsigned index)
{
    eevdf_dequeue(&queue, &entities[index]);
    eevdf_ref_dequeue(&reference_queue, &reference_entities[index]);
}

static void sleep_entity(unsigned index)
{
    eevdf_sleep(&queue, &entities[index]);
    eevdf_ref_sleep(&reference_queue, &reference_entities[index]);
}

static void account(unsigned index, uint64_t delta)
{
    eevdf_account(&entities[index], delta);
    eevdf_ref_account(&reference_entities[index], delta);
}

static void yield_entity(unsigned index)
{
    eevdf_yield(&entities[index]);
    eevdf_ref_yield(&reference_entities[index]);
}

static void reweight(unsigned index, int nice)
{
    eevdf_reweight(&queue, &entities[index], nice);
    eevdf_ref_reweight(&reference_queue, &reference_entities[index], nice);
}

static void set_delayed(unsigned index, bool delayed)
{
    eevdf_set_delayed(&entities[index], delayed);
    reference_entities[index].delayed = delayed;
}

static int pick(void)
{
    struct eevdf_ref_entity *expected = eevdf_ref_pick(&reference_queue);
    struct eevdf_entity *actual = eevdf_pick(&queue);
    assert((expected == NULL && actual == NULL) ||
           (expected && actual == &entities[expected - reference_entities]));
    return expected ? (int)(expected - reference_entities) : -1;
}

static void refresh_key(unsigned index, int64_t vruntime, int64_t deadline)
{
    entities[index].vruntime = vruntime;
    entities[index].deadline = deadline;
    reference_entities[index].vruntime = vruntime;
    reference_entities[index].deadline = deadline;
    eevdf_refresh(&entities[index]);
}

static void test_ties_wrap_and_negative_average(void)
{
    reset_world();
    enqueue(0, 0);
    enqueue(1, 0);
    enqueue(2, 0);
    for (unsigned i = 0; i < 3; ++i) refresh_key(i, 0, 100);
    check_all();
    assert(pick() == 2);
    check_all();

    /* Force order renumbering while equal deadlines depend on newest-first. */
    queue.next_order = UINT64_MAX;
    enqueue(3, 0);
    refresh_key(3, 0, 100);
    check_all();
    assert(pick() == 3);
    check_all();

    reset_world();
    enqueue(0, 0);
    enqueue(1, 5);
    refresh_key(0, -1, 1000);
    refresh_key(1, 0, 1000);
    uint64_t load;
    assert(eevdf_average(&queue, &load) == -1);
    assert(eevdf_ref_average(&reference_queue, &load) == -1);
    check_all();
    pick();
    check_all();
}

static void test_delayed_cleanup_and_rekeys(void)
{
    reset_world();
    for (unsigned i = 0; i < 5; ++i) enqueue(i, (int)i - 2);
    refresh_key(0, -3000, 9000);
    refresh_key(1, 0, 8000);
    refresh_key(2, -500, 7000);
    refresh_key(3, 4000, 6000);
    refresh_key(4, 10000, 5000);
    set_delayed(0, true);
    set_delayed(2, true);
    set_delayed(4, true);
    check_all();
    pick();
    check_all();
    assert(!entities[0].queued && entities[0].lag == 0);
    assert(!entities[2].queued && entities[2].lag == 0);
    assert(entities[4].queued && entities[4].delayed);

    /* Exercise account, yield, and reweight key changes in place. */
    account(1, 13000);
    check_all();
    yield_entity(3);
    check_all();
    reweight(3, 19);
    check_all();
    reweight(4, -20);
    check_all();
    pick();
    check_all();
}

static void test_weighted_sum_rebase_boundary(void)
{
    reset_world();
    enqueue(0, 19); /* weight 15 */
    enqueue(1, 18); /* weight 18 */
    refresh_key(0, -614891469123651719LL, 0);
    refresh_key(1, -1, 100);

    uint64_t actual_load, reference_load;
    int64_t actual_average = eevdf_average(&queue, &actual_load);
    int64_t reference_average = eevdf_ref_average(&reference_queue,
                                                  &reference_load);
    assert(actual_load == 33 && reference_load == 33);
    assert(actual_average == reference_average);
    assert(pick() == 0);
    check_all();

    /* The legacy order sums a large negative first, then two positive terms;
     * after removing it, rebasing makes the remaining sum fit signed 64-bit. */
    reset_world();
    enqueue(1, 18); /* weight 18, positive */
    enqueue(2, 17); /* weight 23, positive */
    enqueue(0, 19); /* weight 15, negative and newest in the reference list */
    refresh_key(0, -479615345916448341LL, 0);
    refresh_key(1, 399679454930373618LL, 100);
    refresh_key(2, 312792616902031527LL, 200);
    check_all();
    dequeue(0);
    check_all();
}

static void test_weighted_sum_refresh_boundary(void)
{
    reset_world();
    enqueue(3, 19);
    enqueue(2, 19);
    enqueue(1, 19);
    refresh_key(1, 1, 1000);
    refresh_key(2, -614891469123651720LL, 2000);
    refresh_key(3, -1, 3000);
    check_all();
    account(1, 1);
    check_all();
}

static void test_large_tree_stress(void)
{
    reset_world();
    for (unsigned i = 0; i < ENTITY_COUNT; ++i)
        enqueue(i, (int)((i * 37u) % 40u) - 20);
    for (unsigned i = 0; i < ENTITY_COUNT; ++i) {
        int64_t vruntime = (int64_t)((i * 7919u) % 200001u) - 100000;
        int64_t deadline = (int64_t)((i * 131u) % 97u) * 1000 - 48000;
        refresh_key(i, vruntime, deadline);
        if (i % 7u == 0) set_delayed(i, true);
    }
    check_all();
    uint64_t load;
    int64_t average = eevdf_average(&queue, &load);
    int inorder[ENTITY_COUNT];
    unsigned inorder_count = 0;
    struct tree_summary summary = verify_subtree(queue.root, NULL, inorder,
                                                 &inorder_count);
    unsigned height_bound = 0;
    for (unsigned capacity = 1; capacity < ENTITY_COUNT + 1; capacity <<= 1)
        ++height_bound;
    assert(summary.height <= 2 * height_bound);
    check_tree_search(average, false, summary.height);
    check_tree_search(average, true, summary.height);
    (void)load;
    (void)pick();
    check_all();

    for (unsigned step = 0; step < ENTITY_COUNT; ++step) {
        unsigned i = (step * 1291u) % ENTITY_COUNT;
        if (entities[i].queued) dequeue(i);
        check_all();
    }
    assert(!queue.root && !queue.head && !queue.tail);
}

static void test_randomized_differential(void)
{
    static const int nice_values[] = {-20, -5, 0, 5, 12, 19};
    reset_world();
    for (unsigned i = 0; i < ENTITY_COUNT; ++i) {
        enqueue(i, nice_values[next_random() %
            (sizeof(nice_values) / sizeof(nice_values[0]))]);
        check_all();
    }

    for (unsigned step = 0; step < RANDOM_STEPS; ++step) {
        unsigned i = choose_entity();
        struct eevdf_entity *e = &entities[i];
        switch (next_random() % 10) {
        case 0:
            if (!e->queued)
                enqueue(i, nice_values[next_random() %
                    (sizeof(nice_values) / sizeof(nice_values[0]))]);
            break;
        case 1:
            if (e->queued) dequeue(i);
            break;
        case 2:
            pick();
            break;
        case 3:
            if (e->queued) account(i, 1 + next_random() % 9000);
            break;
        case 4:
            if (e->queued) sleep_entity(i);
            break;
        case 5:
            reweight(i, nice_values[next_random() %
                (sizeof(nice_values) / sizeof(nice_values[0]))]);
            break;
        case 6:
            if (e->weight) yield_entity(i);
            break;
        case 7:
            if (e->queued) set_delayed(i, (next_random() & 1u) != 0);
            break;
        case 8: {
            int64_t v = (int64_t)(next_random() % 100001) - 50000;
            int64_t d = v + (int64_t)(next_random() % 30001) - 15000;
            refresh_key(i, v, d);
            break;
        }
        case 9: {
            uint64_t load_a, load_r;
            int64_t average_a = eevdf_average(&queue, &load_a);
            int64_t average_r = eevdf_ref_average(&reference_queue, &load_r);
            assert(average_a == average_r && load_a == load_r);
            break;
        }
        }
        check_all();
    }

    for (unsigned i = 0; i < ENTITY_COUNT; ++i)
        if (entities[i].queued) dequeue(i);
    check_all();
    assert(!queue.root && !queue.head && !queue.tail);
}

int main(void)
{
    test_ties_wrap_and_negative_average();
    test_delayed_cleanup_and_rekeys();
    test_weighted_sum_rebase_boundary();
    test_weighted_sum_refresh_boundary();
    test_large_tree_stress();
    test_randomized_differential();
    puts("PASS EEVDF red-black index invariants and differential policy oracle");
    return 0;
}
#endif
