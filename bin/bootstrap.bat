@echo off
cd /d "%~dp0\.."
cl /O2 /Fe:kiln.exe bootstrap.c
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)
echo kiln built successfully.
