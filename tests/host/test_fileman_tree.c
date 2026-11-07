#include "fileman.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void add_node(unsigned index, unsigned id, unsigned parent, const char *path,
                     const char *label, int expanded, int loaded, int has_children)
{
    struct fileman_tree_node *node = &fileman_tree_nodes[index];
    memset(node, 0, sizeof(*node));
    node->used = 1;
    node->id = id;
    node->parent_id = parent;
    node->expanded = expanded;
    node->loaded = loaded;
    node->has_children = has_children;
    snprintf(node->path, sizeof(node->path), "%s", path);
    snprintf(node->label, sizeof(node->label), "%s", label);
}

static const struct fileman_tree_node *by_id(unsigned id)
{
    for (unsigned i = 0; i < fileman_tree_node_count; ++i)
        if (fileman_tree_nodes[i].used && fileman_tree_nodes[i].id == id)
            return &fileman_tree_nodes[i];
    return NULL;
}

static const struct fileman_tree_node *by_label(unsigned parent, const char *label)
{
    for (unsigned i = 0; i < fileman_tree_node_count; ++i)
        if (fileman_tree_nodes[i].used && fileman_tree_nodes[i].parent_id == parent &&
            strcmp(fileman_tree_nodes[i].label, label) == 0)
            return &fileman_tree_nodes[i];
    return NULL;
}

static void join(char *path, size_t size, const char *base, const char *part)
{
    assert(snprintf(path, size, "%s/%s", base, part) > 0);
    assert(strlen(path) < size - 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char current[RELIEFOS_FS_PATH_LEN], path[RELIEFOS_FS_PATH_LEN];
    join(current, sizeof(current), argv[1], "current");
    assert(mkdir(current, 0700) == 0);
    join(path, sizeof(path), current, "kept");
    assert(mkdir(path, 0700) == 0);
    char kept[RELIEFOS_FS_PATH_LEN];
    snprintf(kept, sizeof(kept), "%s", path);
    join(path, sizeof(path), kept, "nested");
    assert(mkdir(path, 0700) == 0);
    char nested[RELIEFOS_FS_PATH_LEN];
    snprintf(nested, sizeof(nested), "%s", path);
    join(path, sizeof(path), current, "added");
    assert(mkdir(path, 0700) == 0);
    join(path, sizeof(path), current, ".hidden");
    assert(mkdir(path, 0700) == 0);

    copy_text(current_path, sizeof(current_path), current);
    fileman_show_hidden = 0;
    fileman_tree_node_count = 6;
    fileman_tree_next_id = 100;
    add_node(0, 1, 0, current, "current", 1, 1, 1);
    add_node(1, 11, 1, kept, "kept", 1, 1, 1);
    add_node(2, 12, 1, "/removed", "removed", 1, 1, 1);
    add_node(3, 13, 11, nested, "nested", 0, 0, 0);
    add_node(4, 14, 0, "/other", "other", 0, 1, 1);
    add_node(5, 15, 12, "/removed/child", "removed-child", 1, 1, 1);
    struct fileman_tree_node untouched = fileman_tree_nodes[4];
    fileman_tree_refresh_current();
    assert(!by_id(12) && !by_id(15));
    assert(by_id(11) && by_id(11)->expanded && by_id(11)->loaded);
    assert(by_id(13) && by_id(13)->parent_id == 11);
    assert(by_id(14) && !memcmp(by_id(14), &untouched, sizeof(untouched)));
    assert(by_label(1, "added") && by_label(1, "kept"));
    assert(!by_label(1, "added")->expanded && !by_label(1, "added")->loaded);
    assert(!by_label(1, ".hidden"));
    struct fileman_tree_item visible[FILEMAN_TREE_MAX_NODES];
    unsigned n = build_tree_items(visible, FILEMAN_TREE_MAX_NODES);
    assert(n >= 5 && strcmp(visible[1].label, "added") == 0 &&
           strcmp(visible[2].label, "kept") == 0 &&
           strcmp(visible[3].label, "nested") == 0);

    /* A collapsed current node is left alone even if the directory changes. */
    for (unsigned i = 0; i < fileman_tree_node_count; ++i)
        if (fileman_tree_nodes[i].id == 1) fileman_tree_nodes[i].expanded = 0;
    join(path, sizeof(path), current, "later");
    assert(mkdir(path, 0700) == 0);
    fileman_tree_refresh_current();
    assert(!by_label(1, "later") && by_label(1, "kept"));

    /* Showing hidden files adds .hidden, but never the . and .. pseudoentries. */
    for (unsigned i = 0; i < fileman_tree_node_count; ++i)
        if (fileman_tree_nodes[i].id == 1) fileman_tree_nodes[i].expanded = 1;
    fileman_show_hidden = 1;
    fileman_tree_refresh_current();
    assert(by_label(1, ".hidden") && by_label(1, "later"));
    assert(!by_label(1, ".") && !by_label(1, ".."));

    char empty[RELIEFOS_FS_PATH_LEN];
    join(empty, sizeof(empty), argv[1], "empty");
    assert(mkdir(empty, 0700) == 0);
    copy_text(current_path, sizeof(current_path), empty);
    fileman_tree_node_count = 2;
    add_node(0, 1, 0, empty, "empty", 1, 1, 1);
    add_node(1, 2, 1, "/vanished", "vanished", 0, 0, 0);
    fileman_tree_refresh_current();
    assert(fileman_tree_node_count == 1 && by_id(1)->expanded && !by_id(1)->has_children);
    join(path, sizeof(path), empty, "fresh");
    assert(mkdir(path, 0700) == 0);
    fileman_tree_refresh_current();
    assert(by_id(1)->expanded && by_id(1)->has_children && by_label(1, "fresh"));

    /* An incomplete scan at the tree limit cannot prove a child disappeared. */
    char crowded[RELIEFOS_FS_PATH_LEN];
    join(crowded, sizeof(crowded), argv[1], "crowded");
    assert(mkdir(crowded, 0700) == 0);
    for (unsigned i = 0; i < FILEMAN_TREE_MAX_NODES + 1; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "child-%03u", i);
        join(path, sizeof(path), crowded, name);
        assert(mkdir(path, 0700) == 0);
    }
    copy_text(current_path, sizeof(current_path), crowded);
    fileman_tree_node_count = 2;
    add_node(0, 1, 0, crowded, "crowded", 1, 1, 1);
    add_node(1, 2, 1, "/not-seen", "not-seen", 0, 0, 0);
    fileman_tree_refresh_current();
    assert(by_id(2));
    puts("ok - expanded current tree reconciliation, retained subtrees, order, empty and limited scans");
    return 0;
}
