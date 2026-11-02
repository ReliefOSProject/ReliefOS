#!/bin/sh
# Menu/key launches inherit IceWM's nice value.  Build jobs started from the
# terminal use normal priority, just like the initial XDM session terminal.
if test -x /bin/busybox; then
    /bin/busybox renice 0 -p "$$" >/dev/null 2>&1 || :
fi
# Keep st's default palette and start the user's login shell.
exec /usr/bin/st -T ReliefOS -g 100x32+0+0 -e "${SHELL:-/bin/sh}" -l
