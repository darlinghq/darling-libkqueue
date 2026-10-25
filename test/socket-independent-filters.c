#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno);          \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static int descriptors(void) {
  int count = 0;
  for (int i = 0; i < 512; i++)
    if (fcntl(i, F_GETFD) >= 0)
      count++;
  return count;
}
static int eof(int fd) {
  char byte;
  for (int i = 0; i < 100; i++) {
    int n = recv(fd, &byte, 1, MSG_DONTWAIT);
    if (n == 0)
      return 1;
    if (n > 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
      return 0;
    usleep(5000);
  }
  return 0;
}
static int connection(int *client, int *peer) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  *client = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_len = sizeof(a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  CHECK(listener >= 0 && *client >= 0);
  CHECK(!bind(listener, (struct sockaddr *)&a, len) && !listen(listener, 1) &&
        !getsockname(listener, (struct sockaddr *)&a, &len) &&
        !connect(*client, (struct sockaddr *)&a, len));
  *peer = accept(listener, NULL, NULL);
  CHECK(*peer >= 0);
  close(listener);
  return 0;
}
static int change(int q, int fd, int filter, int flags, int tag) {
  struct kevent e;
  EV_SET(&e, fd, filter, flags | EV_RECEIPT, 0, 0, (void *)(uintptr_t)tag);
  struct kevent receipt;
  struct timespec immediate = {0, 0};
  CHECK(kevent(q, &e, 1, &receipt, 1, &immediate) == 1);
  CHECK((receipt.flags & EV_ERROR) && receipt.data == 0);
  return 0;
}
static int expect(int q, int fd, int wanted, int readTag, int writeTag) {
  int seen = 0;
  for (int tries = 0; tries < 4 && seen != wanted; tries++) {
    struct kevent e[4];
    struct timespec t = {0, 200000000};
    int n = kevent(q, NULL, 0, e, 4, &t);
    CHECK(n > 0);
    for (int i = 0; i < n; i++) {
      CHECK(e[i].ident == (uintptr_t)fd && !(e[i].flags & EV_ERROR));
      int bit = e[i].filter == EVFILT_READ    ? 1
                : e[i].filter == EVFILT_WRITE ? 2
                                              : 0;
      CHECK(bit && (wanted & bit));
      CHECK((uintptr_t)e[i].udata ==
            (uintptr_t)(bit == 1 ? readTag : writeTag));
      seen |= bit;
    }
  }
  CHECK(seen == wanted);
  return 0;
}
static int exercise(int reverse) {
  int client, peer;
  CHECK(!connection(&client, &peer));
  int q = kqueue();
  CHECK(q >= 0);
  CHECK(!change(q, client, reverse ? EVFILT_WRITE : EVFILT_READ,
                EV_ADD | EV_CLEAR, reverse ? 22 : 11));
  CHECK(!change(q, client, reverse ? EVFILT_READ : EVFILT_WRITE,
                EV_ADD | EV_CLEAR, reverse ? 11 : 22));
  CHECK(send(peer, "a", 1, 0) == 1);
  CHECK(!expect(q, client, 3, 11, 22));
  char c;
  CHECK(recv(client, &c, 1, 0) == 1);
  CHECK(!change(q, client, EVFILT_WRITE, EV_DISABLE, 0));
  CHECK(send(peer, "b", 1, 0) == 1);
  CHECK(!expect(q, client, 1, 11, 22));
  CHECK(recv(client, &c, 1, 0) == 1);
  CHECK(!change(q, client, EVFILT_WRITE, EV_ENABLE, 0));
  CHECK(!expect(q, client, 2, 11, 22));
  CHECK(!change(q, client, EVFILT_READ, EV_ADD | EV_CLEAR, 33));
  CHECK(send(peer, "c", 1, 0) == 1);
  CHECK(!expect(q, client, 1, 33, 22));
  CHECK(recv(client, &c, 1, 0) == 1);
  CHECK(!change(q, client, EVFILT_WRITE, EV_DELETE, 0));
  CHECK(send(peer, "d", 1, 0) == 1);
  CHECK(!expect(q, client, 1, 33, 22));
  CHECK(recv(client, &c, 1, 0) == 1);
  CHECK(!change(q, client, EVFILT_WRITE, EV_ADD | EV_CLEAR, 44));
  CHECK(!expect(q, client, 2, 33, 44));
  CHECK(!change(q, client, EVFILT_READ, EV_DELETE, 0));
  CHECK(!change(q, client, EVFILT_WRITE, EV_ADD | EV_CLEAR, 55));
  CHECK(!expect(q, client, 2, 33, 55));
  close(client);
  CHECK(eof(peer));
  close(peer);
  close(q);
  return 0;
}
static int regular_file_cleanup(void) {
  int before = descriptors();
  for (int i = 0; i < 20; i++) {
    char path[] = "/tmp/kqueue-regular-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(unlink(path) == 0);
    CHECK(write(fd, "r", 1) == 1 && lseek(fd, 0, SEEK_SET) == 0);
    int q = kqueue();
    CHECK(q >= 0);
    CHECK(!change(q, fd, EVFILT_READ, EV_ADD, 7));
    CHECK(!expect(q, fd, 1, 7, 0));
    if (i & 1)
      CHECK(!change(q, fd, EVFILT_READ, EV_DISABLE, 0));
    CHECK(close(q) == 0);
    char byte;
    CHECK(read(fd, &byte, 1) == 1 && byte == 'r');
    CHECK(close(fd) == 0);
  }
  CHECK(descriptors() == before);
  return 0;
}

int main(void) {
  if (exercise(0) || exercise(1) || regular_file_cleanup())
    return 1;
  int before = descriptors();
  for (int i = 0; i < 20; i++) {
    int client, peer;
    CHECK(!connection(&client, &peer));
    int q = kqueue();
    CHECK(q >= 0);
    CHECK(!change(q, client, EVFILT_READ, EV_ADD | EV_CLEAR, 1));
    CHECK(!change(q, client, EVFILT_WRITE, EV_ADD | EV_CLEAR, 2));
    if (i & 1)
      CHECK(!change(q, client, EVFILT_READ, EV_DISABLE, 0));
    close(q);
    CHECK(send(peer, "z", 1, 0) == 1);
    char c;
    CHECK(recv(client, &c, 1, 0) == 1);
    close(client);
    CHECK(eof(peer));
    close(peer);
  }
  CHECK(descriptors() == before);
  puts("Independent read/write filters, transitions, close and descriptor "
       "cleanup passed");
  return 0;
}
