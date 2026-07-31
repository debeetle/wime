/*
 * fcitx5-xhup: standalone 小鹤双拼 input method addon for fcitx5.
 *
 * Depends ONLY on fcitx5 core + a static dictionary (wlime's XHD0 format).
 * No fcitx5-chinese-addons, no libime.
 *
 * The addon:
 *   - loads xhup.dict (~/.local/share/fcitx5/xhup.dict, or wlime's xiaohe.dict)
 *   - direct code -> word lookup as you type (小鹤双拼 codes, 2 keys per syllable)
 *   - candidate paging, selection by digit/space, recency reordering
 */

#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dict.h"

#define XHUP_PAGE_SIZE 5
#define XHUP_MAX_CANDS DICT_MAX_CANDIDATES

namespace fcitx {

class XhupEngine;

// Recency table: word -> number of times selected by the user.
static std::unordered_map<std::string, int> G_RECENT;

static void recordRecent(const char *word) { ++G_RECENT[std::string(word)]; }

static int recentCount(const char *word) {
    auto it = G_RECENT.find(word);
    return it == G_RECENT.end() ? 0 : it->second;
}

// Candidate that commits its text when selected (by key or by mouse click).
class XhupCandidateWord : public CandidateWord {
public:
    XhupCandidateWord(XhupEngine *engine, std::string word, bool isFull)
        : CandidateWord(Text(word)), engine_(engine), word_(std::move(word)),
          isFull_(isFull) {}

    void select(InputContext *ic) const override;

private:
    XhupEngine *engine_;
    std::string word_;
    bool isFull_;
};

// Per-input-context composing state.
class XhupState : public InputContextProperty {
public:
    std::string buffer; // current code being typed
    std::vector<const char *> cands; // candidates (pointers into dict pool)
    std::vector<bool> candsIsFull;   // exact-match vs merged/partial candidate
    int page = 0;

    void reset() {
        buffer.clear();
        cands.clear();
        candsIsFull.clear();
        page = 0;
    }

    bool isFull(size_t idx) const { return idx < candsIsFull.size() && candsIsFull[idx]; }
};

class XhupEngine : public InputMethodEngine {
public:
    XhupEngine(Instance *instance);

    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;
    void reset(const InputMethodEntry &entry, InputContextEvent &event) override;

    // Called from XhupCandidateWord::select (mouse click / UI selection).
    void commitSelected(InputContext &ic, const std::string &word, bool isFull);

    XhupState *state(InputContext &ic) { return ic.propertyFor(&stateFactory_); }

    void updateUI(InputContext &ic);
    void clearComposing(InputContext &ic);
    void commitRaw(InputContext &ic);

private:
    void computeCandidates(XhupState &s);
    void commitSelection(InputContext &ic, XhupState &s, int pageIdx);

