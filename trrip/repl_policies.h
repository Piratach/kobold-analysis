
#pragma once

#include <functional>
#include <tuple>

#include "bithacks.h"
#include "cache_arrays.h"
#include "coherence_ctrls.h"
#include "memory_hierarchy.h"
#include "ms_shared.h"
#include "mtrand.h"

// #define DEBUG_REPL_POLICY
#ifdef DEBUG_REPL_POLICY
#define DBG_REPL(...) qlog(__VA_ARGS__)
#else
#define DBG_REPL(...)
#endif

namespace platy {
namespace sim {

class CC;

/* Generic replacement policy interface. A replacement policy is initialized by the cache
 * (by calling setTop/BottomCC) and used by the cache array. Usage follows two models:
 * - On lookups, update() is called if the replacement policy is to be updated on a hit
 * - On each replacement, rank() is called with the req and a list of replacement
 *   candidates.
 * - When the replacement is done, replaced() is called. (See below for more detail.)
 */
class ReplPolicy {
protected:
    CC* cc = nullptr;

public:
    virtual void setCC(CC* _cc) { cc = _cc; }

    virtual void update(LineIdx id, const MemReq* req) = 0;
    virtual void replaced(LineIdx id) = 0;

    virtual LineIdx rankCands(const MemReq* req, SetAssocCands cands) = 0;
    virtual LineIdx rankCands(const MemReq* req, ArrayCands cands) = 0;
};

typedef std::function<ReplPolicy*(uint32_t)> ReplPolicyFactory;

/* Add DECL_RANK_BINDINGS to each class that implements the new interface,
 * then implement a single, templated rank() function (see below for examples)
 * This way, we achieve a simple, single interface that is specialized transparently to
 * each type of array (this code is performance-critical)
 */
#define DECL_RANK_BINDING(T) \
    LineIdx rankCands(const MemReq* req, T cands) override { return rank(req, cands); }
#define DECL_RANK_BINDINGS()          \
    DECL_RANK_BINDING(SetAssocCands); \
    DECL_RANK_BINDING(ArrayCands);

/* Plain ol' LRU, though this one is sharers-aware, prioritizing lines that have
 * sharers down in the hierarchy vs lines not shared by anyone.
 */
template <bool sharersAware>
class LRUReplPolicy : public ReplPolicy {
protected:
    uint64_t timestamp;  // incremented on each access
    StrongVec<LineIdx, uint64_t> array;
    LineIdx numLines;

public:
    explicit LRUReplPolicy(LineIdx _numLines) : timestamp(1), numLines(_numLines) {
        array.resize(numLines, 0);
    }

    void update(LineIdx id, const MemReq* req) override {
        (void)req;
        qassert(id.isValid());
        array[id] = timestamp++;
    }

    void replaced(LineIdx id) override {
        qassert(id.isValid());
        array[id] = 0;
    }

    template <typename C>
    inline LineIdx rank(const MemReq* req, C cands) {
        (void)req;
        LineIdx bestCand{LineIdx::INVALID};
        uint64_t bestScore = (uint64_t)-1;
        for (auto ci = cands.begin(); ci != cands.end(); ci++) {
            qassert((*ci).isValid());

            uint32_t s = score(*ci);
            bestCand = (s < bestScore) ? *ci : bestCand;
            bestScore = MIN(s, bestScore);
        }
        return bestCand;
    }

    DECL_RANK_BINDINGS();

private:
    inline uint64_t score(LineIdx id) {  // higher is least evictable
        // array[id] < timestamp always, so this prioritizes by:
        // (1) valid (if not valid, it's 0)
        // (2) sharers, and
        // (3) timestamp

        if (cc == nullptr) {
            return array[id];
        }

        auto isvalid = cc->isValid(id);
        auto nsharers = cc->numSharers(id);
        return (sharersAware ? nsharers : 0) * timestamp + array[id] * isvalid;
    }
};

}  // namespace sim
}  // namespace platy
