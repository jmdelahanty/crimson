# Crimson macOS Internal Testing

This build is for trusted internal testing. It has an ad-hoc signature, not an
Apple Developer ID signature, and has not been notarized by Apple.

## Requirements

- Apple Silicon Mac (`uname -m` prints `arm64`)
- macOS 15.5 or newer (`sw_vers -productVersion`)
- Homebrew installed under `/opt/homebrew`
- Access to the complete recording folder, including both `zarr/` and `cams/`

Install the one current external runtime dependency:

```bash
brew install glfw
```

## Install And Open

Keep the ZIP and `.sha256` file together and verify the transfer before
expanding it:

```bash
shasum -a 256 -c Crimson-*.zip.sha256
```

1. Expand the verified ZIP and move `Crimson.app` to `/Applications`.
2. In Finder, Control-click `Crimson.app` and choose **Open**.
3. Confirm **Open** in the security dialog.
4. If macOS still blocks it, use **System Settings > Privacy & Security > Open
   Anyway**. Do not disable Gatekeeper globally.

The first launch can also be run from Terminal to retain diagnostic output:

```bash
/Applications/Crimson.app/Contents/MacOS/Crimson
```

## Open A Recording

1. Mount the lab filesystem or make the complete recording folder available.
2. In Crimson, choose **File > Load Zarr Archive...**.
3. Select the recording's analysis `.zarr` directory.

Crimson reads the acquisition-video contract from the archive and resolves its
recording-relative camera path. The mount prefix may differ between Macs, but
the recording's relative `zarr/` and `cams/` layout must remain intact.

If the archive, camera video, or network mount cannot be opened, Crimson keeps
the existing session open and displays the error.

## Report A Problem

Include these details with a failure report:

```bash
uname -m
sw_vers
brew --prefix glfw
```

Also include the selected Zarr path, the exact on-screen error, and the Terminal
output from launching Crimson with the command above.
