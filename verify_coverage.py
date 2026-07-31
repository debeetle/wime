#!/usr/bin/env python3
"""小鹤双拼解码器（匹配本项目词典的实际编码）"""

INITIALS = {
    'b': 'b', 'p': 'p', 'm': 'm', 'f': 'f',
    'd': 'd', 't': 't', 'n': 'n', 'l': 'l',
    'g': 'g', 'k': 'k', 'h': 'h',
    'j': 'j', 'q': 'q', 'x': 'x',
    'z': 'z', 'c': 'c', 's': 's', 'r': 'r',
    'y': 'y', 'w': 'w',
    'v': 'zh', 'i': 'ch', 'u': 'sh',
    'a': '', 'o': '', 'e': '',
}

# 标准韵母键位
FINAL_MAP = {
    'a': 'a', 'b': 'in', 'c': 'ao', 'e': 'e', 'f': 'en',
    'g': 'eng', 'h': 'ang', 'i': 'i', 'j': 'an',
    'd': 'ai', 'l': 'uang', 'm': 'ian', 'n': 'iao',
    'p': 'ie', 'q': 'iu', 'r': 'uan', 's': 'ong',
    'u': 'u', 'w': 'ei', 'y': 'un', 'z': 'ou',
}

def get_final(key2, sm):
    is_jqxln = sm in ('j','q','x','l','n')
    is_jqxlnbpm = sm in ('j','q','x','l','n','b','p','m')
    is_bpmf = sm in ('b','p','m','f')
    is_nl = sm in ('n','l')

    if key2 == 'o':
        return 'o' if (is_bpmf or sm in ('w','y')) else 'uo'
    if key2 == 'v':
        return 'v' if is_nl else 'ui'
    if key2 == 't':
        return 've' if is_nl else 'ue'

    # d 键: ai (黛)
    if key2 == 'd':
        return 'ai'

    # k 键: ing (jqxlnbpm + dty) | uai (其余)
    if key2 == 'k':
        if is_jqxlnbpm or sm in ('d','t','y'):
            return 'ing'
        return 'uai'

    # x 键: ia (jqxlnbpm) | ua (其余)
    if key2 == 'x':
        if is_jqxlnbpm:
            return 'ia'
        if sm == 'r':
            return 'uo'
        return 'ua'

    # s 键: iong (jqx) | ong (其余)
    if key2 == 's':
        return 'iong' if sm in ('j','q','x') else 'ong'

    # l 键: iang (jqxln) | uang (两望)
    if key2 == 'l':
        return 'iang' if sm in ('j','q','x','l','n') else 'uang'

    return FINAL_MAP.get(key2, None)

def decode(k1, k2):
    if k1 == 'e' and k2 == 'r':
        return 'er'
    if k1 in ('a','o','e') and k1 == k2:
        return k1
    # 零声母直接拼音键位
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
    sm = INITIALS.get(k1, k1)
    ym = get_final(k2, sm)
    if ym is None:
        return None
    return sm + ym

tests = [
    ('b','u','bu'), ('n','i','ni'), ('h','c','hao'), ('v','v','zhui'),
    ('v','k','zhuai'), ('u','d','shai'), ('u','l','shuang'), ('w','o','wo'), ('b','o','bo'),
    ('m','o','mo'), ('f','o','fo'), ('y','o','yo'), ('d','o','duo'),
    ('g','o','guo'), ('n','v','nv'), ('l','v','lv'), ('n','t','nve'),
    ('l','t','lve'), ('e','r','er'), ('a','a','a'),
    ('a','c','ao'), ('a','j','an'), ('a','h','ang'), ('e','f','en'),
    ('e','w','ei'), ('o','z','ou'), ('j','u','ju'), ('q','u','qu'),
    ('r','i','ri'), ('z','i','zi'), ('u','i','shi'), ('v','i','zhi'),
    ('i','i','chi'), ('j','k','jing'), ('x','k','xing'), ('b','k','bing'),
    ('g','k','guai'), ('g','d','gai'),
    ('j','x','jia'), ('g','x','gua'),
    # 本词典实际使用的补充映射
    ('a','i','ai'), ('a','n','an'), ('a','o','ao'),
    ('e','n','en'), ('o','u','ou'),
    ('b','d','bai'), ('d','d','dai'), ('m','d','mai'),
    ('p','d','pai'), ('t','d','tai'), ('w','d','wai'),
    ('d','k','ding'), ('t','k','ting'), ('y','k','ying'),
    ('j','l','jiang'), ('q','l','qiang'), ('x','l','xiang'), ('g','l','guang'),
    ('j','s','jiong'), ('q','s','qiong'), ('x','s','xiong'),
    ('z','d','zai'), ('c','d','cai'), ('s','d','sai'),
]

if __name__ == '__main__':
    print("=== 解码器单元测试 ===")
    failures = 0
    for k1, k2, expected in tests:
        result = decode(k1, k2)
        status = "✓" if result == expected else f"✗ (得到 {result})"
        if result != expected:
            failures += 1
        print(f"  {k1}{k2} -> {expected}: {status}")
    print(f"\n{len(tests)} 个测试, {failures} 个失败\n")
