#include "NEATManager.hpp"
#include "GameHook.hpp"
#include "SessionStore.hpp"

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <limits>
#include <random>

using namespace geode::prelude;

namespace neatgd {

namespace {
constexpr double SOLVED_FITNESS = 99.0;
constexpr double PRESS_THRESHOLD = 0.55;
constexpr double RELEASE_THRESHOLD = 0.45;
constexpr int TAKEOVER_LEAD_MIN = 120;
constexpr int TAKEOVER_LEAD_MAX = 600;
// generations without improvement before the hand-over point starts moving
// further back, and how many generations per doubling after that
constexpr int STALE_LEAD_AFTER = 8;
constexpr int STALE_LEAD_TIER = 6;

constexpr uint32_t SESSION_MAGIC = 0x5345474E;  // "NGES"
constexpr uint32_t SESSION_VERSION = 3;

template <typename T>
void writePodS(std::ostream& out, T const& v) {
    out.write(reinterpret_cast<char const*>(&v), sizeof(T));
}
template <typename T>
bool readPodS(std::istream& in, T& v) {
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(in);
}
void writeStrS(std::ostream& out, std::string const& s) {
    writePodS(out, static_cast<uint32_t>(s.size()));
    if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
}
bool readStrS(std::istream& in, std::string& s) {
    uint32_t len = 0;
    if (!readPodS(in, len) || len > 65535) return false;
    s.resize(len);
    if (len > 0) in.read(s.data(), len);
    return static_cast<bool>(in);
}

std::string currentLevelName() {
    if (auto pl = PlayLayer::get()) {
        if (pl->m_level) return std::string(pl->m_level->m_levelName);
    }
    return "";
}
}

// ---------------------------------------------------------------------------
// Session metadata peeking (used by the global session browser).
// ---------------------------------------------------------------------------
bool peekSession(std::filesystem::path const& path, SessionMeta& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint32_t magic = 0, version = 0;
    if (!readPodS(in, magic) || magic != SESSION_MAGIC) return false;
    if (!readPodS(in, version)) return false;

    if (version >= 2) {
        uint32_t inputCount = 0;
        int population = 0, generation = 0;
        double bestFitness = 0.0;
        std::string levelName;
        if (!readPodS(in, inputCount) || !readPodS(in, population)
            || !readPodS(in, generation) || !readPodS(in, bestFitness)
            || !readStrS(in, levelName)) {
            return false;
        }
        out.inputCount = static_cast<int>(inputCount);
        out.population = population;
        out.generation = generation;
        out.bestFitness = bestFitness;
        out.levelName = std::move(levelName);
        out.mode = 0;
        if (version >= 3) {
            uint8_t mode = 0;
            if (!readPodS(in, mode)) return false;
            out.mode = static_cast<int>(mode);
        }
        return true;
    }

    // Legacy v1 layout: config block, then counters. Read just enough.
    int population = 0, maxGen = 0, stag = 0, generation = 0, sinceImp = 0;
    double maxMin = 0, speed = 0, maxFps = 0, bestFitness = 0;
    uint8_t hide = 0;
    if (!readPodS(in, population) || !readPodS(in, maxGen) || !readPodS(in, stag)
        || !readPodS(in, maxMin) || !readPodS(in, speed) || !readPodS(in, maxFps)
        || !readPodS(in, hide) || !readPodS(in, generation)
        || !readPodS(in, sinceImp) || !readPodS(in, bestFitness)) {
        return false;
    }
    out.inputCount = static_cast<int>(INPUT_COUNT);
    out.population = population;
    out.generation = generation;
    out.bestFitness = bestFitness;
    out.levelName = "";
    return true;
}

NEATManager* NEATManager::get() {
    static NEATManager instance;
    return &instance;
}

bool NEATManager::beginTraining(TrainingConfig config) {
    if (m_phase != Phase::Idle) {
        log::warn("NEATGD: training already active, ignoring request");
        return false;
    }

    m_config = config;
    m_seed = config.seed != 0 ? config.seed : std::random_device{}();
    if (config.mode != 1) {
        // NEAT (0) and Hybrid (2) both evolve a population of networks.
        m_population = std::make_unique<Population>(
            config.population, static_cast<int>(INPUT_COUNT), 1, m_seed,
            config.toParams());
    } else {
        m_population.reset();
    }
    m_rng.seed(m_seed);
    m_frontier = 0;
    m_seqStuck = 0;
    m_frontierTape.clear();
    m_repairing = false;
    m_repairWindow = 0;
    m_lateralMoves = 0;
    m_currentNet.reset();
    m_attemptActive = false;
    m_cursor = 0;
    m_generation = 0;
    m_sinceImproved = 0;
    m_bestFitness = 0.0;
    m_generationBest = 0.0;
    m_solved = false;
    m_bestGenome = Genome{};
    m_showcaseNet.reset();
    m_showcaseActive = false;
    m_promptPending = false;
    m_resumeSourceId = 0;
    m_lastLoadTransfer = false;
    m_attemptsThisRun = 0;
    m_trainStart = std::chrono::steady_clock::now();

    m_phase = Phase::Training;
    applyTrainingFps();

    log::info(
        "NEATGD: {} training started - population {}, max {} "
        "generations, stagnation limit {}, budget {:.1f}min, speed {:.1f}x, "
        "seed {}",
        config.mode == 1 ? "Climber" : (config.mode == 2 ? "Hybrid" : "NEAT"),
        config.population, config.maxGenerations, config.stagnationLimit,
        config.maxMinutes, config.speed, m_seed);
    return true;
}

void NEATManager::applyTrainingFps() {
    auto gm = GameManager::sharedState();
    if (!gm || m_config.maxFps <= 0.0) return;
    m_savedFpsTarget = gm->m_customFPSTarget;
    m_fpsOverridden = true;
    gm->m_customFPSTarget = static_cast<float>(m_config.maxFps);
    gm->updateCustomFPS();
    log::info(
        "NEATGD: frame-rate target raised to {:.0f} for training (was {:.0f})",
        m_config.maxFps, m_savedFpsTarget);
}

void NEATManager::restoreFps() {
    if (!m_fpsOverridden) return;
    m_fpsOverridden = false;
    if (auto gm = GameManager::sharedState()) {
        gm->m_customFPSTarget = m_savedFpsTarget;
        gm->updateCustomFPS();
        log::info("NEATGD: frame-rate target restored to {:.0f}",
                  m_savedFpsTarget);
    }
}

void NEATManager::setSpeed(double speed) {
    m_config.speed = std::clamp(speed, 1.0, 32.0);
}

void NEATManager::setHideGraphics(bool hide) {
    m_config.hideGraphics = hide;
}

void NEATManager::stop(char const* reason) {
    if (m_phase == Phase::Idle) return;
    log::info("NEATGD: stopped ({}) - best fitness {:.1f}%", reason, m_bestFitness);
    if (m_phase == Phase::Training) savePlayback(false);
    restoreFps();
    m_phase = Phase::Idle;
    m_currentNet.reset();
    m_attemptActive = false;
    m_population.reset();
    m_showcaseNet.reset();
    m_showcaseActive = false;
    m_promptPending = false;
}

bool NEATManager::beginAttempt() {
    if (m_phase != Phase::Training) return false;

    if (m_config.mode == 1) {
        // Climber: the whole attempt is a tape replay - the locked prefix that
        // already works, then a mutated / freshly explored suffix. The look-back
        // widens (deep rewinds) the longer the frontier stays stuck.
        int const lookback =
            climberBacktrack(m_seqParams, m_seqStuck, m_frontier, m_rng);
        int const horizon = m_frontier + m_seqParams.exploreWindow;
        m_replayToggles = buildCandidate(
            m_bestGenome.tapeToggles, m_frontier, lookback, horizon,
            m_seqParams, m_rng, m_seqStuck);
        m_replayCursor = 0;
        m_replayHold = false;
        m_recordToggles.clear();
        m_recordHold = false;
        m_netJumps = 0;
        m_step = 0;
        m_takeoverStep = std::numeric_limits<int>::max();  // never hand to a net
        m_attemptActive = true;
        return true;
    }

    if (!m_population) return false;
    if (m_cursor >= m_population->size()) return false;
    Genome const& g = m_population->genome(m_cursor);
    m_currentNet = std::make_unique<Network>(
        g, m_config.activation, m_config.recurrent);

    m_replayCursor = 0;
    m_replayHold = false;
    m_recordToggles.clear();
    m_recordHold = false;
    m_netJumps = 0;
    m_step = 0;

    if (m_config.mode == 2) {
        // Hybrid: every genome replays the SHARED locked prefix up to the
        // global frontier, then the network takes over past it.
        //
        // When the frontier stalls, the problem is usually NOT at the wall but
        // in the locked clicks leading into it - the player arrives with a
        // position/velocity from which the wall is impossible. So instead of
        // handing the network control at some earlier point (which asks it to
        // improvise everything from there and throws the locked progress away),
        // we REPAIR the tape: unlock a bounded window of clicks before the wall
        // and re-search just those, keeping everything before intact.
        m_repairing = false;
        m_replayToggles = m_frontierTape;
        int lead = std::uniform_int_distribution<int>(
            TAKEOVER_LEAD_MIN, TAKEOVER_LEAD_MAX)(m_rng);

        if (m_seqStuck >= m_seqParams.repairAfter && m_frontier > 0) {
            int const window =
                repairWindow(m_seqParams, m_seqStuck, m_frontier, m_rng);
            if (window > 0) {
                m_replayToggles = buildCandidate(
                    m_frontierTape, m_frontier, window, m_frontier,
                    m_seqParams, m_rng, m_seqStuck);
                m_repairing = true;
                m_repairWindow = window;
                lead = 0;  // the repaired tape owns everything up to the wall
            }
        }
        m_takeoverStep = std::max(0, m_frontier - lead);
    } else {
        // NEAT: each genome replays its own inherited tape, then the network
        // takes over a little before where that tape died.
        //
        // If the whole population has stopped improving, that short hand-over
        // is itself the trap: the inherited tape always delivers the player to
        // the wall in the same doomed state and the network only ever gets the
        // last few hundred frames to fix it. The longer the run stagnates, the
        // further back control is handed, so earlier clicks can be re-played
        // differently.
        m_replayToggles = g.tapeToggles;
        int lead = std::uniform_int_distribution<int>(
            TAKEOVER_LEAD_MIN, TAKEOVER_LEAD_MAX)(m_rng);
        if (m_sinceImproved > STALE_LEAD_AFTER && g.reachStep > 0) {
            int const tiers = std::min(
                (m_sinceImproved - STALE_LEAD_AFTER) / STALE_LEAD_TIER, 5);
            int const widened = std::min(
                TAKEOVER_LEAD_MAX << tiers, g.reachStep);
            if (widened > TAKEOVER_LEAD_MIN) {
                std::uniform_int_distribution<int> d(
                    TAKEOVER_LEAD_MIN, widened);
                lead = std::min(d(m_rng), d(m_rng));  // bias to shallow rewinds
            }
        }
        m_takeoverStep = std::max(0, g.reachStep - lead);
    }

    m_attemptActive = true;
    return true;
}

bool NEATManager::tapeStateAt(int step) {
    while (m_replayCursor < m_replayToggles.size()
           && m_replayToggles[m_replayCursor] <= step) {
        m_replayHold = !m_replayHold;
        ++m_replayCursor;
    }
    return m_replayHold;
}

bool NEATManager::shouldHoldCurrent(
    std::vector<double> const& inputs, bool holding) {
    if (!m_attemptActive) return false;
    int const step = m_step++;
    bool hold;
    if (step < m_takeoverStep) {
        hold = tapeStateAt(step);
    } else if (m_currentNet) {
        double const out = m_currentNet->eval(inputs);
        hold = out > (holding ? RELEASE_THRESHOLD : PRESS_THRESHOLD);
    } else {
        hold = m_recordHold;  // climber mode has no net; keep current state
    }
    if (hold != m_recordHold) {
        m_recordHold = hold;
        m_recordToggles.push_back(step);
        // count only presses the network itself decided: penalising a genome
        // for the clicks it merely replayed from the locked prefix is both
        // unfair and, on a click-heavy level, a large constant bias against
        // every solution that needs a lot of orbs.
        if (hold && step >= m_takeoverStep) ++m_netJumps;
    }
    return hold;
}

void NEATManager::endAttempt(double percent, bool died, int jumps) {
    (void)jumps;  // total presses incl. the replayed prefix; fitness uses
                  // m_netJumps (the network's own presses) instead
    if (m_phase != Phase::Training) return;
    if (!m_attemptActive) return;
    m_attemptActive = false;
    m_currentNet.reset();
    ++m_attemptsThisRun;

    if (m_config.mode == 1) {
        // Climber: keep the run only if it reached further than the frontier.
        int const reached = m_step;
        bool const solved = !died && percent >= SOLVED_FITNESS;

        if (reached > m_frontier || solved) {
            m_bestGenome = Genome{};
            m_bestGenome.tapeToggles = std::move(m_recordToggles);
            m_bestGenome.reachStep = reached;
            m_frontier = reached;
            m_bestFitness = std::max(m_bestFitness, percent);
            m_seqStuck = 0;
        } else if (reached == m_frontier && m_frontier > 0
                   && m_seqStuck >= m_seqParams.repairAfter
                   && std::uniform_real_distribution<double>(0.0, 1.0)(m_rng)
                       < 0.35) {
            // lateral move: same distance, different clicks. Changes the state
            // the player arrives at the wall in, which is what actually escapes
            // a dead-end where the wall is unreachable from the locked approach.
            m_bestGenome.tapeToggles = std::move(m_recordToggles);
            m_bestGenome.reachStep = reached;
            ++m_lateralMoves;
            ++m_seqStuck;
        } else {
            ++m_seqStuck;
        }
        m_recordToggles.clear();

        if (solved) {
            m_bestFitness = std::max(m_bestFitness, percent);
            finishTraining(true, "level beaten!");
            return;
        }

        double const elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_trainStart).count();
        if (m_config.maxMinutes > 0.0 && elapsed >= m_config.maxMinutes * 60.0) {
            log::warn(
                "NEATGD: climber time budget of {:.1f}min exhausted at {:.1f}%",
                m_config.maxMinutes, m_bestFitness);
            finishTraining(false, "time budget exhausted");
        }
        return;  // otherwise keep climbing (reset -> beginAttempt)
    }

