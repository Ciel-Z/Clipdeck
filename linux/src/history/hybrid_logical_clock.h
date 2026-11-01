#pragma once

#include <QMutex>
#include <QtGlobal>

#include <functional>

// Hybrid logical clock (docs/lan-sync-design.md §3): orders history changes
// across devices whose system clocks disagree. A value packs milliseconds since
// the epoch into the high 48 bits and a counter into the low 16, so values
// compare as plain integers and still read as wall-clock time.
//
// Every local change takes now(); a value received from another device goes
// through receive(), which moves this clock past it. Values only ever grow,
// even when the system clock steps backwards.
class HybridLogicalClock final {
public:
    static constexpr int kCounterBits = 16;
    static constexpr quint64 kCounterMask = (quint64(1) << kCounterBits) - 1;

    explicit HybridLogicalClock(std::function<qint64()> wallClockMs = {});

    quint64 now();
    quint64 receive(quint64 remote);
    // Makes later values greater than one already stored (database start-up).
    void observe(quint64 value);
    quint64 last() const;

    static quint64 make(qint64 milliseconds, quint32 counter = 0);
    static qint64 milliseconds(quint64 value) { return qint64(value >> kCounterBits); }
    static quint32 counter(quint64 value) { return quint32(value & kCounterMask); }

    // The process-wide clock: the Deck and the capture pipeline each open their
    // own database connection, and both must draw from one sequence.
    static HybridLogicalClock &shared();

private:
    quint64 advance(qint64 wall, quint64 floor);

    std::function<qint64()> wallClockMs_;
    mutable QMutex mutex_;
    quint64 last_ = 0;
};
