#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_BIT(bit, array) (((array)[(bit) >> 5] >> ((bit) & 31)) & 1U)

static bool is_gamepad(int fd) {
  uint32_t key_bits[10] = {0};

  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
    return false;
  }

  return TEST_BIT(BTN_GAMEPAD, key_bits);
}

int find_and_grab_gamepad(void) {
  char path[32];

  for (int i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/input/event%d", i);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }

    if (is_gamepad(fd) && ioctl(fd, EVIOCGRAB, 1) == 0) {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      return fd;
    }

    close(fd);
  }

  return -1;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <port>\n", argv[0]);
    return EXIT_FAILURE;
  }

  int port = atoi(argv[1]);
  signal(SIGPIPE, SIG_IGN);

  int ev_fd = find_and_grab_gamepad();
  if (ev_fd < 0) {
    fprintf(stderr, "No active gamepad found across /dev/input/event*\n");
    return EXIT_FAILURE;
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    close(ev_fd);
    return EXIT_FAILURE;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in saddr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  if (bind(server_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0 ||
      listen(server_fd, 1) < 0) {
    close(server_fd);
    close(ev_fd);
    return EXIT_FAILURE;
  }

  printf("READY\n");
  fflush(stdout);

  int client_fd = accept(server_fd, NULL, NULL);
  close(server_fd);
  if (client_fd < 0) {
    close(ev_fd);
    return EXIT_FAILURE;
  }

  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  struct input_event ev;
  while (read(ev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
    if (write(client_fd, &ev, sizeof(ev)) != sizeof(ev)) {
      break;
    }
  }

  ioctl(ev_fd, EVIOCGRAB, 0);
  close(ev_fd);
  close(client_fd);
  return EXIT_SUCCESS;
}