    if (!m_population) return;

    if (m_config.mode == 2) {
        // Hybrid frontier bookkeeping.
        int const reached = m_step;
        if (reached > m_frontier) {
            // real progress: lock this path in for the whole population
            m_frontierTape = m_recordToggles;  // copy before the move below
            m_frontier = reached;
            m_seqStuck = 0;
            m_repairing = false;
        } else if (m_repairing && reached >= m_frontier && m_frontier > 0) {
            // A LATERAL move: a repaired tape that still reaches the wall but
            // gets there through different clicks. No extra progress, yet this
            // is exactly what breaks a softlock - the player now arrives at the
            // wall in a different state, so the section past it becomes
            // reachable where it was not. Accepted often enough to keep
            // exploring, rarely enough not to thrash.
            if (std::uniform_real_distribution<double>(0.0, 1.0)(m_rng) < 0.35) {
                m_frontierTape = m_recordToggles;
                ++m_lateralMoves;
            }
            ++m_seqStuck;  // no progress: keep escalating the repair window
        } else {
            ++m_seqStuck;
        }
    }

    m_population->setTape(m_cursor, std::move(m_recordToggles), m_step);
    m_recordToggles.clear();

    m_population->setFitness(
        m_cursor,
        std::max(0.0, percent - m_config.jumpPenalty * m_netJumps));
    m_generationBest = std::max(m_generationBest, percent);
    if (percent > m_bestFitness) {
        m_bestFitness = percent;
        m_bestGenome = m_population->genome(m_cursor);
        m_sinceImproved = -1;
    }
    if (!died && percent >= SOLVED_FITNESS) {
        log::info("NEATGD: a genome survived to the end of the level!");
        m_solved = true;
    }

