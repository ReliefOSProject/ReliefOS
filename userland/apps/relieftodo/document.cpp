#include "document.h"
#include <stdio.h>
namespace rt {
namespace {
uint32_t crc32(const char *data, size_t size) {
    uint32_t crc = ~uint32_t(0);
    for (size_t n = 0; n < size; ++n) {
        crc ^= static_cast<unsigned char>(data[n]);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
struct Writer {
    Bytes bytes;
    bool raw(const char *s, size_t n) {
        size_t at = bytes.size();
        if (n > MaxFileBytes || at > MaxFileBytes - n || !bytes.resize(at + n)) return false;
        if (n) memcpy(bytes.data()+at, s, n);
        return true;
    }
    bool text(const char *s) { return raw(s, strlen(s)); }
    bool number(uint64_t value, size_t count) {
        char b[8];
        for (size_t n=0; n<count; ++n) b[n] = char(value >> (8*n));
        return raw(b,count);
    }
    bool field(const Text &s) { return number(s.size(),4) && raw(s.c_str(),s.size()); }
};
struct Reader {
    const char *bytes; size_t size, at = 0;
    bool number(uint64_t &value, size_t count) {
        if (count > size - at) return false;
        value = 0;
        for (size_t n=0; n<count; ++n) value |= uint64_t(static_cast<unsigned char>(bytes[at++])) << (8*n);
        return true;
    }
    bool field(Text &s, size_t limit) {
        uint64_t n;
        if (!number(n,4) || n > limit || n > size-at || !s.set(bytes+at,size_t(n))) return false;
        at += size_t(n); return true;
    }
};
bool csv_field(Writer &w, const char *s) {
    if (!w.text("\"")) return false;
    const char *p=s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if ((*p=='=' || *p=='+' || *p=='-' || *p=='@') && !w.text("'")) return false;
    for (; *s; ++s) if ((*s=='"' && !w.text("\"")) || !w.raw(s,1)) return false;
    return w.text("\"");
}
bool md_text(Writer &w, const char *s) {
    for (; *s; ++s) {
        if (strchr("\\`*_{}[]<>()#+-.!|",*s) && !w.text("\\")) return false;
        if (!w.raw(s,1)) return false;
    }
    return true;
}
const char *priority_name(Priority p) {
    const char *names[]={"低","普通","高","紧急"}; return names[unsigned(p)];
}
}
bool encode_document(const Document &doc, Bytes &out, Text &error) {
    if (!validate_document(doc,error)) return false;
    Writer w;
    bool ok = w.text("RTDO") && w.number(1,4) && w.number(0,4) && w.number(0,4) &&
        w.number(doc.next_id,8) && w.number(doc.tasks.size(),4) && w.field(doc.title);
    for (size_t n=0; ok && n<doc.tasks.size(); ++n) {
        const Task &t=doc.tasks[n];
        ok = w.number(t.id,8) && w.number(t.created,8) && w.number(t.updated,8) && w.number(t.order,8) &&
            w.number(unsigned(t.priority),1) && w.number(t.completed?1:0,1) && w.number(0,2) &&
            w.field(t.title) && w.field(t.notes) && w.field(t.project) && w.field(t.due) && w.field(t.tags);
    }
    if (!ok) { error.set("文档超出大小限制或内存不足"); return false; }
    uint32_t size=uint32_t(w.bytes.size()), crc=crc32(w.bytes.data()+16,size-16);
    for (unsigned n=0; n<4; ++n) { w.bytes[8+n]=char(size>>(8*n)); w.bytes[12+n]=char(crc>>(8*n)); }
    out.swap(w.bytes); error.clear(); return true;
}
bool decode_document(const char *bytes, size_t size, Document &out, Text &error) {
    if (!bytes || size < 32 || size > MaxFileBytes || memcmp(bytes,"RTDO",4)) {
        error.set("不是有效的 ReliefTodo 文档，或文档已截断"); return false;
    }
    Reader r{bytes,size,4}; uint64_t version, declared, crc, count;
    Document d;
    bool ok = r.number(version,4) && version==1 && r.number(declared,4) && declared==size &&
        r.number(crc,4) && crc==crc32(bytes+16,size-16) && r.number(d.next_id,8) &&
        r.number(count,4) && count<=MaxTasks && r.field(d.title,256) && d.tasks.reserve(size_t(count));
    for (size_t n=0; ok && n<size_t(count); ++n) {
        Task t; uint64_t priority, done, reserved;
        ok = r.number(t.id,8) && r.number(t.created,8) && r.number(t.updated,8) && r.number(t.order,8) &&
            r.number(priority,1) && priority<=3 && r.number(done,1) && done<=1 && r.number(reserved,2) && !reserved &&
            r.field(t.title,MaxTitle) && r.field(t.notes,MaxNotes) && r.field(t.project,MaxProject) &&
            r.field(t.due,10) && r.field(t.tags,MaxTags);
        if (ok) { t.priority=Priority(priority); t.completed=done!=0; ok=d.tasks.push(static_cast<Task &&>(t)); }
    }
    if (!ok || r.at != size) { error.set("文档版本不支持、内容损坏或超出限制"); return false; }
    if (!validate_document(d,error)) return false;
    out=static_cast<Document &&>(d); error.clear(); return true;
}
bool export_csv(const Document &doc, Bytes &out, Text &error) {
    if (!validate_document(doc,error)) return false;
    Writer w; bool ok=w.text("\xef\xbb\xbf标题,状态,优先级,截止日期,项目,标签,备注\r\n");
    for (size_t n=0; ok && n<doc.tasks.size(); ++n) {
        const Task &t=doc.tasks[n];
        const char *cells[]={t.title.c_str(),t.completed?"已完成":"待办",priority_name(t.priority),t.due.c_str(),t.project.c_str(),t.tags.c_str(),t.notes.c_str()};
        for (unsigned c=0; ok && c<7; ++c) ok=csv_field(w,cells[c]) && w.text(c==6?"\r\n":",");
    }
    if (!ok) { error.set("导出内容超出限制或内存不足"); return false; }
    out.swap(w.bytes); error.clear(); return true;
}
bool export_markdown(const Document &doc, Bytes &out, Text &error) {
    if (!validate_document(doc,error)) return false;
    Writer w; bool ok=w.text("# ") && md_text(w,doc.title.empty()?"任务清单":doc.title.c_str()) && w.text("\n\n");
    for (size_t n=0; ok && n<doc.tasks.size(); ++n) {
        const Task &t=doc.tasks[n];
        ok=w.text(t.completed?"- [x] ":"- [ ] ") && md_text(w,t.title.c_str()) && w.text("\n");
        const char *names[]={"优先级：","截止日期：","项目：","标签："};
        const char *values[]={priority_name(t.priority),t.due.c_str(),t.project.c_str(),t.tags.c_str()};
        for (unsigned c=0; ok && c<4; ++c) if (*values[c]) ok=w.text("  - ") && w.text(names[c]) && md_text(w,values[c]) && w.text("\n");
        if (ok && !t.notes.empty()) {
            ok=w.text("\n    ");
            for (const char *p=t.notes.c_str(); ok && *p; ++p) ok=w.raw(p,1) && (*p!='\n' || w.text("    "));
            ok=ok && w.text("\n");
        }
        ok=ok && w.text("\n");
    }
    if (!ok) { error.set("导出内容超出限制或内存不足"); return false; }
    out.swap(w.bytes); error.clear(); return true;
}
}
