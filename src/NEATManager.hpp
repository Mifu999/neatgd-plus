#pragma once

#include "Neat.hpp"
#include "PlaybackStore.hpp"
#include "Sequence.hpp"
#include "SessionStore.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace neatgd {

struct TrainingConfig {
    // core
    int population = 50;
    int maxGenerations = 50;
    int stagnationLimit = 15;
    double maxMinutes = 10.0;
    double speed = 16.0;
    double maxFps = 360.0;
    bool hideGraphics = false;

    // fitness shaping
    double jumpPenalty = 0.02;

    // NEAT hyper-parameters (defaults reproduce the original behaviour)
    double addConnProb = 0.08;
    double addNodeProb = 0.03;
    double weightMutateProb = 0.8;
    double perturbProb = 0.9;
    double weightPower = 1.0;
    double survivalThreshold = 0.5;
    double mutateOnlyProb = 0.25;
    double compatThreshold = 3.0;
    int championThreshold = 5;
    int targetSpecies = 0;   // 0 = fixed compat threshold; >0 = adaptive
    int activation = 0;      // 0 sigmoid, 1 tanh
    bool recurrent = false;

    // 0 = fresh random seed each run; otherwise fixed for reproducibility
    uint32_t seed = 0;

    // learning mode: 0 = NEAT (neuroevolution), 1 = Climber (sequence search)
    int mode = 0;

    NeatParams toParams() const {
        NeatParams p;
        p.addConnProb = addConnProb;
        p.addNodeProb = addNodeProb;
        p.weightMutateProb = weightMutateProb;
        p.perturbProb = perturbProb;
        p.weightPower = weightPower;
        p.survivalThreshold = survivalThreshold;
        p.mutateOnlyProb = mutateOnlyProb;
        p.compatThreshold = compatThreshold;
        p.championThreshold = championThreshold;
        p.targetSpecies = targetSpecies;
        p.activation = activation;
        p.recurrent = recurrent;
        return p;
    }
};

// Reads just the display metadata out of a session file (handles both the
// legacy v1 layout and the current v2 layout). id / levelKey / timestamp are
// filled in by the caller from the filename.
bool peekSession(std::filesystem::path const& path, SessionMeta& out);

class NEATManager {
public:
    enum class Phase { Idle, Training, Showcase };

    static NEATManager* get();

    Phase phase() const { return m_phase; }
    bool isActive() const { return m_phase != Phase::Idle; }
    int mode() const { return m_config.mode; }
    double trainingSpeed() const { return m_config.speed; }
    bool hideGraphics() const { return m_config.hideGraphics; }

    bool beginTraining(TrainingConfig config);
    // Loads a session by its source level + id. If the player is currently on a
    // *different* level, the evolved brains are transferred (level-specific
    // tapes are cleared and training restarts from generation 0); on the same
    // level it is a seamless full resume. Returns false on error (see
    // lastError()).
    bool loadSession(std::string const& sourceLevelKey, int64_t id);
    bool resumeTraining(std::string const& levelKey, int64_t sessionId) {
        return loadSession(levelKey, sessionId);
    }
    void stop(char const* reason);

    // Saves the full live training state as a new session without interrupting
    // the run. Returns the new session id, or 0 on failure.
    int64_t saveCheckpoint();

    // Live controls usable while a run is in progress.
    void setSpeed(double speed);
    void setHideGraphics(bool hide);
    bool wasTransfer() const { return m_lastLoadTransfer; }
    std::string const& lastError() const { return m_lastError; }

    bool beginAttempt();
    bool attemptInProgress() const { return m_attemptActive; }
    bool replaying() const {
        return (m_attemptActive || m_showcaseActive)
            && m_step < m_takeoverStep;
    }
    bool shouldHoldCurrent(std::vector<double> const& inputs, bool holding);
    void endAttempt(double percent, bool died, int jumps);

    bool takeFinishedPrompt(std::string& textOut);
    bool armShowcase();
    bool beginPlayback(Playback const& playback);
    void onShowcaseStart();
    bool showcaseActive() const { return m_showcaseActive; }
    bool shouldHold(std::vector<double> const& inputs, bool holding);
    void onShowcaseEnd(double percent);

    // HUD / viewer accessors.
    bool hasBestGenome() const { return m_bestGenome.nodeCount > 0; }
    Genome const& bestGenome() const { return m_bestGenome; }
    int speciesCount() const {
        return m_population ? m_population->lastSpeciesCount() : 0;
    }
    double bestFitness() const { return m_bestFitness; }
    int generation() const { return m_generation; }

    std::string statusText() const;
    std::string progressText() const;

private:
    void finishTraining(bool solved, char const* reason);
    void savePlayback(bool completed);
    bool writeSession(std::string const& levelKey, std::string const& levelName,
                      int64_t id) const;
    int64_t freshSessionId(std::string const& levelKey) const;
    void applyTrainingFps();
    void restoreFps();
    bool tapeStateAt(int step);

    Phase m_phase = Phase::Idle;
    TrainingConfig m_config;
    std::unique_ptr<Population> m_population;

    std::unique_ptr<Network> m_currentNet;
    bool m_attemptActive = false;
    int m_cursor = 0;

    std::vector<int> m_replayToggles;
    size_t m_replayCursor = 0;
    bool m_replayHold = false;
    int m_takeoverStep = 0;
    int m_step = 0;
    std::vector<int> m_recordToggles;
    bool m_recordHold = false;
    int m_frontierCap = std::numeric_limits<int>::max();
    std::mt19937 m_rng{std::random_device{}()};

    int64_t m_resumeSourceId = 0;
    bool m_lastLoadTransfer = false;
    std::string m_lastError;
    uint32_t m_seed = 0;
    int m_attemptsThisRun = 0;

    // Climber (sequence) mode state
    int m_frontier = 0;
    int m_seqStuck = 0;
    SequenceParams m_seqParams;
    std::vector<int> m_frontierTape;  // hybrid: shared locked prefix to the frontier
    bool m_repairing = false;         // this attempt is re-searching locked clicks
    int m_netJumps = 0;               // presses the network itself made this attempt
    int m_repairWindow = 0;           // frames unlocked on this attempt
    int m_lateralMoves = 0;           // accepted same-distance route changes

    int m_generation = 0;
    int m_sinceImproved = 0;
    double m_bestFitness = 0.0;
    double m_generationBest = 0.0;
    bool m_solved = false;
    Genome m_bestGenome;
    std::chrono::steady_clock::time_point m_trainStart;

    std::unique_ptr<Network> m_showcaseNet;
    bool m_showcaseActive = false;

    bool m_promptPending = false;
    std::string m_promptText;

    float m_savedFpsTarget = 0.f;
    bool m_fpsOverridden = false;
};

}