    ++m_cursor;

    if (m_solved) {
        finishTraining(true, "level beaten!");
        return;
    }
    if (m_cursor < m_population->size()) return;

    double const elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_trainStart).count();
    ++m_generation;
    m_sinceImproved = m_sinceImproved < 0 ? 0 : m_sinceImproved + 1;

    log::info(
        "NEATGD: generation {}/{} - best {:.1f}%, all-time {:.1f}% "
        "({:.0f}s elapsed, {} species)",
        m_generation, m_config.maxGenerations, m_generationBest,
        m_bestFitness, elapsed, m_population->lastSpeciesCount());
    m_generationBest = 0.0;

    if (m_generation >= m_config.maxGenerations) {
        log::info("NEATGD: generation limit reached");
        finishTraining(false, "generation limit reached");
        return;
    }
    if (m_sinceImproved >= m_config.stagnationLimit) {
        log::warn(
            "NEATGD: no improvement for {} generations, stopping early at "
            "{:.1f}%",
            m_config.stagnationLimit, m_bestFitness);
        finishTraining(
            false,
            fmt::format(
                "stagnant for {} generations", m_config.stagnationLimit)
                .c_str());
        return;
    }
    if (m_config.maxMinutes > 0.0 && elapsed >= m_config.maxMinutes * 60.0) {
        log::warn(
            "NEATGD: time budget of {:.1f}min exhausted, stopping at {:.1f}%",
            m_config.maxMinutes, m_bestFitness);
        finishTraining(false, "time budget exhausted");
        return;
    }

    bool const explore = m_sinceImproved >= 3;
    if (explore) {
        log::info(
            "NEATGD: stagnant for {} generations - boosting mutation and "
            "injecting fresh genomes",
            m_sinceImproved);
    }
    m_population->epoch(explore, m_bestFitness > 0.0 ? &m_bestGenome : nullptr);
    m_cursor = 0;
}

