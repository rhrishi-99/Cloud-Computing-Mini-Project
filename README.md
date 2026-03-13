# Mini-UnionFS

A simplified Union File System in userspace using FUSE 3, written in C.  
Implements layer stacking, Copy-on-Write (CoW), and whiteout-based deletions —  
the same mechanism Docker uses under the hood.

---

## Prerequisites

Linux (Ubuntu 22.04 LTS recommended) with FUSE 3 support.

```bash
sudo apt-get install libfuse3-dev build-essential
```

---

## Build

```bash
make
```

This produces the `./mini_unionfs` binary.

---

## Usage

```
./mini_unionfs <lower_dir> <upper_dir> <mount_point>
```

| Argument | Description |
|---|---|
| `lower_dir` | Read-only base layer (e.g. a container image) |
| `upper_dir` | Read-write layer where all changes are written |
| `mount_point` | Virtual directory that shows the merged view |

### Quick example

```bash
# 1. Create your directories
mkdir -p /tmp/lower /tmp/upper /tmp/mnt

# 2. Seed the lower layer with some files
echo "hello from base" > /tmp/lower/hello.txt
echo "will be deleted" > /tmp/lower/bye.txt

# 3. Mount
./mini_unionfs /tmp/lower /tmp/upper /tmp/mnt

# 4. Use the mount like any normal directory
ls /tmp/mnt                        # shows: hello.txt  bye.txt

cat /tmp/mnt/hello.txt             # hello from base

echo "modified!" >> /tmp/mnt/hello.txt   # triggers Copy-on-Write
cat /tmp/upper/hello.txt           # upper now has a copy with the change
cat /tmp/lower/hello.txt           # lower is untouched

rm /tmp/mnt/bye.txt                # creates /tmp/upper/.wh.bye.txt
ls /tmp/mnt                        # bye.txt is gone from the merged view
ls /tmp/lower                      # bye.txt still exists in the lower layer

# 5. Unmount when done
fusermount3 -u /tmp/mnt
```

---

## How It Works

### Layer Stacking
When you read or list files, Mini-UnionFS merges both directories. If the same filename exists in both layers, the **upper layer wins**.

### Copy-on-Write (CoW)
Writing to a file that only exists in `lower_dir` automatically copies it to `upper_dir` first. The lower layer is **never modified**.

### Whiteout Files
Deleting a file from `lower_dir` through the mount creates a hidden marker file in `upper_dir` named `.wh.<filename>`. The filesystem treats this as "file deleted" and hides it from the merged view.

---

## Run the Test Suite

```bash
chmod +x test_unionfs.sh
./test_unionfs.sh
```

Runs 10 automated tests covering visibility, CoW, whiteout, file creation, readdir merging, and mkdir. Requires the compiled binary to be in the same directory.

---

## Project Files

| File | Description |
|---|---|
| `mini_unionfs.c` | Full source code |
| `Makefile` | Build script |
| `test_unionfs.sh` | Automated test suite |
| `design_document.docx` | 2–3 page design doc (data structures, edge cases) |

---

## Unmounting

```bash
fusermount3 -u <mount_point>

# or on older systems:
fusermount -u <mount_point>
```

Always unmount before deleting the `upper_dir` or `lower_dir`.

---

## Troubleshooting

**`Transport endpoint is not connected`** — the mount crashed without unmounting. Run `fusermount3 -u <mount_point>` to clean up, then remount.

**`fuse: failed to open /dev/fuse`** — you may need to load the fuse kernel module: `sudo modprobe fuse`

**Permission denied on mount** — FUSE requires either root or membership in the `fuse` group: `sudo usermod -aG fuse $USER` (then log out and back in).
