#ifndef RELIEFOS_FLTK_FILES_H
#define RELIEFOS_FLTK_FILES_H
#include "fltk_support.h"
#include <FL/Fl_File_Chooser.H>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace reliefos_fltk {
inline bool choose_file(const char *title, const char *initial, char *path, unsigned capacity, bool save = false)
{
    Fl_File_Chooser chooser(initial, "*.{bmp,dib,png,BMP,DIB,PNG}",
                            save ? Fl_File_Chooser::CREATE : Fl_File_Chooser::SINGLE, title);
    chooser.preview(0);
    chooser.show();
    while (chooser.shown()) Fl::wait();
    const char *selected = chooser.value();
    if (!selected || !selected[0]) return false;
    if (!copy_input(path, capacity, selected)) { fl_alert("%s", T("Input is too long")); return false; }
    return true;
}
inline bool suffix(const char *text, const char *ending)
{
    size_t n = strlen(text), m = strlen(ending);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
        unsigned char ch = text[n-m+i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a'-'A';
        if (ch != static_cast<unsigned char>(ending[i])) return false;
    }
    return true;
}
inline int read_file(const char *path, unsigned maximum, unsigned char **data, unsigned *length)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > maximum) {
        close(fd); return -1;
    }
    unsigned char *buffer = static_cast<unsigned char *>(malloc(st.st_size));
    if (!buffer) { close(fd); return -1; }
    unsigned done = 0, size = st.st_size;
    while (done < size) {
        ssize_t got = read(fd, buffer + done, size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { free(buffer); close(fd); return -1; }
        done += got;
    }
    close(fd); *data = buffer; *length = size; return 0;
}
inline int write_file(const char *path, const unsigned char *data, unsigned length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    unsigned done = 0;
    while (done < length) {
        ssize_t count = write(fd, data + done, length - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return -1; }
        done += count;
    }
    return close(fd);
}
}
#endif
