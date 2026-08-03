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

#define DICT_MAGIC 0x58484432
#define DICT_MAX_CANDIDATES 30

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
    uint32_t code_pool_size;
    uint32_t word_pool_size;
    uint32_t word_offsets_count;
    uint32_t word_freqs_count;
    uint32_t max_code_len;
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
    // 按 code_length 分桶:len_starts[len]/len_counts[len] 给出
    // code_length == len 的条目在 entries[] 中的连续区间(桶内按 code 排序)。
    uint32_t max_code_len;
    const int32_t *len_starts;
    const int32_t *len_counts;
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
    G_DICT.max_code_len = hdr->max_code_len;

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
    ptr += sizeof(int32_t) * hdr->word_freqs_count;

    G_DICT.len_starts = (const int32_t *)ptr;
    ptr += sizeof(int32_t) * (hdr->max_code_len + 1);

    G_DICT.len_counts = (const int32_t *)ptr;

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

// 精确匹配:只在 code_length == key_len 的桶内二分,桶内按 code 排序。
static inline int dict_find(const char *key, int key_len) {
    if (key_len < 0 || (uint32_t)key_len > G_DICT.max_code_len)
        return -1;
    int lo = G_DICT.len_starts[key_len];
    int hi = lo + G_DICT.len_counts[key_len] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const DictCodeEntry *e = &G_DICT.entries[mid];
        int r = memcmp(key, G_DICT.code_pool + e->code_offset, key_len);
        if (r == 0) return mid;
        if (r < 0) hi = mid - 1;
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

// 前缀联想:在 code_length == code_len 的桶内找第一个以 prefix 开头的条目下标。
// 桶内条目按 code 排序且长度一致,故前缀匹配段是连续的。
static inline int dict_prefix_lower(const char *prefix, int prefix_len, int code_len) {
    if (code_len < prefix_len || (uint32_t)code_len > G_DICT.max_code_len)
        return -1;
    int lo = G_DICT.len_starts[code_len];
    int hi = lo + G_DICT.len_counts[code_len] - 1;
    int result = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const DictCodeEntry *e = &G_DICT.entries[mid];
        int r = memcmp(prefix, G_DICT.code_pool + e->code_offset, prefix_len);
        if (r <= 0) {
            if (r == 0) result = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return result;
}

static inline bool dict_has_prefix(const char *prefix, int prefix_len, int code_len) {
    return dict_prefix_lower(prefix, prefix_len, code_len) >= 0;
}

static inline int32_t dict_word_freq(int word_idx) {
    if (!G_DICT.word_freqs)
        return 0;
    return G_DICT.word_freqs[word_idx];
}

#endif /* DICT_H */
