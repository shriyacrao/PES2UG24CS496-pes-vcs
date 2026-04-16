// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include <time.h>
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
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
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
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

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    // TODO: Implement index loading
    // (See Lab Appendix for logical steps)
    FILE *fp = fopen(".pes/index", "r");
    if (!fp) {
        index->count = 0;
        return 0;
    }

    index->count = 0;

    while (1) {
        IndexEntry e;
        char hex[65];

        int scanned = fscanf(fp, "%o %64s %lu %u %511s",
                             &e.mode,
                             hex,
                             &e.mtime_sec,
                             &e.size,
                             e.path);

        if (scanned != 5) break;

        if (hex_to_hash(hex, &e.hash) != 0) continue;

        index->entries[index->count++] = e;
    }

    fclose(fp);
    return 0;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // TODO: Implement atomic index saving
    // (See Lab Appendix for logical steps)
    if (!index) return -1;

    FILE *fp = fopen(".pes/index.tmp", "w");
    if (!fp) return -1;

    Index *copy = malloc(sizeof(Index));
    if (!copy) {
        fclose(fp);
        return -1;
    }
    *copy = *index;

    for (int i = 0; i < copy->count - 1; i++) {
        for (int j = i + 1; j < copy->count; j++) {
            if (strcmp(copy->entries[i].path, copy->entries[j].path) > 0) {
                IndexEntry tmp = copy->entries[i];
                copy->entries[i] = copy->entries[j];
                copy->entries[j] = tmp;
            }
        }
    }

    for (int i = 0; i < copy->count; i++) {
        char hex[65];
        hash_to_hex(&copy->entries[i].hash, hex);

        fprintf(fp, "%o %s %lu %u %s\n",
                copy->entries[i].mode,
                hex,
                copy->entries[i].mtime_sec,
                copy->entries[i].size,
                copy->entries[i].path);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    free(copy);

    if (rename(".pes/index.tmp", ".pes/index") != 0) {
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // TODO: Implement file staging
    // (See Lab Appendix for logical steps)
    if (!index || !path) return -1;
    if (index->count < 0 || index->count >= MAX_INDEX_ENTRIES) return -1; 
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    void *data = malloc(size);
    if (!data) {
        fclose(fp);
        return -1;
    }

   if (fread(data, 1, size, fp) != (size_t)size) {
       free(data);
       fclose(fp);
       return -1;
    }        
    fclose(fp);

    ObjectID hash;
    if (object_write(OBJ_BLOB, data, size, &hash) != 0) {
        free(data);
        return -1;
    }

    free(data);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *existing = index_find(index, path);

if (existing) {
    existing->hash = hash;
    existing->mtime_sec = st.st_mtime;
    existing->size = st.st_size;
    existing->mode = 0100644;
    return index_save(index);
}

IndexEntry e;
e.hash = hash;
e.mode = 0100644;

e.mtime_sec = st.st_mtime;
e.size = st.st_size;
snprintf(e.path, sizeof(e.path), "%s", path);

    index->entries[index->count++] = e;

    return index_save(index);
}
