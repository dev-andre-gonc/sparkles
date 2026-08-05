@echo off
REM Builds the Sparkles VST3 plugin (Release, x64) and installs it to the configured
REM plugin folder via the project's PostBuildEvent (see projects/config/Sparkles-win.props).
setlocal
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
cd /d "%~dp0.."
"%MSBUILD%" Sparkles.sln -p:Configuration=Release -p:Platform=x64 -t:Sparkles-vst3
endlocal
