#!/usr/bin/env python3
"""
从 dict.txt + word_freq.txt 构建 XHD2 词典(供 xhup/xhfly 等输入法共用)

用法: python3 gen_dict.py [输出词典路径] [词典源码目录]
词典源码目录内含该输入法私有的 dict.txt / user_dict.txt;
word_freq.txt 始终从本脚本所在目录读取(按词索引,各输入法共用)。
不传参数时输出到本目录下的 xiaohe.dict。
"""

import sys
import os
import struct
from collections import defaultdict

MAGIC = 0x58484432  # "XHD2" (v3: 条目按 (code_length, code) 排序 + 每长度桶索引)
MAX_CANDIDATES = 30


def load_word_frequencies():
    """加载词频表 (word_freq.txt)。

    word_freq.txt 已包含合并后的全部词频(原 word_freq + 缩放后的 Google 词频),
    无需再运行时合并。
    """
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


def parse_dictionary(filepath, user_dir):
    """解析词库 + 按词频排序"""
    entries = defaultdict(list)
    seen = defaultdict(set)
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
            # 用 set 去重:list 的 `in` 是 O(n),同编码词多时退化为 O(n^2)
            if word not in seen[code]:
                seen[code].add(word)
                entries[code].append(word)

    # 3. 根据词频倒序排序 (高频在前)
    print("sorting word by frequency", file=sys.stderr)
    for code, words in entries.items():
        entries[code] = sorted(words, key=lambda w: freq_map.get(w, 0), reverse=True)

    # 4. 载入用户自定义高优词库 (user_dict.txt)，置于候选最前
    for user_file in ["user_dict.txt"]:
        user_path = os.path.join(user_dir, user_file)
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
                    parts = line.split("\t")
                    if len(parts) < 2:
                        continue
                    code = parts[0].lower()
                    word = parts[1]
                    if not code.isalpha():
                        continue
                    if word in entries[code]:
                        entries[code].remove(word)
                    entries[code].insert(0, word)
                    freq_map[word] = 2000000000  # 用户词库全局最高优先

    return entries, freq_map


def generate_binary(entries, freq_map, bin_path):
    # 条目按 (code_length, code) 排序,同长度代码相邻,
    # 运行时按长度桶二分查找,前缀联想只扫目标长度区间(见 dict.h)。
    sorted_codes = sorted(entries.keys(), key=lambda c: (len(c), c))
    total = len(sorted_codes)

    max_code_len = max((len(c) for c in sorted_codes), default=0)

    code_pool = bytearray()
    code_entries = []

    word_pool = bytearray()
    word_offsets = []
    word_freqs = []

    len_starts = [0] * (max_code_len + 1)
    len_counts = [0] * (max_code_len + 1)

    for code in sorted_codes:
        cl = len(code)
        if len_counts[cl] == 0:
            len_starts[cl] = len(code_entries)
        len_counts[cl] += 1

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
            freq = freq_map.get(w, 0)
            word_freqs.append(max(0, min(freq, 0x7FFFFFFF)))

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
                "<7I",
                MAGIC,
                total,
                len(code_pool),
                len(word_pool),
                len(word_offsets),
                len(word_freqs),
                max_code_len,
            )
        )

        for co, cl, ws, wc in code_entries:
            f.write(struct.pack("<iihhi", co, ws, cl, wc, 0))

        f.write(bytes(code_pool))
        f.write(bytes(word_pool))

        for off in word_offsets:
            f.write(struct.pack("<i", off))

        for fr in word_freqs:
            f.write(struct.pack("<i", fr))

        for s in len_starts:
            f.write(struct.pack("<i", s))

        for c in len_counts:
            f.write(struct.pack("<i", c))

    return total, len(code_pool), len(word_pool), len(word_offsets)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = sys.argv[2] if len(sys.argv) > 2 else script_dir
    dict_txt = os.path.join(src_dir, "dict.txt")
    dict_bin = (
        sys.argv[1]
        if len(sys.argv) > 1
        else os.path.join(script_dir, "xiaohe.dict")
    )

    if not os.path.exists(dict_txt):
        print(f"error: cannot find {dict_txt}", file=sys.stderr)
        sys.exit(1)

    print("parsing dictionary", file=sys.stderr)
    entries, freq_map = parse_dictionary(dict_txt, src_dir)
    total_words = sum(len(v) for v in entries.values())
    print(
        f"  {len(entries)} unique encodes, {total_words} candidate words",
        file=sys.stderr,
    )

    name = os.path.basename(dict_bin)
    print(f"generating {name}", file=sys.stderr)
    n, cp, wp, wo = generate_binary(entries, freq_map, dict_bin)
    bin_size = os.path.getsize(dict_bin)
    print(
        f"{n} items, encode pool {cp} byte, candidate words pool {wp} byte",
        file=sys.stderr,
    )
    print(
        f"{name}: {bin_size} byte ({bin_size / 1024 / 1024:.1f} MB)",
        file=sys.stderr,
    )

    print("complete", file=sys.stderr)


if __name__ == "__main__":
    main()
