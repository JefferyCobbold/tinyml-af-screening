#include "af_session.h"

#include <string.h>

void af_session_reset(struct af_session *s)
{
	memset(s, 0, sizeof(*s));
	s->state = AF_STATE_WAITING;
}

enum af_event af_session_push(struct af_session *s, int32_t raw_ir,
			      struct af_window_result *result)
{
	const bool on = raw_ir > AF_CONTACT_LEVEL;

	switch (s->state) {
	case AF_STATE_WAITING:
		s->on_run = on ? s->on_run + 1 : 0;
		if (s->on_run == AF_CONTACT_CONFIRM) {
			/* The first "on" sample was AF_CONTACT_CONFIRM samples ago */
			s->since_first_on = AF_CONTACT_CONFIRM;
			s->off_run = 0;
			s->state = AF_STATE_SETTLING;
			return AF_EVENT_FINGER_DETECTED;
		}
		return AF_EVENT_NONE;

	case AF_STATE_SETTLING:
		s->off_run = on ? 0 : s->off_run + 1;
		if (s->off_run >= AF_LIFT_SAMPLES) {
			af_session_reset(s);
			return AF_EVENT_FINGER_LIFTED;
		}
		/* since_first_on counts samples including the first "on" one, so the
		 * sample at offset AF_SETTLE_SAMPLES is number AF_SETTLE_SAMPLES + 1 */
		if (++s->since_first_on <= AF_SETTLE_SAMPLES) {
			return AF_EVENT_NONE;
		}
		/* This sample is the first one analysed, as in the notebook */
		af_pipeline_reset(&s->pipeline);
		af_pipeline_push(&s->pipeline, raw_ir, result);
		s->state = AF_STATE_MEASURING;
		return AF_EVENT_MEASURING;

	case AF_STATE_MEASURING:
		s->off_run = on ? 0 : s->off_run + 1;
		if (s->off_run >= AF_LIFT_SAMPLES) {
			af_session_reset(s);
			return AF_EVENT_FINGER_LIFTED;
		}
		return af_pipeline_push(&s->pipeline, raw_ir, result) ? AF_EVENT_WINDOW_DONE
								      : AF_EVENT_NONE;
	}
	return AF_EVENT_NONE;
}
