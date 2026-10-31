#include "view.h"
#include "window.h"
#include "../fltk_support.h"
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_draw.H>
#include <math.h>
#include <stdio.h>
#include <string.h>
namespace rw {
namespace {
constexpr int Bar=16,Gap=22,Padding=20;
Fl_Color rgb_color(uint32_t rgb) { return fl_rgb_color(rgb>>16,(rgb>>8)&255,rgb&255); }
bool blank_char(uint32_t cp) { return cp==' ' || cp=='\t' || cp=='\n' || cp=='\f' || cp==0x3000; }
bool cjk(uint32_t cp) { return (cp>=0x2e80&&cp<=0x9fff)||(cp>=0xf900&&cp<=0xfaff)||(cp>=0x20000&&cp<=0x323af); }
void decoration(const Text &text,unsigned page,int X,int Y,int width) {
    char output[2048]; size_t at=0;
    for(size_t i=0;i<text.size()&&at+8<sizeof(output);) {
        if(!strncmp(text.c_str()+i,"{page}",6)) { at+=snprintf(output+at,sizeof(output)-at,"%u",page+1); i+=6; }
        else output[at++]=text.c_str()[i++];
    }
    output[at]=0; fl_font(FL_HELVETICA,12); fl_color(fl_rgb_color(104,104,104));
    fl_draw(output,X,Y,width,20,FL_ALIGN_CENTER|FL_ALIGN_INSIDE|FL_ALIGN_CLIP);
}
}
PageView::PageView(int X,int Y,int W,int H,WordWindow *owner):Fl_Group(X,Y,W,H),owner_(owner) {
    box(FL_NO_BOX); begin();
    vertical_=new Fl_Scrollbar(X+W-Bar,Y,Bar,H-Bar); vertical_->type(FL_VERTICAL); vertical_->callback(scroll_cb,this);
    horizontal_=new Fl_Scrollbar(X,Y+H-Bar,W-Bar,Bar); horizontal_->type(FL_HORIZONTAL); horizontal_->callback(scroll_cb,this);
    end(); resizable(nullptr); Fl::add_timeout(.5,blink_cb,this);
}
PageView::~PageView() { Fl::remove_timeout(blink_cb,this); Fl::remove_timeout(drag_cb,this); }
double PageView::scale() const { return zoom_*96.0/7200.0; }
int PageView::canvas_width() const { return w()-Bar; }
int PageView::ruler_height() const { return ruler_?24:0; }
int PageView::canvas_height() const { return h()-Bar-ruler_height(); }
bool PageView::layout_current() const { return laid_out_==owner_->editor.doc.revision; }
double PageView::content_width() const {
    double paper=owner_->editor.doc.page.width*scale()+Padding*2;
    return paper>canvas_width()?paper:canvas_width();
}
double PageView::content_height() const { return Padding*2+layout.pages*owner_->editor.doc.page.height*scale()+(layout.pages-1)*Gap; }
double PageView::paper_left() const { return (content_width()-owner_->editor.doc.page.width*scale())/2; }
double PageView::paper_top(unsigned page) const { return Padding+page*(owner_->editor.doc.page.height*scale()+Gap); }
int PageView::font(const TextStyle &s) {
    unsigned family=unsigned(s.family); if(family>2) family=0;
    return 16+int(family)*4+(s.bold?1:0)+(s.italic?2:0);
}
Metrics PageView::measure(void *,uint32_t cp,const TextStyle &style) {
    char encoded[4]; unsigned n=encode_utf8(cp,encoded);
    fl_font(font(style),int(round(style.size*96.0/72.0)));
    Metrics result;
    result.advance=n?float(fl_width(encoded,int(n))*72.0/96.0):0;
    if(cp=='\n'||cp=='\f'||cp==0x200d||(cp>=0xfe00&&cp<=0xfe0f)) result.advance=0;
    result.ascent=float((fl_height()-fl_descent())*72.0/96.0);
    result.descent=float(fl_descent()*72.0/96.0);
    return result;
}
void PageView::update_bars() {
    double vh=canvas_height(),vw=canvas_width();
    double total=content_height(); if(total<vh) total=vh;
    double value=vertical_->value(); if(value>total-vh) value=total-vh; if(value<0) value=0;
    vertical_->value(value,vh,0,total); vertical_->linesize(32);
    total=content_width(); value=horizontal_->value(); if(value>total-vw) value=total-vw; if(value<0) value=0;
    horizontal_->value(value,vw,0,total); horizontal_->linesize(32);
}
bool PageView::reflow(bool force) {
    if(!force && laid_out_==owner_->editor.doc.revision) return true;
    if(!layout.build(owner_->editor.doc,measure)) { owner_->message(layout.error.c_str()); return false; }
    laid_out_=owner_->editor.doc.revision; update_bars(); redraw(); return true;
}
void PageView::set_zoom(int value) {
    if(value<25) value=25;
    if(value>300) value=300;
    double factor=double(value)/zoom_; zoom_=value;
    vertical_->value(vertical_->value()*factor); horizontal_->value(horizontal_->value()*factor);
    update_bars(); redraw(); owner_->selected();
}
void PageView::fit_width() { set_zoom(int((canvas_width()-Padding*2)*72.0*100/(owner_->editor.doc.page.width*96.0))); }
void PageView::show_ruler(bool value) { ruler_=value; update_bars(); redraw(); }
unsigned PageView::current_page() const {
    if(!layout_current()) return 0;
    size_t line=layout.line_for(owner_->editor.caret,previous_affinity_);
    return line<layout.lines.size()?layout.lines[line].page:0;
}
void PageView::ensure_caret() {
    if(!layout_current()||layout.lines.empty()) return;
    size_t i=layout.line_for(owner_->editor.caret,previous_affinity_); if(i>=layout.lines.size()) return;
    const Line &line=layout.lines[i];
    double top=paper_top(line.page)+line.y*scale(),bottom=top+line.height*scale();
    double left=paper_left()+layout.caret_x(owner_->editor.caret,i)*scale();
    if(top<vertical_->value()+4) vertical_->value(top-8);
    else if(bottom>vertical_->value()+canvas_height()-4) vertical_->value(bottom-canvas_height()+12);
    if(left<horizontal_->value()+8) horizontal_->value(left-12);
    else if(left>horizontal_->value()+canvas_width()-8) horizontal_->value(left-canvas_width()+16);
    update_bars(); blink_=true; redraw();
}
void PageView::resize(int X,int Y,int W,int H) {
    Fl_Widget::resize(X,Y,W,H);
    vertical_->resize(X+W-Bar,Y,Bar,H-Bar);
    horizontal_->resize(X,Y+H-Bar,W-Bar,Bar); update_bars(); redraw();
}
void PageView::draw() {
    fl_push_clip(x(),y(),canvas_width(),h()-Bar);
    fl_color(fl_rgb_color(177,181,187)); fl_rectf(x(),y(),canvas_width(),h()-Bar);
    if(!layout_current()) {
        fl_color(FL_BLACK); fl_font(FL_HELVETICA,14);
        fl_draw(layout.error.empty()?T("Operation failed"):layout.error.c_str(),
                x()+24,y()+ruler_height()+24,canvas_width()-48,140,
                FL_ALIGN_TOP_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_WRAP);
        fl_pop_clip(); draw_children(); return;
    }
    const Document &doc=owner_->editor.doc;
    double s=scale(),left=x()+paper_left()-horizontal_->value();
    int origin=y()+ruler_height();
    double pitch=doc.page.height*s+Gap;
    int first=int((vertical_->value()-Padding)/pitch); if(first<0) first=0;
    int last=int((vertical_->value()+canvas_height())/pitch)+1;
    if(last>=int(layout.pages)) last=int(layout.pages)-1;
    fl_push_clip(x(),origin,canvas_width(),canvas_height());
    for(int page=first;page<=last;++page) {
        double top=origin+paper_top(unsigned(page))-vertical_->value();
        int width=int(doc.page.width*s),height=int(doc.page.height*s);
        fl_color(fl_rgb_color(135,139,145)); fl_rectf(int(left)+3,int(top)+3,width,height);
        fl_color(FL_WHITE); fl_rectf(int(left),int(top),width,height);
        fl_color(fl_rgb_color(145,149,154)); fl_rect(int(left),int(top),width,height);
        decoration(doc.page.header,unsigned(page),int(left+doc.page.left*s),int(top+doc.page.top*s/2)-10,int((doc.page.width-doc.page.left-doc.page.right)*s));
        decoration(doc.page.footer,unsigned(page),int(left+doc.page.left*s),int(top+(doc.page.height-doc.page.bottom/2)*s)-10,int((doc.page.width-doc.page.left-doc.page.right)*s));
        if(doc.page.page_numbers) {
            char page_number[32]; snprintf(page_number,sizeof(page_number),"%d",page+1);
            fl_font(FL_HELVETICA,12); fl_color(fl_rgb_color(104,104,104));
            fl_draw(page_number,int(left+width-50*s),int(top+height-doc.page.bottom*s/2));
        }
    }
    size_t lo=0,hi=layout.lines.size();
    while(lo<hi) { size_t middle=(lo+hi)/2; if(int(layout.lines[middle].page)<first) lo=middle+1; else hi=middle; }
    size_t a=owner_->editor.selection_start(),b=owner_->editor.selection_end();
    for(size_t li=lo;layout_current()&&li<layout.lines.size();++li) {
        const Line &line=layout.lines[li]; if(int(line.page)>last) break;
        double top=origin+paper_top(line.page)-vertical_->value();
        if(top+(line.y+line.height)*s<origin||top+line.y*s>origin+canvas_height()) continue;
        if(line.paragraph_start && line.paragraph<doc.paragraphs.size()) {
            ListKind kind=doc.paragraphs[line.paragraph].style.list;
            if(kind!=ListKind::Plain) {
                char marker[32]; if(kind==ListKind::Number) snprintf(marker,sizeof(marker),"%u.",line.list_number); else strcpy(marker,"•");
                fl_font(font(doc.style_at(line.start)),int(doc.style_at(line.start).size*s)); fl_color(rgb_color(0x303030));
                fl_draw(marker,int(left+(line.x-20)*s),int(top+line.baseline*s));
            }
        }
        for(size_t i=line.start;i<line.end && i<doc.chars.size();) {
            const GlyphBox &box=layout.boxes[i];
            size_t next=next_cluster(doc,i); if(next>line.end) next=line.end; if(next<=i) next=i+1;
            float advance=0; for(size_t k=i;k<next;++k) advance+=layout.boxes[k].width;
            int X=int(left+box.x*s),Y=int(top+line.y*s),W=int(ceil(advance*s));
            bool selected=i<b&&next>a;
            size_t cluster_start=i;
            while(cluster_start>line.start&&!layout.grapheme_boundaries[cluster_start]) --cluster_start;
            if(selected) {
                fl_color(fl_rgb_color(58,103,191));
                fl_rectf(X,Y,W?W:2,int(ceil(line.height*s)));
            }
            for(size_t run=i;run<next;) {
                const TextStyle &style=doc.styles[doc.chars[run].style];
                size_t run_end=run+1;
                while(run_end<next&&doc.styles[doc.chars[run_end].style]==style) ++run_end;
                float run_advance=0;
                for(size_t k=run;k<run_end;++k) run_advance+=layout.boxes[k].width;
                size_t run_origin=run;
                if(run_advance<=0) run_origin=cluster_start;
                int run_x=int(left+layout.boxes[run_origin].x*s);
                int run_width=int(ceil(run_advance*s));
                if(!selected&&style.highlight) {
                    fl_color(rgb_color(style.background));
                    fl_rectf(run_x,Y,run_width?run_width:2,int(ceil(line.height*s)));
                }
                if(doc.chars[run].codepoint!='\t'&&doc.chars[run].codepoint!='\n'&&doc.chars[run].codepoint!='\f') {
                    size_t chunk=run;
                    while(chunk<run_end) {
                        char bytes[512]; unsigned at=0; size_t chunk_end=chunk;
                        while(chunk_end<run_end) {
                            char encoded[4]; unsigned count=encode_utf8(doc.chars[chunk_end].codepoint,encoded);
                            if(!count||at+count>=sizeof(bytes)) break;
                            memcpy(bytes+at,encoded,count); at+=count; ++chunk_end;
                        }
                        if(chunk_end==chunk) break;
                        bytes[at]=0;
                        float chunk_advance=0;
                        for(size_t k=chunk;k<chunk_end;++k) chunk_advance+=layout.boxes[k].width;
                        size_t chunk_origin=chunk;
                        if(chunk_advance<=0) chunk_origin=cluster_start;
                        int chunk_x=int(left+layout.boxes[chunk_origin].x*s);
                        fl_font(font(style),int(round(style.size*s)));
                        fl_color(selected?FL_WHITE:rgb_color(style.foreground));
                        fl_draw(bytes,int(at),chunk_x,int(top+line.baseline*s));
                        chunk=chunk_end;
                    }
                    if(style.underline) fl_line(run_x,int(top+line.baseline*s+2),run_x+run_width,int(top+line.baseline*s+2));
                    if(style.strike) fl_line(run_x,int(top+(line.baseline-style.size*.32)*s),run_x+run_width,int(top+(line.baseline-style.size*.32)*s));
                }
                run=run_end;
            }
            i=next;
        }
        if(line.end<doc.chars.size()&&line.end>=a&&line.end<b&&doc.chars[line.end].codepoint=='\n') {
            fl_color(fl_rgb_color(58,103,191)); fl_rectf(int(left+layout.caret_x(line.end,li)*s),int(top+line.y*s),int(4*s),int(line.height*s));
        }
    }
    if(layout_current()&&blink_&&Fl::focus()==this&&!owner_->is_busy()&&layout.lines.size()) {
        size_t li=layout.line_for(owner_->editor.caret,previous_affinity_);
        if(li<layout.lines.size()) {
            const Line &line=layout.lines[li]; double top=origin+paper_top(line.page)-vertical_->value();
            int X=int(left+layout.caret_x(owner_->editor.caret,li)*s);
            fl_color(FL_BLACK); fl_rectf(X,int(top+line.y*s),1,int(ceil(line.height*s)));
            fl_set_spot(font(owner_->editor.typing_style),int(owner_->editor.typing_style.size*s),X,int(top+line.baseline*s),2,int(line.height*s),window());
        }
    }
    fl_pop_clip();
    if(ruler_) {
        fl_color(FL_BACKGROUND_COLOR); fl_rectf(x(),y(),canvas_width(),24);
        fl_color(FL_WHITE); fl_rectf(int(left),y()+2,int(doc.page.width*s),20);
        fl_color(fl_rgb_color(215,216,217)); fl_rectf(int(left),y()+2,int(doc.page.left*s),20);
        fl_rectf(int(left+(doc.page.width-doc.page.right)*s),y()+2,int(doc.page.right*s),20);
        fl_color(FL_DARK3); fl_font(FL_HELVETICA,10);
        for(unsigned mm=0;mm<unsigned(doc.page.width*25.4/72);mm+=5) {
            int X=int(left+mm*72/25.4*s); int tick=mm%10?4:7; fl_line(X,y()+22-tick,X,y()+22);
            if(mm&&mm%10==0) { char number[16]; snprintf(number,sizeof(number),"%u",mm/10); fl_draw(number,X-3,y()+12); }
        }
    }
    fl_pop_clip(); draw_children();
}
size_t PageView::point_to_offset(int X,int Y,size_t *line_out) const {
    if(!layout_current()||layout.lines.empty()) {
        if(line_out) *line_out=layout.lines.size();
        size_t caret=owner_->editor.caret;
        return caret<owner_->editor.doc.chars.size()?caret:owner_->editor.doc.chars.size();
    }
    double world=Y-y()-ruler_height()+vertical_->value()-Padding;
    double pitch=owner_->editor.doc.page.height*scale()+Gap;
    int page=int(floor(world/pitch)); if(page<0) page=0; if(page>=int(layout.pages)) page=int(layout.pages)-1;
    float px=float((X-x()+horizontal_->value()-paper_left())/scale());
    float py=float((Y-y()-ruler_height()+vertical_->value()-paper_top(unsigned(page)))/scale());
    size_t result=layout.hit_test(unsigned(page),px,py);
    if(line_out) {
        *line_out=layout.line_for(result);
        if(*line_out&&layout.lines[*line_out-1].page==unsigned(page)) {
            const Line &prior=layout.lines[*line_out-1];
            if(prior.end==result&&py<prior.y+prior.height) --*line_out;
        }
    }
    return result;
}
void PageView::move_to(size_t offset,bool extend,bool previous) {
    size_t anchor=extend?owner_->editor.anchor:offset;
    owner_->editor.select(anchor,offset); previous_affinity_=previous; blink_=true;
    ensure_caret(); owner_->selected(); redraw();
}
void PageView::move_vertical(int amount,bool extend) {
    if(!layout_current()||layout.lines.empty()) return;
    size_t current=layout.line_for(owner_->editor.caret,previous_affinity_);
    if(desired_x_<0) desired_x_=layout.caret_x(owner_->editor.caret,current);
    int64_t target=int64_t(current)+amount; if(target<0) target=0; if(target>=int64_t(layout.lines.size())) target=layout.lines.size()-1;
    const Line &line=layout.lines[size_t(target)];
    size_t offset=layout.hit_test(line.page,desired_x_,line.y+line.height*.5f);
    move_to(offset,extend,offset==line.end);
}
void PageView::drag_select() {
    int top=y()+ruler_height(),bottom=top+canvas_height();
    if(drag_y_<top) vertical_->value(vertical_->value()-24);
    if(drag_y_>bottom) vertical_->value(vertical_->value()+24);
    update_bars(); size_t li=0;
    size_t offset=point_to_offset(drag_x_,drag_y_,&li);
    owner_->editor.select(owner_->editor.anchor,offset);
    previous_affinity_=li<layout.lines.size()&&layout.lines[li].end==offset;
    blink_=true; redraw(); owner_->selected();
}
void PageView::scroll_cb(Fl_Widget *,void *context) { static_cast<PageView *>(context)->redraw(); }
void PageView::blink_cb(void *context) {
    PageView *view=static_cast<PageView *>(context);
    if(Fl::focus()==view&&!view->owner_->is_busy()) { view->blink_=!view->blink_; view->redraw(); }
    Fl::repeat_timeout(.5,blink_cb,view);
}
void PageView::drag_cb(void *context) {
    PageView *view=static_cast<PageView *>(context);
    if(view->dragging_) { view->drag_select(); Fl::repeat_timeout(.06,drag_cb,view); }
}
int PageView::handle(int event) {
    if(owner_->is_busy()) return 1;
    Editor &editor=owner_->editor;
    if(event==FL_FOCUS||event==FL_UNFOCUS) { blink_=true; redraw(); return 1; }
    if(event==FL_PASTE) { owner_->paste_text(Fl::event_text(),size_t(Fl::event_length())); return 1; }
    if(event==FL_MOUSEWHEEL) {
        if(Fl::event_state(FL_CTRL)) set_zoom(zoom_-Fl::event_dy()*10);
        else { vertical_->value(vertical_->value()+Fl::event_dy()*48); horizontal_->value(horizontal_->value()+Fl::event_dx()*48); update_bars(); redraw(); }
        return 1;
    }
    if(event==FL_PUSH) {
        if(Fl::event_x()>=x()+canvas_width()||Fl::event_y()>=y()+h()-Bar) return Fl_Group::handle(event);
        if(Fl::event_y()<y()+ruler_height()) return 1;
        take_focus(); Fl::compose_reset(); desired_x_=-1; size_t li=0;
        size_t at=point_to_offset(Fl::event_x(),Fl::event_y(),&li);
        if(Fl::event_button()==FL_RIGHT_MOUSE) {
            if(at<editor.selection_start()||at>editor.selection_end()) move_to(at,false);
            Fl_Menu_Button menu(Fl::event_x(),Fl::event_y(),0,0); menu.type(Fl_Menu_Button::POPUP3);
            const char *labels[]={T("Cut"),T("Copy"),T("Paste"),T("Select all"),T("Paragraph...")};
            Command commands[]={Command::Cut,Command::Copy,Command::Paste,Command::SelectAll,Command::Paragraph};
            for(const char *label:labels) menu.add(label);
            const Fl_Menu_Item *choice=menu.popup(); if(choice) owner_->act(commands[choice-menu.menu()]); return 1;
        }
        if(Fl::event_button()==FL_MIDDLE_MOUSE) { move_to(at,false); owner_->paste_plain_=true; Fl::paste(*this,0); return 1; }
        if(Fl::event_clicks()>0&&!editor.doc.chars.empty()) {
            size_t a=at,b=at;
            if(Fl::event_clicks()>1) {
                size_t p=editor.doc.paragraph_at(at); a=editor.doc.paragraphs[p].start;
                b=p+1<editor.doc.paragraphs.size()?editor.doc.paragraphs[p+1].start:editor.doc.chars.size();
            } else if(at<editor.doc.chars.size()&&cjk(editor.doc.chars[at].codepoint)) b=next_cluster(editor.doc,at);
            else { while(a&&!blank_char(editor.doc.chars[a-1].codepoint)&&!cjk(editor.doc.chars[a-1].codepoint)) a=previous_cluster(editor.doc,a);
                while(b<editor.doc.chars.size()&&!blank_char(editor.doc.chars[b].codepoint)&&!cjk(editor.doc.chars[b].codepoint)) b=next_cluster(editor.doc,b); }
            editor.select(a,b); owner_->selected(); redraw(); return 1;
        }
        move_to(at,Fl::event_state(FL_SHIFT),li<layout.lines.size()&&layout.lines[li].end==at);
        dragging_=true; drag_x_=Fl::event_x(); drag_y_=Fl::event_y(); Fl::add_timeout(.06,drag_cb,this); return 1;
    }
    if(event==FL_DRAG&&dragging_) { drag_x_=Fl::event_x(); drag_y_=Fl::event_y(); drag_select(); return 1; }
    if(event==FL_RELEASE&&dragging_) {
        dragging_=false; Fl::remove_timeout(drag_cb,this);
        Text text; if(editor.selection_start()!=editor.selection_end()&&document_text(editor.doc,editor.selection_start(),editor.selection_end(),text)) Fl::copy(text.c_str(),int(text.size()),0);
        return 1;
    }
    if(event==FL_KEYDOWN) {
        int key=Fl::event_key(); bool shift=Fl::event_state(FL_SHIFT),ctrl=Fl::event_state(FL_CTRL);
        if(!Fl::compose_state&&!Fl::event_state(FL_ALT)) {
            size_t at=editor.caret;
            if(key==FL_Left||key==FL_Right) {
                desired_x_=-1;
                bool had_selection=editor.anchor!=editor.caret;
                if(!shift&&had_selection) at=key==FL_Left?editor.selection_start():editor.selection_end();
                else if(!ctrl&&!shift&&!had_selection&&layout_current()) {
                    size_t line_index=layout.line_for(at,previous_affinity_);
                    if(key==FL_Left&&!previous_affinity_&&line_index>0&&layout.lines[line_index].start==at) {
                        const Line &prior=layout.lines[line_index-1],&next=layout.lines[line_index];
                        if(prior.end==at&&prior.paragraph==next.paragraph&&!prior.paragraph_end) {
                            move_to(at,false,true); return 1;
                        }
                    } else if(key==FL_Right&&previous_affinity_&&line_index+1<layout.lines.size()&&layout.lines[line_index].end==at) {
                        const Line &prior=layout.lines[line_index],&next=layout.lines[line_index+1];
                        if(next.start==at&&prior.paragraph==next.paragraph&&!prior.paragraph_end) {
                            move_to(at,false,false); return 1;
                        }
                    }
                }
                if(at==editor.caret&&(!had_selection||shift)) {
                    if(key==FL_Left) { at=previous_cluster(editor.doc,at); if(ctrl) while(at&&!blank_char(editor.doc.chars[at-1].codepoint)&&!cjk(editor.doc.chars[at-1].codepoint)) at=previous_cluster(editor.doc,at); }
                    else { at=next_cluster(editor.doc,at); if(ctrl) while(at<editor.doc.chars.size()&&!blank_char(editor.doc.chars[at].codepoint)&&!cjk(editor.doc.chars[at].codepoint)) at=next_cluster(editor.doc,at); }
                }
                move_to(at,shift); return 1;
            }
            if(key==FL_Up||key==FL_Down||key==FL_Page_Up||key==FL_Page_Down) {
                int amount=(key==FL_Up||key==FL_Page_Up)?-1:1;
                if(key==FL_Page_Up||key==FL_Page_Down) amount*=int(canvas_height()/(18*scale()));
                move_vertical(amount,shift); return 1;
            }
            if(key==FL_Home||key==FL_End) {
                desired_x_=-1;
                if(ctrl) at=key==FL_Home?0:editor.doc.chars.size();
                else {
                    if(!layout_current()) return 1;
                    size_t li=layout.line_for(at,previous_affinity_);
                    if(li<layout.lines.size()) at=key==FL_Home?layout.lines[li].start:layout.lines[li].end;
                }
                move_to(at,shift,key==FL_End); return 1;
            }
            if(key==FL_BackSpace||key==FL_Delete) {
                if(editor.erase(key==FL_BackSpace)) owner_->changed(); else owner_->message(editor.error.c_str()); return 1;
            }
            if(key==FL_Enter||key==FL_KP_Enter||key==FL_Tab) {
                if(ctrl&&key!=FL_Tab) { owner_->act(Command::PageBreak); return 1; }
                if(ctrl) return 0;
                size_t p=editor.doc.paragraph_at(editor.caret);
                size_t end=p+1<editor.doc.paragraphs.size()?editor.doc.paragraphs[p+1].start-1:editor.doc.chars.size();
                if(key!=FL_Tab&&editor.doc.paragraphs[p].start==end&&editor.doc.paragraphs[p].style.list!=ListKind::Plain) {
                    ParagraphStyle style=editor.doc.paragraphs[p].style; style.list=ListKind::Plain;
                    if(editor.apply_paragraph(style,ParaList)) owner_->changed();
                    return 1;
                }
                if(editor.insert_utf8(key==FL_Tab?"\t":"\n",1)) owner_->changed(); else owner_->message(editor.error.c_str()); return 1;
            }
        }
        int remove_bytes=0;
        if(Fl::compose(remove_bytes)) {
            size_t a=editor.anchor,c=editor.caret,start=c;
            while(remove_bytes>0&&start) { char bytes[4]; --start; remove_bytes-=int(encode_utf8(editor.doc.chars[start].codepoint,bytes)); }
            if(start!=c) editor.select(start,c);
            if(Fl::event_length()||start!=c) {
                if(editor.insert_utf8(Fl::event_text(),size_t(Fl::event_length()))) owner_->changed();
                else { editor.select(a,c); owner_->message(editor.error.c_str()); }
            }
            return 1;
        }
    }
    return Fl_Group::handle(event);
}
}
