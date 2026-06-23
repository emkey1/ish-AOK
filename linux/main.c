#include <linux/compiler_attributes.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <user/user.h>

extern void run_kernel(void);

int main(int argc, const char *argv[])
{
	int i;
	size_t len = 0;
	size_t limit = sizeof(boot_command_line);
	for (i = 1; i < argc; i++) {
		if (i > 1) {
			if (len + 1 < limit) {
				boot_command_line[len++] = ' ';
				boot_command_line[len] = '\0';
			}
		}
		size_t arg_len = strlen(argv[i]);
		if (len + arg_len < limit) {
			memcpy(boot_command_line + len, argv[i], arg_len + 1);
			len += arg_len;
		}
	}
	run_kernel();
	for (;;) host_pause();
	return 0;
}
