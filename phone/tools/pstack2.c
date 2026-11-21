/*
 * pstack2 PID [BASE]: one sample of every thread's pc/lr and, when a thread is
 * in SweetMetal2's set_viewport entry, the encoder's replay-log Vec:
 * cap/ptr/len at encoder+0xd58/0xd60/0xd68 and its byte count at +0xdd0.
 * A cap of -1 is the Vec's None: the pass is not logging.
 *
 * The installed SweetMetal2 is the build in phone/metal2/out: set_viewport is
 * at +0x267d70. BASE is the image's dyld load address (the address atos -l
 * takes); omit it to find the SweetMetal2 image through the task's dyld
 * list. Needs task_for_pid (build and sign like pstack).
 */
#include <mach/mach.h>
#include <mach-o/dyld_images.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SET_VIEWPORT_START 0x267d70ULL
#define SET_VIEWPORT_END   0x267dacULL   /* x0 holds the encoder before +0x1c */
#define LOG_OFF            0xd58ULL      /* Vec cap; -1 = no log */

static int rd(task_t t, uint64_t a, void *out, size_t n)
{
	vm_size_t got = 0;
	return vm_read_overwrite(t, (vm_address_t)a, n, (vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n ? 0 : -1;
}

static uint64_t image_base(task_t t)
{
	struct task_dyld_info di;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	if (task_info(t, TASK_DYLD_INFO, (task_info_t)&di, &cnt) != KERN_SUCCESS)
		return 0;
	struct dyld_all_image_infos ai;
	if (rd(t, di.all_image_info_addr, &ai, sizeof(ai)))
		return 0;
	for (uint32_t i = 0; i < ai.infoArrayCount; i++) {
		struct dyld_image_info ii;
		if (rd(t, (uint64_t)(uintptr_t)&ai.infoArray[i], &ii, sizeof(ii)))
			break;
		char path[512];
		if (rd(t, (uint64_t)(uintptr_t)ii.imageFilePath, path, sizeof(path) - 1))
			continue;
		path[sizeof(path) - 1] = 0;
		if (strstr(path, "SweetMetal2") && !strstr(path, "libvulkan"))
			return (uint64_t)(uintptr_t)ii.imageLoadAddress;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pstack2 PID [BASE]\n");
		return 2;
	}
	task_t t;
	kern_return_t kr = task_for_pid(mach_task_self(), atoi(argv[1]), &t);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "task_for_pid: %s\n", mach_error_string(kr));
		return 1;
	}
	uint64_t base = argc >= 3 ? strtoull(argv[2], NULL, 0) : image_base(t);
	printf("base 0x%llx\n", (unsigned long long)base);
	thread_act_array_t th;
	mach_msg_type_number_t nth;
	if (task_threads(t, &th, &nth) != KERN_SUCCESS)
		return 1;
	for (unsigned i = 0; i < nth; i++) {
		arm_thread_state64_t s;
		mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&s, &sc) != KERN_SUCCESS)
			continue;
		uint64_t pc = arm_thread_state64_get_pc(s);
		if (!base || pc < base + SET_VIEWPORT_START || pc >= base + SET_VIEWPORT_END)
			continue;
		/* set_viewport moves the encoder from x0 to x20 at +0x267d8c. */
		uint64_t enc = (pc - base) > 0x267d8c ? s.__x[20] : s.__x[0];
		uint64_t log = 0, ptr = 0, len = 0, size = 0;
		int ok = rd(t, enc + LOG_OFF, &log, sizeof(log)) == 0;
		if (ok) {
			rd(t, enc + LOG_OFF + 8, &ptr, sizeof(ptr));
			rd(t, enc + LOG_OFF + 16, &len, sizeof(len));
			rd(t, enc + LOG_OFF + 0x78, &size, sizeof(size));
		}
		printf("thread %u pc +0x%llx enc 0x%llx log %s0x%llx ptr 0x%llx len %llu bytes %llu\n",
		       i, (unsigned long long)(pc - base), (unsigned long long)enc,
		       log == ~0ULL ? "none " : "cap ", (unsigned long long)log,
		       (unsigned long long)ptr, (unsigned long long)len,
		       (unsigned long long)size);
	}
	return 0;
}
