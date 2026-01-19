# ABOUTME: Runs the ARM64 patch demo and checks the expected output lines.
# ABOUTME: Used as a minimal regression test for the runtime patching flow.
#!/usr/bin/env sh
set -eu

UNAME_S=$(uname -s)
UNAME_M=$(uname -m)

if [ "$UNAME_S" != "Darwin" ] || [ "$UNAME_M" != "arm64" ]; then
    echo "skip: requires macOS arm64"
    exit 0
fi

OUTPUT=$(./arm64-patching/patch_demo 2>&1 || true)

echo "$OUTPUT" | grep -q "\\[demo\\] calling target_function before patch"
echo "$OUTPUT" | grep -q "\\[demo\\] patching prologue"
echo "$OUTPUT" | grep -q "\\[demo\\] calling target_function after patch"
echo "$OUTPUT" | grep -q "\\[target\\] result=1337"
