#include "model.h"
#include <math.h>
#include <stdint.h>

namespace rp {
namespace {

constexpr float BoundsEpsilon = 0.02f;
constexpr size_t HistoryLimit = 64;
constexpr size_t HistoryBudget = 64u * 1024u * 1024u;

bool set_error(Text &error, const char *message) {
    error.set(message, strlen(message));
    return false;
}

bool decode_one(const char *bytes, size_t length, size_t &at, uint32_t &scalar) {
    if (at >= length) return false;
    const unsigned char first = static_cast<unsigned char>(bytes[at]);
    if (first < 0x80) {
        scalar = first;
        ++at;
        return true;
    }
    unsigned count = 0;
    uint32_t value = 0;
    if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; }
    else return false;
    if (count > length - at) return false;
    for (unsigned n = 1; n < count; ++n) {
        const unsigned char continuation = static_cast<unsigned char>(bytes[at + n]);
        if ((continuation & 0xc0) != 0x80) return false;
        if (n == 1 && ((first == 0xe0 && continuation < 0xa0) ||
                       (first == 0xed && continuation > 0x9f) ||
                       (first == 0xf0 && continuation < 0x90) ||
                       (first == 0xf4 && continuation > 0x8f))) return false;
        value = (value << 6) | (continuation & 0x3f);
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    at += count;
    scalar = value;
    return true;
}

bool text_controls_valid(uint32_t scalar) {
    if (scalar == 0) return false;
    if (scalar < 0x20 && scalar != '\n' && scalar != '\t') return false;
    return scalar < 0x7f || scalar > 0x9f;
}

bool finite_rect(const Rect &rect) {
    return isfinite(rect.x) && isfinite(rect.y) && isfinite(rect.w) && isfinite(rect.h);
}

bool valid_style(const Style &style) {
    return style.text <= 0xffffff && style.fill <= 0xffffff && style.stroke <= 0xffffff &&
        isfinite(style.font_size) && style.font_size >= 6 && style.font_size <= 144 &&
        isfinite(style.line_width) && style.line_width >= 0 && style.line_width <= 20 &&
        style.family <= 2 && static_cast<unsigned>(style.alignment) <=
            static_cast<unsigned>(Alignment::Right);
}

bool valid_rect(const Rect &rect, float page_width, float page_height) {
    if (!finite_rect(rect) || rect.w < 4 || rect.h < 4) return false;
    return rect.x >= -BoundsEpsilon && rect.y >= -BoundsEpsilon &&
        rect.x + rect.w <= page_width + BoundsEpsilon &&
        rect.y + rect.h <= page_height + BoundsEpsilon;
}

bool same_rect(const Rect &a, const Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool same_style(const Style &a, const Style &b) {
    return a.text == b.text && a.fill == b.fill && a.stroke == b.stroke &&
        a.font_size == b.font_size && a.line_width == b.line_width && a.family == b.family &&
        a.bold == b.bold && a.italic == b.italic && a.underline == b.underline &&
        a.bullets == b.bullets && a.filled == b.filled && a.outlined == b.outlined &&
        a.alignment == b.alignment;
}

bool same_object(const Object &a, const Object &b) {
    return a.id == b.id && a.kind == b.kind && same_rect(a.rect, b.rect) &&
        same_style(a.style, b.style) && a.text == b.text && a.asset == b.asset;
}

bool same_slide(const Slide &a, const Slide &b) {
    if (a.background != b.background || a.notes != b.notes ||
        a.objects.size() != b.objects.size()) return false;
    for (size_t i = 0; i < a.objects.size(); ++i)
        if (!same_object(a.objects[i], b.objects[i])) return false;
    return true;
}

bool same_asset(const Asset &a, const Asset &b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}

bool same_deck_content(const Deck &a, const Deck &b) {
    if (a.width != b.width || a.height != b.height || a.next_id != b.next_id ||
        a.title != b.title || a.author != b.author || a.slides.size() != b.slides.size() ||
        a.assets.size() != b.assets.size()) return false;
    for (size_t i = 0; i < a.slides.size(); ++i)
        if (!same_slide(a.slides[i], b.slides[i])) return false;
    for (size_t i = 0; i < a.assets.size(); ++i)
        if (!same_asset(a.assets[i], b.assets[i])) return false;
    return true;
}

int compare_u32(const void *a, const void *b) {
    const uint32_t left = *static_cast<const uint32_t *>(a);
    const uint32_t right = *static_cast<const uint32_t *>(b);
    return left < right ? -1 : left > right ? 1 : 0;
}

bool add_size(size_t a, size_t b, size_t &result) {
    if (b > SIZE_MAX - a) return false;
    result = a + b;
    return true;
}

bool selected_id(const Array<uint32_t> &ids, uint32_t id) {
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return true;
    return false;
}

bool insert_slide(Array<Slide> &slides, size_t at, Slide &&inserted) {
    if (at > slides.size()) return false;
    Array<Slide> next;
    if (!next.reserve(slides.size() + 1)) return false;
    for (size_t i = 0; i <= slides.size(); ++i) {
        if (i == at && !next.push(static_cast<Slide &&>(inserted))) return false;
        if (i < slides.size()) {
            Slide copy;
            if (!copy.clone_from(slides[i]) || !next.push(static_cast<Slide &&>(copy))) return false;
        }
    }
    slides.swap(next);
    return true;
}

bool make_layout_slide(Layout layout, Deck &deck, Slide &slide) {
    if (static_cast<unsigned>(layout) > static_cast<unsigned>(Layout::TwoColumns)) return false;
    if (layout == Layout::Blank) return true;
    if (!deck.next_id || deck.next_id == UINT32_MAX) return false;
    Object title;
    title.id = deck.next_id++;
    title.kind = Kind::Text;
    title.rect = {64, 32, deck.width - 128, 88};
    title.style.font_size = 36;
    title.style.bold = true;
    if (!title.text.set("标题", strlen("标题")) || !slide.objects.push(static_cast<Object &&>(title))) return false;
    if (layout == Layout::Title) return true;

    if (layout == Layout::Content) {
        if (!deck.next_id || deck.next_id == UINT32_MAX) return false;
        Object content;
        content.id = deck.next_id++;
        content.kind = Kind::Text;
        content.rect = {80, 150, deck.width - 160, deck.height - 200};
        content.style.bullets = true;
        return content.text.set("在此输入内容", strlen("在此输入内容")) &&
            slide.objects.push(static_cast<Object &&>(content));
    }
    if (deck.next_id >= UINT32_MAX - 2) return false;
    Object left;
    left.id = deck.next_id++;
    left.kind = Kind::Text;
    left.rect = {64, 150, deck.width * 0.40f, deck.height - 200};
    left.style.bullets = true;
    if (!left.text.set("左侧内容", strlen("左侧内容")) || !slide.objects.push(static_cast<Object &&>(left))) return false;
    Object right;
    right.id = deck.next_id++;
    right.kind = Kind::Text;
    right.rect = {deck.width * 0.55f, 150, deck.width * 0.40f, deck.height - 200};
    right.style.bullets = true;
    return right.text.set("右侧内容", strlen("右侧内容")) &&
        slide.objects.push(static_cast<Object &&>(right));
}

bool next_id(Deck &deck, uint32_t &id) {
    if (!deck.next_id || deck.next_id == UINT32_MAX) return false;
    id = deck.next_id++;
    return true;
}

bool next_paste_id(Deck &deck, const Slide &fragment, uint32_t &id) {
    while (deck.next_id && deck.next_id < UINT32_MAX) {
        const uint32_t candidate = deck.next_id++;
        bool collides = false;
        for (size_t i = 0; i < fragment.objects.size(); ++i)
            if (fragment.objects[i].id == candidate) { collides = true; break; }
        if (!collides) { id = candidate; return true; }
    }
    return false;
}

void clamp_group_offset(const Slide &slide, float page_width, float page_height,
                        float &dx, float &dy) {
    if (slide.objects.empty()) { dx = 0; dy = 0; return; }
    float min_x = slide.objects[0].rect.x;
    float min_y = slide.objects[0].rect.y;
    float max_x = min_x + slide.objects[0].rect.w;
    float max_y = min_y + slide.objects[0].rect.h;
    for (size_t i = 1; i < slide.objects.size(); ++i) {
        const Rect &r = slide.objects[i].rect;
        if (r.x < min_x) min_x = r.x;
        if (r.y < min_y) min_y = r.y;
        if (r.x + r.w > max_x) max_x = r.x + r.w;
        if (r.y + r.h > max_y) max_y = r.y + r.h;
    }
    if (max_x + dx > page_width) dx = page_width - max_x;
    if (min_x + dx < 0) dx = -min_x;
    if (max_y + dy > page_height) dy = page_height - max_y;
    if (min_y + dy < 0) dy = -min_y;
}

void clamp_selected_offset(const Slide &slide, const Array<uint32_t> &selected,
                           float page_width, float page_height, float &dx, float &dy) {
    bool first = true;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (size_t i = 0; i < slide.objects.size(); ++i) {
        const Object &object = slide.objects[i];
        if (!selected_id(selected, object.id)) continue;
        const Rect &r = object.rect;
        if (first) { min_x = r.x; min_y = r.y; max_x = r.x + r.w; max_y = r.y + r.h; first = false; }
        else {
            if (r.x < min_x) min_x = r.x;
            if (r.y < min_y) min_y = r.y;
            if (r.x + r.w > max_x) max_x = r.x + r.w;
            if (r.y + r.h > max_y) max_y = r.y + r.h;
        }
    }
    if (first) { dx = 0; dy = 0; return; }
    if (max_x + dx > page_width) dx = page_width - max_x;
    if (min_x + dx < 0) dx = -min_x;
    if (max_y + dy > page_height) dy = page_height - max_y;
    if (min_y + dy < 0) dy = -min_y;
}

bool object_id_exists(const Deck &deck, uint32_t id) {
    for (size_t s = 0; s < deck.slides.size(); ++s)
        for (size_t i = 0; i < deck.slides[s].objects.size(); ++i)
            if (deck.slides[s].objects[i].id == id) return true;
    return false;
}

bool compact_assets(Deck &deck) {
    if (deck.assets.size() > MaxAssets) return false;
    uint32_t asset_map[MaxAssets];
    for (size_t i = 0; i < MaxAssets; ++i) asset_map[i] = UINT32_MAX;
    Array<Asset> referenced;
    for (size_t s = 0; s < deck.slides.size(); ++s) {
        const Slide &slide = deck.slides[s];
        for (size_t i = 0; i < slide.objects.size(); ++i) {
            const Object &object = slide.objects[i];
            if (object.kind != Kind::Image) continue;
            if (object.asset >= deck.assets.size()) return false;
            if (asset_map[object.asset] != UINT32_MAX) continue;
            asset_map[object.asset] = static_cast<uint32_t>(referenced.size());
            if (!referenced.push(deck.assets[object.asset])) return false;
        }
    }
    for (size_t s = 0; s < deck.slides.size(); ++s) {
        Slide &slide = deck.slides[s];
        for (size_t i = 0; i < slide.objects.size(); ++i) {
            Object &object = slide.objects[i];
            if (object.kind == Kind::Image) object.asset = asset_map[object.asset];
        }
    }
    deck.assets.swap(referenced);
    return true;
}

bool clean_selection(const Deck &deck, size_t slide, const Array<uint32_t> &source,
                     Array<uint32_t> &clean) {
    if (slide >= deck.slides.size()) return false;
    for (size_t i = 0; i < source.size(); ++i) {
        const uint32_t id = source[i];
        if (id && deck.object(slide, id) && !selected_id(clean, id) && !clean.push(id)) return false;
    }
    return true;
}

bool add_text_bytes(size_t &total, const Text &text) {
    return add_size(total, text.size(), total);
}

} // namespace

bool valid_utf8(const char *bytes, size_t length) {
    if (!bytes && length) return false;
    size_t at = 0;
    while (at < length) {
        uint32_t scalar = 0;
        if (!decode_one(bytes, length, at, scalar) || !text_controls_valid(scalar)) return false;
    }
    return true;
}

bool Slide::clone_from(const Slide &other) {
    Array<Object> objects;
    if (!objects.clone_from(other.objects)) return false;
    Text notes(other.notes);
    background = other.background;
    this->notes = static_cast<Text &&>(notes);
    this->objects.swap(objects);
    return true;
}

void Deck::swap(Deck &other) noexcept {
    const float old_width = width; width = other.width; other.width = old_width;
    const float old_height = height; height = other.height; other.height = old_height;
    const uint32_t old_next = next_id; next_id = other.next_id; other.next_id = old_next;
    const uint64_t old_revision = revision; revision = other.revision; other.revision = old_revision;
    Text old_title(static_cast<Text &&>(title));
    title = static_cast<Text &&>(other.title);
    other.title = static_cast<Text &&>(old_title);
    Text old_author(static_cast<Text &&>(author));
    author = static_cast<Text &&>(other.author);
    other.author = static_cast<Text &&>(old_author);
    slides.swap(other.slides);
    assets.swap(other.assets);
}

bool Deck::init() {
    Deck next;
    next.width = 960;
    next.height = 540;
    next.next_id = 1;
    next.revision = 1;
    Slide blank;
    if (!next.slides.push(static_cast<Slide &&>(blank))) return false;
    swap(next);
    return true;
}

bool Deck::clone_from(const Deck &other) {
    Deck next;
    next.width = other.width;
    next.height = other.height;
    next.next_id = other.next_id;
    next.revision = other.revision;
    next.title = other.title;
    next.author = other.author;
    if (!next.slides.reserve(other.slides.size()) || !next.assets.reserve(other.assets.size())) return false;
    for (size_t i = 0; i < other.slides.size(); ++i) {
        Slide copy;
        if (!copy.clone_from(other.slides[i]) || !next.slides.push(static_cast<Slide &&>(copy))) return false;
    }
    for (size_t i = 0; i < other.assets.size(); ++i)
        if (!next.assets.push(other.assets[i])) return false;
    swap(next);
    return true;
}

Object *Deck::object(size_t slide, uint32_t id) {
    if (slide >= slides.size() || !id) return nullptr;
    Slide &page = slides[slide];
    for (size_t i = 0; i < page.objects.size(); ++i) if (page.objects[i].id == id) return &page.objects[i];
    return nullptr;
}

const Object *Deck::object(size_t slide, uint32_t id) const {
    if (slide >= slides.size() || !id) return nullptr;
    const Slide &page = slides[slide];
    for (size_t i = 0; i < page.objects.size(); ++i) if (page.objects[i].id == id) return &page.objects[i];
    return nullptr;
}

bool Deck::validate(Text &error) const {
    if (!isfinite(width) || !isfinite(height) || width < 240 || height < 240 ||
        width > 4000 || height > 4000) return set_error(error, "页面尺寸无效");
    if (slides.empty() || slides.size() > MaxSlides) return set_error(error, "幻灯片数量无效");
    if (assets.size() > MaxAssets) return set_error(error, "图片数量超限");
    if (!next_id || !revision) return set_error(error, "对象编号或修订号无效");
    if (title.size() > MaxTextBytes || author.size() > MaxTextBytes ||
        !valid_utf8(title.c_str(), title.size()) || !valid_utf8(author.c_str(), author.size()))
        return set_error(error, "文档信息文本无效");

    size_t object_count = 0;
    size_t byte_count = 0;
    if (!add_text_bytes(byte_count, title) || !add_text_bytes(byte_count, author))
        return set_error(error, "文档大小超限");
    size_t total_asset_bytes = 0;
    for (size_t a = 0; a < assets.size(); ++a) {
        const Asset &asset = assets[a];
        if (!asset.width || !asset.height || asset.width > 4096 || asset.height > 4096)
            return set_error(error, "图片尺寸无效");
        const uint64_t rgba_size = static_cast<uint64_t>(asset.width) * asset.height * 4u;
        if (rgba_size > MaxAssetBytes || rgba_size != asset.rgba.size())
            return set_error(error, "图片数据长度无效");
        if (rgba_size > SIZE_MAX - total_asset_bytes) return set_error(error, "图片数据超限");
        total_asset_bytes += static_cast<size_t>(rgba_size);
        if (total_asset_bytes > MaxAssetBytes) return set_error(error, "图片数据超限");
        if (!add_text_bytes(byte_count, asset.rgba)) return set_error(error, "文档大小超限");
    }
    if (byte_count > MaxFileBytes) return set_error(error, "文档大小超限");

    size_t id_bytes = 0;
    for (size_t s = 0; s < slides.size(); ++s) {
        const Slide &slide = slides[s];
        if (slide.background > 0xffffff || slide.objects.size() > MaxObjectsPerSlide)
            return set_error(error, "幻灯片属性无效");
        if (slide.objects.size() > MaxObjects - object_count)
            return set_error(error, "对象数量超限");
        object_count += slide.objects.size();
        if (!add_text_bytes(byte_count, slide.notes) || slide.notes.size() > MaxNotesBytes ||
            !valid_utf8(slide.notes.c_str(), slide.notes.size()))
            return set_error(error, "备注文本无效");
        for (size_t i = 0; i < slide.objects.size(); ++i) {
            const Object &object = slide.objects[i];
            if (!object.id || object.id == UINT32_MAX || !valid_rect(object.rect, width, height) ||
                !valid_style(object.style) || !valid_utf8(object.text.c_str(), object.text.size()) ||
                object.text.size() > MaxTextBytes ||
                static_cast<unsigned>(object.kind) > static_cast<unsigned>(Kind::Image))
                return set_error(error, "对象属性无效");
            if (object.kind == Kind::Image) {
                if (object.asset >= assets.size()) return set_error(error, "图片引用无效");
            } else if (object.asset != UINT32_MAX) return set_error(error, "非图片对象包含图片引用");
            if (!add_text_bytes(byte_count, object.text)) return set_error(error, "文档大小超限");
        }
    }
    if (byte_count > MaxFileBytes) return set_error(error, "文档大小超限");
    if (object_count) {
        if (object_count > SIZE_MAX / sizeof(uint32_t)) return set_error(error, "对象数量超限");
        id_bytes = object_count * sizeof(uint32_t);
        uint32_t *ids = static_cast<uint32_t *>(malloc(id_bytes));
        if (!ids) return set_error(error, "内存不足");
        size_t at = 0;
        uint32_t max_id = 0;
        for (size_t s = 0; s < slides.size(); ++s)
            for (size_t i = 0; i < slides[s].objects.size(); ++i) {
                const uint32_t id = slides[s].objects[i].id;
                ids[at++] = id;
                if (id > max_id) max_id = id;
            }
        qsort(ids, object_count, sizeof(uint32_t), compare_u32);
        bool unique = true;
        for (size_t i = 1; i < object_count; ++i)
            if (ids[i - 1] == ids[i]) { unique = false; break; }
        free(ids);
        if (!unique || next_id <= max_id) return set_error(error, "对象编号重复或无效");
    }
    error.clear();
    return true;
}

struct Editor::History {
    struct State {
        Deck deck;
        size_t current = 0;
        Array<uint32_t> selected;
        bool clone_from(const Deck &source, size_t page, const Array<uint32_t> &ids) {
            Deck next;
            Array<uint32_t> next_ids;
            if (!next.clone_from(source) || !next_ids.clone_from(ids)) return false;
            deck.swap(next);
            selected.swap(next_ids);
            current = page;
            return true;
        }
        size_t bytes(size_t selection_count) const {
            size_t total = sizeof(State);
            const size_t slide_bytes = deck.slides.size() * sizeof(Slide);
            const size_t asset_bytes = deck.assets.size() * sizeof(Asset);
            if (selection_count > SIZE_MAX / sizeof(uint32_t)) return SIZE_MAX;
            const size_t selection_bytes = selection_count * sizeof(uint32_t);
            if (!add_size(total, slide_bytes, total) || !add_size(total, asset_bytes, total) ||
                !add_size(total, selection_bytes, total) || !add_text_bytes(total, deck.title) ||
                !add_text_bytes(total, deck.author)) return SIZE_MAX;
            for (size_t s = 0; s < deck.slides.size(); ++s) {
                if (!add_size(total, deck.slides[s].objects.size() * sizeof(Object), total) ||
                    !add_text_bytes(total, deck.slides[s].notes)) return SIZE_MAX;
                for (size_t i = 0; i < deck.slides[s].objects.size(); ++i)
                    if (!add_text_bytes(total, deck.slides[s].objects[i].text)) return SIZE_MAX;
            }
            return total;
        }
        size_t bytes() const { return bytes(selected.size()); }
    };
    Array<State> states;
    size_t cursor = 0;
};

Editor::Editor() {
    if (!deck.init()) { set_error(error, "无法创建空白文档"); return; }
    revision_high_water_ = deck.revision;
    void *memory = malloc(sizeof(History));
    if (!memory) { set_error(error, "无法创建撤销历史"); return; }
    history_ = new (memory, Placement{}) History();
    History::State state;
    if (!state.clone_from(deck, current, selected) || !history_->states.push(static_cast<History::State &&>(state))) {
        history_->~History(); free(history_); history_ = nullptr;
        set_error(error, "无法创建撤销历史");
    }
}

Editor::~Editor() {
    if (history_) { history_->~History(); free(history_); }
}

bool Editor::commit_with_state(Deck &&candidate, size_t next_current,
                               const Array<uint32_t> &next_selected) {
    Text validation;
    if (!candidate.validate(validation)) return set_error(error, validation.c_str());
    if (next_current >= candidate.slides.size()) return set_error(error, "当前幻灯片无效");
    Array<uint32_t> selection;
    if (!clean_selection(candidate, next_current, next_selected, selection))
        return set_error(error, "内存不足");
    if (same_deck_content(deck, candidate)) {
        Array<uint32_t> history_selection;
        if (history_ && !history_selection.clone_from(selection))
            return set_error(error, "内存不足");
        if (history_ && history_->cursor < history_->states.size()) {
            History::State &state = history_->states[history_->cursor];
            state.current = next_current;
            state.selected.swap(history_selection);
        }
        current = next_current;
        selected.swap(selection);
        error.clear();
        return true;
    }
    if (!history_) return set_error(error, "撤销历史不可用");
    if (history_->states.empty() || history_->cursor >= history_->states.size())
        return set_error(error, "撤销历史无效");
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    Array<uint32_t> prior_selection;
    if (!clean_selection(deck, current, selected, prior_selection))
        return set_error(error, "内存不足");
    if (revision_high_water_ == UINT64_MAX) return set_error(error, "修订号已耗尽");
    const uint64_t revision = revision_high_water_ + 1;
    candidate.revision = revision;

    History::State added;
    if (!added.clone_from(candidate, next_current, selection)) return set_error(error, "内存不足");
    if (added.bytes() > HistoryBudget) return set_error(error, "撤销状态超出内存限制");
    const size_t kept = history_->cursor + 1;
    size_t total = 0;
    for (size_t i = 0; i < kept; ++i) {
        const size_t bytes = i == history_->cursor
            ? history_->states[i].bytes(prior_selection.size()) : history_->states[i].bytes();
        if (!add_size(total, bytes, total)) return set_error(error, "撤销历史超出内存限制");
    }
    if (!add_size(total, added.bytes(), total)) return set_error(error, "撤销历史超出内存限制");
    size_t drop = 0;
    size_t state_count = kept + 1;
    while (state_count > 1 && (state_count > HistoryLimit || total > HistoryBudget) && drop < kept) {
        const size_t bytes = drop == history_->cursor
            ? history_->states[drop].bytes(prior_selection.size()) : history_->states[drop].bytes();
        total = bytes <= total ? total - bytes : 0;
        --state_count;
        ++drop;
    }
    if (state_count > HistoryLimit || total > HistoryBudget)
        return set_error(error, "撤销历史超出内存限制");
    if (!history_->states.reserve(state_count)) return set_error(error, "内存不足");

    // Everything that can fail has completed. Move the retained path in place,
    // refresh its live view state, discard redo, then append the prepared state.
    if (history_->states.size() > kept)
        history_->states.erase(kept, history_->states.size() - kept);
    if (drop) history_->states.erase(0, drop);
    if (drop <= history_->cursor) {
        History::State &previous = history_->states[history_->cursor - drop];
        previous.current = current;
        previous.selected.swap(prior_selection);
    }
    history_->states.push_reserved(static_cast<History::State &&>(added));
    deck.swap(candidate);
    current = next_current;
    selected.swap(selection);
    history_->cursor = history_->states.size() - 1;
    revision_high_water_ = revision;
    error.clear();
    return true;
}

bool Editor::commit(Deck &&candidate) {
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::replace_deck(Deck &&candidate) {
    Text validation;
    if (!candidate.validate(validation)) return set_error(error, validation.c_str());
    if (revision_high_water_ == UINT64_MAX) return set_error(error, "修订号已耗尽");
    const uint64_t revision = revision_high_water_ + 1;
    candidate.revision = revision;
    void *replacement_memory = malloc(sizeof(History));
    if (!replacement_memory) return set_error(error, "内存不足");
    History *replacement = static_cast<History *>(replacement_memory);
    new (replacement, Placement{}) History();
    History::State state;
    Array<uint32_t> empty;
    if (!state.clone_from(candidate, 0, empty) || state.bytes() > HistoryBudget ||
        !replacement->states.push(static_cast<History::State &&>(state))) {
        replacement->~History(); free(replacement_memory);
        return set_error(error, "内存不足");
    }
    deck.swap(candidate);
    current = 0;
    selected.clear();
    if (history_) { history_->~History(); free(history_); }
    history_ = replacement;
    history_->cursor = 0;
    revision_high_water_ = revision;
    error.clear();
    return true;
}

void Editor::select(uint32_t id, bool additive) {
    if (!additive) selected.clear();
    if (!id || !deck.object(current, id) || selected_id(selected, id)) return;
    if (!selected.push(id)) set_error(error, "内存不足");
    else error.clear();
}

void Editor::select_all() {
    if (current >= deck.slides.size()) return;
    Array<uint32_t> next;
    const Slide &slide = deck.slides[current];
    if (!next.reserve(slide.objects.size())) { set_error(error, "内存不足"); return; }
    for (size_t i = 0; i < slide.objects.size(); ++i)
        if (!next.push(slide.objects[i].id)) { set_error(error, "内存不足"); return; }
    selected.swap(next);
    error.clear();
}

void Editor::clear_selection() { selected.clear(); error.clear(); }

bool Editor::add_slide(Layout layout) {
    if (current >= deck.slides.size() || deck.slides.size() >= MaxSlides)
        return set_error(error, "无法添加幻灯片");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Slide slide;
    if (!make_layout_slide(layout, candidate, slide) || !candidate.slides.push(static_cast<Slide &&>(slide)))
        return set_error(error, "无法添加幻灯片");
    Array<uint32_t> empty;
    return commit_with_state(static_cast<Deck &&>(candidate), deck.slides.size(), empty);
}

bool Editor::duplicate_slide() {
    if (current >= deck.slides.size() || deck.slides.size() >= MaxSlides)
        return set_error(error, "无法复制幻灯片");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Slide duplicate;
    if (!duplicate.clone_from(candidate.slides[current])) return set_error(error, "内存不足");
    for (size_t i = 0; i < duplicate.objects.size(); ++i) {
        uint32_t id = 0;
        if (!next_id(candidate, id)) return set_error(error, "对象编号已耗尽");
        duplicate.objects[i].id = id;
    }
    if (!insert_slide(candidate.slides, current + 1, static_cast<Slide &&>(duplicate)))
        return set_error(error, "内存不足");
    Array<uint32_t> empty;
    return commit_with_state(static_cast<Deck &&>(candidate), current + 1, empty);
}

bool Editor::delete_slide() {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    if (candidate.slides.size() == 1) {
        Slide blank;
        candidate.slides[0] = static_cast<Slide &&>(blank);
        candidate.assets.clear();
    } else {
        candidate.slides.erase(current);
        if (!compact_assets(candidate)) return set_error(error, "内存不足");
    }
    Array<uint32_t> empty;
    const size_t page = current < candidate.slides.size() ? current : candidate.slides.size() - 1;
    return commit_with_state(static_cast<Deck &&>(candidate), page, empty);
}

bool Editor::move_slide(int delta) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if (!delta || deck.slides.size() < 2) { error.clear(); return true; }
    int64_t target = static_cast<int64_t>(current) + static_cast<int64_t>(delta);
    if (target < 0) target = 0;
    if (target >= static_cast<int64_t>(deck.slides.size())) target = static_cast<int64_t>(deck.slides.size() - 1);
    if (target == static_cast<int64_t>(current)) { error.clear(); return true; }
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    size_t at = current;
    while (at < static_cast<size_t>(target)) {
        Slide item(static_cast<Slide &&>(candidate.slides[at]));
        candidate.slides[at] = static_cast<Slide &&>(candidate.slides[at + 1]);
        candidate.slides[at + 1] = static_cast<Slide &&>(item);
        ++at;
    }
    while (at > static_cast<size_t>(target)) {
        Slide item(static_cast<Slide &&>(candidate.slides[at]));
        candidate.slides[at] = static_cast<Slide &&>(candidate.slides[at - 1]);
        candidate.slides[at - 1] = static_cast<Slide &&>(item);
        --at;
    }
    return commit_with_state(static_cast<Deck &&>(candidate), static_cast<size_t>(target), selected);
}

bool Editor::insert_object(Object object, uint32_t *new_id) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if (deck.slides[current].objects.size() >= MaxObjectsPerSlide)
        return set_error(error, "当前幻灯片对象数量超限");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    uint32_t id = 0;
    if (!next_id(candidate, id)) return set_error(error, "对象编号已耗尽");
    object.id = id;
    if (!candidate.slides[current].objects.push(static_cast<Object &&>(object)))
        return set_error(error, "内存不足");
    Array<uint32_t> next;
    if (!next.push(id)) return set_error(error, "内存不足");
    if (!commit_with_state(static_cast<Deck &&>(candidate), current, next)) return false;
    if (new_id) *new_id = id;
    return true;
}

bool Editor::insert_image(uint32_t width, uint32_t height, const char *rgba, size_t length,
                          Rect rect, uint32_t *new_id) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if (!width || !height || width > 4096 || height > 4096 || (!rgba && length))
        return set_error(error, "图片尺寸或数据无效");
    const uint64_t expected = static_cast<uint64_t>(width) * height * 4u;
    if (expected != length || expected > MaxAssetBytes) return set_error(error, "图片数据长度无效");
    if (deck.slides[current].objects.size() >= MaxObjectsPerSlide || deck.assets.size() >= MaxAssets)
        return set_error(error, "图片或对象数量超限");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Asset asset;
    asset.width = width;
    asset.height = height;
    if (!asset.rgba.set(rgba, length) || !candidate.assets.push(static_cast<Asset &&>(asset)))
        return set_error(error, "内存不足");
    uint32_t id = 0;
    if (!next_id(candidate, id)) return set_error(error, "对象编号已耗尽");
    Object image;
    image.id = id;
    image.kind = Kind::Image;
    image.rect = rect;
    image.asset = static_cast<uint32_t>(candidate.assets.size() - 1);
    if (!candidate.slides[current].objects.push(static_cast<Object &&>(image)))
        return set_error(error, "内存不足");
    Array<uint32_t> next;
    if (!next.push(id)) return set_error(error, "内存不足");
    if (!commit_with_state(static_cast<Deck &&>(candidate), current, next)) return false;
    if (new_id) *new_id = id;
    return true;
}

