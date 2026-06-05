@echo off

cmd /s /c ""C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 > nul && msbuild sh4xe.sln /p:Configuration=Release /p:Platform=Win32 /p:DeployAfterBuild=true /m"