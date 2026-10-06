#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/tty0", O_RDWR);
    struct vt_stat state;
    struct vt_mode old_mode;
    struct vt_mode process_mode = { .mode = VT_PROCESS, .relsig = 10, .acqsig = 12 };
    int free_vt;
    int keyboard_mode;
    int graphics = KD_GRAPHICS;
    int text = KD_TEXT;

    if (fd < 0) return 1;
    if (ioctl(fd, VT_GETSTATE, &state) < 0) return 2;
    if (ioctl(fd, VT_OPENQRY, &free_vt) < 0 || free_vt < 1 || free_vt > 6) return 3;
    if (ioctl(fd, VT_GETMODE, &old_mode) < 0 || old_mode.mode != VT_AUTO) return 4;
    if (ioctl(fd, VT_SETMODE, &process_mode) < 0) return 5;
    if (ioctl(fd, VT_RELDISP, 1) < 0) return 6;
    if (ioctl(fd, VT_SETMODE, &old_mode) < 0) return 7;
    if (ioctl(fd, KDGKBMODE, &keyboard_mode) < 0) return 8;
    if (ioctl(fd, KDSKBMODE, keyboard_mode) < 0) return 9;
    if (ioctl(fd, KDSETMODE, graphics) < 0) return 10;
    if (ioctl(fd, KDGETMODE, &graphics) < 0 || graphics != KD_GRAPHICS) return 11;
    if (ioctl(fd, KDSETMODE, text) < 0) return 12;
    close(fd);
    return 0;
}
