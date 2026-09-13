#include "GameHook.hpp"
#include "NEATManager.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCAnimate.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCParticleSystem.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;
using namespace neatgd;

namespace neatgd {

namespace {

constexpr float FLOOR_TOP = 90.f;
constexpr float PLAYER_HALF = 15.f;

// Classify an object into what it means for *control*. Types come from
// Enums.hpp (GameObjectType) in the official Geode bindings for GD 2.2081.
// Returns false for objects a player never interacts with (decoration, etc).
bool classifyObject(GameObjectType type, LevelObject& lo) {
    using T = GameObjectType;
    switch (type) {
        case T::Solid:
        case T::Slope:
            lo.kind = ObjKind::Solid;
            lo.slope = type == T::Slope;
            return true;

        case T::Hazard:
        case T::AnimatedHazard:
            lo.kind = ObjKind::Hazard;
            lo.hazard = true;
            return true;

        // ---- rings / orbs: require a press WHILE touching them ----
        case T::YellowJumpRing:
        case T::PinkJumpRing:
        case T::RedJumpRing:
        case T::CustomRing:
            lo.kind = ObjKind::Orb;
            lo.flagJump = true;
            return true;
        case T::GravityRing:
        case T::GravityDashRing:
            lo.kind = ObjKind::Orb;
            lo.flagGravity = true;
            lo.flagDash = type == T::GravityDashRing;
            return true;
        case T::DashRing:
            lo.kind = ObjKind::Orb;
            lo.flagDash = true;
            return true;
        case T::GreenRing:
        case T::DropRing:
        case T::SpiderOrb:
        case T::TeleportOrb:
            lo.kind = ObjKind::Orb;
            return true;

        // ---- pads: fire on contact, no input required ----
        case T::YellowJumpPad:
            lo.kind = ObjKind::Pad;
            lo.flagJump = true;
            return true;
        case T::PinkJumpPad:
            lo.kind = ObjKind::Pad;
            lo.flagJump = true;
            return true;
        case T::RedJumpPad:
            lo.kind = ObjKind::Pad;
            lo.flagJump = true;
            lo.flagStrong = true;
            return true;
        case T::GravityPad:
        case T::SpiderPad:
            lo.kind = ObjKind::Pad;
            lo.flagGravity = true;
            return true;

        // ---- portals: change the rules mid-flight ----
        case T::ShipPortal:
        case T::CubePortal:
        case T::BallPortal:
        case T::UfoPortal:
        case T::WavePortal:
        case T::RobotPortal:
        case T::SpiderPortal:
        case T::SwingPortal:
            lo.kind = ObjKind::Portal;
            lo.flagMode = true;
            return true;
        case T::InverseGravityPortal:
        case T::NormalGravityPortal:
        case T::GravityTogglePortal:
            lo.kind = ObjKind::Portal;
            lo.flagGravity = true;
            return true;
        case T::MiniSizePortal:
        case T::RegularSizePortal:
            lo.kind = ObjKind::Portal;
            lo.flagSize = true;
            return true;
        case T::DualPortal:
        case T::SoloPortal:
            lo.kind = ObjKind::Portal;
            lo.flagDual = true;
            return true;

        default:
            return false;
    }
}

}

std::vector<LevelObject> extractLevelObjects(PlayLayer* layer) {
    std::vector<LevelObject> objects;
    if (!layer || !layer->m_objects) return objects;

    int orbs = 0, pads = 0, portals = 0;
    for (auto obj : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (!obj) continue;
        if (obj == layer->m_player1 || obj == layer->m_player2) continue;

        LevelObject lo;
        if (!classifyObject(obj->m_objectType, lo)) continue;

        auto const& rect = obj->getObjectRect();
        if (rect.size.width > 0.f && rect.size.height > 0.f) {
            lo.x = rect.origin.x + rect.size.width / 2.f;
            lo.y = rect.origin.y + rect.size.height / 2.f;
            lo.w = rect.size.width;
            lo.h = rect.size.height;
        } else {
            lo.x = obj->getPositionX();
            lo.y = obj->getPositionY();
        }
        lo.objectId = obj->m_objectID;
        switch (lo.kind) {
            case ObjKind::Orb: ++orbs; break;
            case ObjKind::Pad: ++pads; break;
            case ObjKind::Portal: ++portals; break;
            default: break;
        }
        objects.push_back(lo);
    }

    std::sort(
        objects.begin(), objects.end(),
        [](LevelObject const& a, LevelObject const& b) { return a.x < b.x; });
    log::info(
        "NEATGD: cached {} objects ({} orbs, {} pads, {} portals)",
        objects.size(), orbs, pads, portals);
    return objects;
}

static void resumePauseLayer() {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene || !scene->getChildren()) return;
    for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
        if (auto pause = typeinfo_cast<PauseLayer*>(child)) {
            pause->onResume(nullptr);
            return;
        }
    }
}

