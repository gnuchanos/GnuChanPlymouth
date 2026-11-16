# GnuChanPlymouth

GnuChanPlymouth draws the GnuChanOS artwork and a six-second animation through
DRM/KMS. It starts after kernel graphics modules and the live-ISO GPU selector,
then restores the previous display mode before GnuChanDM starts. It does not
load, unload, blacklist, or defer a graphics driver.

The Intel, AMD, and NVIDIA profiles live in `src/config/`. Each profile selects
the DRM device, wait period, and animation settings; GPU detection prefers the
display marked as the PCI boot display and tries other DRM cards if necessary.
An explicit `gnuchan.gpu=` boot-menu selection takes precedence. The live
selector tries `amdgpu` and verifies it bound before falling back to `radeon`,
which covers older Radeon cards; unsupported NVIDIA proprietary branches fall
back to the kernel's `nouveau` driver.

Build and install on Debian with the PNG development package installed:

```sh
python3 makefile.py build
sudo python3 makefile.py install
```

`GNUCHANOS_ASSETS` can point at the distribution's `assets/` directory, and
`DESTDIR` can be used to stage an install. `GnuChanPlymouth --probe-drm` lists
connected DRM outputs without changing display modes.

On an existing system, disable the initramfs Plymouth splash before rebooting;
otherwise Plymouth retains DRM master and GnuChanPlymouth cannot present its
image:

```sh
sudo sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"/GRUB_CMDLINE_LINUX_DEFAULT="quiet nosplash"/' /etc/default/grub
sudo update-grub
```

Confirm `/etc/default/grub` contains `quiet nosplash` and that the generated
kernel command line includes `nosplash` before rebooting. The GnuchanOS
installer sets `nosplash` automatically on newly installed systems.
