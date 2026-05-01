#include "ThreadQos.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace libera_timecode {

#ifdef __APPLE__
namespace {
// Convert nanoseconds to mach_absolute_time ticks for the local CPU.
uint32_t nsToMachTicks(uint64_t ns) {
    static mach_timebase_info_data_t tb = []() {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info;
    }();
    return static_cast<uint32_t>(ns * tb.denom / tb.numer);
}
} // namespace
#endif

void setHighPriorityForOutputThread() {
#ifdef __APPLE__
    // Give the thread a high QoS even if the realtime policy below is denied.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // THREAD_TIME_CONSTRAINT_POLICY is the realtime scheduling class macOS
    // uses for audio threads. It promises us at most `computation` ticks of
    // CPU within every `period` ticks, with a hard `constraint` deadline.
    // Even under heavy system load, these threads are not preempted by
    // ordinary work. (App Nap / QoS throttling do not apply.)
    //
    // Our outputs poll at ~2ms with sub-ms work per iteration, so we ask
    // for 500us of compute every 2ms with a 1.5ms constraint.
    thread_time_constraint_policy_data_t policy{};
    policy.period      = nsToMachTicks(2'000'000);   // 2 ms
    policy.computation = nsToMachTicks(500'000);     // 500 us
    policy.constraint  = nsToMachTicks(1'500'000);   // 1.5 ms
    policy.preemptible = TRUE;

    const thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    const kern_return_t rv = thread_policy_set(mach_thread,
                                               THREAD_TIME_CONSTRAINT_POLICY,
                                               reinterpret_cast<thread_policy_t>(&policy),
                                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (rv != KERN_SUCCESS) {
        thread_precedence_policy_data_t precedence{};
        precedence.importance = 63;
        thread_policy_set(mach_thread,
                          THREAD_PRECEDENCE_POLICY,
                          reinterpret_cast<thread_policy_t>(&precedence),
                          THREAD_PRECEDENCE_POLICY_COUNT);
    }
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#else
    sched_param p{};
    p.sched_priority = 1;
    pthread_setschedparam(pthread_self(), SCHED_RR, &p);
#endif
}

} // namespace libera_timecode
