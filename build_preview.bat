@echo off
setlocal
set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" (
    echo Visual Studio 2022 Build Tools were not found.
    exit /b 1
)

"%MSBUILD%" "%~dp0preview\NativeMusicPlayerPreview.vcxproj" /m /p:Configuration=Release /p:Platform=x64
exit /b %errorlevel%
