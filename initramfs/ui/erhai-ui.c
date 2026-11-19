// SPDX-License-Identifier: GPL-2.0-only
/*
 * erhai-ui: the screens of the early boot on the panel, driven by
 * erhai-storage through two pipes in /run/erhai-ui:
 *   in   commands, one per line:
 *          setup              choose the encryption and the password;
 *                             answers "setup everything|files|none"
 *          askpass MESSAGE    ask the storage password; answers "pass"
 *          progress TITLE     the progress screen
 *          step TEXT          the step that runs now (no amount known yet)
 *          bytes DONE TOTAL   how far the step is
 *          message TEXT       an error with a Restart button; answers "restart"
 *          done               clear the panel and exit
 *   out  the answers
 * A password is handed over in /run/erhai-ui/pw (mode 0600), not in a pipe.
 *
 * erhai-ui copy SRC DST copies a file to a block device and reports its
 * progress as "bytes" commands.
 */
#include "lvgl.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DIR_UI "/run/erhai-ui"
#define FONT "A:/usr/share/fonts/erhai/Ubuntu.ttf"

#define C_BG 0x0e1014
#define C_CARD 0x1a1d23
#define C_CARD_ON 0x232a36
#define C_ACCENT 0x4c8dff
#define C_TEXT 0xf2f4f7
#define C_DIM 0xa4abb6
#define C_ERR 0xff6b6b

static int fd_in = -1, fd_out = -1, running = 1;
static lv_font_t *f_title, *f_body, *f_small, *f_big;
static lv_obj_t *scr;

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void reply(const char *s)
{
	char b[256];
	int n = snprintf(b, sizeof(b), "%s\n", s);
	if (write(fd_out, b, n) < 0)
		perror("erhai-ui: reply");
}

/* ---- layout helpers ---- */

static lv_obj_t *label(lv_obj_t *p, const char *text, lv_font_t *f, uint32_t color)
{
	lv_obj_t *l = lv_label_create(p);
	lv_label_set_text(l, text);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static lv_obj_t *column(lv_obj_t *p, int32_t w)
{
	lv_obj_t *c = lv_obj_create(p);
	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, w, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 36, 0);
	return c;
}

static lv_obj_t *button(lv_obj_t *p, const char *text, int primary, lv_event_cb_t cb, void *data)
{
	lv_obj_t *b = lv_button_create(p);
	lv_obj_set_height(b, 150);
	lv_obj_set_style_pad_hor(b, 90, 0);
	lv_obj_set_style_radius(b, 75, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CARD_ON), 0);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
	return b;
}

static lv_obj_t *row(lv_obj_t *p)
{
	lv_obj_t *r = lv_obj_create(p);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 40, 0);
	lv_obj_set_style_pad_top(r, 30, 0);
	return r;
}

static lv_obj_t *kb, *focus_ta, *ta1, *ta2;

