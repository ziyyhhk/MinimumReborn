#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Rolling frame-time statistics. Main thread only.
//
// Two numbers are stored per drawn frame:
//   frameMs - wall time since the previous drawn frame (what the player sees)
//   workMs  - CPU time spent inside the mod's CCDirector::drawScene hook, i.e. the scene
//             traversal and the GL submission for that frame. This is a CPU-side number.
//             It is NOT GPU time; the mod deliberately has no GPU timer because a blocking
//             query would itself cost frames.
//
// The ring buffer is fixed size, so memory use is bounded for the whole session. The
// expensive parts (sorting for percentiles) only run when the overlay refreshes, four
// times a second, never per frame.

namespace reborn {

    class FrameStats {
    public:
        static constexpr std::size_t kCapacity = 240;

        void push(double frameMs, double workMs) {
            m_frame[m_next] = frameMs > 0.0 ? frameMs : 0.0;
            m_work[m_next] = workMs > 0.0 ? workMs : 0.0;
            m_next = (m_next + 1) % kCapacity;
            if (m_count < kCapacity) ++m_count;
        }

        void clear() {
            m_count = 0;
            m_next = 0;
        }

        std::size_t size() const { return m_count; }

        double averageFrameMs(std::size_t n) const {
            if (m_count == 0) return 0.0;
            n = std::min(n, m_count);
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) sum += at(m_frame, i);
            return sum / static_cast<double>(n);
        }

        double averageWorkMs(std::size_t n) const {
            if (m_count == 0) return 0.0;
            n = std::min(n, m_count);
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) sum += at(m_work, i);
            return sum / static_cast<double>(n);
        }

        // Frame-time variability over the newest n frames (standard deviation, ms).
        double jitterMs(std::size_t n) const {
            if (m_count < 2) return 0.0;
            n = std::min(n, m_count);
            double const mean = averageFrameMs(n);
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                double const d = at(m_frame, i) - mean;
                sum += d * d;
            }
            return std::sqrt(sum / static_cast<double>(n));
        }

        // 1% low as an FPS number: the average of the slowest 1% of frame times, converted.
        // Uses at least one sample; "--" is the caller's job when there is no data yet.
        double onePercentLowFps(std::size_t n) const {
            if (m_count == 0) return 0.0;
            n = std::min(n, m_count);
            double sorted[kCapacity];
            for (std::size_t i = 0; i < n; ++i) sorted[i] = at(m_frame, i);
            std::sort(sorted, sorted + n);  // ascending: slowest at the end
            std::size_t const worst = std::max<std::size_t>(1, n / 100);
            double sum = 0.0;
            for (std::size_t i = n - worst; i < n; ++i) sum += sorted[i];
            double const avgWorst = sum / static_cast<double>(worst);
            return avgWorst > 0.0 ? 1000.0 / avgWorst : 0.0;
        }

        // 95th percentile frame time in ms (the "slow frames" the average hides).
        double percentileFrameMs(std::size_t n, double percentile) const {
            if (m_count == 0) return 0.0;
            n = std::min(n, m_count);
            double sorted[kCapacity];
            for (std::size_t i = 0; i < n; ++i) sorted[i] = at(m_frame, i);
            std::sort(sorted, sorted + n);
            double const p = std::clamp(percentile, 0.0, 1.0);
            std::size_t const index = static_cast<std::size_t>(p * static_cast<double>(n - 1));
            return sorted[index];
        }

    private:
        // The newest sample is at m_next - 1; walk backwards so `n` means "newest n".
        double at(double const* ring, std::size_t offsetFromNewest) const {
            std::size_t const index = (m_next + kCapacity - 1 - offsetFromNewest) % kCapacity;
            return ring[index];
        }

        double m_frame[kCapacity]{};
        double m_work[kCapacity]{};
        std::size_t m_count = 0;
        std::size_t m_next = 0;
    };

    // The one rolling window used by the frame hook, the overlay and the popup.
    FrameStats& frameStats();
}