void NEATManager::finishTraining(bool solved, char const* reason) {
    restoreFps();

    // Snapshot the finished population as a transferable session before it is
    // torn down, so a completed run's brains can be carried to other levels.
    if (m_population && m_bestFitness > 0.0) {
        if (auto pl = PlayLayer::get(); pl && pl->m_level) {
            auto const key = PlaybackStore::levelKeyFor(pl->m_level);
            int64_t const id = freshSessionId(key);
            writeSession(key, currentLevelName(), id);
        }
    }

    savePlayback(true);

    m_population.reset();
    m_currentNet.reset();
    m_attemptActive = false;

    if (m_bestFitness <= 0.0) {
        log::warn("NEATGD: training produced no usable genome");
        if (auto notif = Notification::create(
                fmt::format("NEAT done ({}) - no usable genome", reason),
                NotificationIcon::Info, 4.f)) {
            notif->show();
        }
        m_phase = Phase::Idle;
        return;
    }

    m_phase = Phase::Idle;
    char const* note = m_config.mode == 1
        ? "<cj>The click sequence is saved to this level's playback list - "
          "watch it, or resume to keep climbing.</c>"
        : "<cj>It's saved to this level's playback list, and a transferable "
          "session was stored.</c>";
    m_promptText = fmt::format(
        "Training finished{} (<cy>{}</c>)\n"
        "Best run: <cg>{:.1f}%</c>\n\n"
        "Watch the playback now?\n"
        "{}",
        solved ? " - <cg>level beaten!</c>" : "", reason, m_bestFitness, note);
    m_promptPending = true;
    log::info(
        "NEATGD: training finished{} - best {:.1f}%, prompting for playback",
        solved ? " (level beaten)" : "", m_bestFitness);
}

int64_t NEATManager::freshSessionId(std::string const& levelKey) const {
    int64_t id = static_cast<int64_t>(std::time(nullptr));
    std::error_code ec;
    while (std::filesystem::exists(
        PlaybackStore::sessionFileFor(levelKey, id), ec)) {
        ++id;
    }
    return id;
}

int64_t NEATManager::saveCheckpoint() {
    if (m_phase != Phase::Training) {
        m_lastError = "no training in progress";
        return 0;
    }
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) {
        m_lastError = "no level";
        return 0;
    }
    auto const key = PlaybackStore::levelKeyFor(pl->m_level);
    int64_t const id = freshSessionId(key);
    if (!writeSession(key, currentLevelName(), id)) {
        m_lastError = "could not write session file";
        return 0;
    }
    log::info("NEATGD: checkpoint saved for level {} (id {})", key, id);
    if (auto notif = Notification::create(
            fmt::format("Checkpoint saved ({:.1f}%)", m_bestFitness),
            NotificationIcon::Success, 2.f)) {
        notif->show();
    }
    return id;
}

void NEATManager::savePlayback(bool completed) {
    auto playLayer = PlayLayer::get();
    if (!playLayer || !playLayer->m_level) return;
    auto const key = PlaybackStore::levelKeyFor(playLayer->m_level);

    if (m_bestFitness <= 0.0 || m_bestGenome.reachStep <= 0) {
        if (m_resumeSourceId != 0) {
            PlaybackStore::deleteSession(key, m_resumeSourceId);
            SessionStore::remove(m_resumeSourceId, key);
            m_resumeSourceId = 0;
        }
        return;
    }

    Playback p;
    p.timestamp = static_cast<int64_t>(std::time(nullptr));
    p.percent = m_bestFitness;
    p.reachStep = m_bestGenome.reachStep;
    p.toggles = m_bestGenome.tapeToggles;
    p.completed = completed;

    std::time_t const t = static_cast<std::time_t>(p.timestamp);
    std::tm tm{};
#ifdef GEODE_IS_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    p.name = fmt::format("{} ({:.1f}%)", buf, m_bestFitness);

    auto list = PlaybackStore::load(key);
    if (m_resumeSourceId != 0) {
        list.erase(
            std::remove_if(
                list.begin(), list.end(),
                [&](Playback const& e) {
                    return e.timestamp == m_resumeSourceId;
                }),
            list.end());
        PlaybackStore::deleteSession(key, m_resumeSourceId);
        SessionStore::remove(m_resumeSourceId, key);
        m_resumeSourceId = 0;
    }

    // A stopped (incomplete) run keeps a resumable session tied to this
    // playback entry, so it can be continued later from the playback list.
    if (!completed && !writeSession(key, currentLevelName(), p.timestamp)) {
        log::warn("NEATGD: failed to write resume session for level {}", key);
    }

    list.push_back(std::move(p));
    if (PlaybackStore::save(key, list)) {
        log::info("NEATGD: playback saved to library for level {}", key);
    }
}

