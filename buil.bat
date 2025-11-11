@echo off
setlocal

REM ==================================================================
REM == USER: Set your paths here! ==
REM ==================================================================

REM 1. Set the path to your OptiTrack SDK
REM    (Note: Guessed the Windows folder name)
set "SDK_DIR=%USERPROFILE%\Downloads\OptiTrack_Camera_SDK_3.4.0_Beta1_Windows\CameraSDK"

REM 2. Set the path to your VCPKG root installation folder
set "VCPKG_ROOT=C:\dev\vcpkg"

REM ==================================================================

REM --- No need to edit below here ---

REM vcpkg paths (assumes 64-bit windows)
set "VCPKG_INCLUDE_DIR=%VCPKG_ROOT%\installed\x64-windows\include"
set "VCPKG_LIB_DIR=%VCPKG_ROOT%\installed\x64-windows\lib"
set "VCPKG_BIN_DIR=%VCPKG_ROOT%\installed\x64-windows\bin"

set "SOURCE_FILE=src\main.cpp"
set "OUTPUT_DIR=build"
set "EXECUTABLE_NAME=optitrack_recorder.exe"
set "OUTPUT_PATH=%OUTPUT_DIR%\%EXECUTABLE_NAME%"

REM Check if we're in a developer command prompt
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo ERROR: 'cl.exe' not found.
    echo Please run this script from a Developer Command Prompt for Visual Studio.
    goto :eof
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

echo tryna compile FN.

REM cl.exe [source] /Fe:[output exe] [compiler flags] /I[includes] /link [linker flags]
cl.exe "%SOURCE_FILE%" /Fe:"%OUTPUT_PATH%" ^
    /O2 /EHsc /MD /std:c++latest /W4 ^
    /I"%SDK_DIR%\include" ^
    /I"%VCPKG_INCLUDE_DIR%" ^
    /link ^
    /LIBPATH:"%SDK_DIR%\lib" CameraLibrary.lib ^
    /LIBPATH:"%VCPKG_LIB_DIR%" avcodec.lib avutil.lib avformat.lib swscale.lib SDL2.lib SDL2main.lib

if %errorlevel% neq 0 (
    echo Build FAILED.
    goto :eof
)

echo Done FN.
echo.
echo --- IMPORTANT ---
echo To run the executable, copy all required .dll files into the '%OUTPUT_DIR%' folder:
echo   - From OptiTrack: %SDK_DIR%\lib\CameraLibrary.dll
echo   - From VCPKG:     %VCPKG_BIN_DIR%\*.dll (all FFmpeg/SDL DLLs)

:eof
