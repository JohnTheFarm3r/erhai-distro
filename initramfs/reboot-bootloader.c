// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reboot into the bootloader (fastboot): LINUX_REBOOT_CMD_RESTART2 with
 * "bootloader" makes nvmem-reboot-mode write 0x02 into the PMK8550 SDAM
 * restart reason cell, as Android's "reboot bootloader" does.
 */
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	sync();
	return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		       LINUX_REBOOT_CMD_RESTART2, "bootloader");
}
