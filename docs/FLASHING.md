# Flashing the OS to an SD card

The Cardputer Zero boots from a microSD card. The Lite ships without one, so this is the first
thing to do. You need a microSD card of **16 GB or more**, a card reader, and a computer.

There are three systems you can put on the card. Start with the first.

| | What | When to use it |
| --- | --- | --- |
| **A. M5Stack's stock image** | The official system. Known to boot. ZeroOS RF installs on top of it. | First boot of a new unit, and whenever you want a known-good fallback. |
| **B. The ZeroOS image** | Our own base OS, built by the `cardputer-zero` repository. | Once that image has been seen to boot. **It has not been tried on hardware yet.** |
| **C. The ZeroOS RF image** | The ZeroOS image with the RF home screen and radio programs already installed. Nothing to install afterwards. | Once B is known to boot. **Never built to completion or booted yet.** |

After A or B, [install the RF kit](#3-install-zeroos-rf) and run the first-run check. After C, go
straight to the check.

Nothing on this page has been tried by us on a real unit. The stock steps follow M5Stack's own
[image guide](https://docs.m5stack.com/en/compute/cardputerzero/image) and
[usage guide](https://docs.m5stack.com/en/compute/cardputerzero/usage); if they disagree with this
page, they are right.

## A. Stock image (recommended)

1. **Get M5 Imager**, M5Stack's flashing tool, from its
   [releases page](https://github.com/CardputerZero/m5stack-imager/releases) and install it.
2. Put the microSD card in the reader. Everything on it will be erased.
3. Open M5 Imager, choose **CardputerZero** as the device, and choose the newest image version.
   The same image serves the standard model and the Lite.
4. Choose the card as the target. **Check it is the card and not another drive.**
5. In the settings step, fill in:
   - **Username and password.** Use `pi` as the username if you have no preference; the commands
     in these docs are written for it. Any name works: replace `pi` with yours.
   - **Wi-Fi** name and password.
   - **Enable SSH.** The RF kit is copied over and checked through SSH.
6. Start the write and wait for it to finish and verify. Eject the card.
7. With the Cardputer **switched off**, insert the card. Slide the power switch **to the right**.
   The first boot takes longer than later ones.
8. If you skipped the settings in step 5, the device shows a setup wizard for the username, network
   and SSH; finish it and restart.
9. **Find the device's address**: open the *IP Panel* or *Settings* on the device and note the IP
   address (for example `192.168.1.57`).
10. From the computer, check you can log in (use your address):

    ```bash
    ssh pi@192.168.1.57
    ```

To switch off: slide the power switch **to the left**. The system shuts down, then power is cut.

**Without M5 Imager.** The images are also published as `.img.xz` files on the
[pi-gen releases page](https://github.com/CardputerZero/pi-gen/releases). Those can be written
with Raspberry Pi Imager (*Choose OS → Use custom*) or balenaEtcher. You then set the username,
Wi-Fi and SSH in the wizard on the device at first boot.

## B. ZeroOS image (untested on hardware)

1. In the `cardputer-zero` repository on GitHub, open **Actions → Build image → Run workflow**.
   Set the Wi-Fi country code. The build takes 1.5 to 3 hours.
2. When it finishes, download the image artifact from the run's page and unzip it. Inside is a file
   named like `2026-11-19-zeroos-cardputerzero-arm64.img.xz`.
3. Write it to the card with **Raspberry Pi Imager**: *Choose OS → Use custom*, pick the `.img.xz`,
   pick the card. When asked to customise, set the Wi-Fi name and password there; the image applies
   them on first boot.
4. Insert the card with the Cardputer off, then switch on.

Defaults in this image: username `pi`, password `raspberry`, hostname `cardputer`, SSH on. Change
the password after logging in (`passwd`).

```bash
ssh pi@cardputer.local
```

If the screen stays blank or it doesn't join Wi-Fi, go back to the stock card: this image's first
boot is exactly what is still unproven.

**From a Linux terminal instead of an imager.** Find the card's device name with `lsblk` first
(for example `/dev/sdX`); writing to the wrong one destroys that disk.

```bash
xzcat 2026-11-19-zeroos-cardputerzero-arm64.img.xz | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
```

## C. ZeroOS RF image (never booted)

The same as B, built from this repository, with everything in the kit already installed: the RF
home screen, the radio programs, the SDR packages and the dongle access rules.

1. In this repository on GitHub, open **Actions → Build image → Run workflow** and set the Wi-Fi
   country code. It takes 1.5 to 3 hours. It needs access to the base repository (the
   `BASE_REPO_TOKEN` secret, unless the base is public).
2. Download the `zeroos-rf-image` artifact from the run and unzip it. Inside is a file named like
   `2026-11-20-zeroos-rf-cardputerzero-arm64.img.xz`.
3. Write it to the card and boot exactly as in B. The defaults are the same: username `pi`,
   password `raspberry`, hostname `cardputer`, SSH on.
4. Plug in the dongle and run `zeroos-rf-bringup`. There is no installer step.

To build it on your own machine instead (Docker, about 20 GB of disk, an arm64 build of the
programs first):

```bash
image/build.sh build/device
```

## 3. Install ZeroOS RF

The kit is one file, `zeroos-rf-arm64.tar.gz`: the RF home screen, the radio programs, an
installer and a first-run check. Get it from the newest successful **CI** run on this repository
(*Actions → CI → the run → Artifacts → zeroos-rf-arm64*), or build it as the main README describes.
A kit built by CI without access to the base repository holds the radio programs but not the home
screen; the installer says which it is.

Copy it to the device (use your address and username):

```bash
scp zeroos-rf-arm64.tar.gz pi@192.168.1.57:
```

Then, on the device (over SSH):

```bash
tar xzf zeroos-rf-arm64.tar.gz
```

```bash
sudo zeroos-rf/install.sh
```

The installer needs the internet. Plug in the dongle and antenna, then run the check:

```bash
zeroos-rf-bringup
```

[BRINGUP.md](BRINGUP.md) explains what the check does, what to send back, and what to try when
something fails.

## Trouble with the card

| What you see | Try |
| --- | --- |
| Nothing on the screen after switching on | Card not fully inserted, or the write failed: write it again and let the imager verify. Try another card; cheap cards fail often. |
| It boots but you can't find it on the network | Wi-Fi name or password mistyped in the imager, or a 5 GHz-only network (the Cardputer is 2.4 GHz only). Use the wizard or Settings on the device to join again. |
| `ssh` says connection refused | SSH wasn't enabled: turn it on in the device's Settings, or re-flash with it enabled. |
| `ssh` warns the host key changed | You re-flashed the card. Remove the old key with `ssh-keygen -R <address>` and connect again. |
| The card shows as two small drives on the computer afterwards | Normal: a boot partition and a Linux partition. |
