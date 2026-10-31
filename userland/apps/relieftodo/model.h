#ifndef RELIEFTODO_MODEL_H
#define RELIEFTODO_MODEL_H
#include "storage.h"
namespace rt {
using TaskId = uint64_t;
constexpr size_t MaxTasks = 10000, MaxFileBytes = 16U * 1024U * 1024U;
constexpr size_t MaxTitle = 512, MaxNotes = 65536, MaxProject = 128, MaxTags = 512;
enum class Priority : uint8_t { Low, Normal, High, Urgent };
enum class Filter : uint8_t { All, Today, Upcoming, Completed, Overdue, Active };
enum class SortMode : uint8_t { Manual, Priority, DueDate, Updated, Title };
struct Task {
    TaskId id = 0;
    Text title, notes, project, due, tags;
    Priority priority = Priority::Normal;
    bool completed = false;
    uint64_t created = 0, updated = 0, order = 0;
};
struct Document {
    Text title;
    Array<Task> tasks;
    TaskId next_id = 1;
    uint64_t revision = 1;
    bool clone_from(const Document &);
};
bool valid_utf8(const char *, size_t);
bool valid_date(const char *); // Empty, or a Gregorian date in YYYY-MM-DD (1900–9999).
bool local_date(char (&out)[11], int days = 0);
bool valid_task(const Task &);
bool validate_document(const Document &, Text &error);
Task *find_task(Document &, TaskId);
const Task *find_task(const Document &, TaskId);
bool add_task(Document &, const char *title, const char *notes, const char *project,
              const char *due, Priority, const char *tags, TaskId &);
bool update_task(Document &, TaskId, const char *title, const char *notes, const char *project,
                 const char *due, Priority, const char *tags, bool completed);
bool set_task_done(Document &, TaskId, bool);
bool rename_task(Document &, TaskId, const char *);
bool update_task_notes(Document &, TaskId, const char *);
bool copy_task(Document &, TaskId, TaskId &);
bool delete_task(Document &, TaskId);
bool move_task(Document &, TaskId, int direction);
bool task_matches(const Task &, Filter, const char *query, const char *today,
                  const char *project = nullptr);
bool sort_tasks(Document &, SortMode);
bool seed_example(Document &);
bool same_document_content(const Document &, const Document &);
class History {
    Array<Document> past_, future_;
    void trim();
public:
    bool checkpoint(const Document &);
    void abandon_checkpoint();
    void rollback(Document &);
    void committed() { future_.clear(); }
    bool undo(Document &);
    bool redo(Document &);
    bool can_undo() const { return !past_.empty(); }
    bool can_redo() const { return !future_.empty(); }
    void clear() { past_.clear(); future_.clear(); }
};
}
#endif
