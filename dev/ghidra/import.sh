#!/bin/bash
# Import TFX.DAT into a headless Ghidra project (work/ghidra/TFX) with the LE
# objects at the runtime's load addresses, and run auto-analysis (~1 min).
# Needs toolchain/ghidra_12.0.1_PUBLIC (+ the ghidra-lx-loader extension in
# Ghidra/Extensions) and toolchain/jdk-21.0.13+9; see PORTING_PLAN.md.
cd "$(dirname "$0")/../.."
R=$(cygpath -w "$(pwd)")
JAVA="toolchain/jdk-21.0.13+9/bin/java.exe"
mkdir -p work/ghidra
# The loader's base-address option isn't exposed to headless mode; it reads its
# default from Java preferences, so set that (same as the GUI import dialog).
"$JAVA" "$(cygpath -w dev/ghidra/SetLxPrefs.java)" "0x400000,0x480000,0x490000"
cat > work/ghidra/import.bat <<BAT
@echo off
set JAVA_HOME=$R\toolchain\jdk-21.0.13+9
call "$R\toolchain\ghidra_12.0.1_PUBLIC\support\analyzeHeadless.bat" "$R\work\ghidra" TFX -import "$R\build\tfx\game\TFX.DAT" -overwrite -loader LeLoader -max-cpu 8 < NUL
BAT
cmd //c "$(cygpath -w work/ghidra/import.bat)" > work/ghidra/import.log 2>&1
grep -E "Mapped|REPORT: (Import|Analysis)" work/ghidra/import.log
