#ifndef RELIEFOS_FILEMAN_VIEW_MODEL_H
#define RELIEFOS_FILEMAN_VIEW_MODEL_H

#include <stdint.h>
#define FILEMAN_BUTTON_H 24u
#define FILEMAN_MENU_SEPARATOR 0x01u
#define FILEMAN_MENU_DISABLED 0x04u
#define FILEMAN_TREE_EXPANDED 0x01u
#define FILEMAN_TREE_LEAF 0x04u

struct fileman_rect {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
};

struct fileman_listview_state {
    uint32_t row_count;
    uint32_t visible_rows;
    uint32_t row_height;
    uint32_t scroll;
    int32_t selected;
    uint8_t focused;
};

struct fileman_context_menu_item {
    const char *label;
    uint32_t id;
    uint32_t flags;
};

struct fileman_tree_item {
    const char *label;
    uint32_t id;
    uint32_t depth;
    uint32_t flags;
};

void fileman_list_set_count(struct fileman_listview_state *state, uint32_t count);

#endif
