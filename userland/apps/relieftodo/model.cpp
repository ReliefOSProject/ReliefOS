#include "model.h"
#include <ctype.h>
#include <stdio.h>
#include <time.h>
namespace rt {
namespace {
uint64_t timestamp() { time_t t = time(nullptr); return t > 0 ? uint64_t(t) : 0; }
bool text_ok(const Text &s, size_t max, bool multiline = false) {
    if (s.size() > max || !valid_utf8(s.c_str(), s.size())) return false;
    for (size_t n = 0; n < s.size(); ++n) {
        unsigned char c = static_cast<unsigned char>(s.c_str()[n]);
        if (!c || c == 127 || (c < 32 && !(multiline && (c == '\n' || c == '\t' || c == '\r')))) return false;
    }
    return true;
}
bool not_blank(const Text &s) {
    for (size_t n = 0; n < s.size(); ++n) if (static_cast<unsigned char>(s.c_str()[n]) > 32) return true;
    return false;
}
bool contains(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    if (!length) return true;
    for (const char *p = haystack; *p; ++p) {
        size_t n = 0;
        while (n < length && p[n]) {
            unsigned char a = static_cast<unsigned char>(p[n]), b = static_cast<unsigned char>(needle[n]);
            if (a < 128) a = static_cast<unsigned char>(tolower(a));
            if (b < 128) b = static_cast<unsigned char>(tolower(b));
            if (a != b) break;
            ++n;
        }
        if (n == length) return true;
    }
    return false;
}
bool task_equal(const Task &a, const Task &b) {
    return a.id == b.id && a.title == b.title && a.notes == b.notes && a.project == b.project &&
        a.due == b.due && a.tags == b.tags && a.priority == b.priority && a.completed == b.completed &&
        a.created == b.created && a.updated == b.updated && a.order == b.order;
}
int cmp64(uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
int compare(const Task &a, const Task &b, SortMode mode) {
    int c = 0;
    switch (mode) {
    case SortMode::Priority: c = int(b.priority) - int(a.priority); break;
    case SortMode::DueDate:
        c = a.due.empty() != b.due.empty() ? (a.due.empty() ? 1 : -1) : strcmp(a.due.c_str(), b.due.c_str()); break;
    case SortMode::Updated: c = cmp64(b.updated, a.updated); break;
    case SortMode::Title: c = strcmp(a.title.c_str(), b.title.c_str()); break;
    case SortMode::Manual: break;
    }
    if (!c) c = cmp64(a.order, b.order);
    return c ? c : cmp64(a.id, b.id);
}
template<SortMode Mode> int pointer_compare(const void *a, const void *b) {
    return compare(**static_cast<const Task *const *>(a), **static_cast<const Task *const *>(b), Mode);
}
}
bool valid_utf8(const char *text, size_t length) {
    if (!text && length) return false;
    const auto *s = reinterpret_cast<const unsigned char *>(text);
    size_t n = 0;
    while (n < length) {
        unsigned a = s[n++];
        if (!a) return false;
        if (a < 128) continue;
        unsigned count = 0, lo = 128, hi = 191;
        if (a >= 194 && a <= 223) count = 1;
        else if (a >= 224 && a <= 239) { count = 2; if (a == 224) lo = 160; if (a == 237) hi = 159; }
        else if (a >= 240 && a <= 244) { count = 3; if (a == 240) lo = 144; if (a == 244) hi = 143; }
        else return false;
        if (length - n < count || s[n] < lo || s[n] > hi) return false;
        ++n;
        while (--count) if (s[n] < 128 || s[n++] > 191) return false;
    }
    return true;
}
bool valid_date(const char *date) {
    if (!date) return false;
    if (!*date) return true;
    if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') return false;
    for (int n = 0; n < 10; ++n) if (n != 4 && n != 7 && (date[n] < '0' || date[n] > '9')) return false;
    int y = (date[0]-'0')*1000+(date[1]-'0')*100+(date[2]-'0')*10+date[3]-'0';
    int m = (date[5]-'0')*10+date[6]-'0', d = (date[8]-'0')*10+date[9]-'0';
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return y >= 1900 && m >= 1 && m <= 12 && d >= 1 &&
        d <= days[m-1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
bool local_date(char (&out)[11], int days) {
    time_t t = time(nullptr);
    struct tm local{};
    if (!localtime_r(&t, &local)) return false;
    local.tm_mday += days; local.tm_hour = 12; local.tm_isdst = -1;
    if (mktime(&local) == time_t(-1)) return false;
    return strftime(out, sizeof(out), "%Y-%m-%d", &local) == 10 && valid_date(out);
}
bool valid_task(const Task &t) {
    return t.id && text_ok(t.title, MaxTitle) && not_blank(t.title) && text_ok(t.notes, MaxNotes, true) &&
        text_ok(t.project, MaxProject) && text_ok(t.tags, MaxTags) && text_ok(t.due, 10) &&
        valid_date(t.due.c_str()) && unsigned(t.priority) <= unsigned(Priority::Urgent);
}
bool validate_document(const Document &doc, Text &error) {
    if (!text_ok(doc.title, 256) || !doc.next_id || doc.tasks.size() > MaxTasks) {
        error.set("文档信息无效或任务数量超出限制"); return false;
    }
    size_t bytes = doc.title.size();
    Array<TaskId> ids;
    if (!ids.reserve(doc.tasks.size())) { error.set("内存不足"); return false; }
    for (size_t n = 0; n < doc.tasks.size(); ++n) {
        const Task &t = doc.tasks[n];
        if (!valid_task(t) || t.id >= doc.next_id) { error.set("任务内容、日期或任务编号无效"); return false; }
        bytes += 80 + t.title.size() + t.notes.size() + t.project.size() + t.due.size() + t.tags.size();
        if (bytes > MaxFileBytes) { error.set("文档超出 16 MiB 限制"); return false; }
        ids.push(t.id);
    }
    qsort(ids.data(), ids.size(), sizeof(TaskId), [](const void *a, const void *b) { return cmp64(*static_cast<const TaskId *>(a), *static_cast<const TaskId *>(b)); });
    for (size_t n = 1; n < ids.size(); ++n) if (ids[n] == ids[n-1]) { error.set("文档中存在重复的任务编号"); return false; }
    error.clear(); return true;
}
bool Document::clone_from(const Document &other) {
    Array<Task> next;
    if (!next.clone_from(other.tasks)) return false;
    title = other.title; tasks.swap(next); next_id = other.next_id; revision = other.revision; return true;
}
Task *find_task(Document &doc, TaskId id) {
    for (size_t n = 0; n < doc.tasks.size(); ++n) if (doc.tasks[n].id == id) return &doc.tasks[n];
    return nullptr;
}
const Task *find_task(const Document &doc, TaskId id) {
    for (size_t n = 0; n < doc.tasks.size(); ++n) if (doc.tasks[n].id == id) return &doc.tasks[n];
    return nullptr;
}
bool add_task(Document &doc, const char *title, const char *notes, const char *project,
              const char *due, Priority priority, const char *tags, TaskId &out) {
    if (doc.tasks.size() >= MaxTasks || !doc.next_id || doc.next_id == UINT64_MAX) return false;
    Task t; t.id = doc.next_id; t.priority = priority; t.created = t.updated = timestamp();
    if (!t.title.set(title) || !t.notes.set(notes) || !t.project.set(project) || !t.due.set(due) ||
        !t.tags.set(tags) || !valid_task(t)) return false;
    for (size_t n = 0; n < doc.tasks.size(); ++n) if (doc.tasks[n].order >= t.order) {
        if (doc.tasks[n].order == UINT64_MAX) return false;
        t.order = doc.tasks[n].order + 1;
    }
    if (!doc.tasks.push(t)) return false;
    out = t.id; ++doc.next_id; ++doc.revision; return true;
}
bool update_task(Document &doc, TaskId id, const char *title, const char *notes, const char *project,
                 const char *due, Priority priority, const char *tags, bool completed) {
    Task *old = find_task(doc, id); if (!old) return false;
    Task t = *old; t.priority = priority; t.completed = completed;
    if (!t.title.set(title) || !t.notes.set(notes) || !t.project.set(project) || !t.due.set(due) ||
        !t.tags.set(tags) || !valid_task(t)) return false;
    if (task_equal(t, *old)) return true;
    t.updated = timestamp(); *old = static_cast<Task &&>(t); ++doc.revision; return true;
}
bool set_task_done(Document &doc, TaskId id, bool done) {
    Task *t = find_task(doc, id); if (!t) return false;
    if (t->completed != done) { t->completed = done; t->updated = timestamp(); ++doc.revision; }
    return true;
}
bool rename_task(Document &doc, TaskId id, const char *title) {
    Task *t = find_task(doc, id); if (!t) return false;
    return update_task(doc,id,title,t->notes.c_str(),t->project.c_str(),t->due.c_str(),t->priority,t->tags.c_str(),t->completed);
}
bool update_task_notes(Document &doc, TaskId id, const char *notes) {
    Task *t = find_task(doc, id); if (!t) return false;
    return update_task(doc,id,t->title.c_str(),notes,t->project.c_str(),t->due.c_str(),t->priority,t->tags.c_str(),t->completed);
}
bool copy_task(Document &doc, TaskId id, TaskId &out) {
    const Task *t = find_task(doc, id); if (!t) return false;
    Task copy = *t; // add_task may reallocate the task array.
    return add_task(doc,copy.title.c_str(),copy.notes.c_str(),copy.project.c_str(),copy.due.c_str(),copy.priority,copy.tags.c_str(),out);
}
bool delete_task(Document &doc, TaskId id) {
    for (size_t n = 0; n < doc.tasks.size(); ++n) if (doc.tasks[n].id == id) {
        doc.tasks.erase(n); ++doc.revision; return true;
    }
    return false;
}
bool move_task(Document &doc, TaskId id, int direction) {
    if (!sort_tasks(doc, SortMode::Manual)) return false;
    for (size_t n = 0; n < doc.tasks.size(); ++n) if (doc.tasks[n].id == id) {
        if ((direction < 0 && !n) || (direction >= 0 && n + 1 == doc.tasks.size())) return true;
        size_t to = direction < 0 ? n-1 : n+1;
        uint64_t order = doc.tasks[n].order; doc.tasks[n].order = doc.tasks[to].order; doc.tasks[to].order = order;
        Task t = doc.tasks[n]; doc.tasks[n] = doc.tasks[to]; doc.tasks[to] = t; ++doc.revision; return true;
    }
    return false;
}
bool task_matches(const Task &t, Filter filter, const char *query, const char *today, const char *project) {
    if (project && !(t.project == project)) return false;
    if (query && *query && !contains(t.title.c_str(),query) && !contains(t.notes.c_str(),query) &&
        !contains(t.project.c_str(),query) && !contains(t.tags.c_str(),query)) return false;
    switch (filter) {
    case Filter::All: return true;
    case Filter::Completed: return t.completed;
    case Filter::Active: return !t.completed;
    case Filter::Today: return !t.completed && today && t.due == today;
    case Filter::Overdue: return !t.completed && !t.due.empty() && today && strcmp(t.due.c_str(),today) < 0;
    case Filter::Upcoming: return !t.completed && !t.due.empty() && today && strcmp(t.due.c_str(),today) > 0;
    }
    return false;
}
bool sort_tasks(Document &doc, SortMode mode) {
    Array<const Task *> pointers;
    if (!pointers.reserve(doc.tasks.size())) return false;
    for (size_t n = 0; n < doc.tasks.size(); ++n) pointers.push(&doc.tasks[n]);
    int (*cmp)(const void *, const void *) = nullptr;
    switch (mode) {
    case SortMode::Manual: cmp = pointer_compare<SortMode::Manual>; break;
    case SortMode::Priority: cmp = pointer_compare<SortMode::Priority>; break;
    case SortMode::DueDate: cmp = pointer_compare<SortMode::DueDate>; break;
    case SortMode::Updated: cmp = pointer_compare<SortMode::Updated>; break;
    case SortMode::Title: cmp = pointer_compare<SortMode::Title>; break;
    default: return false;
    }
    qsort(pointers.data(), pointers.size(), sizeof(const Task *), cmp);
    bool changed = false;
    for (size_t n = 0; n < pointers.size(); ++n) if (pointers[n]->id != doc.tasks[n].id) { changed = true; break; }
    if (!changed) return true;
    Array<Task> sorted;
    if (!sorted.reserve(pointers.size())) return false;
    for (size_t n = 0; n < pointers.size(); ++n) sorted.push(*pointers[n]);
    doc.tasks.swap(sorted); ++doc.revision; return true;
}
bool seed_example(Document &out) {
    Document doc; TaskId id = 0;
    char today[11]{}, tomorrow[11]{}, next[11]{}, yesterday[11]{};
    if (!local_date(today) || !local_date(tomorrow,1) || !local_date(next,3) || !local_date(yesterday,-1) ||
        !doc.title.set("我的任务清单")) return false;
    if (!add_task(doc,"规划今天的三件重要任务","先确定优先级，再把工作拆成可以完成的小步骤。\n\n提示：在右侧编辑详情，点击“应用修改”；选择多行可批量完成。","个人",today,Priority::High,"计划,今日",id) ||
        !add_task(doc,"整理 ReliefOS 项目文档","梳理待办事项，补充操作说明和验收记录。","ReliefOS",today,Priority::High,"文档,开发",id) ||
        !add_task(doc,"复查上一轮待办事项","检查未完成的工作，调整截止日期。","ReliefOS",yesterday,Priority::Urgent,"复查",id) ||
        !add_task(doc,"准备每周例会提纲","整理本周进展、问题与下周计划。","工作",tomorrow,Priority::Normal,"会议",id) ||
        !add_task(doc,"阅读一本喜欢的书","每天给阅读留出一点时间。","个人",next,Priority::Low,"阅读",id) ||
        !add_task(doc,"体验 ReliefTodo 的任务工具","试试搜索、日期筛选、项目和标签。","个人","",Priority::Normal,"入门",id) ||
        !set_task_done(doc,id,true)) return false;
    out = static_cast<Document &&>(doc); return true;
}
bool same_document_content(const Document &a, const Document &b) {
    if (!(a.title == b.title) || a.next_id != b.next_id || a.tasks.size() != b.tasks.size()) return false;
    for (size_t n = 0; n < a.tasks.size(); ++n) if (!task_equal(a.tasks[n],b.tasks[n])) return false;
    return true;
}
bool History::checkpoint(const Document &doc) {
    Document copy; if (!copy.clone_from(doc)) return false;
    if (!past_.push(static_cast<Document &&>(copy))) return false;
    trim();
    return true;
}
void History::abandon_checkpoint() { if (!past_.empty()) past_.erase(past_.size()-1); }
void History::rollback(Document &doc) {
    if (past_.empty()) return;
    doc=static_cast<Document &&>(past_[past_.size()-1]); past_.erase(past_.size()-1);
}
void History::trim() {
    size_t bytes=0;
    for (size_t n=0;n<past_.size();++n) {
        const Document &d=past_[n]; bytes+=sizeof(Document)+d.tasks.size()*sizeof(Task)+d.title.size();
        for (size_t k=0;k<d.tasks.size();++k) {
            const Task &t=d.tasks[k]; bytes+=t.title.size()+t.notes.size()+t.project.size()+t.due.size()+t.tags.size();
        }
    }
    while (past_.size()>1 && (past_.size()>40 || bytes>64U*1024U*1024U)) {
        const Document &d=past_[0]; size_t used=sizeof(Document)+d.tasks.size()*sizeof(Task)+d.title.size();
        for (size_t k=0;k<d.tasks.size();++k) {
            const Task &t=d.tasks[k]; used+=t.title.size()+t.notes.size()+t.project.size()+t.due.size()+t.tags.size();
        }
        bytes-=used; past_.erase(0);
    }
}
bool History::undo(Document &doc) {
    if (past_.empty()) return false;
    Document copy; if (!copy.clone_from(doc) || !future_.push(static_cast<Document &&>(copy))) return false;
    doc = static_cast<Document &&>(past_[past_.size()-1]); past_.erase(past_.size()-1); return true;
}
bool History::redo(Document &doc) {
    if (future_.empty()) return false;
    Document copy; if (!copy.clone_from(doc) || !past_.push(static_cast<Document &&>(copy))) return false;
    doc = static_cast<Document &&>(future_[future_.size()-1]); future_.erase(future_.size()-1); trim(); return true;
}
}
