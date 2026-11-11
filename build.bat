@echo off
setlocal
where gcc >nul 2>&1
if errorlevel 1 (
    echo gcc was not found. Install WinLibs or MinGW, then open a new terminal.
    exit /b 1
)
gcc -std=c11 -Wall -Wextra -O2 -o tracker.exe tracker.c -lws2_32
if errorlevel 1 exit /b 1
gcc -std=c11 -Wall -Wextra -O2 -o peer.exe peer.c -lws2_32
if errorlevel 1 exit /b 1
if not exist peer1 mkdir peer1
if not exist peer2 mkdir peer2
if not exist peer3 mkdir peer3
copy /Y peer.exe peer1\peer.exe >nul
copy /Y peer.exe peer2\peer.exe >nul
copy /Y peer.exe peer3\peer.exe >nul
echo Build succeeded. Run tracker.exe, then peer.exe inside peer1, peer2, and peer3.