bool NEATManager::writeSession(
    std::string const& levelKey, std::string const& levelName,
    int64_t id) const {
    if (m_config.mode != 1 && !m_population) return false;
    auto const path = PlaybackStore::sessionFileFor(levelKey, id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    writePodS(out, SESSION_MAGIC);
    writePodS(out, SESSION_VERSION);

    // --- peek-friendly display header (kept first; see peekSession) ---
    writePodS(out, static_cast<uint32_t>(INPUT_COUNT));
    writePodS(out, m_config.population);
    writePodS(out, m_generation);
    writePodS(out, m_bestFitness);
    writeStrS(out, levelName);
    writePodS(out, static_cast<uint8_t>(m_config.mode));  // v3: mode byte

    if (m_config.mode == 1) {
        // --- climber body ---
        writePodS(out, m_config.maxMinutes);
        writePodS(out, m_config.speed);
        writePodS(out, m_config.maxFps);
        writePodS(out, static_cast<uint8_t>(m_config.hideGraphics ? 1 : 0));
        writePodS(out, m_config.jumpPenalty);
        writePodS(out, m_seed);
        writePodS(out, m_frontier);
        writePodS(out, m_seqStuck);
        writePodS(out, static_cast<uint32_t>(m_bestGenome.tapeToggles.size()));
        if (!m_bestGenome.tapeToggles.empty()) {
            out.write(
                reinterpret_cast<char const*>(m_bestGenome.tapeToggles.data()),
                static_cast<std::streamsize>(m_bestGenome.tapeToggles.size())
                    * sizeof(int));
        }
    } else {
        // --- NEAT body ---
        writePodS(out, m_config.maxGenerations);
        writePodS(out, m_config.stagnationLimit);
        writePodS(out, m_config.maxMinutes);
        writePodS(out, m_config.speed);
        writePodS(out, m_config.maxFps);
        writePodS(out, static_cast<uint8_t>(m_config.hideGraphics ? 1 : 0));

        writePodS(out, m_config.addConnProb);
        writePodS(out, m_config.addNodeProb);
        writePodS(out, m_config.weightMutateProb);
        writePodS(out, m_config.perturbProb);
        writePodS(out, m_config.weightPower);
        writePodS(out, m_config.survivalThreshold);
        writePodS(out, m_config.mutateOnlyProb);
        writePodS(out, m_config.compatThreshold);
        writePodS(out, m_config.championThreshold);
        writePodS(out, m_config.targetSpecies);
        writePodS(out, m_config.activation);
        writePodS(out, static_cast<uint8_t>(m_config.recurrent ? 1 : 0));
        writePodS(out, m_config.jumpPenalty);
        writePodS(out, m_seed);

        writePodS(out, m_sinceImproved);
        writePodS(out, m_generationBest);
        writePodS(out, m_cursor);

        writeGenome(out, m_bestGenome);
        m_population->writeState(out);

        if (m_config.mode == 2) {
            // hybrid: persist the shared locked prefix and how far it reaches
            writePodS(out, m_frontier);
            writePodS(out, m_seqStuck);
            writePodS(out, static_cast<uint32_t>(m_frontierTape.size()));
            if (!m_frontierTape.empty()) {
                out.write(
                    reinterpret_cast<char const*>(m_frontierTape.data()),
                    static_cast<std::streamsize>(m_frontierTape.size())
                        * sizeof(int));
            }
        }
    }

    if (!out.good()) return false;

    // Register / refresh the global index entry.
    SessionMeta meta;
    meta.id = id;
    meta.levelKey = levelKey;
    meta.levelName = levelName;
    meta.generation = m_generation;
    meta.bestFitness = m_bestFitness;
    meta.population = m_config.population;
    meta.inputCount = static_cast<int>(INPUT_COUNT);
    meta.mode = m_config.mode;
    meta.timestamp = id;
    SessionStore::upsert(meta);

    return true;
}

bool NEATManager::loadSession(std::string const& sourceLevelKey, int64_t id) {
    m_lastError.clear();
    if (m_phase != Phase::Idle) {
        m_lastError = "already active";
        return false;
    }

    auto const path = PlaybackStore::sessionFileFor(sourceLevelKey, id);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_lastError = "session file missing";
        log::warn("NEATGD: session missing for level {}", sourceLevelKey);
        return false;
    }

    uint32_t magic = 0, version = 0;
    if (!readPodS(in, magic) || magic != SESSION_MAGIC) {
        m_lastError = "not a NEATGD session";
        return false;
    }
    if (!readPodS(in, version) || version < 1 || version > SESSION_VERSION) {
        m_lastError = "unsupported session version";
        return false;
    }

    TrainingConfig config;
    int generation = 0, sinceImproved = 0, cursor = 0;
    double bestFitness = 0.0, generationBest = 0.0;
    uint32_t inputCount = static_cast<uint32_t>(INPUT_COUNT);
    uint32_t seed = 0;
    Genome bestGenome;
    int mode = 0;
    int frontier = 0, seqStuck = 0;
    std::vector<int> seqTape;

    if (version >= 2) {
        int population = 0;
        std::string levelName;
        if (!readPodS(in, inputCount) || !readPodS(in, population)
            || !readPodS(in, generation) || !readPodS(in, bestFitness)
            || !readStrS(in, levelName)) {
            m_lastError = "corrupt session header";
            return false;
        }
        config.population = population;
        if (version >= 3) {
            uint8_t mb = 0;
            if (!readPodS(in, mb)) {
                m_lastError = "corrupt session header";
                return false;
            }
            mode = static_cast<int>(mb);
        }

        if (mode == 1) {
            // --- climber body ---
            uint8_t hide = 0;
            uint32_t tapeLen = 0;
            if (!readPodS(in, config.maxMinutes) || !readPodS(in, config.speed)
                || !readPodS(in, config.maxFps) || !readPodS(in, hide)
                || !readPodS(in, config.jumpPenalty) || !readPodS(in, seed)
                || !readPodS(in, frontier) || !readPodS(in, seqStuck)
                || !readPodS(in, tapeLen) || tapeLen > 10'000'000u) {
                m_lastError = "corrupt climber session";
                return false;
            }
            config.hideGraphics = hide != 0;
            config.seed = seed;
            seqTape.resize(tapeLen);
            if (tapeLen > 0) {
                in.read(reinterpret_cast<char*>(seqTape.data()),
                        static_cast<std::streamsize>(tapeLen) * sizeof(int));
                if (!in) {
                    m_lastError = "corrupt climber tape";
                    return false;
                }
            }
        } else {
            // --- NEAT body (v2/v3) ---
            uint8_t hide = 0, recur = 0;
            if (!readPodS(in, config.maxGenerations)
                || !readPodS(in, config.stagnationLimit)
                || !readPodS(in, config.maxMinutes) || !readPodS(in, config.speed)
                || !readPodS(in, config.maxFps) || !readPodS(in, hide)
                || !readPodS(in, config.addConnProb)
                || !readPodS(in, config.addNodeProb)
                || !readPodS(in, config.weightMutateProb)
                || !readPodS(in, config.perturbProb)
                || !readPodS(in, config.weightPower)
                || !readPodS(in, config.survivalThreshold)
                || !readPodS(in, config.mutateOnlyProb)
                || !readPodS(in, config.compatThreshold)
                || !readPodS(in, config.championThreshold)
                || !readPodS(in, config.targetSpecies)
                || !readPodS(in, config.activation) || !readPodS(in, recur)
                || !readPodS(in, config.jumpPenalty) || !readPodS(in, seed)
                || !readPodS(in, sinceImproved) || !readPodS(in, generationBest)
                || !readPodS(in, cursor) || !readGenome(in, bestGenome)) {
                m_lastError = "corrupt session body";
                return false;
            }
            config.hideGraphics = hide != 0;
            config.recurrent = recur != 0;
            config.seed = seed;
        }
    } else {
        // legacy v1 (NEAT only)
        uint8_t hide = 0;
        if (!readPodS(in, config.population)
            || !readPodS(in, config.maxGenerations)
            || !readPodS(in, config.stagnationLimit)
            || !readPodS(in, config.maxMinutes) || !readPodS(in, config.speed)
            || !readPodS(in, config.maxFps) || !readPodS(in, hide)
            || !readPodS(in, generation) || !readPodS(in, sinceImproved)
            || !readPodS(in, bestFitness) || !readPodS(in, generationBest)
            || !readPodS(in, cursor) || !readGenome(in, bestGenome)) {
            m_lastError = "corrupt v1 session";
            return false;
        }
        config.hideGraphics = hide != 0;
    }

    // A session saved before new sensors existed is migrated, not rejected: the
    // genomes are re-numbered onto the wider input layer (see
    // Population::upgradeInputs). Only a session from a *newer* build, or one
    // whose layout we don't recognise, is refused.
    bool const needsSensorUpgrade =
        inputCount != static_cast<uint32_t>(INPUT_COUNT);
    if (needsSensorUpgrade
        && inputCount != static_cast<uint32_t>(LEGACY_INPUT_COUNT_V1)) {
        m_lastError = fmt::format(
            "incompatible senses ({} vs {} inputs)", inputCount,
            static_cast<int>(INPUT_COUNT));
        log::warn("NEATGD: {}", m_lastError);
        return false;
    }
    config.mode = mode;

    // Decide resume vs transfer based on the level we're standing in.
    std::string targetKey = sourceLevelKey;
    if (auto pl = PlayLayer::get(); pl && pl->m_level) {
        targetKey = PlaybackStore::levelKeyFor(pl->m_level);
    }
    bool const transfer = targetKey != sourceLevelKey;

    if (mode == 1) {
        // A climber tape encodes a specific click sequence for one level's
        // geometry - it cannot be transferred to a different level.
        if (transfer) {
            m_lastError = "climber runs are level-specific (can't transfer)";
            log::warn("NEATGD: {}", m_lastError);
            return false;
        }
        m_config = config;
        m_seed = seed;
        m_population.reset();
        m_rng.seed(seed != 0 ? seed : std::random_device{}());
        m_currentNet.reset();
        m_attemptActive = false;
        m_showcaseNet.reset();
        m_showcaseActive = false;
        m_promptPending = false;
        m_attemptsThisRun = 0;
        m_lastLoadTransfer = false;
        m_bestGenome = Genome{};
        m_bestGenome.tapeToggles = std::move(seqTape);
        m_bestGenome.reachStep = frontier;
        m_frontier = frontier;
        m_seqStuck = seqStuck;
        m_bestFitness = bestFitness;
        m_generation = 0;
        m_sinceImproved = 0;
        m_cursor = 0;
        m_resumeSourceId = id;
        m_solved = false;
        m_trainStart = std::chrono::steady_clock::now();
        m_phase = Phase::Training;
        applyTrainingFps();
        log::info(
            "NEATGD: resumed climber for level {} at frontier {}, best {:.1f}%",
            sourceLevelKey, frontier, bestFitness);
        return true;
    }

    auto population = std::make_unique<Population>(
        std::max(1, config.population), static_cast<int>(INPUT_COUNT), 1,
        seed != 0 ? seed : std::random_device{}(), config.toParams());
    if (!population->readState(in)) {
        m_lastError = "session population corrupt";
        log::warn("NEATGD: resume session corrupt for level {}", sourceLevelKey);
        return false;
    }
    population->setParams(config.toParams());

    if (needsSensorUpgrade) {
        // grow every brain onto the new sensor layout, then keep going
        population->upgradeInputs(static_cast<int>(INPUT_COUNT));
        upgradeGenomeInputs(bestGenome, static_cast<int>(INPUT_COUNT));
        log::info(
            "NEATGD: migrated session from {} to {} inputs (new senses start "
            "at weight 0)",
            inputCount, static_cast<int>(INPUT_COUNT));
    }

    if (mode == 2) {
        // hybrid: the shared locked prefix + frontier follow the population blob
        uint32_t tapeLen = 0;
        if (!readPodS(in, frontier) || !readPodS(in, seqStuck)
            || !readPodS(in, tapeLen) || tapeLen > 10'000'000u) {
            m_lastError = "corrupt hybrid session";
            log::warn("NEATGD: hybrid resume corrupt for level {}", sourceLevelKey);
            return false;
        }
        seqTape.resize(tapeLen);
        if (tapeLen > 0) {
            in.read(reinterpret_cast<char*>(seqTape.data()),
                    static_cast<std::streamsize>(tapeLen) * sizeof(int));
            if (!in) {
                m_lastError = "corrupt hybrid tape";
                return false;
            }
        }
    }

    m_config = config;
    m_seed = seed != 0 ? seed : 0;
    m_population = std::move(population);
    m_currentNet.reset();
    m_attemptActive = false;
    m_showcaseNet.reset();
    m_showcaseActive = false;
    m_promptPending = false;
    m_attemptsThisRun = 0;
    m_lastLoadTransfer = transfer;

    if (transfer) {
        // Keep the evolved brains, discard everything tied to the old level's
        // geometry, and restart the search on this level.
        m_population->stripTapes();
        m_bestGenome = Genome{};
        m_generation = 0;
        m_sinceImproved = 0;
        m_bestFitness = 0.0;
        m_generationBest = 0.0;
        m_cursor = 0;
        m_resumeSourceId = 0;  // don't consume the source: it can be reused
        // the locked click prefix is level-specific - drop it, keep the brains
        m_frontier = 0;
        m_frontierTape.clear();
        m_seqStuck = 0;
        m_repairing = false;
        m_lateralMoves = 0;
        log::info(
            "NEATGD: transferred {} brains from level {} onto level {}",
            m_population->size(), sourceLevelKey, targetKey);
    } else {
        m_bestGenome = std::move(bestGenome);
        m_generation = generation;
        m_sinceImproved = sinceImproved;
        m_bestFitness = bestFitness;
        m_generationBest = generationBest;
        m_cursor = std::clamp(cursor, 0, std::max(0, m_population->size() - 1));
        m_resumeSourceId = id;
        if (mode == 2) {
            // restore the shared locked prefix so the population keeps its progress
            m_frontier = frontier;
            m_frontierTape = std::move(seqTape);
            m_seqStuck = seqStuck;
        }
        log::info(
            "NEATGD: resumed training for level {} at generation {}, best "
            "{:.1f}%",
            sourceLevelKey, m_generation, m_bestFitness);
    }
    m_solved = false;
    m_trainStart = std::chrono::steady_clock::now();

    m_phase = Phase::Training;
    applyTrainingFps();
    return true;
}

