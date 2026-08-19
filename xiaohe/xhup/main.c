#include "dict.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <wayland-client.h>

#define MAX_CANDS 30
#define PAGE_SIZE 3

struct ime_state {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct zwp_input_method_manager_v2 *im_manager;
  struct zwp_input_method_v2 *input_method;
  struct zwp_input_method_keyboard_grab_v2 *keyboard_grab;
  struct zwp_virtual_keyboard_manager_v1 *vk_manager;
  struct zwp_virtual_keyboard_v1 *virtual_keyboard;
  bool keymap_set;
  uint32_t active_mods;
  uint32_t current_serial;

  char buffer[64];
  int buf_len;

  // 候选状态
  const char *cand_ptrs[MAX_CANDS];
  bool cand_is_full[MAX_CANDS];
  int full_match_count;
  int total;
  int page;

  // 候选缓存:候选仅依赖 buffer 内容,预览/翻页时 buffer 未变可直接复用
  char cand_cache_buf[64];
  int cand_cache_len;
  bool cand_cache_valid;

  // 模式与标点
  bool mode_ascii;
  int shift_count;
  bool shift_combo;
  bool key_passthrough_pressed[256];
  // 被输入法消费(未透传)的按键;按下时置位,用于消费配对的 Release
  bool key_consumed_pressed[256];
  bool punct_fullwidth;
  bool double_quote_open;
  bool single_quote_open;
  bool running;

  // 退格 repeat（timerfd + compositor repeat_info 驱动）
  bool backspace_active;
  uint32_t repeat_rate;
  uint32_t repeat_delay;
  int repeat_timer_fd;

  bool zh_indicator_active;
  uint64_t zh_indicator_time;
};

static uint64_t get_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// 墙上时钟(epoch 秒),用于跨会话的用户词频半衰期衰减。
// CLOCK_MONOTONIC 重启归零,不能用于跨重启的时间差。
static uint64_t get_epoch_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec;
}

// --- 用户词频(持久化,学习真实输入习惯) ---
// 静态词典词频是构建时烘焙的语料频率,不会随使用更新。
// 这里维护一份持久化的用户词频:每次上屏一个候选即累加,
// 存到 xhup.userfreq,重启后加载,真正反映用户的输入习惯。
#define MAX_USER_WORDS 8192
#define USERFREQ_BOOST 1000000LL
// 半衰期(秒):用户词频跨会话指数衰减。默认 30 天,即词在 30 天后权重减半,
// 让陈旧的高频词随时间的推移退场,会话内仍即时累加不受影响。
#define USERFREQ_HALF_LIFE_SEC (30 * 24 * 3600)
typedef struct {
  char word[64];
  int count;
  uint64_t last_used;
} UserWord;

static UserWord G_USER_WORDS[MAX_USER_WORDS];
static int G_USER_COUNT = 0;
static char G_USERFREQ_PATH[512] = {0};
static bool G_USERFREQ_DIRTY = false;
static uint64_t G_LAST_SAVE_MS = 0;

// SIGTERM/SIGINT 时置位,主循环退出后统一 flush 用户词频(信号处理须保持 async-signal-safe,
// 不能直接调 user_freq_save)。
static volatile sig_atomic_t G_SIGNAL_FLAG = 0;
static void handle_exit_signal(int sig) {
  (void)sig;
  G_SIGNAL_FLAG = 1;
}

// 用户词频查找哈希表:word -> G_USER_WORDS 下标,空槽为 -1。
// 候选排序/前缀联想热路径里每个候选都要查词频,线性扫描 8192 词会显著拖慢打字。
// 开放寻址 + FNV-1a,容量取 2 的幂保证掩码,负载恒小于 1/2。
#define USER_HASH_BITS 14
#define USER_HASH_SIZE (1 << USER_HASH_BITS)
#define USER_HASH_MASK (USER_HASH_SIZE - 1)
static int G_USER_HASH[USER_HASH_SIZE];

static void user_hash_init(void) {
  for (int i = 0; i < USER_HASH_SIZE; i++)
    G_USER_HASH[i] = -1;
}

static uint32_t hash_word(const char *word) {
  uint32_t h = 2166136261u;
  while (*word) {
    h ^= (unsigned char)*word++;
    h *= 16777619u;
  }
  return h;
}

static void user_hash_insert(int idx) {
  uint32_t slot = hash_word(G_USER_WORDS[idx].word) & USER_HASH_MASK;
  while (G_USER_HASH[slot] >= 0)
    slot = (slot + 1) & USER_HASH_MASK;
  G_USER_HASH[slot] = idx;
}

