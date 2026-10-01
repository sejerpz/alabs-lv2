/*
 * ExpSense (aLabs) — reads a passive expression pedal through audio I/O.
 *
 * Stereo: in_l/in_r -> volume -> out_l/out_r. The reference is out_r (Out 2),
 * which feeds both the right amp and one end of the pot (Ring);
 * the wiper (Tip) returns on an audio input (sense), Sleeve to ground.
 *
 * The divider gain g = sense / out is obtained by fusing two estimators:
 *  - program: least squares on the outgoing signal (20 Hz..2 kHz band), with
 *    compensation of the round-trip latency measured during calibration;
 *  - tone: phase-insensitive I/Q lock-in on a low-level ~19 kHz pilot tone,
 *    enabled only when the program is not enough (hybrid mode).
 * The estimate is mapped to 0..1 (min/max calibration, curve, invert) and is
 * output as CV, control port, MIDI CC (7 or 14 bit) and, optionally, as a
 * volume applied directly to the outgoing audio.
 *
 * A single guided calibration (heel -> toe -> heel) checks the pedal wiring,
 * measures the loop latency at toe and learns the pedal range and direction.
 *
 * Performance notes (target: MOD Dwarf, Cortex-A35, in-order, 64-bit NEON):
 *  - the per-sample loop keeps all its state in locals (registers): nothing is
 *    read back from the instance struct between samples;
 *  - the two identical filter chains (output / sense) and the two lock-in
 *    demodulators (sense / delayed output) run as 2-lane vectors (NEON d-regs);
 *  - the oscillator is single precision, renormalised once per sub-block;
 *  - exp() for the log volume law runs twice per sub-block (ends of a linear
 *    ramp) instead of once per sample; log10() once per run();
 *  - the two delay lines are interleaved (one cache line serves both reads);
 *  - the latency-search correlations accumulate in float, in vectorisable
 *    contiguous loops (the ring buffer of decimated samples is mirrored).
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#define ES_URI "urn:alabs:expsense"

/* small SIMD vectors (GCC/clang extension). v2f is a pair of floats: a NEON
 * d-register on aarch64; elsewhere it is padded to 16 bytes so that it maps to a
 * native SSE register instead of being lowered to scalar code (upper lanes unused). */
#if defined(__aarch64__)
typedef float v2f __attribute__((vector_size(8)));
#else
typedef float v2f __attribute__((vector_size(16)));
#endif
typedef float v4f __attribute__((vector_size(16), may_alias));
typedef float v4fu __attribute__((vector_size(16), aligned(4), may_alias)); /* unaligned load */

typedef enum {
	P_IN_L = 0,
	P_IN_R,
	P_OUT_L,
	P_OUT_R,
	P_SENSE,
	P_CV,
	P_MIDI_OUT,
	P_MODE,
	P_TONE_FREQ,
	P_TONE_LEVEL,
	P_SMOOTH,
	P_CURVE,
	P_INVERT,
	P_VOL_ON,
	P_VOL_LAW,
	P_CALIBRATE,
	P_MIDI_CH,
	P_MIDI_CC,
	P_MIDI_14BIT,
	P_VALUE,
	P_RAW,
	P_TONE_ACTIVE,
	P_STATUS,
	P_LATENCY,
	P_CAL_LO,
	P_CAL_HI,
	P_REF_DB,
	P_VOL_GAIN,
	P_RESULT,
	P_COUNT
} PortIndex;

enum { MODE_HYBRID = 0, MODE_TONE = 1, MODE_PROGRAM = 2 };

enum { VOL_LOG = 0, VOL_LINEAR = 1 };

/* log volume law: g = (10^(3v) - 1) / (10^3 - 1), ~60 dB range, exactly 0 at v = 0 */
#define VOL_LOG_K 6.9077553f /* ln(1000) */
#define VOL_LOG_NORM (1.f / 999.f)

/* status (output port) */
enum {
	ST_UNCAL = 0,   /* never calibrated: tone only, default range */
	ST_RUN,
	ST_HOLD,        /* no reference or pedal unplugged: value held */
	ST_CAL_HEEL,    /* calibration: keep the pedal heel-down */
	ST_CAL_TO_TOE,  /* calibration: move the pedal to toe-down and hold it */
	ST_CAL_AT_TOE,  /* calibration: hold at toe (latency measurement) */
	ST_CAL_TO_HEEL  /* calibration: bring the pedal back to heel-down */
};

/* result of the last calibration (output port) */
enum {
	RES_NONE = 0,
	RES_RUNNING,
	RES_OK,
	RES_OK_LOW,     /* calibrated, but the In 2 level is very low: raise its gain */
	RES_SWAP,       /* returned level barely changes: Tip/Ring swapped (or pedal not moved) */
	RES_NO_SIGNAL,  /* nothing comes back: wiper on sleeve, mono cable, active pedal, unplugged */
	RES_CLIP,       /* In 2 is clipping: lower its gain */
	RES_NO_LATENCY, /* the loop latency could not be measured */
	RES_ABORTED,    /* button pressed again */
	RES_NOT_HELD,   /* the pedal reached toe but was not held there long enough */
	RES_OK_WIRING   /* calibrated, but the low end stays high: probably Tip/Ring swapped */
};

/* latency measurement sub-state */
enum { LAT_IDLE = 0, LAT_COARSE, LAT_FINE, LAT_DONE };

#define DL_SIZE 4096u
#define DL_MASK (DL_SIZE - 1u)
#define DEC 4u
#define N_COARSE 512u /* decimated lags: cover DEC*N_COARSE = 2048 samples */
#define FINE_HALF 8
#define N_FINE (2 * FINE_HALF + 1)
#define SUB 32u /* sub-block: power of two */

#define COARSE_SEC 1.5
#define FINE_SEC 0.75
#define TONE_HOLD_SEC 0.15
#define MIDI_MIN_INTERVAL_SEC 0.002

#define NOISE_DB (-40.f)   /* excitation during calibration */
#define TONE_W 10.f        /* relative weight of the tone estimator */
#define THR_ON 1e-6f       /* program power below which the tone is needed (-60 dB) */
#define THR_OFF 6.3e-6f    /* above which the tone may switch off (-52 dB) */
#define W_MIN 1e-9f        /* minimum total weight to update the estimate */

