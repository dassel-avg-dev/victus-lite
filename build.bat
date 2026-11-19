@echo off
setlocal

where cl >nul 2>nul
if %errorlevel%==0 (
    echo Building with MSVC...
    cl /std:c++17 /O2 /EHsc /W4 main.cpp ole32.lib oleaut32.lib wbemuuid.lib /Fe:victus.exe
    goto :done
)

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo Building with MinGW g++...
    g++ -std=c++17 -O2 -Wall -Wextra main.cpp -o victus.exe -lole32 -loleaut32 -lwbemuuid
    goto :done
)

where clang++ >nul 2>nul
if %errorlevel%==0 (
    echo Building with Clang...
    clang++ -std=c++17 -O2 -Wall -Wextra main.cpp -o victus.exe -lole32 -loleaut32 -lwbemuuid
    goto :done
)

echo No C++ compiler found in PATH.
echo Install "Desktop development with C++" in Visual Studio Installer, or install MinGW-w64:
echo   winget install BrechtSanders.WinLibs.POSIX.UCRT
exit /b 1

:done
if %errorlevel%==0 (
    echo Build succeeded: victus.exe
) else (
    echo Build failed.
)
