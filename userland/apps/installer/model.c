/* Installer wizard model: page flow shared by the Motif and TTY frontends. */
#include "model.h"

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

enum installer_page installer_model_next(enum installer_page page,
                                         enum installer_mode mode)
{
    switch (page) {
    case INSTALLER_PAGE_LANGUAGE:
        return INSTALLER_PAGE_THANKS;
    case INSTALLER_PAGE_THANKS:
        return INSTALLER_PAGE_WELCOME;
    case INSTALLER_PAGE_WELCOME:
        return INSTALLER_PAGE_MODE;
    case INSTALLER_PAGE_MODE:
        return INSTALLER_PAGE_DISK;
    case INSTALLER_PAGE_DISK:
        return mode == INSTALLER_MODE_UPDATE ? INSTALLER_PAGE_CONFIRM
                                             : INSTALLER_PAGE_PARTITIONS;
    case INSTALLER_PAGE_PARTITIONS:
        return INSTALLER_PAGE_ACCOUNTS;
    case INSTALLER_PAGE_ACCOUNTS:
        return INSTALLER_PAGE_CONFIRM;
    case INSTALLER_PAGE_CONFIRM:
        return INSTALLER_PAGE_PROGRESS;
    default:
        return page;
    }
}

enum installer_page installer_model_prev(enum installer_page page,
                                         enum installer_mode mode)
{
    switch (page) {
    case INSTALLER_PAGE_THANKS:
        return INSTALLER_PAGE_LANGUAGE;
    case INSTALLER_PAGE_WELCOME:
        return INSTALLER_PAGE_THANKS;
    case INSTALLER_PAGE_MODE:
        return INSTALLER_PAGE_WELCOME;
    case INSTALLER_PAGE_DISK:
        return INSTALLER_PAGE_MODE;
    case INSTALLER_PAGE_PARTITIONS:
        return INSTALLER_PAGE_DISK;
    case INSTALLER_PAGE_CONFIRM:
        return mode == INSTALLER_MODE_UPDATE ? INSTALLER_PAGE_DISK
                                             : INSTALLER_PAGE_ACCOUNTS;
    case INSTALLER_PAGE_ACCOUNTS:
        return INSTALLER_PAGE_PARTITIONS;
    case INSTALLER_PAGE_FINISH:
        return INSTALLER_PAGE_DISK;
    default:
        return page;
    }
}

int installer_model_can_go_back(enum installer_page page, int install_success)
{
    return !(page == INSTALLER_PAGE_LANGUAGE || page == INSTALLER_PAGE_PROGRESS ||
             (page == INSTALLER_PAGE_FINISH && install_success));
}

int installer_model_can_cancel(enum installer_page page, int install_success)
{
    return !(page == INSTALLER_PAGE_PROGRESS ||
             (page == INSTALLER_PAGE_FINISH && install_success));
}

enum installer_action installer_model_action(enum installer_page page,
                                             enum installer_mode mode,
                                             int install_success)
{
    if (page == INSTALLER_PAGE_CONFIRM) {
        return mode == INSTALLER_MODE_UPDATE ? INSTALLER_ACTION_UPDATE
                                             : INSTALLER_ACTION_INSTALL;
    }
    if (page == INSTALLER_PAGE_FINISH) {
        return install_success ? INSTALLER_ACTION_RESTART : INSTALLER_ACTION_CLOSE;
    }
    return INSTALLER_ACTION_NEXT;
}

const char *installer_model_confirm_word(enum installer_mode mode)
{
    return mode == INSTALLER_MODE_UPDATE ? "UPDATE" : "INSTALL";
}

int installer_model_steps(enum installer_mode mode, enum installer_page *out,
                          int cap)
{
    int count = 0;
    enum installer_page page;
    for (page = INSTALLER_PAGE_LANGUAGE; page < INSTALLER_PAGE_COUNT; ++page) {
        if ((page == INSTALLER_PAGE_ACCOUNTS || page == INSTALLER_PAGE_PARTITIONS) &&
            mode == INSTALLER_MODE_UPDATE)
            continue;
        if (count < cap)
            out[count] = page;
        ++count;
    }
    return count;
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
    append_text(buf, cap, &pos, "Disk ");
    append_number(buf, cap, &pos, id);
    append_text(buf, cap, &pos, "  ");
    append_text(buf, cap, &pos, (name && name[0]) ? name : "Disk");
    append_text(buf, cap, &pos, "  ");
    append_number(buf, cap, &pos, size);
    append_text(buf, cap, &pos, suffix);
}