static int user_hash_find(const char *word) {
  uint32_t slot = hash_word(word) & USER_HASH_MASK;
  while (G_USER_HASH[slot] >= 0) {
    int idx = G_USER_HASH[slot];
    if (strcmp(G_USER_WORDS[idx].word, word) == 0)
      return idx;
    slot = (slot + 1) & USER_HASH_MASK;
  }
  return -1;
}

static void user_hash_rebuild(void) {
  for (int i = 0; i < USER_HASH_SIZE; i++)
    G_USER_HASH[i] = -1;
  for (int i = 0; i < G_USER_COUNT; i++)
    user_hash_insert(i);
}

static int get_user_count(const char *word) {
  int idx = user_hash_find(word);
  return (idx >= 0) ? G_USER_WORDS[idx].count : 0;
}

static void record_user_word(const char *word, uint64_t now_epoch) {
  if (!word || !word[0])
    return;
  int idx = user_hash_find(word);
  if (idx >= 0) {
    G_USER_WORDS[idx].count++;
    G_USER_WORDS[idx].last_used = now_epoch;
    G_USERFREQ_DIRTY = true;
    return;
  }
  if (G_USER_COUNT < MAX_USER_WORDS) {
    idx = G_USER_COUNT;
    G_USER_COUNT++;
  } else {
    // 满员:淘汰使用次数最少者(同次数淘汰最久未用),保留高频词
    int min_idx = 0;
    for (int i = 1; i < G_USER_COUNT; i++) {
      if (G_USER_WORDS[i].count < G_USER_WORDS[min_idx].count ||
          (G_USER_WORDS[i].count == G_USER_WORDS[min_idx].count &&
           G_USER_WORDS[i].last_used < G_USER_WORDS[min_idx].last_used))
        min_idx = i;
    }
    idx = min_idx;
  }
  strncpy(G_USER_WORDS[idx].word, word, 63);
  G_USER_WORDS[idx].word[63] = '\0';
  G_USER_WORDS[idx].count = 1;
  G_USER_WORDS[idx].last_used = now_epoch;
  if (G_USER_COUNT >= MAX_USER_WORDS)
    user_hash_rebuild(); // 满员淘汰:旧词哈希被覆盖,整体重建
  else
    user_hash_insert(idx);
  G_USERFREQ_DIRTY = true;
}

static void user_freq_save(void) {
  if (!G_USERFREQ_PATH[0] || !G_USERFREQ_DIRTY)
    return;
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", G_USERFREQ_PATH);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  for (int i = 0; i < G_USER_COUNT; i++)
    fprintf(f, "%s %d %llu\n", G_USER_WORDS[i].word, G_USER_WORDS[i].count,
            (unsigned long long)G_USER_WORDS[i].last_used);
  fclose(f);
  rename(tmp, G_USERFREQ_PATH);
  G_USERFREQ_DIRTY = false;
}

static void user_freq_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[128];
  uint64_t now_epoch = get_epoch_sec();
  while (G_USER_COUNT < MAX_USER_WORDS && fgets(line, sizeof(line), f)) {
    char word[64];
    int count;
    unsigned long long last = 0;
    // 兼容旧格式 "词 次数"(无时间戳),视为刚使用不衰减
    if (sscanf(line, "%63s %d %llu", word, &count, &last) >= 2 &&
        count > 0) {
      if (last != 0 && now_epoch > last) {
        // 半衰期指数衰减:每过 HALF_LIFE 时间,权重折半
        uint64_t elapsed = now_epoch - last;
        int half_lives = (int)(elapsed / USERFREQ_HALF_LIFE_SEC);
        for (int i = 0; i < half_lives && count > 1; i++)
          count >>= 1;
      }
      strncpy(G_USER_WORDS[G_USER_COUNT].word, word, 63);
      G_USER_WORDS[G_USER_COUNT].word[63] = '\0';
      G_USER_WORDS[G_USER_COUNT].count = count;
      G_USER_WORDS[G_USER_COUNT].last_used = last ? last : now_epoch;
      user_hash_insert(G_USER_COUNT);
      G_USER_COUNT++;
    }
  }
  fclose(f);
}

static void clear_preedit(struct ime_state *s) {
  zwp_input_method_v2_set_preedit_string(s->input_method, "", 0, 0);
  zwp_input_method_v2_commit(s->input_method, s->current_serial);
  wl_display_flush(s->display);
}

static void reset_state(struct ime_state *s) {
  s->buf_len = 0;
  s->buffer[0] = '\0';
  s->full_match_count = 0;
  s->total = 0;
  s->page = 0;
  s->cand_cache_valid = false;
}

// 安全追加格式化文本:cap 为缓冲区总容量,pos 一旦到达 cap 就不再写入,
// 避免 snprintf 返回值超过剩余大小时出现指针越界/长度下溢。
static size_t preedit_append(char *buf, size_t cap, size_t pos,
                             const char *fmt, ...) {
  if (pos >= cap)
    return cap;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
  va_end(ap);
  if (n < 0)
    return pos;
  size_t add = (size_t)n;
  if (add >= cap - pos)
    return cap; // 已截断:后续调用不再写入
  return pos + add;
}

