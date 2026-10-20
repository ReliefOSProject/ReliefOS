#include "../../userland/apps/relieftodo/document.h"
#include "../../userland/apps/relieftodo/model.h"
#include "../../userland/apps/relieftodo/file_io.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

using namespace rt;

static void test_model_and_filters() {
    Document document;
    assert(seed_example(document));
    assert(document.tasks.size() >= 4);

    TaskId id = 0;
    assert(add_task(document, "整理 5.1.0 发布说明", "核对主仓库和内核的变更内容。",
                     "ReliefOS", "2026-10-20", Priority::High, "发布,文档", id));
    assert(id != 0);
    assert(find_task(document, id) != nullptr);
    assert(task_matches(document.tasks[0], Filter::All, "", "2026-10-20"));

    assert(set_task_done(document, id, true));
    const Task *done = find_task(document, id);
    assert(done && done->completed);
    assert(task_matches(*done, Filter::Completed, "", "2026-10-20"));
    assert(!task_matches(*done, Filter::Today, "", "2026-10-20"));

    assert(rename_task(document, id, "整理最终发布说明"));
    assert(update_task_notes(document, id, "已完成版本号和提交范围核对。"));
    assert(copy_task(document, id, id));
    assert(document.tasks.size() >= 5);
    assert(delete_task(document, id));

    assert(sort_tasks(document, SortMode::Priority));
    assert(sort_tasks(document, SortMode::DueDate));
    assert(sort_tasks(document, SortMode::Updated));
}

static void test_document_round_trip() {
    Document source;
    assert(seed_example(source));
    TaskId id = 0;
    assert(add_task(source, "中文任务", "支持 UTF-8、换行和标签。", "产品", "2026-11-02",
                    Priority::Urgent, "中文,验收", id));
    assert(set_task_done(source, id, true));
    source.title.set("ReliefTodo 验收清单");

    Bytes encoded;
    Text error;
    assert(encode_document(source, encoded, error));
    assert(encoded.size() > 16);

    Document decoded;
    assert(decode_document(encoded.data(), encoded.size(), decoded, error));
    assert(same_document_content(source, decoded));
    assert(decoded.title == "ReliefTodo 验收清单");
    assert(find_task(decoded, id) != nullptr);

    const char broken[] = "RTDO\x01\x00\x00";
    Document unchanged;
    assert(seed_example(unchanged));
    const size_t before = unchanged.tasks.size();
    assert(!decode_document(broken, sizeof(broken) - 1, unchanged, error));
    assert(unchanged.tasks.size() == before);
}

static void test_validation_and_history() {
    assert(valid_date("2028-02-29") && valid_date("") && valid_date("2026-10-20"));
    assert(!valid_date("2026-02-29") && !valid_date("2026-13-01") && !valid_date("2026-1-01"));
    assert(!valid_utf8("\xc0\xaf",2) && !valid_utf8("\xed\xa0\x80",3));
    Document d, before;
    assert(seed_example(d) && before.clone_from(d));
    TaskId id=d.tasks[0].id;
    History h;
    assert(h.checkpoint(d));
    assert(rename_task(d,id,"新标题")); h.committed();
    assert(h.undo(d) && same_document_content(d,before));
    assert(h.redo(d) && find_task(d,id)->title=="新标题");
    assert(h.undo(d) && h.checkpoint(d));
    assert(set_task_done(d,id,true)); h.committed();
    assert(!h.can_redo() && h.undo(d) && same_document_content(d,before));
    assert(h.checkpoint(d) && delete_task(d,id)); h.rollback(d);
    assert(same_document_content(d,before));
    assert(!update_task(d,id,"","","","2026-10-20",Priority::Normal,"",false));
    assert(same_document_content(d,before));
    Bytes bytes; Text error; assert(encode_document(d,bytes,error));
    bytes[bytes.size()-1]^=1;
    assert(!decode_document(bytes.data(),bytes.size(),d,error) && same_document_content(d,before));
    assert(encode_document(before,bytes,error)); bytes[4]=2;
    assert(!decode_document(bytes.data(),bytes.size(),d,error));
    assert(add_task(d,"=SUM(1,2)","line 1\nline \"2\"","project","",Priority::Low,"test",id));
    assert(export_csv(d,bytes,error));
    assert(bytes.resize(bytes.size()+1)); bytes[bytes.size()-1]=0;
    assert(strstr(bytes.data(),"\"'=SUM(1,2)\"") && strstr(bytes.data(),"line \"\"2\"\""));
    assert(export_markdown(d,bytes,error));
    assert(bytes.resize(bytes.size()+1)); bytes[bytes.size()-1]=0;
    assert(strstr(bytes.data(),"- [ ]") && strstr(bytes.data(),"- [x]"));
}

static IoJob::Reply await_job(IoJob &job) {
    for (int n=0;n<10000;++n) {
        if (job.poll()) return job.take();
        usleep(1000);
    }
    assert(!"worker did not finish"); return {};
}

