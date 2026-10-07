@echo off
setlocal

@call "%~3%\vcvars32.bat"
@set "INCLUDE=%INCLUDE%;include"

cl /Fo"Cache/" /EHsc /fp:strict /Zc:wchar_t /Gd /MT /O2 /D "WIN32" /D "_WINDLL" /D "_MBCS" "%~1" "Source\VC++\ZorroDLL.cpp" /link /DLL /NOLOGO /IMPLIB:"Cache\Imp.lib" /OUT:"%~2" > Log\compiler.log

endlocal