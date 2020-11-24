set BASEPATH=C:\Users\glike\hacking
set DepsPath=%BASEPATH%\obs-deps\win64
set QT_DIR=%BASEPATH%\Qt5\msvc2017_64
set LibObs_DIR=%BASEPATH%\obs-studio\build\libobs
set build_config=RelWithDebInfo
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
cd ..
