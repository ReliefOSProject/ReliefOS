#ifndef RELIEFOS_LAYOUT_H
#define RELIEFOS_LAYOUT_H

#include <reliefos/rootfs.h>

/*
 * ReliefOS guest root filesystem layout contract.
 *
 * The installed root follows the Alpine Linux FHS shape: /bin, /sbin, /lib,
 * /usr/bin, /usr/sbin and /usr/lib are real directories.  There is no
 * usr-merge and no /lib64. Standard runtime links include:
 *
 *     /var/run -> ../run
 *     /var/lock -> ../run/lock
 *
 * The musl ELF PT_INTERP contract is unchanged: executables request
 * "/lib/ld-musl-x86_64.so.1", and the real interpreter is stored at that
 * path.  Do not point ELF PT_INTERP at a glibc loader.
 *
 * ReliefOS-owned files live below explicitly named subdirectories:
 *
 *     /etc/reliefos                 persistent configuration
 *     /var/lib/reliefos             persistent mutable state
 *     /var/cache/reliefos           cache data
 *     /run/reliefos                 volatile per-boot IPC and session state
 *     /usr/lib/reliefos             private libraries and loader payload
 *     /usr/lib/reliefos/apps        application packages (manifest + ELF)
 *     /usr/lib/reliefos/drivers     Ring-0 driver modules
 *     /usr/lib/reliefos/tests       diagnostic guest probes
 *     /usr/share/reliefos           desktop resources
 *     /usr/share/fonts/reliefos     ReliefOS UI fonts
 *     /usr/share/doc/reliefos       bundled help and vendor notices
 *
 * tools/reliefos_layout.py is the build-side mirror of this header.  Keep the
 * two files synchronized and do not add competing path literal tables.
 */

/* Directories (no trailing slash so they concatenate with "/name"). */
#define RELIEFOS_LAYOUT_BIN "/bin"
#define RELIEFOS_LAYOUT_SBIN "/sbin"
#define RELIEFOS_LAYOUT_LIB "/lib"
#define RELIEFOS_LAYOUT_BOOT "/boot"
#define RELIEFOS_LAYOUT_SRV "/srv"
#define RELIEFOS_LAYOUT_USR_BIN "/usr/bin"
#define RELIEFOS_LAYOUT_USR_SBIN "/usr/sbin"
#define RELIEFOS_LAYOUT_USR_LIB "/usr/lib"
#define RELIEFOS_LAYOUT_USR_SHARE "/usr/share"
#define RELIEFOS_LAYOUT_ETC_RELIEFOS "/etc/reliefos"
#define RELIEFOS_LAYOUT_ETC_SSL_CERTS "/etc/ssl/certs"
#define RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/var/lib/reliefos"
#define RELIEFOS_LAYOUT_VAR_CACHE_RELIEFOS "/var/cache/reliefos"
#define RELIEFOS_LAYOUT_VAR_LOG "/var/log"
#define RELIEFOS_LAYOUT_VAR_TMP "/var/tmp"
#define RELIEFOS_LAYOUT_RUN_RELIEFOS "/run/reliefos"
#define RELIEFOS_LAYOUT_RELIEFOS_LIB "/usr/lib/reliefos"
#define RELIEFOS_LAYOUT_RELIEFOS_APPS "/usr/lib/reliefos/apps"
#define RELIEFOS_LAYOUT_RELIEFOS_DRIVERS "/usr/lib/reliefos/drivers"
#define RELIEFOS_LAYOUT_RELIEFOS_TESTS "/usr/lib/reliefos/tests"
#define RELIEFOS_LAYOUT_RELIEFOS_SHARE "/usr/share/reliefos"
#define RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/usr/share/reliefos/resources"
#define RELIEFOS_LAYOUT_RELIEFOS_FONTS "/usr/share/fonts/reliefos"
#define RELIEFOS_LAYOUT_RELIEFOS_DOC "/usr/share/doc/reliefos"
#define RELIEFOS_LAYOUT_LICENSES "/usr/share/licenses"
#define RELIEFOS_LAYOUT_MISC "/usr/share/misc"
#define RELIEFOS_LAYOUT_TERMINFO "/usr/share/terminfo"
#define RELIEFOS_LAYOUT_LOCALE "/usr/share/locale"

/* Third-party suites retained under /opt with /usr/bin command entries. */
#define RELIEFOS_LAYOUT_OPT_DYNE "/opt/dyne"
#define RELIEFOS_LAYOUT_OPT_PYTHON "/opt/python"

/* Runtime configuration. */
#define RELIEFOS_PATH_RELIEFOS_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/leonos.conf"
#define RELIEFOS_PATH_DISPLAY_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/display.conf"
#define RELIEFOS_PATH_DRIVERS_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/drivers.conf"
#define RELIEFOS_PATH_TASKBAR_CFG RELIEFOS_LAYOUT_ETC_RELIEFOS "/taskbar.cfg"
#define RELIEFOS_PATH_NETWORK_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/network.conf"
#define RELIEFOS_PATH_NETWORK_BAK RELIEFOS_LAYOUT_ETC_RELIEFOS "/network.conf.bak"
#define RELIEFOS_PATH_NETWORK_TMP RELIEFOS_LAYOUT_ETC_RELIEFOS "/network.conf.tmp"
#define RELIEFOS_PATH_LOCALE_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/locale.conf"
#define RELIEFOS_PATH_ENVIRONMENT_CONF RELIEFOS_LAYOUT_ETC_RELIEFOS "/environment.conf"
#define RELIEFOS_PATH_FILEASSOC_CFG RELIEFOS_LAYOUT_ETC_RELIEFOS "/fileassoc.cfg"
#define RELIEFOS_PATH_DESKTOP_ENTRIES RELIEFOS_LAYOUT_ETC_RELIEFOS "/desktop-entries.conf"
#define RELIEFOS_PATH_LESSKEY RELIEFOS_LAYOUT_ETC_RELIEFOS "/lesskey"

