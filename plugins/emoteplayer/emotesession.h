#pragma once

namespace emoteplayer {
// Call before the TJS VM and renderer are destroyed. Safe to repeat and safe
// when Emote was never loaded during a session.
void ResetEmotePlayerSession();
}
