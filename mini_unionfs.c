/*
 * Mini-UnionFS: A simplified Union File System using FUSE
 *
 * Implements:
 *   - Layer stacking (lower_dir read-only, upper_dir read-write)
 *   - Copy-on-Write (CoW) for modifications to lower layer files
 *   - Whiteout files for deletions of lower layer files
 *   - Basic POSIX ops: getattr, readdir, read, write, create, unlink, mkdir, rmdir
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

/* -------------------------------------------------------------------------
 * Global State
 * ------------------------------------------------------------------------- */

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *)fuse_get_context()->private_data)

/* Whiteout prefix used to mark deleted lower-layer files */
#define WHITEOUT_PREFIX ".wh."

/* -------------------------------------------------------------------------
 * Path Helpers
 * ------------------------------------------------------------------------- */

/**
 * Build a full path by joining base_dir and relative path.
 * out must be at least PATH_MAX bytes.
 */
static void build_path(char *out, const char *base_dir, const char *rel_path)
{
    snprintf(out, PATH_MAX, "%s%s", base_dir, rel_path);
}

/**
 * Given a file path like "/foo/bar.txt", produce the whiteout path in
 * upper_dir: "<upper_dir>/foo/.wh.bar.txt"
 */
static void build_whiteout_path(char *out, const char *upper_dir, const char *rel_path)
{
    /* Split rel_path into directory and filename components */
    char dir_part[PATH_MAX];
    char base_part[PATH_MAX];

    strncpy(dir_part,  rel_path, PATH_MAX - 1);
    strncpy(base_part, rel_path, PATH_MAX - 1);
    dir_part[PATH_MAX - 1]  = '\0';
    base_part[PATH_MAX - 1] = '\0';

    /* Find last slash */
    char *last_slash = strrchr(dir_part, '/');
    if (last_slash == NULL || last_slash == dir_part) {
        /* File is in root */
        snprintf(out, PATH_MAX, "%s/" WHITEOUT_PREFIX "%s",
                 upper_dir, base_part + 1); /* skip leading '/' */
    } else {
        char *filename = strrchr(base_part, '/') + 1;
        *last_slash = '\0'; /* truncate dir_part at last slash */
        snprintf(out, PATH_MAX, "%s%s/" WHITEOUT_PREFIX "%s",
                 upper_dir, dir_part, filename);
    }
}

/**
 * Check whether a whiteout file exists for the given rel_path.
 * Returns 1 if whiteout exists, 0 otherwise.
 */
static int whiteout_exists(const char *upper_dir, const char *rel_path)
{
    char wh_path[PATH_MAX];
    build_whiteout_path(wh_path, upper_dir, rel_path);
    struct stat st;
    return (lstat(wh_path, &st) == 0) ? 1 : 0;
}

/**
 * Resolve a virtual path to a real filesystem path.
 *
 * Resolution order:
 *   1. If a whiteout file exists in upper_dir -> the file is deleted, return -ENOENT
 *   2. If file exists in upper_dir             -> return upper path
 *   3. If file exists in lower_dir             -> return lower path
 *   4. Otherwise                               -> return -ENOENT
 *
 * On success writes the resolved absolute path to `resolved` and returns 0.
 */
static int resolve_path(const char *rel_path, char *resolved)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    struct stat st;

    build_path(upper_path, state->upper_dir, rel_path);
    build_path(lower_path, state->lower_dir, rel_path);

    /* Step 1: Check whiteout */
    if (whiteout_exists(state->upper_dir, rel_path)) {
        return -ENOENT;
    }

    /* Step 2: Prefer upper layer */
    if (lstat(upper_path, &st) == 0) {
        strncpy(resolved, upper_path, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return 0;
    }

    /* Step 3: Fall back to lower layer */
    if (lstat(lower_path, &st) == 0) {
        strncpy(resolved, lower_path, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        return 0;
    }

    return -ENOENT;
}

/**
 * Copy a file from lower_dir to upper_dir (Copy-on-Write trigger).
 * Creates any necessary parent directories in upper_dir.
 * Returns 0 on success, -errno on failure.
 */
static int cow_copy(const char *rel_path)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char lower_path[PATH_MAX];
    char upper_path[PATH_MAX];

    build_path(lower_path, state->lower_dir, rel_path);
    build_path(upper_path, state->upper_dir, rel_path);

    /* Ensure parent directory exists in upper layer */
    char parent[PATH_MAX];
    strncpy(parent, upper_path, PATH_MAX - 1);
    parent[PATH_MAX - 1] = '\0';
    char *last = strrchr(parent, '/');
    if (last && last != parent) {
        *last = '\0';
        /* mkdir -p equivalent */
        char tmp[PATH_MAX];
        snprintf(tmp, PATH_MAX, "%s", parent);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }

    /* Open source and destination */
    int src_fd = open(lower_path, O_RDONLY);
    if (src_fd < 0) return -errno;

    struct stat st;
    if (fstat(src_fd, &st) < 0) { close(src_fd); return -errno; }

    int dst_fd = open(upper_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd < 0) { close(src_fd); return -errno; }

    /* Copy data */
    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, n) != n) {
            close(src_fd); close(dst_fd);
            return -EIO;
        }
    }

    close(src_fd);
    close(dst_fd);
    return (n < 0) ? -errno : 0;
}

