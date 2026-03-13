# ── SETUP ────────────────────────────────────────────────────────
mkdir -p lower upper mnt

# Seed the lower layer (read-only "base image")
echo "base_content"    > lower/base.txt
echo "to_be_deleted"   > lower/delete_me.txt
echo "lower_version"   > lower/overlap.txt
echo "upper_version"   > upper/overlap.txt

echo "=== Lower layer contents ==="
ls lower/
echo ""

# ── MOUNT ────────────────────────────────────────────────────────
./mini_unionfs lower upper mnt
echo "=== Mounted! Merged view ==="
ls mnt/
echo ""

# ── DEMO 1: Layer Visibility ─────────────────────────────────────
echo "=== DEMO 1: Files from lower layer are visible ==="
cat mnt/base.txt          # should print: base_content
echo ""

# ── DEMO 2: Upper takes precedence ───────────────────────────────
echo "=== DEMO 2: Upper layer wins on overlap ==="
cat mnt/overlap.txt       # should print: upper_version
echo ""

# ── DEMO 3: Copy-on-Write ────────────────────────────────────────
echo "=== DEMO 3: Copy-on-Write ==="
echo "modified!" >> mnt/base.txt

echo "  mount view:"
cat mnt/base.txt          # shows both lines

echo "  upper/base.txt (copy created here):"
cat upper/base.txt        # CoW copy with modification

echo "  lower/base.txt (untouched):"
cat lower/base.txt        # still just base_content
echo ""

# ── DEMO 4: Whiteout ─────────────────────────────────────────────
echo "=== DEMO 4: Whiteout (delete a lower-layer file) ==="
rm mnt/delete_me.txt

echo "  ls mnt/ (delete_me.txt should be gone):"
ls mnt/

echo "  lower/delete_me.txt still exists:"
ls lower/delete_me.txt

echo "  whiteout marker created in upper:"
ls -la upper/.wh.delete_me.txt
echo ""

# ── DEMO 5: Create new file ───────────────────────────────────────
echo "=== DEMO 5: New files go to upper layer ==="
echo "brand new" > mnt/newfile.txt
echo "  upper/newfile.txt:"
cat upper/newfile.txt
echo "  lower/ (newfile.txt NOT here):"
ls lower/
echo ""

# ── UNMOUNT ───────────────────────────────────────────────────────
fusermount3 -u mnt
echo "=== Unmounted. Done! ==="

# ── CLEANUP (optional) ────────────────────────────────────────────
# rm -rf lower upper mnt