#include <fcntl.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

constexpr char SOCKET_NAME[] = "ctrlcpy";
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

static bool read_all(int fd, void *buf, size_t len) {
  size_t received = 0;
  while (received < len) {
    ssize_t n = read(fd, (char *)buf + received, len - received);
    if (n <= 0) {
      return false;
    }

    received += n;
  }

  return true;
}

static bool adb_okay(int fd) {
  char status[4];
  if (!read_all(fd, status, 4)) {
    return false;
  }

  if (memcmp(status, "OKAY", 4) == 0) {
    return true;
  }

  if (memcmp(status, "FAIL", 4) == 0) {
    char len_hex[5] = {};
    if (read_all(fd, len_hex, 4)) {
      uint32_t msg_len = 0;
      sscanf(len_hex, "%04x", &msg_len);
      char err_msg[256];
      size_t to_read =
          msg_len < sizeof(err_msg) - 1 ? msg_len : sizeof(err_msg) - 1;
      if (read_all(fd, err_msg, to_read)) {
        err_msg[to_read] = '\0';
        fprintf(stderr, "ADB Error: %s\n", err_msg);
      }
    }
  }

  return false;
}

static bool adb_send_req(int fd, const char *service) {
  size_t len = strlen(service);
  if (len > 0xffff) {
    return false;
  }

  char header[5];
  snprintf(header, sizeof(header), "%04zx", len);
  struct iovec iov[2] = {
      {.iov_base = header, .iov_len = 4},
      {.iov_base = (void *)service, .iov_len = len},
  };

  return writev(fd, iov, 2) == (ssize_t)(4 + len) && adb_okay(fd);
}

static int adb_open(const char *service) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(5037),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      !adb_send_req(fd, "host:transport-any") ||
      (service && !adb_send_req(fd, service))) {
    close(fd);
    return -1;
  }

  return fd;
}

static int adb_exec(const char *cmd, bool sync) {
  char service[256];
  snprintf(service, sizeof(service), "exec:%s", cmd);
  int fd = adb_open(service);
  if (fd < 0) {
    return -1;
  }

  if (sync) {
    char buf[128];
    read_all(fd, buf, sizeof(buf));
    close(fd);
    return 0;
  }

  return fd;
}

static bool adb_push(const char *local_path, const char *remote_path,
                     uint32_t mode) {
  int local_fd = open(local_path, O_RDONLY);
  if (local_fd < 0) {
    return false;
  }

  int fd = adb_open("sync:");
  if (fd < 0) {
    close(local_fd);
    return false;
  }

  char target[256];
  int target_len =
      snprintf(target, sizeof(target), "%s,%o", remote_path, mode & 0777);
  uint32_t target_len_le = htole32((uint32_t)target_len);

  struct iovec send_hdr[3] = {
      {.iov_base = (void *)"SEND", .iov_len = 4},
      {.iov_base = &target_len_le, .iov_len = 4},
      {.iov_base = target, .iov_len = (size_t)target_len},
  };
  if (writev(fd, send_hdr, 3) < 0) {
    goto err;
  }

  char chunk[64 * 1024];
  ssize_t n;
  while ((n = read(local_fd, chunk, sizeof(chunk))) > 0) {
    uint32_t chunk_len_le = htole32((uint32_t)n);
    struct iovec data_iov[3] = {
        {.iov_base = (void *)"DATA", .iov_len = 4},
        {.iov_base = &chunk_len_le, .iov_len = 4},
        {.iov_base = chunk, .iov_len = (size_t)n},
    };
    if (writev(fd, data_iov, 3) < 0) {
      goto err;
    }
  }
  close(local_fd);
  local_fd = -1;

  uint32_t now_le = htole32((uint32_t)time(NULL));
  struct iovec done_iov[2] = {
      {.iov_base = (void *)"DONE", .iov_len = 4},
      {.iov_base = &now_le, .iov_len = 4},
  };
  if (writev(fd, done_iov, 2) < 0) {
    close(fd);
    return false;
  }

  bool ok = adb_okay(fd);
  close(fd);
  return ok;

err:
  if (local_fd >= 0) {
    close(local_fd);
  }
  close(fd);
  return false;
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

  adb_exec("cmd display power-off 0", true);

  char run_cmd[128];
  snprintf(run_cmd, sizeof(run_cmd), "%s %s", REMOTE_BIN, SOCKET_NAME);
  grabber_proc_fd = adb_exec(run_cmd, false);
  if (grabber_proc_fd < 0) {
    fprintf(stderr, "Failed to spawn grabber process on phone\n");
    cleanup_and_exit(1);
  }

  char ready_buf[16];
  ssize_t rlen = read(grabber_proc_fd, ready_buf, sizeof(ready_buf) - 1);
  if (rlen <= 0 || strstr(ready_buf, "READY") == nullptr) {
    ready_buf[rlen > 0 ? rlen : 0] = '\0';
    fprintf(stderr, "Grabber failed to start. Remote output: %s\n", ready_buf);
    cleanup_and_exit(1);
  }

  char sock_service[64];
  snprintf(sock_service, sizeof(sock_service), "localabstract:%s", SOCKET_NAME);
  data_sock = adb_open(sock_service);
  if (data_sock < 0) {
    fprintf(stderr, "Failed to connect to socket: %s\n", sock_service);
    cleanup_and_exit(1);
  }

  struct input_event ev;
  while (read(data_sock, &ev, sizeof(ev)) == sizeof(ev)) {
    write(uinput_fd, &ev, sizeof(ev));
  }

  cleanup_and_exit(0);
}
