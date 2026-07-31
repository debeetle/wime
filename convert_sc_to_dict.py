#!/usr/bin/env python3
"""
将 sc.txt (标准拼音词库) 转换为 dict.txt (小鹤双拼编码词库)。

用法: python3 convert_sc_to_dict.py

小鹤双拼编码规则:
- 第一键: 声母 (bpmfdtnlgkhjqxzcsryw; zh→v, ch→i, sh→u; 零声母→a/o/e)
- 第二键: 韵母 (按键盘位映射)
"""

import sys
import os
from collections import defaultdict

# --- 零声母直接键位 (拼音 → 小鹤编码) ---
ZERO_INITIAL_MAP = {
    'a': 'aa',
    'ai': 'ai',
    'an': 'an',
    'ang': 'ah',
    'ao': 'ao',
    'e': 'ee',
    'ei': 'ei',
    'en': 'en',
    'eng': 'eg',
    'er': 'er',
    'o': 'oo',
    'ou': 'ou',
}


def get_initial_key(initial):
    """标准拼音声母 → 小鹤第一键"""
    if initial == 'zh':
        return 'v'
    if initial == 'ch':
        return 'i'
    if initial == 'sh':
        return 'u'
    if initial in ('', 'y', 'w'):
        return initial
    if len(initial) == 1 and initial in 'bpmfdtnlgkhjqxrzcs':
        return initial
    return None


def get_final_key(final, initial=''):
    """拼音韵母 + 声母 → 小鹤第二键"""
    is_jqxln = initial in ('j', 'q', 'x', 'l', 'n')
    is_jqxlnbpm = initial in ('j', 'q', 'x', 'l', 'n', 'b', 'p', 'm')
    is_bpmf = initial in ('b', 'p', 'm', 'f')
    is_nl = initial in ('n', 'l')
    is_dty = initial in ('d', 't', 'y')

    mapping = {
        'a': 'a',      'ai': 'd',     'an': 'j',     'ang': 'h',
        'ao': 'c',     'e': 'e',      'ei': 'w',     'en': 'f',
        'eng': 'g',    'i': 'i',      'ia': 'x',     'ian': 'm',
        'iang': 'l',   'iao': 'n',    'ie': 'p',     'in': 'b',
        'ing': 'k',    'iong': 's',   'iu': 'q',     'ong': 's',
        'ou': 'z',     'u': 'u',      'ua': 'x',     'uai': 'k',
        'uan': 'r',    'uang': 'l',   'ue': 't',     'ui': 'v',
        'un': 'y',     'uo': 'o',     've': 't',     'v': 'v',
    }

    # j/q/x/y 后的 ü 省略两点写作 u，第二键即 u 键 (与官方码表一致)
    if final == 'u' and initial in ('j', 'q', 'x', 'y'):
        return 'u'

    # 零声母
    if not initial or initial in ('',):
        return mapping.get(final)

    # ueng (只有 weng), üan, ün 特殊处理
    if final == 'ue' and is_nl:
        return 't'
    if final == 'ue' and not is_nl:
        return 't'
    if final == 'ueng':
        return 'g'

    # o 键: bpmfwy → o, 其余 → uo
    if final == 'o':
        return 'o' if (is_bpmf or initial in ('w', 'y')) else 'o'
    if final == 'uo':
        return 'o'

    # k 键: jqxlnbpm + dty → ing, 其余 → uai
    if final == 'ing':
        return 'k'
    if final == 'uai':
        return 'k'

    # x 键: jqxlnbpm → ia, 其余 → ua
    if final == 'ia':
        return 'x'
    if final == 'ua':
        return 'x'

    # s 键: jqx → iong, 其余 → ong
    if final == 'iong':
        return 's'
    if final == 'ong':
        return 's'

    # l 键: jqxln → iang, 其余 → uang
    if final == 'iang':
        return 'l'
    if final == 'uang':
        return 'l'

    # t 键: nl → ve, 其余 → ue
    if final == 've':
        return 't'
    if final == 'ue':
        return 't'

    # v 键: nl → v (ü), 其余 → ui
    if final == 'v':
        return 'v'
    if final == 'ui':
        return 'v'

    return mapping.get(final)


# 拼音声母列表
PINYIN_INITIALS = [
    'zh', 'ch', 'sh',
    'b', 'p', 'm', 'f', 'd', 't', 'n', 'l',
    'g', 'k', 'h', 'j', 'q', 'x',
    'r', 'z', 'c', 's',
    'y', 'w',
]

# 拼音韵母列表 (用于声母+韵母拆分)
PINYIN_FINALS = [
    'iong', 'iang', 'uang', 'ueng',
    'eng', 'ang', 'ing', 'ong',
    'ian', 'iao', 'uai', 'uan',
    'ia', 'ie', 'iu', 'ua', 'uo', 'ue', 'ui',
    'ai', 'ei', 'ao', 'ou',
    'an', 'en', 'in', 'un',
    'a', 'o', 'e', 'i', 'u', 'v',
    'er',
]


