# Sourced by the Windows build/run scripts: puts the MSYS2 CLANG64 toolchain first on PATH.
# MSYS2_ROOT overrides the default C:/msys64 (with D:/msys64 and F:/msys64 fallbacks).
msys2_root=${MSYS2_ROOT:-/c/msys64}
if [[ ! -d $msys2_root/clang64/bin ]]; then
    if [[ -d C:/msys64/clang64/bin ]]; then
        msys2_root=C:/msys64
    elif [[ -d /d/msys64/clang64/bin ]]; then
        msys2_root=/d/msys64
    elif [[ -d D:/msys64/clang64/bin ]]; then
        msys2_root=D:/msys64
    elif [[ -d /f/msys64/clang64/bin ]]; then
        msys2_root=/f/msys64
    elif [[ -d F:/msys64/clang64/bin ]]; then
        msys2_root=F:/msys64
    fi
fi
if [[ -d $msys2_root/clang64/bin ]]; then
    export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH"
    export MSYSTEM=CLANG64
fi
