# ARM64Bridge

**Run 32-bit-only Android apps on 64-bit-only devices.**

Recent phones can no longer execute 32-bit (armeabi-v7a) code at all, so old apps and games that were never
updated fail to install ("app not compatible with your device"). ARM64Bridge patches such an APK on the device so
that it installs, and runs its 32-bit native code through an ARMv7 dynamic recompiler.

> **Status: early.** The core is covered by automated tests and a few games have been run on a real device
> (see below), but any given app may still fail. Read the limitations.

## How it works

```
Original APK                         Patched APK
lib/armeabi-v7a/libgame.so   ──►     lib/armeabi-v7a/libgame.so      (untouched)
                                     lib/arm64-v8a/libgame.so        (tiny 64-bit shim)
                                     lib/arm64-v8a/libarm64bridge.so (bridge runtime)
```

1. The ARM64Bridge app adds a `lib/arm64-v8a/` directory to the APK: the bridge runtime plus, for every 32-bit
   library, a small 64-bit *shim* with the same file name. Android now treats the app as a normal 64-bit app.
2. When the app calls `System.loadLibrary("game")`, Android loads the shim, which hands over to the runtime.
3. The runtime reads the real 32-bit `libgame.so` from the APK, loads it below 4 GiB with its own ELF loader and
   executes it with [dynarmic](https://github.com/yuzu-mirror/dynarmic), which recompiles ARMv7 code to ARM64.
4. Calls from the 32-bit code into the system (libc, libm, pthreads, liblog, zlib, EGL, OpenGL ES 1/2, OpenSL ES,
   assets, sensors...) are translated to the real 64-bit functions. JNI is bridged in both directions.

The app's Java/Kotlin code, resources and UI run natively and unchanged; only native (C/C++) code is recompiled.

## Usage

1. Install the latest `arm64bridge-*.apk` from [Releases](../../releases).
2. **Choose an APK…** The app shows the architecture, the native libraries and any system functions the
   bridge does not provide yet.
3. **Patch & install** (or **Patch & save…** to get the patched APK as a file).
   APKs that already have 64-bit libraries (or no native code at all) are not converted: only the
   customizations you select are applied (**Apply & install** / **Apply & save…**).
4. **Run bridge self test** runs a bundled 32-bit ARM test library through the bridge on your device.

Options shown when applicable:

- **Raise targetSdkVersion**: recent Android versions refuse to install apps that target very old SDKs.
- **Low-resolution rendering** (optional): runs the app in Android's screen compatibility mode
  (`anyDensity="false"`). Use it for very old games that draw tiny graphics on a modern high-resolution screen.

- **FPS display** (always, for apps that get the bridge): a small floating button inside the game shows the frame
  rate (no overlay permission needed). Tap it for the menu, drag it to move it, hold it to open this project page.
- **FPS booster** (optional): adds a switch to that menu: off (the game's own limit), 60 FPS or unlimited (the
  display still paces frames). Games whose frame limiter waits on the clock get a new frame time; the bridge
  recognises the Gameloft engine limiters (Minion Rush and other Gameloft games of that time) by their code. For other
  games the render thread's sleeps are halved or skipped. If a game ties its speed to the frame rate it runs too
  fast with the booster on; switch it off in the same menu. The log also records where a game's limiter is
  ("frame limiter probe"), which helps adding support for more engines.

- **Value editor** (optional, works on any APK): adds a tool to the in-game menu that finds a number in the
  game's memory (coins, lives, …) and changes or freezes it: search the value the game shows, change it in the game,
  refine with the new value, and repeat until a few results are left. It runs inside the game's own process and
  uses `/proc/self/mem`, so it needs no root, and it works in bridged 32-bit games and in native 64-bit games
  (those get the floating menu with only this entry). Types: 8/16/32/64-bit integers, float, double. Values that a
  game keeps on its server cannot be changed.

Customization (optional, works on any APK):

- **App name**: the name shown in the launcher.
- **Package name**: installs the app as a separate copy next to the original. Relative component names are made
  absolute, and the app's own content provider authorities and permissions are moved to the new name. Saved data
  and OBB files belong to the package name, so they have to be moved to the new name as well.
- **Permissions**: untick a permission to remove it, or add new ones (suggestions are offered while typing).
- **Screen orientation**: force landscape, portrait or any orientation for all activities.
- **Debuggable**: lets `adb shell run-as` and debuggers attach.
- **Extract native libraries**: makes the installer unpack the `.so` files to disk (helps some old apps).
- **OBB and data files**: add `.obb` files and/or game data (a ZIP or a folder with the contents of
  `Android/data/<package>/`). They are embedded in the APK together with a small content provider that copies them
  to the app's own `Android/obb/<package>/` and `Android/data/<package>/` the first time it starts (apps may always
  write their own folders, so no storage permission is needed and nothing has to be copied by hand). The first start
  takes a little longer, and the files take their size twice (inside the installed APK and in the copied folders).
  When the package name is changed, OBB file names are renamed to match.

