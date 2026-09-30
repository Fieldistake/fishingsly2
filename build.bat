@echo off
where cl >nul 2>nul
if %errorlevel%==0 (
  cl /nologo /O2 /EHsc /std:c++17 main.cpp /Fe:FishingHyperTracker.exe /link /SUBSYSTEM:WINDOWS
) else (
  g++ -O2 -std=c++17 -mwindows -static main.cpp -o FishingHyperTracker.exe -lcomctl32 -luser32 -lgdi32 -lwinmm
)
