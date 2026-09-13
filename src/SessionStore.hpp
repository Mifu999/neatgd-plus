#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neatgd {

// Lightweight, level-agnostic description of a saved training session. The full
// session blob (population, genomes, RNG, config) lives in its own file under
// sessions/; this is just what the browser needs to list and identify it.
struct SessionMeta {
    int64_t id = 0;            // timestamp, also the file id
    std::string levelKey;      // the level the session was trained on
    std::string levelName;     // human-readable level name (may be empty)
    std::string displayName;   // user-renamable label
    int generation = 0;
    double bestFitness = 0.0;
    int population = 0;
    int inputCount = 0;        // sensory layout; must match to load
    int mode = 0;              // 0 = NEAT, 1 = Climber (sequence)
    int64_t timestamp = 0;
};

// The global session index. Sessions from every level live here so the UI can
// offer both a "this level" view and a "global (all)" view, and so a session
// can be transferred onto any other level.
namespace SessionStore {

std::filesystem::path sessionDir();
std::filesystem::path indexFile();

// Reads the index, reconciles it with what's actually on disk (adopting orphan
// session files left by older versions, dropping entries whose file vanished),
// and returns the merged list newest-first.
std::vector<SessionMeta> loadIndex();

// Convenience filtered views.
std::vector<SessionMeta> forLevel(std::string const& levelKey);

bool save(std::vector<SessionMeta> const& list);
bool upsert(SessionMeta const& meta);
bool rename(int64_t id, std::string const& levelKey, std::string const& name);
bool remove(int64_t id, std::string const& levelKey);

// Default label for a freshly discovered/created session.
std::string defaultName(SessionMeta const& meta);

}

}
