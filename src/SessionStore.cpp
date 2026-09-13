#include "SessionStore.hpp"

#include "NEATManager.hpp"  // for neatgd::peekSession
#include "PlaybackStore.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

using namespace geode::prelude;

namespace neatgd {

namespace {

constexpr uint32_t IDX_MAGIC = 0x58444953;  // "SIDX"
constexpr uint32_t IDX_VERSION = 2;
constexpr uint32_t MAX_ENTRIES = 100000;
constexpr uint32_t MAX_STR = 4096;

template <typename T>
void writePod(std::ostream& out, T const& v) {
    out.write(reinterpret_cast<char const*>(&v), sizeof(T));
}
template <typename T>
bool readPod(std::istream& in, T& v) {
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return in.good();
}
void writeStr(std::ostream& out, std::string const& s) {
    writePod(out, static_cast<uint32_t>(s.size()));
    if (!s.empty()) out.write(s.data(), static_cast<std::streamsize>(s.size()));
}
bool readStr(std::istream& in, std::string& s) {
    uint32_t len = 0;
    if (!readPod(in, len) || len > MAX_STR) return false;
    s.resize(len);
    if (len > 0) in.read(s.data(), len);
    return static_cast<bool>(in);
}

// Parse "<levelKey>_<id>.dat" -> (levelKey, id). The id is the trailing numeric
// token; the level key may itself contain underscores (editor_, local_my_lvl).
bool parseSessionFile(std::string const& stem, std::string& levelKey,
                      int64_t& id) {
    auto const us = stem.find_last_of('_');
    if (us == std::string::npos || us + 1 >= stem.size()) return false;
    std::string const idPart = stem.substr(us + 1);
    if (idPart.empty()
        || !std::all_of(idPart.begin(), idPart.end(),
                        [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }
    try {
        id = std::stoll(idPart);
    } catch (...) {
        return false;
    }
    levelKey = stem.substr(0, us);
    return !levelKey.empty();
}

std::vector<SessionMeta> readIndexRaw() {
    std::vector<SessionMeta> list;
    std::ifstream in(SessionStore::indexFile(), std::ios::binary);
    if (!in) return list;
    uint32_t magic = 0, version = 0, count = 0;
    if (!readPod(in, magic) || magic != IDX_MAGIC) return list;
    if (!readPod(in, version) || version < 1 || version > IDX_VERSION) return list;
    if (!readPod(in, count) || count > MAX_ENTRIES) return list;
    list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SessionMeta m;
        if (!readPod(in, m.id) || !readStr(in, m.levelKey)
            || !readStr(in, m.levelName) || !readStr(in, m.displayName)
            || !readPod(in, m.generation) || !readPod(in, m.bestFitness)
            || !readPod(in, m.population) || !readPod(in, m.inputCount)
            || !readPod(in, m.timestamp)) {
            break;
        }
        if (version >= 2) {
            if (!readPod(in, m.mode)) break;
        } else {
            m.mode = 0;
        }
        list.push_back(std::move(m));
    }
    return list;
}

}

namespace SessionStore {

std::filesystem::path sessionDir() {
    return Mod::get()->getSaveDir() / "sessions";
}

std::filesystem::path indexFile() {
    return sessionDir() / "index.dat";
}

std::string defaultName(SessionMeta const& meta) {
    std::time_t const t = static_cast<std::time_t>(meta.timestamp);
    std::tm tm{};
#ifdef GEODE_IS_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return fmt::format("{} ({:.1f}%)", buf, meta.bestFitness);
}

bool save(std::vector<SessionMeta> const& list) {
    auto const path = indexFile();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    writePod(out, IDX_MAGIC);
    writePod(out, IDX_VERSION);
    writePod(out, static_cast<uint32_t>(list.size()));
    for (auto const& m : list) {
        writePod(out, m.id);
        writeStr(out, m.levelKey);
        writeStr(out, m.levelName);
        writeStr(out, m.displayName);
        writePod(out, m.generation);
        writePod(out, m.bestFitness);
        writePod(out, m.population);
        writePod(out, m.inputCount);
        writePod(out, m.timestamp);
        writePod(out, m.mode);
    }
    return out.good();
}

std::vector<SessionMeta> loadIndex() {
    auto list = readIndexRaw();

    // Index the known (id, levelKey) pairs.
    auto present = [&](int64_t id, std::string const& key) {
        return std::any_of(list.begin(), list.end(), [&](SessionMeta const& m) {
            return m.id == id && m.levelKey == key;
        });
    };

    // Drop entries whose backing file disappeared.
    std::error_code ec;
    list.erase(
        std::remove_if(
            list.begin(), list.end(),
            [&](SessionMeta const& m) {
                return !std::filesystem::exists(
                    PlaybackStore::sessionFileFor(m.levelKey, m.id), ec);
            }),
        list.end());

    // Adopt orphan session files (e.g. created by an older NEATGD build) so
    // they show up in the global browser too.
    bool dirty = false;
    if (std::filesystem::exists(sessionDir(), ec)) {
        for (auto const& entry :
             std::filesystem::directory_iterator(sessionDir(), ec)) {
            if (!entry.is_regular_file()) continue;
            auto const& p = entry.path();
            if (p.extension() != ".dat") continue;
            if (p.filename() == "index.dat") continue;

            std::string levelKey;
            int64_t id = 0;
            if (!parseSessionFile(p.stem().string(), levelKey, id)) continue;
            if (present(id, levelKey)) continue;

            SessionMeta m;
            if (!neatgd::peekSession(p, m)) continue;
            m.id = id;
            m.levelKey = levelKey;
            m.timestamp = id;
            if (m.displayName.empty()) m.displayName = defaultName(m);
            list.push_back(std::move(m));
            dirty = true;
        }
    }

    std::sort(list.begin(), list.end(),
              [](SessionMeta const& a, SessionMeta const& b) {
                  return a.timestamp > b.timestamp;
              });

    if (dirty) save(list);
    return list;
}

std::vector<SessionMeta> forLevel(std::string const& levelKey) {
    auto all = loadIndex();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](SessionMeta const& m) {
                                 return m.levelKey != levelKey;
                             }),
              all.end());
    return all;
}

bool upsert(SessionMeta const& meta) {
    auto list = readIndexRaw();
    auto it = std::find_if(list.begin(), list.end(), [&](SessionMeta const& m) {
        return m.id == meta.id && m.levelKey == meta.levelKey;
    });
    if (it != list.end()) {
        std::string const keepName = it->displayName;
        *it = meta;
        if (it->displayName.empty()) it->displayName = keepName;
    } else {
        SessionMeta copy = meta;
        if (copy.displayName.empty()) copy.displayName = defaultName(copy);
        list.push_back(std::move(copy));
    }
    return save(list);
}

bool rename(int64_t id, std::string const& levelKey, std::string const& name) {
    auto list = readIndexRaw();
    bool found = false;
    for (auto& m : list) {
        if (m.id == id && m.levelKey == levelKey) {
            m.displayName = name;
            found = true;
            break;
        }
    }
    return found && save(list);
}

bool remove(int64_t id, std::string const& levelKey) {
    PlaybackStore::deleteSession(levelKey, id);
    auto list = readIndexRaw();
    auto const before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](SessionMeta const& m) {
                                  return m.id == id && m.levelKey == levelKey;
                              }),
               list.end());
    if (list.size() == before) return true;  // nothing indexed, file removed
    return save(list);
}

}

}
