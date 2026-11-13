#!/bin/bash
# usage: dev/ghidra/decompile.sh OUTFILE ADDR...   (or OUTFILE all)
# Decompiles functions of the imported TFX.DAT Ghidra project (work/ghidra, made by
# dev/ghidra/import.sh) to text.  SCRIPT=RefsTo.java OUTFILE LO-HI... lists references.
cd "$(dirname "$0")/../.."
R=$(cygpath -w "$(pwd)")
OUT=$(cygpath -w "$(realpath -m "$1")"); shift
cat > work/ghidra/decompile.bat <<BAT
@echo off
set JAVA_HOME=$R\toolchain\jdk-21.0.13+9
call "$R\toolchain\ghidra_12.0.1_PUBLIC\support\analyzeHeadless.bat" "$R\work\ghidra" TFX -process ${PROG:-TFX.DAT} -noanalysis -readOnly -scriptPath "$R\dev\ghidra" -postScript ${SCRIPT:-DecompileTo.java} "$OUT" $* < NUL
BAT
cmd //c "$(cygpath -w work/ghidra/decompile.bat)" > work/ghidra/decompile.log 2>&1
grep -E "ERROR|Exception|SCRIPT" work/ghidra/decompile.log | head -5
