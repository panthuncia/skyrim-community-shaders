@echo off
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk
cmake --preset=Dev-Fast >nul
cmake --build --preset=Dev-Fast -- -k 0
