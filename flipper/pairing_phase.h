/* Connection/ceremony phase shown by the UI, plus the transition rules the main-thread event
   loop applies to it. Pure header (no Furi) so tests/flipper can exercise the exact rules.

   Why the rules exist (found 2026-10-07, "home menu appears for ~1 s, then only Sync"): the
   Home rows other than Sync require PairingPhaseSessionActive. Phase changes reach the main
   loop through one queue from three producers that are NOT ordered against each other:
     - BleEventWorker posts Authenticating / SessionActive (and Done is posted by the main
       thread itself AFTER its blocking SD save of a fresh pairing);
     - the "Bt" service thread posts BtStatusConnected/Advertising, delivered asynchronously
       (bt.c queues BtMessageTypeUpdateStatus and the callback runs whenever that thread gets
       to it, reading bt->status at that moment).
   A late BtStatusConnected, or a Done queued behind a faster session, used to overwrite an
   already-active session's phase with Exchanging/Done: the rows vanished and nothing ever
   moved the phase back to SessionActive. Both are now ignored once the ceremony/session is
   further along than the event claims. */
#ifndef FEB_PAIRING_PHASE_H
#define FEB_PAIRING_PHASE_H

#include <stdbool.h>

typedef enum {
    PairingPhaseNone,
    PairingPhaseWaiting,
    PairingPhaseExchanging,
    PairingPhaseConfirming,
    PairingPhaseSaving,
    /* Runtime session establishment (docs/PLAN.md step 6) reuses this same connection-
       phase enum rather than a parallel one -- a connecting peer is either running the
       fresh-pairing ceremony (phases above) or the runtime hello/hello_ack/client_auth
       flow (phases below); the two are mutually exclusive per connection. */
    PairingPhaseAuthenticating,
    PairingPhaseSessionActive,
    PairingPhaseDone,
    PairingPhaseFailed,
} PairingPhase;

/* True while a pairing ceremony or runtime session is mid-flight on the current connection. */
static inline bool pairing_phase_is_live(PairingPhase phase) {
    return phase == PairingPhaseConfirming || phase == PairingPhaseSaving ||
           phase == PairingPhaseAuthenticating || phase == PairingPhaseSessionActive;
}

/* A BtStatusConnected event is only news while the phase is not already past "connected". */
static inline bool pairing_phase_accepts_bt_connected(PairingPhase current) {
    return !pairing_phase_is_live(current);
}

/* New phase after an AppEventPairingPhase(incoming). `Done` (fresh pairing saved) must never
   pull an already-authenticating/active runtime session back out of it. */
static inline PairingPhase pairing_phase_after_event(PairingPhase current, PairingPhase incoming) {
    if(incoming == PairingPhaseDone &&
       (current == PairingPhaseAuthenticating || current == PairingPhaseSessionActive)) {
        return current;
    }
    return incoming;
}

#endif
