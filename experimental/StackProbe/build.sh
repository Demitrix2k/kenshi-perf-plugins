#!/usr/bin/env bash
# Builds StackProbe.dll with clang-cl + lld-link, no Windows SDK or CRT needed.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
cat > build/kernel32.def <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetCurrentProcess
GetCurrentThreadId
GetCurrentThread
OpenThread
SuspendThread
ResumeThread
GetThreadContext
K32EnumProcessModules
GetModuleFileNameA
GetTickCount64
SetThreadPriority
GetLocalTime
CreateFileA
WriteFile
GetFileAttributesExA
ReadFile
CloseHandle
CreateThread
Sleep
EOF
cat > build/ntdll.def <<'EOF'
LIBRARY ntdll.dll
EXPORTS
NtQueryInformationThread
EOF
llvm-dlltool -m i386:x86-64 -d build/kernel32.def -l build/kernel32.lib
llvm-dlltool -m i386:x86-64 -d build/ntdll.def -l build/ntdll.lib
clang-cl --target=x86_64-pc-windows-msvc /O2 /GS- /Zl /EHs-c- /GR- /c stackprobe.cpp /Fobuild/stackprobe.obj
lld-link /dll /noentry /nodefaultlib /machine:x64 /out:build/StackProbe.dll build/stackprobe.obj build/kernel32.lib build/ntdll.lib
echo "built build/StackProbe.dll"