static bool trainingGraphicsHidden() {
    auto mgr = NEATManager::get();
    return mgr->phase() == NEATManager::Phase::Training && mgr->hideGraphics();
}

}

class $modify(NEATScheduler, CCScheduler) {
    void update(float dt) {
        auto mgr = NEATManager::get();
        if (mgr->phase() == NEATManager::Phase::Training) {
            dt *= static_cast<float>(mgr->trainingSpeed());
        }
        CCScheduler::update(dt);
    }
};

class $modify(NEATPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<LevelObject> cache;
        size_t scanStart = 0;
        bool holding = false;
        int jumps = 0;
        int playerCallDepth = 0;
        bool resetRequested = false;
        CCLabelBMFont* statusLabel = nullptr;
        CCLabelBMFont* centerLabel = nullptr;
        bool graphicsHidden = false;
        int frameCounter = 0;
        // frames the jump input has been in its current state (press or
        // release); feeds the rhythm sensor
        int holdFrames = 0;
    };

public:
    void setHolding(bool hold) {
        if (m_fields->holding == hold) return;
        m_fields->holding = hold;
        m_fields->holdFrames = 0;
        if (!m_player1) return;
        if (hold) {
            ++m_fields->jumps;
            m_player1->pushButton(PlayerButton::Jump);
        } else {
            m_player1->releaseButton(PlayerButton::Jump);
        }
    }

    size_t& playerScan() { return m_fields->scanStart; }
    bool holdingState() { return m_fields->holding; }
    int jumpCount() { return m_fields->jumps; }
    void tickHoldFrames() { ++m_fields->holdFrames; }
    CCLabelBMFont* centerLabel() { return m_fields->centerLabel; }

    std::vector<double> computeInputs(
        PlayerObject* player, bool holding, size_t& scan) {
        float const px = player->getPositionX();
        float const py = player->getPositionY();

        auto& cache = m_fields->cache;
        while (scan < cache.size()
               && cache[scan].x + cache[scan].w / 2.f < px - 30.f) {
            ++scan;
        }

        float const feetY = py - PLAYER_HALF;
        float const headY = py + PLAYER_HALF;

        float floorTop[SCAN_SAMPLES];
        float ceilBot[SCAN_SAMPLES];
        float hazUp[SCAN_SAMPLES];
        float hazDown[SCAN_SAMPLES];
        for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
            floorTop[s] = FLOOR_TOP;
            ceilBot[s] = headY + NORM_CLEAR;
            hazUp[s] = NORM_CLEAR;
            hazDown[s] = NORM_CLEAR;
        }

        // nearest actionable objects ahead (or slightly behind, for orbs the
        // player is standing in right now)
        LevelObject const* nearOrb = nullptr;
        LevelObject const* nearPad = nullptr;
        LevelObject const* nearPortal = nullptr;

        float const lookEnd = px + SCAN_OFFSETS[SCAN_SAMPLES - 1] + 30.f;
        for (size_t i = scan; i < cache.size(); ++i) {
            auto const& obj = cache[i];
            float const halfW = obj.w / 2.f;
            if (obj.x - halfW > lookEnd) break;

            if (obj.kind == ObjKind::Orb) {
                if (!nearOrb && obj.x + halfW >= px - 20.f) nearOrb = &obj;
                continue;
            }
            if (obj.kind == ObjKind::Pad) {
                if (!nearPad && obj.x + halfW >= px - 20.f) nearPad = &obj;
                continue;
            }
            if (obj.kind == ObjKind::Portal) {
                if (!nearPortal && obj.x + halfW >= px - 20.f) nearPortal = &obj;
                continue;
            }

            // terrain: solids and hazards only
            for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
                float const cx = px + SCAN_OFFSETS[s];
                if (std::abs(obj.x - cx) > halfW) continue;
                if (obj.hazard) {
                    if (obj.y >= py) {
                        hazUp[s] = std::min(hazUp[s], obj.y - py);
                    } else {
                        hazDown[s] = std::min(hazDown[s], py - obj.y);
                    }
                } else {
                    float const top = obj.y + obj.h / 2.f;
                    float const bot = obj.y - obj.h / 2.f;
                    if (top <= py + 60.f) {
                        floorTop[s] = std::max(floorTop[s], top);
                    } else if (bot >= py - 60.f) {
                        ceilBot[s] = std::min(ceilBot[s], bot);
                    }
                }
            }
        }

        std::vector<double> inputs;
        inputs.reserve(INPUT_COUNT);
        inputs.push_back((py - (FLOOR_TOP + PLAYER_HALF)) / NORM_Y);
        inputs.push_back(player->m_yVelocity / NORM_VEL);
        inputs.push_back(player->m_isOnGround ? 1.0 : 0.0);
        inputs.push_back(holding ? 1.0 : 0.0);
        inputs.push_back(player->m_isUpsideDown ? 1.0 : 0.0);
        inputs.push_back(player->m_isShip ? 1.0 : 0.0);
        inputs.push_back(player->m_isBird ? 1.0 : 0.0);
        inputs.push_back(player->m_isBall ? 1.0 : 0.0);
        inputs.push_back(player->m_isDart ? 1.0 : 0.0);
        inputs.push_back(player->m_isRobot ? 1.0 : 0.0);
        inputs.push_back(player->m_isSpider ? 1.0 : 0.0);
        inputs.push_back(player->m_isSwing ? 1.0 : 0.0);
        for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
            inputs.push_back(std::clamp(
                (floorTop[s] - feetY) / NORM_DY, -1.f, 1.f));
        }
        for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
            inputs.push_back(std::clamp(
                1.f - (ceilBot[s] - headY) / NORM_CLEAR, 0.f, 1.f));
        }
        for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
            inputs.push_back(std::clamp(
                1.f - hazUp[s] / NORM_CLEAR, 0.f, 1.f));
        }
        for (size_t s = 0; s < SCAN_SAMPLES; ++s) {
            inputs.push_back(std::clamp(
                1.f - hazDown[s] / NORM_CLEAR, 0.f, 1.f));
        }

        // ---- interaction block (20) ----
        // proximity is 1 when the object is on top of the player and falls to 0
        // at NORM_ORB / NORM_PORTAL, so "press now" is a learnable threshold.
        auto proximity = [](LevelObject const* o, float px_, float norm) {
            if (!o) return 0.f;
            return std::clamp(1.f - std::abs(o->x - px_) / norm, 0.f, 1.f);
        };
        auto vertical = [](LevelObject const* o, float py_) {
            if (!o) return 0.f;
            return std::clamp((o->y - py_) / NORM_DY, -1.f, 1.f);
        };

        inputs.push_back(proximity(nearOrb, px, NORM_ORB));
        inputs.push_back(vertical(nearOrb, py));
        inputs.push_back(nearOrb && nearOrb->flagJump ? 1.0 : 0.0);
        inputs.push_back(nearOrb && nearOrb->flagGravity ? 1.0 : 0.0);
        inputs.push_back(nearOrb && nearOrb->flagDash ? 1.0 : 0.0);
        // the single most actionable bit in the game: a ring is under the
        // player RIGHT NOW, so a press converts into a jump instead of nothing
        inputs.push_back(player->m_touchedRing ? 1.0 : 0.0);

        inputs.push_back(proximity(nearPad, px, NORM_ORB));
        inputs.push_back(vertical(nearPad, py));
        inputs.push_back(nearPad && nearPad->flagGravity ? 1.0 : 0.0);
        inputs.push_back(nearPad && nearPad->flagStrong ? 1.0 : 0.0);

        inputs.push_back(proximity(nearPortal, px, NORM_PORTAL));
        inputs.push_back(vertical(nearPortal, py));
        inputs.push_back(nearPortal && nearPortal->flagMode ? 1.0 : 0.0);
        inputs.push_back(nearPortal && nearPortal->flagGravity ? 1.0 : 0.0);
        inputs.push_back(nearPortal && nearPortal->flagSize ? 1.0 : 0.0);
        inputs.push_back(nearPortal && nearPortal->flagDual ? 1.0 : 0.0);

        inputs.push_back(std::clamp(player->m_playerSpeed / 2.0f, 0.f, 2.f));
        inputs.push_back(player->m_vehicleSize < 0.9f ? 1.0 : 0.0);
        inputs.push_back(player->m_isDashing ? 1.0 : 0.0);
        // how long the button has been in its current state: lets a network
        // learn tap rhythm and "release before the orb" without a clock
        inputs.push_back(std::clamp(
            static_cast<float>(m_fields->holdFrames) / NORM_HOLD, 0.f, 1.f));

        return inputs;
    }

    int& playerCallDepth() { return m_fields->playerCallDepth; }

    void updateStatusLabel() {
        auto mgr = NEATManager::get();

        bool const wantHidden =
            mgr->phase() == NEATManager::Phase::Training && mgr->hideGraphics();
        if (wantHidden != m_fields->graphicsHidden) {
            m_fields->graphicsHidden = wantHidden;
            if (m_objectLayer) m_objectLayer->setVisible(!wantHidden);
            if (m_background) m_background->setVisible(!wantHidden);
            if (wantHidden && !m_fields->centerLabel) {
                auto const winSize = CCDirector::sharedDirector()->getWinSize();
                m_fields->centerLabel = CCLabelBMFont::create("", "goldFont.fnt");
                if (m_fields->centerLabel) {
                    m_fields->centerLabel->setAlignment(kCCTextAlignmentCenter);
                    m_fields->centerLabel->setPosition(
                        winSize.width / 2, winSize.height / 2);
                    m_fields->centerLabel->setScale(0.9f);
                    this->addChild(m_fields->centerLabel, 9999);
                }
            }
            if (m_fields->centerLabel) {
                m_fields->centerLabel->setVisible(wantHidden);
            }
            if (wantHidden) {
                if (auto fmod = FMODAudioEngine::sharedEngine()) {
                    fmod->stopAllMusic(true);
                }
            }
        }

        auto& label = m_fields->statusLabel;
        if (!mgr->isActive()) {
            if (label) label->setVisible(false);
            return;
        }
        if (!label) {
            label = CCLabelBMFont::create("", "chatFont.fnt");
            if (!label) return;
            auto const winSize = CCDirector::sharedDirector()->getWinSize();
            label->setAnchorPoint({0.f, 1.f});
            label->setPosition(5.f, winSize.height - 5.f);
            label->setScale(0.6f);
            this->addChild(label, 9999);
        }
        label->setVisible(true);
        if (++m_fields->frameCounter % 10 == 0) {
            label->setString(mgr->statusText().c_str());
            if (m_fields->graphicsHidden && m_fields->centerLabel) {
                m_fields->centerLabel->setString(mgr->progressText().c_str());
            }
        }
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        m_fields->cache = extractLevelObjects(this);
        m_fields->scanStart = 0;
    }

    void resetLevel() {
        auto mgr = NEATManager::get();

        if (mgr->phase() == NEATManager::Phase::Training
            && mgr->attemptInProgress()) {
            mgr->endAttempt(this->getCurrentPercent(), true, m_fields->jumps);
        }

        PlayLayer::resetLevel();

        m_fields->scanStart = 0;
        m_fields->holding = false;
        m_fields->jumps = 0;
        m_fields->holdFrames = 0;
        m_fields->resetRequested = false;

        if (mgr->phase() == NEATManager::Phase::Training) {
            mgr->beginAttempt();
        } else if (mgr->phase() == NEATManager::Phase::Showcase) {
            mgr->onShowcaseStart();
        }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        updateStatusLabel();
        auto mgr = NEATManager::get();

        std::string prompt;
        if (mgr->takeFinishedPrompt(prompt)) {
            this->pauseGame(false);
            geode::createQuickPopup(
                "NEAT Training", prompt, "Later", "Watch",
                [](FLAlertLayer*, bool watch) {
                    if (!watch) return;
                    auto playLayer = PlayLayer::get();
                    if (!playLayer) return;
                    if (!NEATManager::get()->armShowcase()) return;
                    resumePauseLayer();
                    playLayer->resetLevelFromStart();
                });
            return;
        }

        if (m_fields->resetRequested) {
            m_fields->resetRequested = false;
            this->resetLevel();
            return;
        }

        if (mgr->phase() == NEATManager::Phase::Showcase
            && !mgr->showcaseActive() && m_player1 && !m_player1->m_isDead) {
            mgr->onShowcaseStart();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool const anticheatCall = object == m_anticheatSpike;
        auto mgr = NEATManager::get();

        if (!anticheatCall && mgr->phase() == NEATManager::Phase::Training
            && player == m_player1) {
            if (mgr->attemptInProgress()) {
                setHolding(false);
                mgr->endAttempt(
                    this->getCurrentPercent(), true, m_fields->jumps);
                m_fields->resetRequested = true;
            }
            return;
        }

        PlayLayer::destroyPlayer(player, object);

        if (anticheatCall || !player || !player->m_isDead) return;
        if (player != m_player1) return;

        if (mgr->showcaseActive()) {
            setHolding(false);
            mgr->onShowcaseEnd(this->getCurrentPercent());
        }
    }

    void levelComplete() {
        auto mgr = NEATManager::get();

        if (mgr->phase() == NEATManager::Phase::Training) {
            if (mgr->attemptInProgress()) {
                setHolding(false);
                mgr->endAttempt(100.0, false, m_fields->jumps);
            }
            m_fields->resetRequested = true;
            return;
        }

        if (mgr->showcaseActive()) {
            setHolding(false);
            mgr->onShowcaseEnd(100.0);
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        auto mgr = NEATManager::get();
        if (mgr->isActive()) {
            setHolding(false);
            mgr->stop("level quit");
        }
        PlayLayer::onQuit();
    }
};

class $modify(NEATDirector, CCDirector) {
    void drawScene() {
        if (!trainingGraphicsHidden()) {
            CCDirector::drawScene();
            return;
        }

        this->calculateDeltaTime();
        if (!m_bPaused) {
            this->getScheduler()->update(this->getDeltaTime());
        }
        if (m_pNextScene) {
            this->setNextScene();
        }

        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        kmGLPushMatrix();
        if (auto layer = static_cast<NEATPlayLayer*>(PlayLayer::get())) {
            if (auto label = layer->centerLabel()) {
                label->visit();
            }
        }
        kmGLPopMatrix();

        ++m_uTotalFrames;

        if (m_pobOpenGLView) {
            m_pobOpenGLView->swapBuffers();
        }
    }
};

class $modify(NEATParticleSystem, CCParticleSystem) {
    void update(float dt) {
        if (trainingGraphicsHidden()) return;
        CCParticleSystem::update(dt);
    }
};

class $modify(NEATAnimate, CCAnimate) {
    void update(float t) {
        if (trainingGraphicsHidden()) return;
        CCAnimate::update(t);
    }
};

class $modify(NEATPlayerObject, PlayerObject) {
    void update(float dt) {
        auto layer = static_cast<NEATPlayLayer*>(PlayLayer::get());
        if (!layer || this != layer->m_player1) {
            PlayerObject::update(dt);
            return;
        }

        auto mgr = NEATManager::get();
        if (layer->playerCallDepth() == 0 && !this->m_isDead) {
            layer->tickHoldFrames();
            if (mgr->phase() == NEATManager::Phase::Training
                && mgr->attemptInProgress()) {
                if (mgr->replaying()) {
                    layer->setHolding(
                        mgr->shouldHoldCurrent({}, layer->holdingState()));
                } else {
                    auto inputs = layer->computeInputs(
                        this, layer->holdingState(), layer->playerScan());
                    layer->setHolding(
                        mgr->shouldHoldCurrent(inputs, layer->holdingState()));
                }
            } else if (mgr->showcaseActive()) {
                if (mgr->replaying()) {
                    layer->setHolding(
                        mgr->shouldHold({}, layer->holdingState()));
                } else {
                    auto inputs = layer->computeInputs(
                        this, layer->holdingState(), layer->playerScan());
                    layer->setHolding(
                        mgr->shouldHold(inputs, layer->holdingState()));
                }
            }
        }
        ++layer->playerCallDepth();
        PlayerObject::update(dt);
        --layer->playerCallDepth();
    }
};
