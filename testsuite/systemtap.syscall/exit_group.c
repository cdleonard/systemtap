/* COVERAGE: exit_group */

#include <linux/unistd.h>
#include <sys/syscall.h>

int main ()
{
    // There is no glibc wrapper for this syscall.
    syscall(SYS_exit_group, -1);
    //staptest// exit_group (-1) =
    return 0;
}
