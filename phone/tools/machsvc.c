/*
 * machsvc: probe a launchd MachService without launchctl.
 *   machsvc lookup NAME          bootstrap_look_up, print the port
 *   machsvc checkin NAME [SECS]  bootstrap_check_in, hold the receive right
 * Exit 0 when the call succeeded.
 */
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *service_port);
kern_return_t bootstrap_check_in(mach_port_t bp, const char *service_name, mach_port_t *service_port);

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: machsvc lookup NAME | checkin NAME [SECS]\n");
		return 2;
	}
	mach_port_t bp = MACH_PORT_NULL, port = MACH_PORT_NULL;
	task_get_bootstrap_port(mach_task_self(), &bp);
	int checkin = !strcmp(argv[1], "checkin");
	kern_return_t kr = checkin ? bootstrap_check_in(bp, argv[2], &port)
				   : bootstrap_look_up(bp, argv[2], &port);
	printf("%s %s: %s (0x%x) port=%u\n", argv[1], argv[2], mach_error_string(kr), kr, port);
	fflush(stdout);
	if (kr != KERN_SUCCESS)
		return 1;
	if (checkin) {
		int secs = argc > 3 ? atoi(argv[3]) : 5;
		printf("holding for %d s\n", secs);
		fflush(stdout);
		sleep(secs);
	}
	return 0;
}
