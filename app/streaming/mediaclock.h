#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>

// Maps the host's capture clock onto the client's audible playback clock. The
// epochs are deliberately unrelated. Video timestamps are unwrapped near audio,
// so 32-bit RTP wrap does not introduce a discontinuity during long sessions.
class MediaClock
{
public:
    static MediaClock& get() { static MediaClock clock; return clock; }

    void reset(bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Enabled = enabled;
        m_Host = m_Local = m_Updated = 0;
    }

    bool enabled() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Enabled;
    }

    void update(uint64_t hostUs, uint64_t playbackLocalUs, uint64_t nowUs)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Enabled || !hostUs || hostUs > UINT64_MAX / 90 || !playbackLocalUs) return;
        if (m_Host && hostUs <= m_Host) return;
        if (m_Host && nowUs >= m_Updated && nowUs - m_Updated < 1000000) {
            const auto predicted = int64_t(m_Local) + int64_t(hostUs - m_Host);
            const auto error = int64_t(playbackLocalUs) - predicted;
            if (std::abs(error) < 100000) {
                // Correct jitter gradually while following audio-device clock drift.
                playbackLocalUs = predicted + std::clamp(error / 8, int64_t(-2000), int64_t(2000));
            }
        }
        m_Host = hostUs;
        m_Local = playbackLocalUs;
        m_Updated = nowUs;
    }

    std::optional<int64_t> videoDelay(uint32_t video90k, uint64_t nowUs) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Enabled || !m_Host || nowUs < m_Updated || nowUs - m_Updated > 1000000) return {};
        const auto audio90k = uint32_t((m_Host * 90 + 500) / 1000);
        const auto difference = int32_t(video90k - audio90k);
        if (difference < -90000 || difference > 90000) return {};
        const auto delay = int64_t(m_Local) + int64_t(difference) * 1000 / 90 - int64_t(nowUs);
        // A failed device, clock discontinuity, or extreme output latency must not freeze video.
        if (delay > 150000 || delay < -1000000) return {};
        return delay;
    }

private:
    mutable std::mutex m_Mutex;
    bool m_Enabled = false;
    uint64_t m_Host = 0, m_Local = 0, m_Updated = 0;
};