static void clear(void)
{
	kb = focus_ta = ta1 = ta2 = NULL;
	lv_obj_clean(scr);
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- text entry: on-screen keyboard and hardware keyboards ---- */

static void focus(lv_obj_t *ta)
{
	if (focus_ta && focus_ta != ta)
		lv_obj_remove_state(focus_ta, LV_STATE_FOCUSED);
	focus_ta = ta;
	if (ta) {
		lv_obj_add_state(ta, LV_STATE_FOCUSED);
		if (kb)
			lv_keyboard_set_textarea(kb, ta);
	}
}

static void ta_focus_cb(lv_event_t *e)
{
	focus(lv_event_get_target(e));
}

static void kb_cancel_cb(lv_event_t *e)
{
	(void)e;
	focus(focus_ta);
}

static lv_obj_t *password_field(lv_obj_t *p, const char *placeholder)
{
	lv_obj_t *ta = lv_textarea_create(p);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_password_mode(ta, true);
	lv_textarea_set_password_show_time(ta, 0);
	lv_textarea_set_placeholder_text(ta, placeholder);
	lv_textarea_set_max_length(ta, 128);
	lv_obj_set_width(ta, lv_pct(100));
	lv_obj_set_style_text_font(ta, f_body, 0);
	lv_obj_set_style_pad_all(ta, 34, 0);
	lv_obj_set_style_radius(ta, 24, 0);
	lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_border_width(ta, 4, 0);
	lv_obj_set_style_border_color(ta, lv_color_hex(C_CARD_ON), 0);
	lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
	lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
	return ta;
}

static void keyboard(void)
{
	kb = lv_keyboard_create(scr);
	lv_obj_set_size(kb, lv_pct(100), 880);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(kb, &lv_font_montserrat_48, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(0x16181d), 0);
	lv_obj_set_style_pad_all(kb, 24, 0);
	lv_obj_set_style_pad_gap(kb, 18, 0);
	lv_obj_set_style_radius(kb, 0, 0);
	lv_obj_set_style_border_width(kb, 0, 0);
	lv_obj_add_event_cb(kb, kb_cancel_cb, LV_EVENT_CANCEL, NULL);
}

static void show_cb(lv_event_t *e)
{
	lv_obj_t *cb = lv_event_get_target(e);
	bool on = lv_obj_has_state(cb, LV_STATE_CHECKED);
	lv_obj_t **tas = lv_event_get_user_data(e);
	for (int i = 0; i < 2; i++)
		if (tas[i])
			lv_textarea_set_password_mode(tas[i], !on);
}

static lv_obj_t *show_toggle(lv_obj_t *p, lv_obj_t **tas)
{
	lv_obj_t *cb = lv_checkbox_create(p);
	lv_checkbox_set_text(cb, "Show the password");
	lv_obj_set_style_text_font(cb, f_small, 0);
	lv_obj_set_style_text_color(cb, lv_color_hex(C_DIM), 0);
	lv_obj_set_style_pad_column(cb, 24, 0);
	lv_obj_set_style_width(cb, 56, LV_PART_INDICATOR);
	lv_obj_set_style_height(cb, 56, LV_PART_INDICATOR);
	lv_obj_add_event_cb(cb, show_cb, LV_EVENT_VALUE_CHANGED, tas);
	return cb;
}

/* Hardware keyboards (the keyboard cover or USB), US layout. */
#define MAX_KBD 8
static int kbd_fd[MAX_KBD], kbd_n, shift, caps;
static const char *map_lo = "\0\0" "1234567890-=\b\t" "qwertyuiop[]\n\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char *map_hi = "\0\0" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\n\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static int test_bit(const unsigned long *b, int n)
{
	return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

static void tab_next(void);

static void hw_key(int code)
{
	if (!focus_ta)
		return;
	if (code == KEY_ENTER || code == KEY_KPENTER) {
		lv_obj_send_event(focus_ta, LV_EVENT_READY, NULL);
		return;
	}
	if (code == KEY_BACKSPACE) {
		lv_textarea_delete_char(focus_ta);
		return;
	}
	if (code == KEY_TAB) {
		tab_next();
		return;
	}
	if (code > KEY_SPACE)
		return;
	int up = shift;
	char c = map_lo[code];
	if (c >= 'a' && c <= 'z')
		up = shift ^ caps;
	c = (up ? map_hi : map_lo)[code];
	if (c >= ' ')
		lv_textarea_add_char(focus_ta, c);
}

static void hw_poll(void)
{
	struct input_event ev;
	for (int i = 0; i < kbd_n; i++)
		while (read(kbd_fd[i], &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY)
				continue;
			if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
				shift = ev.value != 0;
			else if (ev.code == KEY_CAPSLOCK && ev.value == 1)
				caps = !caps;
			else if (ev.value)
				hw_key(ev.code);
		}
}

/* Touch screens and pens become pointers, keyboards are read by hw_poll. */
static void inputs(int32_t w, int32_t h)
{
	char path[64];
	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		unsigned long ev[1] = {0}, key[KEY_CNT / (8 * sizeof(long)) + 1] = {0},
			      abs[ABS_CNT / (8 * sizeof(long)) + 1] = {0}, prop[1] = {0};
		ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
		ioctl(fd, EVIOCGPROP(sizeof(prop)), prop);
		int ax = test_bit(abs, ABS_MT_POSITION_X) ? ABS_MT_POSITION_X : test_bit(abs, ABS_X) ? ABS_X : -1;
		int ay = ax == ABS_MT_POSITION_X ? ABS_MT_POSITION_Y : ABS_Y;
		if (test_bit(ev, EV_ABS) && ax >= 0 && test_bit(prop, INPUT_PROP_DIRECT)) {
			struct input_absinfo x, y;
			if (ioctl(fd, EVIOCGABS(ax), &x) || ioctl(fd, EVIOCGABS(ay), &y)) {
				close(fd);
				continue;
			}
			close(fd);
			lv_indev_t *in = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
			if (!in)
				continue;
			/* A device that is taller than wide on a wide panel is turned. */
			int swap = ((x.maximum - x.minimum) > (y.maximum - y.minimum)) != (w > h);
			lv_evdev_set_swap_axes(in, swap);
			if (swap)
				lv_evdev_set_calibration(in, y.minimum, x.minimum, y.maximum, x.maximum);
			else
				lv_evdev_set_calibration(in, x.minimum, y.minimum, x.maximum, y.maximum);
		} else if (test_bit(ev, EV_KEY) && test_bit(key, KEY_A) && kbd_n < MAX_KBD) {
			/* Keys go only here, never to the console behind the screens. */
			ioctl(fd, EVIOCGRAB, 1);
			kbd_fd[kbd_n++] = fd;
		} else {
			close(fd);
		}
	}
}

/* ---- setup: what is encrypted, then the password ---- */

static const char *choices[] = {"everything", "files", "none"};
static int choice;
static lv_obj_t *opt[3], *err_l;
static lv_obj_t *show_tas[2];
static void view_setup(void);
static void view_newpass(void);

static void tab_next(void)
{
	if (focus_ta == ta1 && ta2)
		focus(ta2);
	else if (ta1)
		focus(ta1);
}

static void opt_cb(lv_event_t *e)
{
	choice = (int)(intptr_t)lv_event_get_user_data(e);
	for (int i = 0; i < 3; i++) {
		if (i == choice)
			lv_obj_add_state(opt[i], LV_STATE_CHECKED);
		else
			lv_obj_remove_state(opt[i], LV_STATE_CHECKED);
	}
}

static void setup_next_cb_now(void *unused)
{
	(void)unused;
	if (!strcmp(choices[choice], "none")) {
		lv_obj_clean(scr);
		reply("setup none");
	} else {
		view_newpass();
	}
}

/* Screens change after the event that asked for it has been handled. */
static void setup_next_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(setup_next_cb_now, NULL);
}