/* -------------------------------------------------------------------------
 * FUSE Operations
 * ------------------------------------------------------------------------- */

static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void)fi;
    char resolved[PATH_MAX];
    int res = resolve_path(path, resolved);
    if (res != 0) return res;

    if (lstat(resolved, stbuf) < 0)
        return -errno;
    return 0;
}

static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    struct dirent *de;
    DIR *dp;

    build_path(upper_path, state->upper_dir, path);
    build_path(lower_path, state->lower_dir, path);

    /* We use a simple seen[] approach: collect names from upper first,
     * then add names from lower that aren't whiteout'd and not already seen. */
    #define MAX_ENTRIES 4096
    char *seen[MAX_ENTRIES];
    int   seen_count = 0;

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* --- Pass 1: Upper layer --- */
    dp = opendir(upper_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            const char *name = de->d_name;

            /* Skip . and .. */
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            /* Skip whiteout files themselves – they are internal bookkeeping */
            if (strncmp(name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0)
                continue;

            struct stat st;
            char full[PATH_MAX];
            snprintf(full, PATH_MAX, "%s/%s", upper_path, name);
            lstat(full, &st);
            filler(buf, name, &st, 0, 0);

            if (seen_count < MAX_ENTRIES)
                seen[seen_count++] = strdup(name);
        }
        closedir(dp);
    }

    /* --- Pass 2: Lower layer --- */
    dp = opendir(lower_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            const char *name = de->d_name;

            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

            /* Build the virtual path for whiteout check */
            char vpath[PATH_MAX];
            if (strcmp(path, "/") == 0)
                snprintf(vpath, PATH_MAX, "/%s", name);
            else
                snprintf(vpath, PATH_MAX, "%s/%s", path, name);

            /* Skip if whiteout'd */
            if (whiteout_exists(state->upper_dir, vpath))
                continue;

            /* Skip if already emitted from upper layer */
            int already_seen = 0;
            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], name) == 0) { already_seen = 1; break; }
            }
            if (already_seen) continue;

            struct stat st;
            char full[PATH_MAX];
            snprintf(full, PATH_MAX, "%s/%s", lower_path, name);
            lstat(full, &st);
            filler(buf, name, &st, 0, 0);
        }
        closedir(dp);
    }

    /* Free seen list */
    for (int i = 0; i < seen_count; i++) free(seen[i]);

    return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char resolved[PATH_MAX];
    char upper_path[PATH_MAX];

    int res = resolve_path(path, resolved);
    if (res != 0) return res;

    build_path(upper_path, state->upper_dir, path);

    /* If the user is opening for writing and the file lives in lower_dir,
     * trigger Copy-on-Write: copy the file to upper_dir first. */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (strncmp(resolved, state->upper_dir, strlen(state->upper_dir)) != 0) {
            /* File is in lower_dir – copy it to upper_dir */
            int cow_res = cow_copy(path);
            if (cow_res != 0) return cow_res;
        }
    }

    /* Verify the target is actually openable */
    char target[PATH_MAX];
    if (lstat(upper_path, &(struct stat){}) == 0)
        strncpy(target, upper_path, PATH_MAX - 1);
    else
        strncpy(target, resolved,   PATH_MAX - 1);
    target[PATH_MAX - 1] = '\0';

    int fd = open(target, fi->flags);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int unionfs_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
    (void)fi;
    char resolved[PATH_MAX];
    int res = resolve_path(path, resolved);
    if (res != 0) return res;

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) return -errno;

    ssize_t n = pread(fd, buf, size, offset);
    close(fd);
    if (n < 0) return -errno;
    return (int)n;
}

static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_path(upper_path, state->upper_dir, path);

    /* CoW: if file doesn't yet exist in upper_dir, copy it from lower */
    struct stat st;
    if (lstat(upper_path, &st) != 0) {
        char lower_path[PATH_MAX];
        build_path(lower_path, state->lower_dir, path);
        if (lstat(lower_path, &st) == 0) {
            int cow_res = cow_copy(path);
            if (cow_res != 0) return cow_res;
        }
    }

    int fd = open(upper_path, O_WRONLY);
    if (fd < 0) return -errno;

    ssize_t n = pwrite(fd, buf, size, offset);
    close(fd);
    if (n < 0) return -errno;
    return (int)n;
}

