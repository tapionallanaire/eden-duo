#!/bin/bash
# Presses the AYN Thor's built-in controller buttons (adb key events don't reach Switch games).
# Usage: bash pad.sh <action> [action ...]
#   a | b | x | y | plus | minus     tap a button
#   left|right|up|down[:ms]          hold the D-pad (default 400 ms), then release it
#   wait:ms                          pause
# e.g. bash pad.sh a wait:1500 a right:2000
# Only with the user's go-ahead: this drives the game for real.
export MSYS_NO_PATHCONV=1
dev=/dev/input/event9
cmds=""
key() { cmds+="sendevent $dev 1 $1 1; sendevent $dev 0 0 0; sleep 0.12; sendevent $dev 1 $1 0; sendevent $dev 0 0 0; "; }
hat() { cmds+="sendevent $dev 3 $1 $2; sendevent $dev 0 0 0; sleep $3; sendevent $dev 3 $1 0; sendevent $dev 0 0 0; "; }
for act in "$@"; do
  name=${act%%:*}; arg=${act#*:}; [ "$arg" = "$act" ] && arg=400
  secs=$(awk "BEGIN{print $arg/1000}")
  case $name in
    a) key 304 ;; b) key 305 ;; x) key 307 ;; y) key 308 ;;
    plus) key 315 ;; minus) key 314 ;;
    left) hat 16 -1 $secs ;; right) hat 16 1 $secs ;;
    up) hat 17 -1 $secs ;; down) hat 17 1 $secs ;;
    wait) cmds+="sleep $secs; " ;;
    *) echo "unknown action: $act"; exit 2 ;;
  esac
done
adb shell "$cmds"
