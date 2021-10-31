call "%~dp0..\ci_includes.generated.cmd"

mkdir package
cd package

REM Package ZIP archive
7z a "%PluginName%-%PackageVersion%-Windows.zip" "..\release\*"

REM Build installer
iscc ..\installer\installer-Windows.generated.iss /O. /F"%PluginName%-%PackageVersion%-Windows-Installer"
