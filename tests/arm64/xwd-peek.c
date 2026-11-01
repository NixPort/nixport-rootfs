// Attaches Xvfb's -shmem framebuffer through fxshmd's protocol (the same way the
// app's display bridge does, without glibc's shm wrappers) and prints the XWD
// header. Usage: xwd-peek <fxshmd socket> <shmid>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../../tools/fxshmd/fxshm-proto.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <socket> <shmid>\n", argv[0]); return 2; }
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, argv[1], sizeof sa.sun_path - 1);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("connect"); return 1; }
    struct fxshm_req req = { .version = FXSHM_PROTO_VERSION, .op = FXSHM_OP_ATTACH, .id = atoi(argv[2]),
                             .flags = 010000, .pid = getpid() };
    send(s, &req, sizeof req, 0);
    struct fxshm_resp resp;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { &resp, sizeof resp };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    if (recvmsg(s, &msg, 0) != (ssize_t)sizeof resp || resp.result < 0) { fprintf(stderr, "attach failed: %s\n", strerror(resp.err)); return 1; }
    int fd = -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    const uint8_t *h = mmap(NULL, resp.seg.segsz, PROT_READ, MAP_SHARED, fd, 0);
    if (h == MAP_FAILED) { perror("mmap"); return 1; }
    printf("segment %llu bytes; XWD header_size=%u version=%u depth=%u %ux%u bpp=%u stride=%u ncolors=%u\n",
           (unsigned long long)resp.seg.segsz, be32(h), be32(h + 4), be32(h + 12), be32(h + 16), be32(h + 20),
           be32(h + 44), be32(h + 48), be32(h + 76));
    return 0;
}
