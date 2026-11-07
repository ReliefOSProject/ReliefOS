/* ReliefMail owns a POSIX-VFS build of the same generated SQLite amalgamation. */
#define SQLITE_THREADSAFE 0
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_WAL 1
#define SQLITE_MAX_MMAP_SIZE 0
#define SQLITE_DEFAULT_MEMSTATUS 0

/* Keep generated-amalgamation diagnostics scoped to its include boundary. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#endif

#include "sqlite3.c"

/* This wrapper translation unit contains only the generated amalgamation; keep
 * these narrow suppressions active through compiler end-of-unit diagnostics. */
