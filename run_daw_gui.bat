@echo off
rem Build and run MiniDAW (multitrack recorder). Double-click this file to use it.
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 set "PATH=C:\msys64\ucrt64\bin;%PATH%"

if not exist output mkdir output

echo Building daw_gui.exe ...
g++ -O2 daw_gui.cpp -o output\daw_gui.exe -static -mwindows -lwinmm -lgdiplus -lgdi32 -lole32 -lcomdlg32 -lshell32 2>nul
if errorlevel 1 (
    echo.
    echo Build failed. Showing compiler errors:
    g++ -O2 daw_gui.cpp -o output\daw_gui.exe -static -mwindows -lwinmm -lgdiplus -lgdi32 -lole32 -lcomdlg32 -lshell32
    pause
    exit /b 1
)

echo Starting MiniDAW ...
start "" "output\daw_gui.exe"
