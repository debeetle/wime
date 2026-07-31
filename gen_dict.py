#!/usr/bin/env python3
"""
从 dictionary.txt + word_freq.txt 构建 xiaohe.dict 

用法: python3 gen_dat.py
"""

import sys
import os
import struct
from collections import defaultdict

MAGIC = 0x58484430  # "XHD0"
MAX_CANDIDATES = 30


def load_word_frequencies():
    """加载词频表 (word_freq.txt)"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    freq_path = os.path.join(script_dir, "word_freq.txt")
    freq_map = {}

    if os.path.exists(freq_path):
        with open(freq_path, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 2:
                    word = parts[0]
                    try:
                        freq = int(parts[1])
                        freq_map[word] = freq
                    except ValueError:
                        pass
        print(
            f"loaded word frequency data including {len(freq_map)} items",
            file=sys.stderr,
        )
    else:
        print("no word_freq.txt, use default order", file=sys.stderr)

    return freq_map


def parse_dictionary(filepath):
    """解析词库 + 按词频排序"""
    entries = defaultdict(list)
    freq_map = load_word_frequencies()

    # 从 dictionary.txt 加载所有条目 (含单字和多字词)
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            code = parts[0].lower()
            word = parts[1]
            if not code.isalpha():
                continue
            if word not in entries[code]:
                entries[code].append(word)

    # 3. 根据词频倒序排序 (高频在前)
    print("sorting word by frequency", file=sys.stderr)
    for code, words in entries.items():
        entries[code] = sorted(words, key=lambda w: freq_map.get(w, 0), reverse=True)

    # 4. 载入用户自定义高优词库 (user_dict.txt / custom_dict.txt)，置于候选最前
    script_dir = os.path.dirname(os.path.abspath(__file__))
    for user_file in ["user_dict.txt", "custom_dict.txt"]:
        user_path = os.path.join(script_dir, user_file)
        if os.path.exists(user_path):
            print(
                f"reading custom dictionary {user_file}...",
                file=sys.stderr,
            )
            with open(user_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split()
                    if len(parts) < 2:
                        continue
                    code = parts[0].lower()
                    word = parts[1]
                    if not code.isalpha():
                        continue
                    if word in entries[code]:
                        entries[code].remove(word)
                    entries[code].insert(0, word)

    return entries


def generate_binary(entries, bin_path):
    sorted_codes = sorted(entries.keys())
    total = len(sorted_codes)

    code_pool = bytearray()
    code_entries = []

    word_pool = bytearray()
    word_offsets = []

    for code in sorted_codes:
        code_offset = len(code_pool)
        code_bytes = code.encode("ascii")
        code_pool.extend(code_bytes)
        code_length = len(code_bytes)

        words = entries[code][:MAX_CANDIDATES]
        word_start = len(word_offsets)
        word_count = len(words)
        for w in words:
            off = len(word_pool)
            word_pool.extend(w.encode("utf-8"))
            word_pool.append(0)
            word_offsets.append(off)

        code_entries.append((code_offset, code_length, word_start, word_count))

    def pad4(pool):
        r = len(pool) % 4
        if r != 0:
            pool.extend(b"\x00" * (4 - r))

    pad4(code_pool)
    pad4(word_pool)

    with open(bin_path, "wb") as f:
        f.write(
            struct.pack(
                "<6I",
                MAGIC,
                total,
                len(code_pool),
                len(word_pool),
                len(word_offsets),
                0,
            )
        )

        for co, cl, ws, wc in code_entries:
            f.write(struct.pack("<iihhi", co, ws, cl, wc, 0))

        f.write(bytes(code_pool))
        f.write(bytes(word_pool))

        for off in word_offsets:
            f.write(struct.pack("<i", off))

    return total, len(code_pool), len(word_pool), len(word_offsets)


def generate_header(header_path):
    code = r"""#ifndef DICT_H
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

#define DICT_MAGIC 0x58484430
#define DICT_MAX_CANDIDATES 30

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
    uint32_t code_pool_size;
    uint32_t word_pool_size;
    uint32_t word_offsets_count;
    uint32_t reserved;
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

#endif /* DICT_H */
"""
    with open(header_path, "w", encoding="utf-8") as f:
        f.write(code)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    dict_txt = os.path.join(script_dir, "dict.txt")
    dict_bin = os.path.join(script_dir, "xiaohe.dict")
    dict_h = os.path.join(script_dir, "dict.h")

    if not os.path.exists(dict_txt):
        print(f"error: cannot find {dict_txt}", file=sys.stderr)
        sys.exit(1)

    print("parsing dictionary", file=sys.stderr)
    entries = parse_dictionary(dict_txt)
    total_words = sum(len(v) for v in entries.values())
    print(
        f"  {len(entries)} unique encodes, {total_words} candidate words",
        file=sys.stderr,
    )

    print("generating xiaohe.dict", file=sys.stderr)
    n, cp, wp, wo = generate_binary(entries, dict_bin)
    bin_size = os.path.getsize(dict_bin)
    print(
        f"{n} items, encode pool {cp} byte, candiate words pool {wp} byte",
        file=sys.stderr,
    )
    print(
        f"xiaohe.dict: {bin_size} byte ({bin_size / 1024 / 1024:.1f} MB)",
        file=sys.stderr,
    )

    print("generating dict.h", file=sys.stderr)
    generate_header(dict_h)

    print("complete", file=sys.stderr)


if __name__ == "__main__":
    main()