static char evdev_to_char(uint32_t key) {
  if (key >= KEY_Q && key <= KEY_P)
    return "qwertyuiop"[key - KEY_Q];
  if (key >= KEY_A && key <= KEY_L)
    return "asdfghjkl"[key - KEY_A];
  if (key >= KEY_Z && key <= KEY_M)
    return "zxcvbnm"[key - KEY_Z];
  return 0;
}

static void passthrough_key(struct ime_state *s, uint32_t time, uint32_t key,
                            uint32_t st) {
  if (st != 0 && st != 1)
    return;
  if (key < 256)
    s->key_passthrough_pressed[key] = (st == 1);
  if (s->virtual_keyboard && s->keymap_set)
    zwp_virtual_keyboard_v1_key(s->virtual_keyboard, time, key, st);
}

static void compute_candidates(struct ime_state *s) {
  // 候选只依赖 buffer:预览/翻页时内容未变,直接复用上次结果,
  // 必须在清零 total 之前返回,否则预览状态被清掉。
  if (s->cand_cache_valid && s->cand_cache_len == s->buf_len &&
      memcmp(s->cand_cache_buf, s->buffer, s->buf_len) == 0)
    return;

  s->total = 0;
  s->full_match_count = 0;
  if (s->buf_len == 0)
    return;

  if (s->buf_len % 2 == 0) {
    // 偶数位:完整编码精确匹配(完整词)+ 首音节单字
    int full_hits = dict_lookup(s->buffer, s->buf_len, s->cand_ptrs, MAX_CANDS);
    s->full_match_count = full_hits;
    s->total = full_hits;
    for (int i = 0; i < full_hits; i++)
      s->cand_is_full[i] = true;
    if (s->buf_len > 2) {
      const char *first_cands[MAX_CANDS];
      int first_hits = dict_lookup(s->buffer, 2, first_cands, MAX_CANDS);
      for (int i = 0; i < first_hits && s->total < MAX_CANDS; i++) {
        bool dup = false;
        for (int j = 0; j < s->total; j++) {
          if (strcmp(s->cand_ptrs[j], first_cands[i]) == 0) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          s->cand_ptrs[s->total] = first_cands[i];
          s->cand_is_full[s->total] = false;
          s->total++;
        }
      }
    }
  } else {
    // 奇数位:双拼编码必为偶数,此处 buffer 只能是某个编码的前缀。
    // 已完成音节(上一偶数位)的候选已经展示过,这里只做整串前缀联想,
    // 让进行中的音节参与候选(如 "nih" 关联到 nihc→你好)。
    // 扫描所有匹配编码,每个取最高频词,按全局词频取前 MAX_CANDS。
    // 结合用户持久化词频:用户真正用过的词优先于静态语料词频。
    // 字数上限规则:预测不能超过编码当前可表示的最大信息量。
    // 2 字母 = 1 音节 = 1 字,奇数 buf_len 只完成了部分音节,
    // 预测词字数上限 = (buf_len+1)/2,即联想编码长度 <= buf_len+1。
    // 例:1 码只预览单字,3 码最多 2 字词,5 码最多 3 字词。
    const char *tmp_words[MAX_CANDS];
    int64_t tmp_freqs[MAX_CANDS];
    int tmp_count = 0;
    // 只在 code_length == buf_len+1 的桶内前缀扫描(构建时按长度分桶),
    // 桶内条目长度一致且按 code 排序,前缀匹配段连续,无需再逐条判长。
    int target_len = s->buf_len + 1;
    int idx = dict_prefix_lower(s->buffer, s->buf_len, target_len);
    if (idx >= 0) {
      int bucket_end =
          G_DICT.len_starts[target_len] + G_DICT.len_counts[target_len];
      for (int i = idx; i < bucket_end; i++) {
        const DictCodeEntry *e = &G_DICT.entries[i];
        if (memcmp(G_DICT.code_pool + e->code_offset, s->buffer, s->buf_len) !=
            0)
          break;
        if (e->word_count <= 0)
          continue;
        const char *w = G_DICT.word_pool + G_DICT.word_offsets[e->word_start];
        int64_t f = (int64_t)get_user_count(w) * USERFREQ_BOOST +
                    dict_word_freq(e->word_start);
        if (tmp_count == MAX_CANDS && f <= tmp_freqs[MAX_CANDS - 1])
          continue;
        bool dup = false;
        for (int j = 0; j < tmp_count; j++) {
          if (strcmp(tmp_words[j], w) == 0) {
            dup = true;
            break;
          }
        }
        if (dup)
          continue;
        int j = tmp_count;
        while (j > 0 && tmp_freqs[j - 1] < f)
          j--;
        if (tmp_count < MAX_CANDS)
          tmp_count++;
        for (int k = tmp_count - 1; k > j; k--) {
          tmp_words[k] = tmp_words[k - 1];
          tmp_freqs[k] = tmp_freqs[k - 1];
        }
        tmp_words[j] = w;
        tmp_freqs[j] = f;
      }
    }
    for (int i = 0; i < tmp_count; i++) {
      s->cand_ptrs[i] = tmp_words[i];
      s->cand_is_full[i] = false;
    }
    s->total = tmp_count;
    s->full_match_count = 0;
  }

  if (s->total > 1) {
    int score[MAX_CANDS];
    for (int i = 0; i < s->total; i++)
      score[i] = get_user_count(s->cand_ptrs[i]);
    for (int i = 1; i < s->total; i++) {
      const char *tmp = s->cand_ptrs[i];
      bool tmp_full = s->cand_is_full[i];
      int sc = score[i];
      if (sc == 0 && !tmp_full)
        continue;
      int j = i - 1;
      while (j >= 0 && ((!s->cand_is_full[j] && tmp_full) ||
                        (s->cand_is_full[j] == tmp_full && score[j] < sc))) {
        s->cand_ptrs[j + 1] = s->cand_ptrs[j];
        s->cand_is_full[j + 1] = s->cand_is_full[j];
        score[j + 1] = score[j];
        j--;
      }
      s->cand_ptrs[j + 1] = tmp;
      s->cand_is_full[j + 1] = tmp_full;
      score[j + 1] = sc;
    }
    int new_full = 0;
    for (int i = 0; i < s->total && s->cand_is_full[i]; i++)
      new_full++;
    s->full_match_count = new_full;
  }

  memcpy(s->cand_cache_buf, s->buffer, s->buf_len);
  s->cand_cache_buf[s->buf_len] = '\0';
  s->cand_cache_len = s->buf_len;
  s->cand_cache_valid = true;
}

