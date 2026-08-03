# wime — minimal IME based on the wayland text-input-method protocol directly

基于 `zwp_input_method_v2` 的原生 Wayland 中文输入法,无外部输入法框架依赖,开机即用。
wime 是一个多输入法项目:每个输入法一个目录,共用根目录的 Wayland 协议与 `xiaohe/` 下的公共词库/构建脚本。

```
wime/
├── input-method-unstable-v2.xml    # Wayland 协议源码(所有输入法共用)
├── virtual-keyboard-unstable-v1.xml
├── xiaohe/                         # 小鹤双拼族公共文件
│   ├── dict.h                      # 词典 mmap/查询
│   ├── gen_dict.py                 # 词典构建脚本(源码目录作参数,word_freq.txt 读公共)
│   ├── word_freq.txt               # 静态词频主表(按词索引,各输入法共用)
│   ├── xhup/                       # xhup 输入法
│   │   ├── dict.txt                # 可读词库源码(编码<TAB>词,各输入法私有)
│   │   ├── user_dict.txt           # 用户自定义高优词(编码<TAB>词,各输入法私有)
│   │   ├── Makefile
│   │   └── main.c
│   └── xhfly/                      # xhfly 输入法
│       ├── dict.txt
│       ├── user_dict.txt
│       ├── Makefile
│       └── main.c
└── hu/  ...                        # (未来其它输入法)
```

### 词典格式

`DICT_MAGIC 0x58484432`(XHD2),头部 7 字段依次为:
`magic | entry_count | code_pool_size | word_pool_size | word_offsets_count | word_freqs_count | max_code_len`。
条目按 `(code_length, code)` 排序,尾部另存每长度桶索引 `len_starts[]/len_counts[]`,
精确匹配与前缀联想均只在对应长度桶内二分,避免扫描整表。
静态词频数组 `word_freqs[]`(每个候选词构建时的语料词频)写在各词偏移表之后,
运行时参与候选排序:候选分 = 用户词频 × USERFREQ_BOOST + 静态词频。

## 构建

```sh
make -C xiaohe/xhup   # 构建 xhup
make -C xiaohe/xhfly  # 构建 xhfly
# 需安装 wayland-scanner、pkg-config、mold(option)
```

`make` 会依次:

1. `wayland-scanner` 从根目录 `*.xml` 生成协议头/代码;
2. `xiaohe/gen_dict.py` 读取各输入法目录下的 `dict.txt` + `user_dict.txt`(及公共 `xiaohe/word_freq.txt`)构建词典;`word_freq.txt` 已合并 word_freq 与 Google 两份词频源;`xhup.dict`/`xhfly.dict` 为各自构建产物;`make` 会重建;
3. 编译 `main.c` 生成可执行文件 `xhup`/`xhfly`。

## 数据文件

| 文件 | 作用 |
|------|------|
| `xiaohe/xhfly/dict.txt` (xhup 同) | 可读词库源码(`编码<TAB>词`,各输入法私有,音形/双拼可各自演变) |
| `xiaohe/word_freq.txt` | 静态词频主表(已合并 word_freq + 缩放后的 Google 词频,按词索引,对候选取主要排序作用,各输入法共用) |
| `xiaohe/xhfly/user_dict.txt` (xhup 同) | 用户自定义高优词,构建时置于候选最前、全局最高频(各输入法私有) |

## 词频学习

- 上屏即写入 `<输入法名>.userfreq`,同会话即时生效,跨会话持久化。
- 用户词频采用 30 天半衰期指数衰减:每过 30 天,次数折半,长期不用逐渐失效。
- `word_freq.txt` 提供的静态词频始终参与排序,是用户词频的底座。

## 使用

```sh
./xiaohe/xhfly/xhfly   # 需在 Wayland 会话中运行
```

- 输入小鹤双拼编码自动组字;
- `=`: 预览下一个候选 / 候选框翻页下一页;`-`: 上一候选 / 上一页;
- 候选框展开后 `PgUp`/`PgDn`、`↑`/`↓` 翻页;
- 标点自动转全角,`Shift` 切换中西文。
