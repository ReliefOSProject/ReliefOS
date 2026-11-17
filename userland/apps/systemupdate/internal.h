/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_SYSTEM_UPDATE_INTERNAL_H
#define RELIEFOS_SYSTEM_UPDATE_INTERNAL_H
#define _GNU_SOURCE 1
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_METADATA_MAX (256u * 1024u * 1024u)
#define UPDATE_OUTPUT_MAX (32u * 1024u * 1024u)
#define UPDATE_RESERVE (64ull * 1024u * 1024u)

struct update_state {
    char phase[32], target[65], policy[65], seal[65];
    int reboot;
};
struct update_context {
    char root[PATH_MAX], base[PATH_MAX], plan[PATH_MAX], cache[PATH_MAX];
    char apk[PATH_MAX], fetch[PATH_MAX], bundle[PATH_MAX], id[65];
    int machine, isolated, lock, committing;
    unsigned timeout_seconds;
    struct update_state state;
};
extern volatile sig_atomic_t update_cancelled;

int update_error(const char *operation, int result);
int update_path(char out[PATH_MAX], const char *root, const char *suffix);
int update_directory(const char *path, int create);
int update_regular(const char *path, uint64_t limit);
int update_copy(const char *source, const char *destination, unsigned mode);
int update_copy_tree(const char *source, const char *destination);
int update_write(const char *path, const void *bytes, size_t size, unsigned mode);
int update_read(const char *path, char **bytes, size_t *size, size_t limit);
int update_hash(const char *path, char output[65]);
int update_hash_tree(const char *path, char output[65]);
int update_target_hash(struct update_context *context, char output[65]);
int update_policy_hash(struct update_context *context, char output[65]);
int update_random_id(char output[65]);
int update_token(const char *text);
int update_state_save(struct update_context *context, const char *phase);
int update_state_load(struct update_context *context);
int update_active(struct update_context *context, int publish);
int update_space(const char *path, uint64_t required);
int update_tree_bytes(const char *path, uint64_t *bytes);
int update_run(struct update_context *context, char *const arguments[], const char *output, int offline_commit);
void update_event(struct update_context *context, const char *event);
int update_apk(struct update_context *context, const char *root, int network,
               const char *output, const char *const arguments[]);
int update_sources(struct update_context *context);
int update_rpr_url(struct update_context *context, char output[2048]);
int update_cache_verify(struct update_context *context, int snapshot);
int update_plan_seal(struct update_context *context, char output[65]);
int update_kernel_check(struct update_context *context, int installed);
int update_kernel_preflight(struct update_context *context);
int update_kernel_publish(struct update_context *context);
int update_prepare(struct update_context *context, int resume);
int update_apply(struct update_context *context, int repair);
int update_status(struct update_context *context);
int update_discard(struct update_context *context);
int update_configure(struct update_context *context);
int update_probe(struct update_context *context);
int update_guard(struct update_context *context);
int update_plan_open(struct update_context *context, const char *id);
int update_context_init(struct update_context *context, const char *root, int writing);
#endif
