#include "library.h"

#include <string.h>

namespace rm {
namespace {

bool ascii_letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

bool utf8_boundary(const char *source, size_t size, size_t at) noexcept {
    return at <= size &&
           (at == size || (static_cast<unsigned char>(source[at]) & 0xc0) != 0x80);
}

bool contains_ascii_fold(const char *text, const char *query) noexcept {
    if (!query || !*query) return true;
    if (!text) return false;
    const size_t query_size = strlen(query);
    for (size_t start = 0; text[start]; ++start) {
        size_t at = 0;
        while (at < query_size && text[start + at]) {
            unsigned char left = static_cast<unsigned char>(text[start + at]);
            unsigned char right = static_cast<unsigned char>(query[at]);
            if (left >= 'A' && left <= 'Z') left = static_cast<unsigned char>(left + 'a' - 'A');
            if (right >= 'A' && right <= 'Z') right = static_cast<unsigned char>(right + 'a' - 'A');
            if (left != right) break;
            ++at;
        }
        if (at == query_size) return true;
    }
    return false;
}

const char *entry_chinese_terms(const ToolEntry &entry) noexcept {
    const char *key = entry.key;
    if (!key) return "";
    if (!strncmp(key, "greek.", 6)) return "希腊字母 希腊符号";
    if (!strncmp(key, "operator.", 9)) {
        if (!strcmp(key, "operator.plus")) return "运算符 加号";
        if (!strcmp(key, "operator.minus")) return "运算符 减号";
        if (!strcmp(key, "operator.plus-minus")) return "运算符 正负号 加减号";
        if (!strcmp(key, "operator.minus-plus")) return "运算符 负正号 减加号";
        if (!strcmp(key, "operator.times")) return "运算符 乘号 乘法";
        if (!strcmp(key, "operator.dot")) return "运算符 点乘 居中点";
        if (!strcmp(key, "operator.divide")) return "运算符 除号 除法";
        if (!strcmp(key, "operator.star")) return "运算符 星乘 星号";
        return "运算符";
    }
    if (!strncmp(key, "relation.", 9)) {
        if (!strcmp(key, "relation.equals")) return "关系符 等于 相等";
        if (!strcmp(key, "relation.less")) return "关系符 小于";
        if (!strcmp(key, "relation.greater")) return "关系符 大于";
        if (!strcmp(key, "relation.less-equal")) return "关系符 小于等于";
        if (!strcmp(key, "relation.greater-equal")) return "关系符 大于等于";
        if (!strcmp(key, "relation.not-equal")) return "关系符 不等 不等于";
        if (!strcmp(key, "relation.approx")) return "关系符 约等 近似";
        if (!strcmp(key, "relation.equivalent")) return "关系符 等价";
        if (!strcmp(key, "relation.similar")) return "关系符 相似";
        if (!strcmp(key, "relation.proportional")) return "关系符 正比 成比例";
        if (!strcmp(key, "relation.much-less")) return "关系符 远小于";
        if (!strcmp(key, "relation.much-greater")) return "关系符 远大于";
        if (!strcmp(key, "relation.perpendicular")) return "关系符 垂直 正交";
        if (!strcmp(key, "relation.parallel")) return "关系符 平行";
        return "关系符";
    }
    if (!strncmp(key, "arrow.", 6)) return "箭头 映射";
    if (!strncmp(key, "set.", 4)) return "集合 属于 子集 并集 交集";
    if (!strncmp(key, "bracket.", 8)) return "括号 方括号 花括号 定界符";
    if (!strncmp(key, "spacing.", 8)) return "间距 空白";
    if (!strcmp(key, "structure.fraction") ||
        !strcmp(key, "structure.display-fraction")) return "分数 分子 分母";
    if (!strcmp(key, "structure.root") || !strcmp(key, "structure.indexed-root"))
        return "根式 根号 开方";
    if (!strcmp(key, "structure.superscript")) return "上标 脚标";
    if (!strcmp(key, "structure.subscript")) return "下标 脚标";
    if (!strncmp(key, "structure.auto-", 15) ||
        !strcmp(key, "structure.absolute-value") ||
        !strcmp(key, "structure.norm")) return "括号 定界符 绝对值 范数";
    if (!strcmp(key, "structure.sum")) return "求和 求和号";
    if (!strcmp(key, "structure.product")) return "连乘 求积";
    if (!strncmp(key, "structure.", 10) && strstr(key, "integral"))
        return "积分 重积分 闭合积分";
    if (!strcmp(key, "structure.limit")) return "极限";
    if (!strncmp(key, "function.", 9)) {
        if (!strcmp(key, "function.sin")) return "函数 正弦";
        if (!strcmp(key, "function.cos")) return "函数 余弦";
        if (!strcmp(key, "function.tan")) return "函数 正切";
        if (!strcmp(key, "function.log")) return "函数 对数";
        if (!strcmp(key, "function.ln")) return "函数 自然对数";
        if (!strcmp(key, "function.exp")) return "函数 指数";
        if (!strcmp(key, "function.min")) return "函数 最小值";
        if (!strcmp(key, "function.max")) return "函数 最大值";
        return "函数";
    }
    if (!strcmp(key, "structure.matrix") ||
        !strcmp(key, "structure.parenthesized-matrix") ||
        !strcmp(key, "structure.bracketed-matrix")) return "矩阵 行 列";
    if (!strcmp(key, "structure.cases")) return "分段函数 分类讨论 cases";
    if (!strcmp(key, "structure.aligned")) return "对齐 多行公式 aligned";
    if (!strcmp(key, "structure.vector") || !strcmp(key, "structure.hat") ||
        !strcmp(key, "structure.overline")) return "向量 帽子 横线 重音";
    if (!strcmp(key, "structure.text")) return "文本 文字 中文";
    if (!strcmp(key, "structure.roman") || !strcmp(key, "structure.bold") ||
        !strcmp(key, "structure.italic")) return "数学字体 罗马 粗体 斜体";
    return "";
}

size_t codepoint_size(unsigned char first) noexcept {
    if (first >= 0xf0) return 4;
    if (first >= 0xe0) return 3;
    if (first >= 0xc0) return 2;
    return 1;
}

bool append_limited(Bytes &bytes, const char *source, size_t size,
                    Error &error) noexcept {
    if (!size) return true;
    if (bytes.size() > kMaxTextBytes || size > kMaxTextBytes - bytes.size()) {
        error.set(Code::Limit, "replacement result exceeds the source limit");
        return false;
    }
    if (!bytes.append(source, size)) {
        error.set(Code::NoMemory, "could not allocate replacement result");
        return false;
    }
    return true;
}

} // namespace

FavoriteCloseDecision favorite_close_decision(bool close_requested,
                                               bool io_pending, bool dirty,
                                               bool save_failed) noexcept {
    if (!close_requested) return FavoriteCloseDecision::None;
    if (io_pending) return FavoriteCloseDecision::WaitForIo;
    if (!dirty) return FavoriteCloseDecision::Close;
    if (save_failed) return FavoriteCloseDecision::ConfirmDiscard;
    return FavoriteCloseDecision::Save;
}

unsigned structure_column_limit(Tool tool) noexcept {
    return tool == Tool::Cases ? 2u : 16u;
}

bool command_completion_prefix(const char *source, size_t caret, Text &prefix,
                              Range &replace_range, Error &error) noexcept {
    if (!source) {
        error.set(Code::Format, "source text is missing");
        return false;
    }
    const size_t size = strlen(source);
    size_t bad_offset = 0;
    if (!detail::valid_utf8(source, size, &bad_offset)) {
        error.set(Code::InvalidUtf8, "source is not valid UTF-8", bad_offset,
                  bad_offset + 1);
        return false;
    }
    if (caret > size || !utf8_boundary(source, size, caret)) {
        error.set(Code::Format, "caret is not on a UTF-8 boundary", caret, caret);
        return false;
    }

    size_t left = caret;
    while (left && ascii_letter(source[left - 1])) --left;
    if (!left || source[left - 1] != '\\') {
        error.set(Code::NotFound, "place the caret in a backslash command");
        return false;
    }

    const size_t slash = left - 1;
    size_t preceding_slashes = 0;
    size_t before = slash;
    while (before && source[before - 1] == '\\') {
        --before;
        ++preceding_slashes;
    }
    if (preceding_slashes & 1u) {
        error.set(Code::NotFound, "the backslash is escaped");
        return false;
    }

    size_t right = caret;
    while (right < size && ascii_letter(source[right])) ++right;
    Text candidate;
    if (!candidate.set(source + slash, caret - slash)) {
        error.set(Code::NoMemory, "could not store the completion prefix");
        return false;
    }
    prefix.swap(candidate);
    replace_range = {slash, right};
    error.clear();
    return true;
}

bool replace_all_utf8(const char *source, const char *needle,
                      const char *replacement, Text &output,
                      size_t &replace_count, Error &error) noexcept {
    replace_count = 0;
    if (!source || !needle || !replacement) {
        error.set(Code::Format, "find and replace text must be provided");
        return false;
    }
    const size_t source_size = strlen(source);
    const size_t needle_size = strlen(needle);
    const size_t replacement_size = strlen(replacement);
    size_t bad_offset = 0;
    if (!detail::valid_utf8(source, source_size, &bad_offset) ||
        !detail::valid_utf8(needle, needle_size, &bad_offset) ||
        !detail::valid_utf8(replacement, replacement_size, &bad_offset)) {
        error.set(Code::InvalidUtf8, "find and replace text must be valid UTF-8",
                  bad_offset, bad_offset + 1);
        return false;
    }
    if (!needle_size) {
        error.set(Code::Format, "search text cannot be empty");
        return false;
    }

    Bytes bytes;
    size_t scan = 0;
    size_t copied = 0;
    size_t found = 0;
    while (scan <= source_size && needle_size <= source_size - scan) {
        if (memcmp(source + scan, needle, needle_size) == 0) {
            if (!append_limited(bytes, source + copied, scan - copied, error) ||
                !append_limited(bytes, replacement, replacement_size, error))
                return false;
            scan += needle_size;
            copied = scan;
            ++found;
        } else {
            scan += codepoint_size(static_cast<unsigned char>(source[scan]));
        }
    }
    if (!append_limited(bytes, source + copied, source_size - copied, error))
        return false;

    Text candidate;
    if (!candidate.set(reinterpret_cast<const char *>(bytes.data()), bytes.size())) {
        error.set(Code::NoMemory, "could not store replacement result");
        return false;
    }
    output.swap(candidate);
    replace_count = found;
    error.clear();
    return true;
}

bool catalog_entry_matches(const ToolEntry &entry, const char *localized_name,
                           const char *localized_category,
                           const char *query) noexcept {
    if (!query || !*query) return true;
    return contains_ascii_fold(localized_name, query) ||
           contains_ascii_fold(localized_category, query) ||
           contains_ascii_fold(entry.key, query) ||
           contains_ascii_fold(entry.msgid, query) ||
           contains_ascii_fold(entry.category, query) ||
           contains_ascii_fold(entry.command, query) ||
           contains_ascii_fold(entry.insert, query) ||
           contains_ascii_fold(entry.parameters, query) ||
           contains_ascii_fold(entry.help_example, query) ||
           contains_ascii_fold(catalog_category_zh(entry.category), query) ||
           contains_ascii_fold(entry_chinese_terms(entry), query);
}

const char *catalog_category_zh(const char *category) noexcept {
    if (!category) return "其他";
    if (!strcmp(category, "Greek letters")) return "希腊字母";
    if (!strcmp(category, "Binary operators")) return "二元运算";
    if (!strcmp(category, "Relations")) return "关系符";
    if (!strcmp(category, "Arrows")) return "箭头";
    if (!strcmp(category, "Sets")) return "集合";
    if (!strcmp(category, "Brackets")) return "括号";
    if (!strcmp(category, "Spacing")) return "间距";
    if (!strcmp(category, "Fractions")) return "分数";
    if (!strcmp(category, "Roots")) return "根式";
    if (!strcmp(category, "Scripts")) return "上下标";
    if (!strcmp(category, "Delimiters")) return "定界符";
    if (!strcmp(category, "Big operators")) return "大型运算";
    if (!strcmp(category, "Functions")) return "函数";
    if (!strcmp(category, "Matrices")) return "矩阵";
    if (!strcmp(category, "Accents")) return "重音符号";
    if (!strcmp(category, "Text and fonts")) return "文字与字体";
    return "其他";
}

} // namespace rm
