@echo off
rem Build a Windows 95 compatible 32-bit GUI executable with Open Watcom.
wcl386 -q -bt=nt -l=nt_win -3r -os -s -w4 rage2p_test.c -fe=R2PTEST.EXE
if errorlevel 1 goto fail
echo Built R2PTEST.EXE
exit /b 0
:fail
echo Build failed.
exit /b 1
