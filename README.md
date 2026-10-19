# CTRLCPY

Low-latency controller relay from an Android phone to a Linux PC over ADB. 

No APK installation required. Minimal overhead, automatic controller discovery, and zero display power draw.

---

## Features

* **Zero Android Install:** Pushes a lightweight standalone ELF binary to `/data/local/tmp` on launch. No sideloading, no background APK services.
* **Low Latency & Minimal CPU Overhead:** Reads raw Linux `evdev` events directly on the device and streams them over a dedicated `TCP_NODELAY` socket to a virtual Linux `/dev/uinput` gamepad.
* **Power Efficient:** Automatically turns the phone's physical display off via ADB while keeping the controller poll loop active.
* **Autonomous Controller Detection:** Scans `/dev/input/event*` nodes dynamically on the phone. Filters out touchscreens and volume keys to attach to your gamepad automatically (Bluetooth or USB-C telescopic grips like GameSir, Kishi, or Backbone).
* **Linux Host Native:** The Lord's operating system.

---

## Requirements

### Host (Linux)
* An active **ADB server** running (`adb` installed and daemon up).
* Permissions to access `/dev/uinput` (either running via `sudo` or adding your user to the `input` / `uinput` group).

### Device (Android)
* **USB Debugging** or **Wireless Debugging** enabled in Developer Options.
* A connected gamepad (Bluetooth or direct USB-C connection).

---

## Setting Up ADB

`ctrlcpy` communicates directly with your local ADB daemon on `localhost:5037`. Make sure your device is authorized before running the program.

### Option A: Wired (USB)
1. Plug your phone into your PC via USB.
2. Ensure USB debugging is enabled on your phone.
3. Verify connection:
   ```bash
   adb devices
    ```

*(Accept the authorization prompt on your phone display if prompted).*

### Option B: Wireless Debugging (Android 11+)

1. Connect your PC and phone to the same Wi-Fi network.
2. In **Settings > Developer Options > Wireless Debugging**, tap **Pair device with pairing code**.
3. Pair once from your PC terminal:
    ```bash
    adb pair <ip_address>:<pairing_port>

    ```
4. Connect to the displayed wireless debugging port:
    ```bash
    adb connect <ip_address>:<port>
    ```

---

## Permissions (`/dev/uinput`)

To run `ctrlcpy` without `sudo`, grant your user access to `/dev/uinput`:

```bash
# Add current user to the input group
sudo usermod -aG input $USER

# Add a udev rule for uinput access
echo 'KERNEL=="uinput", MODE="0660", GROUP="input", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-uinput.rules

# Reload udev rules
sudo udevadm control --reload-rules && sudo udevadm trigger

```

*Log out and log back in for group changes to take effect. Otherwise, run `ctrlcpy` with `sudo`.*

---

## Installation

### From Releases

Download the prebuilt host and target binaries from the **Releases** page:

1. Extract `ctrlcpy` (host) and `grabber` (Android target) into the same directory.
2. Make the host executable:
```bash
chmod +x ctrlcpy

```
3. Run:
```bash
./ctrlcpy
```

---

### Building from Source
The host binary requires a C23-compliant compiler (`gcc >= 14` or `clang >= 18`), while the remote grabber requires an ARM cross-compiler (Android NDK or `aarch64-linux-gnu-gcc`).

#### 1. Compile the Android Grabber

Using the Android NDK (API level 24+):

```bash
$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang \
    -O3 -s grabber.c -o grabber

```

*(Or use a static toolchain like `aarch64-linux-gnu-gcc -O3 -static grabber.c -o grabber`).*

#### 2. Compile the Host

```bash
gcc -std=c23 -O3 ctrlcpy.c -o ctrlcpy
```

Make sure the compiled `grabber` binary sits in the same working directory as `ctrlcpy` when running so it can be pushed automatically.

---

## Usage

1. Connect your controller to your phone (Bluetooth or USB-C).
2. Connect your phone to your PC via USB or Wireless ADB.
3. Run `ctrlcpy`:
```bash
./ctrlcpy

```

`ctrlcpy` will:

* Push `grabber` to `/data/local/tmp/grabber` with executable permissions.
* Setup ADB port forwarding on port `12345`.
* Power down the phone screen.
* Scan and lock onto the controller node.
* Create a virtual gamepad on your Linux machine and stream inputs.

Press `Ctrl+C` to quit. The host cleans up the uinput device, terminates the remote grabber, and releases exclusive controller access.