static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    (void)fi;
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_path(upper_path, state->upper_dir, path);

    /* Remove any existing whiteout for this path */
    char wh_path[PATH_MAX];
    build_whiteout_path(wh_path, state->upper_dir, path);
    unlink(wh_path);

    int fd = open(upper_path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int unionfs_unlink(const char *path)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    struct stat st;

    build_path(upper_path, state->upper_dir, path);
    build_path(lower_path, state->lower_dir, path);

    /* Case 1: File exists in upper_dir -> physically remove it */
    if (lstat(upper_path, &st) == 0) {
        if (unlink(upper_path) < 0) return -errno;
    }

    /* Case 2: File exists in lower_dir -> create a whiteout in upper_dir */
    if (lstat(lower_path, &st) == 0) {
        char wh_path[PATH_MAX];
        build_whiteout_path(wh_path, state->upper_dir, path);

        /* Ensure parent directory exists */
        char parent[PATH_MAX];
        strncpy(parent, wh_path, PATH_MAX - 1);
        parent[PATH_MAX - 1] = '\0';
        char *last = strrchr(parent, '/');
        if (last && last != parent) {
            *last = '\0';
            mkdir(parent, 0755);
        }

        int fd = open(wh_path, O_CREAT | O_WRONLY, 0000);
        if (fd < 0) return -errno;
        close(fd);
    }

    return 0;
}

static int unionfs_mkdir(const char *path, mode_t mode)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_path(upper_path, state->upper_dir, path);
    if (mkdir(upper_path, mode) < 0) return -errno;
    return 0;
}

static int unionfs_rmdir(const char *path)
{
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    struct stat st;

    build_path(upper_path, state->upper_dir, path);
    build_path(lower_path, state->lower_dir, path);

    if (lstat(upper_path, &st) == 0) {
        if (rmdir(upper_path) < 0) return -errno;
    }

    /* Create whiteout for directory if it exists in lower */
    if (lstat(lower_path, &st) == 0) {
        char wh_path[PATH_MAX];
        build_whiteout_path(wh_path, state->upper_dir, path);
        int fd = open(wh_path, O_CREAT | O_WRONLY, 0000);
        if (fd < 0) return -errno;
        close(fd);
    }

    return 0;
}

static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi)
{
    (void)fi;
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_path(upper_path, state->upper_dir, path);

    /* CoW if needed */
    struct stat st;
    if (lstat(upper_path, &st) != 0) {
        char lower_path[PATH_MAX];
        build_path(lower_path, state->lower_dir, path);
        if (lstat(lower_path, &st) == 0) {
            int cow_res = cow_copy(path);
            if (cow_res != 0) return cow_res;
        }
    }

    if (truncate(upper_path, size) < 0) return -errno;
    return 0;
}

static int unionfs_chmod(const char *path, mode_t mode,
                         struct fuse_file_info *fi)
{
    (void)fi;
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_path(upper_path, state->upper_dir, path);

    /* CoW if file only exists in lower */
    struct stat st;
    if (lstat(upper_path, &st) != 0) {
        int cow_res = cow_copy(path);
        if (cow_res != 0) return cow_res;
    }

    if (chmod(upper_path, mode) < 0) return -errno;
    return 0;
}

static int unionfs_utimens(const char *path, const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    (void)fi;
    char resolved[PATH_MAX];
    int res = resolve_path(path, resolved);
    if (res != 0) return res;

    if (utimensat(AT_FDCWD, resolved, tv, AT_SYMLINK_NOFOLLOW) < 0)
        return -errno;
    return 0;
}

/* -------------------------------------------------------------------------
 * FUSE Operations Table
 * ------------------------------------------------------------------------- */

static struct fuse_operations unionfs_oper = {
    .getattr  = unionfs_getattr,
    .readdir  = unionfs_readdir,
    .open     = unionfs_open,
    .read     = unionfs_read,
    .write    = unionfs_write,
    .create   = unionfs_create,
    .unlink   = unionfs_unlink,
    .mkdir    = unionfs_mkdir,
    .rmdir    = unionfs_rmdir,
    .truncate = unionfs_truncate,
    .chmod    = unionfs_chmod,
    .utimens  = unionfs_utimens,
};

/* -------------------------------------------------------------------------
 * Entry Point
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <lower_dir> <upper_dir> <mount_point> [fuse options]\n",
            argv[0]);
        return 1;
    }

    struct mini_unionfs_state *state = calloc(1, sizeof(*state));
    if (!state) {
        perror("calloc");
        return 1;
    }

    /* Resolve to absolute paths */
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: could not resolve lower or upper directory\n");
        free(state);
        return 1;
    }

    fprintf(stderr, "Mini-UnionFS starting\n");
    fprintf(stderr, "  lower_dir : %s\n", state->lower_dir);
    fprintf(stderr, "  upper_dir : %s\n", state->upper_dir);
    fprintf(stderr, "  mount     : %s\n", argv[3]);

    /*
     * Rebuild argv for fuse_main:
     *   argv[0] = program name
     *   argv[1] = mount point
     *   argv[2..] = any extra fuse options
     */
    int fuse_argc = argc - 2;
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv) { perror("malloc"); return 1; }

    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3]; /* mount point */
    for (int i = 4; i < argc; i++)
        fuse_argv[i - 2] = argv[i];

    return fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);
}