bool Editor::update_objects(const Array<Object> &replacements) {
    if (replacements.empty()) { error.clear(); return true; }
    for (size_t i = 0; i < replacements.size(); ++i) {
        const uint32_t id = replacements[i].id;
        if (!id || !object_id_exists(deck, id)) return set_error(error, "更新对象编号无效");
        for (size_t j = 0; j < i; ++j)
            if (replacements[j].id == id) return set_error(error, "更新对象编号重复");
    }
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    for (size_t i = 0; i < replacements.size(); ++i) {
        bool found = false;
        for (size_t s = 0; s < candidate.slides.size() && !found; ++s)
            for (size_t j = 0; j < candidate.slides[s].objects.size(); ++j)
                if (candidate.slides[s].objects[j].id == replacements[i].id) {
                    candidate.slides[s].objects[j] = replacements[i];
                    found = true;
                    break;
                }
        if (!found) return set_error(error, "更新对象编号无效");
    }
    if (!compact_assets(candidate)) return set_error(error, "图片引用无效或内存不足");
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::delete_selected() {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if (selected.empty()) { error.clear(); return true; }
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Slide &slide = candidate.slides[current];
    for (size_t i = slide.objects.size(); i > 0; --i)
        if (selected_id(selected, slide.objects[i - 1].id)) slide.objects.erase(i - 1);
    if (!compact_assets(candidate)) return set_error(error, "内存不足");
    Array<uint32_t> empty;
    return commit_with_state(static_cast<Deck &&>(candidate), current, empty);
}

bool Editor::duplicate_selected() {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if (selected.empty()) { error.clear(); return true; }
    const Slide &source = deck.slides[current];
    size_t count = 0;
    for (size_t i = 0; i < source.objects.size(); ++i)
        if (selected_id(selected, source.objects[i].id)) ++count;
    if (!count) { error.clear(); return true; }
    if (source.objects.size() + count > MaxObjectsPerSlide) return set_error(error, "对象数量超限");
    float dx = 16, dy = 16;
    clamp_selected_offset(source, selected, deck.width, deck.height, dx, dy);
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Array<uint32_t> next_selection;
    Slide &target = candidate.slides[current];
    for (size_t i = 0; i < source.objects.size(); ++i) {
        const Object &original = source.objects[i];
        if (!selected_id(selected, original.id)) continue;
        Object duplicate = original;
        if (!next_id(candidate, duplicate.id)) return set_error(error, "对象编号已耗尽");
        duplicate.rect.x += dx;
        duplicate.rect.y += dy;
        if (!target.objects.push(static_cast<Object &&>(duplicate)) ||
            !next_selection.push(target.objects[target.objects.size() - 1].id))
            return set_error(error, "内存不足");
    }
    return commit_with_state(static_cast<Deck &&>(candidate), current, next_selection);
}

bool Editor::copy_selected(Deck &fragment) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    Deck next;
    next.width = deck.width;
    next.height = deck.height;
    next.next_id = 1;
    next.revision = 1;
    Slide copied;
    const Slide &source = deck.slides[current];
    uint32_t asset_map[MaxAssets];
    for (size_t i = 0; i < MaxAssets; ++i) asset_map[i] = UINT32_MAX;
    uint32_t max_id = 0;
    for (size_t i = 0; i < source.objects.size(); ++i) {
        const Object &object = source.objects[i];
        if (!selected_id(selected, object.id)) continue;
        Object copy = object;
        if (copy.kind == Kind::Image) {
            if (copy.asset >= deck.assets.size()) return set_error(error, "图片引用无效");
            if (asset_map[copy.asset] == UINT32_MAX) {
                asset_map[copy.asset] = static_cast<uint32_t>(next.assets.size());
                if (!next.assets.push(deck.assets[copy.asset])) return set_error(error, "内存不足");
            }
            copy.asset = asset_map[copy.asset];
        }
        if (!copied.objects.push(static_cast<Object &&>(copy))) return set_error(error, "内存不足");
        if (object.id > max_id) max_id = object.id;
    }
    next.next_id = max_id == UINT32_MAX - 1 ? UINT32_MAX : max_id + 1;
    if (!next.slides.push(static_cast<Slide &&>(copied))) return set_error(error, "内存不足");
    Text validation;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    fragment.swap(next);
    error.clear();
    return true;
}

