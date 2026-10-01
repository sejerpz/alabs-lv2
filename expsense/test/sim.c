/*
 * Simulation host for ExpSense.
 * Loop model: out_r -> FIR [0.85 0.15] (falling HF response) -> fractional
 * delay D -> pot (g = 0.05 + 0.9*p) -> loop gain k -> + noise -> sense.
 * Main scenario: calibration, wah with pauses and distortion, volume swells
 * from zero. Then the calibration is run against several wiring faults.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#define FS 48000.0
#define BS 128
#define DUR 20.0
#define LOOP_D 317.37 /* simulated round-trip latency (samples) */
#define LOOP_K 0.7f

enum {
	P_IN_L = 0, P_IN_R, P_OUT_L, P_OUT_R, P_SENSE, P_CV, P_MIDI_OUT, P_MODE, P_TONE_FREQ, P_TONE_LEVEL,
	P_SMOOTH, P_CURVE, P_INVERT, P_VOL_ON, P_VOL_LAW, P_CALIBRATE, P_MIDI_CH,
	P_MIDI_CC, P_MIDI_14BIT, P_VALUE, P_RAW, P_TONE_ACTIVE, P_STATUS, P_LATENCY,
	P_CAL_LO, P_CAL_HI, P_REF_DB, P_VOL_GAIN, P_RESULT, P_COUNT
};

/* ---- URID map ---- */
static char* uris[256];
static int n_uris;
static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri)
{
	(void)h;
	for (int i = 0; i < n_uris; ++i)
		if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
	uris[n_uris] = strdup(uri);
	return (LV2_URID)(++n_uris);
}

/* ---- in-memory state store ---- */
typedef struct { uint32_t key, type; size_t size; char val[16]; } Prop;
static Prop props[16];
static int n_props;
static LV2_State_Status st_store(LV2_State_Handle h, uint32_t key, const void* v, size_t size,
                                 uint32_t type, uint32_t flags)
{
	(void)h; (void)flags;
	props[n_props].key = key; props[n_props].type = type; props[n_props].size = size;
	memcpy(props[n_props].val, v, size);
	++n_props;
	return LV2_STATE_SUCCESS;
}
static const void* st_retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type,
                               uint32_t* flags)
{
	(void)h;
	for (int i = 0; i < n_props; ++i)
		if (props[i].key == key) {
			*size = props[i].size; *type = props[i].type; *flags = 0;
			return props[i].val;
		}
	return NULL;
}

/* ---- scenario ---- */
static float pedal_pos(double t)
{
	/* guided calibration: heel, move to toe and hold, back to heel */
	if (t < 1.0) return 0.f;
	if (t < 1.4) return (float)((t - 1.0) / 0.4);
	if (t < 5.3) return 1.f;
	if (t < 5.7) return (float)(1.0 - (t - 5.3) / 0.4);
	if (t < 6.5) return 0.f;
	if (t < 12.0) return (float)(0.55 + 0.40 * sin(2 * M_PI * 2.5 * (t - 6.5))); /* wah */
	/* volume: heel down, swell, hold, down, fast swell */
	if (t < 13.0) return 0.f;
	if (t < 14.0) return (float)(t - 13.0);
	if (t < 16.0) return 1.f;
	if (t < 17.0) return (float)(17.0 - t);
	if (t < 18.0) return 0.f;
	if (t < 18.3) return (float)((t - 18.0) / 0.3);
	return 1.f;
}

static int playing(double t)
{
	if (t < 5.5) return 0;
	if (t >= 8.0 && t < 9.0) return 0; /* pause during the wah */
	return 1;
}

/* Karplus-Strong */
static float ks_buf[2048];
static int ks_len, ks_pos;
static uint32_t rng = 1;
static float frand(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (float)((rng >> 8) & 0xFFFF) / 32768.f - 1.f;
}
static void pluck(float f)
{
	ks_len = (int)(FS / f);
	for (int i = 0; i < ks_len; ++i) ks_buf[i] = 0.5f * frand();
	ks_pos = 0;
}
static float ks_next(void)
{
	if (!ks_len) return 0.f;
	int nx = (ks_pos + 1) % ks_len;
	float y = ks_buf[ks_pos];
	ks_buf[ks_pos] = 0.996f * 0.5f * (ks_buf[ks_pos] + ks_buf[nx]);
	ks_pos = nx;
	return y;
}

