@echo off
setlocal
rem Override SLE_VCVARS to use a different Visual Studio installation.
if not defined SLE_VCVARS set "SLE_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%SLE_VCVARS%" >nul
if errorlevel 1 exit /b 1
set "JSON="
for /d %%P in ("%LOCALAPPDATA%\.xmake\packages\n\nlohmann_json\v3.12.0\*") do if exist "%%~P\include\nlohmann\json.hpp" set "JSON=%%~P\include"
if not defined JSON (echo Missing xmake nlohmann_json v3.12.0 package & exit /b 1)
set "INCLUDE=%JSON%;%INCLUDE%"
node tests/generate-gif-fixtures.cjs
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc tests/input_model_test.cpp /Febuild/input_model_test.exe /Fobuild/input_model_test.obj
if errorlevel 1 exit /b 1
build\input_model_test.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc tests/vanilla_ui_test.cpp /Febuild/vanilla_ui_test.exe /Fobuild/vanilla_ui_test.obj
if errorlevel 1 exit /b 1
build\vanilla_ui_test.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc tests/gif_test.cpp src-client/GifAnimation.cpp /Febuild/gif_test.exe /Fobuild/ windowscodecs.lib ole32.lib
if errorlevel 1 exit /b 1
build\gif_test.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc /DSLE_CLIENT tests/image_pipeline_test.cpp src-client/GifAnimation.cpp src/common/BuiltinImages.cpp /Febuild/image_pipeline_test.exe /Fobuild/ windowscodecs.lib ole32.lib bcrypt.lib winhttp.lib
if errorlevel 1 exit /b 1
build\image_pipeline_test.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc tests/status_queue_test.cpp /Febuild/status_queue_test.exe /Fobuild/ winhttp.lib
if errorlevel 1 exit /b 1
build\status_queue_test.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc /utf-8 /Isrc tests/udp_probe_test.cpp src-server/UdpEcho.cpp /Febuild/udp_probe_test.exe /Fobuild/ ws2_32.lib bcrypt.lib
if errorlevel 1 exit /b 1
build\udp_probe_test.exe
if errorlevel 1 exit /b 1