#define CAL_TONE_DB (-30.f)          /* pilot tone level during calibration */
#define CAL_TAU_SEC 0.05             /* energy integration: much longer than the loop latency */
#define CAL_GATE_RATIO 1.25f         /* output lock-in above this x the emitted tone: program transient */
#define CAL_GATE_SEC 0.04            /* freeze the integration this long after a transient */
#define CAL_SETTLE_SEC 0.25          /* ignore the start (filters settling) */
#define CAL_HEEL_SEC 0.7             /* the pedal is read heel-down until here */
#define CAL_MOVE_TIMEOUT_SEC 8.0     /* waiting for the pedal to reach toe */
#define CAL_MOVED 0.35f              /* |r - heel| / max above this: pedal away from heel */
#define CAL_STILL_TOL 0.05f          /* "still": moves less than this fraction of |r - heel| */
#define CAL_STILL_SEC 0.4            /* held still this long: the pedal is at toe */
#define CAL_TOE_MOVED 0.1f           /* leaving toe by this fraction of the excursion ... */
#define CAL_TOE_LEAVE_SEC 0.25       /* ... for this long restarts the toe phase (ignores transients) */
#define CAL_LAT_TIMEOUT_SEC 8.0      /* latency measurement (with retries) gives up after this */
#define CAL_LEARN_SEC 0.5            /* tone correction learning at toe, after the latency */
#define CAL_BACK_TOL 0.15f           /* back at heel: within this fraction of the excursion */
#define CAL_BACK_SEC 0.3             /* ... for this long */
#define CAL_BACK_TIMEOUT_SEC 8.0     /* not coming back is not an error */
#define CAL_NO_SIGNAL 0.003f         /* ratio below this: nothing comes back (-50 dB) */
#define CAL_LOW 0.05f                /* max ratio below this: In 2 level too low (-26 dB) */
/* With Tip/Ring swapped, Out 2 drives the wiper and In 2 reads a pot end through part
 * of the pot: r = Zin / (Zin + Rx). On a low-impedance input (the Dwarf's In 2 loads a
 * typical pot by about R/Zin = 2) this still moves enough to calibrate, but its low end
 * stays at 1/(1 + R/Zin) of the top (~30%), while correct wiring goes close to zero. */
#define CAL_WIRING_RATIO 0.2f        /* low end / high end above this: check the wiring */
#define CAL_CLIP 0.98f               /* sense peak at or above this: In 2 clipping */
#define TONE_K_RATE 0.002f           /* tone correction learning rate, per sub-block */
#define TONE_K_RATE_CAL 0.02f        /* faster while calibrating at toe */

typedef struct {
	float b0, b1, b2, a1, a2, z1, z2;
} Biquad;

typedef struct {
	/* ports */
	const float* in_l;
	const float* in_r;
	float* out_l;
	float* out_r;
	const float* sense;
	float* cv;
	LV2_Atom_Sequence* midi_out;
	const float* c[P_COUNT];
	float* o[P_COUNT];

	double fs;
	LV2_URID_Map* map;
	LV2_Atom_Forge forge;
	struct {
		LV2_URID midi_Event, atom_Float, k_latency, k_lo, k_hi, k_tone_k;
	} uris;

	/* parameter-derived coefficients, recomputed only when the parameter changes */
	float p_smooth, a_sm, dec_sub; /* dec_sub = (1 - a_sm)^SUB */
	float p_tone_level, tone_amp;

	/* quadrature oscillator: ph = {cos, sin}, rotated by rot_c / rot_s = {-sin w, sin w} */
	v2f ph, rot_s;
	float rot_c, cur_freq;
	float tone_env, a_tone_env;
	int tone_hold;
	bool tone_want;

	/* 20 Hz high-pass + 2 kHz low-pass, lane 0: output, lane 1: sense */
	v2f hp_x1, hp_y1, bq_z1, bq_z2;
	float hp_a, b0, b1, b2, a1, a2;
	Biquad lp_noise;

	/* lock-in: 3-pole one-pole chains, lane 0: sense, lane 1: delayed output */
	float a_t;
	v2f zre[3], zim[3];
	float tone_k; /* loop response correction at 19 kHz vs low band */

	/* least squares on the program */
	float a_p, np, dp, ep;

	/* estimate */
	float raw, raw_sm, a_raw, value_tgt, value_sm, vol_g, a_vol, wsum;
	bool have_raw, holding;

	/* calibration */
	int cal_phase;              /* ST_CAL_* while calibrating, ST_UNCAL when idle */
	int lat_phase;              /* LAT_* */
	int result;                 /* RES_* of the last calibration */
	bool cal_prev;
	uint32_t cal_count, phase_t0, lat_count, lat_total, still, learn, back, leave;
	bool reached_toe;
	uint32_t coarse_len, fine_len;
	uint32_t xdec_w, dec_phase;
	int fine_base;
	float a_cal, c_es, c_ex;    /* sense/output tone energies (latency-independent ratio) */
	uint32_t gate;              /* samples left with the integration frozen */
	float heel_acc, heel, toe_acc, toe, toe_ref, still_ref, r_max, speak, cal_amp;
	uint32_t heel_n, toe_n;
	/* calibration in use before the current one started (restored on failure/abort) */
	float bk_latency, bk_tone_k;
	bool bk_lat_valid;
	uint32_t rng;
	float noise_amp;

	/* active calibration: lo = raw at heel, hi = raw at toe (hi < lo when the pedal reads high at heel) */
	float latency, lat_frac;
	uint32_t lat_int;
	bool lat_valid;
	float lo, hi;

	/* state restore (non-RT thread), applied in run() */
	int restore_pending;
	float r_latency, r_lo, r_hi, r_tone_k;

	/* MIDI */
	float midi_last;
	int midi_last_ch, midi_last_cc, midi_last_14;
	uint32_t midi_timer, midi_min_interval;

	/* latency search. cc/cf are stored in reversed lag order (index m = last - k) so
	 * that each update is a contiguous multiply-accumulate; xdec is mirrored */
	double cc_ex, cc_es, cf_ex, cf_es;
	float cf[N_FINE];
	float xdec[2 * N_COARSE];
	v4f cc[N_COARSE / 4];

	/* delay line of the emitted signal: {full band, 20 Hz..2 kHz} */
	uint32_t w;
	v2f dl[DL_SIZE];
} ExpSense;

/* ------------------------------------------------------------------------- */

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float db2lin(float db)
{
	return powf(10.f, db / 20.f);
}

static inline float onepole_coef(double fs, double tau_sec)
{
	return (float)(1.0 - exp(-1.0 / (tau_sec * fs)));
}

