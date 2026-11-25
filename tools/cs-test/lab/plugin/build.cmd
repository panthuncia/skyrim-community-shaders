@echo off
rem Builds tools\cs-test\lab\plugin\build\Release\!CSTestLab.dll, the lab helper that cs-test puts in each lab slot.
rem Needs VCPKG_ROOT (the same vcpkg Community Shaders builds with) and Visual Studio 2026.
setlocal
if "%VCPKG_ROOT%"=="" (echo VCPKG_ROOT is not set & exit /b 1)
set HERE=%~dp0
cmake -S "%HERE%." -B "%HERE%build" -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release ^
  -DVCPKG_OVERLAY_TRIPLETS="%HERE%..\..\..\..\cmake\triplets" || exit /b 1
cmake --build "%HERE%build" --config Release || exit /b 1
echo Built %HERE%build\Release\!CSTestLab.dll
