@echo off
rem Build obtan.exe with MSVC. Output: obtan.exe in this folder.
rem This file is ASCII-only on purpose (cmd reads .cmd in the OEM code page).
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:build
if not exist build mkdir build
rc /nologo /fo build\obtan.res src\obtan.rc || exit /b 1
cl /nologo /utf-8 /W4 /O2 /MT /Fo:build\ /Fe:obtan.exe src\obtan.c src\source.c src\net.c src\json.c src\rx.c src\util.c build\obtan.res /link /SUBSYSTEM:WINDOWS /STACK:4194304 || exit /b 1
for %%F in (obtan.exe) do echo OK: obtan.exe (%%~zF bytes)
