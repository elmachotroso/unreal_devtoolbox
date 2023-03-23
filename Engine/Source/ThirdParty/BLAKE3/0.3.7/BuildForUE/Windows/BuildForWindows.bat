@rem Copyright Epic Games, Inc. All Rights Reserved.
@echo off

set ENGINE_ROOT=%~dp0..\..\..\..\..\..
call "%ENGINE_ROOT%\Build\BatchFiles\RunUAT.bat" BuildCMakeLib -TargetPlatform=Win32 -TargetLib=BLAKE3 -TargetLibVersion=0.3.7 -TargetConfigs=Release -LibOutputPath=lib -CMakeGenerator=VS2019 -SkipCreateChangelist || exit /b
call "%ENGINE_ROOT%\Build\BatchFiles\RunUAT.bat" BuildCMakeLib -TargetPlatform=Win64 -TargetLib=BLAKE3 -TargetLibVersion=0.3.7 -TargetConfigs=Release -LibOutputPath=lib -CMakeGenerator=VS2019 -SkipCreateChangelist || exit /b
