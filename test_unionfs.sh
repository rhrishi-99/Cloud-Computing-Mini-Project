#!/bin/bash
# test_unionfs.sh – Automated test suite for Mini-UnionFS
# Place this file alongside the compiled binary and run:
#   chmod +x test_unionfs.sh && ./test_unionfs.sh

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "${GREEN}PASSED${NC}"; ((PASS++)); }
fail() { echo -e "${RED}FAILED${NC}"; ((FAIL++)); }

echo "============================================"
echo "  Mini-UnionFS Test Suite"
echo "============================================"

# ── Setup ────────────────────────────────────────
rm -rf "$TEST_DIR"
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

echo "base_only_content"  > "$LOWER_DIR/base.txt"
echo "to_be_deleted"      > "$LOWER_DIR/delete_me.txt"
echo "upper_file_content" > "$UPPER_DIR/upper_only.txt"
echo "lower_version"      > "$LOWER_DIR/overlap.txt"
echo "upper_version"      > "$UPPER_DIR/overlap.txt"

# Mount
"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"
if [ $? -ne 0 ]; then
    echo -e "${RED}FATAL: Failed to mount Mini-UnionFS${NC}"
    exit 1
fi
sleep 1

# ── Test 1: Lower layer visibility ───────────────
echo -n "Test 1: Lower layer visibility ... "
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then pass; else fail; fi

# ── Test 2: Upper layer visibility ───────────────
echo -n "Test 2: Upper layer visibility ... "
if grep -q "upper_file_content" "$MOUNT_DIR/upper_only.txt" 2>/dev/null; then pass; else fail; fi

# ── Test 3: Upper takes precedence over lower ────
echo -n "Test 3: Upper layer precedence  ... "
if grep -q "upper_version" "$MOUNT_DIR/overlap.txt" 2>/dev/null; then pass; else fail; fi

# ── Test 4: Copy-on-Write ────────────────────────
echo -n "Test 4: Copy-on-Write           ... "
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
cow_mount=$(grep -c "modified_content" "$MOUNT_DIR/base.txt"   2>/dev/null || echo 0)
cow_upper=$(grep -c "modified_content" "$UPPER_DIR/base.txt"   2>/dev/null || echo 0)
cow_lower=$(grep -c "modified_content" "$LOWER_DIR/base.txt"   2>/dev/null || echo 0)
if [ "$cow_mount" -ge 1 ] && [ "$cow_upper" -ge 1 ] && [ "$cow_lower" -eq 0 ]; then pass; else fail; fi

# ── Test 5: Lower file untouched after CoW ───────
echo -n "Test 5: Lower file untouched    ... "
if grep -q "base_only_content" "$LOWER_DIR/base.txt" 2>/dev/null; then pass; else fail; fi

# ── Test 6: Whiteout mechanism ───────────────────
echo -n "Test 6: Whiteout mechanism      ... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
wh_hidden=$([ ! -f "$MOUNT_DIR/delete_me.txt" ] && echo 1 || echo 0)
wh_lower_intact=$([ -f "$LOWER_DIR/delete_me.txt" ] && echo 1 || echo 0)
wh_file_created=$([ -f "$UPPER_DIR/.wh.delete_me.txt" ] && echo 1 || echo 0)
if [ "$wh_hidden" -eq 1 ] && [ "$wh_lower_intact" -eq 1 ] && [ "$wh_file_created" -eq 1 ]; then
    pass
else
    fail
fi

# ── Test 7: Create new file in mount ─────────────
echo -n "Test 7: Create new file         ... "
echo "new_content" > "$MOUNT_DIR/new_file.txt" 2>/dev/null
if grep -q "new_content" "$UPPER_DIR/new_file.txt" 2>/dev/null; then pass; else fail; fi

# ── Test 8: readdir shows merged view ────────────
echo -n "Test 8: readdir merged view     ... "
ls_out=$(ls "$MOUNT_DIR" 2>/dev/null)
if echo "$ls_out" | grep -q "base.txt" && echo "$ls_out" | grep -q "upper_only.txt"; then pass; else fail; fi

# ── Test 9: Whiteout'd file not in readdir ───────
echo -n "Test 9: Whiteout'd file hidden  ... "
if ! echo "$ls_out" | grep -q "delete_me.txt"; then pass; else fail; fi

# ── Test 10: mkdir creates dir in upper ──────────
echo -n "Test 10: mkdir in upper layer   ... "
mkdir "$MOUNT_DIR/newdir" 2>/dev/null
if [ -d "$UPPER_DIR/newdir" ]; then pass; else fail; fi

# ── Teardown ─────────────────────────────────────
fusermount3 -u "$MOUNT_DIR" 2>/dev/null || \
    fusermount  -u "$MOUNT_DIR" 2>/dev/null || \
    umount "$MOUNT_DIR" 2>/dev/null

rm -rf "$TEST_DIR"

echo "============================================"
echo -e "Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}"
echo "============================================"
[ "$FAIL" -eq 0 ]