static const char *get_candidate(struct ime_state *s, int idx) {
  if (idx < 0 || idx >= s->total)
    return NULL;
  return s->cand_ptrs[idx];
}

static void update_preedit(struct ime_state *s) {
  if (!s->input_method)
    return;
  if (s->buf_len == 0) {
    clear_preedit(s);
    return;
  }
  compute_candidates(s);
  char text[512] = {0};
  size_t pos = preedit_append(text, sizeof(text), 0, "[%s] ", s->buffer);
  if (s->total > 0) {
    int start = s->page * PAGE_SIZE;
    int end = start + PAGE_SIZE;
    if (end > s->total)
      end = s->total;
    int pages = (s->total + PAGE_SIZE - 1) / PAGE_SIZE;
    for (int i = start; i < end; i++) {
      const char *c = get_candidate(s, i);
      if (c)
        pos = preedit_append(text, sizeof(text), pos, "%d.%s ", i - start + 1,
                             c);
    }
    if (pages > 1)
      preedit_append(text, sizeof(text), pos, "(%d/%d)", s->page + 1, pages);
  }
  // 无光标选区(0,0):避免应用把 preedit 渲染成高亮/下划线选框。
  zwp_input_method_v2_set_preedit_string(s->input_method, text, 0, 0);
  zwp_input_method_v2_commit(s->input_method, s->current_serial);
  wl_display_flush(s->display);
}

static void commit_candidate_at(struct ime_state *s, int real_idx) {
  const char *t = get_candidate(s, real_idx);
  if (!t)
    return;
  record_user_word(t, get_epoch_sec());
  zwp_input_method_v2_commit_string(s->input_method, t);
  bool is_full = (real_idx < s->total) ? s->cand_is_full[real_idx] : false;
  if (is_full || s->buf_len <= 2) {
    clear_preedit(s);
    reset_state(s);
  } else {
    memmove(s->buffer, s->buffer + 2, s->buf_len - 2 + 1);
    s->buf_len -= 2;
    s->page = 0;
    update_preedit(s);
  }
}

static void clear_composing(struct ime_state *s) {
  reset_state(s);
  update_preedit(s);
}

// --- Keyboard Grab ---

static void handle_grab_keymap(void *data,
                               struct zwp_input_method_keyboard_grab_v2 *grab,
                               uint32_t format, int32_t fd, uint32_t size) {
  (void)grab;
  struct ime_state *s = data;
  if (s->virtual_keyboard) {
    zwp_virtual_keyboard_v1_keymap(s->virtual_keyboard, format, fd, size);
    s->keymap_set = true;
  }
  close(fd);
}

