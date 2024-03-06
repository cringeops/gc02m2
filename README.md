# gc02m2
a rookie attempt to bring gc02m2 image sensor support for PineTab 2

## What is it?

This is the only "documentation" we have for the gc02m2 image sensor module used in PineTab 2 as a frontal camera.
It has been forked from Rockchip repository, stripped of some RK stuff, and shaped enough to be compiled against kernel 6.6.

Feel free to contact me on Matrix at @cringeops:matrix.org.

## TODO
- Find out why the sensor is not powering up properly <-- we are here
- Bring 2-lane CSI support for RK devices (good luck with that)
- [Make use of frame descriptors](https://patchwork.kernel.org/project/linux-media/patch/20220103162414.27723-8-laurent.pinchart+renesas@ideasonboard.com/)
- Remove all RK-specific definitions
- Groom the code well enough to be submitted into the mainline

## How to build it?

0. Install aarch64 cross-compiler on your build machine, i.e.
```
$ sudo apt install gcc-aarch64-linux-gnu
```

1. Fetch dependencies
```
$ git clone https://github.com/cringeops/gc02m2
$ git clone --depth 1 --branch v6.6.13-danctnix1 https://github.com/dreemurrs-embedded/linux-pinetab2`
$ git clone https://github.com/dreemurrs-embedded/Pine64-Arch`
```

3. Build the kernel with custom DTS
```
$ cp Pine64-Arch/PKGBUILDS/pine64/linux-pinetab2/config linux-pinetab2/.config
$ cp ../gc02m2/dts/rk3566-pinetab2.dtsi arch/arm64/boot/dts/rockchip/rk3566-pinetab2.dtsi
$ cd linux-pinetab2
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

4. Build the module
```
$ cd gc02m2
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -C ../linux-pinetab2 M=$PWD modules
```

## How to use it?

0. Deliver these build atrifacts to PineTab
  - `linux-pinetab2/arch/arm64/boot/dts/rockchip/rk3566-pinetab2-v0.1.dtb`
  - `linux-pinetab2/arch/arm64/boot/dts/rockchip/rk3566-pinetab2-v2.0.dtb`
  - `gc02m2/gc02m2/gc02m2.ko`

1. Substitute the DTBs on PineTab
```
$ sudo cp rk3566-pinetab2-v0.1.dtb /boot/dtbs/rockchip
$ sudo cp rk3566-pinetab2-v2.0.dtb /boot/dtbs/rockchip
```

2. Reboot PineTab
3. Load the module
```
$ sudo modprobe --force-vermagic ./gc02m2.ko
```
