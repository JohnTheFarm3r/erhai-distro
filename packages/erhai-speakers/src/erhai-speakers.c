// SPDX-License-Identifier: GPL-2.0-only
/*
 * erhai-speakers: thermal protection of the six speakers.
 *
 * While the speakers play, every amplifier sends the current it drives
 * through its speaker back over the TDM interface (I2SCTRL1.CFSEL = IV
 * sense: one 32-bit word per slot, full scale 3.3 A). The coil power is
 * I^2 * R(T), with R from the factory calibration and the copper
 * temperature coefficient. A two stage thermal model per speaker (voice
 * coil over magnet, magnet over the inside of the tablet) turns that power
 * into a coil temperature. Above the knee temperature that speaker's
 * amplifier is turned down, quickly while the coil heats and slowly once
 * it cools.
 */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CARD		"hw:0"
#define FEEDBACK_PCM	"hw:0,3"
#define PLAYBACK_STATUS	"/proc/asound/card0/pcm0p/sub0/status"
#define FEEDBACK_ROUTE	"MultiMedia4 Mixer Secondary TDM0 Feedback"
#define CALIBRATION	"/media/aw_cali.bin"
#define STATUS_FILE	"/run/erhai-speakers"

#define RATE		48000
#define CHANNELS	8
#define BLOCK		960		/* 20 ms */
#define FULL_SCALE_A	3.3
#define ALPHA		0.0039		/* copper, per kelvin */
#define CAL_TEMP	25.0
#define VOL_STEP_DB	0.25		/* amplifier volume control step */

struct speaker {
	const char *amp;	/* control name prefix */
	int channel;		/* feedback capture channel */
	double r0;		/* coil resistance at CAL_TEMP, ohm */
	double rth_coil, tau_coil;	/* coil over magnet: K/W, s */
	double rth_mag, tau_mag;	/* magnet over ambient: K/W, s */
	double knee, limit;	/* coil temperature, degrees C */

	double rise_coil, rise_mag;	/* kelvin over the next stage */
	double atten;		/* dB */
	long base, written;	/* volume control values */
	double power;		/* last block, W */
};

/*
 * The capture front end maps the device channels by their position
 * types, so bus slots arrive as: 0 1 5 2 3 4 (6+7) 6. Amplifier n sends
 * on slot n; slots 4 and 7 are idle. The four woofers carry the factory
 * calibration; amplifiers 5 and 6 drive the smaller tweeters.
 */
static struct speaker spk[] = {
	{ "Amp0", 0, 7.0, 25, 1.5, 40, 90, 85, 100 },
	{ "Amp1", 1, 7.0, 25, 1.5, 40, 90, 85, 100 },
	{ "Amp2", 3, 7.0, 25, 1.5, 40, 90, 85, 100 },
	{ "Amp3", 4, 7.0, 25, 1.5, 40, 90, 85, 100 },
	{ "Amp5", 2, 7.0, 40, 0.8, 60, 60, 85, 100 },
	{ "Amp6", 7, 7.0, 40, 0.8, 60, 60, 85, 100 },
};
#define NSPK (sizeof(spk) / sizeof(spk[0]))

static volatile sig_atomic_t running = 1;
static snd_ctl_t *ctl;
static double ambient = 35.0;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int ctl_get(const char *name, long *val)
{
	snd_ctl_elem_value_t *v;
	int ret;

	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(v, name);
	ret = snd_ctl_elem_read(ctl, v);
	if (ret < 0)
		return ret;
	*val = snd_ctl_elem_value_get_integer(v, 0);
	return 0;
}

