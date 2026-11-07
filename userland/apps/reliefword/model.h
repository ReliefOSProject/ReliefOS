#ifndef RELIEFWORD_MODEL_H
#define RELIEFWORD_MODEL_H
#include "storage.h"
namespace rw {
constexpr size_t MaxChars=500000, MaxParagraphs=50000, MaxStyles=512;
using Progress=bool (*)(void *,unsigned,unsigned);
enum class FontFamily:uint8_t { Sans,Serif,Mono };
enum class Alignment:uint8_t { Left,Center,Right,Justify };
enum class ListKind:uint8_t { Plain,Bullet,Number };
struct TextStyle {
    uint32_t foreground=0x202020,background=0xffff80;
    uint16_t size=12;
    FontFamily family=FontFamily::Serif;
    uint8_t bold=0,italic=0,underline=0,strike=0,highlight=0;
    bool operator==(const TextStyle &other) const;
};
enum StyleMask:unsigned { Size=1,Family=2,Bold=4,Italic=8,Underline=16,Strike=32,Foreground=64,Highlight=128,AllStyle=255 };
struct ParagraphStyle {
    Alignment alignment=Alignment::Left;
    ListKind list=ListKind::Plain;
    int16_t left_indent=0,right_indent=0,first_indent=0;
    uint16_t before=0,after=6,line_spacing=120;
    bool operator==(const ParagraphStyle &other) const;
};
enum ParagraphMask:unsigned { ParaAlignment=1,ParaList=2,ParaIndent=4,ParaSpacing=8,AllParagraph=15 };
struct PageSettings {
    uint16_t width=595,height=842,left=54,right=54,top=54,bottom=54;
    bool page_numbers=true;
    Text header,footer;
};
struct Glyph { uint32_t codepoint=0; uint16_t style=0; };
struct Paragraph { size_t start=0; ParagraphStyle style; };
struct Document {
    Array<Glyph> chars;
    Array<TextStyle> styles;
    Array<Paragraph> paragraphs;
    PageSettings page;
    Text title,author;
    uint64_t revision=1;
    bool init();
    bool clone_from(const Document &other);
    bool validate(Text &error) const;
    size_t paragraph_at(size_t offset) const;
    const TextStyle &style_at(size_t offset) const;
};
bool valid_scalar(uint32_t cp);
bool valid_utf8(const char *text,size_t length);
bool decode_utf8(const char *text,size_t length,Array<uint32_t> &output,Text &error);
unsigned encode_utf8(uint32_t cp,char output[4]);
bool document_text(const Document &doc,size_t start,size_t end,Text &output);
size_t previous_cluster(const Document &doc,size_t offset);
size_t next_cluster(const Document &doc,size_t offset);
struct Statistics { size_t characters=0,nonspace=0,words=0,paragraphs=0; };
Statistics statistics(const Document &doc,size_t start=0,size_t end=SIZE_MAX);
class Editor {
    struct History;
    History *history_=nullptr;
    uint64_t revision_high_water_=1;
public:
    Document doc;
    Text error;
    size_t anchor=0,caret=0;
    TextStyle typing_style;
    Editor();
    ~Editor();
    Editor(const Editor &)=delete;
    Editor &operator=(const Editor &)=delete;
    size_t selection_start() const { return anchor<caret?anchor:caret; }
    size_t selection_end() const { return anchor>caret?anchor:caret; }
    void select(size_t start,size_t end);
    bool insert_utf8(const char *text,size_t length);
    bool erase(bool backward);
    bool apply_style(const TextStyle &style,unsigned mask=AllStyle);
    bool apply_paragraph(const ParagraphStyle &style,unsigned mask=AllParagraph);
    bool set_page(const PageSettings &page);
    bool copy_selection(Document &output);
    bool paste_document(const Document &fragment);
    bool find(const char *query,bool match_case,size_t from,size_t &start,size_t &end) const;
    bool replace_all(const char *query,const char *replacement,bool match_case,unsigned &count);
    bool undo(); bool redo();
    bool can_undo() const; bool can_redo() const;
    bool replace_document(Document &&replacement);
    void reset_history();
};
}
#endif
