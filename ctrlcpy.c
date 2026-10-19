#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

constexpr uint32_t FORWARD_PORT = 12345;
constexpr char LOCAL_BIN[] = "grabber";
constexpr char REMOTE_BIN[] = "/data/local/tmp/grabber";

static int uinput_fd = -1;
static int data_sock = -1;
static int grabber_proc_fd = -1;

void cleanup_and_exit([[maybe_unused]] int sig) {
  if (data_sock >= 0) {
    close(data_sock);
  }
  if (grabber_proc_fd >= 0) {
    close(grabber_proc_fd);
  }
  if (uinput_fd >= 0) {
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
  }

  exit(0);
}

static int adb_connect_server() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(5037),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static bool adb_send_cmd(int fd, const char *service) {
  char header[5];
  size_t len = strlen(service);
  snprintf(header, sizeof(header), "%04zx", len);

  if (write(fd, header, 4) != 4 || write(fd, service, len) != (ssize_t)len) {
    return false;
  }

  char status[4];
  return !(read(fd, status, 4) != 4 || memcmp(status, "OKAY", 4) != 0);
}

static bool write_all(int fd, const void *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, (const char *)buf + sent, len - sent);
    if (n <= 0) {
      return false;
    }

    sent += n;
  }

  return true;
}

static bool adb_push(const char *local_path, const char *remote_path,
                     uint32_t mode) {
  int local_fd = open(local_path, O_RDONLY);
  if (local_fd < 0) {
    return false;
  }

  int fd = adb_connect_server();
  if (fd < 0) {
    close(local_fd);
    return false;
  }

  if (!adb_send_cmd(fd, "host:transport-any") || !adb_send_cmd(fd, "sync:")) {
    close(local_fd);
    close(fd);
    return false;
  }

  char target[256];
  int target_len =
      snprintf(target, sizeof(target), "%s,%o", remote_path, mode & 0777);
  uint32_t len_le = target_len;

  if (!write_all(fd, "SEND", 4) || !write_all(fd, &len_le, 4) ||
      !write_all(fd, target, target_len)) {
    close(local_fd);
    close(fd);
    return false;
  }

  char chunk[64 * 1024];
  ssize_t n;
  while ((n = read(local_fd, chunk, sizeof(chunk))) > 0) {
    uint32_t chunk_len = (uint32_t)n;
    if (!write_all(fd, "DATA", 4) || !write_all(fd, &chunk_len, 4) ||
        !write_all(fd, chunk, chunk_len)) {
      close(local_fd);
      close(fd);
      return false;
    }
  }
  close(local_fd);

  uint32_t now = (uint32_t)time(nullptr);
  if (!write_all(fd, "DONE", 4) || !write_all(fd, &now, 4)) {
    close(fd);
    return false;
  }

  char status[4];
  bool ok = (read(fd, status, 4) == 4 && memcmp(status, "OKAY", 4) == 0);
  close(fd);
  return ok;
}

static bool adb_exec_sync(const char *cmd) {
  int fd = adb_connect_server();
  if (fd < 0) {
    return false;
  }

  if (!adb_send_cmd(fd, "host:transport-any")) {
    close(fd);
    return false;
  }

  char service[256];
  snprintf(service, sizeof(service), "shell:%s", cmd);
  if (!adb_send_cmd(fd, service)) {
    close(fd);
    return false;
  }

  char buf[64];
  while (read(fd, buf, sizeof(buf)) > 0) {
  }

  close(fd);
  return true;
}

static int adb_exec_spawn(const char *cmd) {
  int fd = adb_connect_server();
  if (fd < 0) {
    return -1;
  }

  if (!adb_send_cmd(fd, "host:transport-any")) {
    close(fd);
    return -1;
  }

  char service[256];
  snprintf(service, sizeof(service), "shell:%s", cmd);
  if (!adb_send_cmd(fd, service)) {
    close(fd);
    return -1;
  }

  return fd;
}