bool NEATManager::takeFinishedPrompt(std::string& textOut) {
    if (!m_promptPending) return false;
    m_promptPending = false;
    textOut = std::move(m_promptText);
    m_promptText.clear();
    return true;
}

bool NEATManager::armShowcase() {
    if (m_phase != Phase::Idle || m_bestFitness <= 0.0) {
        return false;
    }
    // NEAT bests have a network; climber bests are a pure click sequence.
    if (m_bestGenome.nodeCount <= 0 && m_bestGenome.tapeToggles.empty()) {
        return false;
    }
    m_showcaseActive = false;
    m_phase = Phase::Showcase;
    return true;
}

bool NEATManager::beginPlayback(Playback const& playback) {
    if (m_phase != Phase::Idle) return false;
    if (playback.reachStep <= 0) return false;
    m_bestGenome = Genome{};
    m_bestGenome.tapeToggles = playback.toggles;
    m_bestGenome.reachStep = playback.reachStep;
    m_bestFitness = playback.percent;
    m_showcaseActive = false;
    m_phase = Phase::Showcase;
    log::info("NEATGD: replaying saved playback '{}' ({:.1f}%)",
              playback.name, playback.percent);
    return true;
}

void NEATManager::onShowcaseStart() {
    if (m_phase != Phase::Showcase) return;
    m_showcaseNet = m_bestGenome.nodeCount > 0
        ? std::make_unique<Network>(
              m_bestGenome, m_config.activation, m_config.recurrent)
        : nullptr;
    m_replayToggles = m_bestGenome.tapeToggles;
    m_replayCursor = 0;
    m_replayHold = false;
    m_step = 0;
    m_takeoverStep = m_bestGenome.reachStep;
    m_showcaseActive = true;
    log::info("NEATGD: showcase attempt started (trained best {:.1f}%)",
              m_bestFitness);
}

