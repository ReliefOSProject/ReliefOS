#!/bin/sh
# The session terminal: the colours and initial size of the previous session.
exec xterm -title ReliefOS -geometry 100x32+0+0 \
    -bg '#f7f8fb' -fg '#22242e' -cr '#3b62a6'
