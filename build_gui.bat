@echo off
rem Build OpenCode Session Manager GUI (MinGW-w64 + Dear ImGui + Win32 + OpenGL3 + sqlite3)
rem No bundled DLLs - only Windows system libraries (opengl32.dll, dwmapi.dll, ...).
rem
rem Override the toolchain location:
rem   set MINGW_ROOT=D:\msys64\ucrt64
rem   set MINGW_OPT=D:\msys64\ucrt64\opt
setlocal

if "%MINGW_ROOT%"=="" set MINGW_ROOT=C:\MinGW
if "%MINGW_OPT%"==""  set MINGW_OPT=%MINGW_ROOT%\opt

set GCC=%MINGW_ROOT%\bin\g++
set WINDRES=%MINGW_ROOT%\bin\windres
set IMGUI=imgui
set INCS=-I"%IMGUI%" -I"%IMGUI%\backends" -I"%MINGW_OPT%\include"
set LIBS=-L"%MINGW_OPT%\lib" -lsqlite3 -lopengl32 -ldwmapi -luser32 -lgdi32 -lshell32 -lole32
set OUT=dist

rem Set OPENCODE_NO_STATIC=1 when linking against shared libraries.
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

%GCC% -std=c++17 -O2 -Wall %STATIC_FLAG% %INCS% ^
    main_gui.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp ^
    "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" ^
    "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_opengl3.cpp" ^
    "%OUT%\resource.o" %LIBS% -mwindows -o "%OUT%\opencode-session-manager-gui.exe"
if errorlevel 1 goto :fail

echo Build OK: %OUT%\opencode-session-manager-gui.exe
exit /b 0

:fail
echo Build FAILED
exit /b 1