/* ---- pedal test against several wiring cases ---- */

enum { W_STD, W_REV_DIR, W_SWAP, W_NONE, W_CLIP, W_LOW, W_SWAP_LOWZ };
enum { RUN_CAL, RUN_CAL_ABORT, RUN_CAL_NOT_MOVED, RUN_CAL_TOE_SHORT };

static const char* res_names[] = { "None", "Running", "OK", "OK, In 2 level low", "Swap Send/Return",
	                               "No signal", "In 2 clipping", "Latency not measurable", "Aborted",
	                               "Toe not held", "OK, check wiring" };

/* guided calibration as a user would do it: heel, toe (held), back to heel */
static float cal_pedal(double t)
{
	if (t < 1.0) return 0.f;
	if (t < 1.4) return (float)((t - 1.0) / 0.4);
	if (t < 5.3) return 1.f;
	if (t < 5.7) return (float)(1.0 - (t - 5.3) / 0.4);
	return 0.f;
}

typedef struct { int result; double t_end; float value_heel; } CaseResult;

/* one button press (0.1 s, like the MOD GUI) */
static float press(double t, double at) { return (t >= at && t < at + 0.1) ? 1.f : 0.f; }

static CaseResult run_case(const LV2_Descriptor* d, const LV2_Feature* const* feats, int wiring, int what)
{
	static float in[BS], in_r[BS], out[BS], out_r[BS], sense[BS], cv[BS];
	const int HN = 8192;
	float* hist = calloc(HN, sizeof(float));
	float ctl[P_COUNT] = { 0 };
	ctl[P_MODE] = 0; ctl[P_TONE_FREQ] = 19000; ctl[P_TONE_LEVEL] = -50; ctl[P_SMOOTH] = 3;
	ctl[P_CURVE] = 1; ctl[P_VOL_LAW] = 0; ctl[P_MIDI_CH] = 1; ctl[P_MIDI_CC] = 11;

	LV2_Handle h = d->instantiate(d, FS, ".", feats);
	d->connect_port(h, P_IN_L, in);
	d->connect_port(h, P_IN_R, in_r);
	d->connect_port(h, P_OUT_L, out);
	d->connect_port(h, P_OUT_R, out_r);
	d->connect_port(h, P_SENSE, sense);
	d->connect_port(h, P_CV, cv);
	d->connect_port(h, P_MIDI_OUT, NULL);
	for (int p = P_MODE; p < P_COUNT; ++p) d->connect_port(h, p, &ctl[p]);
	d->activate(h);

	const double dur = 14.0;
	const float k = wiring == W_CLIP ? 5.f : (wiring == W_LOW ? 0.03f : LOOP_K);
	CaseResult res = { 0, -1.0, -1.f };
	uint32_t nr = 11;
	double next_pluck = 0.0;
	int note_i = 0;
	long n = 0;
	for (long blk = 0; blk * BS < (long)(dur * FS); ++blk) {
		const double tb = (double)blk * BS / FS;
		ctl[P_CALIBRATE] = press(tb, 0.05) + (what == RUN_CAL_ABORT ? press(tb, 2.0) : 0.f);
		for (int i = 0; i < BS; ++i) {
			const long kk = n + i;
			const double t = kk / FS;
			if (t >= next_pluck) { pluck(110.f * (1 + note_i++ % 3)); next_pluck = t + 0.4; }
			in[i] = in_r[i] = 0.6f * ks_next();
			const float yd = hist[(kk - 317) & (HN - 1)];
			float p = what == RUN_CAL_NOT_MOVED ? 0.f : cal_pedal(t);
			if (what == RUN_CAL_TOE_SHORT && t >= 2.5) p = 0.f; /* leaves toe after ~1 s */
			float g;
			switch (wiring) {
			case W_REV_DIR: g = 0.05f + 0.9f * (1.f - p); break;
			/* Out 2 drives the wiper, In 2 (1 Mohm) reads a pot end through part of a 25k pot */
			case W_SWAP: g = 1e6f / (1e6f + 25e3f * (1.f - p)); break;
			/* same, on a low-impedance input: Zin = R / 2.2 (measured on the Dwarf: 0.76 at mid) */
			case W_SWAP_LOWZ: g = 1.f / (1.f + 2.2f * (1.f - p)); break;
			case W_NONE: g = 0.f; break;
			default: g = 0.05f + 0.9f * p; break;
			}
			nr = nr * 1664525u + 1013904223u;
			const float noise = ((float)(nr >> 8) / 16777216.f - 0.5f) * 1e-4f;
			float sv = k * g * yd + noise;
			if (sv > 1.f) sv = 1.f;
			if (sv < -1.f) sv = -1.f;
			sense[i] = sv;
		}
		d->run(h, BS);
		for (int i = 0; i < BS; ++i) hist[(n + i) & (HN - 1)] = out_r[i];
		n += BS;
		if (res.t_end < 0 && tb > 0.3 && (int)ctl[P_RESULT] != 1) res.t_end = tb;
	}
	res.result = (int)ctl[P_RESULT];
	res.value_heel = ctl[P_VALUE]; /* the pedal is back at heel at the end */
	d->cleanup(h);
	free(hist);
	return res;
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: sim plugin.so out.csv\n"); return 1; }
	void* lib = dlopen(argv[1], RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
	const LV2_Descriptor* d = df(0);

	LV2_URID_Map map = { NULL, map_uri };
	LV2_Feature fmap = { LV2_URID__map, &map };
	const LV2_Feature* feats[] = { &fmap, NULL };
	LV2_Handle h = d->instantiate(d, FS, ".", feats);

	static float in[BS], in_r[BS], out[BS], out_r[BS], sense[BS], cv[BS];
	static uint64_t midibuf[2048];
	float ctl[P_COUNT] = { 0 };
	ctl[P_MODE] = 0; ctl[P_TONE_FREQ] = 19000; ctl[P_TONE_LEVEL] = -50; ctl[P_SMOOTH] = 3;
	ctl[P_CURVE] = 1; ctl[P_VOL_LAW] = 0; ctl[P_MIDI_CH] = 1; ctl[P_MIDI_CC] = 11;

	d->connect_port(h, P_IN_L, in);
	d->connect_port(h, P_IN_R, in_r);
	d->connect_port(h, P_OUT_L, out);
	d->connect_port(h, P_OUT_R, out_r);
	d->connect_port(h, P_SENSE, sense);
	d->connect_port(h, P_CV, cv);
	d->connect_port(h, P_MIDI_OUT, midibuf);
	for (int p = P_MODE; p < P_COUNT; ++p) d->connect_port(h, p, &ctl[p]);
	d->activate(h);

	/* output history for the loop */
	const int HN = 8192;
	float* hist = calloc(HN, sizeof(float));
	long n = 0;
	float fir_prev = 0.f;
	const int Di = (int)floor(LOOP_D);
	const float Df = (float)(LOOP_D - Di);
	const float notes[] = { 82.4f, 110.f, 146.8f, 196.f, 246.9f, 329.6f, 164.8f, 220.f };
	int note_i = 0;
	double next_pluck = 5.5;

	FILE* csv = fopen(argv[2], "w");
	fprintf(csv, "t,p,value,raw,tone,status,out_rms\n");

	/* per-segment metrics */
	struct Seg { const char* name; double t0, t1; double e_abs, e_max, tone, n; double out2; double out_n; double d2; double r2; double ir2; } seg[] = {
		{ "wah playing", 6.8, 8.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "wah pause", 8.1, 9.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "wah distorted", 10.0, 12.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "vol heel down", 12.2, 13.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "vol swell", 13.0, 14.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "vol full", 14.2, 16.0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ "vol fast swell", 18.0, 18.6, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	const int NSEG = sizeof(seg) / sizeof(seg[0]);
	/* reference delay compensating smoothing + estimators (empirical) */
	const int REF_LAG_BLK = 3;
	float p_hist[64] = { 0 };
	int midi_count = 0, last_cc = -1;
	double cal_end_t = -1;
	double cpu = 0.0;
	uint32_t nr = 7;

	for (long blk = 0; blk * BS < (long)(DUR * FS); ++blk) {
		const double tb = (double)blk * BS / FS;
		ctl[P_CALIBRATE] = (tb >= 0.2 && tb < 0.3) ? 1.f : 0.f; /* one button press */
		if (cal_end_t < 0 && ctl[P_RESULT] >= 2.f) cal_end_t = tb;
		ctl[P_VOL_ON] = tb >= 12.0 ? 1.f : 0.f;

		for (int i = 0; i < BS; ++i) {
			const long k = n + i;
			const double t = k / FS;
			/* guitar */
			if (playing(t) && t >= next_pluck) {
				pluck(notes[note_i++ % 8]);
				next_pluck = t + 0.4;
			}
			float g = playing(t) ? ks_next() : 0.f;
			if (!playing(t)) ks_len = 0;
			if (t >= 10.0 && t < 12.0) g = 0.4f * tanhf(8.f * g);
			in[i] = 0.6f * g;
			in_r[i] = in[i]; /* R = L: out_r - out_l must contain only tone/excitation */
			/* loop: y delayed by D (all needed samples belong to past blocks) */
			/* windowed-sinc fractional delay (flat to ~20 kHz, like a converter) */
			float yd = 0.f;
			for (int j = -16; j < 16; ++j) {
				const double x = j - Df;
				const double sc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
				const double wv = 0.5 + 0.5 * cos(M_PI * x / 17.0);
				yd += (float)(sc * wv) * hist[(k - Di - j) & (HN - 1)];
			}
			const float gp = 0.05f + 0.9f * pedal_pos(t);
			nr = nr * 1664525u + 1013904223u;
			const float noise = ((float)(nr >> 8) / 16777216.f - 0.5f) * 1e-4f; /* ~ -90 dBFS */
			sense[i] = LOOP_K * gp * yd + noise;
		}

		midibuf[0] = 0;
		LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)midibuf;
		seq->atom.size = sizeof(midibuf);
		seq->atom.type = 0;

		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		d->run(h, BS);
		clock_gettime(CLOCK_MONOTONIC, &b);
		cpu += (b.tv_sec - a.tv_sec) + 1e-9 * (b.tv_nsec - a.tv_nsec);

		LV2_ATOM_SEQUENCE_FOREACH(seq, ev)
		{
			const uint8_t* m = (const uint8_t*)(ev + 1);
			if ((m[0] & 0xF0) == 0xB0) { ++midi_count; last_cc = m[2]; }
		}

		double o2 = 0, d2 = 0, r2 = 0, ir2 = 0;
		for (int i = 0; i < BS; ++i) {
			/* loop FIR applied to the output, then stored in history */
			const float yf = 0.85f * out_r[i] + 0.15f * fir_prev; /* Out 2 feeds the pedal */
			fir_prev = out_r[i];
			hist[(n + i) & (HN - 1)] = yf;
			o2 += out_r[i] * out_r[i];
			d2 += (double)(out_r[i] - out[i]) * (out_r[i] - out[i]);
			r2 += (double)out[i] * out[i];
			ir2 += (double)in[i] * in[i];
		}
		n += BS;

		const double t = (double)n / FS;
		const float p = pedal_pos(t);
		memmove(p_hist + 1, p_hist, sizeof(float) * 63);
		p_hist[0] = p;
		const float pref = p_hist[REF_LAG_BLK];
		const double orms = sqrt(o2 / BS);
		fprintf(csv, "%.5f,%.4f,%.4f,%.4f,%.3f,%d,%.6g\n", t, p, ctl[P_VALUE], ctl[P_RAW],
		        ctl[P_TONE_ACTIVE], (int)ctl[P_STATUS], orms);
		for (int s = 0; s < NSEG; ++s)
			if (t >= seg[s].t0 && t < seg[s].t1) {
				const double e = fabs(ctl[P_VALUE] - pref);
				seg[s].e_abs += e;
				if (e > seg[s].e_max) seg[s].e_max = e;
				seg[s].tone += ctl[P_TONE_ACTIVE];
				seg[s].n += 1;
				seg[s].out2 += o2;
				seg[s].out_n += BS;
				seg[s].d2 += d2;
				seg[s].r2 += r2;
				seg[s].ir2 += ir2;
			}
	}
	fclose(csv);

	printf("measured latency: %.2f samples (true %.2f + ~0.15 from the FIR)\n", ctl[P_LATENCY], LOOP_D);
	printf("calibration ended by itself at %.2f s, result %d (2 = OK)\n", cal_end_t, (int)ctl[P_RESULT]);
	printf("cal min/max: %.4f / %.4f  (expected ~%.4f / %.4f)\n", ctl[P_CAL_LO], ctl[P_CAL_HI],
	       LOOP_K * 0.05, LOOP_K * 0.95);
	printf("\n%-18s %8s %8s %8s %10s %10s %9s\n", "segment", "err_mean", "err_max", "tone%", "out dBFS", "R-L dBFS", "L/inL dB");
	for (int s = 0; s < NSEG; ++s)
		printf("%-18s %8.4f %8.4f %8.0f %10.1f %10.1f %9.1f\n", seg[s].name, seg[s].e_abs / seg[s].n, seg[s].e_max,
		       100 * seg[s].tone / seg[s].n, 10 * log10(seg[s].out2 / seg[s].out_n + 1e-30),
		       10 * log10(seg[s].d2 / seg[s].out_n + 1e-30), 10 * log10((seg[s].r2 + 1e-30) / (seg[s].ir2 + 1e-30)));
	printf("\nMIDI CC sent: %d (last value %d)\n", midi_count, last_cc);
	printf("CPU: %.1f ns/sample (x86 host)\n", 1e9 * cpu / (double)n);

	/* test state save/restore */
	const LV2_State_Interface* si = d->extension_data(LV2_STATE__interface);
	si->save(h, st_store, NULL, 0, NULL);
	LV2_Handle h2 = d->instantiate(d, FS, ".", feats);
	float ctl2[P_COUNT];
	memcpy(ctl2, ctl, sizeof(ctl));
	ctl2[P_CALIBRATE] = 0;
	d->connect_port(h2, P_IN_L, in);
	d->connect_port(h2, P_IN_R, in_r);
	d->connect_port(h2, P_OUT_L, out);
	d->connect_port(h2, P_OUT_R, out_r);
	d->connect_port(h2, P_SENSE, sense);
	d->connect_port(h2, P_CV, cv);
	d->connect_port(h2, P_MIDI_OUT, NULL);
	for (int p = P_MODE; p < P_COUNT; ++p) d->connect_port(h2, p, &ctl2[p]);
	d->activate(h2);
	si->restore(h2, st_retrieve, NULL, 0, NULL);
	memset(in, 0, sizeof(in));
	memset(sense, 0, sizeof(sense));
	d->run(h2, BS);
	printf("state restore: lat %.2f lo %.4f hi %.4f status %d -> %s\n", ctl2[P_LATENCY],
	       ctl2[P_CAL_LO], ctl2[P_CAL_HI], (int)ctl2[P_STATUS],
	       (fabsf(ctl2[P_LATENCY] - ctl[P_LATENCY]) < 1e-3f && ctl2[P_CAL_HI] == ctl[P_CAL_HI]) ? "OK" : "FAIL");
	d->cleanup(h2);
	d->cleanup(h);

	/* guided calibration against wiring faults, abort and no movement */
	struct { const char* name; int wiring; int what; int expect; } cases[] = {
		{ "standard wiring", W_STD, RUN_CAL, 2 },
		{ "heel reads high", W_REV_DIR, RUN_CAL, 2 },
		{ "Tip/Ring swapped", W_SWAP, RUN_CAL, 4 },
		{ "swapped, low-Z In 2", W_SWAP_LOWZ, RUN_CAL, 10 },
		{ "no signal", W_NONE, RUN_CAL, 5 },
		{ "In 2 clipping", W_CLIP, RUN_CAL, 6 },
		{ "In 2 level low", W_LOW, RUN_CAL, 3 },
		{ "pedal never moved", W_STD, RUN_CAL_NOT_MOVED, 4 },
		{ "pressed again", W_STD, RUN_CAL_ABORT, 8 },
		{ "toe not held", W_STD, RUN_CAL_TOE_SHORT, 9 },
	};
	int fails = 0;
	printf("\ncalibration cases (end = when the result appears; value = output at heel afterwards):\n");
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
		const CaseResult r = run_case(d, feats, cases[c].wiring, cases[c].what);
		int ok = r.result == cases[c].expect;
		/* after a successful calibration the pedal at heel must read 0, in both directions */
		if (ok && (r.result == 2 || r.result == 3) && r.value_heel > 0.05f) ok = 0;
		fails += !ok;
		printf("  %-20s -> %-24s end %5.2f s  value %.3f  %s\n", cases[c].name, res_names[r.result],
		       r.t_end, r.value_heel, ok ? "OK" : "FAIL");
	}
	return fails ? 1 : 0;
}
