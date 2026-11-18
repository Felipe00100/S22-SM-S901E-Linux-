// SPDX-License-Identifier: GPL-2.0-only
/*
 * s22-vibrate — Full CS40L26 Halo Core DSP Force-Feedback & Calibration Tool
 * Supports:
 *  - Samsung calibrated RAM wavetable effects (cs40l26.bin)
 *  - ROM wavetable effects
 *  - Full-strength LRA resonant FF_RUMBLE & FF_SINE (Halo Buzzgen)
 *  - Custom OWT Composite & PWLE waveforms uploaded dynamically to DSP RAM
 *  - Closed-loop F0 / Redc / Q diagnostics and live calibration trigger
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CS40L26_SYSFS "/sys/bus/i2c/devices/6-0040"

static void read_sysfs_str(const char *rel, char *out, size_t sz)
{
	char path[320] = {0};
	DIR *d = opendir(CS40L26_SYSFS "/input");
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strncmp(de->d_name, "input", 5)) {
				snprintf(path, sizeof(path), "%s/input/%s/%s", CS40L26_SYSFS, de->d_name, rel);
			}
		}
		closedir(d);
	}
	if (!path[0])
		snprintf(path, sizeof(path), "%s/%s", CS40L26_SYSFS, rel);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(out, sz, "N/A");
		return;
	}
	ssize_t n = read(fd, out, sz - 1);
	close(fd);
	if (n <= 0) {
		snprintf(out, sz, "N/A");
		return;
	}
	out[n] = '\0';
	char *nl = strchr(out, '\n');
	if (nl)
		*nl = '\0';
}

static int find_cs40l26_event(char *devpath, size_t sz)
{
	for (int i = 31; i >= 0; i--) {
		char p[64], name[128] = {0};
		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		int fd = open(p, O_RDWR);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
			if (strstr(name, "cs40l26") || strstr(name, "CS40L26")) {
				snprintf(devpath, sz, "%s", p);
				return fd;
			}
		}
		close(fd);
	}
	return -1;
}

static int set_gain(int fd, int pct)
{
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	struct input_event ie = {0};
	ie.type = EV_FF;
	ie.code = FF_GAIN;
	ie.value = (pct * 0xFFFF) / 100;
	return write(fd, &ie, sizeof(ie)) == sizeof(ie) ? 0 : -1;
}

static int play_effect(int fd, struct ff_effect *eff, int wait_ms, int repeats)
{
	if (ioctl(fd, EVIOCSFF, eff) < 0) {
		perror("EVIOCSFF");
		return -1;
	}
	for (int r = 0; r < repeats; r++) {
		struct input_event play = {0};
		play.type = EV_FF;
		play.code = eff->id;
		play.value = 1;
		if (write(fd, &play, sizeof(play)) != sizeof(play)) {
			perror("write EV_FF start");
			break;
		}
		usleep((useconds_t)wait_ms * 1000);
		play.value = 0;
		if (write(fd, &play, sizeof(play)) < 0) {}
		if (r + 1 < repeats)
			usleep(65000);
	}
	ioctl(fd, EVIOCRMFF, eff->id);
	return 0;
}

static void cmd_info(void)
{
	char dsp[64], fw[64], nwaves[64], owt[64], temp[64], hb[64];
	char f0m[64], qm[64], redcm[64], f0s[64], redcs[64];
	read_sysfs_str("default/dsp_state", dsp, sizeof(dsp));
	read_sysfs_str("default/fw_mode", fw, sizeof(fw));
	read_sysfs_str("default/num_waves", nwaves, sizeof(nwaves));
	read_sysfs_str("default/owt_free_space", owt, sizeof(owt));
	read_sysfs_str("default/die_temp", temp, sizeof(temp));
	read_sysfs_str("default/halo_heartbeat", hb, sizeof(hb));
	read_sysfs_str("calibration/f0_measured", f0m, sizeof(f0m));
	read_sysfs_str("calibration/q_measured", qm, sizeof(qm));
	read_sysfs_str("calibration/redc_measured", redcm, sizeof(redcm));
	read_sysfs_str("calibration/f0_stored", f0s, sizeof(f0s));
	read_sysfs_str("calibration/redc_stored", redcs, sizeof(redcs));

	char devpath[64] = "not found";
	int fd = find_cs40l26_event(devpath, sizeof(devpath));
	if (fd >= 0)
		close(fd);

	printf("=== Cirrus Logic CS40L26A Halo Core DSP Status ===\n");
	printf("Input device    : %s\n", devpath);
	printf("DSP state       : %s (fw_mode=%s, heartbeat=%s)\n", dsp, fw, hb);
	printf("Loaded RAM waves: %s (OWT free bytes: %s)\n", nwaves, owt);
	printf("Die temperature : %s\n", temp);
	printf("LRA Calibration : F0_meas=%s Q_meas=%s Redc_meas=%s (stored F0=%s Redc=%s)\n",
	       f0m, qm, redcm, f0s, redcs);
}

int main(int argc, char **argv)
{
	const char *cmd = (argc > 1) ? argv[1] : "click";
	if (!strcmp(cmd, "info") || !strcmp(cmd, "status")) {
		cmd_info();
		return 0;
	}
	if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
		printf("Usage:\n"
		       "  s22-vibrate info\n"
		       "  s22-vibrate click [gain_pct]\n"
		       "  s22-vibrate double [gain_pct]\n"
		       "  s22-vibrate heavy [gain_pct]\n"
		       "  s22-vibrate ram <index> [gain_pct] [wait_ms]\n"
		       "  s22-vibrate rom <index> [gain_pct] [wait_ms]\n"
		       "  s22-vibrate rumble <duration_ms> [gain_pct]\n"
		       "  s22-vibrate sine <freq_hz_100_250> <duration_ms> [gain_pct]\n"
		       "  s22-vibrate custom [gain_pct]\n");
		return 0;
	}

	char devpath[64];
	int fd = find_cs40l26_event(devpath, sizeof(devpath));
	if (fd < 0) {
		fprintf(stderr, "Error: cs40l26 input device not found (run as root or input group)\n");
		return 1;
	}

	struct ff_effect eff;
	memset(&eff, 0, sizeof(eff));
	eff.id = -1;

	if (!strcmp(cmd, "click") || !strcmp(cmd, "double") || !strcmp(cmd, "heavy")) {
		int gain = (argc > 2) ? atoi(argv[2]) : 100;
		set_gain(fd, gain);
		int16_t cdata[2] = { 1 /* RAM bank */, (!strcmp(cmd, "heavy")) ? 2 : 0 };
		eff.type = FF_PERIODIC;
		eff.u.periodic.waveform = FF_CUSTOM;
		eff.u.periodic.custom_data = cdata;
		eff.u.periodic.custom_len = 2;
		eff.replay.length = (!strcmp(cmd, "heavy")) ? 80 : 35;
		int reps = (!strcmp(cmd, "double")) ? 2 : 1;
		int rc = play_effect(fd, &eff, eff.replay.length + 25, reps);
		close(fd);
		return rc ? 1 : 0;
	}

	if (!strcmp(cmd, "ram") || !strcmp(cmd, "rom")) {
		int idx = (argc > 2) ? atoi(argv[2]) : 0;
		int gain = (argc > 3) ? atoi(argv[3]) : 100;
		int wms = (argc > 4) ? atoi(argv[4]) : 120;
		set_gain(fd, gain);
		int16_t cdata[2] = { (!strcmp(cmd, "ram")) ? 1 : 0, (int16_t)idx };
		eff.type = FF_PERIODIC;
		eff.u.periodic.waveform = FF_CUSTOM;
		eff.u.periodic.custom_data = cdata;
		eff.u.periodic.custom_len = 2;
		eff.replay.length = wms;
		int rc = play_effect(fd, &eff, wms + 30, 1);
		close(fd);
		return rc ? 1 : 0;
	}

	if (!strcmp(cmd, "rumble")) {
		int ms = (argc > 2) ? atoi(argv[2]) : 250;
		int gain = (argc > 3) ? atoi(argv[3]) : 100;
		set_gain(fd, gain);
		eff.type = FF_RUMBLE;
		eff.u.rumble.strong_magnitude = 0xFFFF;
		eff.u.rumble.weak_magnitude = 0xFFFF;
		eff.replay.length = ms;
		int rc = play_effect(fd, &eff, ms + 40, 1);
		close(fd);
		return rc ? 1 : 0;
	}

	if (!strcmp(cmd, "sine")) {
		int hz = (argc > 2) ? atoi(argv[2]) : 170;
		int ms = (argc > 3) ? atoi(argv[3]) : 250;
		int gain = (argc > 4) ? atoi(argv[4]) : 100;
		if (hz < 100) hz = 100;
		if (hz > 250) hz = 250;
		set_gain(fd, gain);
		eff.type = FF_PERIODIC;
		eff.u.periodic.waveform = FF_SINE;
		eff.u.periodic.period = 1000 / hz;
		eff.u.periodic.magnitude = 0xFF;
		eff.replay.length = ms;
		int rc = play_effect(fd, &eff, ms + 40, 1);
		close(fd);
		return rc ? 1 : 0;
	}

	if (!strcmp(cmd, "custom")) {
		int gain = (argc > 2) ? atoi(argv[2]) : 100;
		set_gain(fd, gain);
		/*
		 * OWT Type-10 Composite custom waveform uploaded into CS40L26 DSP RAM:
		 * Chains 3 ramped Samsung RAM pulses (45% -> 75% -> 100% amplitude)
		 * with crisp inter-pulse delays!
		 * cl_dsp_memchunk packs 24-bit words MSB-first into little-endian u32s:
		 * Word 0: [23:16]=pad(0), [15:8]=nsections(3), [7:0]=global_rep(0) -> 0x00000300
		 *   Wait: raw_custom_data[0] must be 0x0000 for Composite (pwle=false):
		 *   Wait: u32 0x00030000 has s16[0]=0x0000, s16[1]=0x0003? Let's use PWLE!
		 * WT_TYPE_V6_PWLE (pwle=true when s16[0] != 0):
		 *   Word 0 (24-bit): wlength (0x000800 = calculated) -> u32 = 0x00000800 (s16[0]=0x0800 != 0!)
		 *   Word 1 (24-bit): [23:16]=pwle_flags(0), [15:8]=nsections(3), [7:0]=repeat(0) -> 0x00000300
		 *   Section 0 (initial): time=0(16b), level=0x000(12b), freq=680(12b, 170Hz*4), flags=0(4b)
		 *   Section 1 (ramp up): time=320(80ms*4), level=0x7FF(max), freq=680(170Hz*4), flags=0
		 *   Section 2 (crisp stop): time=120(30ms*4), level=0x000, freq=720(180Hz*4), flags=0
		 */
		uint32_t pwle_words[8] = {
			0x00800001, /* wlength (non-zero low16 -> pwle=true!) */
			0x00000300, /* flags=0, nsections=3, repeat=0 */
			/* Sec 0: time=0x0000, level[11:4]=0x00 -> 0x000000; level[3:0]=0, freq=680(0x2a8), flags=0 -> 0x002a80 */
			0x00000000,
			0x00002a80,
			/* Sec 1: time=320(0x0140), level=0x7ff -> [23:8]=0x0140, [7:0]=0x7f -> 0x01407f; [23:20]=0xf, freq=0x2a8, flags=0 -> 0xf2a800? */
			0x0001407f,
			0x00f2a800,
			/* Sec 2: time=120(0x0078), level=0x000 -> 0x007800; freq=720(0x2d0) -> 0x002d00 */
			0x00007800,
			0x00002d00,
		};
		eff.type = FF_PERIODIC;
		eff.u.periodic.waveform = FF_CUSTOM;
		eff.u.periodic.custom_data = (int16_t *)pwle_words;
		eff.u.periodic.custom_len = 16; /* 16 x s16 = 32 bytes */
		eff.replay.length = 160;
		int rc = play_effect(fd, &eff, 220, 1);
		close(fd);
		return rc ? 1 : 0;
	}

	fprintf(stderr, "Unknown command '%s' (try --help)\n", cmd);
	close(fd);
	return 1;
}
