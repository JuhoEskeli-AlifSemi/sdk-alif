# Video Capture Sample

Minimal video capture sample for Alif E8-DK. Captures a few frames and prints
buffer addresses for memory dump via debugger.

Based on the working `alif_img_class` camera initialization pattern.

## Supported configurations

| Camera | Mode     | Board              |
|--------|----------|--------------------|
| ARX3A0 | non-ISP  | E8-DK HP           |
| ARX3A0 | ISP      | E8-DK HP           |
| OV5675 | non-ISP  | E8-DK HP           |
| OV5675 | ISP      | E8-DK HP           |

## Build commands

### ARX3A0 (non-ISP)
```
west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp -S ethos-u55-enable \
  samples/drivers/video_capture_new -- \
  -DEXTRA_DTC_OVERLAY_FILE="serial_camera_arx3a0_selfie.overlay serial_camera.overlay"
```

### ARX3A0 (ISP)
```
west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp -S ethos-u55-enable \
  samples/drivers/video_capture_new -- \
  -DEXTRA_DTC_OVERLAY_FILE="serial_camera_arx3a0_selfie.overlay serial_camera_isp.overlay" \
  -DOVERLAY_CONFIG="isp.conf"
```

### OV5675 (non-ISP)
```
west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp -S ethos-u55-enable \
  samples/drivers/video_capture_new -- \
  -DEXTRA_DTC_OVERLAY_FILE="serial_camera_ov5675_selfie.overlay serial_camera.overlay" \
  -DOVERLAY_CONFIG="ov5675.conf"
```

### OV5675 (ISP)
```
west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp -S ethos-u55-enable \
  samples/drivers/video_capture_new -- \
  -DEXTRA_DTC_OVERLAY_FILE="serial_camera_ov5675_selfie.overlay serial_camera_isp.overlay" \
  -DOVERLAY_CONFIG="isp.conf"
```

## Expected output

```
[00:00:00.xxx] <inf> video_capture: Device: <device_name>
[00:00:00.xxx] <inf> video_capture: Pipeline format: 01Y  1296x972
[00:00:00.xxx] <inf> video_capture: Buffer size: 2519424 (pitch=2592 height=972)
[00:00:00.xxx] <inf> video_capture: Buffer[0]: addr=0x02000040 size=2519424
[00:00:00.xxx] <inf> video_capture: Capture started - waiting for 3 frames
[00:00:01.xxx] <inf> video_capture: Frame 0: addr=0x02000040 bytesused=2519424 timestamp=xxx ms
[00:00:01.xxx] <inf> video_capture:   first 16 bytes: xx xx xx xx ...
...
[00:00:03.xxx] <inf> video_capture: Capture complete
```

Use the printed `dump binary memory` commands to extract frames via debugger.

## Inspecting the dumped image

ARX3A0
```
ffplay -f rawvideo -pixel_format gray16le -video_size 560x560   -vf "lut=y='val*65535/1023',format=gray" raw10.bin
```

OV5675
```
fplay -f rawvideo -pixel_format gray16le -video_size 1296x972   -vf "lut=y='val*65535/1023',format=gray" raw_ov10.bin
```