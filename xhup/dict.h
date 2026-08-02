#ifndef DICT_H
#define DICT_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define DICT_MAGIC 0x58484431
#define DICT_MAX_CANDIDATES 30

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
    uint32_t code_pool_size;
    uint32_t word_pool_size;
    uint32_t word_offsets_count;
    uint32_t word_freqs_count;
} DictHeader;

typedef struct {
    int32_t code_offset;
    int32_t word_start;
    int16_t code_length;
    int16_t word_count;
    int32_t _pad;
} DictCodeEntry;

typedef struct {
    void *mmap_base;
    size_t mmap_size;
    uint32_t entry_count;
    const DictCodeEntry *entries;
    const char *code_pool;
    const char *word_pool;
    const int32_t *word_offsets;
    const int32_t *word_freqs;
} Dict;

static Dict G_DICT = {0};

static inline bool dict_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open dict"); return false; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return false; }

    void *base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return false; }

    const DictHeader *hdr = (const DictHeader *)base;
    if (hdr->magic != DICT_MAGIC) {
        fprintf(stderr, "错误: 词库文件格式不正确\n");
        munmap(base, st.st_size);
        return false;
    }

    G_DICT.mmap_base = base;
    G_DICT.mmap_size = st.st_size;
    G_DICT.entry_count = hdr->entry_count;

    const char *ptr = (const char *)base + sizeof(DictHeader);
    G_DICT.entries = (const DictCodeEntry *)ptr;
    ptr += sizeof(DictCodeEntry) * hdr->entry_count;

    G_DICT.code_pool = ptr;
    ptr += hdr->code_pool_size;

    G_DICT.word_pool = ptr;
    ptr += hdr->word_pool_size;

    G_DICT.word_offsets = (const int32_t *)ptr;
    ptr += sizeof(int32_t) * hdr->word_offsets_count;

    G_DICT.word_freqs = (const int32_t *)ptr;

    return true;
}

static inline void dict_unload(void) {
    if (G_DICT.mmap_base) {
        munmap(G_DICT.mmap_base, G_DICT.mmap_size);
        G_DICT.mmap_base = NULL;
    }
}

static inline int dict_compare(const char *key, int key_len, int idx) {
    const DictCodeEntry *e = &G_DICT.entries[idx];
    const char *code = G_DICT.code_pool + e->code_offset;
    int clen = e->code_length;
    int n = key_len < clen ? key_len : clen;
    int r = memcmp(key, code, n);
    if (r != 0) return r;
    return key_len - clen;
}

static inline int dict_find(const char *key, int key_len) {
    int lo = 0, hi = (int)G_DICT.entry_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = dict_compare(key, key_len, mid);
        if (cmp == 0) return mid;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

static inline int dict_lookup(const char *key, int key_len, const char **out, int max_out) {
    int idx = dict_find(key, key_len);
    if (idx < 0) return 0;
    const DictCodeEntry *e = &G_DICT.entries[idx];
    int count = e->word_count;
    if (count > max_out) count = max_out;
    for (int i = 0; i < count; i++) {
        out[i] = G_DICT.word_pool + G_DICT.word_offsets[e->word_start + i];
    }
    return count;
}

static inline int dict_prefix_lower(const char *prefix, int prefix_len) {
    int lo = 0, hi = (int)G_DICT.entry_count - 1, result = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const DictCodeEntry *e = &G_DICT.entries[mid];
        const char *code = G_DICT.code_pool + e->code_offset;
        int clen = e->code_length;
        int n = prefix_len < clen ? prefix_len : clen;
        int cmp = memcmp(prefix, code, n);
        if (cmp == 0 && prefix_len <= clen) {
            result = mid;
            hi = mid - 1;
        } else if (cmp <= 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return result;
}

static inline bool dict_has_prefix(const char *prefix, int prefix_len) {
    return dict_prefix_lower(prefix, prefix_len) >= 0;
}

static inline int32_t dict_word_freq(int word_idx) {
    if (!G_DICT.word_freqs)
        return 0;
    return G_DICT.word_freqs[word_idx];
}

#endif /* DICT_H */
