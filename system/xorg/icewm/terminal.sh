#!/bin/sh
# Keep st's default palette and start the user's login shell.
exec /usr/bin/st -T ReliefOS -g 100x32+0+0 -e "${SHELL:-/bin/sh}" -l
