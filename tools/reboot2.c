/* SPDX-License-Identifier: MIT
 * reboot2 <normal|recovery|bootloader|download>: safe LINUX_REBOOT_CMD_RESTART2 helper.
 * Requires an explicit target argument so running `reboot2` with no args never
 * accidentally reboots into TWRP recovery.
 */
static long sys(long n, long a, long b, long c, long d) {
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c, x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static int streq(const char *a, const char *b) {
	while (*a && *b) {
		if (*a++ != *b++)
			return 0;
	}
	return *a == *b;
}

void _start_c(long *sp) {
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	const char *arg;
	static const char usage[] = "Usage: reboot2 <normal|recovery|bootloader|download>\n";

	if (argc < 2 || !argv[1] || !argv[1][0]) {
		sys(64, 2, (long)usage, sizeof(usage) - 1, 0); /* write(stderr, ...) */
		sys(93, 2, 0, 0, 0);                           /* exit(2) */
	}

	arg = streq(argv[1], "normal") ? "" : argv[1];
	sys(81, 0, 0, 0, 0);                                   /* sync() */
	sys(142, 0xfee1dead, 672274793, 0xA1B2C3D4, (long)arg); /* reboot(RESTART2) */
	sys(93, 1, 0, 0, 0);
}

asm(".global _start\n_start:\n mov x0, sp\n bl _start_c\n");
