// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // TODO: Implement
    const char *type_str = NULL;
    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0) return -1;
    header[header_len++] = '\0';

    size_t total_len = (size_t)header_len + len;
    uint8_t *full = malloc(total_len);
    if (!full) return -1;

    memcpy(full, header, (size_t)header_len);
    memcpy(full + header_len, data, len);

    compute_hash(full, total_len, id_out);

    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    if (mkdir(OBJECTS_DIR, 0755) < 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    char shard_dir[512];
    strncpy(shard_dir, final_path, sizeof(shard_dir));
    shard_dir[sizeof(shard_dir) - 1] = '\0';
    char *slash = strrchr(shard_dir, '/');
    if (!slash) {
        free(full);
        return -1;
    }
    *slash = '\0';

    if (mkdir(shard_dir, 0755) < 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    ssize_t written = write(fd, full, total_len);
    if (written < 0 || (size_t)written != total_len) {
        close(fd);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (fsync(fd) < 0) {
        close(fd);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (close(fd) < 0) {
        unlink(tmp_path);
        free(full);
        return -1;
    }

    if (rename(tmp_path, final_path) < 0) {
        unlink(tmp_path);
        free(full);
        return -1;
    }

    int dirfd = open(shard_dir, O_RDONLY);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }

    free(full);
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // TODO: Implement
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fp);
        return -1;
    }

    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        fclose(fp);
        return -1;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        free(file_data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // ─── Parse header ─────────────────────────────
    // format: "type size\0data"

    char type_str[16];
    size_t size = 0;

    size_t i = 0;

    // read type
    while (i < (size_t)file_size && file_data[i] != ' ') {
        i++;
    }

    if (i >= (size_t)file_size) {
        free(file_data);
        return -1;
    }

    memcpy(type_str, file_data, i);
    type_str[i] = '\0';

    i++; // skip space

    // read size
    size_t size_start = i;
    while (i < (size_t)file_size && file_data[i] != '\0') {
        i++;
    }

    if (i >= (size_t)file_size) {
        free(file_data);
        return -1;
    }

    char size_buf[32];
    memcpy(size_buf, file_data + size_start, i - size_start);
    size_buf[i - size_start] = '\0';

    size = strtoul(size_buf, NULL, 10);

    i++; // skip null

    // data starts here
    if (i + size > (size_t)file_size) {
        free(file_data);
        return -1;
    }

    uint8_t *data = malloc(size);
    if (!data) {
        free(file_data);
        return -1;
    }

    memcpy(data, file_data + i, size);

    // ─── verify hash ─────────────────────────────
    ObjectID check;
    compute_hash(file_data, file_size, &check);

    char hex1[HASH_HEX_SIZE + 1];
    char hex2[HASH_HEX_SIZE + 1];

    hash_to_hex(id, hex1);
    hash_to_hex(&check, hex2);

    if (strcmp(hex1, hex2) != 0) {
        free(file_data);
        free(data);
        return -1;
    }

    // ─── set outputs ─────────────────────────────
    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(file_data);
        free(data);
        return -1;
    }

    *data_out = data;
    *len_out = size;

    free(file_data);
    return 0;
}

