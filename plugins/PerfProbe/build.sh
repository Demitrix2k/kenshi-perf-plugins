#!/usr/bin/env bash
# Builds PerfProbe.dll with clang-cl + lld-link, no Windows SDK or CRT needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cat > build/kernel32.def <<'DEF'
LIBRARY kernel32.dll
EXPORTS
GetModuleHandleA
GetProcAddress
VirtualProtect
VirtualAlloc
FlushInstructionCache
GetCurrentProcess
GetEnvironmentVariableA
CreateFileA
WriteFile
QueryPerformanceCounter
QueryPerformanceFrequency
DEF
llvm-dlltool -m i386:x86-64 -d build/kernel32.def -l build/kernel32.lib
clang-cl --target=x86_64-pc-windows-msvc /O2 /GS- /Zl /EHs-c- /GR- /c perfprobe.cpp /Fobuild/perfprobe.obj
lld-link /dll /noentry /nodefaultlib /machine:x64 /out:build/PerfProbe.dll build/perfprobe.obj build/kernel32.lib
echo "built build/PerfProbe.dll"
