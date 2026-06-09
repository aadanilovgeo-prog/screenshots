# scroll_capture (C)

## Download

```
c/dist/scroll_capture_v1.0.0.exe
```

Filename format: `scroll_capture_v{VERSION}.exe`

## Local build (no version bump)

```bat
cd c
build.bat
```

Builds exe for the **current** version from `/VERSION` without incrementing it.

## Version bump

Version is incremented **automatically only when changes merge to `main`** (GitHub Actions):
1. Bump patch in `VERSION`
2. Build `scroll_capture_vX.Y.Z.exe`
3. Commit and push to `main`

## Run

```bat
scroll_capture_v1.0.0.exe
scroll_capture_v1.0.0.exe --version
```