static void bq_lowpass(Biquad* f, double fs, double fc, double q)
{
	const double w0 = 2.0 * M_PI * fc / fs;
	const double cw = cos(w0), sw = sin(w0);
	const double alpha = sw / (2.0 * q);
	const double a0 = 1.0 + alpha;
	f->b0 = (float)((1.0 - cw) * 0.5 / a0);
	f->b1 = (float)((1.0 - cw) / a0);
	f->b2 = f->b0;
	f->a1 = (float)(-2.0 * cw / a0);
	f->a2 = (float)((1.0 - alpha) / a0);
	f->z1 = f->z2 = 0.f;
}

static inline float bq_run(Biquad* f, float x)
{
	const float y = f->b0 * x + f->z1;
	f->z1 = f->b1 * x - f->a1 * y + f->z2;
	f->z2 = f->b2 * x - f->a2 * y;
	return y;
}

static inline float white(uint32_t* s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return (float)((int32_t)x) * (1.f / 2147483648.f);
}

static inline float vol_log(float v)
{
	return (expf(VOL_LOG_K * v) - 1.f) * VOL_LOG_NORM;
}

static void set_freq(ExpSense* s, float f)
{
	const double w = 2.0 * M_PI * f / s->fs;
	const float sw = (float)sin(w);
	s->rot_c = (float)cos(w);
	s->rot_s = (v2f){ -sw, sw };
	s->cur_freq = f;
}

static void set_latency(ExpSense* s, float lat)
{
	lat = clampf(lat, 0.f, (float)(DL_SIZE - 4));
	s->latency = lat;
	s->lat_int = (uint32_t)floorf(lat);
	s->lat_frac = lat - (float)s->lat_int;
}

static void reset_program_est(ExpSense* s)
{
	s->np = s->dp = s->ep = 0.f;
}

static inline bool calibrating(const ExpSense* s)
{
	return s->cal_phase != ST_UNCAL;
}

/* ---- latency measurement (runs while the pedal is held at toe) ---- */

static void lat_start(ExpSense* s)
{
	s->lat_phase = LAT_COARSE;
	s->lat_count = 0;
	memset(s->xdec, 0, sizeof(s->xdec));
	memset(s->cc, 0, sizeof(s->cc));
	s->cc_ex = s->cc_es = 0.0;
	s->xdec_w = 0;
	s->dec_phase = 0;
}

/* coarse correlation update: cc[m] += x[m] * sl over N_COARSE contiguous lags */
static inline void coarse_acc(v4f* restrict cc, const float* restrict x, float sl)
{
	const v4f slv = { sl, sl, sl, sl };
	for (uint32_t m = 0; m < N_COARSE / 4; ++m) {
		cc[m] += *(const v4fu*)(x + 4 * m) * slv;
	}
}

/* evaluate the correlations at the end of each search; called once per sub-block */
static void lat_step(ExpSense* s, uint32_t ns)
{
	s->lat_count += ns;
	if (s->lat_phase == LAT_COARSE && s->lat_count >= s->coarse_len) {
		const float* cc = (const float*)s->cc;
		uint32_t best = 0;
		float bv = 0.f;
		for (uint32_t m = 0; m < N_COARSE; ++m) {
			const float v = fabsf(cc[m]);
			if (v > bv) {
				bv = v;
				best = m;
			}
		}
		const double rho = (double)bv * bv / (s->cc_ex * s->cc_es + 1e-30);
		if (rho < 0.2) {
			lat_start(s); /* weak correlation: retry */
			return;
		}
		const uint32_t lag = N_COARSE - 1u - best; /* reversed storage */
		s->fine_base = (int)(lag * DEC) - FINE_HALF;
		if (s->fine_base < 0) {
			s->fine_base = 0;
		}
		memset(s->cf, 0, sizeof(s->cf));
		s->cf_ex = s->cf_es = 0.0;
		s->lat_count = 0;
		s->lat_phase = LAT_FINE;
	} else if (s->lat_phase == LAT_FINE && s->lat_count >= s->fine_len) {
		int best = 0;
		float bv = 0.f;
		for (int m = 0; m < N_FINE; ++m) {
			const float v = fabsf(s->cf[m]);
			if (v > bv) {
				bv = v;
				best = m;
			}
		}
		const double rho = (double)bv * bv / (s->cf_ex * s->cf_es + 1e-30);
		if (rho < 0.2) {
			lat_start(s);
			return;
		}
		/* cf[m] holds lag fine_base + (N_FINE - 1 - m): the previous lag is m + 1 */
		double delta = 0.0;
		if (best > 0 && best < N_FINE - 1) {
			const double ym = fabsf(s->cf[best + 1]), y0 = bv, yp = fabsf(s->cf[best - 1]);
			const double den = ym - 2.0 * y0 + yp;
			if (den < 0.0) {
				delta = clampf((float)(0.5 * (ym - yp) / den), -0.5f, 0.5f);
			}
		}
		set_latency(s, (float)(s->fine_base + (N_FINE - 1 - best) + delta));
		s->lat_valid = true;
		reset_program_est(s);
		s->lat_phase = LAT_DONE;
	}
}

/* ---- guided calibration: heel -> toe (latency) -> heel ---- */

static void cal_set_phase(ExpSense* s, int phase)
{
	s->cal_phase = phase;
	s->phase_t0 = s->cal_count;
	s->still = 0;
	s->back = 0;
	s->leave = 0;
}

static void cal_begin(ExpSense* s)
{
	s->bk_latency = s->latency;
	s->bk_lat_valid = s->lat_valid;
	s->bk_tone_k = s->tone_k;
	s->cal_count = 0;
	s->lat_phase = LAT_IDLE;
	s->lat_total = 0;
	s->c_es = s->c_ex = 0.f;
	s->gate = 0;
	s->heel_acc = 0.f;
	s->heel_n = 0;
	s->heel = s->toe = 0.f;
	s->r_max = 0.f;
	s->speak = 0.f;
	s->reached_toe = false;
	s->result = RES_RUNNING;
	cal_set_phase(s, ST_CAL_HEEL);
}

/* end the calibration: commit on success, otherwise restore the previous one */
static void cal_end(ExpSense* s, int result)
{
	if (result == RES_OK || result == RES_OK_LOW || result == RES_OK_WIRING) {
		/* heel and toe were measured as uncorrected tone ratios: bring them to the
		 * raw scale of the fused estimator, then keep a 1% dead zone at both ends */
		const float heel = s->tone_k * s->heel;
		const float toe = s->tone_k * s->toe;
		const float span = toe - heel; /* negative when the pedal reads high at heel */
		s->lo = heel + 0.01f * span;
		s->hi = toe - 0.01f * span;
	} else {
		set_latency(s, s->bk_latency);
		s->lat_valid = s->bk_lat_valid;
		s->tone_k = s->bk_tone_k;
		reset_program_est(s);
	}
	s->result = result;
	s->lat_phase = LAT_IDLE;
	s->cal_phase = ST_UNCAL;
}