The **About** entry in the menu shows the version and links to this repository.

Notes:

- The patched APK is re-signed with a key generated on the device. Anything tied to the original signature
  (Google Play sign-in, licence checks, in-app purchases, some tamper checks) will not work, and Play Protect
  may warn about the patched app.
- Expansion data (OBB) and game data can be embedded with the **OBB and data files** option, or copied to
  `Android/obb/<package>/` and `Android/data/<package>/` by hand.
- After updating ARM64Bridge, patch the app again: the runtime is copied into the patched APK.

## Tested on a device

| App | Result |
| --- | --- |
| Minion Rush 5.7.0h (Gameloft) | Starts and plays at the game's own 30 fps cap |
| Angry Birds (early version) | Starts and plays, with sound |

## When an app does not work

Every patched app writes the bridge log to **Download/ARM64Bridge/** on each start
(`arm64bridge-<package>-<time>.log`; on Android 9 and older: `Android/data/<package>/files/`). After a crash the
end of the file shows where the 32-bit code was and which system functions it called last. Attach it to bug reports.

## Limitations

- **Speed**: native code is recompiled, not run directly; expect a fraction of native speed.
- **Not bridged yet**: `NativeActivity`, OpenGL ES 3, Vulkan, ELF TLS, input/camera/media NDK APIs.
- Libraries loaded at run time with `System.load()` from the app's data directory, direct `ByteBuffer`s
  (`GetDirectBufferAddress`), signal handlers and `fork` are not supported.
- Only ARM guests (armeabi-v7a / armeabi) on ARM64 hosts.

## Building

```bash
# Bridge tests: real 32-bit ARM code run through the bridge under a desktop JVM
# (needs cmake, ninja, clang with ARM target, lld, a JDK, zlib and Boost headers)
core/test/run-host-test.sh

# Android app (needs the Android SDK and NDK 27)
./gradlew assembleRelease
```

GitHub Actions (`.github/workflows/build.yml`) runs the tests on x86_64 and ARM64 hosts and builds the APK on every
push. Every push to the default branch and every `v*` tag publishes a GitHub release (`v0.1.<build number>`).

Signing: `app/ci-signing.jks` in this repository is a public CI key (password `arm64bridge`); it only exists so
that releases can update each other. To sign with your own key, define the repository secrets
`A64B_KEYSTORE_BASE64`, `A64B_KEYSTORE_PASSWORD`, `A64B_KEY_ALIAS` and `A64B_KEY_PASSWORD`.

## Repository layout

| Path | Contents |
| --- | --- |
| `core/src` | Runtime: guest memory, CPU (dynarmic), ELF loader, JNI bridge, libc/pthread/GL/audio thunks |
| `core/shim` | The 64-bit shim that stands in for each 32-bit library |
| `core/guest` | 32-bit ARM test/demo library |
| `core/test` | End-to-end test under a desktop JVM |
| `app` | Android app: APK analysis, patching, signing, installing |

## License

GPL-2.0-or-later. Only use it with apps you are legally entitled to use.
