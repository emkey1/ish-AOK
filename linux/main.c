#include <linux/compiler_attributes.h>
#include <linux/start_kernel.h>
#include <linux/string.h>
#include <user/user.h>

extern void run_kernel(void);

int main(int argc, const char *argv[])
{
	int i;
	for (i = 1; i < argc; i++) {
		if (i > 1)
			strncat(boot_command_line, " ", sizeof(boot_command_line) - strlen(boot_command_line) - 1);
		strncat(boot_command_line, argv[i], sizeof(boot_command_line) - strlen(boot_command_line) - 1);
	}
	run_kernel();
	for (;;) host_pause();
	return 0;
}
