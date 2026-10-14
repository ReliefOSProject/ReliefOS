#!/bin/sh
# The session terminal: the colours and initial size of the previous session.
# st fixes its palette at build time, so restore the session colours through the
# OSC colour escapes it understands, then hand it a login shell.
exec /usr/bin/st -T ReliefOS -g 100x32+0+0 -e /bin/sh -c 'printf "\033]10;#22242e\007\033]11;#f7f8fb\007\033]12;#3b62a6\007\033[2J\033[H"; exec "${SHELL:-/bin/sh}" -l'
