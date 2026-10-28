#include <cassert>

#include "conversation_lifecycle.h"

int main() {
    using Result = ConversationLifecycle::EndResult;
    ConversationLifecycle session;
    const auto old_speech = session.Generation();
    assert(session.Accepts(old_speech));
    assert(session.End("end-1", "old-session", "new-session", kDeviceStateListening) ==
           Result::StaleSession);
    assert(session.Accepts(old_speech));
    for (auto state : {kDeviceStateStarting, kDeviceStateWifiConfiguring, kDeviceStateUpgrading,
                       kDeviceStateActivating, kDeviceStateAudioTesting, kDeviceStateFatalError}) {
        assert(session.End("end-1", "current", "current", state) == Result::InvalidState);
        assert(session.Accepts(old_speech));
    }
    assert(session.End("end-1", "current", "current", kDeviceStateSpeaking) == Result::Ended);
    assert(session.Ended());
    assert(!session.Accepts(old_speech));
    assert(!session.Accepts(session.Generation()));
    const auto ended_generation = session.Generation();
    assert(session.End("end-1", "current", "current", kDeviceStateIdle) == Result::AlreadyEnded);
    assert(session.Generation() == ended_generation);
    assert(session.LastEndRequestId() == "end-1");

    // A new local wake/transport accepts new speech, never an old queued callback.
    session.Begin();
    assert(!session.Ended());
    assert(session.Accepts(session.Generation()));
    assert(!session.Accepts(old_speech));
    assert(!session.Accepts(ended_generation));
    assert(session.End("end-1", "current", "current", kDeviceStateListening) ==
           Result::StaleSession);
    assert(!session.Ended());
    assert(session.End("end-1", "current", "next", kDeviceStateListening) == Result::StaleSession);
    assert(!session.Ended());
    assert(session.End("end-2", "next", "next", kDeviceStateListening) == Result::Ended);
    assert(session.LastEndRequestId() == "end-2");
}
