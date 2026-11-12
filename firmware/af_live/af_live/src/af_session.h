/*
 * Measurement session: decides when the finger is on the sensor and feeds
 * the AF pipeline only while it is.
 *
 * Same rule as the notebook:
 *   - finger "on" when IR > 50,000 counts (no finger reads ~8,000)
 *   - contact confirmed after 1 s continuously on
 *   - analysis starts 3 s after the first "on" sample (pressure settling)
 *   - finger "lifted" after 0.5 s continuously off; the session then restarts
 *
 * Plain C, no Zephyr dependencies, so it can be tested on a PC.
 */
#ifndef AF_SESSION_H_
#define AF_SESSION_H_

#include "af_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AF_CONTACT_LEVEL   50000
#define AF_CONTACT_CONFIRM AF_FS              /* 1 s on before contact counts */
#define AF_SETTLE_SAMPLES  (3 * AF_FS)        /* analysis starts 3 s after first "on" */
#define AF_LIFT_SAMPLES    (AF_FS / 2)        /* 0.5 s off = finger lifted */

enum af_state {
	AF_STATE_WAITING,     /* no finger */
	AF_STATE_SETTLING,    /* finger detected, waiting for it to settle */
	AF_STATE_MEASURING,   /* feeding the pipeline */
};

enum af_event {
	AF_EVENT_NONE,
	AF_EVENT_FINGER_DETECTED,
	AF_EVENT_MEASURING,
	AF_EVENT_FINGER_LIFTED,
	AF_EVENT_WINDOW_DONE,   /* *result holds a completed 30 s window */
};

struct af_session {
	enum af_state state;
	int on_run;           /* consecutive samples above the contact level */
	int off_run;          /* consecutive samples below it */
	int since_first_on;   /* samples since the first "on" sample of this contact */
	struct af_pipeline pipeline;
};

void af_session_reset(struct af_session *s);

/* Feed one raw IR sample; returns what happened. */
enum af_event af_session_push(struct af_session *s, int32_t raw_ir,
			      struct af_window_result *result);

#ifdef __cplusplus
}
#endif

#endif /* AF_SESSION_H_ */
