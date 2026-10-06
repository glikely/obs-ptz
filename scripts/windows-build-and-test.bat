@echo off
setlocal enabledelayedexpansion

rem Builds obs-ptz with -DENABLE_UI_TESTS=ON for the given architecture,
rem overlays it into that architecture's standing OBS install under
rem C:\OBS-Test\<arch>, and runs the tests in tests\obs-integration
rem against it. See tests/ui-harness/README.md's "Testing on Windows"
rem section for the one-time setup this depends on (C:\OBS-Test\arm64
rem and C:\OBS-Test\x64 must already exist).
rem
rem Usage: windows-build-and-test.bat <arm64|x64> [pytest arguments, in place of the whole of tests\obs-integration]
rem Run from the repo root (the directory this script's own CMakePresets.json lives in).

set ARCH=%1
if "%ARCH%"=="" (
  echo Usage: %~n0 ^<arm64^|x64^>
  exit /b 1
)
if not "%ARCH%"=="arm64" if not "%ARCH%"=="x64" (
  echo Unknown architecture "%ARCH%" - expected arm64 or x64
  exit /b 1
)

set OBSDIR=C:\OBS-Test\%ARCH%
if not exist "%OBSDIR%\bin\64bit\obs64.exe" (
  echo %OBSDIR% has no obs64.exe - run the one-time setup in
  echo tests/ui-harness/README.md's "Testing on Windows" section first.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
  echo vswhere.exe couldn't find a Visual Studio install with the C++ workload
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH%
if errorlevel 1 (
  echo vcvarsall.bat failed for %ARCH%
  exit /b 1
)

echo === cmake configure (%ARCH%) ===
cmake --preset windows-%ARCH% -DENABLE_UI_TESTS=ON
if errorlevel 1 exit /b 1

echo === cmake build (%ARCH%) ===
cmake --build build_%ARCH% --target obs-ptz --config RelWithDebInfo
if errorlevel 1 exit /b 1

set RUNDIR=%CD%\build_%ARCH%\rundir\RelWithDebInfo
echo === overlaying obs-ptz into %OBSDIR% ===
copy /y "%RUNDIR%\obs-ptz.dll" "%OBSDIR%\obs-plugins\64bit\obs-ptz.dll"
copy /y "%RUNDIR%\obs-ptz.pdb" "%OBSDIR%\obs-plugins\64bit\obs-ptz.pdb"
robocopy "%RUNDIR%\obs-ptz" "%OBSDIR%\data\obs-plugins\obs-ptz" /E /NFL /NDL /NJH /NJS
if errorlevel 8 (
  echo overlay robocopy failed
  exit /b 1
)

set VENV=C:\OBS-Test\venv
if not exist "%VENV%\Scripts\python.exe" (
  echo === making the Python environment in %VENV% ===
  python -m venv "%VENV%"
  if errorlevel 1 exit /b 1
  "%VENV%\Scripts\python.exe" -m pip install -q -r tests\obs-integration\requirements.txt
  if errorlevel 1 exit /b 1
)

echo === running tests\obs-integration against %ARCH% ===
set PTZ_TEST_OBS_BIN=%OBSDIR%\bin\64bit\obs64.exe
set PTZSIM_OBS_BINARY=%PTZ_TEST_OBS_BIN%
shift
set PYTEST_ARGS=%1 %2 %3 %4 %5 %6 %7 %8 %9
if "%1"=="" set PYTEST_ARGS=tests\obs-integration
"%VENV%\Scripts\python.exe" -m pytest -q %PYTEST_ARGS%
exit /b %ERRORLEVEL%
