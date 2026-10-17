#include "window.h"
#include "view.h"
#include "../fltk_support.h"
#include <FL/Fl_Color_Chooser.H>
#include <FL/x.H>
#include <FL/fl_draw.H>
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
namespace rw {
namespace {
const int Zooms[]={50,75,90,100,110,125,150,175,200,250,300};
unsigned long clipboard_owner() {
    using AtomFn=Atom (*)(Display *,const char *,Bool);
    using OwnerFn=Window (*)(Display *,Atom);
    static AtomFn atom=reinterpret_cast<AtomFn>(dlsym(RTLD_DEFAULT,"XInternAtom"));
    static OwnerFn owner=reinterpret_cast<OwnerFn>(dlsym(RTLD_DEFAULT,"XGetSelectionOwner"));
    return atom&&owner&&fl_display?owner(fl_display,atom(fl_display,"CLIPBOARD",False)):0;
}
void group_label(int X,int Y,int W,const char *label) {
    Fl_Box *box=new Fl_Box(X,Y,W,16,label); box->labelsize(11); box->labelcolor(FL_DARK3);
}
}
WordWindow::WordWindow():Fl_Double_Window(1160,740,"ReliefWord") {
    size_range(860,540); callback(close_cb,this); saved_revision_=editor.doc.revision;
    begin(); top_=new Fl_Group(0,0,w(),34);
    Fl_Menu_Button *file=new Fl_Menu_Button(7,3,64,28,T("&File")); file->box(FL_THIN_UP_BOX);
    auto item=[file](const char *label,Command command,int shortcut=0) { file->add(label,shortcut,dispatch,reinterpret_cast<void *>(intptr_t(command)+1)); };
    item(T("New"),Command::New,FL_CTRL+'n'); item(T("Open..."),Command::Open,FL_CTRL+'o');
    item(T("Save"),Command::Save,FL_CTRL+'s'); item(T("Save as..."),Command::SaveAs,FL_CTRL+FL_SHIFT+'s');
    item(T("Import TXT..."),Command::ImportText); item(T("Import HTML..."),Command::ImportHtml);
    item(T("Export TXT..."),Command::ExportText); item(T("Export HTML..."),Command::ExportHtml);
    item(T("Example document"),Command::Example); item(T("About ReliefWord"),Command::About); item(T("Exit"),Command::Quit);
    const char *tabs[]={T("Home tab"),T("Page layout"),T("View")};
    for(int i=0;i<3;++i) { tabs_[i]=new Fl_Button(80+i*88,3,84,28,tabs[i]); tabs_[i]->callback(tab_cb,reinterpret_cast<void *>(intptr_t(i))); tabs_[i]->clear_visible_focus(); }
    Fl_Box *stretch=new Fl_Box(348,0,w()-552,34);
    const char *quick[]={T("Save"),T("Undo"),T("Redo")}; Command commands[]={Command::Save,Command::Undo,Command::Redo};
    for(int i=0;i<3;++i) {
        Fl_Button *button=new Fl_Button(w()-200+i*65,4,61,26,quick[i]); button->callback(dispatch,reinterpret_cast<void *>(intptr_t(commands[i])+1)); button->clear_visible_focus();
        if(i==0) quick_save_=button;
        if(i==1) undo_=button;
        if(i==2) redo_=button;
    }
    top_->resizable(stretch); top_->end();
    ribbon_=new Fl_Group(0,34,w(),100); ribbon_scroll_=new Fl_Scroll(0,34,w(),100);
    ribbon_scroll_->type(Fl_Scroll::HORIZONTAL); ribbon_scroll_->scrollbar_size(10); build_ribbon();
    ribbon_scroll_->end(); ribbon_->resizable(ribbon_scroll_); ribbon_->end();
    view=new PageView(0,136,w(),h()-164,this);
    bottom_=new Fl_Group(0,h()-28,w(),28);
    status_=new Fl_Box(8,h()-27,w()/2-16,26,T("Ready")); status_->labelsize(13); status_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);
    summary_=new Fl_Box(w()/2,h()-27,w()/2-8,26); summary_->labelsize(13); summary_->align(FL_ALIGN_RIGHT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);
    bottom_->end(); end(); resizable(view);
    show_tab(0); view->reflow(true); selected();
    int X,Y,W,H; Fl::screen_work_area(X,Y,W,H);
    int width=w()<W-24?w():W-24,height=h()<H-48?h():H-48;
    if(width>=860&&height>=540) resize(X+(W-width)/2,Y+(H-height)/2,width,height);
    Fl::add_timeout(60,recovery_tick,this);
}
WordWindow::~WordWindow() {
    Fl::remove_timeout(recovery_tick,this);
    if(recovery_descriptor_<0&&!recovery_path_.empty()) unlink(recovery_path_.c_str());
    unlock_recovery_path(recovery_descriptor_); unlock_recovery_path(recovered_descriptor_);
    if(search_window_) delete search_window_;
}
void WordWindow::build_ribbon() {
    auto button=[](int X,int Y,int W,const char *label,Command command,const char *tip=nullptr,int H=26) {
        Fl_Button *b=new Fl_Button(X,Y,W,H,label); b->callback(dispatch,reinterpret_cast<void *>(intptr_t(command)+1));
        b->clear_visible_focus(); if(tip) b->tooltip(tip); return b;
    };
    panes_[0]=new Fl_Group(0,34,1100,88);
    button(10,42,60,T("Paste"),Command::Paste,T("Paste (Ctrl+V)"),54);
    button(76,42,58,T("Cut"),Command::Cut); button(76,72,58,T("Copy"),Command::Copy);
    group_label(10,100,124,T("Clipboard"));
    font_=new Fl_Choice(152,42,150,26); font_->add(T("Sans serif")); font_->add(T("Serif")); font_->add(T("Monospace"));
    font_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Font)+1)); font_->tooltip(T("Font family"));
    size_=new Fl_Input_Choice(308,42,74,26); size_->tooltip(T("Font size in points (6–96); press Enter to apply"));
    const int sizes[]={8,9,10,11,12,14,16,18,20,24,28,32,36,48,60,72};
    for(int value:sizes) { char label[16]; snprintf(label,sizeof(label),"%d",value); size_->add(label); }
    size_->input()->when(FL_WHEN_ENTER_KEY_ALWAYS); size_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Size)+1));
    button(388,42,62,T("Clear format"),Command::ClearFormat);
    const char *format[]={"B","I","U","S"}; const char *tips[]={T("Bold (Ctrl+B)"),T("Italic (Ctrl+I)"),T("Underline (Ctrl+U)"),T("Strikethrough")};
    Command styles[]={Command::Bold,Command::Italic,Command::Underline,Command::Strike};
    for(int i=0;i<4;++i) { format_[i]=button(152+i*34,72,30,format[i],styles[i],tips[i]); format_[i]->type(FL_TOGGLE_BUTTON); }
    format_[0]->labelfont(FL_HELVETICA_BOLD); format_[1]->labelfont(FL_HELVETICA_ITALIC);
    button(292,72,76,T("Text color"),Command::Color); button(374,72,76,T("Highlight"),Command::Highlight);
    group_label(152,100,298,T("Font"));
    const char *align[]={T("Left"),T("Center"),T("Right"),T("Justify")};
    Command aligns[]={Command::AlignLeft,Command::AlignCenter,Command::AlignRight,Command::AlignJustify};
    for(int i=0;i<4;++i) button(470+i*65,42,61,align[i],aligns[i]);
    button(470,72,80,T("Bullets"),Command::Bullets); button(556,72,80,T("Numbering"),Command::Numbering);
    button(642,72,88,T("Paragraph..."),Command::Paragraph); group_label(470,100,260,T("Paragraph"));
    preset_=new Fl_Choice(750,42,142,26); preset_->add(T("Body text")); preset_->add(T("Title")); preset_->add(T("Heading 1")); preset_->add(T("Heading 2"));
    preset_->value(0);
    preset_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Preset)+1));
    button(750,72,68,T("Outdent"),Command::Outdent); button(824,72,68,T("Indent"),Command::Indent); group_label(750,100,142,T("Styles"));
    button(912,42,88,T("Find..."),Command::Find); button(912,72,88,T("Replace..."),Command::Replace);
    button(1008,42,82,T("Select all"),Command::SelectAll); button(1008,72,82,T("Word count"),Command::Count);
    group_label(912,100,178,T("Editing")); panes_[0]->end();
    panes_[1]=new Fl_Group(0,34,900,88);
    button(10,42,116,T("Page setup..."),Command::PageSetup,T("Paper, orientation, margins, headers and footers"),54);
    button(136,42,110,T("Page break"),Command::PageBreak,T("Insert a page break (Ctrl+Enter)"));
    button(136,72,110,T("Insert date"),Command::Date); group_label(10,100,236,T("Page"));
    button(270,42,116,T("Paragraph..."),Command::Paragraph,T("Indentation, spacing and line height"),54);
    button(398,42,90,T("Outdent"),Command::Outdent); button(398,72,90,T("Indent"),Command::Indent);
    button(500,42,110,T("Bullets"),Command::Bullets); button(500,72,110,T("Numbering"),Command::Numbering);
    group_label(270,100,340,T("Paragraph")); panes_[1]->end();
    panes_[2]=new Fl_Group(0,34,900,88);
    zoom_=new Fl_Choice(10,42,100,26); for(int value:Zooms) { char label[16]; snprintf(label,sizeof(label),"%d%%",value); zoom_->add(label); }
    zoom_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Zoom)+1));
    button(10,72,46,"−",Command::ZoomOut); button(64,72,46,"+",Command::ZoomIn);
    button(126,42,108,T("Fit width"),Command::FitWidth); button(126,72,108,"100%",Command::Zoom100);
    group_label(10,100,224,T("Zoom"));
    ruler_button_=new Fl_Check_Button(262,45,132,26,T("Ruler")); ruler_button_->value(1);
    ruler_button_->callback(dispatch,reinterpret_cast<void *>(intptr_t(Command::Ruler)+1));
    button(262,76,132,T("Word count"),Command::Count); group_label(262,104,132,T("Display")); panes_[2]->end();
}
void WordWindow::dispatch(Fl_Widget *widget,void *data) {
    WordWindow *window=static_cast<WordWindow *>(widget->window());
    if(!window->updating_&&!window->busy_) window->act(static_cast<Command>(intptr_t(data)-1));
}
void WordWindow::tab_cb(Fl_Widget *widget,void *data) { static_cast<WordWindow *>(widget->window())->show_tab(int(intptr_t(data))); }
void WordWindow::close_cb(Fl_Widget *,void *context) { static_cast<WordWindow *>(context)->act(Command::Quit); }
void WordWindow::show_tab(int tab) {
    ribbon_index_=tab;
    for(int i=0;i<3;++i) { if(i==tab) panes_[i]->show(); else panes_[i]->hide(); tabs_[i]->value(i==tab); }
    ribbon_scroll_->scroll_to(0,0); ribbon_->redraw(); if(view) view->take_focus();
}
void WordWindow::message(const char *text) { status_->copy_label(T(text&&*text?text:"Ready")); status_->redraw(); }
void WordWindow::selected() {
    if(!view) return;
    updating_=true;
    const TextStyle &s=editor.typing_style;
    font_->value(int(s.family)); char size[24]; snprintf(size,sizeof(size),"%u",s.size); size_->value(size);
    const unsigned flags[]={s.bold,s.italic,s.underline,s.strike}; for(int i=0;i<4;++i) format_[i]->value(flags[i]!=0);
    if(editor.can_undo()) undo_->activate(); else undo_->deactivate();
    if(editor.can_redo()) redo_->activate(); else redo_->deactivate();
    for(size_t i=0;i<sizeof(Zooms)/sizeof(Zooms[0]);++i) if(view->zoom()==Zooms[i]) zoom_->value(int(i));
    ruler_button_->value(view->ruler());
    if(statistics_revision_!=editor.doc.revision) { statistics_=statistics(editor.doc); statistics_revision_=editor.doc.revision; }
    char summary[256];
    snprintf(summary,sizeof(summary),T("Page %u / %u   ·   %zu characters   ·   %zu words   ·   %d%%"),view->current_page()+1,view->layout.pages,statistics_.nonspace,statistics_.words,view->zoom());
    summary_->copy_label(summary);
    const char *base=path_.empty()?T("Untitled"):strrchr(path_.c_str(),'/'); if(!base) base=path_.c_str(); else if(*base=='/') ++base;
    char title[1024]; snprintf(title,sizeof(title),"%s%s — ReliefWord",editor.doc.revision==saved_revision_?"":"* ",base); copy_label(title);
    updating_=false;
}
void WordWindow::changed() {
    bool laid_out=view->reflow();
    if(laid_out) view->ensure_caret();
    selected(); view->redraw(); view->take_focus();
    if(laid_out) message(editor.doc.revision==saved_revision_?T("Ready"):T("Modified"));
    else message(view->layout.error.empty()?T("Operation failed"):view->layout.error.c_str());
}
void WordWindow::paste_text(const char *bytes,size_t length,bool plain) {
    if(busy_) return;
    bool rich=!plain&&!paste_plain_&&clipboard_owner_&&clipboard_owner()==clipboard_owner_&&length==clipboard_text_.size()&&!memcmp(bytes,clipboard_text_.c_str(),length);
    bool ok=rich?editor.paste_document(clipboard_):editor.insert_utf8(bytes,length);
    paste_plain_=false; if(ok) changed(); else message(editor.error.c_str());
}
void WordWindow::act(Command command) {
    if(busy_) return;
    Fl::compose_reset();
    TextStyle style=editor.typing_style;
    ParagraphStyle paragraph=editor.doc.paragraphs[editor.doc.paragraph_at(editor.caret)].style;
    bool edited=false,ok=true;
    switch(command) {
    case Command::New:
        if(confirm_discard()) {
            Document doc;
            if(!doc.init()) { message(T("Could not create a new document")); return; }
            if(!editor.replace_document(static_cast<Document &&>(doc))) { message(editor.error.c_str()); return; }
            discard_recovery(); path_.clear(); stamp_={}; import_path_.clear(); import_stamp_={};
            saved_revision_=editor.doc.revision; statistics_revision_=0; view->reflow(true); changed();
        }
        return;
    case Command::Open: { Text path; if(choose_path(path,false,FileFormat::Native)) load_initial(path.c_str()); return; }
    case Command::Save: save(false); return;
    case Command::SaveAs: save(true); return;
    case Command::ImportText: case Command::ImportHtml: {
        FileFormat format=command==Command::ImportText?FileFormat::Text:FileFormat::Html;
        Text path; if(choose_path(path,false,format)&&confirm_discard()) open_file(path.c_str(),format); return;
    }
    case Command::ExportText: export_file(FileFormat::Text); return;
    case Command::ExportHtml: export_file(FileFormat::Html); return;
    case Command::Example: example_document(); return;
    case Command::Quit:
        if(confirm_discard()) { discard_recovery(); if(search_window_) search_window_->hide(); hide(); } return;
    case Command::Undo: ok=editor.undo(); edited=ok; break;
    case Command::Redo: ok=editor.redo(); edited=ok; break;
    case Command::Copy: case Command::Cut:
        if(editor.selection_start()==editor.selection_end()) return;
        ok=editor.copy_selection(clipboard_)&&document_text(editor.doc,editor.selection_start(),editor.selection_end(),clipboard_text_);
        if(ok) { Fl::copy(clipboard_text_.c_str(),int(clipboard_text_.size()),1); clipboard_owner_=clipboard_owner(); }
        if(ok&&command==Command::Cut) { ok=editor.insert_utf8("",0); edited=ok; }
        break;
    case Command::Paste: case Command::PastePlain: paste_plain_=command==Command::PastePlain; view->take_focus(); Fl::paste(*view,1); return;
    case Command::SelectAll: editor.select(0,editor.doc.chars.size()); view->redraw(); selected(); view->take_focus(); return;
    case Command::Find: search_dialog(false); return;
    case Command::Replace: search_dialog(true); return;
    case Command::FindNext: find_next(); return;
    case Command::ReplaceOne: replace_one(); return;
    case Command::ReplaceAll: replace_all(); return;
    case Command::Bold: style.bold=!style.bold; ok=editor.apply_style(style,Bold); edited=ok; break;
    case Command::Italic: style.italic=!style.italic; ok=editor.apply_style(style,Italic); edited=ok; break;
    case Command::Underline: style.underline=!style.underline; ok=editor.apply_style(style,Underline); edited=ok; break;
    case Command::Strike: style.strike=!style.strike; ok=editor.apply_style(style,Strike); edited=ok; break;
    case Command::Font: style.family=FontFamily(font_->value()); ok=editor.apply_style(style,Family); edited=ok; break;
    case Command::Size: {
        char *end=nullptr; long value=strtol(size_->value(),&end,10);
        if(!end||*end||value<6||value>96) { message(T("Enter a font size from 6 to 96 points")); selected(); return; }
        style.size=uint16_t(value); ok=editor.apply_style(style,Size); edited=ok; break;
    }
    case Command::Color: {
        double r=(style.foreground>>16)/255.0,g=((style.foreground>>8)&255)/255.0,b=(style.foreground&255)/255.0;
        if(!fl_color_chooser(T("Text color"),r,g,b)) return;
        style.foreground=uint32_t(r*255+.5)<<16|uint32_t(g*255+.5)<<8|uint32_t(b*255+.5); ok=editor.apply_style(style,Foreground); edited=ok; break;
    }
    case Command::Highlight: style.highlight=!style.highlight; ok=editor.apply_style(style,Highlight); edited=ok; break;
    case Command::ClearFormat: ok=editor.apply_style(TextStyle{}); edited=ok; break;
    case Command::Preset: {
        size_t anchor=editor.anchor,caret=editor.caret;
        if(anchor==caret) { size_t p=editor.doc.paragraph_at(caret); editor.select(editor.doc.paragraphs[p].start,p+1<editor.doc.paragraphs.size()?editor.doc.paragraphs[p+1].start:editor.doc.chars.size()); }
        TextStyle preset; int index=preset_->value(); preset.size=index==1?24:index==2?18:index==3?14:12; preset.bold=index==2||index==3;
        preset.foreground=index==2||index==3?0x244b7a:0x202020;
        ok=editor.apply_style(preset); if(anchor==caret) editor.select(anchor,caret); edited=ok; break;
    }
    case Command::AlignLeft: case Command::AlignCenter: case Command::AlignRight: case Command::AlignJustify:
        paragraph.alignment=Alignment(int(command)-int(Command::AlignLeft)); ok=editor.apply_paragraph(paragraph,ParaAlignment); edited=ok; break;
    case Command::Bullets: case Command::Numbering: {
        ListKind kind=command==Command::Bullets?ListKind::Bullet:ListKind::Number;
        paragraph.list=paragraph.list==kind?ListKind::Plain:kind; ok=editor.apply_paragraph(paragraph,ParaList); edited=ok; break;
    }
    case Command::Indent: case Command::Outdent:
        paragraph.left_indent+=command==Command::Indent?18:-18; if(paragraph.left_indent<0) paragraph.left_indent=0;
        ok=editor.apply_paragraph(paragraph,ParaIndent); edited=ok; break;
    case Command::Paragraph: paragraph_dialog(); return;
    case Command::PageSetup: page_dialog(); return;
    case Command::PageBreak: ok=editor.insert_utf8("\f",1); edited=ok; break;
    case Command::Date: {
        time_t now=time(nullptr); struct tm calendar{}; localtime_r(&now,&calendar); char date[64]; strftime(date,sizeof(date),"%Y-%m-%d",&calendar);
        ok=editor.insert_utf8(date,strlen(date)); edited=ok; break;
    }
    case Command::Ruler: view->show_ruler(ruler_button_->value()); selected(); return;
    case Command::Zoom: if(zoom_->value()>=0&&zoom_->value()<int(sizeof(Zooms)/sizeof(Zooms[0]))) view->set_zoom(Zooms[zoom_->value()]); return;
    case Command::ZoomIn: view->set_zoom(view->zoom()+10); return;
    case Command::ZoomOut: view->set_zoom(view->zoom()-10); return;
    case Command::Zoom100: view->set_zoom(100); return;
    case Command::FitWidth: view->fit_width(); return;
    case Command::Count: {
        Statistics s=statistics(editor.doc,editor.selection_start(),editor.selection_end()); bool selection=editor.anchor!=editor.caret;
        if(!selection) s=statistics(editor.doc);
        fl_message(T("%s\nCharacters: %zu\nNon-space characters: %zu\nWords / CJK characters: %zu\nParagraphs: %zu\nPages: %u"),selection?T("Selection"):T("Document"),s.characters,s.nonspace,s.words,s.paragraphs,view->layout.pages); return;
    }
    case Command::About: fl_message("ReliefWord\n%s",T("A native ReliefOS word processor.\nCtrl+S: save  ·  Ctrl+F: find  ·  Ctrl+Enter: page break\nUse the original .rword format to keep all formatting.")); return;
    }
    if(edited) changed(); else { selected(); view->take_focus(); }
    if(!ok) message(editor.error.empty()?T("Operation failed"):editor.error.c_str());
}
void WordWindow::example_document() {
    if(!confirm_discard()) return;
    Editor example;
    if(!example.error.empty()) { message(example.error.c_str()); return; }
    const char *text=T("ReliefOS project report\nA document made with ReliefWord\n\nOverview\nWrite, edit and arrange Chinese and English text on real pages. The ribbon keeps everyday commands within reach.\n\nThis week's work\nDraft the proposal and collect feedback.\nReview the document together.\nSave the final document in the original format.\n\nNext steps\nUse headings, lists and paragraph spacing to make the report easy to read. You can open TXT or HTML and save an editable .rword copy.\n\nPrepared with ReliefWord — simple tools for everyday writing.");
    if(!example.insert_utf8(text,strlen(text))) { message(example.error.c_str()); return; }
    for(size_t p=0;p<example.doc.paragraphs.size();++p) {
        size_t start=example.doc.paragraphs[p].start,end=p+1<example.doc.paragraphs.size()?example.doc.paragraphs[p+1].start-1:example.doc.chars.size();
        TextStyle s; ParagraphStyle ps;
        if(p==0) { s.size=24; s.foreground=0x244b7a; ps.after=12; }
        else if(p==1) { s.size=12; s.foreground=0x606970; ps.after=16; }
        else if(p==3||p==6||p==11) { s.size=16; s.bold=1; s.foreground=0x244b7a; ps.before=10; ps.after=8; }
        if(p>=7&&p<=9) ps.list=ListKind::Bullet;
        example.select(start,end);
        if(!example.apply_style(s)) { message(example.error.c_str()); return; }
        if(!example.apply_paragraph(ps)) { message(example.error.c_str()); return; }
    }
    example.select(0,0);
    if(!editor.replace_document(static_cast<Document &&>(example.doc))) { message(editor.error.c_str()); return; }
    editor.reset_history(); discard_recovery(); path_.clear(); stamp_={}; import_path_.clear(); import_stamp_={};
    saved_revision_=editor.doc.revision; statistics_revision_=0; view->reflow(true); changed();
}
void WordWindow::resize(int X,int Y,int W,int H) {
    Fl_Double_Window::resize(X,Y,W,H);
    if(!view) return;
    if(top_) top_->resize(0,0,W,34);
    if(quick_save_) quick_save_->resize(W-200,4,61,26);
    if(undo_) undo_->resize(W-135,4,61,26);
    if(redo_) redo_->resize(W-70,4,61,26);
    if(ribbon_) ribbon_->resize(0,34,W,100);
    if(ribbon_scroll_) ribbon_scroll_->resize(0,34,W,100);
    view->resize(0,136,W,H-164);
    if(bottom_) bottom_->resize(0,H-28,W,28);
    if(status_) status_->resize(8,H-27,W/2-16,26);
    if(summary_) summary_->resize(W/2,H-27,W/2-8,26);
    view->redraw();
}
int WordWindow::handle(int event) {
    if((event==FL_KEYDOWN||event==FL_SHORTCUT)&&Fl::event_key()==FL_Escape) {
        if(search_window_&&search_window_->shown()) search_window_->hide();
        if(editor.anchor!=editor.caret) {
            editor.select(editor.caret,editor.caret); view->redraw(); selected();
        }
        if(view) view->take_focus();
        return 1;
    }
    if(size_&&Fl::focus()==size_->input()&&(event==FL_KEYDOWN||event==FL_SHORTCUT)&&
       Fl::event_state(FL_CTRL)&&!Fl::event_state(FL_ALT)) {
        switch(Fl::event_key()) {
        case 'a': case 'c': case 'x': case 'v': case 'z': case 'y':
            return Fl_Double_Window::handle(event);
        }
    }
    if((event==FL_KEYDOWN||event==FL_SHORTCUT)&&!busy_&&!Fl::compose_state&&Fl::event_state(FL_CTRL)&&!Fl::event_state(FL_ALT)) {
        int key=Fl::event_key(); bool shift=Fl::event_state(FL_SHIFT); Command command;
        switch(key) {
        case 'n': command=Command::New; break; case 'o': command=Command::Open; break;
        case 's': command=shift?Command::SaveAs:Command::Save; break; case 'z': command=shift?Command::Redo:Command::Undo; break;
        case 'y': command=Command::Redo; break; case 'c': command=Command::Copy; break; case 'x': command=Command::Cut; break;
        case 'v': command=shift?Command::PastePlain:Command::Paste; break; case 'a': command=Command::SelectAll; break;
        case 'b': command=Command::Bold; break; case 'i': command=Command::Italic; break; case 'u': command=Command::Underline; break;
        case 'f': command=Command::Find; break; case 'h': command=Command::Replace; break;
        case 'l': command=Command::AlignLeft; break; case 'e': command=Command::AlignCenter; break; case 'r': command=Command::AlignRight; break; case 'j': command=Command::AlignJustify; break;
        case FL_Enter: command=Command::PageBreak; break; case '+': case '=': command=Command::ZoomIn; break; case '-': command=Command::ZoomOut; break;
        default:return Fl_Double_Window::handle(event);
        }
        act(command); return 1;
    }
    if(event==FL_KEYDOWN&&Fl::event_key()==FL_F+3) { act(Command::FindNext); return 1; }
    return Fl_Double_Window::handle(event);
}
}
