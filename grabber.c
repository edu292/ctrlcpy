#include <fcntl.h>
#include <linux/input.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static inline bool ev_has_key(const uint32_t *bits, size_t key) {
  return (bits[key / 32] & (1U << (key % 32))) != 0;
}

static bool is_gamepad(int fd) {
  uint32_t key_bits[10] = {};

  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
    return false;
  }

  return ev_has_key(key_bits, BTN_GAMEPAD);
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
    fprintf(stderr, "Usage: %s <socket_name>\n", argv[0]);
    return EXIT_FAILURE;
  }

  const char *socket_name = argv[1];
  size_t socket_name_len = strlen(socket_name);
  size_t max_name_len = sizeof(((struct sockaddr_un *)0)->sun_path) - 1;
  if (socket_name_len > max_name_len) {
    fprintf(stderr, "Socket name exceeds max allowed length (%zu bytes)\n",
            max_name_len);
    return EXIT_FAILURE;
  }

  signal(SIGPIPE, SIG_IGN);

  int ev_fd = find_and_grab_gamepad();
  if (ev_fd < 0) {
    fprintf(stderr, "No gamepad found\n");
    return EXIT_FAILURE;
  }

  int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    close(ev_fd);
    return EXIT_FAILURE;
  }

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  addr.sun_path[0] = '\0';
  memcpy(&addr.sun_path[1], socket_name, socket_name_len);

  socklen_t addr_len =
      offsetof(struct sockaddr_un, sun_path) + 1 + socket_name_len;
  if (bind(server_fd, (struct sockaddr *)&addr, addr_len) < 0 ||
      listen(server_fd, 1) < 0) {
    close(server_fd);
    close(ev_fd);
    return EXIT_FAILURE;
  }

  printf("READY\n");
  fflush(stdout);

  int client_fd = accept(server_fd, nullptr, nullptr);
  close(server_fd);
  if (client_fd < 0) {
    close(ev_fd);
    return EXIT_FAILURE;
  }

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