static void handle_grab_key(void *data,
                            struct zwp_input_method_keyboard_grab_v2 *grab,
                            uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t key_state) {
  (void)grab;
  (void)serial;
  struct ime_state *s = data;
  bool pressed = (key_state == 1);
  bool shift = (s->active_mods & 0x01) != 0;

  char c = evdev_to_char(key);
  bool composing = (s->buf_len > 0);
  bool consume = false;

  // 0. 之前透传的按键，Release 必须透传
  if (key < 256 && key_state == 0 && s->key_passthrough_pressed[key]) {
    passthrough_key(s, time, key, 0);
    return;
  }

  // 0.1 之前被输入法消费的按键,Release 也必须消费,保证 Press/Release 成对
  if (key < 256 && key_state == 0 && s->key_consumed_pressed[key]) {
    s->key_consumed_pressed[key] = false;
    if (key == KEY_BACKSPACE) {
      s->backspace_active = false;
      struct itimerspec its = {0};
      timerfd_settime(s->repeat_timer_fd, 0, &its, NULL);
    }
    return;
  }

  // Shift 组合跟踪：shift 按下期间有其他非 shift 键按下，标记为组合
  if (s->shift_count > 0 && key_state == 1 && key != KEY_LEFTSHIFT &&
      key != KEY_RIGHTSHIFT)
    s->shift_combo = true;

  // 非退格键按下时，取消退格 repeat 并 disarm timerfd。
  // [zh] 指示若仍挂起且尚未组字,立即撤下并清空 preedit 再放行按键:
  // 无按键时由主循环 400ms 超时负责消失;一旦开始按键则立即消失,
  // 避免 GTK 等客户端把随后透传的字符并入指示的 preedit 区域,随清屏被一并删除。
  if (key != KEY_BACKSPACE && key_state == 1) {
    s->backspace_active = false;
    if (s->zh_indicator_active && s->buf_len == 0) {
      s->zh_indicator_active = false;
      clear_preedit(s);
    }
    struct itimerspec its = {0};
    timerfd_settime(s->repeat_timer_fd, 0, &its, NULL);
  }

  // 1. Ctrl / Alt / Super：始终透传
  if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL || key == KEY_LEFTALT ||
      key == KEY_RIGHTALT || key == KEY_LEFTMETA || key == KEY_RIGHTMETA) {
    if (composing)
      clear_composing(s);
    passthrough_key(s, time, key, key_state);
    return;
  }

  // Shift：单独按下释放切换中英文，组合使用正常（fcitx5 行为）
  if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
    if (key_state == 1) {
      s->shift_count++;
      s->shift_combo = false;
      if (s->active_mods & ~0x01)
        s->shift_combo = true;
    } else if (s->shift_count > 0) {
      s->shift_count--;
      if (s->shift_count == 0 && !s->shift_combo) {
        if (composing) {
          zwp_input_method_v2_commit_string(s->input_method, s->buffer);
          clear_preedit(s);
          reset_state(s);
        }
        s->mode_ascii = !s->mode_ascii;
        if (!s->mode_ascii) {
          zwp_input_method_v2_set_preedit_string(s->input_method, "[zh]", 0, 0);
          zwp_input_method_v2_commit(s->input_method, s->current_serial);
          s->zh_indicator_active = true;
          s->zh_indicator_time = get_time_ms();
        } else {
          clear_preedit(s);
        }
      }
    }
    return;
  }

  // Ctrl+Shift+. 切换全角/半角标点
  if (key == KEY_DOT && key_state == 1 && (s->active_mods & 0x05) == 0x05)
    s->punct_fullwidth = !s->punct_fullwidth;

  // 英文模式：全部透传
  if (s->mode_ascii) {
    passthrough_key(s, time, key, key_state);
    return;
  }

  // 3. 全角标点
  // 仅无修饰键或仅 Shift 时转全角标点;Ctrl/Alt/Super 组合必须透传
  if (s->punct_fullwidth && (s->active_mods & ~0x01) == 0) {
    const char *punct_str = NULL;
    if (key == KEY_COMMA)
      punct_str = shift ? "《" : "，";
    else if (key == KEY_DOT)
      punct_str = shift ? "》" : "。";
    else if (key == KEY_SLASH)
      punct_str = shift ? "？" : "、";
    else if (key == KEY_SEMICOLON)
      punct_str = shift ? "：" : "；";
    else if (key == KEY_1 && shift)
      punct_str = "！";
    else if (key == KEY_4 && shift)
      punct_str = "￥";
    else if (key == KEY_6 && shift)
      punct_str = "……";
    else if (key == KEY_9 && shift)
      punct_str = "（";
    else if (key == KEY_0 && shift)
      punct_str = "）";
    else if (key == KEY_BACKSLASH)
      punct_str = shift ? "|" : "、";
    else if (key == KEY_GRAVE && shift)
      punct_str = "～";
    else if (key == KEY_LEFTBRACE)
      punct_str = shift ? "『" : "【";
    else if (key == KEY_RIGHTBRACE)
      punct_str = shift ? "』" : "】";
    else if (key == KEY_APOSTROPHE) {
      if (shift) {
        punct_str = s->double_quote_open ? "\u201d" : "\u201c";
        if (pressed)
          s->double_quote_open = !s->double_quote_open;
      } else {
        punct_str = s->single_quote_open ? "\u2019" : "\u2018";
        if (pressed)
          s->single_quote_open = !s->single_quote_open;
      }
    }
    if (punct_str) {
      consume = true;
      if (pressed) {
        if (composing) {
          if (s->total > 0)
            commit_candidate_at(s, s->page * PAGE_SIZE);
          else
            zwp_input_method_v2_commit_string(s->input_method, s->buffer);
        }
        zwp_input_method_v2_commit_string(s->input_method, punct_str);
        clear_preedit(s);
        reset_state(s);
      }
    }
  }

  // Shift+字母:先结清挂起的 preedit,再透传。
  // 单独 Shift 按下/释放用于切换中英文,按下时不能清 preedit,因此这里补结清;
  // Ctrl/Alt/Super 组合在对应修饰键按下时已由 clear_composing 处理,无需重复。
  if (!consume && pressed && composing && shift && c >= 'a' && c <= 'z' &&
      (s->active_mods & ~0x01) == 0) {
    if (s->total > 0)
      commit_candidate_at(s, s->page * PAGE_SIZE);
    else
      zwp_input_method_v2_commit_string(s->input_method, s->buffer);
    clear_preedit(s);
    reset_state(s);
  }

  // 修饰键组合全部透传
  if (!consume && s->active_mods != 0) {
    passthrough_key(s, time, key, key_state);
    return;
  }

  // A. 字母键
  if (c >= 'a' && c <= 'z') {
    consume = true;
    if (pressed && s->buf_len < 60) {
      s->buffer[s->buf_len++] = c;
      s->buffer[s->buf_len] = '\0';
      s->page = 0;
      update_preedit(s);
    }
  }
  // B. 空格
  else if (key == KEY_SPACE && composing) {
    consume = true;
    if (pressed) {
      if (s->total > 0)
        commit_candidate_at(s, s->page * PAGE_SIZE);
      else {
        zwp_input_method_v2_commit_string(s->input_method, s->buffer);
        clear_composing(s);
      }
    }
  }
  // C. 数字键 1-3:仅当对应候选存在时消费,否则走兜底
  else if (key >= KEY_1 && key <= KEY_3 && composing) {
    int idx = key - KEY_1;
    int real = s->page * PAGE_SIZE + idx;
    if (real < s->total) {
      consume = true;
      if (pressed)
        commit_candidate_at(s, real);
    }
  }
  // D. 退格：Press 立即删一字 + arm timerfd, Release 停 timerfd
  else if (key == KEY_BACKSPACE && composing) {
    consume = true;
    if (key_state == 1) {
      s->backspace_active = true;
      if (s->buf_len > 0) {
        s->buf_len--;
        s->buffer[s->buf_len] = '\0';
        s->page = 0;
        update_preedit(s);
      }
      uint64_t interval_ns =
          (s->repeat_rate > 0) ? (1000000000ULL / s->repeat_rate) : 33000000ULL;
      uint64_t delay_ns = (uint64_t)s->repeat_delay * 1000000ULL;
      struct itimerspec its = {
          .it_interval = {.tv_sec = (time_t)(interval_ns / 1000000000ULL),
                          .tv_nsec = (long)(interval_ns % 1000000000ULL)},
          .it_value = {.tv_sec = (time_t)(delay_ns / 1000000000ULL),
                       .tv_nsec = (long)(delay_ns % 1000000000ULL)}};
      timerfd_settime(s->repeat_timer_fd, 0, &its, NULL);
    } else if (key_state == 0) {
      s->backspace_active = false;
      struct itimerspec its = {0};
      timerfd_settime(s->repeat_timer_fd, 0, &its, NULL);
    }
  }
  // E. Escape
  else if (key == KEY_ESC && composing) {
    consume = true;
    if (pressed)
      clear_composing(s);
  }
  // F. = 翻页下一页
  else if (key == KEY_EQUAL && composing && s->total > 0) {
    consume = true;
    if (pressed) {
      int pages = (s->total + PAGE_SIZE - 1) / PAGE_SIZE;
      if (s->page + 1 < pages) {
        s->page++;
        update_preedit(s);
      }
    }
  }
  // G. - 翻页上一页
  else if (key == KEY_MINUS && composing && s->total > 0) {
    consume = true;
    if (pressed) {
      if (s->page > 0) {
        s->page--;
        update_preedit(s);
      }
    }
  }
  // H. 翻页下一页
  else if ((key == KEY_PAGEDOWN || key == KEY_DOWN) && composing &&
           s->total > 0) {
    consume = true;
    if (pressed) {
      int pages = (s->total + PAGE_SIZE - 1) / PAGE_SIZE;
      if (s->page + 1 < pages) {
        s->page++;
        update_preedit(s);
      }
    }
  }
  // I. 翻页上一页
  else if ((key == KEY_PAGEUP || key == KEY_UP) && composing && s->total > 0) {
    consume = true;
    if (pressed) {
      if (s->page > 0) {
        s->page--;
        update_preedit(s);
      }
    }
  }
  // J. Enter
  else if (key == KEY_ENTER && composing) {
    consume = true;
    if (pressed) {
      zwp_input_method_v2_commit_string(s->input_method, s->buffer);
      clear_preedit(s);
      reset_state(s);
    }
  }

  // 记录被消费的按下事件,Release 时成对消费,避免把孤立的 Release 透传给应用。
  if (consume && pressed && key < 256)
    s->key_consumed_pressed[key] = true;

  // 兜底透传:任何未消费键先结清挂起的 preedit 再放行。
  // 若在组字中按下,先提交当前候选(无候选则提交编码),保证 GTK/Qt 等
  // 客户端不会在 preedit 挂起时收到按键而把字符并入 preedit 区域被吞。
  if (!consume) {
    if (pressed && composing) {
      if (s->total > 0)
        commit_candidate_at(s, s->page * PAGE_SIZE);
      else {
        zwp_input_method_v2_commit_string(s->input_method, s->buffer);
        clear_preedit(s);
        reset_state(s);
      }
    }
    passthrough_key(s, time, key, key_state);
  }
}

