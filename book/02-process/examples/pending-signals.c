/* User-space experiment for book sections 2.8 and 2.9; run on Linux.
 * Build outside linux/: cc -O2 -Wall -Wextra -std=c11 pending-signals.c -o /tmp/pending-signals
 * Sends signals only to this single-threaded process; never changes system limits.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t handled;

static void mark_handled(int signo)
{
    (void)signo;
    handled = 1;
}

int main(void)
{
    struct sigaction action = { .sa_handler = SIG_DFL };
    struct timespec delay = { .tv_sec = 5, .tv_nsec = 0 };
    const struct timespec no_wait = { .tv_sec = 0, .tv_nsec = 0 };
    sigset_t pending_set, old_mask, handler_set;
    siginfo_t info;
    int rt = SIGRTMIN, standard_count = 0, rt_count = 0;
    int i, signo;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (rt > SIGRTMAX) {
        fputs("no usable real-time signal\n", stderr);
        return EXIT_FAILURE;
    }
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) == -1 ||
        sigaction(rt, &action, NULL) == -1) {
        perror("sigaction default");
        return EXIT_FAILURE;
    }
    sigemptyset(&pending_set);
    sigaddset(&pending_set, SIGUSR1);
    sigaddset(&pending_set, rt);
    if (sigprocmask(SIG_BLOCK, &pending_set, &old_mask) == -1) {
        perror("sigprocmask block");
        return EXIT_FAILURE;
    }

    for (i = 0; i < 3; i++) {
        union sigval value = { .sival_int = 101 + i };

        if (kill(getpid(), SIGUSR1) == -1 || sigqueue(getpid(), rt, value) == -1) {
            perror("signal send");
            return EXIT_FAILURE;
        }
    }
    printf("pid=%ld standard=%d realtime=%d; inspect status for 5 seconds\n",
           (long)getpid(), SIGUSR1, rt);
    while (nanosleep(&delay, &delay) == -1) {
        if (errno != EINTR) {
            perror("nanosleep");
            return EXIT_FAILURE;
        }
    }

    for (;;) {
        signo = sigtimedwait(&pending_set, &info, &no_wait);
        if (signo == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            perror("sigtimedwait");
            return EXIT_FAILURE;
        }
        if (signo == SIGUSR1)
            standard_count++;
        if (signo == rt) {
            rt_count++;
            printf("realtime instance: value=%d code=%d\n",
                   info.si_value.sival_int, info.si_code);
        }
    }
    printf("consumed: standard=%d realtime=%d\n", standard_count, rt_count);
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
        perror("sigprocmask restore");
        return EXIT_FAILURE;
    }

    action.sa_handler = mark_handled;
    action.sa_flags = 0;
    if (sigaction(SIGUSR2, &action, NULL) == -1) {
        perror("sigaction handler");
        return EXIT_FAILURE;
    }
    sigemptyset(&handler_set);
    sigaddset(&handler_set, SIGUSR2);
    if (sigprocmask(SIG_UNBLOCK, &handler_set, NULL) == -1) {
        perror("sigprocmask handler");
        return EXIT_FAILURE;
    }
    if (raise(SIGUSR2) != 0) {
        fputs("raise failed\n", stderr);
        return EXIT_FAILURE;
    }
    printf("user handler returned: %s\n", handled ? "yes" : "no");
    return standard_count == 1 && rt_count == 3 && handled
        ? EXIT_SUCCESS : EXIT_FAILURE;
}
