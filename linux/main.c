#include <linux/compiler_attributes.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <user/user.h>

extern void run_kernel(void);

int main(int argc, const char *argv[])
{
	int i;
	size_t len = 0;
	for (i = 1; i < argc; i++) {
		size_t arg_len = strlen(argv[i]);
		if (i > 1) {
			boot_command_line[len++] = ' ';
		}
		memcpy(boot_command_line + len, argv[i], arg_len);
		len += arg_len;
		boot_command_line[len] = '\0';
	}
	run_kernel();
	for (;;) host_pause();
	return 0;
}
