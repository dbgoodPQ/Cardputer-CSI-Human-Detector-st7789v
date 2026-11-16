# LAN time server (offline network)

Lets one machine on a network **without internet** act as the clock for every
other device (the Cardputer, PCs, ...), using standard NTP on **UDP port 123**.
Nothing is downloaded during install: copy this `timeserver` folder to a USB key
and run the installer on the server.

The server hands out **its own clock**, so that clock must be right. Set it by
hand once, then check it now and then (a PC clock drifts a few seconds a month).

## Windows (10 / 11 / Server)

1. Copy the `timeserver` folder from the USB key to the server.
2. Open `windows` and double-click **`install.cmd`**, then accept the admin prompt.
3. The installer:
   - copies the server to `C:\ProgramData\LanTimeServer\`
   - stops and disables the Windows Time service (it holds port 123 and, with
     no internet source, reports "unsynchronized", which ESP32 clients reject)
   - opens UDP 123 inbound in Windows Firewall
   - adds a startup task, **LAN Time Server** (runs as SYSTEM and restarts if it stops)
   - runs a self test and shows the IP address(es) to give your clients
4. Log: `C:\ProgramData\LanTimeServer\server.log`. Each client appears there at most once an hour.

Test from another Windows PC: `powershell -ExecutionPolicy Bypass -File test-timeserver.ps1 -Server <server IP>`
(or `w32tm /stripchart /computer:<server IP> /samples:3 /dataonly`).

Remove: double-click `uninstall.cmd` (this also restores the Windows Time service).

## Ubuntu Server (and other Linux / Raspberry Pi)

Ubuntu Server doesn't mount USB keys automatically:

```sh
lsblk                                   # find the key, e.g. sdb1 (check the size)
sudo mount /dev/sdb1 /mnt
cp -r /mnt/timeserver ~/                # keep a copy on the server
sudo umount /mnt
cd ~/timeserver/linux
sudo sh install.sh
```

Then set the server's clock if it's wrong (the installer prints the current time):

```sh
sudo timedatectl set-timezone America/New_York
sudo timedatectl set-time '2026-11-16 14:30:00'    # local time
```

If `set-time` says "Automatic time synchronization is enabled" (chrony mode, Ubuntu 25.10+), use
`sudo date -s '2026-11-16 14:30:00' && sudo hwclock --systohc` instead. Don't run
`timedatectl set-ntp false` in that mode: it stops chrony, which is your time server.

What the installer does on Ubuntu:
- **24.04 and older** (systemd-timesyncd, no chrony): installs the Python server as
  the service `lan-timeserver` and turns off timesyncd, which has no internet to reach.
  Check it with `systemctl status lan-timeserver` and `journalctl -u lan-timeserver`.
- **25.10 and newer** (chrony is the default): adds the lines below to `/etc/chrony/chrony.conf`.

Details for every distribution:

- If **chrony** is installed, the installer configures it (`local stratum 8`, `allow all`)
  between `# >>> lan-timeserver` markers and saves a backup of the config.
- Otherwise it installs the bundled **Python 3** server (standard library only) as the
  systemd service `lan-timeserver`. Use `sudo sh install.sh --python` to force this mode.
- It opens UDP 123 in ufw or firewalld if one of them is active, then runs a self test.

Test from any machine with Python 3: `python3 ntp_server.py --query <server IP>`

Remove: `sudo sh uninstall.sh`

## NAS

Most NAS boxes (Synology, QNAP, ...) have a built-in "NTP service / act as NTP server"
checkbox in their time settings, so you don't need this package there.

## Clients

- Give each client the server's IP. It's best to reserve that IP in the router's DHCP
  settings so it never changes.
- **Cardputer** (fw 2.6.0-dev5 or later): settings `` ` `` > LOGS > `s` > TIME SERVER, type the
  server's IP and press Enter. The row then shows `ok` once the server has answered. The
  Cardputer asks at start, when the Wi-Fi comes back, and every hour. The day log shows
  `clock set (time server <ip>)`.
- Windows clients: `w32tm /config /manualpeerlist:<server IP> /syncfromflags:manual /update`
  then `w32tm /resync`.

## Files

| File | Purpose |
|---|---|
| `windows/install.cmd`, `uninstall.cmd` | Double-click entry points (ask for admin) |
| `windows/ntp-server.ps1` | The NTP server (PowerShell, no dependencies) |
| `windows/test-timeserver.ps1` | Query a server and show the time offset |
| `linux/install.sh`, `uninstall.sh` | Installer / remover |
| `linux/ntp_server.py` | NTP server and `--query` test client (Python 3 standard library) |
| `linux/lan-timeserver.service` | systemd unit for the Python server |

Replies: NTP v3/v4, stratum 2, refid `LOCL`, leap indicator "no warning". Clients
only accept a server that reports itself as synchronized.
