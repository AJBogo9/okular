/*
    SPDX-FileCopyrightText: 2026 Andreas Bogossian <andreas.bogossian9@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_SCROLLLOD_H
#define OKULAR_SCROLLLOD_H

#include <cmath>

/**
 * Scroll level of detail.
 *
 * Rasterisation sustains about 4.6 pages/s, while a free-spin flick traverses 40
 * to 60 pages/s, so during fast motion the renderer loses the race and the view
 * shows blank pages. Rendering at a fraction of the target size is about 4x
 * cheaper, which trades sharpness the reader cannot resolve while moving for
 * pages that actually appear.
 *
 * This header is deliberately free of Qt so the policy can be tested on the host
 * without building the application.
 */
namespace ScrollLod
{

struct Policy {
    /** Above this scroll speed, reduced resolution is used. */
    double engageAbovePxPerSec = 2000.0;
    /** Below this scroll speed, full resolution returns. Hysteresis: keep the two apart. */
    double releaseBelowPxPerSec = 1200.0;
    /** Samples further apart than this cannot be trusted as velocity. */
    long long maxSampleGapMs = 250;
    /** A single step larger than this many viewport heights is navigation, not motion. */
    double maxJumpViewportHeights = 2.0;
};

/**
 * Turns a stream of scroll positions into an "is the view moving fast" decision.
 */
class Tracker
{
public:
    Tracker() = default;

    void setPolicy(const Policy &policy);
    const Policy &policy() const;

    /**
     * Feeds one scroll position, in viewport pixels, with a monotonic timestamp.
     * Returns whether reduced resolution should be used from now on.
     */
    bool sample(double x, double y, long long timeMs, double viewportHeight);

    /** Drops the reference point. Use when the coordinate system changes. */
    void reset();

    bool engaged() const;
    double velocity() const;

private:
    void releaseAt(double x, double y, long long timeMs);

    Policy m_policy;
    bool m_engaged = false;
    bool m_haveReference = false;
    double m_velocity = 0.0;
    double m_lastX = 0.0;
    double m_lastY = 0.0;
    long long m_lastTimeMs = 0;
};

/**
 * The pixmap size to ask for while moving. Deterministic for a given input, so a
 * page that already holds a reduced pixmap is recognised instead of re-requested.
 */
inline int reducedSize(int fullSize, int factor)
{
    if (factor <= 1) {
        return fullSize;
    }
    const int reduced = fullSize / factor;
    return reduced > 1 ? reduced : 1;
}

inline void Tracker::setPolicy(const Policy &policy)
{
    m_policy = policy;
}

inline const Policy &Tracker::policy() const
{
    return m_policy;
}

inline bool Tracker::sample(double x, double y, long long timeMs, double viewportHeight)
{
    if (!m_haveReference) {
        m_haveReference = true;
        m_lastX = x;
        m_lastY = y;
        m_lastTimeMs = timeMs;
        return m_engaged;
    }

    const long long elapsedMs = timeMs - m_lastTimeMs;
    if (elapsedMs <= 0) {
        // Same millisecond, or a clock that went backwards: no information either way.
        return m_engaged;
    }

    const double dx = x - m_lastX;
    const double dy = y - m_lastY;
    const double distance = std::sqrt(dx * dx + dy * dy);

    if (distance == 0.0) {
        // Several call sites fire for one scroll event, so a repeated position is
        // routine and must not read as "stopped". Only silence means stopped.
        if (elapsedMs > m_policy.maxSampleGapMs) {
            releaseAt(x, y, timeMs);
        }
        return m_engaged;
    }

    const double jumpLimit = m_policy.maxJumpViewportHeights * viewportHeight;
    const bool stale = elapsedMs > m_policy.maxSampleGapMs;
    const bool teleport = jumpLimit > 0.0 && distance > jumpLimit;
    if (stale || teleport) {
        // Not motion: an idle view waking up, or navigation such as a jump to a
        // bookmark. Either way the old reference point says nothing about speed.
        releaseAt(x, y, timeMs);
        return m_engaged;
    }

    m_velocity = distance * 1000.0 / static_cast<double>(elapsedMs);
    m_engaged = m_engaged ? m_velocity > m_policy.releaseBelowPxPerSec : m_velocity > m_policy.engageAbovePxPerSec;

    m_lastX = x;
    m_lastY = y;
    m_lastTimeMs = timeMs;
    return m_engaged;
}

inline void Tracker::reset()
{
    m_engaged = false;
    m_haveReference = false;
    m_velocity = 0.0;
}

inline void Tracker::releaseAt(double x, double y, long long timeMs)
{
    m_engaged = false;
    m_velocity = 0.0;
    m_haveReference = true;
    m_lastX = x;
    m_lastY = y;
    m_lastTimeMs = timeMs;
}

inline bool Tracker::engaged() const
{
    return m_engaged;
}

inline double Tracker::velocity() const
{
    return m_velocity;
}

}

#endif