/*
 * Called once per sub-block while calibrating, with the lock-in magnitudes of
 * sense (ms) and of the output (mx), and the magnitude the emitted tone alone
 * would give (mtone). The latency may not be known yet, so ms and mx are not
 * time-aligned: transients (pick attacks, which carry energy near the tone
 * frequency) would make their ratio swing. Two measures keep the ratio r robust
 * without knowing the latency:
 *  - both energies are integrated over a window much longer than the latency;
 *  - the integration is frozen while the output carries more than the tone
 *    (a program transient), and for a short while after, so that the delayed
 *    copy of the transient on sense is skipped too.
 */
static void cal_step(ExpSense* s, uint32_t ns, float ms, float mx, float mtone)
{
	s->cal_count += ns;
	const double t = (double)s->cal_count / s->fs;
	const double tp = (double)(s->cal_count - s->phase_t0) / s->fs;
	if (mx > CAL_GATE_RATIO * mtone) {
		s->gate = (uint32_t)(CAL_GATE_SEC * s->fs);
	} else if (s->gate > ns) {
		s->gate -= ns;
	} else {
		s->gate = 0;
	}
	if (s->gate == 0) {
		s->c_es += s->a_cal * (ms * ms - s->c_es);
		s->c_ex += s->a_cal * (mx * mx - s->c_ex);
	}
	if (s->c_ex < 1e-14f || t < CAL_SETTLE_SEC) {
		return;
	}
	const float r = sqrtf(s->c_es / s->c_ex);
	if (r > s->r_max) {
		s->r_max = r;
	}

	switch (s->cal_phase) {
	case ST_CAL_HEEL:
		s->heel_acc += r;
		++s->heel_n;
		if (t >= CAL_HEEL_SEC) {
			s->heel = s->heel_acc / (float)s->heel_n;
			s->still_ref = r;
			cal_set_phase(s, ST_CAL_TO_TOE);
		}
		break;

	case ST_CAL_TO_TOE: {
		const float ref = fmaxf(s->r_max, s->heel);
		const float away = fabsf(r - s->heel);
		if (ref > CAL_NO_SIGNAL && away > CAL_MOVED * ref) {
			if (fabsf(r - s->still_ref) > CAL_STILL_TOL * away) {
				s->still_ref = r;
				s->still = 0;
			} else {
				s->still += ns;
			}
			if ((double)s->still >= CAL_STILL_SEC * s->fs) {
				/* held at toe: measure the latency there, where the return is strongest */
				s->toe_acc = 0.f;
				s->toe_n = 0;
				s->toe_ref = r;
				s->learn = 0;
				s->reached_toe = true;
				lat_start(s);
				cal_set_phase(s, ST_CAL_AT_TOE);
			}
		} else {
			s->still_ref = r;
			s->still = 0;
		}
		if (s->cal_phase == ST_CAL_TO_TOE && tp >= CAL_MOVE_TIMEOUT_SEC) {
			if (s->reached_toe) {
				cal_end(s, RES_NOT_HELD); /* toe was reached, then left too early */
			} else if (s->r_max < CAL_NO_SIGNAL) {
				/* the reading never moved away from heel */
				cal_end(s, RES_NO_SIGNAL);
			} else if (s->speak >= CAL_CLIP) {
				cal_end(s, RES_CLIP);
			} else {
				cal_end(s, RES_SWAP);
			}
		}
		break;
	}

	case ST_CAL_AT_TOE:
		if (fabsf(r - s->toe_ref) > CAL_TOE_MOVED * fabsf(s->toe_ref - s->heel)) {
			s->leave += ns;
			if ((double)s->leave >= CAL_TOE_LEAVE_SEC * s->fs) {
				if (s->lat_phase == LAT_DONE) {
					/* latency already measured: the (shortened) learning is enough,
					 * keep the toe value collected so far */
					s->toe = s->toe_acc / (float)s->toe_n;
					cal_set_phase(s, ST_CAL_TO_HEEL);
				} else {
					/* the pedal left toe during the latency measurement: wait for it
					 * to be held again, then measure again */
					s->lat_phase = LAT_IDLE;
					s->still_ref = r;
					cal_set_phase(s, ST_CAL_TO_TOE);
				}
			}
			break; /* do not average a moving pedal into the toe value */
		}
		s->leave = 0;
		s->toe_acc += r;
		++s->toe_n;
		if (s->speak >= CAL_CLIP) {
			cal_end(s, RES_CLIP);
			break;
		}
		if (s->lat_phase != LAT_DONE) {
			s->lat_total += ns;
			if ((double)s->lat_total >= CAL_LAT_TIMEOUT_SEC * s->fs) {
				cal_end(s, RES_NO_LATENCY);
				break;
			}
			lat_step(s, ns);
		} else {
			s->learn += ns; /* tone correction is learned in run() meanwhile */
			if ((double)s->learn >= CAL_LEARN_SEC * s->fs) {
				s->toe = s->toe_acc / (float)s->toe_n;
				cal_set_phase(s, ST_CAL_TO_HEEL);
			}
		}
		break;

	case ST_CAL_TO_HEEL:
		if (fabsf(r - s->heel) < CAL_BACK_TOL * fabsf(s->toe - s->heel)) {
			s->back += ns;
		} else {
			s->back = 0;
		}
		if ((double)s->back >= CAL_BACK_SEC * s->fs || tp >= CAL_BACK_TIMEOUT_SEC) {
			if (s->speak >= CAL_CLIP) {
				cal_end(s, RES_CLIP);
			} else {
				const float top = fmaxf(s->heel, s->toe);
				const float bottom = fminf(s->heel, s->toe);
				if (bottom > CAL_WIRING_RATIO * top) {
					cal_end(s, RES_OK_WIRING);
				} else {
					cal_end(s, top < CAL_LOW ? RES_OK_LOW : RES_OK);
				}
			}
		}
		break;
	}
}