static lv_obj_t *option(lv_obj_t *p, int i, const char *title, const char *text)
{
	lv_obj_t *b = lv_obj_create(p);
	lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(b, 44, 0);
	lv_obj_set_style_pad_row(b, 14, 0);
	lv_obj_set_style_radius(b, 32, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), LV_STATE_CHECKED);
	lv_obj_set_style_border_width(b, 5, 0);
	lv_obj_set_style_border_color(b, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *t = label(b, title, f_big, C_TEXT);
	lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_t *d = label(b, text, f_small, C_DIM);
	lv_obj_add_flag(d, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_event_cb(b, opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	opt[i] = b;
	return b;
}

static void view_setup(void)
{
	clear();
	kb = NULL;
	focus_ta = ta1 = ta2 = NULL;
	lv_obj_t *c = column(scr, 2100);
	lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
	label(c, "Set up the storage", f_title, C_TEXT);
	label(c, "Choose what a password protects. The tablet asks for it each time it starts. "
		 "Changing this later means installing again.", f_body, C_DIM);
	option(c, 0, "Encrypt everything",
	       "The system and your files. Recommended.");
	option(c, 1, "Encrypt only my files",
	       "Your files are protected, the system itself is not.");
	option(c, 2, "No encryption",
	       "No password at startup. Anyone holding the tablet can read its storage.");
	lv_obj_t *r = row(c);
	button(r, "Continue", 1, setup_next_cb, NULL);
	lv_obj_add_state(opt[choice], LV_STATE_CHECKED);
}

static void back_cb_now(void *unused)
{
	(void)unused;
	view_setup();
}

/* Screens change after the event that asked for it has been handled. */
static void back_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(back_cb_now, NULL);
}

static void write_pw(const char *pw)
{
	int fd = open(DIR_UI "/pw", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd >= 0) {
		if (write(fd, pw, strlen(pw)) < 0)
			perror("erhai-ui: pw");
		close(fd);
	}
}

static void newpass_done_cb_now(void *unused)
{
	(void)unused;
	const char *a = lv_textarea_get_text(ta1), *b = lv_textarea_get_text(ta2);
	if (strlen(a) < 6) {
		lv_label_set_text(err_l, "Use at least 6 characters.");
		focus(ta1);
		return;
	}
	if (strcmp(a, b)) {
		lv_label_set_text(err_l, "The two passwords are not the same.");
		focus(ta2);
		return;
	}
	write_pw(a);
	char r[64];
	snprintf(r, sizeof(r), "setup %s", choices[choice]);
	lv_obj_clean(scr);
	kb = NULL;
	focus_ta = ta1 = ta2 = NULL;
	reply(r);
}

/* Screens change after the event that asked for it has been handled. */
static void newpass_done_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(newpass_done_cb_now, NULL);
}

