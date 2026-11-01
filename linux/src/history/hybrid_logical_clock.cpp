#include "hybrid_logical_clock.h"

#include <QDateTime>
#include <QMutexLocker>

#include <algorithm>

HybridLogicalClock::HybridLogicalClock(std::function<qint64()> wallClockMs)
    : wallClockMs_(wallClockMs ? std::move(wallClockMs) : [] { return QDateTime::currentMSecsSinceEpoch(); }) {}

quint64 HybridLogicalClock::make(qint64 milliseconds, quint32 counter) {
    return (quint64(std::max<qint64>(0, milliseconds)) << kCounterBits) | (counter & kCounterMask);
}

// The next value: the wall clock if it is ahead of everything seen so far,
// otherwise one tick past the larger of the last value and `floor`.
quint64 HybridLogicalClock::advance(qint64 wall, quint64 floor) {
    const quint64 seen = std::max(last_, floor);
    const quint64 fromWall = make(wall);
    last_ = fromWall > seen ? fromWall : seen + 1; // a full counter carries into the milliseconds
    return last_;
}

quint64 HybridLogicalClock::now() {
    const qint64 wall = wallClockMs_();
    QMutexLocker locker(&mutex_);
    return advance(wall, 0);
}

quint64 HybridLogicalClock::receive(quint64 remote) {
    const qint64 wall = wallClockMs_();
    QMutexLocker locker(&mutex_);
    return advance(wall, remote);
}

void HybridLogicalClock::observe(quint64 value) {
    QMutexLocker locker(&mutex_);
    last_ = std::max(last_, value);
}

quint64 HybridLogicalClock::last() const {
    QMutexLocker locker(&mutex_);
    return last_;
}

HybridLogicalClock &HybridLogicalClock::shared() {
    static HybridLogicalClock clock;
    return clock;
}