/* ------------------------------------------------------------------------- */

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* features)
{
	(void)d;
	(void)path;
	LV2_URID_Map* map = NULL;
	for (int i = 0; features && features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		}
	}
	if (!map) {
		return NULL;
	}

	ExpSense* s = (ExpSense*)calloc(1, sizeof(ExpSense));
	if (!s) {
		return NULL;
	}
	s->fs = rate;
	s->map = map;
	s->uris.midi_Event = map->map(map->handle, LV2_MIDI__MidiEvent);
	s->uris.atom_Float = map->map(map->handle, LV2_ATOM__Float);
	s->uris.k_latency = map->map(map->handle, ES_URI "#latency");
	s->uris.k_lo = map->map(map->handle, ES_URI "#cal_lo");
	s->uris.k_hi = map->map(map->handle, ES_URI "#cal_hi");
	s->uris.k_tone_k = map->map(map->handle, ES_URI "#tone_k");
	lv2_atom_forge_init(&s->forge, map);

	s->ph = (v2f){ 1.f, 0.f };
	set_freq(s, 19000.f);
	s->a_tone_env = onepole_coef(rate, 0.010);
	s->a_t = (float)(1.0 - exp(-2.0 * M_PI * 200.0 / rate));
	s->a_p = onepole_coef(rate, 0.003);
	s->a_raw = onepole_coef(rate / SUB, 0.010);
	s->a_vol = onepole_coef(rate, 0.002);
	s->p_smooth = -1.f;     /* forces the coefficient computation on the first run() */
	s->p_tone_level = 1.f;

	Biquad lp;
	bq_lowpass(&lp, rate, 2000.0, 0.7071);
	s->b0 = lp.b0;
	s->b1 = lp.b1;
	s->b2 = lp.b2;
	s->a1 = lp.a1;
	s->a2 = lp.a2;
	s->hp_a = (float)exp(-2.0 * M_PI * 20.0 / rate);
	bq_lowpass(&s->lp_noise, rate, 2000.0, 0.7071);

	s->coarse_len = (uint32_t)(COARSE_SEC * rate);
	s->fine_len = (uint32_t)(FINE_SEC * rate);
	s->midi_min_interval = (uint32_t)(MIDI_MIN_INTERVAL_SEC * rate);
	s->noise_amp = db2lin(NOISE_DB) * 1.7320508f; /* uniform: rms = amp/sqrt(3) */
	s->cal_amp = db2lin(CAL_TONE_DB);
	s->a_cal = onepole_coef(rate / SUB, CAL_TAU_SEC);
	s->rng = 0x12345678u;

	s->tone_k = 1.f;
	s->lo = 0.f;
	s->hi = 1.f;
	s->vol_g = 1.f;
	s->tone_env = 1.f;
	s->tone_want = true;
	s->cal_phase = ST_UNCAL;
	s->result = RES_NONE;
	s->midi_last = -1000.f;
	s->midi_last_ch = s->midi_last_cc = s->midi_last_14 = -1;
	return (LV2_Handle)s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	ExpSense* s = (ExpSense*)h;
	switch (port) {
	case P_IN_L: s->in_l = (const float*)data; break;
	case P_IN_R: s->in_r = (const float*)data; break;
	case P_OUT_L: s->out_l = (float*)data; break;
	case P_OUT_R: s->out_r = (float*)data; break;
	case P_SENSE: s->sense = (const float*)data; break;
	case P_CV: s->cv = (float*)data; break;
	case P_MIDI_OUT: s->midi_out = (LV2_Atom_Sequence*)data; break;
	default:
		/* control ports: inputs are read through c[], outputs written through o[] */
		if (port < P_COUNT) {
			s->c[port] = (const float*)data;
			s->o[port] = (float*)data;
		}
		break;
	}
}

static void activate(LV2_Handle h)
{
	ExpSense* s = (ExpSense*)h;
	memset(s->dl, 0, sizeof(s->dl));
	memset(s->zre, 0, sizeof(s->zre));
	memset(s->zim, 0, sizeof(s->zim));
	s->hp_x1 = s->hp_y1 = s->bq_z1 = s->bq_z2 = (v2f){ 0.f, 0.f };
	reset_program_est(s);
	s->w = 0;
}

static inline void midi_cc(ExpSense* s, uint32_t frame, int ch, int cc, int val)
{
	const uint8_t msg[3] = { (uint8_t)(0xB0 | (ch & 0x0F)), (uint8_t)(cc & 0x7F),
		                     (uint8_t)(val & 0x7F) };
	if (lv2_atom_forge_frame_time(&s->forge, frame)) {
		if (lv2_atom_forge_atom(&s->forge, 3, s->uris.midi_Event)) {
			lv2_atom_forge_write(&s->forge, msg, 3);
		}
	}
}

/* run()-invariant data of the per-sample loop */
typedef struct {
	const float* restrict in_l;
	const float* restrict in_r;
	const float* restrict sense;
	float* restrict out_l;
	float* restrict out_r;
	float* restrict cv;
	v2f hp_a, b0, b1, b2, a1, a2; /* 20 Hz high-pass, 2 kHz low-pass (both lanes) */
	v2f a_t;                      /* lock-in */
	v2f rot_s;
	float rot_c, a_sm, a_vol, a_tone_env, a_p, tamp, noise_amp;
	float kv, kl;  /* volume target: vt = kv*vt + kl*value_sm + dvt (branch-free) */
	int vol_mode;  /* 0 off, 1 linear, 2 log */
} Coef;

/*
 * One sub-block of the per-sample loop. lat_mode is a literal at each call site
 * (0: normal, 1: coarse latency search, 2: fine search) and the function is always
 * inlined, so the normal path carries neither the excitation noise nor the
 * correlation state. All state lives in locals (registers) for the whole sub-block.
 */
