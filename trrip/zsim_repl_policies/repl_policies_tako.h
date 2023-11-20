
#pragma once

#include "repl_policies.h"
#include "system_tako.h"

namespace platy {
namespace sim {
namespace ms {
namespace tako {

/* These replacement policies additionally prevent certain deadlocks among callbacks in
 * Tako.
 *
 * The high-level idea is that
 * - callbacks should not be allowed to trigger another callback at the same or a lower
 *   cache location, and
 * - each cache set must always contain at least "callbackFreeWays" entries which are not
 *   registered for callbacks at any cache location
 *
 * They also support pinning lines in the cache. See TakoCache and OOO core for
 * more details.
 *
 * See the Tako doc for more details.
 */

// TODO (bcs): For each repl policy, probably best to extract out a function to get the
// victim given the valid candidates since there are so many conditions for determining
// the valid candidates, resulting in lots of duplicated code.

class TakoBaseReplPolicy : public ReplPolicy {
public:
    void pinLine(const LineIdx lineIdx) {
        ++pinnedLines[lineIdx];  // Inserts zero-initialized new entry if doesn't exist.
        DBG_REPL("line {}, incrementing pin count to {}", lineIdx, pinnedLines[lineIdx]);
    }
    void unpinLine(const LineIdx lineIdx) {
        auto it = pinnedLines.find(lineIdx);
        qassert(it != pinnedLines.end());

        // TODO (bcs): Check that the line is valid? Think about how we want to deal with
        // invalidations.

        --(it->second);
        DBG_REPL("line {}, decrementing pin count to {}", lineIdx, pinnedLines[lineIdx]);

        if (it->second == 0) {
            pinnedLines.erase(it);
        }
    }

protected:
    const uint32_t callbackFreeWays;  // Minimum # of entries in each set which must not
                                      // be registered for callbacks.

    explicit TakoBaseReplPolicy(
        const TileIdx _tile,
        const ms_shared::tako_shared::Location _location,
        const uint32_t _callbackFreeWays)
        : callbackFreeWays(_callbackFreeWays), tile(_tile), location(_location) {
        qassert(callbackFreeWays > 0);
    }

    bool isRegistered(const LineAddr lineAddr) const {
        // Returns true if the address is registered at any cache location for OnWriteback
        // or OnCleanEviction.
        return ms::tako::takoSystem
                   ->shouldInvokeCallback(
                       lineAddr, ms_shared::tako_shared::Location::PRIVATE_CACHE,
                       CallbackType::ON_WRITEBACK)
                   .shouldInvoke
               || ms::tako::takoSystem
                      ->shouldInvokeCallback(
                          lineAddr, ms_shared::tako_shared::Location::PRIVATE_CACHE,
                          CallbackType::ON_CLEAN_EVICTION)
                      .shouldInvoke
               || ms::tako::takoSystem
                      ->shouldInvokeCallback(
                          lineAddr, ms_shared::tako_shared::Location::SHARED_CACHE,
                          CallbackType::ON_WRITEBACK)
                      .shouldInvoke
               || ms::tako::takoSystem
                      ->shouldInvokeCallback(
                          lineAddr, ms_shared::tako_shared::Location::SHARED_CACHE,
                          CallbackType::ON_CLEAN_EVICTION)
                      .shouldInvoke;
    }

    bool isCallbackDataBufferFull() const {
        return ms::tako::takoSystem->isCallbackDataBufferFull(tile, location);
    }

    bool isWritebackBufferFullOfRegistered() const {
        return ms::tako::takoSystem->isWritebackBufferFullOfRegistered(tile, location);
    }

    bool isPinned(const LineIdx lineIdx) const {
        return pinnedLines.find(lineIdx) != pinnedLines.end();
    }

private:
    const TileIdx tile;
    const ms_shared::tako_shared::Location location;

    // We use a count instead of bool because multiple instructions might want a line to
    // be pinned at the same time. Only when the count reaches zero can the line be
    // unpinned.
    UMap<LineIdx, uint32_t> pinnedLines;
};

template <bool sharersAware>
class TakoLRUReplPolicy : public TakoBaseReplPolicy {
protected:
    struct Entry {
        uint64_t timestamp = 0;
        bool isRegistered = false;  // Is registered at any cache location
    };

    uint64_t timestamp;  // incremented on each access
    StrongVec<LineIdx, Entry> array;
    const LineIdx numLines;

public:
    explicit TakoLRUReplPolicy(
        const TileIdx _tile,
        const ms_shared::tako_shared::Location _location,
        const LineIdx _numLines,
        const uint32_t _callbackFreeWays)
        : TakoBaseReplPolicy(_tile, _location, _callbackFreeWays),
          timestamp(1),
          numLines(_numLines) {
        array.resize(numLines, Entry());
    }

