#!/bin/bash
# Busiest threads of a process over an interval, by thread name (read-only).
# Usage: bash thread_load.sh <process name> [seconds=3]
#   e.g. bash thread_load.sh org.azahar_emu.azahar.fatesfix:engine_switch
# Prints % of one CPU core per thread: 100 means a thread that never stops (a bottleneck).
export MSYS_NO_PATHCONV=1
proc=$1; secs=${2:-3}
adb shell "p=\$(pidof $proc | cut -d' ' -f1); [ -z \"\$p\" ] && { echo 'not running'; exit 1; }; \
snap() { for t in /proc/\$p/task/*; do echo \"\${t##*/} \$(cut -d' ' -f14,15 \$t/stat 2>/dev/null) \$(cat \$t/comm 2>/dev/null | tr ' ' '_')\"; done; }; \
snap > /data/local/tmp/tl1; sleep $secs; snap > /data/local/tmp/tl2; \
hz=\$(getconf CLK_TCK); awk -v s=$secs -v hz=\$hz 'NR==FNR{a[\$1]=\$2+\$3; next} (\$1 in a){printf \"%6.1f%%  %s\\n\", (\$2+\$3-a[\$1])*100/(s*hz), \$4}' /data/local/tmp/tl1 /data/local/tmp/tl2 | sort -rn | head -12; rm -f /data/local/tmp/tl1 /data/local/tmp/tl2"