/* Persistent and volatile state. */
#define RELIEFOS_PATH_USERS_DB RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/users.db"
#define RELIEFOS_PATH_ACCOUNTS_DB RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/accounts.db"
#define RELIEFOS_PATH_STARTUP_DB RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/startup.db"
#define RELIEFOS_PATH_STARTUP_DENIALS_DB RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/startup-denials.db"
#define RELIEFOS_PATH_OOBE_DONE RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/oobe.done"
#define RELIEFOS_PATH_LICENSE RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/license.dat"
#define RELIEFOS_PATH_KERNELDEBUG_ENABLED RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/kerneldebug.enabled"
#define RELIEFOS_PATH_KERNELDEBUG_CONTROL RELIEFOS_LAYOUT_VAR_LIB_RELIEFOS "/kernel-debug"
#define RELIEFOS_PATH_SESSION_USER RELIEFOS_LAYOUT_RUN_RELIEFOS "/session-user"

/* Shared resources. */
#define RELIEFOS_PATH_CACERT RELIEFOS_LAYOUT_ETC_SSL_CERTS "/ca-certificates.crt"
#define RELIEFOS_PATH_MAGIC RELIEFOS_LAYOUT_MISC "/magic.mgc"
#define RELIEFOS_PATH_SYSTEM_FONT RELIEFOS_LAYOUT_RELIEFOS_FONTS "/system.psf"
#define RELIEFOS_PATH_UI_METRO_FONT RELIEFOS_LAYOUT_RELIEFOS_FONTS "/leonos-metro.ttf"
#define RELIEFOS_PATH_UI_WIN95_FONT RELIEFOS_LAYOUT_RELIEFOS_FONTS "/leonos-win95.ttf"
#define RELIEFOS_PATH_BROWSER_FONT RELIEFOS_LAYOUT_RELIEFOS_FONTS "/times-new-roman.ttf"
#define RELIEFOS_PATH_BROWSER_CJK_FONT RELIEFOS_LAYOUT_RELIEFOS_FONTS "/simsun.ttc"
#define RELIEFOS_PATH_MOUSE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/mouse.bmp"
#define RELIEFOS_PATH_WALLPAPER_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/wallpaper-metro.bmp"
#define RELIEFOS_PATH_LOGO_PNG RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/logo.png"
#define RELIEFOS_PATH_WINDOW_MINIMIZE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/window-button-minimize.bmp"
#define RELIEFOS_PATH_WINDOW_MAXIMIZE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/window-button-maximize.bmp"
#define RELIEFOS_PATH_WINDOW_RESTORE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/window-button-restore.bmp"
#define RELIEFOS_PATH_WINDOW_CLOSE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/window-button-close.bmp"
#define RELIEFOS_PATH_MINESWEEPER_MINE_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/minesweeper-mine.bmp"
#define RELIEFOS_PATH_MINESWEEPER_FLAG_BMP RELIEFOS_LAYOUT_RELIEFOS_RESOURCES "/minesweeper-flag.bmp"
#define RELIEFOS_PATH_HELP RELIEFOS_LAYOUT_RELIEFOS_DOC "/leonos.hlp"

/* Shared libraries and interpreter payload. */
#define RELIEFOS_PATH_MUSL_INTERP "/lib/ld-musl-x86_64.so.1"
#define RELIEFOS_PATH_LIBC RELIEFOS_LAYOUT_LIB "/libc.so"
#define RELIEFOS_PATH_LIBMIMALLOC RELIEFOS_LAYOUT_LIB "/libmimalloc.so.3"
/* Diagnostic-only probe constant.  ReliefOS does not ship or claim a glibc
 * loader; the musl interpreter above is the only supported PT_INTERP. */
#define RELIEFOS_PATH_GLIBC_INTERP "/lib64/ld-linux-x86-64.so.2"
#define RELIEFOS_PATH_LIBRELIEFOS RELIEFOS_LAYOUT_RELIEFOS_LIB "/libreliefos.so.2"
#define RELIEFOS_PATH_LIBRELIEFOS_COMPAT RELIEFOS_LAYOUT_RELIEFOS_LIB "/libleonos.so.1"
#define RELIEFOS_PATH_OLD_NATIVE_INTERP RELIEFOS_LAYOUT_RELIEFOS_LIB "/ld-leonos.elf"
#define RELIEFOS_PATH_KERNELDEBUG_MODULE RELIEFOS_LAYOUT_RELIEFOS_LIB "/kerneldebug.sys"

/* Runtime view of the boot partition; the ESP itself normally mounts /boot. */
#define RELIEFOS_PATH_BOOT_KERNEL "/boot/reliefos/kernel.sys"
#define RELIEFOS_PATH_BOOT_KERNEL_LEGACY "/boot/leonos/kernel.sys"
#define RELIEFOS_PATH_BOOT_KERNELDEBUG_MARKER "/boot/reliefos/state/kerneldebug.next"
#define RELIEFOS_PATH_BOOT_DISPLAY_CONF "/boot/reliefos/config/display.conf"

#endif /* RELIEFOS_LAYOUT_H */
