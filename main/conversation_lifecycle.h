#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "device_state.h"

// Command metadata is owned by the application task. Network callbacks only read
// the atomic admission token to discard speech queued before an end/new session.
class ConversationLifecycle {
public:
    enum class EndResult { Ended, AlreadyEnded, StaleSession, InvalidState };

    EndResult End(const std::string& request_id, const std::string& expected_session,
                  const std::string& current_session, DeviceState state) {
        if (expected_session.empty() || expected_session != current_session) {
            return EndResult::StaleSession;
        }
        if (state != kDeviceStateIdle && state != kDeviceStateConnecting &&
            state != kDeviceStateListening && state != kDeviceStateSpeaking) {
            return EndResult::InvalidState;
        }
        if (ended_.load() && session_id_ == current_session) {
            return EndResult::AlreadyEnded;
        }
        if (!last_end_request_id_.empty() && request_id == last_end_request_id_) {
            return EndResult::StaleSession;
        }
        session_id_ = current_session;
        last_end_request_id_ = request_id;
        ended_.store(true);
        generation_.fetch_add(1);
        return EndResult::Ended;
    }

    void Begin() {
        generation_.fetch_add(1);
        ended_.store(false);
    }

    bool Ended() const { return ended_.load(); }
    uint32_t Generation() const { return generation_.load(); }
    bool Accepts(uint32_t generation) const {
        return !ended_.load() && generation == generation_.load();
    }
    const std::string& LastEndRequestId() const { return last_end_request_id_; }

private:
    std::atomic<bool> ended_{false};
    std::atomic<uint32_t> generation_{0};
    std::string session_id_;
    std::string last_end_request_id_;
};