static bool adb_setup_forward(int local_port, int remote_port) {
  int fd = adb_connect_server();
  if (fd < 0) {
    return false;
  }

  char cmd[64];
  snprintf(cmd, sizeof(cmd), "host:forward:tcp:%d;tcp:%d", local_port,
           remote_port);

  bool res = adb_send_cmd(fd, cmd);
  close(fd);
  return res;
}

static void setup_abs(int fd, uint16_t code, int32_t min, int32_t max,
                      int32_t flat) {
  ioctl(fd, UI_SET_ABSBIT, code);
  struct uinput_abs_setup s = {
      .code = code,
      .absinfo = {.minimum = min, .maximum = max, .flat = flat},
  };
  ioctl(fd, UI_ABS_SETUP, &s);
}

int create_uinput_device(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    perror("Failed to open /dev/uinput");
    return -1;
  }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  constexpr int buttons[] = {
      BTN_A,   BTN_B,      BTN_X,     BTN_Y,    BTN_TL,     BTN_TR,     BTN_TL2,
      BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
  };
  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
    ioctl(fd, UI_SET_KEYBIT, buttons[i]);
  }

  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  struct {
    uint16_t code;
    int32_t min, max, flat;
  } axes[] = {
      {ABS_X, -127, 127, 15},  {ABS_Y, -127, 127, 15},  {ABS_Z, -127, 127, 15},
      {ABS_RZ, -127, 127, 15}, {ABS_BRAKE, 0, 255, 15}, {ABS_GAS, 0, 255, 15},
      {ABS_HAT0X, -1, 1, 0},   {ABS_HAT0Y, -1, 1, 0},
  };
  for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
    setup_abs(fd, axes[i].code, axes[i].min, axes[i].max, axes[i].flat);
  }

  ioctl(fd, UI_SET_EVBIT, EV_SYN);

  struct uinput_setup usetup = {
      .id = {.bustype = BUS_USB, .vendor = 0x05ac, .product = 0x0223},
      .name = "Forwarded Gamepad",
  };

  if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int main(void) {
  signal(SIGINT, cleanup_and_exit);
  signal(SIGTERM, cleanup_and_exit);

  if ((uinput_fd = create_uinput_device()) < 0) {
    return EXIT_FAILURE;
  }

  if (!adb_push(LOCAL_BIN, REMOTE_BIN, 0755)) {
    fprintf(stderr, "Failed to push grabber binary\n");
    cleanup_and_exit(1);
  }

  if (!adb_setup_forward(FORWARD_PORT, FORWARD_PORT)) {
    fprintf(stderr, "Failed to establish ADB forward rule\n");
    cleanup_and_exit(1);
  }

  adb_exec_sync("cmd display power-off 0");

  char run_cmd[128];
  snprintf(run_cmd, sizeof(run_cmd), "%s %u", REMOTE_BIN, FORWARD_PORT);
  grabber_proc_fd = adb_exec_spawn(run_cmd);
  if (grabber_proc_fd < 0) {
    fprintf(stderr, "Failed to spawn grabber process on phone\n");
    cleanup_and_exit(1);
  }

  char ready_buf[256];
  ssize_t rlen = read(grabber_proc_fd, ready_buf, sizeof(ready_buf) - 1);
  if (rlen <= 0 || strstr(ready_buf, "READY") == nullptr) {
    ready_buf[rlen > 0 ? rlen : 0] = '\0';
    fprintf(stderr, "Grabber failed to start. Remote output: %s\n", ready_buf);
    cleanup_and_exit(1);
  }

  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(FORWARD_PORT),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  int flag = 1;
  setsockopt(data_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  if (connect(data_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("Failed to connect to data socket");
    cleanup_and_exit(1);
  }

  struct input_event ev;
  while (read(data_sock, &ev, sizeof(ev)) == sizeof(ev)) {
    write(uinput_fd, &ev, sizeof(ev));
  }

  cleanup_and_exit(0);
}