static inline __attribute__((always_inline)) void sub_run(ExpSense* restrict s, const Coef* restrict k,
                                                          uint32_t off, uint32_t end, const int lat_mode)
{
	const uint32_t ns = end - off;
	const bool coarse = lat_mode == 1, fine = lat_mode == 2, measuring = lat_mode != 0;
	const float te_tgt = s->tone_want ? 1.f : 0.f;
	const float lf = s->lat_frac;
	const uint32_t li = s->lat_int;
	const float value_tgt = s->value_tgt;
	v2f* restrict dl = s->dl;
	const float* restrict in_l = k->in_l;
	const float* restrict in_r = k->in_r;
	const float* restrict sense = k->sense;
	float* restrict out_l = k->out_l;
	float* restrict out_r = k->out_r;
	float* restrict cv = k->cv;
	const v2f hp_a = k->hp_a, b0 = k->b0, b1 = k->b1, b2 = k->b2, a1 = k->a1, a2 = k->a2;
	const v2f a_t = k->a_t, rot_s = k->rot_s;
	const float rot_c = k->rot_c, a_sm = k->a_sm, a_vol = k->a_vol, a_tone_env = k->a_tone_env;
	const float a_p = k->a_p, tamp = k->tamp, kv = k->kv, kl = k->kl;

	v2f ph = s->ph;
	float value_sm = s->value_sm, vol_g = s->vol_g, tone_env = s->tone_env;
	v2f hx1 = s->hp_x1, hy1 = s->hp_y1, bz1 = s->bq_z1, bz2 = s->bq_z2;
	v2f zr0 = s->zre[0], zr1 = s->zre[1], zr2 = s->zre[2];
	v2f zi0 = s->zim[0], zi1 = s->zim[1], zi2 = s->zim[2];
	float np = s->np, dp = s->dp, ep = s->ep;
	float speak = s->speak;
	uint32_t w = s->w;
	/* excitation noise and decimation, latency search only */
	Biquad lpn;
	uint32_t rng = 0, dec_phase = 0, xdec_w = 0;
	if (measuring) {
		lpn = s->lp_noise;
		rng = s->rng;
		dec_phase = s->dec_phase;
		xdec_w = s->xdec_w;
	}

	/* log volume law: the target is a linear ramp between its values at the two ends
	 * of the sub-block (value_sm follows a known exponential meanwhile), then smoothed
	 * per sample as before. Linear law: the target is value_sm itself. Off: 1. */
	float vt = 1.f, dvt = 0.f;
	if (k->vol_mode == 2) {
		const float dec = (ns == SUB) ? s->dec_sub : powf(1.f - a_sm, (float)ns);
		const float sm_end = value_tgt + (value_sm - value_tgt) * dec;
		vt = vol_log(value_sm);
		dvt = (vol_log(sm_end) - vt) / (float)ns;
	}

	for (uint32_t i = off; i < end; ++i) {
		/* oscillator: {c, s} -> {c*rc - s*rs, s*rc + c*rs} */
		const float oc = ph[0], os = ph[1];
		ph = ph * rot_c + (v2f){ os, oc } * rot_s;

		/* smoothed outputs */
		value_sm += a_sm * (value_tgt - value_sm);
		vt = kv * vt + (kl * value_sm + dvt);
		vol_g += a_vol * (vt - vol_g);
		tone_env += a_tone_env * (te_tgt - tone_env);

		float noise = 0.f;
		if (measuring) {
			noise = bq_run(&lpn, white(&rng) * k->noise_amp);
		}
		/* R (Out 2) feeds the pedal: tone and excitation on R only, L gets volume only */
		const float y = in_r[i] * vol_g + os * (tamp * tone_env) + noise;
		out_l[i] = in_l[i] * vol_g;
		out_r[i] = y;
		if (cv) {
			cv[i] = value_sm * 10.f;
		}

		/* 20 Hz..2 kHz band of {output, sense} */
		const float sv = sense[i];
		const v2f x2 = { y, sv };
		const v2f hy = hp_a * (hy1 + x2 - hx1);
		hx1 = x2;
		hy1 = hy;
		const v2f bl = b0 * hy + bz1;
		bz1 = b1 * hy - a1 * bl + bz2;
		bz2 = b2 * hy - a2 * bl;
		const float yl = bl[0], sl = bl[1];

		dl[w] = (v2f){ y, yl };
		speak = fmaxf(speak, fabsf(sv)); /* only read while calibrating */

		/* delayed output: full band (tone reference) and low band (fractional lag) */
		const v2f d0 = dl[(w - li) & DL_MASK];
		const float yd = d0[0];
		const float d1 = dl[(w - li - 1u) & DL_MASK][1];
		const float yld = d1 + lf * (d0[1] - d1);

		/* tone lock-in: same demodulation on {sense, delayed output} */
		const v2f sy = { sv, yd };
		const v2f xr = sy * oc, xi = sy * -os;
		zr0 += a_t * (xr - zr0);
		zr1 += a_t * (zr0 - zr1);
		zr2 += a_t * (zr1 - zr2);
		zi0 += a_t * (xi - zi0);
		zi1 += a_t * (zi0 - zi1);
		zi2 += a_t * (zi1 - zi2);

		/* least squares on the program */
		np += a_p * (yld * sl - np);
		dp += a_p * (yld * yld - dp);
		ep += a_p * (sl * sl - ep);

		/* correlations for the latency measurement */
		if (coarse) {
			if (++dec_phase == DEC) {
				dec_phase = 0;
				xdec_w = (xdec_w + 1u) & (N_COARSE - 1u);
				s->xdec[xdec_w] = s->xdec[xdec_w + N_COARSE] = yl;
				/* cc[m] <- lag N_COARSE-1-m, i.e. xdec[(xdec_w + 1 + m) mod N] */
				coarse_acc(s->cc, s->xdec + xdec_w + 1u, sl);
				s->cc_ex += (double)(yl * yl);
				s->cc_es += (double)(sl * sl);
			}
		} else if (fine) {
			/* cf[m] <- lag fine_base + N_FINE-1-m */
			const uint32_t b = w - (uint32_t)s->fine_base - (N_FINE - 1u);
			for (uint32_t m = 0; m < N_FINE; ++m) {
				s->cf[m] += dl[(b + m) & DL_MASK][1] * sl;
			}
			s->cf_ex += (double)(yl * yl);
			s->cf_es += (double)(sl * sl);
		}

		w = (w + 1u) & DL_MASK;
	}

	/* phasor renormalisation (one Newton step: |ph| is within 1e-5 of 1) */
	ph *= 1.5f - 0.5f * (ph[0] * ph[0] + ph[1] * ph[1]);

	s->ph = ph;
	s->value_sm = value_sm;
	s->vol_g = vol_g;
	s->tone_env = tone_env;
	s->hp_x1 = hx1;
	s->hp_y1 = hy1;
	s->bq_z1 = bz1;
	s->bq_z2 = bz2;
	s->zre[0] = zr0;
	s->zre[1] = zr1;
	s->zre[2] = zr2;
	s->zim[0] = zi0;
	s->zim[1] = zi1;
	s->zim[2] = zi2;
	s->np = np;
	s->dp = dp;
	s->ep = ep;
	s->speak = speak;
	s->w = w;
	if (measuring) {
		s->lp_noise = lpn;
		s->rng = rng;
		s->dec_phase = dec_phase;
		s->xdec_w = xdec_w;
	}
}

