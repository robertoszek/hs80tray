# hs80tray

Tray indicator for Corsair HS80 Headset

![Tray indicator](tray.png)

You can find this project at: 

|                          GitHub                           |                          Gitlab                           |                                Gitea                                 |
|:---------------------------------------------------------:|:---------------------------------------------------------:|:--------------------------------------------------------------------:|
| [hs80tray](https://github.com/robertoszek/hs80tray) | [hs80tray](https://gitlab.com/robertoszek/hs80tray) | [hs80tray](https://gitea.robertoszek.xyz/robertoszek/hs80tray) |


## Features

- Battery level
- Charging state
- Auto switching default output when connecting headset

## Installing
### Using a package manager
Here's a list of the available packages:

| Package type   | Link                                                    | Maintainer                                    |
|:--------------:|:-------------------------------------------------------:|:---------------------------------------------:|
| AUR (Arch)     | https://aur.archlinux.org/packages/hs80tray             | [robertoszek](https://github.com/robertoszek) |

### Building from source
```bash
git clone https://github.com/robertoszek/hs80tray.git
cd hs80tray
cmake .
make
sudo make install
```

Create the file `/etc/udev/rules.d/corsair-hid.rules` with the following contents:
```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1b1c", ATTRS{idProduct}=="0a73", MODE="0666"
```

And reload the rules (or reboot your system):
```bash
$ sudo udevadm control --reload-rules && sudo udevadm trigger 
```

## Running

```bash
$ hs80tray
```

## Usage

```bash
$ hs80tray -h
Usage: hs80tray [options]
Show a battery indicator on the system tray for Corsair HS80 headset
Options:
  -h, --help      Show this help message
  -d, --device    Switch to sink name when connecting headset
  -v, --verbose   Enable verbose debug output
```

### Switching default output

When connecting the headset, it will switch to the provided sink name by using `--device`. For example:
```bash
$ hs80tray -d alsa_output.usb-Corsair_CORSAIR_HS80_RGB_Wireless_Gaming_Receiver_1a23456789012345-00.pro-output-0
```
You can get the name of your sinks by running:
```bash
$ pactl list short sinks
```


#### WIP
Only gets the messages the headset reports back, so it takes a while to display the battery percentage for the first time.

Still trying to decode the captured data between iCUE and the Wireless Receiver, in order to request status report directly instead of waiting for the headset to report by itself a change of state.
