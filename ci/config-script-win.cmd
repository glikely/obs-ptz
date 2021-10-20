set BASEPATH=%HOMEPATH%\hacking
set DepsPath=%BASEPATH%\obs-deps\win64
set QTDIR=c:\qt\5.15.2\msvc2019_64
set LibObs_DIR=%BASEPATH%\obs-studio\build\libobs
set build_config=RelWithDebInfo
cmake ..
