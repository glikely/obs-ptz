REM Runs all build steps for building on Windows

set build_config=Debug
set OBSPath=%cd%\..\obs-studio
set QTDIR32=c:\Qt\5.15.2\msvc2019
set QTDIR64=c:\Qt\5.15.2\msvc2019_64

call ./ci/windows/prepare-windows.cmd

"c:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild" /p:Configuration=%build_config% build32\obs-ptz.sln
"c:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild" /p:Configuration=%build_config% build64\obs-ptz.sln
