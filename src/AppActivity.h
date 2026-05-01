#pragma once

namespace libera_timecode {

// Tell the OS this process must run with low-latency, real-time-friendly
// scheduling regardless of focus state. On macOS this disables App Nap and
// timer coalescing so backgrounded windows don't cause output thread jitter.
// Safe to call multiple times; subsequent calls are no-ops. The activity
// token lives until the process exits.
void disableAppNap();

} // namespace libera_timecode
