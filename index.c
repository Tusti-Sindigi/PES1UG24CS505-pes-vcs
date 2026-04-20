// index.c — Staging area implementation

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

// forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

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

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

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
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
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
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

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
                if (S_ISREG(st.st_mode)) {
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

// ─── IMPLEMENTATION ───────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char mode_str[16];
    char hash_hex[HASH_HEX_SIZE + 1];
    char path[512];
    uint64_t mtime;
    uint32_t size;

    while (fscanf(f, "%s %s %lu %u %s",
                  mode_str, hash_hex, &mtime, &size, path) == 5) {

        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count++];

        e->mode = strtol(mode_str, NULL, 8);
        hex_to_hash(hash_hex, &e->hash);
        e->mtime_sec = mtime;
        e->size = size;
        strcpy(e->path, path);
    }

    fclose(f);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    const IndexEntry *e1 = a;
    const IndexEntry *e2 = b;
    return strcmp(e1->path, e2->path);
}

int index_save(const Index *index) {
    if (!index) return -1;

    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES)
        return -1;

    char tmp_path[] = INDEX_FILE ".tmp";

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    // Create array of pointers instead of copying struct
    IndexEntry *arr[MAX_INDEX_ENTRIES];

    for (int i = 0; i < index->count; i++) {
        arr[i] = (IndexEntry *)&index->entries[i];
    }

    // Comparator for pointers
    int cmp(const void *a, const void *b) {
        IndexEntry *e1 = *(IndexEntry **)a;
        IndexEntry *e2 = *(IndexEntry **)b;
        return strcmp(e1->path, e2->path);
    }

    qsort(arr, index->count, sizeof(IndexEntry *), cmp);

    for (int i = 0; i < index->count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&arr[i]->hash, hash_hex);

        fprintf(f, "%o %s %lu %u %s\n",
                arr[i]->mode,
                hash_hex,
                arr[i]->mtime_sec,
                arr[i]->size,
                arr[i]->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}
int index_add(Index *index, const char *path) {
    if (!index) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0) {
        fclose(f);
        return -1;
    }

    void *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, size, f) != (size_t)size) {
        fclose(f);
        free(buffer);
        return -1;
    }

    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, buffer, size, &id) != 0) {
        free(buffer);
        return -1;
    }

    free(buffer);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *e = index_find(index, path);

    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;

        e = &index->entries[index->count++];
    }

    if (st.st_mode & S_IXUSR)
        e->mode = 0100755;
    else
        e->mode = 0100644;

    e->hash = id;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    strcpy(e->path, path);

    return index_save(index);
}