    void update(const LineIdx id, const MemReq* req) override {
        qassert(id.isValid());
        // Assigning "isRegistered" on insertion instead of checking on each replacement
        // means that if a morph is unregistered then the entry here will still be
        // indicated as isRegistered. While possibily less efficient for total
        // performance, it would still be deadlock-free and we won't likely have
        // unregistering on the critical path of an application anyways. And a real
        // implementation would probably use a bit with the cache tag like this too.
        array[id] = {timestamp++, isRegistered(req->lineAddr)};
    }

    void replaced(const LineIdx id) override {
        qassert(id.isValid());
        qassert(!isPinned(id));
        array[id] = Entry();
    }

    template <typename C>
    inline LineIdx rank(const MemReq* req, const C cands) {
        const bool canTriggerCallback = req->is(MemReq::CAN_TRIGGER_CALLBACK);
        const bool writebackBufferFull = isWritebackBufferFullOfRegistered();

        // If the access can trigger a callback (i.e., originated from a main core), but
        // the line being inserted is not registered anywhere, then we only need to worry
        // about the writeback buffer. The constraint is that if the writeback buffer is
        // full of registered lines, then we cannot evict another registered line.
        // Otherwise, this access will keep holding an MSHR while waiting for a callback
        // to finish. If all MSHRs are held by such accesses, then there remain no MSHRs
        // for callbacks.
        if (canTriggerCallback && !isRegistered(req->lineAddr)) {
            LineIdx bestCand{LineIdx::INVALID};
            uint64_t bestScore = (uint64_t)-1;

            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (isPinned(*ci)) {
                    continue;
                }

                if (writebackBufferFull && array[*ci].isRegistered) {
                    continue;
                }

                const uint32_t s = score(*ci);
                bestCand = (s < bestScore) ? *ci : bestCand;
                bestScore = MIN(s, bestScore);
            }
            return bestCand;
        }

        // If the access can trigger a callback (i.e., originated from a main core), and
        // the line being inserted is registered, then our deadlock-free constraint is
        // that after this replacement, at least "callbackFreeWays" entries which are not
        // registered at any cache location must remain in the set.
        if (canTriggerCallback) {
            // First iterate over the candidates to determine the number of unregistered
            // entries.
            uint32_t unregisteredEntries = 0;

            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (!array[*ci].isRegistered || !cc->isValid(*ci)) {
                    qassert(!isPinned(*ci));  // Can only pin valid, registered lines.
                    ++unregisteredEntries;
                }
            }

            // We cannot assert here that unregisteredEntries >= callbackFreeWays, even
            // though it should technically be true, because one or more of the
            // unregistered candidates could be currently locked, and thus wouldn't be
            // added to the count. Since we can't know the reason for the entry being
            // locked (e.g., maybe it's separately being evicted), we just count it as
            // registered. So it's OK for unregisteredEntries < callbackFreeWays, and if
            // so we still require evicting a registered entry. It would never be
            // unallowable for an access from a main core to evict a registered entry.
            const bool mustEvictRegisteredEntry = unregisteredEntries <= callbackFreeWays;

            // Now iterate over the candidates to find the victim.
            LineIdx bestCand{LineIdx::INVALID};
            uint64_t bestScore = (uint64_t)-1;

            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (isPinned(*ci)) {
                    continue;
                }

                // If we must evict a registered entry, then we cannot evict either
                // invalid or valid-and-unregistered entries.
                if (mustEvictRegisteredEntry
                    && (!cc->isValid(*ci) || !array[*ci].isRegistered)) {
                    continue;
                }

                // The candidate is a possible victim, so obtain the LRU score.
                const uint32_t s = score(*ci);
                bestCand = (s < bestScore) ? *ci : bestCand;
                bestScore = MIN(s, bestScore);
            }
            return bestCand;
        }

        // Since this access originated from an engine, whether a candidate is an
        // allowable victim is dependent on a deadlock-free check.
        const bool accessFromLocalEngine = req->is(MemReq::FROM_LOCAL_ENGINE);
        const bool callbackDataBufferFull = isCallbackDataBufferFull();

        LineIdx bestCand{LineIdx::INVALID};
        uint64_t bestScore = (uint64_t)-1;