static void run(LV2_Handle h, uint32_t n)
{
	ExpSense* s = (ExpSense*)h;

	if (__atomic_load_n(&s->restore_pending, __ATOMIC_ACQUIRE)) {
		if (s->r_latency >= 0.f) {
			set_latency(s, s->r_latency);
			s->lat_valid = true;
		}
		if (fabsf(s->r_hi - s->r_lo) > 1e-3f) {
			s->lo = s->r_lo;
			s->hi = s->r_hi;
		}
		if (s->r_tone_k > 0.f) {
			s->tone_k = s->r_tone_k;
		}
		reset_program_est(s);
		__atomic_store_n(&s->restore_pending, 0, __ATOMIC_RELEASE);
	}

	/* parameters (the transcendental coefficients only when their parameter changes) */
	const int mode = clampi((int)lrintf(*s->c[P_MODE]), 0, 2);
	const float freq = clampf(*s->c[P_TONE_FREQ], 1000.f, (float)(0.45 * s->fs));
	if (freq != s->cur_freq) {
		set_freq(s, freq);
	}
	const float tone_level = clampf(*s->c[P_TONE_LEVEL], -80.f, -20.f);
	if (tone_level != s->p_tone_level) {
		s->p_tone_level = tone_level;
		s->tone_amp = db2lin(tone_level);
	}
	const float tone_amp = s->tone_amp;
	const float smooth = clampf(*s->c[P_SMOOTH], 0.1f, 200.f);
	if (smooth != s->p_smooth) {
		s->p_smooth = smooth;
		s->a_sm = onepole_coef(s->fs, 0.001 * smooth);
		float d = 1.f - s->a_sm;
		for (uint32_t k = SUB; k > 1u; k >>= 1) {
			d *= d;
		}
		s->dec_sub = d;
	}
	const float a_sm = s->a_sm;
	const float curve = clampf(*s->c[P_CURVE], 0.25f, 4.f);
	const bool invert = *s->c[P_INVERT] > 0.5f;
	const bool vol_on = *s->c[P_VOL_ON] > 0.5f;
	const bool vol_linear = lrintf(*s->c[P_VOL_LAW]) == VOL_LINEAR;
	const bool cal = *s->c[P_CALIBRATE] > 0.5f;
	const int midi_ch = clampi((int)lrintf(*s->c[P_MIDI_CH]), 1, 16) - 1;
	const int midi_ccn = clampi((int)lrintf(*s->c[P_MIDI_CC]), 0, 119);
	const int midi_14 = (*s->c[P_MIDI_14BIT] > 0.5f && midi_ccn < 32) ? 1 : 0;

	/* Calibrate is a trigger button: act on the rising edge only. The calibration
	 * ends by itself; pressing again aborts it. */
	if (cal && !s->cal_prev) {
		if (calibrating(s)) {
			cal_end(s, RES_ABORTED);
		} else {
			cal_begin(s);
		}
	}
	s->cal_prev = cal;
	const float tone_amp_eff = calibrating(s) ? fmaxf(tone_amp, s->cal_amp) : tone_amp;

	/* MIDI out */
	LV2_Atom_Forge_Frame seq_frame;
	bool midi_ok = false;
	if (s->midi_out) {
		const uint32_t cap = s->midi_out->atom.size;
		lv2_atom_forge_set_buffer(&s->forge, (uint8_t*)s->midi_out, cap);
		midi_ok = lv2_atom_forge_sequence_head(&s->forge, &seq_frame, 0) != 0;
	}

	Coef k;
	k.in_l = s->in_l;
	k.in_r = s->in_r;
	k.sense = s->sense;
	k.out_l = s->out_l;
	k.out_r = s->out_r;
	k.cv = s->cv;
	k.hp_a = (v2f){ s->hp_a, s->hp_a };
	k.b0 = (v2f){ s->b0, s->b0 };
	k.b1 = (v2f){ s->b1, s->b1 };
	k.b2 = (v2f){ s->b2, s->b2 };
	k.a1 = (v2f){ s->a1, s->a1 };
	k.a2 = (v2f){ s->a2, s->a2 };
	k.a_t = (v2f){ s->a_t, s->a_t };
	k.a_p = s->a_p;
	k.rot_s = s->rot_s;
	k.rot_c = s->rot_c;
	k.a_sm = a_sm;
	k.a_vol = s->a_vol;
	k.a_tone_env = s->a_tone_env;
	k.tamp = tone_amp_eff;
	k.noise_amp = s->noise_amp;
	k.vol_mode = vol_on ? (vol_linear ? 1 : 2) : 0;
	k.kv = vol_on && vol_linear ? 0.f : 1.f;
	k.kl = vol_on && vol_linear ? 1.f : 0.f;

	for (uint32_t off = 0; off < n; off += SUB) {
		const uint32_t end = (off + SUB < n) ? off + SUB : n;
		const uint32_t ns = end - off;

		switch (s->lat_phase) {
		case LAT_COARSE: sub_run(s, &k, off, end, 1); break;
		case LAT_FINE: sub_run(s, &k, off, end, 2); break;
		default: sub_run(s, &k, off, end, 0); break;
		}

		/* ---- estimate at the end of each sub-block ---- */
		const v2f zr2 = s->zre[2], zi2 = s->zim[2];
		const float ms = sqrtf(zr2[0] * zr2[0] + zi2[0] * zi2[0]);
		const float mx = sqrtf(zr2[1] * zr2[1] + zi2[1] * zi2[1]);
		const float et = mx * mx;
		float gt = 0.f, wt = 0.f;
		if (et > 1e-12f) {
			gt = s->tone_k * ms / mx;
			wt = et * TONE_W;
		}
		if (calibrating(s)) {
			/* lock-in magnitude of the tone alone: A/2 */
			cal_step(s, ns, ms, mx, 0.5f * tone_amp_eff * s->tone_env);
		}
		const float np = s->np, dp = s->dp, ep = s->ep;
		float gp = 0.f, wp = 0.f, rho = 0.f;
		if (dp > 1e-10f) {
			rho = np * np / (dp * ep + 1e-20f);
			gp = fabsf(np) / dp;
			if (s->lat_valid) {
				wp = dp * clampf((rho - 0.80f) / 0.15f, 0.f, 1.f);
			}
		}

		/* learn the tone/program correction when both are reliable */
		if (s->lat_valid && rho > 0.97f && dp > 1e-6f && et > 1e-9f && ms > 1e-9f) {
			const float ratio = gp * mx / ms;
			const float rate = s->lat_phase == LAT_DONE ? TONE_K_RATE_CAL : TONE_K_RATE;
			s->tone_k += rate * (clampf(ratio, 0.25f, 4.f) - s->tone_k);
		}

		const float wsum = wt + wp;
		s->wsum = wsum;
		bool hold = true;
		if (wsum > W_MIN) {
			s->raw = (wt * gt + wp * gp) / wsum;
			s->have_raw = true;
			hold = false;
		}
		const float rmin = fminf(s->lo, s->hi);
		if (s->have_raw && rmin > 0.02f && s->raw < 0.4f * rmin && !calibrating(s)) {
			hold = true; /* pedal unplugged */
		}
		s->holding = hold;
		if (!hold) {
			s->raw_sm += s->a_raw * (s->raw - s->raw_sm);
			/* signed span: hi < lo when the pedal reads high at heel */
			float span = s->hi - s->lo;
			if (fabsf(span) < 1e-4f) {
				span = span < 0.f ? -1e-4f : 1e-4f;
			}
			float v = clampf((s->raw - s->lo) / span, 0.f, 1.f);
			if (invert) {
				v = 1.f - v;
			}
			s->value_tgt = (curve == 1.f) ? v : powf(v, curve);
		}

		/* tone decision */
		bool need;
		if (calibrating(s)) {
			need = true;
		} else if (mode == MODE_TONE) {
			need = true;
		} else if (mode == MODE_PROGRAM) {
			need = false;
		} else {
			const bool weak = !s->lat_valid || dp < THR_ON || rho < 0.85f;
			const bool strong = s->lat_valid && dp > THR_OFF && rho > 0.92f;
			if (weak) {
				s->tone_hold = (int)(TONE_HOLD_SEC * s->fs);
			} else if (strong) {
				s->tone_hold -= (int)ns;
				if (s->tone_hold < 0) {
					s->tone_hold = 0;
				}
			}
			need = s->tone_hold > 0;
		}
		s->tone_want = need;

		/* MIDI */
		s->midi_timer += ns;
		if (midi_ok && s->midi_timer >= s->midi_min_interval) {
			const bool cfg = midi_ch != s->midi_last_ch || midi_ccn != s->midi_last_cc
			                 || midi_14 != s->midi_last_14;
			const float q = s->value_sm * (midi_14 ? 16383.f : 127.f);
			if (cfg || fabsf(q - s->midi_last) >= 0.6f) {
				const int iv = (int)lrintf(q);
				if (cfg || iv != (int)lrintf(s->midi_last)) {
					if (midi_14) {
						midi_cc(s, end - 1, midi_ch, midi_ccn, iv >> 7);
						midi_cc(s, end - 1, midi_ch, midi_ccn + 32, iv & 0x7F);
					} else {
						midi_cc(s, end - 1, midi_ch, midi_ccn, iv);
					}
					s->midi_last = (float)iv;
					s->midi_last_ch = midi_ch;
					s->midi_last_cc = midi_ccn;
					s->midi_last_14 = midi_14;
					s->midi_timer = 0;
				}
			}
		}
	}

	if (midi_ok) {
		lv2_atom_forge_pop(&s->forge, &seq_frame);
	}

	/* monitor */
	int status;
	if (calibrating(s)) {
		status = s->cal_phase;
	} else if (s->holding) {
		status = ST_HOLD;
	} else {
		status = s->lat_valid ? ST_RUN : ST_UNCAL;
	}
	if (s->o[P_VALUE]) *s->o[P_VALUE] = s->value_sm;
	if (s->o[P_RAW]) *s->o[P_RAW] = s->raw;
	if (s->o[P_TONE_ACTIVE]) *s->o[P_TONE_ACTIVE] = s->tone_env;
	if (s->o[P_STATUS]) *s->o[P_STATUS] = (float)status;
	if (s->o[P_LATENCY]) *s->o[P_LATENCY] = s->lat_valid ? s->latency : 0.f;
	if (s->o[P_CAL_LO]) *s->o[P_CAL_LO] = s->lo;
	if (s->o[P_CAL_HI]) *s->o[P_CAL_HI] = s->hi;
	if (s->o[P_REF_DB]) *s->o[P_REF_DB] = clampf(10.f * log10f(s->wsum + 1e-30f), -120.f, 0.f);
	if (s->o[P_VOL_GAIN]) *s->o[P_VOL_GAIN] = s->vol_g;
	if (s->o[P_RESULT]) *s->o[P_RESULT] = (float)s->result;
}

