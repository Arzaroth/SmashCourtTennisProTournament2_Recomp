#!/usr/bin/env bash
# Runs ps2_analyzer on the ELF into build/analyzer.toml, the starting point of recomp/sct2.toml.
# recomp/sct2.toml is edited by hand afterwards: merge new analyzer output into it, never overwrite it.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" ] || "$ROOT/scripts/build_tools.sh"
mkdir -p "$ROOT/build"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" "$ELF" "$ROOT/build/analyzer.toml"
echo "Analyzer config in $ROOT/build/analyzer.toml"
