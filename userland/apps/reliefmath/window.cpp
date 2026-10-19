#include "window.h"
#include "process.h"
#include "jobs.h"
#include "export.h"
#include <FL/Fl_Color_Chooser.H>
#include <FL/fl_ask.H>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
namespace rm {
namespace {
double now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return double(t.tv_sec)+t.tv_nsec/1e9;}
const char *worker_path(){return "/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf";}
void group_label(int x,int width,const char *label){auto *b=new Fl_Box(x,100,width,17,label);b->labelsize(12);}
const int Sizes[]={12,14,16,18,20,24,28,32,36,48,60,72,96,128};
}
MathWindow::MathWindow():Fl_Double_Window(1180,780,"ReliefMath"){
    size_range(960,600);callback([](Fl_Widget*,void *p){static_cast<MathWindow*>(p)->run(Command::Close);},this);
    renderer_=new RenderClient;io_=new IoJob;exports_=new ExportQueue;
    Error e;renderer_->start(worker_path(),e);
    OfficeLayout p=OfficeLayout::compute(w(),h(),true,true);
    begin();top_=new Fl_Group(0,0,w(),34);
    auto *file=new Fl_Menu_Button(7,3,66,28,T("&File"));
    auto item=[file](const char *label,Command c,int key=0){file->add(label,key,clicked,reinterpret_cast<void*>(intptr_t(c)+1));};
    item(T("New"),Command::New,FL_CTRL+'n');item(T("Open..."),Command::Open,FL_CTRL+'o');
    item(T("Save"),Command::Save,FL_CTRL+'s');item(T("Save as..."),Command::SaveAs,FL_CTRL+FL_SHIFT+'s');
    item(T("Export SVG..."),Command::ExportSvg);item(T("Export transparent PNG..."),Command::ExportPng);
    item(T("Export LaTeX..."),Command::ExportLatex);item(T("Export selected formulas..."),Command::ExportSelected);
    item(T("Export all formulas..."),Command::ExportAll);item(T("Example formulas"),Command::Example);
    item(T("ReliefMath help"),Command::Help,FL_F+1);item(T("Exit"),Command::Close);
    const char *titles[]={T("Home tab"),T("Symbols"),T("Structures"),T("Typesetting"),T("View")};
    for(int n=0;n<5;n++){
        tabs_[n]=new Fl_Button(80+n*78,3,74,28,titles[n]);tabs_[n]->type(FL_TOGGLE_BUTTON);
        tabs_[n]->callback([](Fl_Widget *w,void *p){static_cast<MathWindow*>(w->window())->show_tab(int(intptr_t(p)));},reinterpret_cast<void*>(intptr_t(n)));
    }
    auto *stretch=new Fl_Box(p.top_stretch.x,p.top_stretch.y,p.top_stretch.width,p.top_stretch.height);
    const char *quick[]={T("Save"),T("Undo"),T("Redo")};Command commands[]={Command::Save,Command::Undo,Command::Redo};
    for(int n=0;n<3;n++){auto *b=new Fl_Button(p.quick_actions.x+n*66,p.quick_actions.y,62,26,quick[n]);b->callback(clicked,reinterpret_cast<void*>(intptr_t(commands[n])+1));if(n==1)undo_=b;if(n==2)redo_=b;}
    // FLTK moves children beyond this resizable box with its right edge,
    // keeping the quick actions right-anchored.
    top_->resizable(stretch);top_->end();
    ribbon_=new Fl_Group(0,34,w(),100);ribbon_scroll_=new Fl_Scroll(0,34,w(),100);ribbon_scroll_->type(Fl_Scroll::HORIZONTAL);ribbon_scroll_->scrollbar_size(10);
    build_ribbon();ribbon_scroll_->end();ribbon_->resizable(ribbon_scroll_);ribbon_->end();
    tile_=new Fl_Tile(0,140,w(),h()-174);
    list_panel_=new Fl_Group(p.list.x,p.list.y,p.list.width,p.list.height);
    filter_=new Fl_Input(8,148,p.list.width-16,27);filter_->tooltip(T("Filter formulas by name or source"));filter_->when(FL_WHEN_CHANGED);
    filter_->callback([](Fl_Widget *w,void*){static_cast<MathWindow*>(w->window())->refill_list();});
    list_=new Fl_Multi_Browser(p.list_content.x,p.list_content.y,p.list_content.width,p.list_content.height);list_->textsize(14);list_->callback(selected,this);
    auto *add=new Fl_Button(p.list_actions.x,p.list_actions.y,58,26,T("Add"));add->callback(clicked,reinterpret_cast<void*>(intptr_t(Command::Add)+1));
    auto *rename=new Fl_Button(p.list_actions.x+62,p.list_actions.y,58,26,T("Rename"));rename->callback(clicked,reinterpret_cast<void*>(intptr_t(Command::Rename)+1));
    auto *remove=new Fl_Button(p.list_actions.x+124,p.list_actions.y,60,26,T("Delete"));remove->callback(clicked,reinterpret_cast<void*>(intptr_t(Command::Delete)+1));
    // The action row sits below the resizable browser, so FLTK moves it
    // with the panel bottom.
    list_panel_->resizable(list_);list_panel_->end();
    edit_panel_=new Fl_Group(p.editor.x,140,p.editor.width,p.editor.height);
    auto *heading=new Fl_Box(p.editor.x+8,142,p.editor.width-16,28,T("LaTeX source"));heading->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);heading->labelsize(14);
    editor=new SourceEditor(p.editor.x,174,p.editor.width,p.editor.height-34,this);editor->buffer(source_);editor->textfont(FL_COURIER);editor->textsize(16);
    editor->linenumber_width(42);editor->linenumber_font(FL_HELVETICA);editor->linenumber_size(12);editor->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS,0);
    static const Fl_Text_Display::Style_Table_Entry table[]={
        {FL_FOREGROUND_COLOR,FL_COURIER,16,0},{FL_BLUE,FL_COURIER,16,0},{FL_DARK_MAGENTA,FL_COURIER,16,0},
        {FL_DARK_GREEN,FL_COURIER,16,0},{FL_RED,FL_COURIER,16,0},{FL_DARK_CYAN,FL_COURIER_BOLD,16,0}};
    editor->highlight_data(&styles_,table,6,'A',nullptr,nullptr);
    source_.add_modify_callback(source_changed,this);edit_panel_->resizable(editor);edit_panel_->end();
    preview_panel_=new Fl_Group(p.preview.x,140,p.preview.width,p.preview.height);
    render_label_=new Fl_Box(p.preview.x+8,142,p.preview.width-116,28,T("Preview"));render_label_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);render_label_->labelsize(13);
    error_button_=new Fl_Button(p.preview.x+p.preview.width-106,144,98,26,T("Locate error"));error_button_->callback(clicked,reinterpret_cast<void*>(intptr_t(Command::ErrorLocation)+1));error_button_->hide();
    preview=new Preview(p.preview.x,176,p.preview.width,p.preview.height-36);preview_panel_->resizable(preview);preview_panel_->end();
    tile_->end();bottom_=new Fl_Group(0,h()-28,w(),28);
    status_=new Fl_Box(8,h()-27,w()/2-16,26,T("Ready"));status_->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);status_->labelsize(13);
    summary_=new Fl_Box(w()/2,h()-27,w()/2-8,26);summary_->align(FL_ALIGN_RIGHT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);summary_->labelsize(13);
    bottom_->end();end();resizable(tile_);show_tab(0);
    FormulaId id=0;document.title.set(T("Untitled"));
    if(add_formula(document,T("Formula 1"),"E=mc^2",id,e)){active_=id;history.mark_saved(document,e);}
    refill_list();refresh_source();init_files();
    Fl::add_timeout(.03,tick,this);Fl::add_timeout(60,recovery_timer,this);
    int x,y,width,height;Fl::screen_work_area(x,y,width,height);
    int ww=w()<width-24?w():width-24,hh=h()<height-48?h():height-48;
    if(ww>=960&&hh>=600)resize(x+(width-ww)/2,y+(height-hh)/2,ww,hh);
}
MathWindow::~MathWindow(){
    Fl::remove_timeout(tick,this);Fl::remove_timeout(recovery_timer,this);source_.remove_modify_callback(source_changed,this);
    delete renderer_;delete exports_;delete io_;destroy_files();
    // Text displays own neither buffer; detach before member buffers are destroyed.
    editor->buffer(nullptr);editor->highlight_data(nullptr,nullptr,0,'A',nullptr,nullptr);
}
void MathWindow::clicked(Fl_Widget *widget,void *data){static_cast<MathWindow*>(widget->window())->run(Command(intptr_t(data)-1));}
void MathWindow::build_ribbon(){
    auto b=[](int x,int y,int width,const char *label,Command command,int height=26){auto *button=new Fl_Button(x,y,width,height,label);button->callback(clicked,reinterpret_cast<void*>(intptr_t(command)+1));button->tooltip(label);return button;};
    auto tool=[](int x,int y,int width,const char *label,Tool t){auto *button=new Fl_Button(x,y,width,26,label);button->callback([](Fl_Widget *w,void *v){static_cast<MathWindow*>(w->window())->insert_tool(Tool(intptr_t(v)));},reinterpret_cast<void*>(intptr_t(t)));button->tooltip(label);};
    ribbon_pages_[0]=new Fl_Group(0,34,1090,88);
    b(10,42,62,T("Paste"),Command::Paste,54);b(78,42,62,T("Cut"),Command::Cut);b(78,72,62,T("Copy"),Command::Copy);group_label(10,130,T("Clipboard"));
    b(160,42,76,T("Add"),Command::Add);b(242,42,76,T("Duplicate"),Command::Duplicate);b(160,72,76,T("Rename"),Command::Rename);b(242,72,76,T("Delete"),Command::Delete);group_label(160,158,T("Formulas"));
    b(338,42,76,T("Move up"),Command::MoveUp);b(338,72,76,T("Move down"),Command::MoveDown);group_label(338,76,T("Order"));
    b(434,42,92,T("Find..."),Command::Find);b(434,72,92,T("Replace..."),Command::Replace);b(532,42,98,T("Complete"),Command::Complete);b(532,72,98,T("Select all"),Command::SelectAll);group_label(434,196,T("Editing"));
    b(650,42,110,T("Templates..."),Command::Templates);b(650,72,110,T("Favorites..."),Command::Favorites);group_label(650,110,T("Library"));
    b(780,42,92,T("Export SVG"),Command::ExportSvg);b(878,42,98,T("Export PNG"),Command::ExportPng);b(780,72,92,T("Copy image"),Command::CopyImage);b(878,72,98,T("Export all..."),Command::ExportAll);group_label(780,196,T("Export"));
    b(996,42,82,T("Help"),Command::Help);b(996,72,82,T("Cancel task"),Command::CancelJob);group_label(996,82,T("Tools"));ribbon_pages_[0]->end();
    ribbon_pages_[1]=new Fl_Group(0,34,1080,88);
    tool(10,42,54,"α",Tool::Alpha);tool(70,42,54,"β",Tool::Beta);tool(130,42,54,"γ",Tool::Gamma);tool(10,72,54,"θ",Tool::Theta);tool(70,72,54,"λ",Tool::Lambda);tool(130,72,54,"π",Tool::Pi);group_label(10,174,T("Greek letters"));
    tool(204,42,54,"±",Tool::PlusMinus);tool(264,42,54,"×",Tool::Times);tool(324,42,54,"÷",Tool::Divide);tool(204,72,54,"≤",Tool::LessEqual);tool(264,72,54,"≈",Tool::Approx);tool(324,72,54,"≠",Tool::NotEqual);group_label(204,174,T("Operators and relations"));
    tool(398,42,54,"→",Tool::To);tool(458,42,54,"⇔",Tool::LeftRightArrow);tool(398,72,54,"∈",Tool::In);tool(458,72,54,"∪",Tool::Union);group_label(398,114,T("Arrows and sets"));
    tool(532,42,54,"∞",Tool::Infinity);tool(592,42,54,"∀",Tool::ForAll);tool(532,72,54,"∃",Tool::Exists);tool(592,72,54,"∩",Tool::Intersection);group_label(532,114,T("Logic"));
    b(666,42,126,T("All symbols..."),Command::Symbols,54);b(806,42,126,T("Favorites..."),Command::Favorites);b(806,72,126,T("Add favorite..."),Command::AddFavorite);group_label(666,266,T("Symbol library"));ribbon_pages_[1]->end();
    ribbon_pages_[2]=new Fl_Group(0,34,1080,88);
    b(10,42,100,T("Fraction..."),Command::Fraction);b(10,72,100,T("Root..."),Command::Root);b(116,42,100,T("Scripts..."),Command::Scripts);b(116,72,100,T("Text..."),Command::Text);group_label(10,206,T("Basic structures"));
    b(236,42,100,T("Integral..."),Command::Integral);b(236,72,100,T("Sum..."),Command::Sum);tool(342,42,100,T("Product"),Tool::Product);tool(342,72,100,T("Limit"),Tool::Limit);group_label(236,206,T("Large operators"));
    b(462,42,108,T("Matrix..."),Command::Matrix);b(462,72,108,T("Cases..."),Command::Cases);b(576,42,108,T("Aligned..."),Command::Aligned);tool(576,72,108,T("Brackets"),Tool::AutoParentheses);group_label(462,222,T("Multiline structures"));
    tool(704,42,96,T("Vector"),Tool::Vector);tool(704,72,96,T("Hat"),Tool::Hat);tool(806,42,96,T("Overline"),Tool::Overline);tool(806,72,96,T("Norm"),Tool::Norm);group_label(704,198,T("Accents"));b(922,42,130,T("Templates..."),Command::Templates,54);group_label(922,130,T("Library"));ribbon_pages_[2]->end();
    ribbon_pages_[3]=new Fl_Group(0,34,1060,88);
    size_choice_=new Fl_Choice(10,42,110,26);for(int size:Sizes){char text[16];snprintf(text,sizeof text,"%d pt",size);size_choice_->add(text);}size_choice_->callback(style_changed,this);
    display_choice_=new Fl_Choice(10,72,110,26);display_choice_->add(T("Display style"));display_choice_->add(T("Inline style"));display_choice_->callback(style_changed,this);group_label(10,110,T("Formula style"));
    tool(140,42,90,T("Roman"),Tool::Roman);tool(236,42,90,T("Bold"),Tool::Bold);tool(140,72,90,T("Italic"),Tool::Italic);tool(236,72,90,T("Text"),Tool::Text);group_label(140,186,T("Math fonts"));
    b(346,42,104,T("Formula color"),Command::Color);b(346,72,104,T("Padding..."),Command::Padding);group_label(346,104,T("Appearance"));
    tool(470,42,96,"\\,",Tool::ThinSpace);tool(572,42,96,"\\quad",Tool::QuadSpace);tool(470,72,96,"\\!",Tool::NegativeSpace);tool(572,72,96,"\\qquad",Tool::QquadSpace);group_label(470,198,T("Spacing"));
    b(688,42,130,T("Export settings..."),Command::ExportAll,54);group_label(688,130,T("Output"));ribbon_pages_[3]->end();
    ribbon_pages_[4]=new Fl_Group(0,34,1060,88);
    b(10,42,64,"−",Command::ZoomOut);b(80,42,64,"+",Command::ZoomIn);b(10,72,134,T("Fit preview"),Command::Fit);group_label(10,134,T("Zoom"));
    b(164,42,150,T("Show formula list"),Command::ShowList);b(164,72,150,T("Show preview"),Command::ShowPreview);group_label(164,150,T("Panels"));
    b(334,42,150,T("Example formulas"),Command::Example);b(334,72,150,T("ReliefMath help"),Command::Help);group_label(334,150,T("Learning"));
    b(504,42,130,T("Copy LaTeX"),Command::Copy);b(504,72,130,T("Copy image"),Command::CopyImage);group_label(504,130,T("Clipboard"));
    b(654,42,130,T("Cancel task"),Command::CancelJob,54);group_label(654,130,T("Background tasks"));ribbon_pages_[4]->end();
}
void MathWindow::show_tab(int tab){for(int n=0;n<5;n++){if(n==tab)ribbon_pages_[n]->show();else ribbon_pages_[n]->hide();tabs_[n]->value(n==tab);}ribbon_scroll_->scroll_to(0,0);ribbon_->redraw();}
Formula *MathWindow::current(){for(size_t n=0;n<document.formulas.size();n++)if(document.formulas[n].id==active_)return &document.formulas[n];return nullptr;}
void MathWindow::selected(Fl_Widget*,void *p){auto *window=static_cast<MathWindow*>(p);int index=window->list_->value();if(index>0&&size_t(index)<=window->visible_ids_.size())window->select_formula(window->visible_ids_[size_t(index-1)]);}
void MathWindow::select_formula(FormulaId id){if(active_==id)return;flush_edit();active_=id;renderer_->cancel();preview->clear();displayed_=Snapshot{};refresh_source();}
void MathWindow::refill_list(){
    Array<FormulaId> selected_ids;for(int n=1;n<=list_->size();n++)if(list_->selected(n)&&size_t(n)<=visible_ids_.size())selected_ids.push(visible_ids_[size_t(n-1)]);
    visible_ids_.clear();list_->clear();const char *query=filter_->value();
    for(size_t n=0;n<document.formulas.size();n++){
        const Formula &f=document.formulas[n];if(*query&&!strstr(f.name.c_str(),query)&&!strstr(f.source.c_str(),query))continue;
        if(!visible_ids_.push(f.id))break;list_->add(f.name.c_str());
        bool choose=f.id==active_;for(size_t s=0;s<selected_ids.size();s++)if(selected_ids[s]==f.id)choose=true;
        if(choose)list_->select(list_->size(),1);
    }
}
void MathWindow::update_status(const char *message){
    if(message)notice_.set(message);status_->copy_label(notice_.empty()?T("Ready"):notice_.c_str());
    Formula *f=current();size_t chars=0;if(f)for(size_t n=0;n<f->source.size();n++)if((static_cast<unsigned char>(f->source.c_str()[n])&0xc0)!=0x80)chars++;
    char text[768];snprintf(text,sizeof text,T("%zu formulas · %zu characters · %d%%"),document.formulas.size(),chars,int(zoom_*100));summary_->copy_label(text);
    const char *base=path_.empty()?T("Untitled"):strrchr(path_.c_str(),'/');if(!base)base=path_.c_str();else if(*base=='/')base++;
    snprintf(text,sizeof text,"%s%s — ReliefMath",history.is_dirty(document)?"* ":"",base);copy_label(text);
    if(history.can_undo()||history.in_transaction())undo_->activate();else undo_->deactivate();if(history.can_redo())redo_->activate();else redo_->deactivate();
    if(f){
        const int count=int(sizeof(Sizes)/sizeof(Sizes[0]));int selected=-1;
        if(size_choice_->size()>count+1)size_choice_->remove(count);
        for(int n=0;n<count;n++)if(Sizes[n]==f->style.point_size)selected=n;
        if(selected<0){char label[32];snprintf(label,sizeof label,"%u pt",f->style.point_size);size_choice_->add(label);selected=count;}
        size_choice_->value(selected);display_choice_->value(f->style.display?0:1);
    }
}
void MathWindow::refresh_source(bool reset){
    Formula *f=current();if(!f&&document.formulas.size()){active_=document.formulas[0].id;f=current();}
    loading_=true;source_.text(f?f->source.c_str():"");loading_=false;Error e;
    if(reset)editor_set_text(core,f?f->source.c_str():"",e);
    if(f)editor->activate();else editor->deactivate();restyle();update_status();schedule_render();
}
void MathWindow::source_changed(int pos,int inserted,int removed,int,const char*,void *p){
    MathWindow &w=*static_cast<MathWindow*>(p);if(w.loading_||(!inserted&&!removed)||!w.current())return;
    if(w.document_locked_){w.loading_=true;w.source_.text(w.current()->source.c_str());w.loading_=false;return;}
    Error e;char *text=w.source_.text_range(pos,pos+inserted);bool okay=text&&editor_replace_text(w.core,{size_t(pos),size_t(pos+removed)},text,e);free(text);
    if(okay&&!w.history.in_transaction())okay=w.history.begin(w.document,e);
    if(okay)okay=set_formula_source(w.document,w.active_,w.core.source.c_str(),e);
    if(!okay){Error reason=e;w.loading_=true;w.source_.text(w.current()->source.c_str());w.loading_=false;editor_set_text(w.core,w.current()->source.c_str(),e);w.update_status(reason.message.c_str());}
    else {w.last_edit_=now();w.schedule_render();w.update_status(T("Modified"));}
    w.restyle();
}
void MathWindow::flush_edit(){if(history.in_transaction()){Error e;if(!history.commit(document,e)){refresh_source();update_status(e.message.c_str());}}}
void MathWindow::restyle(){
    size_t size=size_t(source_.length());Bytes style;if(!style.resize(size+1))return;memset(style.data(),'A',size);style.data()[size]=0;
    Array<LexToken> tokens;Error e;size_t count=0;if(tokens.resize(size+1)&&lex_source(core.source.c_str(),core.source.size(),tokens.data(),tokens.size(),count,e))
        for(size_t n=0;n<count;n++){char value='A';switch(tokens[n].kind){case TokenKind::Command:value='B';break;case TokenKind::GroupOpen:case TokenKind::GroupClose:value='C';break;case TokenKind::Comment:value='D';break;case TokenKind::Error:value='E';break;default:break;}for(size_t at=tokens[n].range.begin;at<tokens[n].range.end&&at<size;at++)style.data()[at]=uint8_t(value);}
    int caret=editor->insert_position();Range match{};
    if(caret>0&&matching_bracket(core.source.c_str(),size_t(caret-1),match,e)&&match.begin<size){style.data()[match.begin]='F';style.data()[size_t(caret-1)]='F';}
    styles_.text(reinterpret_cast<const char*>(style.data()));editor->redisplay_range(0,int(size));
}
void MathWindow::schedule_render(){render_pending_=current()!=nullptr;last_edit_=now();stale_=preview->raster().width!=0;render_error_.clear();error_button_->hide();render_label_->copy_label(stale_?T("Old preview · waiting for render"):T("Waiting for render..."));}
void MathWindow::tick(void *p){Fl::repeat_timeout(.03,tick,p);auto *w=static_cast<MathWindow*>(p);w->poll();}
void MathWindow::recovery_timer(void *p){auto *w=static_cast<MathWindow*>(p);w->recovery_tick();Fl::repeat_timeout(60,recovery_timer,p);}
void MathWindow::poll(){
    if(history.in_transaction()&&now()-last_edit_>.25)flush_edit();
    if(render_pending_&&now()-last_edit_>.18&&current()){
        Snapshot s;s.formula=*current();s.session_id=session_;s.request_id=++request_;s.artifacts=WantRaster;
        Error e;if(renderer_->submit(s,e)){submitted_=s;render_pending_=false;render_label_->copy_label(stale_?T("Old preview · rendering..."):T("Rendering..."));}else {render_error_=e;render_pending_=false;render_label_->copy_label(e.message.c_str());error_button_->show();}
    }
    renderer_->pump();Rendered r;Error e;
    if(renderer_->take(r,e)){
        Snapshot expected;expected.session_id=session_;if(current())expected.formula=*current();
        if(same_snapshot(r.snapshot,expected)&&r.request_id==submitted_.request_id){
            if(e.code==Code::None){displayed_=r.snapshot;preview->set_raster(static_cast<Raster&&>(r.raster));stale_=false;render_error_.clear();error_button_->hide();render_label_->copy_label(T("Preview · up to date"));}
            else {render_error_=e;stale_=preview->raster().width!=0;char label[768];snprintf(label,sizeof label,"%s: %s",stale_?T("Old preview · error"):T("Render error"),T(e.message.c_str()));render_label_->copy_label(label);error_button_->show();update_status(label);}
        }
    }
    if(!interaction_depth_)finish_file_job();exports_->pump();export_progress();
}
void MathWindow::commit_core(bool select_stop){
    if(document_locked_||!current())return;flush_edit();Error e;if(!history.begin(document,e)){refresh_source();update_status(e.message.c_str());return;}
    if(!set_formula_source(document,active_,core.source.c_str(),e)||!history.commit(document,e)){refresh_source();update_status(e.message.c_str());return;}
    loading_=true;source_.text(core.source.c_str());loading_=false;restyle();schedule_render();update_status(T("Modified"));
    if(select_stop&&!core.stops.empty()){Range r=core.stops[0];source_.select(int(r.begin),int(r.end));editor->insert_position(int(r.end));}
    editor->take_focus();
}
void MathWindow::insert_tool(Tool tool){
    if(document_locked_||!current())return;ToolArgs args{};args.rows=2;args.columns=2;int a=editor->insert_position(),b=a;source_.selection_position(&a,&b);
    if(a!=b){char *text=source_.text_range(a,b);args.selected.set(text);free(text);}Snippet s;Error e;
    if(!build_snippet(tool,args,s,e)||!editor_insert_snippet(core,{size_t(a),size_t(b)},s,e)){update_status(e.message.c_str());return;}commit_core(true);
}
void MathWindow::insert_source(const char *text){if(document_locked_)return;Snippet s;Error e;if(!s.source.set(text))return;int a=editor->insert_position(),b=a;source_.selection_position(&a,&b);if(editor_insert_snippet(core,{size_t(a),size_t(b)},s,e))commit_core(false);else update_status(e.message.c_str());}
bool MathWindow::placeholder(bool reverse){bool handled=false;Range r{};Error e;if(editor_tab(core,reverse,handled,r,e)&&handled){source_.select(int(r.begin),int(r.end));editor->insert_position(int(r.end));editor->show_insert_position();return true;}return false;}
void MathWindow::style_changed(Fl_Widget *control,void *p){
    auto *w=static_cast<MathWindow*>(p);if(w->document_locked_)return;w->flush_edit();Formula *f=w->current();if(!f)return;Style style=f->style;
    if(control==w->size_choice_){int selected=w->size_choice_->value();if(selected>=0&&selected<int(sizeof(Sizes)/sizeof(Sizes[0])))style.point_size=uint16_t(Sizes[selected]);}
    else if(control==w->display_choice_)style.display=w->display_choice_->value()==0;
    Error e;if(w->history.begin(w->document,e)&&set_formula_style(w->document,w->active_,style,e)&&w->history.commit(w->document,e)){w->schedule_render();w->update_status(T("Modified"));}else w->update_status(e.message.c_str());
}
void MathWindow::run(Command command){
    UiGuard guard(interaction_depth_);
    if(document_locked_&&command!=Command::CancelJob&&command!=Command::ZoomIn&&command!=Command::ZoomOut&&command!=Command::Fit&&command!=Command::ShowList&&command!=Command::ShowPreview&&command!=Command::Help){update_status(T("Opening document · wait or cancel the file task"));return;}
    if(command==Command::New||command==Command::Open||command==Command::Save||command==Command::SaveAs||command==Command::Close||command==Command::Example){file_action(command);return;}
    if(command==Command::ExportSvg||command==Command::ExportPng||command==Command::ExportLatex||command==Command::ExportSelected||command==Command::ExportAll){export_action(command);return;}
    if(command==Command::CancelJob){renderer_->cancel();render_pending_=false;io_->cancel();exports_->cancel();update_status(T("Cancelling task..."));return;}
    flush_edit();Error e;bool changed=false,okay=true;Formula *f=current();
    switch(command){
    case Command::Undo:okay=history.undo(document,e);changed=okay;break;case Command::Redo:okay=history.redo(document,e);changed=okay;break;
    case Command::Cut:Fl_Text_Editor::kf_cut(0,editor);return;
    case Command::Copy:{int a=0,b=0;if(source_.selection_position(&a,&b)&&a!=b){char *text=source_.selection_text();Fl::copy(text,int(strlen(text)),1);free(text);}else if(f)Fl::copy(f->source.c_str(),int(f->source.size()),1);return;}
    case Command::Paste:editor->take_focus();Fl::paste(*editor,1);return;
    case Command::SelectAll:source_.select(0,source_.length());editor->take_focus();return;
    case Command::Find:find_dialog(false);return;case Command::Replace:find_dialog(true);return;case Command::Complete:complete();return;
    case Command::Symbols:symbol_dialog();return;case Command::Templates:symbol_dialog(true);return;case Command::Favorites:symbol_dialog(false,true);return;
    case Command::AddFavorite:favorite_dialog();return;
    case Command::Fraction:structure_dialog(Tool::Fraction);return;case Command::Root:structure_dialog(Tool::Root);return;
    case Command::Scripts:structure_dialog(Tool::Superscript);return;case Command::Integral:structure_dialog(Tool::Integral);return;
    case Command::Sum:structure_dialog(Tool::Sum);return;case Command::Matrix:structure_dialog(Tool::Matrix);return;
    case Command::Cases:structure_dialog(Tool::Cases);return;case Command::Aligned:structure_dialog(Tool::Aligned);return;case Command::Text:structure_dialog(Tool::Text);return;
    case Command::Add:case Command::Duplicate:case Command::Rename:case Command::Delete:case Command::MoveUp:case Command::MoveDown:{
        if(!history.begin(document,e)){okay=false;break;}FormulaId id=active_;
        if(command==Command::Add){char label[64];snprintf(label,sizeof label,T("Formula %zu"),document.formulas.size()+1);okay=add_formula(document,label,"x",id,e);}
        else if(command==Command::Duplicate&&f)okay=copy_formula(document,active_,id,e);
        else if(command==Command::Rename&&f){const char *name=fl_input(T("Formula name:"),f->name.c_str());okay=name?rename_formula(document,active_,name,e):true;}
        else if(command==Command::Delete&&f){if(fl_choice(T("Delete the current formula?"),T("Cancel"),T("Delete"),nullptr)==1)okay=delete_formula(document,active_,e);}
        else if(f){size_t index=0;while(index<document.formulas.size()&&document.formulas[index].id!=active_)index++;if(command==Command::MoveUp&&index>0)okay=move_formula(document,active_,index-1,e);if(command==Command::MoveDown&&index+1<document.formulas.size())okay=move_formula(document,active_,index+1,e);}
        if(!history.commit(document,e))okay=false;active_=id;changed=true;break;
    }
    case Command::Color:case Command::Padding:{
        if(!f)return;Style style=f->style;
        if(command==Command::Color){double r=(style.rgba>>24)/255.,g=((style.rgba>>16)&255)/255.,b=((style.rgba>>8)&255)/255.;if(!fl_color_chooser(T("Formula color"),r,g,b))return;style.rgba=(uint32_t(r*255+.5)<<24)|(uint32_t(g*255+.5)<<16)|(uint32_t(b*255+.5)<<8)|(style.rgba&255);}
        else {char value[16];snprintf(value,sizeof value,"%u",style.padding);const char *answer=fl_input(T("Padding in points (0–4096):"),value);if(!answer)return;char *end;long n=strtol(answer,&end,10);if(*end||n<0||n>4096){update_status(T("Invalid padding value"));return;}style.padding=uint16_t(n);}
        if(history.begin(document,e)&&set_formula_style(document,active_,style,e))okay=history.commit(document,e);else okay=false;changed=okay;break;
    }
    case Command::CopyImage:{
        Snapshot expected;expected.session_id=session_;if(f)expected.formula=*f;
        if(stale_||render_pending_||!same_snapshot(displayed_,expected)||!preview->raster().width){update_status(T("Wait for a successful current preview before copying an image"));return;}
        const Raster &r=preview->raster();Bytes rgb;if(!rgb.resize(size_t(r.width)*r.height*3)){update_status(T("Cannot allocate clipboard image"));return;}
        for(size_t n=0;n<size_t(r.width)*r.height;n++)for(size_t c=0;c<3;c++)rgb.data()[n*3+c]=uint8_t((r.rgba.data()[n*4+c]*unsigned(r.rgba.data()[n*4+3])+255u*(255-r.rgba.data()[n*4+3]))/255);
        Fl::copy_image(rgb.data(),int(r.width),int(r.height),1);update_status(T("Rendered image copied"));return;
    }
    case Command::ZoomIn:zoom_*=1.25;if(zoom_>8)zoom_=8;preview->zoom(zoom_);update_status();return;
    case Command::ZoomOut:zoom_/=1.25;if(zoom_<.05)zoom_=.05;preview->zoom(zoom_);update_status();return;
    case Command::Fit:zoom_=1;preview->fit();update_status();return;
    case Command::ShowList:show_list_=!show_list_;layout_panels();return;case Command::ShowPreview:show_preview_=!show_preview_;layout_panels();return;
    case Command::ErrorLocation:{size_t a=render_error_.begin,b=render_error_.end;if(b<=a)b=a+1;if(b>core.source.size())b=core.source.size();if(a>b)a=b;source_.select(int(a),int(b));editor->insert_position(int(a));editor->show_insert_position();editor->take_focus();return;}
    case Command::Help:help_dialog();return;default:return;
    }
    if(changed){if(!current())active_=document.formulas.empty()?0:document.formulas[0].id;refill_list();refresh_source();}
    if(!okay)update_status(e.message.c_str());else if(changed)update_status(T("Modified"));
}
void MathWindow::layout_panels(){
    OfficeLayout p=OfficeLayout::compute(w(),h(),show_list_,show_preview_);
    if(show_list_)list_panel_->show();else list_panel_->hide();if(show_preview_)preview_panel_->show();else preview_panel_->hide();
    if(show_list_)list_panel_->resize(p.list.x,p.list.y,p.list.width,p.list.height);
    edit_panel_->resize(p.editor.x,p.editor.y,p.editor.width,p.editor.height);
    if(show_preview_)preview_panel_->resize(p.preview.x,p.preview.y,p.preview.width,p.preview.height);tile_->init_sizes();redraw();
}
void MathWindow::resize(int x,int y,int width,int height){Fl_Double_Window::resize(x,y,width,height);if(top_){top_->resize(0,0,width,34);ribbon_->resize(0,34,width,100);bottom_->resize(0,height-28,width,28);tile_->resize(0,140,width,height-174);layout_panels();tile_->init_sizes();}}
int MathWindow::handle(int event){
    if(event==FL_SHORTCUT&&(Fl::event_state()&FL_CTRL)){
        int key=Fl::event_key();switch(key){case 'n':run(Command::New);return 1;case 'o':run(Command::Open);return 1;case 's':run((Fl::event_state()&FL_SHIFT)?Command::SaveAs:Command::Save);return 1;case 'z':run((Fl::event_state()&FL_SHIFT)?Command::Redo:Command::Undo);return 1;case 'y':run(Command::Redo);return 1;case 'f':run(Command::Find);return 1;case 'h':run(Command::Replace);return 1;default:break;}
    }
    return Fl_Double_Window::handle(event);
}
}
