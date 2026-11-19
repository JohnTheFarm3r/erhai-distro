// SPDX-License-Identifier: GPL-2.0-only
/*
 * erhai-motion: the accelerometer and gyroscope of the sensor core as a
 * motion sensor input device ("OnePlus Pad 3 Motion Sensors"), the layout
 * game controller drivers use: INPUT_PROP_ACCELEROMETER, ABS_X/Y/Z for
 * acceleration (units per g) and ABS_RX/RY/RZ for angular velocity (units
 * per degree per second), with MSC_TIMESTAMP.
 *
 * The sensors stream only while a program holds the event device open.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>

#include <glib.h>
#include <glib-unix.h>
#include <libssc.h>

#define DEVICE_NAME	"OnePlus Pad 3 Motion Sensors"
#define RATE_HZ		100.0f
#define ACC_RES		8192		/* units per g */
#define ACC_RANGE	(16 * ACC_RES)	/* +-16 g */
#define GYRO_RES	1024		/* units per degree per second */
#define GYRO_RANGE	(2000 * GYRO_RES)	/* +-2000 degrees per second */
#define STANDARD_G	9.80665

/*
 * Placement of the BMI3x0 relative to the panel (native landscape), the
 * same matrix the accelerometer has for screen rotation.
 */
static const double mount[3][3] = {
	{ 0, 1, 0 },
	{ -1, 0, 0 },
	{ 0, 0, 1 },
};

static int ufd = -1;
static char event_path[64];
static SSCSensorAccelerometer *accel;
static SSCSensorGyroscope *gyro;
static gboolean streaming;
static guint recheck_id;