static int ctl_set(const char *name, long val, int is_bool)
{
	snd_ctl_elem_value_t *v;

	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_interface(v, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(v, name);
	if (is_bool)
		snd_ctl_elem_value_set_boolean(v, 0, val);
	else
		snd_ctl_elem_value_set_integer(v, 0, val);
	return snd_ctl_elem_write(ctl, v);
}

static void volume_name(const struct speaker *s, char *buf, size_t len)
{
	snprintf(buf, len, "%s PCM Playback Volume", s->amp);
}

/* Write base minus the attenuation; a value changed by others is the new base. */
static void apply(struct speaker *s, int restore)
{
	char name[64];
	long cur, want;

	volume_name(s, name, sizeof(name));
	if (ctl_get(name, &cur) < 0)
		return;
	if (s->written < 0 || cur != s->written)
		s->base = cur;
	want = restore ? s->base : s->base - lround(s->atten / VOL_STEP_DB);
	if (want < 0)
		want = 0;
	if (want != cur && ctl_set(name, want, 0) >= 0)
		cur = want;
	s->written = restore ? -1 : cur;
}

static double read_milli(const char *path)
{
	FILE *f = fopen(path, "r");
	long v;

	if (!f)
		return NAN;
	if (fscanf(f, "%ld", &v) != 1)
		v = LONG_MIN;
	fclose(f);
	return v == LONG_MIN ? NAN : v;
}

/* The warmest of the skin and battery sensors stands for the inside. */
static void update_ambient(void)
{
	char path[96], type[32];
	double t = NAN;

	for (int i = 0; i < 128; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
		f = fopen(path, "r");
		if (!f)
			break;
		if (fscanf(f, "%31s", type) == 1 &&
		    (!strcmp(type, "skin-thermal") || !strcmp(type, "qcom-battmgr-bat"))) {
			double v;

			snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
			v = read_milli(path) / 1000.0;
			if (!isnan(v) && (isnan(t) || v > t))
				t = v;
		}
		fclose(f);
	}
	if (!isnan(t) && t > 0 && t < 80)
		ambient = t;
}

static int playing(void)
{
	char line[64];
	FILE *f = fopen(PLAYBACK_STATUS, "r");
	int run = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "state: RUNNING", 14) || !strncmp(line, "state: DRAINING", 15))
			run = 1;
	fclose(f);
	return run;
}

static double coil_temp(const struct speaker *s)
{
	return ambient + s->rise_mag + s->rise_coil;
}

/* Advance the thermal model of one speaker by dt seconds at power p. */
static void model(struct speaker *s, double p, double dt)
{
	double k1 = 1 - exp(-dt / s->tau_coil), k2 = 1 - exp(-dt / s->tau_mag);

	s->rise_coil += (p * s->rth_coil - s->rise_coil) * k1;
	s->rise_mag += (p * s->rth_mag - s->rise_mag) * k2;
}

/*
 * Attenuation for a coil temperature: none below the knee, 12 dB at the
 * limit, and steeper above it. It follows a rise at once and recovers
 * at 1 dB per second.
 */
static void regulate(struct speaker *s, double dt)
{
	double t = coil_temp(s), target = 0;

	if (t > s->knee)
		target = 12 * (t - s->knee) / (s->limit - s->knee);
	if (t > s->limit)
		target += 3 * (t - s->limit);
	if (target > 40)
		target = 40;
	if (target > s->atten)
		s->atten = target;
	else
		s->atten = fmax(target, s->atten - 1.0 * dt);
}

static void status(int active)
{
	FILE *f = fopen(STATUS_FILE ".tmp", "w");

	if (!f)
		return;
	fprintf(f, "%s ambient %.1f C\n", active ? "playing" : "idle", ambient);
	for (size_t i = 0; i < NSPK; i++)
		fprintf(f, "%s R0 %.3f ohm  power %.3f W  coil %.1f C  attenuation %.2f dB\n",
			spk[i].amp, spk[i].r0, spk[i].power, coil_temp(&spk[i]), spk[i].atten);
	fclose(f);
	rename(STATUS_FILE ".tmp", STATUS_FILE);
}

/*
 * The factory calibration of the four woofers: four ten character fields
 * with the coil resistance in milliohm, in amplifier order. It lives on
 * the persist partition, read without mounting it.
 */
static void load_calibration(void)
{
	FILE *p = popen("debugfs -R 'cat " CALIBRATION "' /dev/disk/by-partlabel/persist 2>/dev/null", "r");
	char buf[64] = "";
	double r[4];
	size_t n;

	if (!p)
		return;
	n = fread(buf, 1, sizeof(buf) - 1, p);
	pclose(p);
	buf[n] = '\0';
	if (sscanf(buf, "%lf %lf %lf %lf", &r[0], &r[1], &r[2], &r[3]) != 4)
		return;
	for (int i = 0; i < 4; i++) {
		if (r[i] < 3000 || r[i] > 12000)
			return;
	}
	for (int i = 0; i < 4; i++)
		spk[i].r0 = r[i] / 1000.0;
	logmsg("woofer calibration %.3f %.3f %.3f %.3f ohm",
	       spk[0].r0, spk[1].r0, spk[2].r0, spk[3].r0);
}