bool Editor::paste_objects(const Deck &fragment) {
    Text validation;
    if (!fragment.validate(validation)) return set_error(error, validation.c_str());
    if (fragment.slides.size() != 1) return set_error(error, "剪贴对象格式无效");
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    const Slide &source = fragment.slides[0];
    if (deck.slides[current].objects.size() + source.objects.size() > MaxObjectsPerSlide)
        return set_error(error, "对象数量超限");
    float dx = 16, dy = 16;
    clamp_group_offset(source, deck.width, deck.height, dx, dy);
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    uint32_t asset_map[MaxAssets];
    for (size_t i = 0; i < MaxAssets; ++i) asset_map[i] = UINT32_MAX;
    for (size_t i = 0; i < source.objects.size(); ++i) {
        const Object &object = source.objects[i];
        if (object.kind != Kind::Image) continue;
        if (object.asset >= fragment.assets.size()) return set_error(error, "图片引用无效");
        if (asset_map[object.asset] != UINT32_MAX) continue;
        const Asset &asset = fragment.assets[object.asset];
        for (size_t a = 0; a < candidate.assets.size(); ++a)
            if (same_asset(candidate.assets[a], asset)) { asset_map[object.asset] = static_cast<uint32_t>(a); break; }
        if (asset_map[object.asset] == UINT32_MAX) {
            if (candidate.assets.size() >= MaxAssets) return set_error(error, "图片数量超限");
            asset_map[object.asset] = static_cast<uint32_t>(candidate.assets.size());
            if (!candidate.assets.push(asset)) return set_error(error, "内存不足");
        }
    }
    Array<uint32_t> next_selection;
    Slide &target = candidate.slides[current];
    for (size_t i = 0; i < source.objects.size(); ++i) {
        Object copy = source.objects[i];
        if (!next_paste_id(candidate, source, copy.id)) return set_error(error, "对象编号已耗尽");
        if (copy.kind == Kind::Image) copy.asset = asset_map[copy.asset];
        copy.rect.x += dx;
        copy.rect.y += dy;
        if (!target.objects.push(static_cast<Object &&>(copy)) ||
            !next_selection.push(target.objects[target.objects.size() - 1].id))
            return set_error(error, "内存不足");
    }
    return commit_with_state(static_cast<Deck &&>(candidate), current, next_selection);
}

