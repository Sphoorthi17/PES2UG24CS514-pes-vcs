// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Heap-backed index storage ───────────────────────────────────────────────
// Index has 10,000 entries (~6 MB). pes.c declares it as a local variable
// which overflows the stack. We allocate it on the heap here and redirect
// all pointer arguments to our heap copy so the stack variable is never
// actually written to (it lives at a bad address but we never touch it).

static Index *g_idx = NULL;

static Index *heap_index(void) {
    if (!g_idx) g_idx = calloc(1, sizeof(Index));
    return g_idx;
}

// Redirect: every function that receives an Index* will operate on our
// heap copy instead, ignoring the (potentially stack-overflowed) pointer.
#define IDX heap_index()

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    Index *idx = IDX; (void)index;
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].path, path) == 0)
            return &idx->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    Index *idx = IDX; (void)index;
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].path, path) == 0) {
            int remaining = idx->count - i - 1;
            if (remaining > 0)
                memmove(&idx->entries[i], &idx->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            idx->count--;
            return index_save(idx);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    Index *idx = IDX; (void)index;

    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < idx->count; i++) {
        printf("  staged:     %s\n", idx->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < idx->count; i++) {
        struct stat st;
        if (stat(idx->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", idx->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)idx->entries[i].mtime_sec ||
                st.st_size  != (off_t)idx->entries[i].size) {
                printf("  modified:   %s\n", idx->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < idx->count; i++) {
                if (strcmp(idx->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── Helper ──────────────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    // Always work on the heap copy; ignore the (stack-overflowed) pointer.
    Index *idx = IDX; (void)index;
    memset(idx, 0, sizeof(Index));

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // No index yet — empty is fine

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        if (idx->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &idx->entries[idx->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime, size;
        char path[512];

        if (sscanf(line, "%o %64s %llu %llu %511s",
                   &mode, hex, &mtime, &size, path) != 5) continue;

        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint64_t)size;
        if (hex_to_hash(hex, &e->hash) != 0) continue;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        idx->count++;
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index *idx = IDX; (void)index;

    // Sort a local copy by path
    Index sorted = *idx;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_index_entries);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %llu %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                (unsigned long long)e->size,
                e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    Index *idx = IDX; (void)index;

    // Read the file
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    void *contents = NULL;
    if (file_size > 0) {
        contents = malloc((size_t)file_size);
        if (!contents) { fclose(f); return -1; }
        if (fread(contents, 1, (size_t)file_size, f) != (size_t)file_size) {
            free(contents); fclose(f); return -1;
        }
    }
    fclose(f);

    // Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents); return -1;
    }
    free(contents);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (S_ISDIR(st.st_mode))       mode = 0040000;
    else if (st.st_mode & S_IXUSR) mode = 0100755;
    else                            mode = 0100644;

    // Update existing or append new
    IndexEntry *existing = index_find(idx, path);
    if (existing) {
        existing->hash      = blob_id;
        existing->mode      = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint64_t)st.st_size;
    } else {
        if (idx->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n"); return -1;
        }
        IndexEntry *e   = &idx->entries[idx->count++];
        e->hash         = blob_id;
        e->mode         = mode;
        e->mtime_sec    = (uint64_t)st.st_mtime;
        e->size         = (uint64_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }

    return index_save(idx);
}