static void handle_grab_modifiers(
    void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial,
    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
    uint32_t group) {
  (void)grab;
  (void)serial;
  struct ime_state *s = data;
  s->active_mods = mods_depressed;
  if (s->virtual_keyboard && s->keymap_set)
    zwp_virtual_keyboard_v1_modifiers(s->virtual_keyboard, mods_depressed,
                                      mods_latched, mods_locked, group);
}

static void
handle_grab_repeat_info(void *data,
                        struct zwp_input_method_keyboard_grab_v2 *grab,
                        int32_t rate, int32_t delay) {
  (void)grab;
  struct ime_state *s = data;
  s->repeat_rate = (rate > 0) ? (uint32_t)rate : 30;
  s->repeat_delay = (delay > 0) ? (uint32_t)delay : 500;
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    .keymap = handle_grab_keymap,
    .key = handle_grab_key,
    .modifiers = handle_grab_modifiers,
    .repeat_info = handle_grab_repeat_info,
};

// --- Input Method v2 ---

static void handle_im_activate(void *d, struct zwp_input_method_v2 *im) {
  (void)d;
  (void)im;
}

static void handle_im_deactivate(void *data, struct zwp_input_method_v2 *im) {
  (void)im;
  struct ime_state *s = data;
  clear_composing(s);
}

