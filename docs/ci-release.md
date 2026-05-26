# CI and Release Setup

This repo uses GitHub Actions for:

- packaged builds on `push` to `main`
- unsigned CI builds on pull requests targeting `main`
- packaged builds and GitHub Releases on tags matching `v*`

The workflow lives at [`.github/workflows/build.yml`](../.github/workflows/build.yml).

## What It Builds

Builds run on:

- macOS
- Linux
- Windows

Push and tag builds package:

- macOS: `.dmg`
- Linux: `.AppImage`
- Windows: `.zip`

macOS signing/notarization and Windows signing are enabled automatically when
the required repository secrets and variables are present. Until then, the
workflow still produces unsigned packages.

## Required GitHub Secrets

Set these repository secrets before testing a signed tagged release:

- `APPLE_CERTIFICATE_P12`
  Base64-encoded Developer ID Application certificate export (`.p12`).
- `APPLE_CERTIFICATE_PASSWORD`
  Password for the `.p12` certificate.
- `APPLE_ID`
  Apple ID email used for notarization.
- `APPLE_APP_PASSWORD`
  App-specific password for notarization.
- `APPLE_TEAM_ID`
  Apple Developer Team ID.
- `AZURE_CLIENT_SECRET`
  Client secret for Azure Trusted Signing.

## Required GitHub Variables

Set these repository variables for Windows signing:

- `AZURE_TENANT_ID`
- `AZURE_CLIENT_ID`
- `AZURE_SIGNING_ENDPOINT`
- `AZURE_CODE_SIGNING_ACCOUNT_NAME`
- `AZURE_CERT_PROFILE_NAME`

## Trigger Behavior

- `push` to `main`
  Builds, packages, and uploads artifacts for macOS, Linux, and Windows.
- `pull_request` to `main`
  Builds the app on macOS, Linux, and Windows and uploads unsigned CI artifacts.
- tag `vX.Y.Z`
  Builds packages on all three platforms and creates a GitHub Release.

## Release Process

1. Ensure the repository secrets and variables above are configured.
2. Push changes to `main` and confirm the normal `Build` workflow passes.
3. Create and push a version tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

4. Wait for the tagged `Build` workflow to complete.
5. Verify the GitHub Release contains:
   `libera-timecode-macos.dmg`, `libera-timecode-linux.AppImage`, and
   `libera-timecode-windows.zip`

## Notes

- App versioning comes from `git describe --tags --abbrev=0`. If no matching
  tag is available, the version falls back to `0.0.0`.
- The workflow uses `fetch-depth: 0` so tags are available during CI.
- Local preset verification command:

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```
