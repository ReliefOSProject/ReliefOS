/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_HOST_MODULE_STAGE_H
#define RELIEFOS_HOST_MODULE_STAGE_H
#include "tools/host/modules/bundle.h"
#include <stdio.h>

/* All output paths belong to a unique, private tree until publication. */
struct kernel_stage { char *path, *destination, *leaf, *temporary_leaf; int root_fd, parent_fd; };
struct kernel_stage_file { char *name; char sha256[65]; off_t size; };
struct kernel_stage_inventory { struct kernel_stage_file *files; size_t count; };
/** Accept canonical tree names, including the standard BusyBox [ and [[ applets. */
bool kernel_stage_tree_member(const char *name);

/** Open a regular JSON manifest below a held directory; identifiers cannot contain NUL. */
int kernel_stage_document(int root_fd, const char *name, json_value *document);
/** Parse an explicit regular host manifest, with the same strict JSON and NUL policy. */
int kernel_stage_document_path(const char *path, json_value *document);
/** Check release/ABI/kernel identity common to installed, module and early manifests. */
int kernel_stage_identity(const json_value *document, const char **release,
                          const char **abi, const char **kernel_sha256);
/** Verify every contained regular file in a manifest's files object. */
int kernel_stage_verify(int root_fd, const json_value *files);
/** Create directories under a held root, refusing symlink components. */
int kernel_stage_mkdir(int root_fd, const char *name, mode_t mode);
/** Write or replace a private regular member, refusing symlinks; sync before return. */
int kernel_stage_write(int root_fd, const char *name, const void *data, size_t size, mode_t mode);
/** Create a private-tree symlink, without replacing existing entries or following parents. */
int kernel_stage_symlink(int root_fd, const char *name, const char *target);
/** Copy held regular source bytes into a private member and recheck the destination hash. */
int kernel_stage_copy(int source_root, const char *source, int target_root,
                      const char *target, const char *sha256, mode_t mode);
/** Copy from the current offset of an owned regular input descriptor. */
int kernel_stage_copy_fd(int input, int target_root, const char *target, const char *sha256, mode_t mode);
/** Open an explicit regular host input without following its final component or blocking on a FIFO. */
int kernel_stage_input(const char *path);
/** Run argv without a shell. Optional captured stdout is bounded to 16 MiB. */
int kernel_stage_run(char *const argv[], struct byte_buffer *capture);
/** Create a unique stage beside destination, never at the filesystem root. */
int kernel_stage_begin(struct kernel_stage *stage, const char *destination);
/** Replace only a prior tree with this producer's regular ownership marker. */
int kernel_stage_publish(struct kernel_stage *stage, const char *marker);
/** Remove only the uncommitted, private tree created by kernel_stage_begin. */
void kernel_stage_end(struct kernel_stage *stage);
/** Inventory sorted regular members; refuse symlinks and special files. */
int kernel_stage_inventory(int root_fd, struct kernel_stage_inventory *inventory);
/** Release inventory names and records. */
void kernel_stage_inventory_free(struct kernel_stage_inventory *inventory);
/** Serialize a JSON string or an existing JSON value; returns -1 on output failure. */
int kernel_stage_quote(FILE *stream, const char *text);
int kernel_stage_json(FILE *stream, const json_value *value);
/** Serialize a sorted checksum inventory as a JSON object. */
int kernel_stage_files_json(FILE *stream, const struct kernel_stage_inventory *inventory);
/** Encode sorted, normalized Linux newc records; console/null are archive-only devices. */
int kernel_stage_newc(int tree_fd, int output_fd, unsigned epoch);
/** Native image producer entry points; argv excludes the command name. */
int kernel_modules_stage(int argc, char **argv);
int kernel_initramfs_stage(int argc, char **argv);
int kernel_payload_stage(int argc, char **argv);
int kernel_boot_stage(int argc, char **argv);
int kernel_stage_check(int argc, char **argv);
int kernel_slot_encode_command(int argc, char **argv);
int kernel_slot_select_command(int argc, char **argv);
int kernel_release_info(int argc, char **argv);
/** Initialize two preallocated records in a fresh private ESP stage only. */
int kernel_slots_initialize(int root_fd, const char *release, const char *abi,
                            const char *kernel_hash, const char *archive_hash);
/** Write a generation-one canonical identity inside an immutable payload version. */
int kernel_slot_template(int root_fd, const char *path, const char *release,
                         const char *abi, const char *kernel_hash, const char *archive_hash);
#endif