static void handle_im_done(void *data, struct zwp_input_method_v2 *im) {
  (void)im;
  struct ime_state *s = data;
  s->current_serial++;
}

static void handle_im_surrounding_text(void *d, struct zwp_input_method_v2 *im,
                                       const char *t, uint32_t c, uint32_t a) {
  (void)d;
  (void)im;
  (void)t;
  (void)c;
  (void)a;
}

static void handle_im_text_change_cause(void *d, struct zwp_input_method_v2 *im,
                                        uint32_t c) {
  (void)d;
  (void)im;
  (void)c;
}

static void handle_im_content_type(void *d, struct zwp_input_method_v2 *im,
                                   uint32_t h, uint32_t p) {
  (void)d;
  (void)im;
  (void)h;
  (void)p;
}

static void handle_im_unavailable(void *d, struct zwp_input_method_v2 *im) {
  (void)d;
  (void)im;
  fprintf(stderr, "other ime detected\n");
  exit(1);
}

static const struct zwp_input_method_v2_listener im_listener = {
    .activate = handle_im_activate,
    .deactivate = handle_im_deactivate,
    .surrounding_text = handle_im_surrounding_text,
    .text_change_cause = handle_im_text_change_cause,
    .content_type = handle_im_content_type,
    .done = handle_im_done,
    .unavailable = handle_im_unavailable,
};

// --- Registry ---

