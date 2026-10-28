@echo off
setlocal
rem ============================================================
rem Build the OpenAlgo broker plugin (32-bit and 64-bit) from
rem this submission folder. Zorro root defaults to the parent of
rem this folder (i.e. this folder sits inside a Zorro install);
rem override by setting the ZORRO_DIR environment variable.
rem
rem Requires: Visual Studio 2022 Build Tools (C++ workload).
rem Output:   bin\OpenAlgo.dll   (32-bit, for Zorro.exe   / Zorro\Plugin\)
rem           bin64\OpenAlgo.dll (64-bit, for Zorro64.exe / Zorro\Plugin64\)
rem ============================================================
if not defined ZORRO_DIR for %%I in ("%~dp0..") do set "ZORRO_DIR=%%~fI"

set "VCBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
if not exist "%VCBUILD%\vcvars32.bat" set "VCBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build"

if not exist "%ZORRO_DIR%\include\zorro.h" (
    echo Cannot find Zorro headers under "%ZORRO_DIR%\include".
    echo Set ZORRO_DIR to your Zorro installation folder and retry.
    exit /b 1
)

pushd "%ZORRO_DIR%"
if not exist Log   mkdir Log
if not exist Cache mkdir Cache
if not exist "%~dp0bin"   mkdir "%~dp0bin"
if not exist "%~dp0bin64" mkdir "%~dp0bin64"

echo === 32-bit build ===
call "%~dp0Source\VC++\compile.bat" "%~dp0Source\VC++\OpenAlgo.cpp" "%~dp0bin\OpenAlgo.dll" "%VCBUILD%"
type Log\compiler.log

echo.
echo === 64-bit build ===
call "%~dp0Source\VC++\compile64.bat" "%~dp0Source\VC++\OpenAlgo.cpp" "%~dp0bin64\OpenAlgo.dll" "%VCBUILD%"
type Log\compiler.log
popd

echo.
echo Done. To install:
echo   copy bin\OpenAlgo.dll    to  Zorro\Plugin\      (for Zorro.exe, 32-bit)
echo   copy bin64\OpenAlgo.dll  to  Zorro\Plugin64\    (for Zorro64.exe, 64-bit)
echo   copy Plugin\OpenAlgoAssets.csv  to  Zorro\Plugin\
endlocal
