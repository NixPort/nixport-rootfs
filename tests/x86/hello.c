// Phase 2 test program: built for x86_64 on the PC, run on the phone through FEX.
#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    uname(&u);
    printf("hello from x86_64 guest code (machine reported to guest: %s)\n", u.machine);
    return 0;
}
