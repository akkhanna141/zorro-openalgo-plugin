@echo off
setlocal

@call "%~3%\vcvars64.bat"
@set "INCLUDE=%INCLUDE%;include"

cl /Fo"Cache/" /EHsc /fp:strict /Zc:wchar_t /Gd /MT /O2 /D "_WINDLL" /D "_MBCS" "%~1" "Source\VC++\ZorroDLL.cpp" /link /DLL /NOLOGO /MACHINE:X64 /IMPLIB:"Cache\Imp.lib" /OUT:"%~2" > Log\compiler.log

endlocal