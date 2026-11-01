// PC test of the gamepad path (NOTES N-054): the app's bridge (app/src/main/cpp/io/pad_bridge.c,
// through a fake JNIEnv) serving the FEX side (AndroidInputPad.h, via padclient.cpp), both
// running natively on the PC. Covers the socket protocol, both event layouts, the ioctl
// answers, the stat fix and unplugging. Not covered: FEX's syscall wiring (FEX does not run
// under qemu-user, N-011), Android's SELinux, SDL/Steam. Run with tests/gamepad/run-host.sh.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int pc_map(const char *guest, char *out, size_t len);
int pc_number_of_mapped(const char *mapped);
int pc_open(const char *mapped, int flags, int is64);
int pc_ioctl(int fd, unsigned cmd, void *arg, long *result);
void pc_fix_fd_stat(int fd, struct stat *st);

jstring Java_ro_cobrabm_fexdroid_PadBridge_start(JNIEnv *, jclass, jstring);
void Java_ro_cobrabm_fexdroid_PadBridge_stop(JNIEnv *, jclass);
jboolean Java_ro_cobrabm_fexdroid_PadBridge_open(JNIEnv *, jclass, jint);
void Java_ro_cobrabm_fexdroid_PadBridge_close(JNIEnv *, jclass, jint);
void Java_ro_cobrabm_fexdroid_PadBridge_send(JNIEnv *, jclass, jint, jintArray, jint);
jint Java_ro_cobrabm_fexdroid_PadBridge_readers(JNIEnv *, jclass, jint);

// A JNIEnv with the four calls pad_bridge.c makes; jstring/jintArray are plain pointers.
static const char *utf(JNIEnv *e, jstring s, jboolean *c) { (void)e; if (c) *c = 0; return (const char *)s; }
static void release(JNIEnv *e, jstring s, const char *p) { (void)e; (void)s; (void)p; }
static jstring newstr(JNIEnv *e, const char *s) { (void)e; return (jstring)strdup(s); }
static void region(JNIEnv *e, jintArray a, jsize start, jsize len, jint *out) {
    (void)e; memcpy(out, (const jint *)a + start, (size_t)len * sizeof(jint));
}
static struct JNINativeInterface fns;
static JNIEnv env_storage;
static JNIEnv *env = &env_storage;

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

