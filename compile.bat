@echo off
echo Compiling VCFLister project...
echo.

REM Try to find and use Visual Studio tools
set VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build
if exist "%VS_PATH%" (
    echo Using Visual Studio 2022 Community
    call "%VS_PATH%\vcvarsall.bat" x64
    msbuild VCFLister.sln /p:Configuration=Debug /p:Platform="x64"
) else (
    echo Visual Studio 2022 not found
    echo Please open the solution in Visual Studio and build manually
    pause
)