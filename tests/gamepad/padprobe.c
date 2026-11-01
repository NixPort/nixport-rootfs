// Gamepad probe for FEX patch 0006 (NOTES N-054): does what SDL's Linux joystick code does
// with /dev/input, as an x86 program under FEX. Build static (x86-64 and i386) so it needs
// nothing from a guest rootfs:
//   clang -O1 -static -o padprobe64 padprobe.c ; clang -O1 -m32 -static -o padprobe32 padprobe.c
// Prints what it finds and the first events; exit 0 when one Xbox 360 pad was seen and
// `want` events arrived.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define BITS_LONG (sizeof(long) * 8)
#define TEST(bit, arr) ((arr[(bit) / BITS_LONG] >> ((bit) % BITS_LONG)) & 1)

static int filter(const struct dirent *e) { return strncmp(e->d_name, "event", 5) == 0; }

int main(int argc, char **argv) {
    const int want = argc > 1 ? atoi(argv[1]) : 3;
    struct dirent **list;
    const int n = scandir("/dev/input", &list, filter, alphasort);
    if (n < 0) { printf("scandir /dev/input: %s\n", strerror(errno)); return 1; }
    printf("sizeof(struct input_event) = %zu, %d node(s)\n", sizeof(struct input_event), n);
    int found = 0;
    for (int i = 0; i < n; i++) {
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", list[i]->d_name);
        struct stat st;
        if (stat(path, &st) != 0) { printf("%s: stat: %s\n", path, strerror(errno)); continue; }
        printf("%s: stat chr=%d rdev=%u:%u\n", path, S_ISCHR(st.st_mode), major(st.st_rdev), minor(st.st_rdev));
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { printf("%s: open: %s\n", path, strerror(errno)); continue; }
        struct stat fst;
        fstat(fd, &fst);
        printf("  fstat chr=%d rdev=%u:%u\n", S_ISCHR(fst.st_mode), major(fst.st_rdev), minor(fst.st_rdev));
        char js[128];
        printf("  JSIOCGNAME -> %d (%s), as for an evdev node\n", ioctl(fd, JSIOCGNAME(sizeof js), js), strerror(errno));
        struct input_id id;
        char name[128] = "";
        int version = 0;
        unsigned long ev[EV_MAX / BITS_LONG + 1] = {0}, key[KEY_MAX / BITS_LONG + 1] = {0}, abs[ABS_MAX / BITS_LONG + 1] = {0};
        if (ioctl(fd, EVIOCGVERSION, &version) < 0 || ioctl(fd, EVIOCGID, &id) < 0 ||
            ioctl(fd, EVIOCGNAME(sizeof name), name) < 0 || ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0 || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0) {
            printf("  evdev ioctl failed: %s\n", strerror(errno));
            close(fd);
            continue;
        }
        printf("  version %#x bus %#x vendor %04x product %04x ver %04x name \"%s\"\n", version, id.bustype, id.vendor,
               id.product, id.version, name);
        printf("  EV_KEY %d EV_ABS %d BTN_A %d BTN_MODE %d ABS_RX %d ABS_HAT0X %d\n", (int)TEST(EV_KEY, ev),
               (int)TEST(EV_ABS, ev), (int)TEST(BTN_A, key), (int)TEST(BTN_MODE, key), (int)TEST(ABS_RX, abs),
               (int)TEST(ABS_HAT0X, abs));
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) printf("  ABS_X %d..%d fuzz %d flat %d\n", ai.minimum, ai.maximum, ai.fuzz, ai.flat);
        if (ioctl(fd, EVIOCGABS(ABS_Z), &ai) == 0) printf("  ABS_Z %d..%d\n", ai.minimum, ai.maximum);
        printf("  EVIOCGABS(ABS_MISC) -> %d (%s)\n", ioctl(fd, EVIOCGABS(ABS_MISC), &ai), strerror(errno));
        if (id.vendor != 0x045e || id.product != 0x028e || !TEST(BTN_A, key)) { close(fd); continue; }
        found++;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int got = 0;
        while (got < want) {
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 5000) <= 0) { printf("  no events within 5 s\n"); break; }
            struct input_event evs[32];
            const ssize_t r = read(fd, evs, sizeof evs);
            if (r <= 0) { printf("  read: %zd %s\n", r, strerror(errno)); break; }
            for (size_t k = 0; k < (size_t)r / sizeof evs[0]; k++, got++)
                printf("  event type %u code %#x value %d\n", evs[k].type, evs[k].code, evs[k].value);
        }
        close(fd);
        if (got < want) return 3;
    }
    return found == 1 ? 0 : 2;
}
