#!/usr/bin/env bash
# Builds CharacterDetailCull.dll with clang-cl + lld-link, no Windows SDK or CRT needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
{ printf 'LIBRARY kernel32.dll\nEXPORTS\n'; printf '%s\n' GetModuleHandleA GetModuleHandleExA GetModuleFileNameA GetProcAddress \
  VirtualProtect VirtualAlloc FlushInstructionCache GetCurrentProcess GetEnvironmentVariableA CreateFileA \
  ReadFile WriteFile CloseHandle GetTickCount64 GetLocalTime; } > build/kernel32.def
llvm-dlltool -m i386:x86-64 -d build/kernel32.def -l build/kernel32.lib
printf 'LIBRARY user32.dll\nEXPORTS\nGetAsyncKeyState\n' > build/user32.def
llvm-dlltool -m i386:x86-64 -d build/user32.def -l build/user32.lib
clang-cl --target=x86_64-pc-windows-msvc /O2 /GS- /Zl /EHs-c- /GR- /c detailcull.cpp /Fobuild/detailcull.obj
lld-link /dll /noentry /nodefaultlib /machine:x64 /out:build/CharacterDetailCull.dll build/detailcull.obj build/kernel32.lib build/user32.lib
echo "built build/CharacterDetailCull.dll"
