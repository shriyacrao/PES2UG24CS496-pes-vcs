// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── HELPERS ────────────────────────────────────────────────────────────────

static int path_has_prefix(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0;
}

static int build_tree_for_prefix(const Index *index, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const char *full = index->entries[i].path;

        if (!path_has_prefix(full, prefix))
            continue;

        const char *rest = full + prefix_len;
        if (*rest == '\0')
            continue;

        const char *slash = strchr(rest, '/');

        if (slash == NULL) {
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = index->entries[i].mode;
            e->hash = index->entries[i].hash;
            snprintf(e->name, sizeof(e->name), "%s", rest);
        } else {
            size_t dir_len = slash - rest;
            char dir_name[256];

            if (dir_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rest, dir_len);
            dir_name[dir_len] = '\0';

            int already_added = 0;
            for (int j = 0; j < tree.count; j++) {
                if (tree.entries[j].mode == MODE_DIR &&
                    strcmp(tree.entries[j].name, dir_name) == 0) {
                    already_added = 1;
                    break;
                }
            }
            if (already_added) continue;

            char child_prefix[512];
            snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, dir_name);

            ObjectID child_id;
            if (build_tree_for_prefix(index, child_prefix, &child_id) != 0)
                return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            e->hash = child_id;
            snprintf(e->name, sizeof(e->name), "%s", dir_name);
        }
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0)
        return -1;

    if (object_write(OBJ_TREE, data, len, id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

static int load_index_local(Index *index) {
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

// ─── FINAL FUNCTION ─────────────────────────────────────────────────────────

int tree_from_index(ObjectID *id_out) {
    if (!id_out) return -1;

    Index index;
    if (load_index_local(&index) != 0)
        return -1;

    return build_tree_for_prefix(&index, "", id_out);
}