static void test_file_transactions() {
    char directory[4096];
    snprintf(directory,sizeof(directory),"%s/relieftodo-verify-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp");
    assert(mkdtemp(directory));
    char path[4200], link[4200], parent_link[4200], through_link[4300];
    snprintf(path,sizeof(path),"%s/tasks.rtodo",directory);
    snprintf(link,sizeof(link),"%s/link.rtodo",directory);
    snprintf(parent_link,sizeof(parent_link),"%s/link-dir",directory);
    snprintf(through_link,sizeof(through_link),"%s/tasks.rtodo",parent_link);
    Document d; assert(seed_example(d));
    Bytes bytes, loaded; Text error; FileStamp absent, saved, stamp;
    assert(encode_document(d,bytes,error) && file_stamp(path,absent,error) && !absent.exists);
    IoJob job;
    assert(job.write(path,bytes,absent,error));
    auto reply=await_job(job); assert(reply.ok); saved=reply.stamp;
    assert(job.read(path,error)); reply=await_job(job); assert(reply.ok);
    assert(reply.bytes.size()==bytes.size() && !memcmp(reply.bytes.data(),bytes.data(),bytes.size()));
    assert(same_stamp(saved,reply.stamp));
    char equivalent[4300]; snprintf(equivalent,sizeof(equivalent),"%s/./tasks.rtodo",directory);
    assert(job.probe(equivalent,error,path)); reply=await_job(job); assert(!reply.ok);
    assert(job.write(equivalent,bytes,saved,error,path)); reply=await_job(job); assert(!reply.ok);
    assert(read_file(path,loaded,stamp,error) && same_stamp(saved,stamp));
    // Legacy filesystems may report path-derived inode numbers for aliases.
    // An identical native document must still never be an export destination.
    char duplicate[4300]; snprintf(duplicate,sizeof(duplicate),"%s/copy.rtodo",directory);
    assert(atomic_save(duplicate,bytes,FileStamp{},stamp,error));
    assert(job.probe(duplicate,error,path)); reply=await_job(job); assert(!reply.ok);
    assert(job.write(duplicate,bytes,stamp,error,path)); reply=await_job(job); assert(!reply.ok);
    assert(unlink(duplicate)==0);
    assert(atomic_save(path,bytes,saved,stamp,error));
    assert(!atomic_save(path,bytes,saved,absent,error));
    assert(read_file(path,loaded,saved,error));
    assert(!atomic_save(path,bytes,FileStamp{},absent,error));

    // A second writer must not enter the compare/rename transaction while
    // another ReliefTodo instance holds the directory's stable save lock.
    char lockpath[4200]; snprintf(lockpath,sizeof(lockpath),"%s/.relieftodo-save.lock",directory);
    int lockfd=open(lockpath,O_RDWR|O_CREAT|O_CLOEXEC,0600);
    assert(lockfd>=0 && flock(lockfd,LOCK_EX|LOCK_NB)==0);
    assert(!atomic_save(path,bytes,saved,stamp,error));
    assert(read_file(path,loaded,stamp,error) && same_stamp(saved,stamp));
    assert(flock(lockfd,LOCK_UN)==0 && close(lockfd)==0);
    assert(atomic_save(path,bytes,saved,stamp,error));
    assert(file_stamp(lockpath,saved,error));
    assert(!atomic_save(lockpath,bytes,saved,stamp,error));
    // A second descriptor for the save lock's FAT short-name alias would
    // conflict just like an independently locked existing destination.
    assert(read_file(path,loaded,saved,error));
    int targetfd=open(path,O_RDONLY|O_CLOEXEC);
    assert(targetfd>=0 && flock(targetfd,LOCK_EX|LOCK_NB)==0);
    assert(!atomic_save(path,bytes,saved,stamp,error));
    assert(flock(targetfd,LOCK_UN)==0 && close(targetfd)==0);
    assert(symlink(path,link)==0 && !read_file(link,loaded,absent,error));
    assert(!atomic_save(link,bytes,FileStamp{},absent,error));
    assert(symlink(directory,parent_link)==0 && !read_file(through_link,loaded,absent,error));
    assert(!read_file("../tasks.rtodo",loaded,absent,error));
    assert(unlink(link)==0 && unlink(parent_link)==0 && unlink(path)==0 && unlink(lockpath)==0 && rmdir(directory)==0);
}

static int verify_guest(const char *directory) {
    char path[4200]; snprintf(path,sizeof(path),"%s/saved.rtodo",directory);
    Bytes bytes; FileStamp stamp; Text error; Document d;
    assert(read_file(path,bytes,stamp,error) && decode_document(bytes.data(),bytes.size(),d,error));
    assert(d.tasks.size()==7);
    const Task *marker=nullptr;
    for (size_t n=0;n<d.tasks.size();++n) if (d.tasks[n].title=="TodoAcceptance") marker=&d.tasks[n];
    assert(marker && marker->notes=="Persisted by real GUI" && marker->project=="ReliefOS");
    assert(marker->tags=="qemu,verified" && marker->due=="2030-01-02" && marker->priority==Priority::Urgent && !marker->completed);
    puts("ReliefTodo persisted guest document verified"); return 0;
}

int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--verify-guest")) return verify_guest(argv[2]);
    test_model_and_filters();
    test_document_round_trip();
    test_validation_and_history();
    test_file_transactions();
    puts("ReliefTodo model, document, history, export and file transaction tests passed");
    return 0;
}
