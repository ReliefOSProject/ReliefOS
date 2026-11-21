/* Installer wizard model: page flow shared by the FLTK and TTY frontends. */
#include "model.h"

#include <libintl.h>
#include <stddef.h>
#include <string.h>

int installer_model_secret_characters(const char *text, uint32_t length)
{
    uint32_t offset = 0;
    int count = 0;
    if (length && !text) return -1;
    while (offset < length) {
        uint32_t codepoint = (unsigned char)text[offset++];
        unsigned remaining = 0;
        uint32_t minimum = 0;
        if (codepoint >= 0x80) {
            if (codepoint >= 0xc2 && codepoint <= 0xdf) {
                remaining = 1; minimum = 0x80; codepoint &= 0x1f;
            } else if (codepoint >= 0xe0 && codepoint <= 0xef) {
                remaining = 2; minimum = 0x800; codepoint &= 0x0f;
            } else if (codepoint >= 0xf0 && codepoint <= 0xf4) {
                remaining = 3; minimum = 0x10000; codepoint &= 0x07;
            } else return -1;
            while (remaining--) {
                if (offset >= length) return -1;
                unsigned char next = text[offset++];
                if ((next & 0xc0) != 0x80) return -1;
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return -1;
        }
        if (codepoint < 33 || codepoint == 0x7f || codepoint == 0x85 ||
            codepoint == 0xa0 || codepoint == 0x1680 ||
            (codepoint >= 0x2000 && codepoint <= 0x200a) ||
            codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202f ||
            codepoint == 0x205f || codepoint == 0x3000) return -1;
        ++count;
    }
    return count;
}

int installer_model_edit_secret(char *secret, uint32_t cap, uint32_t start,
                                uint32_t end, const char *text, uint32_t length)
{
    uint32_t used, start_byte = 0, end_byte = 0;
    int count, inserted;
    if (!secret || !cap || (length && !text)) return 0;
    for (used = 0; used < cap && secret[used]; ++used) {}
    if (used == cap || start > end) return 0;
    count = installer_model_secret_characters(secret, used);
    inserted = installer_model_secret_characters(text, length);
    if (count < 0 || inserted < 0 || end > (uint32_t)count ||
        count - (end - start) + inserted > 32) return 0;
    for (uint32_t i = 0; i < end; ++i) {
        if (i == start) start_byte = end_byte;
        ++end_byte;
        while (end_byte < used && ((unsigned char)secret[end_byte] & 0xc0) == 0x80)
            ++end_byte;
    }
    if (start == end) start_byte = end_byte;
    if (length >= cap || used - (end_byte - start_byte) >= cap - length) return 0;
    memmove(secret + start_byte + length, secret + end_byte, used - end_byte + 1);
    if (length) memcpy(secret + start_byte, text, length);
    memset(secret + used - (end_byte - start_byte) + length, 0,
           cap - (used - (end_byte - start_byte) + length));
    return 1;
}

static const enum installer_page fresh_steps[] = {
    INSTALLER_PAGE_LANGUAGE, INSTALLER_PAGE_LICENSE, INSTALLER_PAGE_MODE,
    INSTALLER_PAGE_DISK, INSTALLER_PAGE_BOOTLOADERS, INSTALLER_PAGE_PARTITIONS,
    INSTALLER_PAGE_SYSTEM, INSTALLER_PAGE_ACCOUNTS, INSTALLER_PAGE_CONFIRM,
    INSTALLER_PAGE_PROGRESS, INSTALLER_PAGE_FINISH
};
static const enum installer_page update_steps[] = {
    INSTALLER_PAGE_LANGUAGE, INSTALLER_PAGE_LICENSE, INSTALLER_PAGE_MODE,
    INSTALLER_PAGE_DISK, INSTALLER_PAGE_CONFIRM, INSTALLER_PAGE_PROGRESS,
    INSTALLER_PAGE_FINISH
};

int installer_model_steps(enum installer_mode mode, enum installer_page *out, int cap)
{
    const enum installer_page *steps = mode == INSTALLER_MODE_UPDATE ? update_steps : fresh_steps;
    int count = mode == INSTALLER_MODE_UPDATE
        ? (int)(sizeof(update_steps) / sizeof(update_steps[0]))
        : (int)(sizeof(fresh_steps) / sizeof(fresh_steps[0]));
    if (out && cap > 0)
        for (int i = 0; i < count && i < cap; ++i) out[i] = steps[i];
    return count;
}

int installer_model_step_index(enum installer_page page, enum installer_mode mode)
{
    enum installer_page steps[INSTALLER_PAGE_COUNT];
    int count = installer_model_steps(mode, steps, INSTALLER_PAGE_COUNT);
    for (int i = 0; i < count; ++i) if (steps[i] == page) return i;
    return -1;
}

enum installer_page installer_model_next(enum installer_page page, enum installer_mode mode)
{
    if (page == INSTALLER_PAGE_WELCOME) return INSTALLER_PAGE_MODE;
    if (page == INSTALLER_PAGE_PROGRESS || page == INSTALLER_PAGE_FINISH) return page;
    enum installer_page steps[INSTALLER_PAGE_COUNT];
    int count = installer_model_steps(mode, steps, INSTALLER_PAGE_COUNT);
    int index = installer_model_step_index(page, mode);
    return index >= 0 && index + 1 < count ? steps[index + 1] : page;
}

enum installer_page installer_model_prev(enum installer_page page, enum installer_mode mode)
{
    if (page == INSTALLER_PAGE_WELCOME) return INSTALLER_PAGE_LICENSE;
    if (page == INSTALLER_PAGE_FINISH) return INSTALLER_PAGE_DISK;
    if (page == INSTALLER_PAGE_PROGRESS) return page;
    enum installer_page steps[INSTALLER_PAGE_COUNT];
    installer_model_steps(mode, steps, INSTALLER_PAGE_COUNT);
    int index = installer_model_step_index(page, mode);
    return index > 0 ? steps[index - 1] : page;
}

int installer_model_can_go_back(enum installer_page page, int install_success)
{
    return page >= 0 && page < INSTALLER_PAGE_COUNT &&
        !(page == INSTALLER_PAGE_LANGUAGE || page == INSTALLER_PAGE_PROGRESS ||
          (page == INSTALLER_PAGE_FINISH && install_success));
}

int installer_model_can_cancel(enum installer_page page, int install_success)
{
    return page >= 0 && page < INSTALLER_PAGE_COUNT &&
        !(page == INSTALLER_PAGE_PROGRESS ||
          (page == INSTALLER_PAGE_FINISH && install_success));
}

int installer_model_can_visit(enum installer_page current, enum installer_page target,
                              enum installer_mode mode, int install_success)
{
    int source = installer_model_step_index(current, mode);
    int destination = installer_model_step_index(target, mode);
    int confirm = installer_model_step_index(INSTALLER_PAGE_CONFIRM, mode);
    return source >= 0 && destination >= 0 && destination < source && destination <= confirm &&
        current != INSTALLER_PAGE_PROGRESS && !(current == INSTALLER_PAGE_FINISH && install_success);
}

enum installer_action installer_model_action(enum installer_page page, enum installer_mode mode,
                                             int install_success)
{
    if (page == INSTALLER_PAGE_CONFIRM)
        return mode == INSTALLER_MODE_UPDATE ? INSTALLER_ACTION_UPDATE : INSTALLER_ACTION_INSTALL;
    if (page == INSTALLER_PAGE_FINISH)
        return install_success ? INSTALLER_ACTION_RESTART : INSTALLER_ACTION_RETRY;
    return INSTALLER_ACTION_NEXT;
}

const char *installer_model_confirm_word(enum installer_mode mode)
{
    return mode == INSTALLER_MODE_UPDATE ? "" : "INSTALL";
}

const char *installer_model_page_title(enum installer_page page, enum installer_mode mode)
{
    switch (page) {
    case INSTALLER_PAGE_LANGUAGE: return gettext("Language");
    case INSTALLER_PAGE_LICENSE: return gettext("License");
    case INSTALLER_PAGE_MODE: case INSTALLER_PAGE_WELCOME: return gettext("Installation mode");
    case INSTALLER_PAGE_DISK: return gettext("Target disk");
    case INSTALLER_PAGE_BOOTLOADERS: return gettext("Bootloaders");
    case INSTALLER_PAGE_PARTITIONS: return gettext("Partition layout");
    case INSTALLER_PAGE_SYSTEM: return gettext("System options");
    case INSTALLER_PAGE_ACCOUNTS: return gettext("Accounts");
    case INSTALLER_PAGE_CONFIRM:
        return mode == INSTALLER_MODE_UPDATE ? gettext("Review system update") : gettext("Review installation");
    case INSTALLER_PAGE_PROGRESS:
        return mode == INSTALLER_MODE_UPDATE ? gettext("Updating system") : gettext("Installing system");
    case INSTALLER_PAGE_FINISH: return gettext("Installation result");
    default: return gettext("Installation");
    }
}

static void append_text(char *buf, uint32_t cap, uint32_t *pos, const char *text)
{
    for (; *text && *pos + 1 < cap; ++text)
        buf[(*pos)++] = *text;
    buf[*pos] = 0;
}

static void append_number(char *buf, uint32_t cap, uint32_t *pos, uint64_t value)
{
    char digits[24];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count && *pos + 1 < cap)
        buf[(*pos)++] = digits[--count];
    buf[*pos] = 0;
}

void installer_model_format_disk_line(char *buf, uint32_t cap, uint32_t id,
                                      const char *name, uint64_t sector_count,
                                      uint32_t sector_size)
{
    uint32_t pos = 0;
    uint64_t size = 0;
    const char *suffix = " MiB";

    if (!buf || !cap)
        return;
    buf[0] = 0;
    if (sector_size)
        size = (sector_count * (uint64_t)sector_size) / (1024ULL * 1024ULL);
    if (size >= 1024ULL) {
        size /= 1024ULL;
        suffix = " GiB";
    }
    append_text(buf, cap, &pos, gettext("Disk "));
    append_number(buf, cap, &pos, id);
    append_text(buf, cap, &pos, "  ");
    append_text(buf, cap, &pos, (name && name[0]) ? name : gettext("Disk"));
    append_text(buf, cap, &pos, "  ");
    append_number(buf, cap, &pos, size);
    append_text(buf, cap, &pos, suffix);
}
