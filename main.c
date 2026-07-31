#include "dict.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define MAX_CANDS 30

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

  // 模式与标点
  bool mode_ascii;
  int shift_count;
  bool shift_combo;
  bool key_passthrough_pressed[256];
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

#define PAGE_SIZE 5

// --- 最近输入词频重排 ---
#define MAX_RECENT_WORDS 2048
typedef struct {
  char word[64];
  int count;
  uint64_t last_used;
} RecentWord;

static RecentWord G_RECENT_WORDS[MAX_RECENT_WORDS];
static int G_RECENT_COUNT = 0;

static void record_recent_word(const char *word, uint64_t now_ms) {
  if (!word || !word[0])
    return;
  for (int i = 0; i < G_RECENT_COUNT; i++) {
    if (strcmp(G_RECENT_WORDS[i].word, word) == 0) {
      G_RECENT_WORDS[i].count++;
      G_RECENT_WORDS[i].last_used = now_ms;
      return;
    }
  }
  if (G_RECENT_COUNT < MAX_RECENT_WORDS) {
    strncpy(G_RECENT_WORDS[G_RECENT_COUNT].word, word, 63);
    G_RECENT_WORDS[G_RECENT_COUNT].word[63] = '\0';
    G_RECENT_WORDS[G_RECENT_COUNT].count = 1;
    G_RECENT_WORDS[G_RECENT_COUNT].last_used = now_ms;
    G_RECENT_COUNT++;
  } else {
    int oldest_idx = 0;
    uint64_t min_time = G_RECENT_WORDS[0].last_used;
    for (int i = 1; i < G_RECENT_COUNT; i++) {
      if (G_RECENT_WORDS[i].last_used < min_time) {
        min_time = G_RECENT_WORDS[i].last_used;
        oldest_idx = i;
      }
    }
    strncpy(G_RECENT_WORDS[oldest_idx].word, word, 63);
    G_RECENT_WORDS[oldest_idx].word[63] = '\0';
    G_RECENT_WORDS[oldest_idx].count = 1;
    G_RECENT_WORDS[oldest_idx].last_used = now_ms;
  }
}

