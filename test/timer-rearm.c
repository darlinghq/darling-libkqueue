/* Guest regression: EV_ADD must update an existing timer even with EV_ENABLE. */
#include <sys/event.h>
#include <sys/socket.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions required
#endif
static void update(int kq, unsigned flags, long long usec, unsigned long long token) {
    struct kevent64_s change;
    memset(&change,0,sizeof(change));
    change.ident=42;
    change.filter=EVFILT_TIMER;
    change.flags=flags;
    change.fflags=NOTE_USECONDS;
    change.data=usec;
    change.udata=token;
    assert(kevent64(kq,&change,1,NULL,0,0,NULL)==0);
}
static void receive(int kq, unsigned long long token) {
    struct kevent64_s event;
    struct timespec timeout={1,0};
    int count=kevent64(kq,NULL,0,&event,1,0,&timeout);
    printf("event count=%d\n",count);
    assert(count==1);
    assert(event.ident==42 && event.filter==EVFILT_TIMER);
    assert(!(event.flags&EV_ERROR));
    assert(event.udata==token && event.data>=1);
}
static void ioUpdate(int kq,int fd,int filter,unsigned flags,unsigned long long token) {
    struct kevent64_s change;
    memset(&change,0,sizeof(change));
    change.ident=fd;
    change.filter=filter;
    change.flags=flags;
    change.udata=token;
    assert(kevent64(kq,&change,1,NULL,0,0,NULL)==0);
}
static void ioReceive(int kq,int fd,int filter,unsigned long long token) {
    struct kevent64_s event;
    struct timespec timeout={1,0};
    assert(kevent64(kq,NULL,0,&event,1,0,&timeout)==1);
    assert(event.ident==(unsigned)fd && event.filter==filter && event.udata==token);
    assert(!(event.flags&EV_ERROR));
}
static void ioRegression(int filter) {
    int pair[2],kq=kqueue();
    assert(kq>=0 && socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    if (filter==EVFILT_READ)
        assert(write(pair[1],"x",1)==1);
    ioUpdate(kq,pair[0],filter,EV_ADD|EV_ENABLE,11);
    ioUpdate(kq,pair[0],filter,EV_ADD|EV_ENABLE,12);
    ioReceive(kq,pair[0],filter,12);
    ioUpdate(kq,pair[0],filter,EV_DISABLE,0);
    ioUpdate(kq,pair[0],filter,EV_ADD|EV_ENABLE,13);
    ioReceive(kq,pair[0],filter,13);
    ioUpdate(kq,pair[0],filter,EV_ADD|EV_DISABLE,14);
    ioUpdate(kq,pair[0],filter,EV_ADD|EV_DISABLE,15);
    struct kevent64_s event;
    struct timespec zero={0,0};
    assert(kevent64(kq,NULL,0,&event,1,0,&zero)==0);
    ioUpdate(kq,pair[0],filter,EV_ENABLE,0);
    ioReceive(kq,pair[0],filter,15);
    ioUpdate(kq,pair[0],filter,EV_DISABLE,0);
    ioUpdate(kq,pair[0],filter,EV_ADD,16);
    ioReceive(kq,pair[0],filter,16);
    ioUpdate(kq,pair[0],filter,EV_DELETE,0);
    close(pair[0]);close(pair[1]);close(kq);
}
int main(void) {
    setbuf(stdout,NULL);
    int kq=kqueue();
    assert(kq>=0);
    update(kq,EV_ADD|EV_ENABLE|EV_ONESHOT,30000000,1);
    update(kq,EV_ADD|EV_ENABLE|EV_ONESHOT,20000,2);
    receive(kq,2);
    puts("PASS: enabled rearm changes deadline and udata");
    update(kq,EV_ADD|EV_ENABLE,30000000,3);
    update(kq,EV_ADD|EV_DISABLE,20000,4);
    update(kq,EV_ADD|EV_DISABLE,20000,5);
    struct kevent64_s event;
    struct timespec timeout={0,50000000};
    assert(kevent64(kq,NULL,0,&event,1,0,&timeout)==0);
    update(kq,EV_ENABLE,0,0);
    receive(kq,5);
    update(kq,EV_DELETE,0,0);
    close(kq);
    ioRegression(EVFILT_READ);
    ioRegression(EVFILT_WRITE);
    puts("PASS: timer update with enable and disable");
    return 0;
}
