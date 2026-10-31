// Independent host process: create fixtures and inspect files saved by the GUI.
#include "document.h"
#include <stdio.h>
#include <string.h>

namespace {
int failures=0;
void check(bool ok,const char *what) { if(!ok) { fprintf(stderr,"FAIL: %s\n",what); ++failures; } }
bool text_of(const rw::Document &doc,rw::Text &text) { return rw::document_text(doc,0,doc.chars.size(),text); }
bool write_fixture(const char *directory) {
    rw::Editor editor;
    const char *body="ReliefWord 项目周报\n在 ReliefOS 中书写、整理和分享\n\n本周进展\n"
        "这是使用 ReliefWord 编写的中文文档。文字和页面一起显示，让编辑、排版和阅读保持直观。"
        "你可以选择一段文字，再从功能区调整字体、字号、颜色和对齐方式。\n\n"
        "接下来要做的事\n整理项目资料，写下重要的发现。\n与同事一起审阅，完善内容与表达。\n保存为原生文档，保留完整的格式。\n\n"
        "文字与页面\n中文与 English 可以在同一份文档中使用。用标题、列表和适当的段落间距，"
        "让一份报告更容易浏览。导入 TXT 或 HTML 后，可以继续编辑并另存为 .rword。\n\n"
        "Prepared with ReliefWord · ReliefOS";
    if(!editor.insert_utf8(body,strlen(body))) return false;
    for(size_t p=0;p<editor.doc.paragraphs.size();++p) {
        const size_t start=editor.doc.paragraphs[p].start;
        const size_t end=p+1<editor.doc.paragraphs.size()?editor.doc.paragraphs[p+1].start-1:editor.doc.chars.size();
        rw::TextStyle style; rw::ParagraphStyle paragraph;
        if(p==0) { style.size=24; style.foreground=0x244b7a; paragraph.after=12; }
        else if(p==1) { style.foreground=0x606970; paragraph.after=16; }
        else if(p==3||p==6||p==11) { style.size=16; style.bold=1; style.foreground=0x244b7a; paragraph.before=8; paragraph.after=8; }
        if(p>=7&&p<=9) paragraph.list=rw::ListKind::Bullet;
        editor.select(start,end);
        if(!editor.apply_style(style)||!editor.apply_paragraph(paragraph)) return false;
    }
    if(!editor.doc.title.set("ReliefWord 项目周报")||!editor.doc.author.set("ReliefOS")||
       !editor.doc.page.header.set("ReliefOS / 工作文档")||!editor.doc.page.footer.set("第 {page} 页")) return false;
    char path[4096]; snprintf(path,sizeof(path),"%s/word-fixture.rword",directory);
    rw::Text error;
    if(!rw::save_document(path,editor.doc,rw::FileFormat::Native,error)) { fprintf(stderr,"%s\n",error.c_str()); return false; }
    const char *html="<!doctype html><html><body><h1>HTML 中文导入</h1><div><p>Paragraph one</p></div><pre>code\n  spaces</pre><p><a href='https://example.invalid'>linked text</a></p></body></html>";
    snprintf(path,sizeof(path),"%s/import-source.html",directory);
    FILE *file=fopen(path,"wb"); if(!file) return false;
    bool ok=fwrite(html,1,strlen(html),file)==strlen(html); if(fclose(file)) ok=false;
    snprintf(path,sizeof(path),"%s/import-source.txt",directory);
    file=fopen(path,"wb"); if(!file) return false;
    const char *txt="UTF-8 中文文本\r\n第二行 plain text\n";
    ok=fwrite(txt,1,strlen(txt),file)==strlen(txt)&&ok; if(fclose(file)) ok=false;
    return ok;
}
bool occurrence(rw::Editor &editor,const char *needle,size_t from,size_t &a,size_t &b) {
    return editor.find(needle,true,from,a,b);
}
void verify(const char *path,const char *mode) {
    rw::Document document; rw::Text error,text,warning;
    rw::FileFormat format=!strcmp(mode,"html")?rw::FileFormat::Html:!strcmp(mode,"txt")?rw::FileFormat::Text:rw::FileFormat::Native;
    if(!rw::load_document(path,format,document,error,&warning)) { fprintf(stderr,"load %s: %s\n",path,error.c_str()); ++failures; return; }
    check(document.validate(error),"saved document is valid");
    if(!text_of(document,text)) { check(false,"document text allocation"); return; }
    if(!strcmp(mode,"branch")) { check(text=="B","saved edit after undo branch"); return; }
    if(!strcmp(mode,"recovery")) check(strstr(text.c_str(),"Recovery marker 2468")!=nullptr,"recovered unsaved changes");
    if(!strcmp(mode,"html-import")) {
        check(strstr(text.c_str(),"HTML 中文导入")!=nullptr,"imported Chinese heading");
        check(strstr(text.c_str(),"code\n  spaces")!=nullptr,"HTML pre whitespace");
        check(strstr(text.c_str(),"linked text")!=nullptr,"link visible text retained");
        rw::Editor editor; check(editor.replace_document(static_cast<rw::Document &&>(document)),"inspect HTML import");
        size_t a=0,b=0;
        check(occurrence(editor,"code",0,a,b),"pre code found");
        for(size_t i=a;i<b;++i) check(editor.doc.style_at(i).family==rw::FontFamily::Mono,"pre uses monospace");
        return;
    }
    if(!strcmp(mode,"txt-import")) { check(text=="UTF-8 中文文本\n第二行 plain text\n","TXT UTF-8 and CRLF normalized"); return; }
    check(strstr(text.c_str(),"ReliefWord 项目周报")!=nullptr,"original Chinese fixture content");
    check(strstr(text.c_str(),"中文输入框粘贴测试")!=nullptr,"real X11 UTF-8 clipboard paste");
    check(strstr(text.c_str(),"ReliefWordCheck: 123456789")!=nullptr,"fast key input and replace-all");
    check(strstr(text.c_str(),"WordBench")==nullptr,"all search matches replaced");
    check(strstr(text.c_str(),"Second page confirmation")!=nullptr,"text after explicit page break");
    check(strchr(text.c_str(),'\f')!=nullptr,"explicit page break survives save/export");
    const char *expected_tail="\n中文输入框粘贴测试\nReliefWordCheck: 123456789\nReliefWordCheck: 123456789\fSecond page confirmation";
    const char *tail=strstr(text.c_str(),expected_tail);
    const char *ending=!strcmp(mode,"recovery")?"\nRecovery marker 2468":"";
    check(tail&&strcmp(tail+strlen(expected_tail),ending)==0,
          "search Escape restores editor focus; Ctrl+End and page break append at the document end");
    if(format!=rw::FileFormat::Text) {
        check(document.page.width==842&&document.page.height==595,"landscape page setup");
        rw::Editor editor; check(editor.replace_document(static_cast<rw::Document &&>(document)),"open independent editor");
        size_t a=0,b=0,from=0; unsigned matches=0;
        while(occurrence(editor,"ReliefWordCheck: 123456789",from,a,b)) {
            ++matches;
            for(size_t i=a;i<b;++i) check(editor.doc.style_at(i).bold&&editor.doc.style_at(i).size==18,"copied rich text keeps bold and 18pt");
            check(editor.doc.paragraphs[editor.doc.paragraph_at(a)].style.alignment==rw::Alignment::Center,"copied centered paragraph");
            from=b;
        }
        check(matches==2,"two rich-text copies after undo/redo");
    }
}
}
int main(int argc,char **argv) {
    if(argc!=3) return 2;
    if(!strcmp(argv[1],"prepare")) return write_fixture(argv[2])?0:1;
    verify(argv[1],argv[2]);
    if(!failures) printf("ReliefWord guest readback passed: %s (%s)\n",argv[1],argv[2]);
    return failures?1:0;
}
