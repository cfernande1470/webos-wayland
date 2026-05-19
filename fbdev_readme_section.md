## Low-level fbdev experiment

A lower-level display experiment was also performed using Linux fbdev directly, bypassing Wayland and EGL.

This was only a research experiment. It is **not** the recommended display path for this project.

### Devices found

The TV exposed several framebuffer/GPU-related devices:

```text
/dev/fb -> /dev/fb0
/dev/fb0
/dev/fb1
/dev/fb2
/dev/fb3
/dev/galcore
/dev/mali0
```

Framebuffer sysfs reported:

```text
/sys/class/graphics/fb0
  name: osd0_fb
  mode: U:1920x1080p-2
  virtual_size: 1920,2160
  bits_per_pixel: 32
  stride: 7680

/sys/class/graphics/fb1
  name: osd1_fb
  mode: U:512x2160p-4
  virtual_size: 512,4320
  bits_per_pixel: 32
  stride: 2048

/sys/class/graphics/fb2
  name: osd2_fb
  mode: U:128x2p-2691
  virtual_size: 128,4
  bits_per_pixel: 32
  stride: 512

/sys/class/graphics/fb3
  name: crsr_fb
  mode: U:256x256p-69
  virtual_size: 256,512
  bits_per_pixel: 32
  stride: 1024
```

No usable `/dev/dri/card*` DRM/KMS device was found during this test.

### fbdev results

`/dev/fb0` can be opened and memory-mapped. A direct write test succeeded technically:

```text
flash drawn for 2 seconds at 500x300+40+40
restored
```

At first this was barely visible or not visible, but later tests showed that `/dev/fb0` can affect the visible output by causing a screen flash.

`/dev/fb1` also accepted a basic mmap/write/restore test, but no useful visible output was confirmed.

`/dev/fb2` and `/dev/fb3` failed to mmap:

```text
mmap failed: Input/output error
```

`fb3` appears to be a cursor framebuffer (`crsr_fb`), so it was not pursued further.

### fb0 page flipping

`fb0` reports:

```text
xres=1920
yres=1080
xres_virtual=1920
yres_virtual=2160
bits_per_pixel=32
stride=7680
```

This suggests a double-height virtual framebuffer, likely two 1920x1080 pages.

A page-pan test using `FBIOPAN_DISPLAY` worked:

```text
FB /dev/fb0 id=osd0_fb xres=1920 yres=1080 virt=1920x2160 bpp=32 stride=7680 yoffset=0 smem=16973824
PAN to yoffset=1080 for 3 seconds
PAN restore yoffset=0
```

The TV displayed red/green/blue fullscreen test colors, but with visible artifacts. After restoring the original offset, webOS recovered normally.

A later run started with:

```text
yoffset=1080
```

and therefore panned to the same page and restored to the same page:

```text
PAN to yoffset=1080
PAN restore yoffset=1080
```

That run only produced a strange flicker and no useful color output.

### Interpretation

The framebuffer experiment proves that:

```text
/dev/fb0 = visible OSD framebuffer
mmap works
FBIOPAN_DISPLAY works
the TV can show direct fbdev output
```

However, it is not a clean display path:

```text
surface-manager still owns composition
direct fbdev output can flicker
direct fbdev output can show artifacts
alpha / OSD blending may be involved
the current visible page can change under webOS
input and lifecycle are not handled
```

The likely explanation is that `surface-manager` owns the normal display/composition pipeline and may repaint or reuse the OSD framebuffer while the experiment writes or pans it.

### Conclusion

fbdev is lower-level than Wayland/EGL and can touch visible output, but it is unstable and not suitable as the main renderer.

The recommended path remains:

```text
SAM native app
→ Wayland
→ EGL / OpenGL ES
→ Mali-G51 GPU
```

fbdev should remain a documented research experiment only.
