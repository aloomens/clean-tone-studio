@echo off
setlocal
rem Copies Clean Tone Studio.vst3 into the system VST3 folder (needs administrator rights).
set "SRC=%~dp0VST3\Clean Tone Studio.vst3"
set "DST=%CommonProgramFiles%\VST3"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator permission...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

if not exist "%SRC%" (
    echo Could not find "%SRC%". Extract the whole zip first, then run this again.
    pause
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"
rem Version 1.6.0 was called "Open Clean Tone Studio". It is the same plugin (same ID, so your
rem projects load it), so remove that copy: two copies of one plugin confuse DAWs.
if exist "%DST%\Open Clean Tone Studio.vst3" (
    attrib -r "%DST%\Open Clean Tone Studio.vst3" >nul 2>&1
    rmdir /s /q "%DST%\Open Clean Tone Studio.vst3"
    echo Removed "Open Clean Tone Studio.vst3" ^(version 1.6.0's name; it is Clean Tone Studio again^).
)
if exist "%DST%\Clean Tone Studio.vst3" (
    attrib -r "%DST%\Clean Tone Studio.vst3" >nul 2>&1
    rmdir /s /q "%DST%\Clean Tone Studio.vst3"
)
xcopy "%SRC%" "%DST%\Clean Tone Studio.vst3\" /e /i /q /y >nul
if %errorlevel% neq 0 (
    echo Copy failed.
    pause
    exit /b 1
)
rem Let Explorer show the plugin's folder icon (desktop.ini needs a read-only/system folder)
attrib +r "%DST%\Clean Tone Studio.vst3" >nul 2>&1
attrib +h +s "%DST%\Clean Tone Studio.vst3\desktop.ini" >nul 2>&1
echo.
echo Installed to "%DST%\Clean Tone Studio.vst3"
echo Rescan plugins in your DAW to find Clean Tone Studio.
echo.
pause
