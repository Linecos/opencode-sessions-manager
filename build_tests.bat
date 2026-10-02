@echo off
rem Build and run the data layer tests (no ncurses / no OpenGL needed).
rem Usage:  build_tests.bat [scratch-dir]
setlocal

if "%MINGW_ROOT%"=="" set MINGW_ROOT=C:\MinGW
if "%MINGW_OPT%"==""  set MINGW_OPT=%MINGW_ROOT%\opt

set GCC=%MINGW_ROOT%\bin\g++
set INCS=-I. -I"%MINGW_OPT%\include"
set LIBS=-L"%MINGW_OPT%\lib" -lsqlite3 -luser32 -lshell32 -lole32
set OUT=dist
set SCRATCH=%1
if "%SCRATCH%"=="" set SCRATCH=test-scratch

if not exist "%GCC%.exe" (
    echo g++ not found at "%GCC%.exe" -- set MINGW_ROOT first.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

%GCC% -std=c++17 -O2 -Wall -Wextra %INCS% ^
    tests\test_data.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp %LIBS% ^
    -o "%OUT%\test_data.exe"
if errorlevel 1 goto :fail

echo Running tests...
"%OUT%\test_data.exe" "%SCRATCH%"
set RC=%errorlevel%
if not "%RC%"=="0" (
    echo Tests FAILED
    exit /b %RC%
)
echo Tests OK
exit /b 0

:fail
echo Build FAILED
exit /b 1
