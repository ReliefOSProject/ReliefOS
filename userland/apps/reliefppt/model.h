#ifndef RELIEFPPT_MODEL_H
#define RELIEFPPT_MODEL_H
#include "storage.h"

namespace rp {
constexpr size_t MaxSlides = 500, MaxObjectsPerSlide = 256, MaxObjects = 20000,
    MaxAssets = 256, MaxAssetBytes = 64u * 1024u * 1024u, MaxTextBytes = 32768,
    MaxNotesBytes = 65536, MaxFileBytes = 96u * 1024u * 1024u;

enum class Kind : uint8_t { Text, Rectangle, Ellipse, Line, Arrow, Image };
enum class Alignment : uint8_t { Left, Center, Right };
enum class Layout : uint8_t { Blank, Title, Content, TwoColumns };
enum class Align : uint8_t { Left, CenterX, Right, Top, CenterY, Bottom };
enum class Layer : uint8_t { Top, Bottom, Up, Down };

struct Rect { float x = 0, y = 0, w = 240, h = 100; };
struct Style {
    uint32_t text = 0x202020, fill = 0xdce9f7, stroke = 0x3976ac;
    float font_size = 28, line_width = 2;
    uint8_t family = 0; // 0 SimSun, 1 Liberation Sans, 2 Liberation Mono
    bool bold = false, italic = false, underline = false, bullets = false,
        filled = false, outlined = false;
    Alignment alignment = Alignment::Left;
};
struct Object {
    uint32_t id = 0;
    Kind kind = Kind::Text;
    Rect rect;
    Style style;
    Text text;
    uint32_t asset = UINT32_MAX;
};
struct Asset { uint32_t width = 0, height = 0; Text rgba; };
struct Slide {
    uint32_t background = 0xffffff;
    Text notes;
    Array<Object> objects;
    bool clone_from(const Slide &other);
};
struct Deck {
    float width = 960, height = 540;
    uint32_t next_id = 1;
    uint64_t revision = 1;
    Text title, author;
    Array<Slide> slides;
    Array<Asset> assets;
    bool init();
    bool clone_from(const Deck &other);
    bool validate(Text &error) const;
    Object *object(size_t slide, uint32_t id);
    const Object *object(size_t slide, uint32_t id) const;
    void swap(Deck &other) noexcept;
};

bool valid_utf8(const char *, size_t); // Reject NUL and invalid scalars; allow LF/tab.

class Editor {
    struct History;
    History *history_ = nullptr;
    uint64_t revision_high_water_ = 1;
    bool commit_with_state(Deck &&candidate, size_t next_current,
        const Array<uint32_t> &next_selected);
public:
    Deck deck;
    size_t current = 0;
    Array<uint32_t> selected;
    Text error;
    Editor();
    ~Editor();
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;
    bool commit(Deck &&candidate); // Validate, atomically swap, and prune selection.
    bool replace_deck(Deck &&candidate); // Fresh revision and clean history.
    void select(uint32_t id, bool additive = false);
    void select_all();
    void clear_selection();
    bool add_slide(Layout layout = Layout::Content);
    bool duplicate_slide();
    bool delete_slide();
    bool move_slide(int delta);
    bool insert_object(Object object, uint32_t *new_id = nullptr);
    bool insert_image(uint32_t width, uint32_t height, const char *rgba,
        size_t length, Rect rect, uint32_t *new_id = nullptr);
    bool update_objects(const Array<Object> &replacements); // Existing IDs only.
    bool delete_selected();
    bool duplicate_selected();
    bool copy_selected(Deck &fragment);
    bool paste_objects(const Deck &fragment);
    bool set_text(uint32_t id, const char *, size_t);
    bool set_notes(const char *, size_t);
    bool set_background(uint32_t rgb);
    bool resize_page(bool wide);
    bool align_selected(Align);
    bool layer_selected(Layer);
    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
};

bool example_deck(Deck &, Text &error); // Three Chinese slides with an embedded image.
}
#endif
