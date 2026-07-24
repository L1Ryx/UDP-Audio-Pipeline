# Release Packaging

Lossy Audio Lab ships as small zip packages for macOS and Windows.

## Release Artifacts

- `Lossy-Audio-Lab-macOS-arm64.zip`: Apple Silicon macOS build with the app binary, docs, and copied Homebrew SDL3/Opus dylibs.
- `Lossy-Audio-Lab-Windows-x64.zip`: Windows x64 build with `lossy_audio_lab.exe`, docs, and copied vcpkg DLLs.

The GitHub Actions workflow also uploads both zips as run artifacts when started manually.

## Creating A GitHub Release

1. Commit the code and push it to GitHub.
2. Create and push a version tag:

```sh
git tag v0.1.0
git push origin v0.1.0
```

3. The `Release Packages` workflow builds macOS and Windows packages.
4. On tag builds, the workflow creates or updates the GitHub release and attaches both zips.

Use `workflow_dispatch` from the GitHub Actions page when you want a test package without creating a release tag.

## Local Sanity Check

```sh
cmake --preset release
cmake --build --preset release --target lossy_audio_lab udp_audio_tests udp_audio_opus_tests
ctest --preset release
./build/release/lossy_audio_lab --headless
```

The app target is `lossy_audio_lab`.