static void handle_global(void *data, struct wl_registry *reg, uint32_t name,
                          const char *iface, uint32_t ver) {
  struct ime_state *s = data;
  (void)ver;
  if (strcmp(iface, wl_seat_interface.name) == 0)
    s->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
    s->im_manager =
        wl_registry_bind(reg, name, &zwp_input_method_manager_v2_interface, 1);
  else if (strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
    s->vk_manager = wl_registry_bind(
        reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void handle_global_remove(void *d, struct wl_registry *r, uint32_t n) {
  (void)d;
  (void)r;
  (void)n;
}

static const struct wl_registry_listener reg_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

int main(int argc, char *argv[]) {
  (void)argc;

  char dict_path[512] = {0};
  bool dict_ok = false;

  ssize_t len = readlink("/proc/self/exe", dict_path, sizeof(dict_path) - 16);
  if (len > 0) {
    dict_path[len] = '\0';
    char *slash = strrchr(dict_path, '/');
    if (slash) {
      strcpy(slash + 1, "xhup.dict");
      dict_ok = dict_load(dict_path);
      strcpy(slash + 1, "xhup.userfreq");
      snprintf(G_USERFREQ_PATH, sizeof(G_USERFREQ_PATH), "%s", dict_path);
      user_hash_init();
      user_freq_load(G_USERFREQ_PATH);
    }
  }

  if (!dict_ok)
    dict_ok = dict_load("xhup.dict");
  if (!dict_ok && argc > 1)
    dict_ok = dict_load(argv[1]);

  if (!dict_ok) {
    fprintf(stderr, "cannot load xhup.dict\n");
    return 1;
  }

  G_LAST_SAVE_MS = get_time_ms();

  signal(SIGTERM, handle_exit_signal);
  signal(SIGINT, handle_exit_signal);

  struct ime_state s = {0};
  s.mode_ascii = true;
  s.punct_fullwidth = false;
  s.running = true;
  s.repeat_rate = 30;
  s.repeat_delay = 500;

  s.repeat_timer_fd =
      timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  s.display = wl_display_connect(NULL);
  if (!s.display) {
    fprintf(stderr, "no wayland display\n");
    dict_unload();
    return 1;
  }

  s.registry = wl_display_get_registry(s.display);
  wl_registry_add_listener(s.registry, &reg_listener, &s);
  wl_display_roundtrip(s.display);

  if (!s.im_manager || !s.seat || !s.vk_manager) {
    fprintf(stderr,
            "input-method-v2 or virtual-keyboard-v1 protocol missing\n");
    wl_display_disconnect(s.display);
    dict_unload();
    return 1;
  }

  s.virtual_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
      s.vk_manager, s.seat);
  s.input_method =
      zwp_input_method_manager_v2_get_input_method(s.im_manager, s.seat);
  zwp_input_method_v2_add_listener(s.input_method, &im_listener, &s);
  s.keyboard_grab = zwp_input_method_v2_grab_keyboard(s.input_method);
  zwp_input_method_keyboard_grab_v2_add_listener(s.keyboard_grab,
                                                 &grab_listener, &s);

  int display_fd = wl_display_get_fd(s.display);
  struct pollfd fds[2] = {{.fd = display_fd, .events = POLLIN},
                          {.fd = s.repeat_timer_fd, .events = POLLIN}};

  while (s.running) {
    wl_display_flush(s.display);
    if (wl_display_dispatch_pending(s.display) < 0)
      break;

    int poll_timeout = -1;
    if (s.zh_indicator_active) {
      uint64_t elapsed = get_time_ms() - s.zh_indicator_time;
      if (elapsed >= 400) {
        s.zh_indicator_active = false;
        if (s.buf_len == 0) {
          clear_preedit(&s);
        }
      } else {
        poll_timeout = (int)(400 - elapsed);
      }
    }

    int ret = poll(fds, 2, poll_timeout);
    if (ret < 0)
      break;
    if (G_SIGNAL_FLAG)
      break;
    if (fds[0].revents & POLLIN) {
      if (wl_display_dispatch(s.display) < 0)
        break;
    }
    // 退格 repeat 信号
    if (fds[1].revents & POLLIN) {
      uint64_t exp;
      while (read(s.repeat_timer_fd, &exp, sizeof(exp)) > 0) {
      }
      if (s.backspace_active && s.buf_len > 0) {
        s.buf_len--;
        s.buffer[s.buf_len] = '\0';
        s.page = 0;
        update_preedit(&s);
      }
    }

    // 用户词频定时落盘(最多每 5 秒一次)
    if (G_USERFREQ_DIRTY) {
      uint64_t now = get_time_ms();
      if (now - G_LAST_SAVE_MS >= 180000) {
        user_freq_save();
        G_LAST_SAVE_MS = now;
      }
    }
  }

  s.running = false;

  user_freq_save();

  if (s.repeat_timer_fd >= 0)
    close(s.repeat_timer_fd);

  if (s.virtual_keyboard)
    zwp_virtual_keyboard_v1_destroy(s.virtual_keyboard);
  wl_display_disconnect(s.display);
  dict_unload();
  return 0;
}