static void ta1_ready_cb(lv_event_t *e)
{
	(void)e;
	focus(ta2);
}

static void view_newpass(void)
{
	clear();
	lv_obj_t *c = column(scr, 2100);
	lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 130);
	label(c, "Choose a password", f_title, C_TEXT);
	label(c, "If the password is forgotten, nothing on the storage can be opened again.",
	      f_body, C_DIM);
	ta1 = password_field(c, "Password");
	ta2 = password_field(c, "The same password again");
	show_tas[0] = ta1;
	show_tas[1] = ta2;
	show_toggle(c, show_tas);
	err_l = label(c, "", f_small, C_ERR);
	lv_obj_t *r = row(c);
	button(r, "Back", 0, back_cb, NULL);
	button(r, "Continue", 1, newpass_done_cb, NULL);
	lv_obj_add_event_cb(ta1, ta1_ready_cb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ta2, newpass_done_cb, LV_EVENT_READY, NULL);
	keyboard();
	focus(ta1);
}

/* ---- unlock ---- */

static void askpass_done_cb_now(void *unused)
{
	(void)unused;
	const char *a = lv_textarea_get_text(ta1);
	if (!*a)
		return;
	write_pw(a);
	lv_obj_clean(scr);
	kb = NULL;
	focus_ta = ta1 = ta2 = NULL;
	lv_obj_t *c = column(scr, 2100);
	lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
	label(c, "Unlocking the storage ...", f_body, C_DIM);
	reply("pass");
}

/* Screens change after the event that asked for it has been handled. */
static void askpass_done_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(askpass_done_cb_now, NULL);
}

static void view_askpass(const char *msg)
{
	clear();
	lv_obj_t *c = column(scr, 1800);
	lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 260);
	label(c, "Enter the storage password", f_title, C_TEXT);
	label(c, *msg ? msg : "The storage of this tablet is encrypted.", f_body, *msg ? C_ERR : C_DIM);
	ta1 = password_field(c, "Password");
	ta2 = NULL;
	show_tas[0] = ta1;
	show_tas[1] = NULL;
	show_toggle(c, show_tas);
	lv_obj_t *r = row(c);
	button(r, "Unlock", 1, askpass_done_cb, NULL);
	lv_obj_add_event_cb(ta1, askpass_done_cb, LV_EVENT_READY, NULL);
	keyboard();
	focus(ta1);
}

/* ---- progress ---- */

static lv_obj_t *p_step, *p_bar, *p_pct, *p_detail, *p_spin;
static double p_rate;
static uint64_t p_last_done;
static uint32_t p_last_ms;

