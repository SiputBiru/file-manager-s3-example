# gui-raygui

A small desktop client for the file-manager API, built with C, raylib, and raygui. It performs the same actions as the HTML page, but as a native window.

## Features

- Drag and drop an image into the window, or use a native file dialog.
- Upload through a presigned URL.
- List thumbnails for the current user.
- Resize to a chosen width.
- Square crop to a chosen size.
- Save the selected image to disk through a native save dialog.
- Delete the selected file.
- Switch backend URL and user ID at runtime.
- Non-blocking HTTP: requests run on a worker thread.

## Requirements

- CMake 3.20 or newer
- A C compiler
- raylib with CMake config files
- libcurl
- Xcode command line tools on macOS

## Build on macOS

```bash
brew install raylib curl cmake
cd gui-raygui
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Build on Linux

```bash
sudo apt install build-essential cmake libraylib-dev libcurl4-openssl-dev
cd gui-raygui
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If CMake cannot find raylib, pass its prefix:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/raylib
```

## Build on Windows

Use MSYS2:

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-raylib mingw-w64-ucrt-x86_64-curl
```

Then run the same CMake commands from the MSYS2 shell.

## Run

Start one backend first, for example the Node app on port 3000. Then:

```bash
./build/gui-raygui
```

The default backend is `http://localhost:3000` and the default user is `demo-user`. You can change both in the window.

Environment variables:

| Variable | Default | Meaning |
| :--- | :--- | :--- |
| `S3_API` | `http://localhost:3000` | Backend base URL |
| `S3_USER` | `demo-user` | Value sent as `x-user-id` |

## Headless Selftest

The selftest runs the API flow without opening a window:

```bash
./build/gui-raygui --selftest /path/to/image.jpg
```

It uploads the image, resizes it to width 320, crops it to 200 x 200, deletes it, and checks the list after each step. Exit code 0 means pass.

Use `S3_API` and `S3_USER` to point it at another backend or user:

```bash
S3_API=http://localhost:3001 S3_USER=tester \
  ./build/gui-raygui --selftest /tmp/test.jpg
```

## Notes

- CORS is not needed. This is a native client, not a browser.
- Resize and crop happen on the backend. The client only displays the results.
- Save downloads the current display image. If the file has been edited, the saved image is the edited result; otherwise it is the original object.
- Thumbnails are decoded with stb_image, because some raylib builds ship without JPEG support. Raylib still handles the window, OpenGL, and textures.
- The HTML page in `public/` remains available. This GUI is an additional client, not a replacement.
- The worker thread downloads thumbnails and runs API calls. Textures are created on the UI thread.

## Vendored Dependencies

See `vendor/README.md`.