static void emit(__u16 type, __u16 code, __s32 value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };

	if (write(ufd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
		g_warning("uinput write: %s", g_strerror(errno));
}

static void report(__u16 base, double scale, gfloat x, gfloat y, gfloat z)
{
	double in[3] = { x, y, z };

	for (int i = 0; i < 3; i++) {
		double v = mount[i][0] * in[0] + mount[i][1] * in[1] + mount[i][2] * in[2];

		emit(EV_ABS, base + i, (__s32)lround(v * scale));
	}
	emit(EV_MSC, MSC_TIMESTAMP, (__s32)(g_get_monotonic_time() & 0xffffffff));
	emit(EV_SYN, SYN_REPORT, 0);
}

/* Acceleration in m/s^2. */
static void on_accel(SSCSensorAccelerometer *s, gfloat x, gfloat y, gfloat z, gpointer data)
{
	report(ABS_X, ACC_RES / STANDARD_G, x, y, z);
}

/* Angular velocity in rad/s. */
static void on_gyro(SSCSensorGyroscope *s, gfloat x, gfloat y, gfloat z, gpointer data)
{
	report(ABS_RX, GYRO_RES * 180.0 / G_PI, x, y, z);
}

static void set_streaming(gboolean on)
{
	g_autoptr(GError) error = NULL;

	if (on == streaming)
		return;
	if (on) {
		if (!ssc_sensor_accelerometer_open_sync(accel, NULL, &error) ||
		    !ssc_sensor_gyroscope_open_sync(gyro, NULL, &error)) {
			g_warning("cannot start the sensors: %s", error->message);
			ssc_sensor_accelerometer_close_sync(accel, NULL, NULL);
			return;
		}
	} else {
		ssc_sensor_gyroscope_close_sync(gyro, NULL, NULL);
		ssc_sensor_accelerometer_close_sync(accel, NULL, NULL);
	}
	streaming = on;
	g_message("motion sensors %s", on ? "streaming" : "idle");
}

/* Whether any other process holds the event device open. */
static gboolean device_in_use(void)
{
	g_autoptr(GDir) proc = g_dir_open("/proc", 0, NULL);
	const char *pid;
	pid_t self = getpid();

	if (!proc)
		return FALSE;
	while ((pid = g_dir_read_name(proc))) {
		g_autofree char *fds = NULL;
		g_autoptr(GDir) dir = NULL;
		const char *fd;

		if (!g_ascii_isdigit(pid[0]) || atoi(pid) == self)
			continue;
		fds = g_strdup_printf("/proc/%s/fd", pid);
		dir = g_dir_open(fds, 0, NULL);
		if (!dir)
			continue;
		while ((fd = g_dir_read_name(dir))) {
			g_autofree char *link = g_strdup_printf("%s/%s", fds, fd);
			char target[64];
			ssize_t n = readlink(link, target, sizeof(target) - 1);

			if (n <= 0)
				continue;
			target[n] = '\0';
			if (strcmp(target, event_path) == 0)
				return TRUE;
		}
	}
	return FALSE;
}

static gboolean recheck(gpointer data)
{
	recheck_id = 0;
	set_streaming(device_in_use());
	return G_SOURCE_REMOVE;
}

/*
 * inotify merges identical unread events, so its open and close events
 * cannot be counted. Each one triggers a scan of the open files instead.
 */
static gboolean on_inotify(gint fd, GIOCondition cond, gpointer data)
{
	char buf[4096];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
	if (!recheck_id)
		recheck_id = g_timeout_add(200, recheck, NULL);
	return G_SOURCE_CONTINUE;
}

static void abs_axis(__u16 code, int range, int res)
{
	struct uinput_abs_setup a = {
		.code = code,
		.absinfo = { .minimum = -range, .maximum = range, .resolution = res },
	};

	if (ioctl(ufd, UI_ABS_SETUP, &a) < 0)
		g_error("UI_ABS_SETUP: %s", g_strerror(errno));
}

static void create_device(void)
{
	struct uinput_setup setup = {
		.id = { .bustype = BUS_VIRTUAL, .vendor = 0x22d9, .product = 0x0001, .version = 1 },
	};
	char sysname[32];
	g_autofree char *dirpath = NULL;
	g_autoptr(GDir) dir = NULL;
	const char *e;

	ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (ufd < 0)
		g_error("/dev/uinput: %s", g_strerror(errno));
	ioctl(ufd, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER);
	ioctl(ufd, UI_SET_EVBIT, EV_ABS);
	ioctl(ufd, UI_SET_EVBIT, EV_MSC);
	ioctl(ufd, UI_SET_MSCBIT, MSC_TIMESTAMP);
	for (__u16 c = ABS_X; c <= ABS_Z; c++) {
		ioctl(ufd, UI_SET_ABSBIT, c);
		abs_axis(c, ACC_RANGE, ACC_RES);
	}
	for (__u16 c = ABS_RX; c <= ABS_RZ; c++) {
		ioctl(ufd, UI_SET_ABSBIT, c);
		abs_axis(c, GYRO_RANGE, GYRO_RES);
	}
	g_strlcpy(setup.name, DEVICE_NAME, sizeof(setup.name));
	if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0)
		g_error("uinput device: %s", g_strerror(errno));
	if (ioctl(ufd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0)
		g_error("UI_GET_SYSNAME: %s", g_strerror(errno));

	/* The event node appears shortly after creation. */
	dirpath = g_strdup_printf("/sys/devices/virtual/input/%s", sysname);
	for (int i = 0; i < 50 && !event_path[0]; i++) {
		dir = g_dir_open(dirpath, 0, NULL);
		while (dir && (e = g_dir_read_name(dir)))
			if (g_str_has_prefix(e, "event"))
				g_snprintf(event_path, sizeof(event_path), "/dev/input/%s", e);
		g_clear_pointer(&dir, g_dir_close);
		if (!event_path[0])
			g_usleep(20000);
	}
	if (!event_path[0])
		g_error("no event node for %s", sysname);
	for (int i = 0; i < 50 && access(event_path, F_OK) != 0; i++)
		g_usleep(20000);
	g_message("%s on %s", DEVICE_NAME, event_path);
}

static gboolean on_term(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	int ifd;

	accel = ssc_sensor_accelerometer_new_sync(NULL, &error);
	if (!accel)
		g_error("accelerometer: %s", error->message);
	gyro = ssc_sensor_gyroscope_new_sync(NULL, &error);
	if (!gyro)
		g_error("gyroscope: %s", error->message);
	g_object_set(accel, SSC_SENSOR_REQUESTED_RATE, RATE_HZ, NULL);
	g_object_set(gyro, SSC_SENSOR_REQUESTED_RATE, RATE_HZ, NULL);
	g_signal_connect(accel, "measurement", G_CALLBACK(on_accel), NULL);
	g_signal_connect(gyro, "measurement", G_CALLBACK(on_gyro), NULL);

	create_device();

	ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ifd < 0 || inotify_add_watch(ifd, event_path, IN_OPEN | IN_CLOSE) < 0)
		g_error("inotify on %s: %s", event_path, g_strerror(errno));
	g_unix_fd_add(ifd, G_IO_IN, on_inotify, NULL);
	g_unix_signal_add(SIGTERM, on_term, loop);
	g_unix_signal_add(SIGINT, on_term, loop);
	recheck(NULL);

	g_main_loop_run(loop);

	set_streaming(FALSE);
	ioctl(ufd, UI_DEV_DESTROY);
	close(ufd);
	return 0;
}
