#pragma once

#include <cstdint>
#include <iosfwd>
#include <random>
#include <unordered_map>
#include <vector>

namespace neatgd {

// --------------------------------------------------------------------------
// Tunable NEAT hyper-parameters.
//
// Every default below reproduces the original NEATGD behaviour exactly, so a
// genome / session created before these were exposed evolves identically when
// loaded with default params. New trainings can override any of them from the
// settings UI. Params are runtime config: they are NOT part of the serialized
// population blob (the manager stores them alongside the session and re-applies
// them on load), which keeps old session files byte-compatible.
// --------------------------------------------------------------------------
struct NeatParams {
    // structural mutation
    double addConnProb = 0.08;
    double addNodeProb = 0.03;
    // weight mutation
    double weightMutateProb = 0.8;   // chance to touch weights at all
    double perturbProb = 0.9;        // of those, chance to nudge vs replace
    double weightPower = 1.0;        // multiplier on the perturbation sigma
    // selection
    double survivalThreshold = 0.5;  // top fraction of a species that may breed
    int championThreshold = 5;       // species this big copy their champion as-is
    double mutateOnlyProb = 0.25;    // chance a child is asexual (mutate only)
    // speciation
    double compatThreshold = 3.0;    // starting compatibility distance
    int targetSpecies = 0;           // 0 = fixed threshold; >0 = adapt toward N
    // network dynamics
    int activation = 0;              // 0 = sigmoid(4.9) [default], 1 = tanh
    bool recurrent = false;          // allow recurrent / backward connections
};

struct ConnGene {
    int in = 0;
    int out = 0;
    double weight = 0.0;
    bool enabled = true;
    int innovation = 0;
};

struct Genome {
    int numInputs = 0;
    int numOutputs = 0;
    int nodeCount = 0;
    std::vector<double> depths;
    std::vector<ConnGene> conns;
    double fitness = 0.0;

    std::vector<int> tapeToggles;
    int reachStep = 0;

    int outputId(int i) const { return numInputs + 1 + i; }

    // counts used by the HUD / network viewer
    int enabledConns() const {
        int n = 0;
        for (auto const& c : conns) if (c.enabled) ++n;
        return n;
    }
    int hiddenNodes() const { return nodeCount - numInputs - 1 - numOutputs; }
};

void writeGenome(std::ostream& out, Genome const& g);
bool readGenome(std::istream& in, Genome& g);

// Re-number a genome built for a smaller input layer so it keeps working after
// new sensors are appended. Node ids are laid out as
//   [0 .. numInputs-1] inputs, [numInputs] bias, then outputs, then hidden,
// so everything from the bias onward simply shifts by the added input count.
// Behaviour is preserved exactly: the new inputs start with no connections.
void upgradeGenomeInputs(Genome& g, int newNumInputs);

class Network {
public:
    // activation: 0 sigmoid, 1 tanh (hidden only; output is always sigmoid so
    // the press/release thresholds keep their meaning). recurrent enables
    // backward links that read the previous frame's activations.
    explicit Network(Genome const& genome, int activation = 0,
                     bool recurrent = false);

    double eval(std::vector<double> const& inputs);

private:
    struct Link {
        int from;
        double weight;
    };
    struct Node {
        int id;
        std::vector<Link> incoming;     // forward (read current frame)
        std::vector<Link> recIncoming;  // recurrent (read previous frame)
    };
    int m_numInputs;
    int m_activation;
    bool m_recurrent;
    std::vector<Node> m_order;
    std::vector<double> m_values;
    std::vector<double> m_prev;
    int m_outputId;
};

class Population {
public:
    Population(int size, int numInputs, int numOutputs, uint32_t seed,
               NeatParams const& params = {});

    void setParams(NeatParams const& params) { m_params = params; }
    NeatParams const& params() const { return m_params; }

    int size() const { return static_cast<int>(m_genomes.size()); }
    Genome const& genome(int i) const { return m_genomes[i]; }
    void setFitness(int i, double fitness) { m_genomes[i].fitness = fitness; }
    void setTape(int i, std::vector<int> toggles, int reachStep) {
        m_genomes[i].tapeToggles = std::move(toggles);
        m_genomes[i].reachStep = reachStep;
    }

    Genome const& best() const;
    int lastSpeciesCount() const { return m_lastSpecies; }
    double compatThreshold() const { return m_compat; }

    // Drop every genome's level-specific tape / progress so the evolved brains
    // can continue training on a *different* level from scratch. Topology,
    // weights, innovation history and RNG are preserved.
    void stripTapes();

    // Grow the input layer of an already-evolved population (used when a new
    // build adds sensors). Every genome is re-numbered and gains zero-weight
    // connections from each new input to each output, so behaviour is
    // unchanged on load while weight mutation can start using the new senses
    // immediately. No-op if the population already has newNumInputs inputs.
    void upgradeInputs(int newNumInputs);

    void epoch(bool explore = false, Genome const* elite = nullptr);

    void writeState(std::ostream& out) const;
    bool readState(std::istream& in);

private:
    int innovationFor(int in, int out);
    void randomizeWeights(Genome& g);
    Genome freshGenome();
    void mutate(Genome& g, bool explore);
    void mutateWeights(Genome& g, bool explore);
    bool mutateAddConnection(Genome& g);
    void mutateAddNode(Genome& g);
    Genome crossover(Genome const& fitter, Genome const& other);
    static double compatibility(Genome const& a, Genome const& b);
    static void pruneDisabled(Genome& g);

    double rand01() { return m_uniform(m_rng); }
    double randWeight() { return m_uniform(m_rng) * 4.0 - 2.0; }

    std::vector<Genome> m_genomes;
    Genome m_proto;
    NeatParams m_params;
    double m_compat = 3.0;   // live (possibly adaptive) compatibility threshold
    int m_lastSpecies = 0;
    std::mt19937 m_rng;
    std::uniform_real_distribution<double> m_uniform{0.0, 1.0};
    std::normal_distribution<double> m_gauss{0.0, 0.5};
    std::unordered_map<int64_t, int> m_innovations;
    int m_nextInnovation = 0;
};

}
