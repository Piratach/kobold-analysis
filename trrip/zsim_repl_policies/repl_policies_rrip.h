// Copied from the Tako directory. Author: bschwedo

#pragma once

#include "mtrand.h"
#include "repl_policies.h"
#include "repl_policies_dip.h"

namespace platy {
namespace sim {

// TODO (bcs): Add stats. Look at zsim for example.

// TODO (bcs): How would Tako deadlock prevention deal with clock-based RRIP
// policies? When evicting an entry from strictly either callbackWays or not callbackWays,
// are the scores for all lines aged, or only for the subset?

// TODO (bcs): Doesn't need LRUReplPolicyWithoutCC?
class SRRIPReplPolicy : public ReplPolicy {
protected:
    StrongVec<LineIdx, int64_t> array;
    const LineIdx numLines;
    const int64_t vmax;
    MTRand rnd;

    // higher is most evictable.
    static const int64_t clean = std::numeric_limits<int64_t>::max();

public:
    SRRIPReplPolicy(const LineIdx _numLines, const uint32_t M)
        : numLines(_numLines), vmax((1 << M) - 1), rnd(4242) {
        qassert(vmax > 0);  // otherwise this is useless...
        array.resize(numLines, clean);
    }

    void update(const LineIdx id, const MemReq* req) override {
        (void)req;
        qassert(id.isValid());

        if (score(id) == clean) {
            changePrio(id, vmax - 1);  // predict long re-reference
        } else {
            changePrio(id, 0);  // predict near-immediate re-reference
        }
    }

    void replaced(const LineIdx id) override { changePrio(id, clean); }

    template <typename C>
    inline LineIdx rank(const MemReq* req, C cands) {
        (void)req;

        const uint32_t numCandidates = cands.size().get();
        LineIdx bestCands[numCandidates];
        int64_t bestPrio = -1;
        uint32_t pos = 0;

        for (auto ci = cands.begin(); ci != cands.end(); ci++) {
            qassert((*ci).isValid());

            // TODO (bcs): Make this CC-aware?
            const LineIdx c = *ci;
            const int64_t prio = score(c);

            if (prio == bestPrio) {
                bestCands[++pos] = c;
            } else if (prio > bestPrio) {
                bestPrio = prio;
                pos = 0;
                bestCands[0] = c;
            }
        }

        // Age cands if needed
        if (bestPrio < vmax) {
            const uint32_t aging = vmax - bestPrio;
            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                const LineIdx c = *ci;
                changePrio(c, score(c) + aging);
            }
        }

        return bestCands[rnd.randInt(pos)];  // break ties randomly
    }

    DECL_RANK_BINDINGS();

protected:
    inline int64_t score(const LineIdx id) {  // higher is most evictable
        return array[id];
    }

    inline void changePrio(const LineIdx id, int64_t newPrio) { array[id] = newPrio; }
};

class BRRIPReplPolicy : public SRRIPReplPolicy {
public:
    BRRIPReplPolicy(const LineIdx _numLines, uint32_t M)
        : SRRIPReplPolicy(_numLines, M) {}

    void update(const LineIdx id, const MemReq* req) override {
        (void)req;
        qassert(id.isValid());

        if (score(id) == clean) {
            if (rnd.randInt(32) == 0) {
                changePrio(id, vmax - 1);
            } else {
                changePrio(id, vmax);
            }
        } else {
            changePrio(id, 0);  // predict near-immediate re-reference
        }
    }
};

typedef SetSamplingReplPolicy<SRRIPReplPolicy, BRRIPReplPolicy> DRRIPReplPolicy;

}  // namespace sim
}  // namespace platy