static void view_progress(const char *title)
{
	clear();
	kb = NULL;
	focus_ta = ta1 = ta2 = NULL;
	lv_obj_t *c = column(scr, 2100);
	lv_obj_align(c, LV_ALIGN_CENTER, 0, -60);
	lv_obj_set_style_pad_row(c, 44, 0);
	label(c, title, f_title, C_TEXT);
	p_step = label(c, "", f_big, C_TEXT);
	p_bar = lv_bar_create(c);
	lv_obj_set_size(p_bar, lv_pct(100), 40);
	lv_bar_set_range(p_bar, 0, 1000);
	lv_obj_set_style_bg_color(p_bar, lv_color_hex(0x2c3340), 0);
	lv_obj_set_style_bg_color(p_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	lv_obj_t *r = lv_obj_create(c);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	p_detail = lv_label_create(r);
	lv_obj_set_style_text_font(p_detail, f_small, 0);
	lv_obj_set_style_text_color(p_detail, lv_color_hex(C_DIM), 0);
	lv_label_set_text(p_detail, "");
	p_pct = lv_label_create(r);
	lv_obj_set_style_text_font(p_pct, f_big, 0);
	lv_obj_set_style_text_color(p_pct, lv_color_hex(C_TEXT), 0);
	lv_label_set_text(p_pct, "");
	p_spin = lv_spinner_create(c);
	lv_obj_set_size(p_spin, 140, 140);
	lv_obj_set_style_arc_width(p_spin, 14, 0);
	lv_obj_set_style_arc_width(p_spin, 14, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(p_spin, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(p_spin, lv_color_hex(C_CARD_ON), 0);
	label(c, "Keep the tablet on until this is finished.", f_small, C_DIM);
}

static void progress_busy(const char *text)
{
	if (!p_step)
		view_progress("Please wait");
	lv_label_set_text(p_step, text);
	lv_obj_add_flag(p_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(lv_obj_get_parent(p_pct), LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(p_spin, LV_OBJ_FLAG_HIDDEN);
	p_rate = 0;
	p_last_done = 0;
	p_last_ms = 0;
}

static void size_text(char *b, size_t n, double v)
{
	if (v >= 1e9)
		snprintf(b, n, "%.1f GB", v / 1e9);
	else
		snprintf(b, n, "%.0f MB", v / 1e6);
}

static void progress_bytes(uint64_t done, uint64_t total)
{
	if (!p_step || !total)
		return;
	if (done > total)
		done = total;
	lv_obj_add_flag(p_spin, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(p_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(lv_obj_get_parent(p_pct), LV_OBJ_FLAG_HIDDEN);
	uint32_t t = now_ms();
	if (p_last_ms && t > p_last_ms && done >= p_last_done) {
		double r = (done - p_last_done) * 1000.0 / (t - p_last_ms);
		p_rate = p_rate ? 0.7 * p_rate + 0.3 * r : r;
	}
	p_last_ms = t;
	p_last_done = done;
	lv_bar_set_value(p_bar, (int32_t)(done * 1000 / total), LV_ANIM_OFF);
	char pct[16], a[32], b[32], d[160];
	snprintf(pct, sizeof(pct), "%d %%", (int)(done * 100 / total));
	lv_label_set_text(p_pct, pct);
	size_text(a, sizeof(a), done);
	size_text(b, sizeof(b), total);
	if (p_rate > 1e5 && done < total) {
		double left = (total - done) / p_rate;
		char s[24], tl[48];
		size_text(s, sizeof(s), p_rate);
		if (left < 60)
			snprintf(tl, sizeof(tl), "about %d seconds left", (int)left + 1);
		else
			snprintf(tl, sizeof(tl), "about %d minutes left", (int)(left / 60) + 1);
		snprintf(d, sizeof(d), "%s of %s, %s/s, %s", a, b, s, tl);
	} else {
		snprintf(d, sizeof(d), "%s of %s", a, b);
	}
	lv_label_set_text(p_detail, d);
}

/* ---- error ---- */

static void restart_cb_now(void *unused)
{
	(void)unused;
	reply("restart");
}

/* Screens change after the event that asked for it has been handled. */
static void restart_cb(lv_event_t *e)
{
	(void)e;
	lv_async_call(restart_cb_now, NULL);
}

static void view_message(const char *text)
{
	clear();
	kb = NULL;
	focus_ta = ta1 = ta2 = NULL;
	p_step = NULL;
	lv_obj_t *c = column(scr, 2000);
	lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
	label(c, "Something went wrong", f_title, C_TEXT);
	label(c, text, f_body, C_DIM);
	lv_obj_t *r = row(c);
	button(r, "Restart", 1, restart_cb, NULL);
}

/* ---- commands ---- */

static void command(char *l)
{
	char *arg = strchr(l, ' ');
	if (arg)
		*arg++ = 0;
	else
		arg = "";
	if (!strcmp(l, "setup")) {
		p_step = NULL;
		view_setup();
	} else if (!strcmp(l, "askpass")) {
		p_step = NULL;
		view_askpass(arg);
	} else if (!strcmp(l, "progress")) {
		view_progress(arg);
		progress_busy("");
	} else if (!strcmp(l, "step")) {
		progress_busy(arg);
	} else if (!strcmp(l, "bytes")) {
		unsigned long long d = 0, t = 0;
		if (sscanf(arg, "%llu %llu", &d, &t) == 2)
			progress_bytes(d, t);
	} else if (!strcmp(l, "message")) {
		view_message(arg);
	} else if (!strcmp(l, "done")) {
		running = 0;
	}
}

static void commands(void)
{
	static char buf[4096];
	static size_t len;
	ssize_t n;
	while ((n = read(fd_in, buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += n;
		char *s = buf, *nl;
		while ((nl = memchr(s, '\n', buf + len - s))) {
			*nl = 0;
			command(s);
			s = nl + 1;
		}
		len = buf + len - s;
		memmove(buf, s, len);
		if (len == sizeof(buf) - 1)
			len = 0;
	}
}

/* ---- copy SRC DST ---- */

static int copy(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY | O_CLOEXEC), out = open(dst, O_WRONLY | O_CLOEXEC);
	int ui = open(DIR_UI "/in", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	struct stat st;
	if (in < 0 || out < 0 || fstat(in, &st)) {
		perror("erhai-ui copy");
		return 1;
	}
	size_t bs = 8 << 20;
	char *b = malloc(bs);
	uint64_t done = 0, total = st.st_size;
	uint32_t last = 0;
	for (;;) {
		ssize_t n = read(in, b, bs);
		if (n < 0) {
			perror("erhai-ui copy: read");
			return 1;
		}
		if (!n)
			break;
		for (ssize_t o = 0; o < n;) {
			ssize_t w = write(out, b + o, n - o);
			if (w <= 0) {
				perror("erhai-ui copy: write");
				return 1;
			}
			o += w;
		}
		done += n;
		uint32_t t = now_ms();
		if (ui >= 0 && t - last >= 500) {
			char m[64];
			int k = snprintf(m, sizeof(m), "bytes %llu %llu\n", (unsigned long long)done,
					 (unsigned long long)total);
			if (write(ui, m, k) < 0)
				;
			last = t;
		}
	}
	if (fsync(out)) {
		perror("erhai-ui copy: fsync");
		return 1;
	}
	if (ui >= 0) {
		char m[64];
		int k = snprintf(m, sizeof(m), "bytes %llu %llu\n", (unsigned long long)done,
				 (unsigned long long)total);
		if (write(ui, m, k) < 0)
			;
	}
	return 0;
}

static uint32_t tick(void)
{
	return now_ms();
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "copy"))
		return copy(argv[2], argv[3]);

	fd_in = open(DIR_UI "/in", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	fd_out = open(DIR_UI "/out", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd_in < 0 || fd_out < 0) {
		perror("erhai-ui: " DIR_UI);
		return 1;
	}
	/* The console stops drawing on the panel and reading keys while the
	 * screens are up. */
	int tty = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC), kbmode = K_UNICODE;
	if (tty >= 0) {
		ioctl(tty, KDGKBMODE, &kbmode);
		ioctl(tty, KDSKBMODE, K_OFF);
		ioctl(tty, KDSETMODE, KD_GRAPHICS);
	}

	lv_init();
	lv_tick_set_cb(tick);
	lv_display_t *d = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(d, "/dev/fb0");
	lv_linux_fbdev_set_force_refresh(d, true);
	int32_t w = lv_display_get_horizontal_resolution(d), h = lv_display_get_vertical_resolution(d);

	f_title = lv_tiny_ttf_create_file(FONT, 96);
	f_big = lv_tiny_ttf_create_file(FONT, 60);
	f_body = lv_tiny_ttf_create_file(FONT, 52);
	f_small = lv_tiny_ttf_create_file(FONT, 44);
	if (!f_title || !f_big || !f_body || !f_small)
		f_title = f_big = f_body = f_small = (lv_font_t *)&lv_font_montserrat_48;
	lv_theme_t *th = lv_theme_default_init(d, lv_color_hex(C_ACCENT), lv_color_hex(C_ERR), true, f_body);
	lv_display_set_theme(d, th);
	scr = lv_screen_active();
	clear();
	inputs(w, h);

	while (running) {
		commands();
		hw_poll();
		uint32_t next = lv_timer_handler();
		if (next > 10)
			next = 10;
		usleep(next * 1000);
	}
	lv_obj_clean(scr);
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_refr_now(NULL);
	for (int i = 0; i < kbd_n; i++)
		close(kbd_fd[i]);
	if (tty >= 0) {
		/* Nothing typed on the screens stays for the console to read. */
		tcflush(tty, TCIFLUSH);
		ioctl(tty, KDSETMODE, KD_TEXT);
		ioctl(tty, KDSKBMODE, kbmode);
	}
	return 0;
}
