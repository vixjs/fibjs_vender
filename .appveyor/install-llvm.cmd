REM OLD_VER -- https://github.com/llvm/llvm-project/releases/download/llvmorg-%OLD_VER%/LLVM-%OLD_VER%-win64.exe
REM 8.0.1 -- https://github.com/llvm/llvm-project/releases/download/llvmorg-8.0.1/LLVM-8.0.1-win64.exe
REM 9.0.0 -- http://releases.llvm.org/9.0.0/LLVM-9.0.0-win64.exe

if /I "%PLATFORM%" == "" (
    if /I "%PROCESSOR_ARCHITECTURE%" == "AMD64" (
        @set PLATFORM=x64
    ) else (
        @set PLATFORM=x86
    )
)

if NOT EXIST llvm-installer.exe (
    IF /I "%PLATFORM%" == "x64" (
        curl -Ls -o "llvm-installer.exe" "http://releases.llvm.org/9.0.0/LLVM-9.0.0-win64.exe"
        ECHO "install llvm for x64"
    )
    IF /I "%PLATFORM%" == "x86" (
        curl -Ls -o "llvm-installer.exe" "http://releases.llvm.org/9.0.0/LLVM-9.0.0-win32.exe"
        ECHO "install llvm for x86"
    )
)

START /WAIT llvm-installer.exe /S /D=C:\"Program Files\LLVM"
@set PATH="C:\Program Files\LLVM\bin";%PATH%

EXIT