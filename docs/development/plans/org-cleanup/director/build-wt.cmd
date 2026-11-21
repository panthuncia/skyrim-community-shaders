@echo off
REM build-wt.cmd <worktree dir> <configure log>: Dev-Fast build of a worktree (copy CMakeUserPresets.json in first; Aftermath off).
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %1
cmake --preset=Dev-Fast -DAFTERMATH=OFF > %2 2>&1
cmake --build --preset=Dev-Fast -- -k 0
