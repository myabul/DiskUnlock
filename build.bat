@echo off
setlocal enableextensions enabledelayedexpansion
cd /d "%~dp0"
title DiskUnlock Studio - Moduler Derleme

set "OUT=DiskUnlocker.exe"
set "OBJDIR=build\obj"
set "RES=build\app.res"
set "SHIMDIR=build\shim"

echo ============================================================
echo  DiskUnlock Studio build (src\core + src\hw + src\app + src\ui)
echo ============================================================

where g++ >nul 2>&1
if errorlevel 1 (
    echo [HATA] g++ bulunamadi. MinGW-W64 bin dizinini PATH icine ekleyin.
    exit /b 1
)
where windres >nul 2>&1
if errorlevel 1 (
    echo [HATA] windres bulunamadi. MinGW-W64 kurulumu eksik gorunuyor.
    exit /b 1
)

if not exist "build" mkdir "build"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
if not exist "%SHIMDIR%" mkdir "%SHIMDIR%"
del /q "%OBJDIR%\*.obj" 2>nul
del /q "%RES%" 2>nul

set "CFLAGS=-O2 -std=c++20 -Wall -Wextra -c"
set "LDFLAGS=-mwindows -municode -static -static-libgcc -static-libstdc++"
set "LIBS=-lgdiplus -lcomctl32 -luxtheme -lsetupapi -lcfgmgr32 -lshlwapi -lwinmm -lrpcrt4 -lole32"

set "SRC=src\main.cpp"
for %%f in (src\core\*.cpp) do set "SRC=!SRC! %%f"
for %%f in (src\hw\*.cpp)   do set "SRC=!SRC! %%f"
for %%f in (src\app\*.cpp)  do set "SRC=!SRC! %%f"
for %%f in (src\ui\*.cpp)   do set "SRC=!SRC! %%f"

echo.
echo [1/4] Kaynak dosyalar derleniyor...
set "OBJS="
for %%f in (%SRC%) do (
    set "OBJ=%OBJDIR%\%%~nf.obj"
    g++ %CFLAGS% "%%f" -o "!OBJ!"
    if errorlevel 1 (
        echo.
        echo [HATA] Derleme basarisiz: %%f
        exit /b 1
    )
    set "OBJS=!OBJS! !OBJ!"
    echo        ok  %%f
)

echo.
echo [2/4] Kaynak paketleniyor (icon + manifest + version)...
windres app.rc -O coff -o "%RES%"
if errorlevel 1 (
    echo.
    echo [HATA] windres basarisiz. app.rc / manifest.xml kontrol edin.
    exit /b 1
)

rem MinGW'nin varsayilan "asInvoker" manifesti (default-manifest.o) bizimkiyle
rem carpisiyor ve .rsrc merge hatasi uretiyor. Bos bir nesne ile golgeleyince
rem yalnizca app.rc icindeki requireAdministrator manifesti kalir.
if not exist "%SHIMDIR%\default-manifest.o" (
    echo static int diskunlock_manifest_shim_;> "%TEMP%\dm_shim.c"
    g++ -c "%TEMP%\dm_shim.c" -o "%SHIMDIR%\default-manifest.o"
    if errorlevel 1 (
        echo [HATA] manifest shim nesnesi olusturulamadi.
        exit /b 1
    )
    del "%TEMP%\dm_shim.c" 2>nul
)

echo.
echo [3/4] Baglanti: %OUT%
g++ %LDFLAGS% -B "%SHIMDIR%" -o "%OUT%" !OBJS! "%RES%" %LIBS%
if errorlevel 1 (
    echo.
    echo [HATA] Link basarisiz.
    exit /b 1
)

echo.
echo [4/4] Manifest dogulamasi...
findstr /m /c:"requireAdministrator" "%OUT%" >nul 2>&1
if errorlevel 1 (
    echo [HATA] Beklenen manifest exede bulunamadi! Yonetici istegi calismaz.
    exit /b 1
)
findstr /m /c:"asInvoker" "%OUT%" >nul 2>&1
if not errorlevel 1 (
    echo [HATA] Exede iki manifest var: asInvoker ve requireAdministrator.
    echo        Yonetici istegi Windows loader sirasina bagli kalir. Derlemeyi kontrol edin.
    exit /b 1
)

echo.
echo ============================================================
echo [BASARILI] %OUT% olusturuldu.
echo   Manifest: requireAdministrator + PerMonitorV2 + Common-Controls v6 (tek kopya)
echo ============================================================
exit /b 0
