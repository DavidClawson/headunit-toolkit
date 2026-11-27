#!/bin/bash
# radio-session.sh: catch a fresh power-up in FEL, run dram/radio.bin while
# recording the rear RCA through a USB line-in (sox, device "iMic USB audio
# system"), then print the level per second. Needs sox; set AUDIO_DIR to keep
# the recording.
cd /Users/david/Desktop/stereo/tools/fel
A=${AUDIO_DIR:-/tmp}   # where the iMic recording goes
# wait for any old FEL session to be gone, so we catch a fresh power-up
while ~/src/xfel/xfel version 2>/dev/null | grep -q AWUSBFEX; do sleep 1; done
./fel-catch.sh 1800 || exit 1
xfel_ok=0; for i in $(seq 20); do ~/src/xfel/xfel version 2>/dev/null | grep -q AWUSBFEX && { xfel_ok=$((xfel_ok+1)); [ $xfel_ok -ge 3 ] && break; } || xfel_ok=0; sleep 0.5; done
sox -q -t coreaudio "iMic USB audio system" -c 2 -r 44100 $A/run5.wav trim 0 100 &
REC=$!
./felrun.py dram/radio.bin || { kill $REC; say "Radio test failed."; exit 1; }
wait $REC
say "Radio test done."
echo "=== iMic RMS per second (whole run)"
for s in $(seq 0 99); do printf "%2ds %s\n" $s "$(sox $A/run5.wav -n trim $s 1 stat 2>&1 | grep -E 'RMS +amp' | tr -s ' ' | cut -d' ' -f3)"; done | awk '{printf "%s%s", $0, (NR%5==0?"\n":"   ")}'
