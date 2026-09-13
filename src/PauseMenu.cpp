#include "GameHook.hpp"
#include "NEATManager.hpp"
#include "PlaybackStore.hpp"
#include "SessionStore.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>

using namespace geode::prelude;
using namespace neatgd;

namespace {

CCNode* makeIconNode(char const* frame, char const* fallback, float target) {
    CCNode* icon = CCSprite::createWithSpriteFrameName(frame);
    if (!icon) icon = ButtonSprite::create(fallback);
    if (!icon) return nullptr;
    auto const size = icon->getContentSize();
    float const largest = std::max(size.width, size.height);
    if (largest > 0.f) icon->setScale(target / largest);
    return icon;
}

// --- central config persistence -------------------------------------------
// Every setting (core + advanced) lives in the mod's saved values so the two
// settings popups stay in sync and a run picks up whatever was last chosen.

TrainingConfig configFromSaved() {
    TrainingConfig d;
    auto m = Mod::get();
    TrainingConfig c;
    c.population = static_cast<int>(m->getSavedValue<int64_t>("population", d.population));
    c.maxGenerations = static_cast<int>(m->getSavedValue<int64_t>("max-generations", d.maxGenerations));
    c.stagnationLimit = static_cast<int>(m->getSavedValue<int64_t>("stagnation-limit", d.stagnationLimit));
    c.maxMinutes = m->getSavedValue<double>("max-minutes", d.maxMinutes);
    c.speed = m->getSavedValue<double>("speed", d.speed);
    c.maxFps = m->getSavedValue<double>("max-fps", d.maxFps);
    c.hideGraphics = m->getSavedValue<bool>("hide-graphics", d.hideGraphics);

    c.jumpPenalty = m->getSavedValue<double>("jump-penalty", d.jumpPenalty);
    c.addConnProb = m->getSavedValue<double>("add-conn-prob", d.addConnProb);
    c.addNodeProb = m->getSavedValue<double>("add-node-prob", d.addNodeProb);
    c.weightMutateProb = m->getSavedValue<double>("weight-mute-prob", d.weightMutateProb);
    c.perturbProb = m->getSavedValue<double>("perturb-prob", d.perturbProb);
    c.weightPower = m->getSavedValue<double>("weight-power", d.weightPower);
    c.survivalThreshold = m->getSavedValue<double>("survival-threshold", d.survivalThreshold);
    c.mutateOnlyProb = m->getSavedValue<double>("mutate-only-prob", d.mutateOnlyProb);
    c.compatThreshold = m->getSavedValue<double>("compat-threshold", d.compatThreshold);
    c.championThreshold = static_cast<int>(m->getSavedValue<int64_t>("champion-threshold", d.championThreshold));
    c.targetSpecies = static_cast<int>(m->getSavedValue<int64_t>("target-species", d.targetSpecies));
    c.activation = static_cast<int>(m->getSavedValue<int64_t>("activation", d.activation));
    c.recurrent = m->getSavedValue<bool>("recurrent", d.recurrent);
    c.seed = static_cast<uint32_t>(m->getSavedValue<int64_t>("seed", static_cast<int64_t>(d.seed)));
    c.mode = static_cast<int>(m->getSavedValue<int64_t>("mode", d.mode));
    return c;
}

int parseIntField(TextInput* input, int fallback, int lo, int hi) {
    int value = fallback;
    if (input) {
        if (auto num = utils::numFromString<int>(std::string(input->getString()))) {
            value = num.unwrap();
        }
    }
    return std::clamp(value, lo, hi);
}

double parseDoubleField(TextInput* input, double fallback, double lo, double hi) {
    double value = fallback;
    if (input) {
        if (auto num = utils::numFromString<double>(std::string(input->getString()))) {
            value = num.unwrap();
        }
    }
    return std::clamp(value, lo, hi);
}

}

// ===========================================================================
// Small reusable rename popup (used for playbacks and sessions).
// ===========================================================================
class RenameTextPopup : public geode::Popup {
protected:
    TextInput* m_input = nullptr;
    std::function<void(std::string const&)> m_onSave;

    bool init(
        std::string const& title, std::string const& current,
        std::function<void(std::string const&)> onSave) {
        if (!Popup::init(280.f, 140.f)) return false;
        m_onSave = std::move(onSave);
        this->setTitle(title);

        auto const size = m_mainLayer->getContentSize();
        m_input = TextInput::create(220.f, "Name");
        if (!m_input) return false;
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(48);
        m_input->setString(current);
        m_input->setPosition(size.width / 2, size.height / 2 + 8.f);
        m_mainLayer->addChild(m_input);

        if (auto sprite = ButtonSprite::create("Save")) {
            auto btn = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(RenameTextPopup::onSave));
            if (btn) {
                btn->setPosition(size.width / 2, 32.f);
                m_buttonMenu->addChild(btn);
            }
        }
        return true;
    }

    void onSave(CCObject*) {
        std::string const name = m_input ? std::string(m_input->getString()) : "";
        auto const cb = m_onSave;
        this->onClose(nullptr);
        if (!name.empty() && cb) cb(name);
    }

