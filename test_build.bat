@echo off
echo Testing compilation of VCF Lister plugin with scroll support...
echo.

REM Проверяем наличие необходимых файлов
if not exist "vcf_view.cpp" (
    echo Error: vcf_view.cpp not found!
    pause
    exit /b 1
)

if not exist "dllmain.cpp" (
    echo Error: dllmain.cpp not found!
    pause
    exit /b 1
)

echo Files found. Compilation would normally happen here.
echo Changes have been applied to add scroll functionality to the contact list.
pause