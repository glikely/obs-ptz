mkdir package
cd package


git rev-parse --short HEAD > package-version.txt
set /p PackageVersion=<package-version.txt
del package-version.txt

REM Package ZIP archive
dir ..\
dir .

7z a "obs-midi-%PackageVersion%-Windows.zip" "..\release\*"

REM Build installer
iscc ..\installer\installer.iss /O. /F"obs-midi-%PackageVersion%-Windows-Installer"