static int wait_readers(int slot, int n) {
    for (int i = 0; i < 200; i++) {
        if (Java_ro_cobrabm_fexdroid_PadBridge_readers(env, NULL, slot) == n) return 1;
        usleep(10000);
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    fns.GetStringUTFChars = utf; fns.ReleaseStringUTFChars = release; fns.NewStringUTF = newstr;
    fns.GetIntArrayRegion = region;
    env_storage = &fns;

    char data[] = "/tmp/padtest-XXXXXX";
    if (!mkdtemp(data)) { perror("mkdtemp"); return 1; }
    char data_slash[256], dir[256];
    snprintf(data_slash, sizeof data_slash, "%s/", data);
    snprintf(dir, sizeof dir, "%s/fexdroid-input", data);
    setenv("PADTEST_DATA_DIR", data_slash, 1);

    char mapped[512];
    CHECK(!pc_map("/dev/input", mapped, sizeof mapped), "no directory yet: /dev/input stays the host's");
    const char *s = (const char *)Java_ro_cobrabm_fexdroid_PadBridge_start(env, NULL, (jstring)dir);
    CHECK(strcmp(s, "Gamepads: on") == 0, "bridge start: %s", s);
    free((void *)s);
    CHECK(pc_map("/dev/input", mapped, sizeof mapped) && strcmp(mapped, dir) == 0, "/dev/input -> %s", mapped);
    CHECK(!pc_map("/dev/inputs", mapped, sizeof mapped), "/dev/inputs is not mapped");
    CHECK(Java_ro_cobrabm_fexdroid_PadBridge_open(env, NULL, 0), "slot 0 open");

    int nodes = 0;
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) nodes += strncmp(e->d_name, "event", 5) == 0;
    if (d) closedir(d);
    CHECK(nodes == 1, "one event node in the directory (%d)", nodes);

    pc_map("/dev/input/event200", mapped, sizeof mapped);
    CHECK(pc_number_of_mapped(mapped) == 200, "event200 is pad number %d", pc_number_of_mapped(mapped));
    char other[512];
    pc_map("/dev/input", other, sizeof other);
    CHECK(pc_number_of_mapped(other) == -1, "the directory itself is no pad");

    const int fd = pc_open(mapped, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 1);
    CHECK(fd >= 0, "64-bit open (fd %d, %s)", fd, fd < 0 ? strerror(errno) : "");
    CHECK(wait_readers(0, 1), "bridge got the hello");

    struct stat st;
    fstat(fd, &st);
    pc_fix_fd_stat(fd, &st);
    CHECK(S_ISCHR(st.st_mode) && major(st.st_rdev) == 13 && minor(st.st_rdev) == 264, "fstat: char device 13:%u", minor(st.st_rdev));
    struct stat plain;
    fstat(0, &plain);
    const mode_t before = plain.st_mode;
    pc_fix_fd_stat(0, &plain);
    CHECK(plain.st_mode == before, "fstat of another fd untouched");

    long r = 0;
    char js[64];
    CHECK(!pc_ioctl(fd, JSIOCGNAME(sizeof js), js, &r), "JSIOCGNAME is not ours (falls through to the kernel)");
    struct input_id id;
    CHECK(pc_ioctl(fd, EVIOCGID, &id, &r) && r == 0 && id.vendor == 0x045e && id.product == 0x028e && id.bustype == BUS_USB,
          "EVIOCGID %04x:%04x bus %x", id.vendor, id.product, id.bustype);
    char name[8];
    CHECK(pc_ioctl(fd, EVIOCGNAME(sizeof name), name, &r) && r == 8 && strcmp(name, "Microso") == 0,
          "EVIOCGNAME truncated to the buffer: \"%s\" (%ld)", name, r);
    unsigned long ev[1] = {0}, keys[KEY_MAX / 64 + 1] = {0}, abs[1] = {0};
    pc_ioctl(fd, EVIOCGBIT(0, sizeof ev), ev, &r);
    CHECK(ev[0] == ((1ul << EV_SYN) | (1ul << EV_KEY) | (1ul << EV_ABS)), "event types %#lx", ev[0]);
    pc_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys, &r);
    CHECK((keys[BTN_A / 64] >> (BTN_A % 64) & 1) && (keys[BTN_THUMBR / 64] >> (BTN_THUMBR % 64) & 1) &&
          !(keys[BTN_C / 64] >> (BTN_C % 64) & 1), "buttons A..THUMBR, no C");
    pc_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs, &r);
    CHECK(abs[0] == ((1ul << ABS_X) | (1ul << ABS_Y) | (1ul << ABS_Z) | (1ul << ABS_RX) | (1ul << ABS_RY) | (1ul << ABS_RZ) |
                     (1ul << ABS_HAT0X) | (1ul << ABS_HAT0Y)), "axes %#lx", abs[0]);
    struct input_absinfo ai;
    CHECK(pc_ioctl(fd, EVIOCGABS(ABS_RY), &ai, &r) && r == 0 && ai.minimum == -32768 && ai.maximum == 32767, "ABS_RY range");
    CHECK(pc_ioctl(fd, EVIOCGABS(ABS_RZ), &ai, &r) && r == 0 && ai.minimum == 0 && ai.maximum == 255, "ABS_RZ range");
    CHECK(pc_ioctl(fd, EVIOCGABS(ABS_MISC), &ai, &r) && r == -EINVAL, "ABS_MISC: EINVAL");
    CHECK(pc_ioctl(fd, EVIOCGUNIQ(sizeof js), js, &r) && r == -ENOENT, "no unique id: ENOENT");
    int effects = -1;
    CHECK(pc_ioctl(fd, EVIOCGEFFECTS, &effects, &r) && r == 0 && effects == 0, "no force feedback effects");
    CHECK(pc_ioctl(fd, EVIOCGRAB, (void *)1, &r) && r == 0, "EVIOCGRAB accepted");

    // A 32-bit reader next to the 64-bit one.
    const int fd32 = pc_open(mapped, O_RDONLY | O_NONBLOCK, 0);
    CHECK(fd32 >= 0 && wait_readers(0, 2), "32-bit open, two readers");

    jint batch[] = {EV_KEY, BTN_A, 1, EV_ABS, ABS_X, -1234};
    Java_ro_cobrabm_fexdroid_PadBridge_send(env, NULL, 0, (jintArray)batch, 2);
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 1000) == 1, "64-bit reader readable");
    struct input_event got[8];
    ssize_t n = read(fd, got, sizeof got);
    CHECK(n == 3 * 24 && got[0].type == EV_KEY && got[0].code == BTN_A && got[0].value == 1 && got[1].type == EV_ABS &&
          got[1].code == ABS_X && got[1].value == -1234 && got[2].type == EV_SYN && got[2].code == SYN_REPORT && got[0].input_event_sec > 0,
          "64-bit events: %zd bytes, A=%d X=%d then SYN", n, got[0].value, got[1].value);
    struct { uint32_t sec, usec; uint16_t type, code; int32_t value; } got32[8];
    p.fd = fd32;
    poll(&p, 1, 1000);
    n = read(fd32, got32, sizeof got32);
    CHECK(n == 3 * 16 && got32[0].code == BTN_A && got32[1].value == -1234 && got32[2].type == EV_SYN && got32[0].sec > 0,
          "32-bit events: %zd bytes", n);

    // What a guest writes (rumble) is dropped, the pad keeps working.
    struct input_event ff = {.type = EV_FF, .code = 0, .value = 1};
    CHECK(write(fd, &ff, sizeof ff) == sizeof ff, "write accepted");
    jint b2[] = {EV_KEY, BTN_A, 0};
    Java_ro_cobrabm_fexdroid_PadBridge_send(env, NULL, 0, (jintArray)b2, 1);
    p.fd = fd;
    poll(&p, 1, 1000);
    n = read(fd, got, sizeof got);
    CHECK(n == 2 * 24 && got[0].value == 0, "still delivered after a write");

    // Unplugged: readers see the end, the node is gone, a later open fails.
    Java_ro_cobrabm_fexdroid_PadBridge_close(env, NULL, 0);
    p.fd = fd;
    poll(&p, 1, 1000);
    n = read(fd, got, sizeof got);
    CHECK(n == 0, "end of stream after unplugging (%zd)", n);
    CHECK(access(mapped, F_OK) != 0, "node removed");
    const int gone = pc_open(mapped, O_RDONLY, 1);
    CHECK(gone < 0 && errno == ENOENT, "open after unplugging: %s", strerror(errno));
    close(fd);
    close(fd32);

    // Plugged in again.
    CHECK(Java_ro_cobrabm_fexdroid_PadBridge_open(env, NULL, 0), "slot 0 open again");
    const int again = pc_open(mapped, O_RDONLY, 1);
    CHECK(again >= 0 && wait_readers(0, 1), "open again");
    close(again);
    CHECK(wait_readers(0, 0), "closed reader is dropped");

    Java_ro_cobrabm_fexdroid_PadBridge_stop(env, NULL);
    CHECK(access(mapped, F_OK) != 0, "stop removes the nodes");
    rmdir(dir);
    rmdir(data);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}