static int get_recent_count(const char *word) {
  for (int i = 0; i < G_RECENT_COUNT; i++) {
    if (strcmp(G_RECENT_WORDS[i].word, word) == 0)
      return G_RECENT_WORDS[i].count;
  }
  return 0;
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
  s->total = 0;
  s->full_match_count = 0;
  if (s->buf_len == 0)
    return;

  if (s->buf_len % 2 == 0) {
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
    if (s->buf_len == 1) {
      int exact_hits = dict_lookup(s->buffer, 1, s->cand_ptrs, MAX_CANDS);
      s->total = exact_hits;
      s->full_match_count = exact_hits;
      for (int i = 0; i < exact_hits; i++)
        s->cand_is_full[i] = true;
      int idx = dict_prefix_lower(s->buffer, 1);
      if (idx >= 0) {
        for (int i = 0; i < MAX_CANDS && (idx + i) < (int)G_DICT.entry_count &&
                        s->total < MAX_CANDS;
             i++) {
          const DictCodeEntry *e = &G_DICT.entries[idx + i];
          const char *code = G_DICT.code_pool + e->code_offset;
          if (e->code_length >= 1 && code[0] == s->buffer[0]) {
            if (e->code_length == 1)
              continue;
            if (e->word_count > 0) {
              for (int k = 0; k < e->word_count && s->total < MAX_CANDS; k++) {
                const char *w =
                    G_DICT.word_pool + G_DICT.word_offsets[e->word_start + k];
                bool dup = false;
                for (int j = 0; j < s->total; j++) {
                  if (strcmp(s->cand_ptrs[j], w) == 0) {
                    dup = true;
                    break;
                  }
                }
                if (!dup) {
                  s->cand_ptrs[s->total] = w;
                  s->cand_is_full[s->total] = false;
                  s->total++;
                  break;
                }
              }
            }
          } else
            break;
        }
      }
    } else {
      int full_hits =
          dict_lookup(s->buffer, s->buf_len - 1, s->cand_ptrs, MAX_CANDS);
      s->full_match_count = 0;
      s->total = full_hits;
      for (int i = 0; i < full_hits; i++)
        s->cand_is_full[i] = false;
    }
  }

  if (s->total > 1) {
    int score[MAX_CANDS];
    for (int i = 0; i < s->total; i++)
      score[i] = get_recent_count(s->cand_ptrs[i]);
    for (int i = 1; i < s->total; i++) {
      const char *tmp = s->cand_ptrs[i];
      bool tmp_full = s->cand_is_full[i];
      int sc = score[i];
      if (sc == 0 && !tmp_full)
        continue;
      int j = i - 1;
      while (j >= 0 && ((!s->cand_is_full[j] && tmp_full) ||
                        (s->cand_is_full[j] == tmp_full &&
                         score[j] < sc))) {
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
  size_t pos = snprintf(text, sizeof(text), "[%s] ", s->buffer);
  if (s->total > 0) {
    int start = s->page * PAGE_SIZE;
    int end = start + PAGE_SIZE;
    if (end > s->total)
      end = s->total;
    int pages = (s->total + PAGE_SIZE - 1) / PAGE_SIZE;
    for (int i = start; i < end; i++) {
      const char *c = get_candidate(s, i);
      if (c)
        pos += snprintf(text + pos, sizeof(text) - pos, "%d.%s ", i - start + 1,
                        c);
    }
    if (pages > 1)
      pos += snprintf(text + pos, sizeof(text) - pos, "(%d/%d)", s->page + 1,
                      pages);
  }
  uint32_t len = strlen(text);
  zwp_input_method_v2_set_preedit_string(s->input_method, text, 0, len);
  zwp_input_method_v2_commit(s->input_method, s->current_serial);
  wl_display_flush(s->display);
}

static void commit_candidate(struct ime_state *s, int page_idx) {
  int real_idx = s->page * PAGE_SIZE + page_idx;
  const char *t = get_candidate(s, real_idx);
  if (!t)
    return;
  record_recent_word(t, get_time_ms());
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
  bool ctrl = (s->active_mods & 0x04) != 0;
  bool alt = (s->active_mods & 0x08) != 0;

  char c = evdev_to_char(key);
  bool composing = (s->buf_len > 0);
  bool consume = false;

  // 0. 之前透传的按键，Release 必须透传
  if (key < 256 && key_state == 0 && s->key_passthrough_pressed[key]) {
    passthrough_key(s, time, key, 0);
    return;
  }

  // Shift 组合跟踪：shift 按下期间有其他非 shift 键按下，标记为组合
  if (s->shift_count > 0 && key_state == 1 && key != KEY_LEFTSHIFT &&
      key != KEY_RIGHTSHIFT)
    s->shift_combo = true;

  // 非退格键按下时，取消退格 repeat 并 disarm timerfd
  if (key != KEY_BACKSPACE && key_state == 1) {
    s->backspace_active = false;
    s->zh_indicator_active = false;
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
          zwp_input_method_v2_set_preedit_string(s->input_method, "[zh]", 0, 4);
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
  if (s->punct_fullwidth && !ctrl && !alt) {
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
        if (composing && s->total > 0)
          commit_candidate(s, 0);
        zwp_input_method_v2_commit_string(s->input_method, punct_str);
        clear_preedit(s);
      }
    }
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
        commit_candidate(s, 0);
      else {
        zwp_input_method_v2_commit_string(s->input_method, s->buffer);
        clear_composing(s);
      }
    }
  }
  // C. 数字键 1-5
  else if (key >= KEY_1 && key <= KEY_5 && composing) {
    consume = true;
    if (pressed) {
      int idx = key - KEY_1;
      int real = s->page * PAGE_SIZE + idx;
      if (real < s->total)
        commit_candidate(s, idx);
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
  // F. 翻页下一页
  else if ((key == KEY_EQUAL || key == KEY_DOT || key == KEY_PAGEDOWN ||
            key == KEY_DOWN) &&
           composing && s->total > 0) {
    consume = true;
    if (pressed) {
      int pages = (s->total + PAGE_SIZE - 1) / PAGE_SIZE;
      if (s->page + 1 < pages) {
        s->page++;
        update_preedit(s);
      }
    }
  }
  // G. 翻页上一页
  else if ((key == KEY_MINUS || key == KEY_COMMA || key == KEY_PAGEUP ||
            key == KEY_UP) &&
           composing && s->total > 0) {
    consume = true;
    if (pressed) {
      if (s->page > 0) {
        s->page--;
        update_preedit(s);
      }
    }
  }
  // H. Enter
  else if (key == KEY_ENTER && composing) {
    consume = true;
    if (pressed) {
      zwp_input_method_v2_commit_string(s->input_method, s->buffer);
      clear_preedit(s);
      reset_state(s);
    }
  }

  if (!consume)
    passthrough_key(s, time, key, key_state);
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
      strcpy(slash + 1, "xiaohe.dict");
      dict_ok = dict_load(dict_path);
    }
  }

  if (!dict_ok)
    dict_ok = dict_load("xiaohe.dict");
  if (!dict_ok && argc > 1)
    dict_ok = dict_load(argv[1]);

  if (!dict_ok) {
    fprintf(stderr, "cannot load xiaohe.dict\n");
    return 1;
  }

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
  }

  s.running = false;

  if (s.repeat_timer_fd >= 0)
    close(s.repeat_timer_fd);

  if (s.virtual_keyboard)
    zwp_virtual_keyboard_v1_destroy(s.virtual_keyboard);
  wl_display_disconnect(s.display);
  dict_unload();
  return 0;
}
