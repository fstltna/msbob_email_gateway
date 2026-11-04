@echo off
rem Build the UMLMM SMTP gateway with Open Watcom (16-bit Windows DLL).
rem Run from the folder containing umlmm.c and umlmm.lnk.
rem
rem   build.bat           normal build
rem   build.bat nodate    build without the INT 21h date/time code, use this
rem                       only if the compiler rejects the _asm blocks
rem                       (messages then get no Date: header and the log lines
rem                       get a dummy time stamp)

if "%WATCOM%"=="" set WATCOM=C:\WATCOM
set PATH=%WATCOM%\BINNT;%WATCOM%\BINW;%PATH%
set INCLUDE=%WATCOM%\H\WIN;%WATCOM%\H
set EDPATH=%WATCOM%\EDDAT

set EXTRA=
if "%1"=="nodate" set EXTRA=-dUSE_DOS_DATE=0

echo Compiling...
wcc -bt=windows -bd -ml -zu -zw -s -zq -os -d0 %EXTRA% umlmm.c
if errorlevel 1 goto fail

echo Linking...
wlink @umlmm.lnk
if errorlevel 1 goto fail

echo.
echo Built UMLMM.DLL
goto end

:fail
echo.
echo BUILD FAILED - copy the messages above and send them to me.

:end
