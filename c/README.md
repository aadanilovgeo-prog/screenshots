# scroll_capture (C)

## Download

```
c/dist/scroll_capture_v1.0.2.exe
```

## Safe Stitch

Default mode: content shift is detected from frame pairs (not from wheel_notches × px).
When uncertain, crop is reduced to avoid losing table rows or image content.

## Local build

```bat
cd c
build.bat
```

## Debug

```bat
scroll_capture_v1.0.2.exe --save-frames ./debug
```

Saves frames, seam previews, and `stitch_log.txt`.

## Output

`long_screenshot_YYYY-MM-DD_HH-MM-SS.png`
