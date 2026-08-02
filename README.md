# wime — minimal IME based on the wayland text-input-method protocol directly

基于 `zwp_input_method_v2` 的原生 Wayland 中文输入法,无外部输入法框架依赖,开机即用。

## 仓库结构

`xhfly/` 与 `xhup/` 是两个完全一致的副本(源码、词典数据均相同)。

### 词典格式

`DICT_MAGIC 0x58484431`(XHD1),头部 6 字段依次为:
`magic | entry_count | code_pool_size | word_pool_size | word_offsets_count | word_freqs_count`。
静态词频数组 `word_freqs[]`(每个候选词构建时的语料词频)写在各词偏移表之后,
运行时参与候选排序:候选分 = 用户词频 × USERFREQ_BOOST + 静态词频。

## 构建

```sh
make            # 需安装 wayland-scanner、pkg-config、mold(可选,见 Makefile)
```

`make` 会依次:

1. `wayland-scanner` 从 `*.xml` 生成协议头/代码;
2. `gen_dict.py` 读取 `dict.txt` + `word_freq.txt`(缺词用 `google_freq.txt` 填空)构建 `xiaohe.dict`;
3. 编译 `main.c` 生成可执行文件 `xiaohe`。

## 数据文件

| 文件 | 作用 |
|------|------|
| `dict.txt` | 小鹤双拼词库(源码,`编码<TAB>词`) |
| `word_freq.txt` | 静态词频主表(对候选取主要排序作用) |
| `google_freq.txt` | Google 词频,补 `word_freq.txt` 缺失词;两者量纲不同,填空时按交集词比值中位数(约 195)缩放 |
| `user_dict.txt` | 用户自定义高优词,构建时置于候选最前、全局最高频 |

## 词频学习

- 上屏即写入 `xiaohe.userfreq`,同会话即时生效,跨会话持久化。
- 用户词频采用 30 天半衰期指数衰减:每过 30 天,次数折半,长期不用逐渐失效。
- `word_freq.txt` 提供的静态词频始终参与排序,是用户词频的底座。

## 使用

```sh
./xiaohe        # 需在 Wayland 会话中运行
```

- 输入小鹤双拼编码自动组字;
- `=`: 预览下一个候选 / 候选框翻页下一页;`-`: 上一候选 / 上一页;
- 候选框展开后 `PgUp`/`PgDn`、`↑`/`↓` 翻页;
- 标点自动转全角,`Shift` 切换中西文。

