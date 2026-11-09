# gpu-rebar-layout

A Linux kernel module that gives selected GPUs their largest Resizable BAR at boot when the BIOS can't,
and pads the PCI bridge windows above them so the kernel can fit them.

I wrote it for my own box: an ASUS TUF X570-Plus with a Broadcom PEX88096 PCIe switch card in its one
x16 slot, and four AMD Radeon AI PRO R9700s (32 GB each) and two RTX 3090s behind it. With all six cards
in, the board hangs at POST if Resizable BAR is on. With it off, Linux 7.0 sized the switch's bridge
windows too small for four 32 GB BARs, and ROCm only turns on peer-to-peer between the R9700s when each
card's whole 32 GB is mapped. The whole story is in
[Fitting Four 32 GB Resizable BARs Behind a PCIe Switch on Linux](https://doug.sh/posts/gpu-bars-behind-a-pcie-switch/).

A few people asked for it, so here it is.

## Read this first

At boot, before your GPU drivers load, this module removes a PCIe root port and everything below it,
then rescans and reassigns resources. On a machine it wasn't written for, that can mean devices that
don't come back or a boot that doesn't finish.

- I've tested it on one machine, the one above, on kernel 7.0.6 (Proxmox VE's 7.0.6-2-pve on Debian 13).
- It uses PCI core internals that can change in any kernel release.
- The padding is a stopgap. Ilpo Järvinen's
  [resource placement fixes](https://lore.kernel.org/r/20261106134341.39493-1-ilpo.jarvinen@linux.intel.com)
  on linux-pci fix the sizing bug properly. With that series on v7.4-rc1, setting the BAR sizes and
  rescanning by hand, all four of my R9700s got their 32 GB BARs without any padding. A board like mine still needs the BAR sizes set and a rescan
  before the GPU drivers load, so on a kernel with that fix, run the module with `pad=0`.
- The module does nothing unless `gpu_rebar_layout.ids=` is on the kernel command line, so deleting that
  parameter at the GRUB menu always gets you a normal boot (see [Turning it off](#turning-it-off)).

If your BIOS can already give every card a large BAR, you don't need this.

## Do you need it?

You might if all of these are true:

- Your GPUs support Resizable BAR, but the BIOS can't turn it on with all of them installed. It hangs, or
  only some cards get large BARs.
- The GPUs sit behind a PCIe switch, or otherwise share a root port, so they all come out of one bridge
  window.
- With large BARs, `dmesg` shows lines like `bridge window [mem ...]: can't assign; no space` for the
  bridges above your cards.
- Nothing but GPUs sits below that root port: no boot disk, no network card, and not the GPU your boot
  console is on.

## What it does

When it loads, for every PCI device whose `vendor:device` is in `ids`, the module:

1. Finds the root port above the device.
2. Checks everything below those root ports, and stops without changing anything if it finds:
   - a device that already has a driver bound, other than `pcieport`. That's why it has to load from the
     initramfs, before `amdgpu`, `nvidia` and the rest;
   - the boot display, the GPU your firmware console is drawn on;
   - anything that isn't a GPU, a GPU's audio function or the switch's own management endpoint, like a
     disk or a NIC, unless you set `allow_other_devices=1`.
3. Sets BAR0's Resizable BAR size to the largest size the card supports, capped by `max_log2_mb`
   (default 15, which is 32 GB). It reads the register back to check, and skips cards that are already
   at that size. If every card is already sized and has its BARs assigned, it logs `nothing to do` and
   stops.
4. Removes the root ports and rescans their buses. Between the scan and resource assignment, it:
   - removes the GPUs' HDMI/DP audio functions, if you set `drop_gpu_audio=1` (more below);
   - raises the start alignment of every smaller memory BAR and expansion ROM on each bus segment to
     match the largest BAR of the same kind on that segment (64-bit prefetchable, or everything else), unless you set `pad=0`.
5. Assigns resources and adds the devices back, so the GPU drivers bind later as usual.
6. Checks that every device came back with all its memory BARs assigned, and logs an error naming any
   that didn't.

The padding is the workaround for the bug. Linux sizes a bridge window by adding up its children's BARs.
When a 32 GB BAR is followed by a small one, like the R9700's 2 MB doorbell BAR, the next card's 32 GB BAR
still has to start on a 32 GB boundary, so the cards take more space than the sum. Aligning each smaller
BAR to the largest one on its segment makes the sum come out right. On my box each R9700 ends up taking
64 GB of address space, and each 3090 512 MB.

The padding covers the non-prefetchable BARs too, which live below 4 GB. Each card's register BAR gets
aligned to the largest one on its segment, so a switch full of cards uses more of that space than before.

### Removing the GPU audio functions

Most GPUs have an HDMI/DP audio function next to them, with a small BAR of its own. With
`drop_gpu_audio=1`, the module clears those BARs during the rescan and removes the audio functions before
any driver sees them, so they take no address space. HDMI and DisplayPort audio from those cards stops
working until something triggers another PCI rescan.

It's off by default. I run my box with it on, since I don't use the cards' audio. Try it if the module
logs unassigned BARs with it off.

## Requirements

- Linux 7.0. I've built and run it on 7.0.6, and other versions may not build.
- Kernel headers for your running kernel (`proxmox-headers-$(uname -r)` on Proxmox,
  `linux-headers-$(uname -r)` on Debian and Ubuntu), `build-essential`, and `dkms`.
- initramfs-tools (Debian, Ubuntu, Proxmox). The two scripts here only cover initramfs-tools, so with
  dracut or mkinitcpio you'll need your own way to load the module early.
- Above 4G Decoding on in the BIOS, and enough 64-bit address space above your RAM for the BARs you're
  asking for. Each 32 GB BAR can end up costing 64 GB of it.
- If Secure Boot is on, the module has to be signed with a key your machine trusts. DKMS can sign it with
  its MOK key on most distributions. An unsigned module fails to load, and since the initramfs script
  ignores load errors, the only sign is that nothing changed.

## Build and install

Find the `vendor:device` IDs of the cards you want to resize:

```sh
lspci -nn | grep -Ei 'vga|3d|display'
```

The R9700 shows up as `[1002:7551]`.

Build and install it with DKMS:

```sh
make                                     # optional: check it builds against the running kernel
sudo mkdir -p /usr/src/gpu-rebar-layout-1.1
sudo cp gpu_rebar_layout.c Makefile dkms.conf /usr/src/gpu-rebar-layout-1.1/
sudo dkms add gpu-rebar-layout/1.1
sudo dkms install gpu-rebar-layout/1.1 -k "$(uname -r)"
```

Add it to the initramfs so it loads before udev binds any GPU driver:

```sh
sudo install -m755 initramfs-hook /etc/initramfs-tools/hooks/gpu-rebar-layout
sudo install -m755 initramfs-init-top /etc/initramfs-tools/scripts/init-top/00-gpu-rebar-layout
sudo update-initramfs -u
```

Try it for one boot first. Press `e` at the GRUB menu, add `gpu_rebar_layout.ids=1002:7551` (with your
IDs) to the end of the `linux` line, and boot with Ctrl-X. If it works, add it to
`GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub` and run `sudo update-grub`.

Mine has:

```
gpu_rebar_layout.ids=1002:7551 gpu_rebar_layout.drop_gpu_audio=1 amdgpu.runpm=0
```

## Parameters

| Parameter | Default | What it does |
|---|---|---|
| `ids` | empty, so it does nothing | Comma-separated `vendor:device` list in hex, e.g. `1002:7551`. Up to eight IDs and sixteen matching cards |
| `max_log2_mb` | `15` | Largest BAR0 size as log2 of MB: 15 is 32 GB, 14 is 16 GB |
| `pad` | `1` | Pads smaller BARs to the largest BAR on their bus segment. Set `0` on a kernel with the upstream fix |
| `drop_gpu_audio` | `0` | Removes the HDMI/DP audio functions of the GPUs below the rescanned root ports |
| `allow_other_devices` | `0` | Rescans even if something other than GPUs shares the root port |

## Checking it worked

```sh
journalctl -k -b | grep -E 'gpu_rebar_layout|BAR0|aligned to|re-enumeration|not assigned|Nothing changed'
sudo lspci -vvs <gpu address> | grep -E 'Region 0|current size'
```

A good boot logs the BAR0 size change for each card, the alignment changes, and a line starting with
`gpu_rebar_layout: re-enumeration done`, and `lspci` shows the new size on `Region 0`. If something didn't
fit, the module logs `re-enumeration finished with problems` and each BAR that didn't get an address.

Some `can't assign` messages are harmless. My box logs one for a bridge window above a 3090 and one for
that 3090's expansion ROM, but every real BAR gets assigned and the NVIDIA driver doesn't need the ROM.
The module's own check ignores expansion ROMs.

## Known issues

With the R9700s' audio functions removed, the cards went into their low-power state (BACO) about 90
seconds after boot and never came back (`resume of IP block <smu> failed -62`), so ROCm had no GPUs. That
same build also started padding the cards' expansion ROMs, which the module still does, and I never
worked out which change caused it.
`amdgpu.runpm=0` on the command line avoids it, but the cards no longer power down when idle.

Once the root ports are removed, the module can't put things back the way they were, so if the rescan
goes wrong, reboot without the parameter. Unloading the module doesn't undo anything either,
since all the changes happen once, at load time.

## Turning it off

For one boot, press `e` at the GRUB menu, delete `gpu_rebar_layout.ids=...` from the `linux` line, and
boot with Ctrl-X. Without `ids`, the module loads and does nothing.

To turn it off for good, remove the parameter from `/etc/default/grub` and run `sudo update-grub`. To
remove the module entirely:

```sh
sudo rm /etc/initramfs-tools/hooks/gpu-rebar-layout \
        /etc/initramfs-tools/scripts/init-top/00-gpu-rebar-layout
sudo dkms remove gpu-rebar-layout/1.1 --all
sudo update-initramfs -u
```

## Related

- [Fitting Four 32 GB Resizable BARs Behind a PCIe Switch on Linux](https://doug.sh/posts/gpu-bars-behind-a-pcie-switch/),
  my write-up of the bug and this module
- [Six GPUs on One x16 Slot](https://doug.sh/posts/six-gpus-one-x16-slot/), the whole build
- [PCI: bridge window undersized when child bridge windows are larger than their alignment](https://lore.kernel.org/linux-pci/20260903063124.9316-1-nbritton@exabit.io/),
  the regression report on linux-pci
- [PCI: Resource placement algorithm fixes (v5)](https://lore.kernel.org/r/20261106134341.39493-1-ilpo.jarvinen@linux.intel.com),
  the upstream fix

## License

GPL-2.0-only. See [LICENSE](LICENSE).
