#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s (errno %d)\n",__LINE__,#x,errno); return 1; } } while(0)
static int registrations(int capacity) {
    int kq=kqueue(), a[2], b[2];
    CHECK(kq>=0 && pipe(a)==0 && pipe(b)==0);
    struct kevent changes[2], output[2];
    EV_SET(&changes[0],a[0],EVFILT_READ,EV_ADD|EV_CLEAR|EV_RECEIPT,0,0,NULL);
    EV_SET(&changes[1],b[0],EVFILT_READ,EV_ADD|EV_CLEAR|EV_RECEIPT,0,0,NULL);
    errno=EAGAIN;
    CHECK(kevent(kq,changes,2,capacity?output:NULL,capacity,NULL)==capacity);
    for(int i=0;i<capacity;i++) CHECK((output[i].flags&EV_ERROR) && output[i].data==0);
    CHECK(write(a[1],"a",1)==1 && write(b[1],"b",1)==1);
    struct timespec timeout={1,0};
    int seen=0;
    for(int i=0;i<2 && seen!=3;i++) {
        int n=kevent(kq,NULL,0,output,2,&timeout);
        CHECK(n>0);
        for(int j=0;j<n;j++) {
            CHECK(output[j].filter==EVFILT_READ && !(output[j].flags&EV_ERROR));
            if(output[j].ident==(uintptr_t)a[0]) seen|=1;
            else if(output[j].ident==(uintptr_t)b[0]) seen|=2;
            else CHECK(0);
        }
    }
    CHECK(seen==3);
    EV_SET(&changes[0],a[0],-1000,EV_ADD|EV_RECEIPT,0,0,NULL);
    errno=EAGAIN;
    CHECK(kevent(kq,changes,1,NULL,0,NULL)==-1 && errno==EINVAL);
    close(a[0]);close(a[1]);close(b[0]);close(b[1]);close(kq);
    return 0;
}
int main(void) {
    if(registrations(0) || registrations(1) || registrations(2)) return 1;
    puts("EV_RECEIPT empty, partial and full output capacity passed");
    return 0;
}