        for (auto ci = cands.begin(); ci != cands.end(); ci++) {
            qassert((*ci).isValid());

            if (isPinned(*ci)) {
                continue;
            }

            // If the candidate is registered anywhere, then we need additional checks for
            // whether evicting the candidate could potentially cause deadlock. See the
            // Tako doc for more details.
            const bool candidateRegistered = array[*ci].isRegistered;
            const bool candidateValid = cc->isValid(*ci);
            if (candidateRegistered && candidateValid) {
                const bool candidateHasSharers = cc->numSharers(*ci) > 0;
                if ((accessFromLocalEngine  // TODO (bcs): Why check local engine here?
                     && (callbackDataBufferFull || writebackBufferFull))
                    || (!accessFromLocalEngine && candidateHasSharers)) {
                    continue;
                }
            }

            // The candidate is a possible victim, so obtain the LRU score.
            const uint32_t s = score(*ci);
            bestCand = (s < bestScore) ? *ci : bestCand;
            bestScore = MIN(s, bestScore);
        }
        return bestCand;
    }

    DECL_RANK_BINDINGS();

private:
    // Highest score is least evictable.
    inline uint64_t score(const LineIdx id) {
        // array[id] < timestamp always, so this prioritizes by:
        // (1) valid (if not valid, it's 0)
        // (2) sharers, and
        // (3) timestamp

        if (cc == nullptr) {
            return array[id].timestamp;
        }

        auto isvalid = cc->isValid(id);
        auto nsharers = cc->numSharers(id);
        return (sharersAware ? nsharers : 0) * timestamp + array[id].timestamp * isvalid;
    }
};

// Notes:
// - No sharers-aware option currently
class TakoSRRIPReplPolicy : public TakoBaseReplPolicy {
protected:
    // higher is most evictable.
    static const int64_t clean = std::numeric_limits<int64_t>::max();

    struct Entry {
        int64_t age = clean;
        bool isRegistered = false;  // Is registered at any cache location
    };

    StrongVec<LineIdx, Entry> array;
    const LineIdx numLines;
    const int64_t vmax;
    MTRand rnd;

public:
    explicit TakoSRRIPReplPolicy(
        const TileIdx _tile,
        const ms_shared::tako_shared::Location _location,
        const LineIdx _numLines,
        const uint32_t M,
        const uint32_t _callbackFreeWays)
        : TakoBaseReplPolicy(_tile, _location, _callbackFreeWays),
          numLines(_numLines),
          vmax((1 << M) - 1),
          rnd(4242) {
        qassert(vmax > 0);  // otherwise this is useless...
        array.resize(numLines, Entry());
    }

    void update(const LineIdx id, const MemReq* req) override {
        qassert(id.isValid());

        if (score(id) == clean) {
            // INSERT: predict long re-reference
            array[id] = {/*age=*/vmax - 1, isRegistered(req->lineAddr)};
        } else {
            // HIT: predict near-immediate re-reference
            array[id] = {/*age=*/0, isRegistered(req->lineAddr)};
        }
    }

    void replaced(const LineIdx id) override {
        qassert(id.isValid());
        qassert(!isPinned(id));
        array[id] = Entry();
    }

    template <typename C>
    inline LineIdx rank(const MemReq* req, C cands) {
        LineIdx bestCands[cands.size().get()];
        int64_t bestPrio = -1;
        uint32_t pos = 0;

        const bool canTriggerCallback = req->is(MemReq::CAN_TRIGGER_CALLBACK);
        const bool writebackBufferFull = isWritebackBufferFullOfRegistered();

        // If the access can trigger a callback (i.e., originated from a main core), but
        // the line being inserted is not registered anywhere, then we only need to worry
        // about the writeback buffer. The constraint is that if the writeback buffer is
        // full of registered lines, then we cannot evict another registered line.
        // Otherwise, this access will keep holding an MSHR while waiting for a callback
        // to finish. If all MSHRs are held by such accesses, then there remain no MSHRs
        // for callbacks.
        if (canTriggerCallback && !isRegistered(req->lineAddr)) {
            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (isPinned(*ci)) {
                    continue;
                }

                if (writebackBufferFull && array[*ci].isRegistered) {
                    continue;
                }

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
        }

        // If the access can trigger a callback (i.e., originated from a main core), and
        // the line being inserted is registered, then our deadlock-free constraint is
        // that after this replacement, at least "callbackFreeWays" entries which are not
        // registered at any cache location must remain in the set.
        else if (canTriggerCallback) {
            // First iterate over the candidates to determine the number of unregistered
            // entries.
            uint32_t unregisteredEntries = 0;

            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (!array[*ci].isRegistered || !cc->isValid(*ci)) {
                    qassert(!isPinned(*ci));  // Can only pin valid, registered lines.
                    ++unregisteredEntries;
                }
            }

            // We cannot assert here that unregisteredEntries >= callbackFreeWays, even
            // though it should technically be true, because one or more of the
            // unregistered candidates could be currently locked, and thus wouldn't be
            // added to the count. Since we can't know the reason for the entry being
            // locked (e.g., maybe it's separately being evicted), we just count it as
            // registered. So it's OK for unregisteredEntries < callbackFreeWays, and if
            // so we still require evicting a registered entry. It would never be
            // unallowable for an access from a main core to evict a registered entry.
            const bool mustEvictRegisteredEntry = unregisteredEntries <= callbackFreeWays;

            // Now iterate over the candidates to find the victim.
            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (isPinned(*ci)) {
                    continue;
                }

                // If we must evict a registered entry, then we cannot evict either
                // invalid or valid-and-unregistered entries.
                if (mustEvictRegisteredEntry
                    && (!cc->isValid(*ci) || !array[*ci].isRegistered)) {
                    continue;
                }

                // The candidate is a possible victim, so obtain the SRRIP score.
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
        }