    Instance *instance_;
    SimpleInputContextPropertyFactory<XhupState> stateFactory_;
    bool dictOk_;
};

// ---------------------------------------------------------------------------

void XhupCandidateWord::select(InputContext *ic) const {
    engine_->commitSelected(*ic, word_, isFull_);
}

XhupEngine::XhupEngine(Instance *instance) : instance_(instance), dictOk_(false) {
    instance_->inputContextManager().registerProperty("xhupState", &stateFactory_);

    const char *home = getenv("HOME");
    const std::vector<std::string> dictPaths = {
        std::string(home ? home : "") + "/.local/share/fcitx5/xhup.dict",
        std::string(home ? home : "") + "/garden/wlime/xiaohe.dict",
        "xiaohe.dict",
    };
    for (const auto &path : dictPaths) {
        if (dict_load(path.c_str())) {
            dictOk_ = true;
            FCITX_INFO() << "Xhup: loaded dictionary from " << path;
            break;
        }
    }
    if (!dictOk_) {
        FCITX_ERROR() << "Xhup: cannot load dictionary";
    }
}

void XhupEngine::reset(const InputMethodEntry & /*entry*/, InputContextEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    auto *s = state(*ic);
    if (s && !s->buffer.empty()) {
        commitRaw(*ic);
    }
}

void XhupEngine::keyEvent(const InputMethodEntry & /*entry*/, KeyEvent &keyEvent) {
    auto *ic = keyEvent.inputContext();
    if (!ic) {
        return;
    }
    auto *s = state(*ic);
    const auto key = keyEvent.key();
    const bool release = keyEvent.isRelease();
    const bool composing = !s->buffer.empty();

    // Ignore releases. Consume them while composing so the client never sees a
    // stray release of a key we handled on press.
    if (release) {
        if (composing) {
            keyEvent.filter();
        }
        return;
    }

    const auto states = key.states();
    const KeyStates ctrlAltSuper = KeyStates(KeyState::Ctrl) | KeyState::Alt |
                                   KeyState::Super;
    if (states & ctrlAltSuper) {
        return; // shortcuts / other modifiers: let through
    }

    const KeySym sym = key.sym();

    // a-z extends the current code (or starts composing).
    if (sym >= FcitxKey_a && sym <= FcitxKey_z) {
        if (dictOk_) {
            s->buffer.push_back(static_cast<char>(sym - FcitxKey_a + 'a'));
            s->page = 0;
            keyEvent.filter();
            updateUI(*ic);
        }
        return;
    }

    if (!composing) {
        return; // nothing composed yet: pass through everything else
    }

    // ---- composing from here on ----

    // Space: select first candidate, or commit raw code if none.
    if (sym == FcitxKey_space) {
        keyEvent.filter();
        if (!s->cands.empty()) {
            commitSelection(*ic, *s, 0);
        } else {
            commitRaw(*ic);
        }
        return;
    }

    // Digits 1..PAGE_SIZE select the candidate at that position on this page.
    if (key.isDigit()) {
        const int d = key.digit();
        if (d >= 1 && d <= XHUP_PAGE_SIZE) {
            const int real = s->page * XHUP_PAGE_SIZE + (d - 1);
            if (real < static_cast<int>(s->cands.size())) {
                keyEvent.filter();
                commitSelection(*ic, *s, d - 1);
                return;
            }
        }
        // Digit not used for selection: commit the raw code and let the digit
        // fall through to the client.
        commitRaw(*ic);
        return;
    }

    switch (sym) {
    case FcitxKey_BackSpace:
        keyEvent.filter();
        if (!s->buffer.empty()) {
            s->buffer.pop_back();
            s->page = 0;
            updateUI(*ic);
        }
        return;
    case FcitxKey_Escape:
        keyEvent.filter();
        clearComposing(*ic);
        return;
    case FcitxKey_Return:
    case FcitxKey_KP_Enter:
        keyEvent.filter();
        commitRaw(*ic);
        return;
    default:
        break;
    }

    // Paging.
    if (sym == FcitxKey_equal || sym == FcitxKey_period ||
        sym == FcitxKey_Page_Down || sym == FcitxKey_Down) {
        keyEvent.filter();
        const int pages = (static_cast<int>(s->cands.size()) + XHUP_PAGE_SIZE - 1) /
                          XHUP_PAGE_SIZE;
        if (s->page + 1 < pages) {
            ++s->page;
            updateUI(*ic);
        }
        return;
    }
    if (sym == FcitxKey_minus || sym == FcitxKey_comma ||
        sym == FcitxKey_Page_Up || sym == FcitxKey_Up) {
        keyEvent.filter();
        if (s->page > 0) {
            --s->page;
            updateUI(*ic);
        }
        return;
    }

    // Any other key while composing: commit the raw code, then let the key
    // pass through to the client (like fcitx pinyin does).
    commitRaw(*ic);
}

void XhupEngine::commitSelected(InputContext &ic, const std::string &word,
                                bool isFull) {
    auto *s = state(ic);
    ic.commitString(word);
    recordRecent(word.c_str());
    if (isFull || s->buffer.size() <= 2) {
        clearComposing(ic);
    } else {
        s->buffer.erase(0, 2); // consume one syllable, keep composing
        s->page = 0;
        updateUI(ic);
    }
}

void XhupEngine::commitSelection(InputContext &ic, XhupState &s, int pageIdx) {
    const int real = s.page * XHUP_PAGE_SIZE + pageIdx;
    if (real < 0 || real >= static_cast<int>(s.cands.size())) {
        return;
    }
    commitSelected(ic, std::string(s.cands[real]), s.isFull(real));
}

void XhupEngine::commitRaw(InputContext &ic) {
    auto *s = state(ic);
    if (!s->buffer.empty()) {
        ic.commitString(s->buffer);
    }
    clearComposing(ic);
}

void XhupEngine::clearComposing(InputContext &ic) {
    auto *s = state(ic);
    s->reset();
    ic.inputPanel().reset();
    ic.updatePreedit();
    ic.updateUserInterface(UserInterfaceComponent::InputPanel);
}

void XhupEngine::computeCandidates(XhupState &s) {
    s.cands.clear();
    s.candsIsFull.clear();
    const int n = static_cast<int>(s.buffer.size());
    if (n == 0) {
        return;
    }

    const char *out[XHUP_MAX_CANDS];

    if (n % 2 == 0) {
        // Exact match of the whole code (phrases and single chars).
        const int full = dict_lookup(s.buffer.c_str(), n, out, XHUP_MAX_CANDS);
        for (int i = 0; i < full; ++i) {
            s.cands.push_back(out[i]);
            s.candsIsFull.push_back(true);
        }
        // While composing a longer phrase, also offer the first single char.
        if (n > 2) {
            const char *first[XHUP_MAX_CANDS];
            const int fh = dict_lookup(s.buffer.c_str(), 2, first, XHUP_MAX_CANDS);
            for (int i = 0; i < fh &&
                            static_cast<int>(s.cands.size()) < XHUP_MAX_CANDS;
                 ++i) {
                bool dup = false;
                for (const char *c : s.cands) {
                    if (std::strcmp(c, first[i]) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    s.cands.push_back(first[i]);
                    s.candsIsFull.push_back(false);
                }
            }
        }
    } else {
        if (n == 1) {
            const int exact = dict_lookup(s.buffer.c_str(), 1, out, XHUP_MAX_CANDS);
            for (int i = 0; i < exact; ++i) {
                s.cands.push_back(out[i]);
                s.candsIsFull.push_back(true);
            }
            // Prefix scan: words whose code starts with this char.
            const int idx = dict_prefix_lower(s.buffer.c_str(), 1);
            if (idx >= 0) {
                for (int i = 0; i < XHUP_MAX_CANDS &&
                                (idx + i) < static_cast<int>(G_DICT.entry_count) &&
                                static_cast<int>(s.cands.size()) < XHUP_MAX_CANDS;
                     ++i) {
                    const DictCodeEntry *e = &G_DICT.entries[idx + i];
                    const char *code = G_DICT.code_pool + e->code_offset;
                    if (e->code_length < 1 || code[0] != s.buffer[0]) {
                        break;
                    }
                    if (e->code_length == 1 || e->word_count <= 0) {
                        continue;
                    }
                    const char *w = G_DICT.word_pool +
                                    G_DICT.word_offsets[e->word_start];
                    bool dup = false;
                    for (const char *c : s.cands) {
                        if (std::strcmp(c, w) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        s.cands.push_back(w);
                        s.candsIsFull.push_back(false);
                    }
                }
            }
        } else {
            // Odd length > 1: the last key starts the next syllable, so look up
            // the code without it.
            const int full = dict_lookup(s.buffer.c_str(), n - 1, out,
                                         XHUP_MAX_CANDS);
            for (int i = 0; i < full; ++i) {
                s.cands.push_back(out[i]);
                s.candsIsFull.push_back(false);
            }
        }
    }

    // Reorder: exact matches first, then by recency.
    const int total = static_cast<int>(s.cands.size());
    if (total > 1) {
        for (int i = 1; i < total; ++i) {
            const char *tmp = s.cands[i];
            const bool tmpFull = s.candsIsFull[i];
            const int score = recentCount(tmp);
            if (score == 0 && !tmpFull) {
                continue;
            }
            int j = i - 1;
            while (j >= 0 && ((!s.candsIsFull[j] && tmpFull) ||
                              (s.candsIsFull[j] == tmpFull &&
                               recentCount(s.cands[j]) < score))) {
                s.cands[j + 1] = s.cands[j];
                s.candsIsFull[j + 1] = s.candsIsFull[j];
                --j;
            }
            s.cands[j + 1] = tmp;
            s.candsIsFull[j + 1] = tmpFull;
        }
    }
}

void XhupEngine::updateUI(InputContext &ic) {
    auto *s = state(ic);
    auto &panel = ic.inputPanel();
    panel.reset();

    if (s->buffer.empty()) {
        ic.updatePreedit();
        ic.updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
    }

    computeCandidates(*s);

    Text preedit;
    preedit.append("[" + s->buffer + "]");
    if (ic.capabilityFlags().test(CapabilityFlag::Preedit)) {
        preedit.setCursor(0);
        panel.setClientPreedit(preedit);
    } else {
        preedit.setCursor(preedit.textLength());
        panel.setPreedit(preedit);
    }

    if (!s->cands.empty()) {
        const int pages =
            (static_cast<int>(s->cands.size()) + XHUP_PAGE_SIZE - 1) /
            XHUP_PAGE_SIZE;
        if (pages > 1) {
            Text aux;
            aux.append("(" + std::to_string(s->page + 1) + "/" +
                       std::to_string(pages) + ")");
            panel.setAuxDown(aux);
        }

        auto list = std::make_unique<CommonCandidateList>();
        list->setPageSize(XHUP_PAGE_SIZE);
        const int start = s->page * XHUP_PAGE_SIZE;
        const int end = std::min(start + XHUP_PAGE_SIZE,
                                 static_cast<int>(s->cands.size()));
        for (int i = start; i < end; ++i) {
            list->append<XhupCandidateWord>(this, std::string(s->cands[i]),
                                            s->isFull(i));
        }
        if (!list->empty()) {
            list->setGlobalCursorIndex(0);
            panel.setCandidateList(std::move(list));
        }
    }

    ic.updatePreedit();
    ic.updateUserInterface(UserInterfaceComponent::InputPanel);
}

class XhupFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new XhupEngine(manager->instance());
    }
};

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(xhup, fcitx::XhupFactory);
