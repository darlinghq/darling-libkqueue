#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    setbuf(stdout, NULL);
    puts("step 1: kqueue()");
    int kq = kqueue();
    assert(kq >= 0);

    puts("step 2: kevent64 add 300 timers");
    enum { N_TIMERS = 300 };
    struct kevent64_s kev[N_TIMERS];
    for (int i = 0; i < N_TIMERS; i++) {
        EV_SET64(&kev[i], (uint64_t)(i + 1), EVFILT_TIMER,
                 EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_USECONDS, 1000, (uint64_t)(i + 100), 0, 0);
    }
    assert(kevent64(kq, kev, N_TIMERS, NULL, 0, 0, NULL) == 0);

    puts("step 3: wait timer");
    struct kevent64_s out[32];
    struct timespec ts = { 2, 0 };
    int got = kevent64(kq, NULL, 0, out, 32, 0, &ts);
    printf("step 4: got=%d\n", got);
    assert(got > 0);

    puts("step 5: delete timers");
    for (int i = 0; i < N_TIMERS; i++) {
        kev[i].flags = EV_DELETE;
    }
    (void)kevent64(kq, kev, N_TIMERS, NULL, 0, 0, NULL);

    puts("step 6: fork");
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        puts("step 7c: child kqueue");
        int ckq = kqueue();
        assert(ckq >= 0);
        struct kevent64_s ckev;
        EV_SET64(&ckev, 777, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
                 NOTE_USECONDS, 1000, 777, 0, 0);
        assert(kevent64(ckq, &ckev, 1, &ckev, 1, 0, &ts) == 1);
        assert(ckev.ident == 777 && !(ckev.flags & EV_ERROR));
        close(ckq);
        puts("step 8c: child exit");
        _exit(0);
    }
    puts("step 7p: waitpid");
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(kq);
    puts("PASS: kqueue allocation avoids malloc/close reentrancy deadlock");
    return 0;
}
