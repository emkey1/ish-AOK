#include <linux/compiler_attributes.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <user/user.h>

extern void run_kernel(void);

int main(int argc, const char *argv[])
{
	int i;
	size_t len = strlen(boot_command_line);
	for (i = 1; i < argc; i++) {
		size_t arg_len;
		if (i > 1) {
			boot_command_line[len++] = ' ';
			boot_command_line[len] = '\0';
		}
		arg_len = strlen(argv[i]);
		memcpy(boot_command_line + len, argv[i], arg_len + 1);
		len += arg_len;
	}
	run_kernel();
	for (;;) host_pause();
	return 0;
}
