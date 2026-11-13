@echo off
rem One-click build: fetches the build tools, asks for your game folder and
rem builds tfx.exe from your own copy of the game.  See tools\setup.ps1 for
rem what it downloads and for its options (build.bat -Game "D:\Games\TFX").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