        // Since this access originated from an engine, whether a candidate is an
        // allowable victim is dependent on a deadlock-free check.
        else {
            const bool accessFromLocalEngine = req->is(MemReq::FROM_LOCAL_ENGINE);
            const bool callbackDataBufferFull = isCallbackDataBufferFull();

            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                qassert((*ci).isValid());

                if (isPinned(*ci)) {
                    continue;
                }

                // If the candidate is registered anywhere, then we need additional checks
                // for whether evicting the candidate could potentially cause deadlock.
                // See the Tako doc for more details.
                const bool candidateRegistered = array[*ci].isRegistered;
                const bool candidateValid = cc->isValid(*ci);
                if (candidateRegistered && candidateValid) {
                    const bool candidateHasSharers = cc->numSharers(*ci) > 0;
                    if ((accessFromLocalEngine  // TODO (bcs): Why check local engine
                                                // here?
                         && (callbackDataBufferFull || writebackBufferFull))
                        || (!accessFromLocalEngine && candidateHasSharers)) {
                        continue;
                    }
                }

                // The candidate is a possible victim, so obtain the SRRIP score.
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
        }

        // If there's no valid candidate currently, return invalid.
        if (bestPrio == -1) {
            return LineIdx::INVALID;
        }

        // Age cands if needed. Need to recalculate bestPrio because, depending on the
        // valid candidates, bestPrio may not include all candidates.
        bestPrio = -1;
        for (auto ci = cands.begin(); ci != cands.end(); ci++) {
            qassert((*ci).isValid());
            bestPrio = MAX(bestPrio, score(*ci));
        }

        if (bestPrio < vmax) {
            const uint32_t aging = vmax - bestPrio;
            for (auto ci = cands.begin(); ci != cands.end(); ci++) {
                array[*ci].age += aging;
            }
        }

        return bestCands[rnd.randInt(pos)];  // break ties randomly
    }

    DECL_RANK_BINDINGS();

protected:
    inline int64_t score(const LineIdx id) {  // higher is most evictable
        if (!cc->isValid(id)) {
            return clean;
        }

        return array[id].age;
    }
};

// Similar to TakoSRRIPReplPolicy. The difference is the eviction priorty assigned to
// entries when inserted. If the access is from a callback, it is given higher initial
// eviction priority (i.e., SRRIP insertion) to encourage early eviction for low-reuse
// data from callbacks. Accesses from the main core are given lower eviction priority
// (i.e., MRU insertion) so that expectedly more-important data (e.g., phantom data) will
// stick around longer.
//
// Notes:
// - No sharers-aware option currently
//
// TODO (bcs): Better name?
class TakoPseudoSRRIPReplPolicy : public TakoSRRIPReplPolicy {
public:
    explicit TakoPseudoSRRIPReplPolicy(
        const TileIdx _tile,
        const ms_shared::tako_shared::Location _location,
        const LineIdx _numLines,
        const uint32_t M,
        const uint32_t _callbackFreeWays)
        : TakoSRRIPReplPolicy(_tile, _location, _numLines, M, _callbackFreeWays) {}

    void update(const LineIdx id, const MemReq* req) override {
        qassert(id.isValid());

        if (score(id) == clean) {
            // INSERT
            if (!req->is(MemReq::CAN_TRIGGER_CALLBACK)) {
                // Access is from a callback. Give it higher eviction priority.
                array[id] = {/*age=*/vmax - 1, isRegistered(req->lineAddr)};
            } else {
                // Access is from a main core. Give it lower eviction priority.
                array[id] = {/*age=*/0, isRegistered(req->lineAddr)};
            }
        } else {
            // HIT: predict near-immediate re-reference
            array[id] = {/*age=*/0, isRegistered(req->lineAddr)};
        }
    }
};

}  // namespace tako
}  // namespace ms
}  // namespace sim
}  // namespace platy
