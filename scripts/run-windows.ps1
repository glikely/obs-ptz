Push-Location $PSScriptRoot\..
cp -r -force .\release\* ..\obs-studio\build_x64\rundir\RelWithDebInfo\
cd ..\obs-studio\build_x64\rundir\RelWithDebInfo\bin\64bit
.\obs64.exe
Pop-Location