bool Editor::set_text(uint32_t id, const char *bytes, size_t length) {
    if ((!bytes && length) || length > MaxTextBytes || !valid_utf8(bytes, length))
        return set_error(error, "文本无效");
    const Object *existing = nullptr;
    size_t page = 0;
    for (size_t s = 0; s < deck.slides.size() && !existing; ++s)
        if ((existing = deck.object(s, id))) page = s;
    if (!existing || existing->kind != Kind::Text) return set_error(error, "文字对象不存在");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Object *target = candidate.object(page, id);
    if (!target->text.set(bytes, length)) return set_error(error, "内存不足");
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::set_notes(const char *bytes, size_t length) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    if ((!bytes && length) || length > MaxNotesBytes || !valid_utf8(bytes, length))
        return set_error(error, "备注文本无效");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    if (!candidate.slides[current].notes.set(bytes, length)) return set_error(error, "内存不足");
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::set_background(uint32_t rgb) {
    if (rgb > 0xffffff || current >= deck.slides.size()) return set_error(error, "背景颜色无效");
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    candidate.slides[current].background = rgb;
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::resize_page(bool wide) {
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    const float width = 960, height = wide ? 540 : 720;
    if (deck.width == width && deck.height == height) { error.clear(); return true; }
    const float sx = width / deck.width, sy = height / deck.height;
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    candidate.width = width;
    candidate.height = height;
    for (size_t s = 0; s < candidate.slides.size(); ++s)
        for (size_t i = 0; i < candidate.slides[s].objects.size(); ++i) {
            Rect &rect = candidate.slides[s].objects[i].rect;
            rect.x *= sx; rect.w *= sx;
            rect.y *= sy; rect.h *= sy;
        }
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::align_selected(Align alignment) {
    if (static_cast<unsigned>(alignment) > static_cast<unsigned>(Align::Bottom))
        return set_error(error, "对齐方式无效");
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    const Slide &source = deck.slides[current];
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool first = true;
    size_t count = 0;
    for (size_t i = 0; i < source.objects.size(); ++i) {
        const Object &object = source.objects[i];
        if (!selected_id(selected, object.id)) continue;
        const Rect &r = object.rect;
        if (first) { min_x = r.x; min_y = r.y; max_x = r.x + r.w; max_y = r.y + r.h; first = false; }
        else {
            if (r.x < min_x) min_x = r.x;
            if (r.y < min_y) min_y = r.y;
            if (r.x + r.w > max_x) max_x = r.x + r.w;
            if (r.y + r.h > max_y) max_y = r.y + r.h;
        }
        ++count;
    }
    if (count < 2) { error.clear(); return true; }
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Slide &slide = candidate.slides[current];
    for (size_t i = 0; i < slide.objects.size(); ++i) {
        Object &object = slide.objects[i];
        if (!selected_id(selected, object.id)) continue;
        switch (alignment) {
            case Align::Left: object.rect.x = min_x; break;
            case Align::CenterX: object.rect.x = (min_x + max_x - object.rect.w) * 0.5f; break;
            case Align::Right: object.rect.x = max_x - object.rect.w; break;
            case Align::Top: object.rect.y = min_y; break;
            case Align::CenterY: object.rect.y = (min_y + max_y - object.rect.h) * 0.5f; break;
            case Align::Bottom: object.rect.y = max_y - object.rect.h; break;
        }
    }
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::layer_selected(Layer layer) {
    if (static_cast<unsigned>(layer) > static_cast<unsigned>(Layer::Down))
        return set_error(error, "图层操作无效");
    if (current >= deck.slides.size()) return set_error(error, "当前幻灯片无效");
    const Slide &source = deck.slides[current];
    bool any = false;
    for (size_t i = 0; i < source.objects.size(); ++i) any |= selected_id(selected, source.objects[i].id);
    if (!any) { error.clear(); return true; }
    Deck candidate;
    if (!candidate.clone_from(deck)) return set_error(error, "内存不足");
    Slide &slide = candidate.slides[current];
    if (layer == Layer::Top || layer == Layer::Bottom) {
        Array<Object> ordered;
        if (!ordered.reserve(slide.objects.size())) return set_error(error, "内存不足");
        for (unsigned pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < slide.objects.size(); ++i) {
                const bool is_selected = selected_id(selected, slide.objects[i].id);
                const bool move_now = layer == Layer::Top ? (pass == 1 ? is_selected : !is_selected)
                                                          : (pass == 0 ? is_selected : !is_selected);
                if (move_now && !ordered.push(slide.objects[i])) return set_error(error, "内存不足");
            }
        slide.objects.swap(ordered);
    } else if (layer == Layer::Up) {
        for (size_t i = slide.objects.size(); i > 1; --i) {
            const size_t at = i - 2;
            if (selected_id(selected, slide.objects[at].id) &&
                !selected_id(selected, slide.objects[at + 1].id)) {
                Object item(static_cast<Object &&>(slide.objects[at]));
                slide.objects[at] = static_cast<Object &&>(slide.objects[at + 1]);
                slide.objects[at + 1] = static_cast<Object &&>(item);
            }
        }
    } else {
        for (size_t at = 0; at + 1 < slide.objects.size(); ++at)
            if (selected_id(selected, slide.objects[at + 1].id) &&
                !selected_id(selected, slide.objects[at].id)) {
                Object item(static_cast<Object &&>(slide.objects[at]));
                slide.objects[at] = static_cast<Object &&>(slide.objects[at + 1]);
                slide.objects[at + 1] = static_cast<Object &&>(item);
            }
    }
    return commit_with_state(static_cast<Deck &&>(candidate), current, selected);
}

bool Editor::undo() {
    if (!history_ || !history_->cursor) return false;
    const History::State &state = history_->states[history_->cursor - 1];
    Deck replacement;
    Array<uint32_t> selection;
    if (!replacement.clone_from(state.deck) || !selection.clone_from(state.selected))
        return set_error(error, "内存不足");
    deck.swap(replacement);
    selected.swap(selection);
    current = state.current;
    --history_->cursor;
    error.clear();
    return true;
}

bool Editor::redo() {
    if (!history_ || history_->cursor + 1 >= history_->states.size()) return false;
    const History::State &state = history_->states[history_->cursor + 1];
    Deck replacement;
    Array<uint32_t> selection;
    if (!replacement.clone_from(state.deck) || !selection.clone_from(state.selected))
        return set_error(error, "内存不足");
    deck.swap(replacement);
    selected.swap(selection);
    current = state.current;
    ++history_->cursor;
    error.clear();
    return true;
}

bool Editor::can_undo() const { return history_ && history_->cursor > 0; }
bool Editor::can_redo() const { return history_ && history_->cursor + 1 < history_->states.size(); }

bool example_deck(Deck &out, Text &error) {
    Deck next;
    if (!next.init()) return set_error(error, "内存不足");
    next.slides.clear();
    next.next_id = 1;
    Slide title;
    Object heading = {};
    heading.id = next.next_id++;
    heading.kind = Kind::Text;
    heading.rect = {80, 100, 800, 100};
    heading.style.font_size = 42;
    heading.style.bold = true;
    if (!heading.text.set("欢迎使用 ReliefPPT", strlen("欢迎使用 ReliefPPT")) ||
        !title.objects.push(static_cast<Object &&>(heading)) ||
        !title.notes.set("这是一个三页演示文稿示例。", strlen("这是一个三页演示文稿示例。")) ||
        !next.slides.push(static_cast<Slide &&>(title))) return set_error(error, "内存不足");

    Slide content;
    Object body;
    body.id = next.next_id++;
    body.kind = Kind::Text;
    body.rect = {72, 70, 816, 120};
    body.style.font_size = 32;
    if (!body.text.set("清晰表达每一个想法", strlen("清晰表达每一个想法")))
        return set_error(error, "内存不足");
    Object rectangle;
    rectangle.id = next.next_id++;
    rectangle.kind = Kind::Rectangle;
    rectangle.rect = {112, 260, 260, 150};
    rectangle.style.fill = 0xdce9f7;
    rectangle.style.filled = true;
    Object ellipse;
    ellipse.id = next.next_id++;
    ellipse.kind = Kind::Ellipse;
    ellipse.rect = {500, 270, 170, 130};
    ellipse.style.fill = 0xc9eedc;
    ellipse.style.filled = true;
    if (!content.objects.push(static_cast<Object &&>(body)) ||
        !content.objects.push(static_cast<Object &&>(rectangle)) ||
        !content.objects.push(static_cast<Object &&>(ellipse)) ||
        !content.notes.set("第二页介绍文字与形状。", strlen("第二页介绍文字与形状。")) ||
        !next.slides.push(static_cast<Slide &&>(content))) return set_error(error, "内存不足");

    const unsigned char rgba[] = {
        0x36, 0x76, 0xac, 0xff, 0x65, 0xa1, 0xc8, 0xff,
        0x9f, 0xd3, 0xc7, 0xff, 0xf0, 0xd8, 0x9a, 0xff,
        0x82, 0xb8, 0x9f, 0xff, 0xf4, 0xec, 0xd7, 0xff
    };
    Asset picture;
    picture.width = 3; picture.height = 2;
    if (!picture.rgba.set(reinterpret_cast<const char *>(rgba), sizeof(rgba)) ||
        !next.assets.push(static_cast<Asset &&>(picture))) return set_error(error, "内存不足");
    Slide last;
    Object image;
    image.id = next.next_id++;
    image.kind = Kind::Image;
    image.rect = {270, 120, 420, 280};
    image.asset = 0;
    Object caption;
    caption.id = next.next_id++;
    caption.kind = Kind::Text;
    caption.rect = {220, 430, 520, 55};
    caption.style.alignment = Alignment::Center;
    caption.style.font_size = 24;
    if (!image.text.set("", 0) || !caption.text.set("图片保存在演示文稿中", strlen("图片保存在演示文稿中")) ||
        !last.objects.push(static_cast<Object &&>(image)) ||
        !last.objects.push(static_cast<Object &&>(caption)) ||
        !last.notes.set("图片数据已嵌入，无需保留来源文件。", strlen("图片数据已嵌入，无需保留来源文件。")) ||
        !next.slides.push(static_cast<Slide &&>(last))) return set_error(error, "内存不足");
    Text validation;
    if (!next.validate(validation)) return set_error(error, validation.c_str());
    out.swap(next);
    error.clear();
    return true;
}

} // namespace rp
