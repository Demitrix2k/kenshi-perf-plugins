#!/usr/bin/env bash
# Builds AnimLOD.dll with clang-cl + lld-link, no Windows SDK or CRT needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cat > build/kernel32.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetModuleHandleA
GetProcAddress
VirtualProtect
FlushInstructionCache
GetCurrentProcess
GetEnvironmentVariableA
CreateFileA
WriteFile
QueryPerformanceCounter
QueryPerformanceFrequency
GetTickCount64
GetLocalTime
EOF
llvm-dlltool -m i386:x86-64 -d build/kernel32.def -l build/kernel32.lib
printf 'LIBRARY user32.dll\nEXPORTS\nGetAsyncKeyState\n' > build/user32.def
llvm-dlltool -m i386:x86-64 -d build/user32.def -l build/user32.lib
clang-cl --target=x86_64-pc-windows-msvc /O2 /GS- /Zl /EHs-c- /GR- /c animlod.cpp /Fobuild/animlod.obj
lld-link /dll /noentry /nodefaultlib /machine:x64 /out:build/AnimLOD.dll build/animlod.obj build/kernel32.lib build/user32.lib
echo "built build/AnimLOD.dll"
