# wime — 小鹤双拼 Wayland 输入法

基于 `zwp_input_method_v2` 的原生 Wayland 中文输入法,使用小鹤双拼编码,
无外部输入法框架依赖,开机即用。

## 仓库结构

| 目录 | 说明 |
|------|------|
| `xhfly/` | 主版本。词典含静态词频(全局 `word_freqs` 数组,`XHD1`),候选排序 = 用户词频 × 用户高优 + 静态词频;候选隐藏,`=`/`-` 逐个预览,预览超过 5 个自动弹出候选框 |
| `xhup/` | 精简版(`XHD0` 无静态词频数组),直接显示候选框;排序只依赖用户词频,未用过的新词按编码序 |

### 词典格式区别

- `xhfly`: `DICT_MAGIC 0x58484431`(XHD1),头部第 6 字段为 `word_freqs_count`,词频数组写在各词偏移表之后。
- `xhup`: `DICT_MAGIC 0x58484430`(XHD0),无词频数组,`word_freqs_count` 恒为 0。

## 构建

```sh
cd xhfly        # 或 xhup
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
| `xiaohe.dict` | 构建产物,由 `gen_dict.py` 生成(不入库) |
| `xiaohe` | 编译产物(不入库) |
| `xiaohe.userfreq` | 运行时用户词频(持久化),格式 `词 次数 最后使用epoch`(不入库) |

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

## 备注

- 多音字示例:喝 在 `dict.txt` 中仅保留 `he 喝`(ye 音为古籍"声音嘶哑、噎塞"之义,日常输入意义不大,两仓库一致不含 `ye 喝`)。
- `hu/` 目录为改名前的旧快照,已从仓库移除。
