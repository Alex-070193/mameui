set PATH=%PATH%;C:\msys64\mingw32\bin

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x86_arm
make OSD=messui TOOLS=1 CROSS_BUILD=1 NOWERROR=1 vs2017_ARM MSBUILD=1 
