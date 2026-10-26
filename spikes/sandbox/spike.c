/*
 * Sandbox spike: under a deny-default seatbelt profile applied with
 * sandbox_init(3), does hv_vcpu_run() still work, do fds opened before the
 * drop still work, and do open/exec/fork/socket fail?
 *
 * Usage: spike profile.sb blockfile
 */
#include <Hypervisor/Hypervisor.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <sandbox.h>

extern char **environ;
static int fails;

static void expect(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        errx(1, "usage: spike profile.sb blockfile");

    /* Everything a tender has before hvt_drop_privileges(). */
    int blk = open(argv[2], O_RDWR);
    if (blk < 0)
        err(1, "open %s", argv[2]);
    int sp[2], kick[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) || pipe(kick))
        err(1, "socketpair/pipe");
    int kq = kqueue();
    if (hv_vm_create(NULL) != HV_SUCCESS)
        errx(1, "hv_vm_create");
    uint8_t *mem = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    uint32_t hvc = 0xd4000002; /* hvc #0 */
    memcpy(mem, &hvc, 4);
    hv_vm_map(mem, 0, 0x4000, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *ex;
    if (hv_vcpu_create(&vcpu, &ex, NULL) != HV_SUCCESS)
        errx(1, "hv_vcpu_create");

    FILE *pf = fopen(argv[1], "r");
    if (!pf)
        err(1, "%s", argv[1]);
    static char profile[65536];
    profile[fread(profile, 1, sizeof profile - 1, pf)] = 0;
    fclose(pf);
    char *sberr = NULL;
    if (sandbox_init(profile, 0, &sberr) != 0)
        errx(1, "sandbox_init: %s", sberr);
    printf("sandbox_init: ok\n");

    /* Must keep working. */
    for (int i = 0; i < 3; i++) {
        hv_vcpu_set_reg(vcpu, HV_REG_PC, 0);
        hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5);
        hv_return_t r = hv_vcpu_run(vcpu);
        if (r != HV_SUCCESS || ex->reason != HV_EXIT_REASON_EXCEPTION) {
            expect("hv_vcpu_run", 0);
            break;
        }
        if (i == 2)
            expect("hv_vcpu_run x3", 1);
    }
    char buf[512] = "x";
    expect("pwrite on pre-opened block fd",
           pwrite(blk, buf, 512, 0) == 512);
    expect("pread on pre-opened block fd", pread(blk, buf, 512, 0) == 512);
    expect("fsync on pre-opened block fd", fsync(blk) == 0);
    expect("write on pre-opened socketpair", write(sp[0], buf, 64) == 64);
    expect("read on pre-opened socketpair", read(sp[1], buf, 64) == 64);
    expect("write on pre-opened pipe", write(kick[1], buf, 1) == 1);
    struct kevent kev[2];
    EV_SET(&kev[0], kick[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
    expect("kevent register + wait",
           kevent(kq, kev, 1, &kev[1], 1, &(struct timespec){0, 0}) == 1);
    expect("write to stdout", write(1, "", 0) == 0);
    struct timespec ts;
    expect("clock_gettime", clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

    /* Must fail. */
    expect("open(/etc/passwd) denied", open("/etc/passwd", O_RDONLY) < 0);
    expect("open(argv[2]) again denied", open(argv[2], O_RDONLY) < 0);
    expect("open(/tmp/x, O_CREAT) denied",
           open("/tmp/sbx-spike", O_CREAT | O_WRONLY, 0600) < 0);
    pid_t pid;
    char *av[] = {"/usr/bin/true", NULL};
    int sr = posix_spawn(&pid, "/usr/bin/true", NULL, NULL, av, environ);
    if (sr == 0) {
        int st;
        waitpid(pid, &st, 0);
        sr = !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    expect("posix_spawn(/usr/bin/true) denied", sr != 0);
    pid = fork();
    if (pid == 0)
        _exit(0);
    if (pid > 0)
        waitpid(pid, NULL, 0);
    expect("fork() denied", pid < 0);
    /*
     * Seatbelt does not mediate socket(2) itself, only what is done with
     * the socket, so check connect/bind/sendto.
     */
    struct sockaddr_in sin = {.sin_len = sizeof sin, .sin_family = AF_INET,
                              .sin_port = htons(53)};
    sin.sin_addr.s_addr = htonl(0x01010101); /* 1.1.1.1 */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    expect("connect(AF_INET 1.1.1.1:53) denied",
           s < 0 || connect(s, (struct sockaddr *)&sin, sizeof sin) < 0);
    int d = socket(AF_INET, SOCK_DGRAM, 0);
    expect("sendto(AF_INET 1.1.1.1:53) denied",
           d < 0 || sendto(d, buf, 1, 0, (struct sockaddr *)&sin, sizeof sin) < 0);
    struct sockaddr_in any = {.sin_len = sizeof any, .sin_family = AF_INET};
    int b = socket(AF_INET, SOCK_STREAM, 0);
    expect("bind(AF_INET) denied",
           b < 0 || bind(b, (struct sockaddr *)&any, sizeof any) < 0);
    struct sockaddr_un sun = {.sun_family = AF_UNIX};
    strcpy(sun.sun_path, "/var/run/mDNSResponder");
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    expect("connect(AF_UNIX /var/run/mDNSResponder) denied",
           u < 0 || connect(u, (struct sockaddr *)&sun, sizeof sun) < 0);

    printf("RESULT: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
