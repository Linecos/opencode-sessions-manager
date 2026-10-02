@echo off
rem Build OpenCode Session Manager (TUI) -- MinGW-w64 + ncursesw + sqlite3
rem
rem The toolchain location is overridable, so the scripts no longer only work
rem on a machine with C:\MinGW:
rem   set MINGW_ROOT=D:\msys64\ucrt64
rem   set MINGW_OPT=D:\msys64\ucrt64\opt   (dir with sqlite3.h / ncursesw / libsqlite3.a)
setlocal

if "%MINGW_ROOT%"=="" set MINGW_ROOT=C:\MinGW
if "%MINGW_OPT%"==""  set MINGW_OPT=%MINGW_ROOT%\opt

set GCC=%MINGW_ROOT%\bin\g++
set WINDRES=%MINGW_ROOT%\bin\windres
set INCS=-I"%MINGW_OPT%\include"
set LIBS=-L"%MINGW_OPT%\lib" -lncursesw -lsqlite3 -luser32 -lshell32 -lole32
set OUT=dist

rem Set OPENCODE_NO_STATIC=1 when linking against shared libraries
rem (e.g. an MSYS2 toolchain), which is what CI does.
set STATIC_FLAG=-static
if "%OPENCODE_NO_STATIC%"=="1" set STATIC_FLAG=

if not exist "%GCC%.exe" (
    echo g++ not found at "%GCC%.exe"
    echo Set MINGW_ROOT to your MinGW-w64 installation, e.g. set MINGW_ROOT=D:\msys64\ucrt64
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

%WINDRES% resource.rc -O coff -o "%OUT%\resource.o"
if errorlevel 1 goto :fail

%GCC% -std=c++17 -O2 -Wall %STATIC_FLAG% -DNCURSES_WIDECHAR %INCS% ^
    main.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp "%OUT%\resource.o" %LIBS% ^
    -o "%OUT%\opencode-session-manager.exe"
if errorlevel 1 goto :fail

echo Build OK: %OUT%\opencode-session-manager.exe
exit /b 0

:fail
echo Build FAILED
exit /b 1