bool NEATManager::shouldHold(std::vector<double> const& inputs, bool holding) {
    if (!m_showcaseActive) return false;
    int const step = m_step++;
    if (step < m_takeoverStep) return tapeStateAt(step);
    if (!m_showcaseNet) return false;
    double const out = m_showcaseNet->eval(inputs);
    return out > (holding ? RELEASE_THRESHOLD : PRESS_THRESHOLD);
}

void NEATManager::onShowcaseEnd(double percent) {
    if (!m_showcaseActive) return;
    m_showcaseActive = false;
    m_showcaseNet.reset();
    log::info("NEATGD: showcase finished at {:.1f}% - returning control",
              percent);
    m_phase = Phase::Idle;
}

std::string NEATManager::progressText() const {
    if (m_phase != Phase::Training) return "";

    double const elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_trainStart).count();
    double const aps = elapsed > 0.1 ? m_attemptsThisRun / elapsed : 0.0;

    auto fmtClock = [](double s) {
        int const r = static_cast<int>(std::max(0.0, s));
        return r >= 3600
            ? fmt::format("{}:{:02}:{:02}", r / 3600, (r / 60) % 60, r % 60)
            : fmt::format("{}:{:02}", r / 60, r % 60);
    };

    if (m_config.mode == 1) {
        std::string tail;
        if (m_config.maxMinutes > 0.0) {
            tail = fmt::format(
                "\nETA {}", fmtClock(m_config.maxMinutes * 60.0 - elapsed));
        }
        return fmt::format(
            "climber\nbest {:.1f}%\nfrontier {}\nstuck {}\n{:.0f} att/s{}",
            m_bestFitness, m_frontier, m_seqStuck, aps, tail);
    }

    if (!m_population) return "";
    double const perGen = m_population->size();
    double const total = perGen * m_config.maxGenerations;
    double const done = m_generation * perGen + m_cursor;
    double const frac = total > 0.0 ? std::min(done / total, 1.0) : 0.0;

    std::string eta = "--:--";
    if (frac > 0.002 && frac < 1.0) {
        double remain = elapsed * (1.0 - frac) / frac;
        if (m_config.maxMinutes > 0.0) {
            remain = std::min(
                remain, std::max(0.0, m_config.maxMinutes * 60.0 - elapsed));
        }
        eta = fmtClock(remain);
    }
    return fmt::format(
        "{}{:.1f}% complete\nETA {}\nbest {:.1f}%\nstale {}/{}\n{:.0f} att/s",
        m_config.mode == 2
            ? fmt::format("hybrid - frontier {}\n", m_frontier)
            : std::string{},
        frac * 100.0, eta, m_bestFitness,
        std::max(m_sinceImproved, 0), m_config.stagnationLimit, aps);
}

