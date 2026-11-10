#!/usr/bin/env bash
# Builds MyGUIQuadFix.dll with clang-cl + lld-link, no Windows SDK or CRT needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cat > build/kernel32.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetModuleHandleA
VirtualProtect
FlushInstructionCache
GetCurrentProcess
GetEnvironmentVariableA
CreateFileA
WriteFile
CloseHandle
GetLocalTime
GetFileAttributesExA
ReadFile
CreateThread
Sleep
EOF
llvm-dlltool -m i386:x86-64 -d build/kernel32.def -l build/kernel32.lib
clang-cl --target=x86_64-pc-windows-msvc /O2 /GS- /Zl /EHs-c- /GR- /c quadfix.cpp /Fobuild/quadfix.obj
lld-link /dll /noentry /nodefaultlib /machine:x64 /out:build/MyGUIQuadFix.dll build/quadfix.obj build/kernel32.lib
echo "built build/MyGUIQuadFix.dll"