static void cleanup(LV2_Handle h)
{
	free(h);
}

/* ---- state: latency, min/max and tone correction ---- */

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store,
                             LV2_State_Handle sh, uint32_t flags,
                             const LV2_Feature* const* features)
{
	(void)flags;
	(void)features;
	ExpSense* s = (ExpSense*)h;
	const uint32_t fl = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
	const float lat = s->lat_valid ? s->latency : -1.f;
	store(sh, s->uris.k_latency, &lat, sizeof(float), s->uris.atom_Float, fl);
	store(sh, s->uris.k_lo, &s->lo, sizeof(float), s->uris.atom_Float, fl);
	store(sh, s->uris.k_hi, &s->hi, sizeof(float), s->uris.atom_Float, fl);
	store(sh, s->uris.k_tone_k, &s->tone_k, sizeof(float), s->uris.atom_Float, fl);
	return LV2_STATE_SUCCESS;
}

static float get_float(ExpSense* s, LV2_State_Retrieve_Function retrieve,
                       LV2_State_Handle sh, LV2_URID key, float def)
{
	size_t size = 0;
	uint32_t type = 0, fl = 0;
	const void* v = retrieve(sh, key, &size, &type, &fl);
	if (v && size == sizeof(float) && type == s->uris.atom_Float) {
		return *(const float*)v;
	}
	return def;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                LV2_State_Handle sh, uint32_t flags,
                                const LV2_Feature* const* features)
{
	(void)flags;
	(void)features;
	ExpSense* s = (ExpSense*)h;
	s->r_latency = get_float(s, retrieve, sh, s->uris.k_latency, -1.f);
	s->r_lo = get_float(s, retrieve, sh, s->uris.k_lo, 0.f);
	s->r_hi = get_float(s, retrieve, sh, s->uris.k_hi, 1.f);
	s->r_tone_k = get_float(s, retrieve, sh, s->uris.k_tone_k, 1.f);
	__atomic_store_n(&s->restore_pending, 1, __ATOMIC_RELEASE);
	return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri)
{
	static const LV2_State_Interface state = { save, restore };
	if (!strcmp(uri, LV2_STATE__interface)) {
		return &state;
	}
	return NULL;
}

static const LV2_Descriptor descriptor = {
	ES_URI, instantiate, connect_port, activate, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
