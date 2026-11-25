#!/bin/bash
# Run in WSL from the project: bash Tests/ardour-check.sh (after the Linux build in ~/src/cts)
# Load the fresh Linux build of Clean Tone Studio (VST3 and LV2) in headless Ardour 8
cd ~/src/cts
A=$PWD/build/CleanToneStudio_artefacts/Release
log=~/.config/ardour8/plugin_metadata/scan_log
cp "$log" /tmp/scan_log.bak
mkdir -p ~/.vst3 ~/.lv2
rm -rf ~/.vst3/"Clean Tone Studio.vst3" ~/.lv2/"Clean Tone Studio.lv2"
cp -r "$A/VST3/Clean Tone Studio.vst3" ~/.vst3/; cp -r "$A/LV2/Clean Tone Studio.lv2" ~/.lv2/
# Ardour wants the cache strictly newer than the plugin: date the fresh copy a minute back
find ~/.vst3/"Clean Tone Studio.vst3" -exec touch -d "1 minute ago" {} +
LD_LIBRARY_PATH=/usr/lib/ardour8 /usr/lib/ardour8/ardour-vst3-scanner -f ~/.vst3/"Clean Tone Studio.vst3" 2>&1 | grep -E "Found Plugin"
# headless Ardour reads the scanner's cache but skips a plugin its scan log marks "updated" (2): mark the fresh scan OK
sed -i 's|\(path="/home/[^"]*/.vst3/Clean Tone Studio.vst3" scan-log="" scan-result="\)[0-9]*"|\10"|' "$log"
grep -o 'path="[^"]*Clean Tone Studio.vst3"[^/]*' "$log"
rm -rf /tmp/cts-ardour; ardour8-new_empty_session /tmp/cts-ardour cts-ardour > /dev/null 2>&1
timeout 180 ardour8-lua "$(dirname "$(readlink -f "$0")")/ardour-check.lua" 2>&1 | grep -E "PASS|FAIL|saved|rror"
echo "ardour exit: ${PIPESTATUS[0]}"
echo "plugins in the saved session: $(grep -c 'name="Clean Tone Studio"' /tmp/cts-ardour/cts-ardour.ardour)"
rm -rf ~/.vst3/"Clean Tone Studio.vst3" ~/.lv2/"Clean Tone Studio.lv2" /tmp/cts-ardour
cp /tmp/scan_log.bak "$log"
