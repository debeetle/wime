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

    # 4. 载入用户自定义高优词库 (user_dict.txt)，置于候选最前
    script_dir = os.path.dirname(os.path.abspath(__file__))
    for user_file in ["user_dict.txt"]:
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


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    dict_txt = os.path.join(script_dir, "dict.txt")
    dict_bin = os.path.join(script_dir, "xiaohe.dict")

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

    print("complete", file=sys.stderr)


if __name__ == "__main__":
    main()
