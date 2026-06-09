# updated_screenshoter

Standalone Windows tool for long page screenshots via adaptive scroll + stitch.

**No Python, Node.js, or browser drivers required** — just download and run `updated_screenshoter.exe`.

## Quick start

1. Download `dist/updated_screenshoter.exe`
2. Open the article in VRM/browser, scroll to the top
3. Run:

```bat
updated_screenshoter.exe
```

4. Select capture region (top-left Enter, bottom-right Enter)
5. Switch to article window during countdown
6. Output: `long_screenshot_YYYY-MM-DD_HH-MM-SS.png` next to exe (or Downloads with `--downloads`)

## How it works

- **Adaptive scroll**: step = 72% of capture height (not fixed monitor pixels)
- **Page stability wait**: waits until screen stops changing before capture
- **Overlap search**: compares bottom 38% of previous frame with top of current (skips top 12% for sticky headers)
- **Confidence fallback**: if match is uncertain, uses calculated overlap (prefers no missing text)
- **Up to 600 frames** by default

## Options

```bat
updated_screenshoter.exe --region 100,80,900,700 -o article.png
updated_screenshoter.exe --scroll-fraction 0.70
updated_screenshoter.exe --max-frames 1000
updated_screenshoter.exe --downloads
updated_screenshoter.exe --stable-wait 400
```

## Build from source

```bat
cd updated_screenshoter
build.bat
```

Requires MinGW `gcc` on Windows.

## Note on VRM/RDP

This tool captures the **screen pixels** inside your selected region. It cannot read browser DOM (`scrollY`) directly, but emulates the same logic using:

- capture area height as viewport
- image diff to detect scroll stop
- adaptive overlap matching between frames