std::string NEATManager::statusText() const {
    switch (m_phase) {
        case Phase::Training: {
            if (m_config.mode == 1) {
                return fmt::format(
                    "Climber  best {:.1f}%  frontier {}  stuck {}  "
                    "tape {}  lat {}  attempt {}",
                    m_bestFitness, m_frontier, m_seqStuck,
                    static_cast<int>(m_bestGenome.tapeToggles.size()),
                    m_lateralMoves, m_attemptsThisRun);
            }
            int const popSize = m_population ? m_population->size() : 0;
            int const species =
                m_population ? m_population->lastSpeciesCount() : 0;
            int const nodes = m_bestGenome.nodeCount > 0
                ? m_bestGenome.hiddenNodes()
                : 0;
            int const conns = m_bestGenome.nodeCount > 0
                ? m_bestGenome.enabledConns()
                : 0;
            return fmt::format(
                "{} gen {}/{}  genome {}/{}  best {:.1f}%  spec {}  "
                "stale {}/{}  net {}h/{}c{}",
                m_config.mode == 2 ? "Hybrid" : "NEAT",
                m_generation + 1, m_config.maxGenerations,
                std::min(m_cursor + 1, std::max(popSize, 1)), popSize,
                m_bestFitness, species, std::max(m_sinceImproved, 0),
                m_config.stagnationLimit, nodes, conns,
                m_config.mode == 2
                    ? fmt::format(
                        "  frontier {}  stuck {}{}", m_frontier, m_seqStuck,
                        m_repairing
                            ? fmt::format(
                                "  repair -{}f  lat {}", m_repairWindow,
                                m_lateralMoves)
                            : std::string{})
                    : std::string{});
        }
        case Phase::Showcase:
            return fmt::format("showcase  best {:.1f}%", m_bestFitness);
        default:
            return "";
    }
}

}
