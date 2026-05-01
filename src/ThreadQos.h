#pragma once

namespace libera_timecode {

// Elevate the calling thread's scheduling priority so timecode output
// threads aren't preempted by background work. macOS uses QoS classes;
// other platforms get a best-effort equivalent. Safe to call once at the
// top of a thread function — failures are silent.
void setHighPriorityForOutputThread();

} // namespace libera_timecode