static snd_pcm_t *open_feedback(void)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period, buffer;
	snd_pcm_t *pcm;
	int ret;
	long route;

	/* Writing the route control reconnects the graph, even to the same value. */
	if (ctl_get(FEEDBACK_ROUTE, &route) < 0 || !route)
		ctl_set(FEEDBACK_ROUTE, 1, 1);
	ret = snd_pcm_open(&pcm, FEEDBACK_PCM, SND_PCM_STREAM_CAPTURE, 0);
	if (ret < 0) {
		logmsg("feedback open: %s", snd_strerror(ret));
		return NULL;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	period = BLOCK;
	buffer = BLOCK * 8;
	ret = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (!ret)
		ret = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
	if (!ret)
		ret = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
	if (!ret)
		ret = snd_pcm_hw_params_set_rate(pcm, hw, RATE, 0);
	/* Whole periods of the front end's size limits, read one at a time. */
	if (!ret)
		ret = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
	if (!ret)
		ret = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
	if (!ret)
		ret = snd_pcm_hw_params(pcm, hw);
	if (ret < 0 || period != BLOCK) {
		logmsg("feedback setup: %s (period %lu)", snd_strerror(ret), period);
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

static void on_signal(int sig)
{
	running = 0;
}

int main(int argc, char **argv)
{
	static int32_t buf[BLOCK * CHANNELS];
	snd_pcm_t *pcm = NULL;
	double last = now(), last_check = 0, last_status = 0, last_ambient = 0;
	int active = 0, ret;

	/* --knee/--limit lower the temperatures, for testing at low levels. */
	for (int i = 1; i + 1 < argc; i += 2) {
		for (size_t j = 0; j < NSPK; j++) {
			if (!strcmp(argv[i], "--knee"))
				spk[j].knee = atof(argv[i + 1]);
			else if (!strcmp(argv[i], "--limit"))
				spk[j].limit = atof(argv[i + 1]);
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stderr, NULL, _IOLBF, 0);

	ret = snd_ctl_open(&ctl, CARD, 0);
	if (ret < 0) {
		logmsg("control open: %s", snd_strerror(ret));
		return 1;
	}
	for (size_t i = 0; i < NSPK; i++)
		spk[i].written = -1;
	load_calibration();
	update_ambient();

	while (running) {
		double t = now(), dt = t - last;

		if (t - last_check >= 0.25) {
			last_check = t;
			active = playing();
			if (active && !pcm)
				pcm = open_feedback();
			if (!active && pcm) {
				snd_pcm_close(pcm);
				pcm = NULL;
			}
		}
		if (t - last_ambient >= 10) {
			last_ambient = t;
			update_ambient();
		}

		if (!pcm) {
			/* Idle: the coils cool; the model runs at a slow pace. */
			for (size_t i = 0; i < NSPK; i++) {
				model(&spk[i], 0, dt);
				spk[i].power = 0;
				regulate(&spk[i], dt);
				if (spk[i].written >= 0 && spk[i].atten == 0)
					apply(&spk[i], 1);
			}
			last = t;
			if (t - last_status >= 1) {
				last_status = t;
				status(0);
			}
			usleep(active ? 20000 : 250000);
			continue;
		}

		ret = snd_pcm_readi(pcm, buf, BLOCK);
		if (ret < 0) {
			ret = snd_pcm_recover(pcm, ret, 1);
			if (ret < 0) {
				logmsg("feedback read: %s", snd_strerror(ret));
				snd_pcm_close(pcm);
				pcm = NULL;
				usleep(500000);
			}
			continue;
		}
		t = now();
		dt = t - last;
		last = t;

		for (size_t i = 0; i < NSPK; i++) {
			struct speaker *s = &spk[i];
			double sum = 0, mean = 0, r, p;

			for (int f = 0; f < ret; f++)
				mean += buf[f * CHANNELS + s->channel];
			mean /= ret;
			for (int f = 0; f < ret; f++) {
				double v = (buf[f * CHANNELS + s->channel] - mean) / 2147483648.0 * FULL_SCALE_A;

				sum += v * v;
			}
			r = s->r0 * (1 + ALPHA * (coil_temp(s) - CAL_TEMP));
			p = ret ? sum / ret * r : 0;
			s->power = p;
			model(s, p, dt);
			regulate(s, dt);
			if (s->atten > 0 || s->written >= 0) {
				long before = s->written;

				apply(s, s->atten == 0);
				if (s->written != before && s->written >= 0 &&
				    (before < 0 || labs(s->written - before) >= 8))
					logmsg("%s: coil %.0f C, %.1f dB", s->amp, coil_temp(s), s->atten);
			}
		}
		if (t - last_status >= 1) {
			last_status = t;
			status(1);
		}
	}

	if (pcm)
		snd_pcm_close(pcm);
	for (size_t i = 0; i < NSPK; i++)
		if (spk[i].written >= 0)
			apply(&spk[i], 1);
	unlink(STATUS_FILE);
	snd_ctl_close(ctl);
	return 0;
}
