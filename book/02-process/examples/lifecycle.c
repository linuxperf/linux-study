/* User-space experiment for book sections 2.6 and 2.7; run on Linux.
 * Build outside linux/: cc -O2 -Wall -Wextra -std=c11 lifecycle.c -o /tmp/lifecycle
 * This program deliberately preserves one zombie for five seconds.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sleep_seconds(time_t seconds)
{
    struct timespec delay = { .tv_sec = seconds, .tv_nsec = 0 };

    while (nanosleep(&delay, &delay) == -1) {
        if (errno != EINTR) {
            perror("nanosleep");
            exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char **argv)
{
    struct sigaction action = { .sa_handler = SIG_DFL };
    siginfo_t info;
    pid_t child, result;
    int status, rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--child") == 0) {
        printf("after exec: pid=%ld ppid=%ld; returning 7\n",
               (long)getpid(), (long)getppid());
        return 7;
    }
    if (argc != 2 || strcmp(argv[1], "--run") != 0) {
        fprintf(stderr, "usage: %s --run\n", argv[0]);
        return EXIT_FAILURE;
    }

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    child = fork();
    if (child == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        printf("before exec: pid=%ld ppid=%ld\n",
               (long)getpid(), (long)getppid());
        execl("/proc/self/exe", "lifecycle", "--child", (char *)NULL);
        perror("execl /proc/self/exe");
        _exit(127);
    }

    printf("parent=%ld child=%ld\n", (long)getpid(), (long)child);
    memset(&info, 0, sizeof(info));
    do {
        rc = waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT);
    } while (rc == -1 && errno == EINTR);
    if (rc == -1) {
        perror("waitid");
        /* Continue to waitpid so an observation failure does not skip reaping. */
    } else {
        printf("WNOWAIT: child=%ld si_code=%d si_status=%d\n",
               (long)info.si_pid, info.si_code, info.si_status);
        printf("inspect /proc/%ld/status now; reaping in 5 seconds\n",
               (long)child);
        sleep_seconds(5);
    }

    do {
        result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (WIFEXITED(status)) {
        printf("reaped child=%ld exit=%d raw_status=%#x\n",
               (long)result, WEXITSTATUS(status), status);
        return rc == 0 && WEXITSTATUS(status) == 7 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (WIFSIGNALED(status))
        printf("child terminated by signal %d\n", WTERMSIG(status));
    return EXIT_FAILURE;
}