def decompose_pinyin(pinyin):
    """
    将拼音拆分为 (声母, 韵母)。
    返回 (initial, final) 或 (None, None) 表示无法拆分。
    """
    if pinyin in ZERO_INITIAL_MAP:
        return ('', pinyin)

    # y/w 开头的拼音: y/w 作为声母键, 保留实际韵母
    if pinyin.startswith('y'):
        remaining = pinyin[1:]
        if remaining == 'u':  # yu = ü, 省略两点，第二键为 u
            return ('y', 'u')
        if remaining == 'uan':  # yuan = üan
            return ('y', 'uan')
        if remaining == 'ue':  # yue = üe
            return ('y', 'ue')
        if remaining == 'un':  # yun = ün
            return ('y', 'un')
        # ya ye yao you yan yang yi yin ying yo yong
        return ('y', remaining)

    if pinyin.startswith('w'):
        remaining = pinyin[1:]
        if remaining == 'u':  # wu
            return ('w', 'u')
        if remaining == 'eng':  # weng
            return ('w', 'ueng')
        # wa wo wai wei wan wen wang
        return ('w', remaining)

    # 标准声母
    for initial in PINYIN_INITIALS:
        if pinyin.startswith(initial) and len(initial) <= 2:
            remaining = pinyin[len(initial):]
            # 查找最长的匹配韵母
            for final in sorted(PINYIN_FINALS, key=len, reverse=True):
                if remaining == final:
                    return (initial, final)
            # try shorter
            return (initial, remaining)

    return (None, None)


def pinyin_to_xiaohe(pinyin):
    """
    将单个拼音音节转换为小鹤双拼编码 (两个字母)。
    返回编码字符串或 None。
    """
    if not pinyin:
        return None

    # ü 归一化为 v
    pinyin = pinyin.replace('ü', 'v')

    # 零声母直接映射
    if pinyin in ZERO_INITIAL_MAP:
        return ZERO_INITIAL_MAP[pinyin]

    initial, final = decompose_pinyin(pinyin)
    if initial is None:
        return None

    # 零声母: 第一键为 a/o/e, 第二键为韵母映射
    if initial == '':
        # ang → first='a', final='ang' → ah
        first_key = pinyin[0]  # a, e, o
        second_key = get_final_key(pinyin, '')
        if first_key and second_key:
            if pinyin in ('ai', 'an', 'ao', 'en', 'er', 'ou'):
                return pinyin  # 直接使用拼音本身
            if pinyin in ('a', 'e', 'o'):
                return pinyin + pinyin  # aa, ee, oo
            return first_key + second_key
        return None

    first_key = get_initial_key(initial)
    second_key = get_final_key(final, initial)
    if not first_key or not second_key:
        return None
    return first_key + second_key


def build_all_mappings():
    """构建所有有效拼音音节 → 小鹤双拼编码的映射表。"""
    mapping = {}
    for first_char in 'abdefghjklmnopqrstwxyz':
        for second_char in 'abcdefghijklmnopqrstuvwxyz':
            xh = first_char + second_char
            pinyin = decode_xiaohe(first_char, second_char)
            if pinyin:
                if pinyin not in mapping:
                    mapping[pinyin] = xh
    return mapping


def decode_xiaohe(k1, k2):
    """小鹤双拼两键 → 拼音 (与 verify_coverage.py 一致，但移除 r→uo for x)"""
    # 零声母标记
    if k1 == 'e' and k2 == 'r':
        return 'er'
    if k1 in ('a', 'o', 'e') and k1 == k2:
        return k1
    if k1 == 'a' and k2 == 'i':
        return 'ai'
    if k1 == 'a' and k2 == 'n':
        return 'an'
    if k1 == 'a' and k2 == 'o':
        return 'ao'
    if k1 == 'e' and k2 == 'n':
        return 'en'
    if k1 == 'o' and k2 == 'u':
        return 'ou'

    # 声母映射
    init_map = {
        'b': 'b', 'p': 'p', 'm': 'm', 'f': 'f',
        'd': 'd', 't': 't', 'n': 'n', 'l': 'l',
        'g': 'g', 'k': 'k', 'h': 'h',
        'j': 'j', 'q': 'q', 'x': 'x',
        'z': 'z', 'c': 'c', 's': 's', 'r': 'r',
        'y': 'y', 'w': 'w',
        'v': 'zh', 'i': 'ch', 'u': 'sh',
        'a': '', 'o': '', 'e': '',
    }
    sm = init_map.get(k1, k1)
    ym = get_final_key(k2, sm)
    if ym is None:
        return None
    return sm + ym


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sc_path = os.path.join(script_dir, 'sc.txt')
    dict_path = os.path.join(script_dir, 'dict.txt')

    if not os.path.exists(sc_path):
        print(f"错误: 找不到 {sc_path}", file=sys.stderr)
        sys.exit(1)

    entries = defaultdict(list)

    with open(sc_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            char = parts[0]
            pinyin = parts[1].lower()
            freq_str = parts[2] if len(parts) >= 3 else '0'

            try:
                freq = float(freq_str)
            except ValueError:
                freq = 0.0

            # 处理多音节拼音 (用 ' 分隔)
            syllables = pinyin.split("'")
            xiaohe_codes = []
            for syl in syllables:
                xh = pinyin_to_xiaohe(syl)
                if xh is None:
                    xiaohe_codes = None
                    break
                xiaohe_codes.append(xh)

            if xiaohe_codes is None:
                continue

            # 拼接所有编码 (单字与多字词均处理)
            code = ''.join(xiaohe_codes)
            if char not in entries[code]:
                entries[code].append(char)

    # 排序: 按编码字母序
    sorted_codes = sorted(entries.keys())

    with open(dict_path, 'w', encoding='utf-8') as f:
        for code in sorted_codes:
            chars = entries[code]
            for ch in chars:
                f.write(f"{code}\t{ch}\n")

    total = sum(len(v) for v in entries.values())
    print(f"生成 {len(entries)} 个编码, {total} 个候选词 → {dict_path}", file=sys.stderr)


if __name__ == '__main__':
    main()