public:
    static RenameTextPopup* create(
        std::string const& title, std::string const& current,
        std::function<void(std::string const&)> onSave) {
        auto ret = new RenameTextPopup();
        if (ret->init(title, current, std::move(onSave))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Network viewer: draws a genome's evolved topology.
// ===========================================================================
class NetworkViewerPopup : public geode::Popup {
protected:
    Genome m_genome;

    bool init(Genome genome) {
        if (!Popup::init(440.f, 300.f)) return false;
        m_genome = std::move(genome);
        this->setTitle("Evolved Brain");

        auto const size = m_mainLayer->getContentSize();

        auto info = CCLabelBMFont::create(
            fmt::format(
                "{} inputs  -  {} hidden  -  {} connections",
                m_genome.numInputs, m_genome.hiddenNodes(),
                m_genome.enabledConns())
                .c_str(),
            "chatFont.fnt");
        if (info) {
            info->setScale(0.6f);
            info->setPosition(size.width / 2, size.height - 22.f);
            m_mainLayer->addChild(info);
        }

        if (m_genome.nodeCount <= 0 || m_genome.nodeCount > 600) {
            auto note = CCLabelBMFont::create(
                m_genome.nodeCount <= 0 ? "No network to display yet."
                                        : "Network too large to render.",
                "chatFont.fnt");
            if (note) {
                note->setScale(0.7f);
                note->setPosition(size.width / 2, size.height / 2);
                m_mainLayer->addChild(note);
            }
            return true;
        }

        this->drawNetwork(size);
        return true;
    }

    void drawNetwork(CCSize const& size) {
        auto draw = CCDrawNode::create();
        if (!draw) return;

        float const left = 30.f;
        float const right = size.width - 30.f;
        float const top = size.height - 45.f;
        float const bottom = 30.f;
        float const usableH = top - bottom;

        int const nI = m_genome.numInputs;
        int const outId = m_genome.outputId(0);

        std::unordered_map<int, CCPoint> pos;

        // input + bias column on the left
        int const inCount = nI + 1;
        for (int i = 0; i <= nI; ++i) {
            float const t = inCount > 1
                ? static_cast<float>(i) / static_cast<float>(inCount - 1)
                : 0.5f;
            pos[i] = CCPoint(left, top - t * usableH);
        }

        // output on the right
        pos[outId] = CCPoint(right, bottom + usableH / 2.f);

        // hidden nodes spread by depth (x) and packed in rows (y)
        std::vector<int> hidden;
        for (int id = nI + 1; id < m_genome.nodeCount; ++id) {
            if (id == outId) continue;
            hidden.push_back(id);
        }
        std::sort(hidden.begin(), hidden.end(), [&](int a, int b) {
            return m_genome.depths[a] < m_genome.depths[b];
        });
        int const rows = std::max(1, static_cast<int>(usableH / 16.f));
        for (size_t k = 0; k < hidden.size(); ++k) {
            int const id = hidden[k];
            double const d = std::clamp(m_genome.depths[id], 0.0, 1.0);
            float const x = left + 40.f
                + static_cast<float>(d) * (right - left - 80.f);
            int const r = static_cast<int>(k) % rows;
            float const y = top - (static_cast<float>(r) + 0.5f)
                                      * (usableH / static_cast<float>(rows));
            pos[id] = CCPoint(x, y);
        }

        // connections first, so dots sit on top
        for (auto const& c : m_genome.conns) {
            if (!c.enabled) continue;
            auto fromIt = pos.find(c.in);
            auto toIt = pos.find(c.out);
            if (fromIt == pos.end() || toIt == pos.end()) continue;
            float const w = static_cast<float>(std::clamp(c.weight, -4.0, 4.0));
            float const thickness = 0.4f + std::abs(w) * 0.35f;
            ccColor4F col = w >= 0.f
                ? ccc4f(0.30f, 0.85f, 0.45f, 0.85f)
                : ccc4f(0.95f, 0.35f, 0.35f, 0.85f);
            draw->drawSegment(fromIt->second, toIt->second, thickness, col);
        }

        // nodes
        for (int i = 0; i <= nI; ++i) {
            draw->drawDot(pos[i], 2.6f, ccc4f(0.45f, 0.7f, 1.f, 1.f));
        }
        for (int id : hidden) {
            draw->drawDot(pos[id], 2.8f, ccc4f(1.f, 0.85f, 0.3f, 1.f));
        }
        draw->drawDot(pos[outId], 4.2f, ccc4f(1.f, 0.4f, 0.85f, 1.f));

        m_mainLayer->addChild(draw);

        auto legend = CCLabelBMFont::create(
            "blue: senses   yellow: hidden   pink: jump   "
            "green/red: + / - weight",
            "chatFont.fnt");
        if (legend) {
            legend->setScale(0.42f);
            legend->setPosition(size.width / 2, 14.f);
            m_mainLayer->addChild(legend);
        }
    }

public:
    static NetworkViewerPopup* create(Genome genome) {
        auto ret = new NetworkViewerPopup();
        if (ret->init(std::move(genome))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Playback list (per level) - watch / resume / rename / delete saved runs.
// ===========================================================================
class PlaybackListPopup : public geode::Popup {
protected:
    static constexpr float LIST_W = 300.f;
    static constexpr float LIST_H = 165.f;
    static constexpr float ROW_H = 34.f;

    Ref<PauseLayer> m_pauseLayer;
    std::function<void()> m_closeSettings;
    std::string m_levelKey;
    std::vector<neatgd::Playback> m_playbacks;
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_emptyLabel = nullptr;

    bool init(
        PauseLayer* pause, std::string levelKey,
        std::function<void()> closeSettings) {
        if (!Popup::init(340.f, 240.f)) return false;
        m_pauseLayer = pause;
        m_levelKey = std::move(levelKey);
        m_closeSettings = std::move(closeSettings);
        this->setTitle("Playbacks");
        m_playbacks = neatgd::PlaybackStore::load(m_levelKey);
        this->rebuildList();
        return true;
    }

    void rebuildList() {
        if (m_scroll) {
            m_scroll->removeFromParent();
            m_scroll = nullptr;
        }
        if (m_emptyLabel) {
            m_emptyLabel->removeFromParent();
            m_emptyLabel = nullptr;
        }
        auto const size = m_mainLayer->getContentSize();

        if (m_playbacks.empty()) {
            m_emptyLabel = CCLabelBMFont::create(
                "No playbacks for this level yet.\n"
                "Finish a training run first!",
                "chatFont.fnt");
            if (m_emptyLabel) {
                m_emptyLabel->setAlignment(kCCTextAlignmentCenter);
                m_emptyLabel->setPosition(size.width / 2, size.height / 2);
                m_emptyLabel->setScale(0.7f);
                m_mainLayer->addChild(m_emptyLabel);
            }
            return;
        }

        m_scroll = ScrollLayer::create(CCSize{LIST_W, LIST_H});
        if (!m_scroll) return;
        float const contentH =
            std::max(LIST_H, ROW_H * static_cast<float>(m_playbacks.size()));
        m_scroll->m_contentLayer->setContentSize({LIST_W, contentH});

        for (size_t i = 0; i < m_playbacks.size(); ++i) {
            auto const& p = m_playbacks[i];
            auto row = CCLayerColor::create(
                ccc4(0, 0, 0, i % 2 ? 70 : 35), LIST_W, ROW_H);
            if (!row) continue;
            row->setPosition(0.f, contentH - ROW_H * (i + 1));

            auto label = CCLabelBMFont::create(p.name.c_str(), "chatFont.fnt");
            if (label) {
                label->setAnchorPoint({0.f, 0.5f});
                label->setPosition(8.f, ROW_H / 2);
                float const w = label->getContentSize().width;
                label->setScale(std::min(0.65f, w > 0.f ? 150.f / w : 0.65f));
                row->addChild(label);
            }

            auto menu = CCMenu::create();
            if (menu) {
                menu->setPosition(0.f, 0.f);
                struct Btn {
                    char const* frame;
                    char const* fallback;
                    SEL_MenuHandler handler;
                };
                std::vector<Btn> buttons;
                buttons.push_back(
                    {"GJ_playBtn2_001.png", ">",
                     menu_selector(PlaybackListPopup::onPlayRow)});
                if (!p.completed
                    && PlaybackStore::sessionExists(m_levelKey, p.timestamp)) {
                    buttons.push_back(
                        {"GJ_playEditorBtn_001.png", "Res",
                         menu_selector(PlaybackListPopup::onResumeRow)});
                }
                buttons.push_back(
                    {"GJ_viewLevelsBtn_001.png", "Edit",
                     menu_selector(PlaybackListPopup::onRenameRow)});
                buttons.push_back(
                    {"GJ_resetBtn_001.png", "Del",
                     menu_selector(PlaybackListPopup::onDeleteRow)});

                float const rightEdge = 288.f;
                float const spacing = 30.f;
                int const count = static_cast<int>(buttons.size());
                for (int b = 0; b < count; ++b) {
                    auto icon = makeIconNode(
                        buttons[b].frame, buttons[b].fallback, 24.f);
                    if (!icon) continue;
                    auto btn = CCMenuItemSpriteExtra::create(
                        icon, this, buttons[b].handler);
                    if (!btn) continue;
                    btn->setPosition(
                        rightEdge - (count - 1 - b) * spacing, ROW_H / 2);
                    btn->setTag(static_cast<int>(i));
                    menu->addChild(btn);
                }
                row->addChild(menu);
            }
            m_scroll->m_contentLayer->addChild(row);
        }

        m_scroll->setPosition((size.width - LIST_W) / 2, 30.f);
        m_mainLayer->addChild(m_scroll);
        m_scroll->scrollToTop();
    }

    int rowIndex(CCObject* sender) const {
        int const idx = static_cast<CCNode*>(sender)->getTag();
        return idx >= 0 && idx < static_cast<int>(m_playbacks.size()) ? idx : -1;
    }

    void onPlayRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto playLayer = PlayLayer::get();
        if (!playLayer) return;
        if (!NEATManager::get()->beginPlayback(m_playbacks[idx])) return;

        Ref<PauseLayer> pause = m_pauseLayer;
        auto const closeSettings = m_closeSettings;
        this->onClose(nullptr);
        if (closeSettings) closeSettings();
        if (pause) pause->onResume(nullptr);
        playLayer->resetLevelFromStart();
    }

    void onResumeRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto playLayer = PlayLayer::get();
        if (!playLayer) return;
        if (!NEATManager::get()->resumeTraining(
                m_levelKey, m_playbacks[idx].timestamp)) {
            return;
        }

        Ref<PauseLayer> pause = m_pauseLayer;
        auto const closeSettings = m_closeSettings;
        this->onClose(nullptr);
        if (closeSettings) closeSettings();
        if (pause) pause->onResume(nullptr);
        playLayer->resetLevelFromStart();
    }

    void onRenameRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto popup = RenameTextPopup::create(
            "Rename Playback", m_playbacks[idx].name,
            [self = Ref(this), idx](std::string const& name) {
                if (idx >= static_cast<int>(self->m_playbacks.size())) return;
                self->m_playbacks[idx].name = name;
                neatgd::PlaybackStore::save(self->m_levelKey, self->m_playbacks);
                self->rebuildList();
            });
        if (popup) popup->show();
    }

    void onDeleteRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        geode::createQuickPopup(
            "Delete Playback",
            fmt::format("Delete <cy>{}</c>?", m_playbacks[idx].name),
            "Cancel", "Delete",
            [self = Ref(this), idx](FLAlertLayer*, bool confirmed) {
                if (!confirmed) return;
                if (idx >= static_cast<int>(self->m_playbacks.size())) return;
                neatgd::PlaybackStore::deleteSession(
                    self->m_levelKey, self->m_playbacks[idx].timestamp);
                self->m_playbacks.erase(self->m_playbacks.begin() + idx);
                neatgd::PlaybackStore::save(self->m_levelKey, self->m_playbacks);
                self->rebuildList();
            });
    }

public:
    static PlaybackListPopup* create(
        PauseLayer* pause, std::string levelKey,
        std::function<void()> closeSettings) {
        auto ret = new PlaybackListPopup();
        if (ret->init(pause, std::move(levelKey), std::move(closeSettings))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Session browser: two tabs - "This Level" and "Global (All)".
// Load any session onto the current level (seamless resume on the same level,
// brain transfer on a different one).
// ===========================================================================
class SessionBrowserPopup : public geode::Popup {
protected:
    static constexpr float LIST_W = 360.f;
    static constexpr float LIST_H = 180.f;
    static constexpr float ROW_H = 42.f;

    Ref<PauseLayer> m_pauseLayer;
    std::function<void()> m_closeSettings;
    std::string m_levelKey;
    bool m_globalTab = false;
    std::vector<SessionMeta> m_rows;
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_emptyLabel = nullptr;
    CCLabelBMFont* m_tabLabel = nullptr;

    bool init(
        PauseLayer* pause, std::string levelKey,
        std::function<void()> closeSettings) {
        if (!Popup::init(420.f, 290.f)) return false;
        m_pauseLayer = pause;
        m_levelKey = std::move(levelKey);
        m_closeSettings = std::move(closeSettings);
        this->setTitle("NEAT Sessions");

        auto const size = m_mainLayer->getContentSize();

        // tab buttons
        if (auto s = ButtonSprite::create("This Level", "bigFont.fnt",
                                          "GJ_button_01.png", 0.6f)) {
            s->setScale(0.7f);
            auto b = CCMenuItemSpriteExtra::create(
                s, this, menu_selector(SessionBrowserPopup::onTabLocal));
            if (b) {
                b->setPosition(size.width / 2 - 70.f, size.height - 34.f);
                m_buttonMenu->addChild(b);
            }
        }
        if (auto s = ButtonSprite::create("Global (All)", "bigFont.fnt",
                                          "GJ_button_05.png", 0.6f)) {
            s->setScale(0.7f);
            auto b = CCMenuItemSpriteExtra::create(
                s, this, menu_selector(SessionBrowserPopup::onTabGlobal));
            if (b) {
                b->setPosition(size.width / 2 + 70.f, size.height - 34.f);
                m_buttonMenu->addChild(b);
            }
        }

        m_tabLabel = CCLabelBMFont::create("", "chatFont.fnt");
        if (m_tabLabel) {
            m_tabLabel->setScale(0.5f);
            m_tabLabel->setPosition(size.width / 2, size.height - 58.f);
            m_mainLayer->addChild(m_tabLabel);
        }

        this->reload();
        return true;
    }

    void reload() {
        m_rows = m_globalTab ? SessionStore::loadIndex()
                             : SessionStore::forLevel(m_levelKey);
        if (m_tabLabel) {
            m_tabLabel->setString(
                fmt::format(
                    "{}  -  {} session{}",
                    m_globalTab ? std::string("Global (all levels)")
                                : fmt::format("This level ({})", m_levelKey),
                    m_rows.size(), m_rows.size() == 1 ? "" : "s")
                    .c_str());
        }
        rebuildList();
    }

    void onTabLocal(CCObject*) {
        if (!m_globalTab) return;
        m_globalTab = false;
        reload();
    }
    void onTabGlobal(CCObject*) {
        if (m_globalTab) return;
        m_globalTab = true;
        reload();
    }

    void rebuildList() {
        if (m_scroll) {
            m_scroll->removeFromParent();
            m_scroll = nullptr;
        }
        if (m_emptyLabel) {
            m_emptyLabel->removeFromParent();
            m_emptyLabel = nullptr;
        }
        auto const size = m_mainLayer->getContentSize();

        if (m_rows.empty()) {
            m_emptyLabel = CCLabelBMFont::create(
                m_globalTab ? "No saved sessions anywhere yet."
                            : "No sessions for this level yet.",
                "chatFont.fnt");
            if (m_emptyLabel) {
                m_emptyLabel->setAlignment(kCCTextAlignmentCenter);
                m_emptyLabel->setPosition(size.width / 2, size.height / 2 - 10.f);
                m_emptyLabel->setScale(0.7f);
                m_mainLayer->addChild(m_emptyLabel);
            }
            return;
        }

        m_scroll = ScrollLayer::create(CCSize{LIST_W, LIST_H});
        if (!m_scroll) return;
        float const contentH =
            std::max(LIST_H, ROW_H * static_cast<float>(m_rows.size()));
        m_scroll->m_contentLayer->setContentSize({LIST_W, contentH});

        for (size_t i = 0; i < m_rows.size(); ++i) {
            auto const& s = m_rows[i];
            bool const sameLevel = s.levelKey == m_levelKey;
            // a session from an older sensor layout is migrated on load, so it
            // still counts as compatible here
            bool const compatible =
                s.inputCount == static_cast<int>(INPUT_COUNT)
                || s.inputCount == static_cast<int>(LEGACY_INPUT_COUNT_V1);

            auto row = CCLayerColor::create(
                ccc4(0, 0, 0, i % 2 ? 70 : 35), LIST_W, ROW_H);
            if (!row) continue;
            row->setPosition(0.f, contentH - ROW_H * (i + 1));

            auto title = CCLabelBMFont::create(s.displayName.c_str(),
                                               "chatFont.fnt");
            if (title) {
                title->setAnchorPoint({0.f, 0.5f});
                title->setPosition(8.f, ROW_H - 13.f);
                float const w = title->getContentSize().width;
                title->setScale(std::min(0.6f, w > 0.f ? 180.f / w : 0.6f));
                row->addChild(title);
            }

            std::string lvl = s.levelName.empty() ? s.levelKey : s.levelName;
            bool const climber = s.mode == 1;
            std::string detail;
            if (climber) {
                detail = fmt::format(
                    "{}climber  {:.1f}%{}",
                    m_globalTab ? lvl + "  -  " : std::string(), s.bestFitness,
                    sameLevel ? "  resume"
                              : (compatible ? "  (level-specific)"
                                            : "  [incompatible]"));
            } else {
                detail = fmt::format(
                    "{}{}{}  g{}  {}p  {:.1f}%{}",
                    m_globalTab ? lvl + "  -  " : std::string(),
                    s.mode == 2 ? "hyb " : std::string(),
                    sameLevel ? "resume" : "transfer", s.generation,
                    s.population, s.bestFitness,
                    compatible ? "" : "  [incompatible]");
            }
            auto sub = CCLabelBMFont::create(detail.c_str(), "chatFont.fnt");
            if (sub) {
                sub->setAnchorPoint({0.f, 0.5f});
                sub->setPosition(8.f, 12.f);
                sub->setScale(0.42f);
                sub->setColor(sameLevel ? ccc3(120, 230, 140)
                                        : ccc3(120, 190, 255));
                if (climber && !sameLevel) sub->setColor(ccc3(180, 180, 180));
                if (!compatible) sub->setColor(ccc3(230, 120, 120));
                row->addChild(sub);
            }

            auto menu = CCMenu::create();
            if (menu) {
                menu->setPosition(0.f, 0.f);
                struct Btn {
                    char const* frame;
                    char const* fallback;
                    SEL_MenuHandler handler;
                };
                std::vector<Btn> buttons = {
                    {"GJ_playBtn2_001.png", "Go",
                     menu_selector(SessionBrowserPopup::onLoadRow)},
                    {"GJ_viewLevelsBtn_001.png", "Ed",
                     menu_selector(SessionBrowserPopup::onRenameRow)},
                    {"GJ_resetBtn_001.png", "Del",
                     menu_selector(SessionBrowserPopup::onDeleteRow)},
                };
                float const rightEdge = LIST_W - 14.f;
                float const spacing = 32.f;
                int const count = static_cast<int>(buttons.size());
                for (int b = 0; b < count; ++b) {
                    auto icon = makeIconNode(
                        buttons[b].frame, buttons[b].fallback, 26.f);
                    if (!icon) continue;
                    auto btn = CCMenuItemSpriteExtra::create(
                        icon, this, buttons[b].handler);
                    if (!btn) continue;
                    btn->setPosition(
                        rightEdge - (count - 1 - b) * spacing, ROW_H / 2);
                    btn->setTag(static_cast<int>(i));
                    menu->addChild(btn);
                }
                row->addChild(menu);
            }
            m_scroll->m_contentLayer->addChild(row);
        }

        m_scroll->setPosition((size.width - LIST_W) / 2, 22.f);
        m_mainLayer->addChild(m_scroll);
        m_scroll->scrollToTop();
    }

    int rowIndex(CCObject* sender) const {
        int const idx = static_cast<CCNode*>(sender)->getTag();
        return idx >= 0 && idx < static_cast<int>(m_rows.size()) ? idx : -1;
    }

    void onLoadRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto const meta = m_rows[idx];
        auto playLayer = PlayLayer::get();
        if (!playLayer) return;

        auto mgr = NEATManager::get();
        if (!mgr->loadSession(meta.levelKey, meta.id)) {
            std::string const err = mgr->lastError();
            if (auto alert = FLAlertLayer::create(
                    "Can't Load Session",
                    fmt::format("Loading failed: <cr>{}</c>.",
                                err.empty() ? "unknown error" : err),
                    "OK")) {
                alert->show();
            }
            return;
        }

        bool const transferred = mgr->wasTransfer();
        Ref<PauseLayer> pause = m_pauseLayer;
        auto const closeSettings = m_closeSettings;
        this->onClose(nullptr);
        if (closeSettings) closeSettings();
        if (auto n = Notification::create(
                transferred ? "Brains transferred - training from scratch here"
                            : "Session resumed",
                transferred ? NotificationIcon::Info
                            : NotificationIcon::Success,
                2.5f)) {
            n->show();
        }
        if (pause) pause->onResume(nullptr);
        playLayer->resetLevelFromStart();
    }

    void onRenameRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto const meta = m_rows[idx];
        auto popup = RenameTextPopup::create(
            "Rename Session", meta.displayName,
            [self = Ref(this), meta](std::string const& name) {
                SessionStore::rename(meta.id, meta.levelKey, name);
                self->reload();
            });
        if (popup) popup->show();
    }

    void onDeleteRow(CCObject* sender) {
        int const idx = rowIndex(sender);
        if (idx < 0) return;
        auto const meta = m_rows[idx];
        geode::createQuickPopup(
            "Delete Session",
            fmt::format("Delete session <cy>{}</c>?\n"
                        "This permanently removes its evolved population.",
                        meta.displayName),
            "Cancel", "Delete",
            [self = Ref(this), meta](FLAlertLayer*, bool confirmed) {
                if (!confirmed) return;
                SessionStore::remove(meta.id, meta.levelKey);
                self->reload();
            });
    }

public:
    static SessionBrowserPopup* create(
        PauseLayer* pause, std::string levelKey,
        std::function<void()> closeSettings) {
        auto ret = new SessionBrowserPopup();
        if (ret->init(pause, std::move(levelKey), std::move(closeSettings))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Advanced settings: the NEAT hyper-parameters.
// ===========================================================================
class AdvancedSettingsPopup : public geode::Popup {
protected:
    TextInput* m_addConn = nullptr;
    TextInput* m_addNode = nullptr;
    TextInput* m_weightPow = nullptr;
    TextInput* m_survival = nullptr;
    TextInput* m_compat = nullptr;
    TextInput* m_target = nullptr;
    TextInput* m_champion = nullptr;
    TextInput* m_jumpPen = nullptr;
    TextInput* m_seed = nullptr;
    CCMenuItemToggler* m_recurrent = nullptr;
    CCMenuItemToggler* m_tanh = nullptr;

    TextInput* makeInput(
        char const* label, CCPoint pos, std::string const& value,
        CommonFilter filter) {
        auto input = TextInput::create(95.f, "0");
        if (!input) return nullptr;
        input->setLabel(label);
        input->setCommonFilter(filter);
        input->setMaxCharCount(10);
        input->setString(value);
        input->setPosition(pos);
        m_mainLayer->addChild(input);
        return input;
    }

    bool init() {
        if (!Popup::init(400.f, 280.f)) return false;
        this->setTitle("Advanced NEAT");

        auto const c = configFromSaved();
        auto const size = m_mainLayer->getContentSize();
        float const r1 = size.height - 62.f;
        float const r2 = size.height - 112.f;
        float const r3 = size.height - 162.f;
        float const cx0 = size.width / 2 - 120.f;
        float const cx1 = size.width / 2;
        float const cx2 = size.width / 2 + 120.f;

        m_addConn = makeInput("Add-conn p", {cx0, r1},
                              fmt::format("{:g}", c.addConnProb), CommonFilter::Float);
        m_addNode = makeInput("Add-node p", {cx1, r1},
                              fmt::format("{:g}", c.addNodeProb), CommonFilter::Float);
        m_weightPow = makeInput("Weight pow", {cx2, r1},
                                fmt::format("{:g}", c.weightPower), CommonFilter::Float);
        m_survival = makeInput("Survival", {cx0, r2},
                               fmt::format("{:g}", c.survivalThreshold), CommonFilter::Float);
        m_compat = makeInput("Compat dist", {cx1, r2},
                             fmt::format("{:g}", c.compatThreshold), CommonFilter::Float);
        m_target = makeInput("Target spec", {cx2, r2},
                             fmt::format("{}", c.targetSpecies), CommonFilter::Uint);
        m_champion = makeInput("Champion", {cx0, r3},
                               fmt::format("{}", c.championThreshold), CommonFilter::Uint);
        m_jumpPen = makeInput("Jump pen.", {cx1, r3},
                              fmt::format("{:g}", c.jumpPenalty), CommonFilter::Float);
        m_seed = makeInput("Seed (0=rng)", {cx2, r3},
                           fmt::format("{}", c.seed), CommonFilter::Uint);

        // toggles
        float const tY = 64.f;
        m_recurrent = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AdvancedSettingsPopup::onNoop), 0.6f);
        if (m_recurrent) {
            m_recurrent->setPosition(cx0 - 6.f, tY);
            m_recurrent->toggle(c.recurrent);
            m_buttonMenu->addChild(m_recurrent);
        }
        if (auto l = CCLabelBMFont::create("Recurrent net", "bigFont.fnt")) {
            l->setScale(0.34f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(cx0 + 8.f, tY);
            m_mainLayer->addChild(l);
        }
        m_tanh = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AdvancedSettingsPopup::onNoop), 0.6f);
        if (m_tanh) {
            m_tanh->setPosition(cx1 + 30.f, tY);
            m_tanh->toggle(c.activation == 1);
            m_buttonMenu->addChild(m_tanh);
        }
        if (auto l = CCLabelBMFont::create("tanh hidden", "bigFont.fnt")) {
            l->setScale(0.34f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(cx1 + 44.f, tY);
            m_mainLayer->addChild(l);
        }

        if (auto info = CCLabelBMFont::create(
                "Defaults reproduce classic NEATGD. Hover-free: tweak freely.",
                "chatFont.fnt")) {
            info->setScale(0.42f);
            info->setPosition(size.width / 2, 92.f);
            m_mainLayer->addChild(info);
        }

        if (auto sprite = ButtonSprite::create("Save")) {
            auto btn = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(AdvancedSettingsPopup::onSave));
            if (btn) {
                btn->setPosition(size.width / 2 - 55.f, 30.f);
                m_buttonMenu->addChild(btn);
            }
        }
        if (auto sprite = ButtonSprite::create("Reset")) {
            auto btn = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(AdvancedSettingsPopup::onReset));
            if (btn) {
                btn->setPosition(size.width / 2 + 55.f, 30.f);
                m_buttonMenu->addChild(btn);
            }
        }
        return true;
    }

    void onNoop(CCObject*) {}

    void onReset(CCObject*) {
        TrainingConfig const d;
        auto m = Mod::get();
        m->setSavedValue<double>("add-conn-prob", d.addConnProb);
        m->setSavedValue<double>("add-node-prob", d.addNodeProb);
        m->setSavedValue<double>("weight-power", d.weightPower);
        m->setSavedValue<double>("survival-threshold", d.survivalThreshold);
        m->setSavedValue<double>("compat-threshold", d.compatThreshold);
        m->setSavedValue<int64_t>("target-species", d.targetSpecies);
        m->setSavedValue<int64_t>("champion-threshold", d.championThreshold);
        m->setSavedValue<double>("jump-penalty", d.jumpPenalty);
        m->setSavedValue<int64_t>("seed", static_cast<int64_t>(d.seed));
        m->setSavedValue<bool>("recurrent", d.recurrent);
        m->setSavedValue<int64_t>("activation", d.activation);
        this->onClose(nullptr);
        if (auto n = Notification::create("Advanced reset to defaults",
                                          NotificationIcon::Info, 2.f)) {
            n->show();
        }
    }

    void onSave(CCObject*) {
        TrainingConfig const d;
        auto m = Mod::get();
        m->setSavedValue<double>("add-conn-prob",
            parseDoubleField(m_addConn, d.addConnProb, 0.0, 1.0));
        m->setSavedValue<double>("add-node-prob",
            parseDoubleField(m_addNode, d.addNodeProb, 0.0, 1.0));
        m->setSavedValue<double>("weight-power",
            parseDoubleField(m_weightPow, d.weightPower, 0.1, 6.0));
        m->setSavedValue<double>("survival-threshold",
            parseDoubleField(m_survival, d.survivalThreshold, 0.05, 1.0));
        m->setSavedValue<double>("compat-threshold",
            parseDoubleField(m_compat, d.compatThreshold, 0.2, 12.0));
        m->setSavedValue<int64_t>("target-species",
            parseIntField(m_target, d.targetSpecies, 0, 200));
        m->setSavedValue<int64_t>("champion-threshold",
            parseIntField(m_champion, d.championThreshold, 1, 1000));
        m->setSavedValue<double>("jump-penalty",
            parseDoubleField(m_jumpPen, d.jumpPenalty, 0.0, 5.0));
        m->setSavedValue<int64_t>("seed",
            static_cast<int64_t>(parseIntField(m_seed, 0, 0, 2000000000)));
        m->setSavedValue<bool>("recurrent", m_recurrent && m_recurrent->isToggled());
        m->setSavedValue<int64_t>("activation",
            m_tanh && m_tanh->isToggled() ? 1 : 0);
        this->onClose(nullptr);
        if (auto n = Notification::create("Advanced settings saved",
                                          NotificationIcon::Success, 2.f)) {
            n->show();
        }
    }

public:
    static AdvancedSettingsPopup* create() {
        auto ret = new AdvancedSettingsPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Live training control panel (shown when NEAT is already running).
// ===========================================================================
class TrainingControlPopup : public geode::Popup {
protected:
    Ref<PauseLayer> m_pauseLayer;
    std::string m_levelKey;
    CCLabelBMFont* m_speedLabel = nullptr;
    CCMenuItemToggler* m_hideToggle = nullptr;

    bool init(PauseLayer* pause, std::string levelKey) {
        if (!Popup::init(320.f, 230.f)) return false;
        m_pauseLayer = pause;
        m_levelKey = std::move(levelKey);
        this->setTitle("NEAT Running");

        auto mgr = NEATManager::get();
        auto const size = m_mainLayer->getContentSize();

        if (auto status = CCLabelBMFont::create(
                fmt::format("Generation {}  -  best {:.1f}%",
                            mgr->generation() + 1, mgr->bestFitness())
                    .c_str(),
                "chatFont.fnt")) {
            status->setScale(0.6f);
            status->setPosition(size.width / 2, size.height - 50.f);
            m_mainLayer->addChild(status);
        }

        // live speed control
        if (auto l = CCLabelBMFont::create("Speed", "bigFont.fnt")) {
            l->setScale(0.4f);
            l->setPosition(size.width / 2, size.height - 78.f);
            m_mainLayer->addChild(l);
        }
        m_speedLabel = CCLabelBMFont::create("", "bigFont.fnt");
        if (m_speedLabel) {
            m_speedLabel->setScale(0.55f);
            m_speedLabel->setPosition(size.width / 2, size.height - 100.f);
            m_mainLayer->addChild(m_speedLabel);
        }
        updateSpeedLabel();

        if (auto minus = makeIconNode("GJ_arrow_03_001.png", "-", 30.f)) {
            auto b = CCMenuItemSpriteExtra::create(
                minus, this, menu_selector(TrainingControlPopup::onSlower));
            if (b) {
                b->setPosition(size.width / 2 - 60.f, size.height - 100.f);
                m_buttonMenu->addChild(b);
            }
        }
        if (auto plus = makeIconNode("GJ_arrow_03_001.png", "+", 30.f)) {
            plus->setScaleX(-plus->getScaleX());
            auto b = CCMenuItemSpriteExtra::create(
                plus, this, menu_selector(TrainingControlPopup::onFaster));
            if (b) {
                b->setPosition(size.width / 2 + 60.f, size.height - 100.f);
                m_buttonMenu->addChild(b);
            }
        }

        // hide-gfx live toggle
        m_hideToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(TrainingControlPopup::onToggleHide), 0.7f);
        if (m_hideToggle) {
            m_hideToggle->setPosition(size.width / 2 - 70.f, 100.f);
            m_hideToggle->toggle(mgr->hideGraphics());
            m_buttonMenu->addChild(m_hideToggle);
        }
        if (auto l = CCLabelBMFont::create("Hide graphics", "bigFont.fnt")) {
            l->setScale(0.4f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(size.width / 2 - 52.f, 100.f);
            m_mainLayer->addChild(l);
        }

        // action buttons row
        auto addBtn = [&](char const* caption, SEL_MenuHandler h, float x) {
            if (auto s = ButtonSprite::create(caption)) {
                s->setScale(0.8f);
                auto b = CCMenuItemSpriteExtra::create(s, this, h);
                if (b) {
                    b->setPosition(x, 56.f);
                    m_buttonMenu->addChild(b);
                }
            }
        };
        addBtn("Checkpoint",
               menu_selector(TrainingControlPopup::onCheckpoint),
               size.width / 2 - 70.f);
        addBtn("View Brain",
               menu_selector(TrainingControlPopup::onViewBrain),
               size.width / 2 + 70.f);

        if (auto s = ButtonSprite::create("Stop Training")) {
            s->setScale(0.8f);
            auto b = CCMenuItemSpriteExtra::create(
                s, this, menu_selector(TrainingControlPopup::onStop));
            if (b) {
                b->setPosition(size.width / 2, 26.f);
                m_buttonMenu->addChild(b);
            }
        }
        return true;
    }

    void updateSpeedLabel() {
        if (m_speedLabel) {
            m_speedLabel->setString(
                fmt::format("{:g}x", NEATManager::get()->trainingSpeed())
                    .c_str());
        }
    }

    void onSlower(CCObject*) {
        auto mgr = NEATManager::get();
        mgr->setSpeed(mgr->trainingSpeed() - 1.0);
        updateSpeedLabel();
    }
    void onFaster(CCObject*) {
        auto mgr = NEATManager::get();
        mgr->setSpeed(mgr->trainingSpeed() + 1.0);
        updateSpeedLabel();
    }
    void onToggleHide(CCObject*) {
        // CCMenuItemToggler flips its visual state before the callback runs,
        // so isToggled() already reflects the desired value.
        if (m_hideToggle) {
            NEATManager::get()->setHideGraphics(m_hideToggle->isToggled());
        }
    }

    void onCheckpoint(CCObject*) {
        NEATManager::get()->saveCheckpoint();
    }

    void onViewBrain(CCObject*) {
        auto mgr = NEATManager::get();
        if (mgr->mode() == 1) {
            if (auto n = Notification::create(
                    "Climber mode has no brain (it learns a click sequence)",
                    NotificationIcon::Info, 2.5f)) {
                n->show();
            }
            return;
        }
        if (!mgr->hasBestGenome()) {
            if (auto n = Notification::create("No brain to show yet",
                                              NotificationIcon::Info, 2.f)) {
                n->show();
            }
            return;
        }
        if (auto p = NetworkViewerPopup::create(mgr->bestGenome())) p->show();
    }

    void onStop(CCObject*) {
        NEATManager::get()->stop("stopped from control panel");
        this->onClose(nullptr);
    }

public:
    static TrainingControlPopup* create(PauseLayer* pause, std::string levelKey) {
        auto ret = new TrainingControlPopup();
        if (ret->init(pause, std::move(levelKey))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Main training settings popup.
// ===========================================================================
class TrainSettingsPopup : public geode::Popup {
protected:
    Ref<PauseLayer> m_pauseLayer;
    TextInput* m_populationInput = nullptr;
    TextInput* m_generationsInput = nullptr;
    TextInput* m_stagnationInput = nullptr;
    TextInput* m_maxMinutesInput = nullptr;
    TextInput* m_speedInput = nullptr;
    TextInput* m_maxFpsInput = nullptr;
    CCMenuItemToggler* m_graphicsToggle = nullptr;
    int m_modeChoice = 0;                       // 0=NEAT, 1=Climber, 2=Hybrid
    ButtonSprite* m_modeBtnSprite = nullptr;

    static char const* modeName(int m) {
        return m == 1 ? "Climber" : (m == 2 ? "Hybrid" : "NEAT");
    }

    TextInput* makeInput(
        char const* label, CCPoint pos, std::string const& value,
        CommonFilter filter) {
        auto input = TextInput::create(100.f, "0");
        if (!input) return nullptr;
        input->setLabel(label);
        input->setCommonFilter(filter);
        input->setMaxCharCount(7);
        input->setString(value);
        input->setPosition(pos);
        m_mainLayer->addChild(input);
        return input;
    }

    void makeInfoButton(CCPoint pos, int tag) {
        CCNode* icon =
            CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        if (!icon) icon = ButtonSprite::create("?");
        if (!icon) return;
        float const largest =
            std::max(icon->getContentSize().width, icon->getContentSize().height);
        if (largest > 0.f) icon->setScale(11.f / largest);
        auto btn = CCMenuItemSpriteExtra::create(
            icon, this, menu_selector(TrainSettingsPopup::onInfo));
        if (!btn) return;
        btn->setPosition(pos);
        btn->setTag(tag);
        m_buttonMenu->addChild(btn);
    }

    void onInfo(CCObject* sender) {
        char const* title = "";
        char const* desc = "";
        switch (sender->getTag()) {
            case 0:
                title = "Population";
                desc = "How many neural networks (genomes) live in each "
                       "generation. A moderate <cy>150-500</c> usually learns "
                       "faster than a huge population.";
                break;
            case 1:
                title = "Generations";
                desc = "Maximum generations to evolve before stopping. Training "
                       "can also stop earlier from the Stagnation or Minutes "
                       "limits.";
                break;
            case 2:
                title = "Stagnation";
                desc = "Stop early if the best score hasn't improved for this "
                       "many generations. It also triggers an exploration "
                       "burst a few generations before giving up.";
                break;
            case 3:
                title = "Minutes";
                desc = "Wall-clock time budget for the whole run. Set to "
                       "<cy>0</c> to disable.";
                break;
            case 4:
                title = "Speed";
                desc = "Game-speed multiplier while training. Physics are "
                       "unchanged. Turn on Hide gfx and raise Max FPS to push "
                       "this higher. Adjustable live from the control panel.";
                break;
            case 6:
                title = "Max FPS";
                desc = "Frame-rate target while training. Higher values let the "
                       "Speed multiplier actually pay off. Best paired with "
                       "Hide gfx. Restored when training ends.";
                break;
            case 5:
                title = "Hide graphics";
                desc = "Blanks the level to a black screen with a progress "
                       "readout while training, spending hardware on physics "
                       "instead of rendering.";
                break;
            case 7:
                title = "Training mode";
                desc = "Tap the button to cycle three modes.\n\n"
                       "<cy>NEAT</c>: evolves a population of neural-network "
                       "brains that read the level through sensors and learn to "
                       "react. Generalizes, and a trained brain can be "
                       "<cj>transferred</c> to other levels.\n\n"
                       "<cy>Climber</c>: no brain - it grows a single click "
                       "sequence, <cg>locking</c> every part that clears an "
                       "obstacle and re-trying only the failing tail. If it "
                       "stays stuck it <co>deep-rewinds</c> to re-search earlier "
                       "locked clicks. Fast to beat one level, but "
                       "level-specific (no transfer).\n\n"
                       "<cy>Hybrid</c>: the <cg>network plays</c>, but the whole "
                       "population shares one <cg>locked click prefix</c> up to "
                       "the frontier - the net only solves the unsolved part "
                       "past it, and the prefix grows as the net pushes further. "
                       "Best of both: brains still transfer, but no effort is "
                       "wasted re-solving sections that already work. "
                       "<cj>Population, Generations and Stagnation apply to NEAT "
                       "and Hybrid; Climber ignores them and runs until beaten "
                       "or Minutes runs out.</c>";
                break;
            default:
                return;
        }
        if (auto alert = FLAlertLayer::create(title, desc, "OK")) alert->show();
    }

    void applyPreset(int population, int gens, int stag, double minutes,
                     double speed, double fps, bool hide) {
        if (m_populationInput) m_populationInput->setString(fmt::format("{}", population));
        if (m_generationsInput) m_generationsInput->setString(fmt::format("{}", gens));
        if (m_stagnationInput) m_stagnationInput->setString(fmt::format("{}", stag));
        if (m_maxMinutesInput) m_maxMinutesInput->setString(fmt::format("{:g}", minutes));
        if (m_speedInput) m_speedInput->setString(fmt::format("{:g}", speed));
        if (m_maxFpsInput) m_maxFpsInput->setString(fmt::format("{:g}", fps));
        if (m_graphicsToggle && m_graphicsToggle->isToggled() != hide) {
            m_graphicsToggle->toggle(hide);
        }
    }

    void onPresetFast(CCObject*) { applyPreset(150, 1000, 25, 5, 24, 600, true); }
    void onPresetBalanced(CCObject*) { applyPreset(300, 2000, 30, 0, 16, 360, false); }
    void onPresetThorough(CCObject*) { applyPreset(500, 5000, 50, 0, 16, 360, false); }
    void onPresetOvernight(CCObject*) { applyPreset(400, 100000, 80, 480, 20, 800, true); }

    bool init(PauseLayer* pause) {
        if (!Popup::init(360.f, 300.f)) return false;
        m_pauseLayer = pause;
        this->setTitle("NEAT Training");

        auto const c = configFromSaved();
        auto const size = m_mainLayer->getContentSize();

        // preset row
        float const presetY = size.height - 44.f;
        struct Preset { char const* name; SEL_MenuHandler h; };
        std::vector<Preset> presets = {
            {"Fast", menu_selector(TrainSettingsPopup::onPresetFast)},
            {"Balanced", menu_selector(TrainSettingsPopup::onPresetBalanced)},
            {"Thorough", menu_selector(TrainSettingsPopup::onPresetThorough)},
            {"Overnight", menu_selector(TrainSettingsPopup::onPresetOvernight)},
        };
        float const pStart = size.width / 2 - 132.f;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (auto s = ButtonSprite::create(presets[i].name, "bigFont.fnt",
                                              "GJ_button_04.png", 0.5f)) {
                s->setScale(0.62f);
                auto b = CCMenuItemSpriteExtra::create(s, this, presets[i].h);
                if (b) {
                    b->setPosition(pStart + i * 88.f, presetY);
                    m_buttonMenu->addChild(b);
                }
            }
        }

        float const topY = size.height / 2 + 28.f;
        float const bottomY = size.height / 2 - 30.f;

        // mode selector: cycle NEAT (population) -> Climber (single click
        // sequence) -> Hybrid (network plays, working clicks locked)
        float const modeY = presetY - 26.f;
        m_modeChoice = std::clamp(c.mode, 0, 2);
        m_modeBtnSprite = ButtonSprite::create(modeName(m_modeChoice), 0.6f);
        if (m_modeBtnSprite) {
            auto btn = CCMenuItemSpriteExtra::create(
                m_modeBtnSprite, this,
                menu_selector(TrainSettingsPopup::onCycleMode));
            btn->setPosition(size.width / 2 - 150.f, modeY);
            m_buttonMenu->addChild(btn);
        }
        if (auto l = CCLabelBMFont::create(
                "Mode (tap to switch)", "bigFont.fnt")) {
            l->setScale(0.34f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(size.width / 2 - 104.f, modeY);
            m_mainLayer->addChild(l);
        }
        makeInfoButton({size.width / 2 + 150.f, modeY}, 7);

        m_populationInput = makeInput(
            "Population", {size.width / 2 - 110.f, topY},
            fmt::format("{}", c.population), CommonFilter::Uint);
        m_generationsInput = makeInput(
            "Generations", {size.width / 2, topY},
            fmt::format("{}", c.maxGenerations), CommonFilter::Uint);
        m_stagnationInput = makeInput(
            "Stagnation", {size.width / 2 + 110.f, topY},
            fmt::format("{}", c.stagnationLimit), CommonFilter::Uint);
        m_maxMinutesInput = makeInput(
            "Minutes (0=off)", {size.width / 2 - 110.f, bottomY},
            fmt::format("{:g}", c.maxMinutes), CommonFilter::Float);
        m_speedInput = makeInput(
            "Speed (x)", {size.width / 2, bottomY},
            fmt::format("{:g}", c.speed), CommonFilter::Float);
        m_maxFpsInput = makeInput(
            "Max FPS", {size.width / 2 + 110.f, bottomY},
            fmt::format("{:g}", c.maxFps), CommonFilter::Float);

        float const infoDX = 46.f;
        float const infoDY = 20.f;
        makeInfoButton({size.width / 2 - 110.f + infoDX, topY + infoDY}, 0);
        makeInfoButton({size.width / 2 + infoDX, topY + infoDY}, 1);
        makeInfoButton({size.width / 2 + 110.f + infoDX, topY + infoDY}, 2);
        makeInfoButton({size.width / 2 - 110.f + infoDX, bottomY + infoDY}, 3);
        makeInfoButton({size.width / 2 + infoDX, bottomY + infoDY}, 4);
        makeInfoButton({size.width / 2 + 110.f + infoDX, bottomY + infoDY}, 6);

        m_graphicsToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(TrainSettingsPopup::onToggleGraphics), 0.6f);
        if (m_graphicsToggle) {
            m_graphicsToggle->setPosition(40.f, 66.f);
            m_graphicsToggle->toggle(c.hideGraphics);
            m_buttonMenu->addChild(m_graphicsToggle);
        }
        if (auto hideLabel = CCLabelBMFont::create("Hide gfx", "bigFont.fnt")) {
            hideLabel->setScale(0.35f);
            hideLabel->setAnchorPoint({0.f, 0.5f});
            hideLabel->setPosition(56.f, 66.f);
            m_mainLayer->addChild(hideLabel);
        }
        makeInfoButton({18.f, 66.f}, 5);

        // Advanced button
        if (auto adv = makeIconNode("GJ_optionsBtn_001.png", "Adv", 30.f)) {
            auto b = CCMenuItemSpriteExtra::create(
                adv, this, menu_selector(TrainSettingsPopup::onAdvanced));
            if (b) {
                b->setPosition(size.width - 110.f, 66.f);
                m_buttonMenu->addChild(b);
            }
        }
        if (auto l = CCLabelBMFont::create("Advanced", "bigFont.fnt")) {
            l->setScale(0.32f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(size.width - 92.f, 66.f);
            m_mainLayer->addChild(l);
        }

        // bottom row: Train (center), Sessions + Playbacks (right)
        if (auto trainSprite = ButtonSprite::create("Train")) {
            auto trainButton = CCMenuItemSpriteExtra::create(
                trainSprite, this, menu_selector(TrainSettingsPopup::onTrain));
            if (trainButton) {
                trainButton->setPosition(size.width / 2 - 30.f, 28.f);
                m_buttonMenu->addChild(trainButton);
            }
        }

        if (auto icon = makeIconNode("gj_folderBtn_001.png", "PB", 30.f)) {
            auto btn = CCMenuItemSpriteExtra::create(
                icon, this, menu_selector(TrainSettingsPopup::onPlaybacks));
            if (btn) {
                btn->setPosition(size.width - 80.f, 28.f);
                m_buttonMenu->addChild(btn);
            }
        }
        if (auto icon = makeIconNode("GJ_savedBtn_001.png", "Ses", 32.f)) {
            auto btn = CCMenuItemSpriteExtra::create(
                icon, this, menu_selector(TrainSettingsPopup::onSessions));
            if (btn) {
                btn->setPosition(size.width - 40.f, 28.f);
                m_buttonMenu->addChild(btn);
            }
        }
        return true;
    }

    void onAdvanced(CCObject*) {
        if (auto p = AdvancedSettingsPopup::create()) p->show();
    }

    void onPlaybacks(CCObject*) {
        auto playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) return;
        auto popup = PlaybackListPopup::create(
            m_pauseLayer,
            neatgd::PlaybackStore::levelKeyFor(playLayer->m_level),
            [self = Ref(this)]() { self->onClose(nullptr); });
        if (popup) popup->show();
    }

    void onSessions(CCObject*) {
        auto playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) return;
        auto popup = SessionBrowserPopup::create(
            m_pauseLayer,
            neatgd::PlaybackStore::levelKeyFor(playLayer->m_level),
            [self = Ref(this)]() { self->onClose(nullptr); });
        if (popup) popup->show();
    }

    void onToggleGraphics(CCObject*) {}

    void onCycleMode(CCObject*) {
        m_modeChoice = (m_modeChoice + 1) % 3;
        if (m_modeBtnSprite) m_modeBtnSprite->setString(modeName(m_modeChoice));
    }

    void onTrain(CCObject*) {
        TrainingConfig const d;
        // core fields come from the visible inputs; advanced fields come from
        // the saved values written by the Advanced popup.
        auto mod = Mod::get();
        int const population = parseIntField(m_populationInput, d.population, 5, 2000);
        int const gens = parseIntField(m_generationsInput, d.maxGenerations, 1, 1000000);
        int const stag = parseIntField(m_stagnationInput, d.stagnationLimit, 1, 1000000);
        double const minutes = parseDoubleField(m_maxMinutesInput, d.maxMinutes, 0.0, 1440.0);
        double const speed = parseDoubleField(m_speedInput, d.speed, 1.0, 32.0);
        double const fps = parseDoubleField(m_maxFpsInput, d.maxFps, 60.0, 2000.0);
        bool const hide = m_graphicsToggle && m_graphicsToggle->isToggled();
        int const mode = m_modeChoice;

        mod->setSavedValue<int64_t>("population", population);
        mod->setSavedValue<int64_t>("max-generations", gens);
        mod->setSavedValue<int64_t>("stagnation-limit", stag);
        mod->setSavedValue<double>("max-minutes", minutes);
        mod->setSavedValue<double>("speed", speed);
        mod->setSavedValue<double>("max-fps", fps);
        mod->setSavedValue<bool>("hide-graphics", hide);
        mod->setSavedValue<int64_t>("mode", mode);

        TrainingConfig config = configFromSaved();
        config.population = population;
        config.maxGenerations = gens;
        config.stagnationLimit = stag;
        config.maxMinutes = minutes;
        config.speed = speed;
        config.maxFps = fps;
        config.hideGraphics = hide;
        config.mode = mode;

        Ref<PauseLayer> pause = m_pauseLayer;
        this->onClose(nullptr);

        if (!pause) return;
        auto playLayer = PlayLayer::get();
        if (!playLayer) {
            log::warn("NEATGD: no PlayLayer, cannot train");
            return;
        }
        if (!NEATManager::get()->beginTraining(config)) return;

        pause->onResume(nullptr);
        playLayer->resetLevelFromStart();
    }

public:
    static TrainSettingsPopup* create(PauseLayer* pause) {
        auto ret = new TrainSettingsPopup();
        if (ret->init(pause)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ===========================================================================
// Pause-menu button.
// ===========================================================================
class $modify(NEATPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menuNode = this->getChildByID("left-button-menu");
        if (!menuNode) {
            log::warn("NEATGD: left-button-menu not found on PauseLayer");
            return;
        }
        auto menu = typeinfo_cast<CCMenu*>(menuNode);
        if (!menu) {
            log::warn("NEATGD: left-button-menu is not a CCMenu");
            return;
        }

        CCNode* sprite = CCSprite::create("itzar.png"_spr);
        if (!sprite) {
            log::warn("NEATGD: itzar.png missing, falling back to text button");
            sprite = ButtonSprite::create("NEAT");
        }
        if (!sprite) return;

        auto const size = sprite->getContentSize();
        float const largest = std::max(size.width, size.height);
        if (largest > 0.f) sprite->setScale(40.f / largest);

        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(NEATPauseLayer::onNEAT));
        if (!button) return;
        button->setID("neat-button"_spr);

        menu->addChild(button);
        menu->updateLayout();
    }

    void onNEAT(CCObject*) {
        auto mgr = NEATManager::get();
        std::string levelKey = "unknown";
        if (auto pl = PlayLayer::get(); pl && pl->m_level) {
            levelKey = PlaybackStore::levelKeyFor(pl->m_level);
        }

        if (mgr->isActive()) {
            // running -> open the live control panel instead of a blind stop
            if (auto popup = TrainingControlPopup::create(this, levelKey)) {
                popup->show();
            }
            return;
        }
        if (auto popup = TrainSettingsPopup::create(this)) {
            popup->show();
        }
    }
};
