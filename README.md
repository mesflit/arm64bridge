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
2. **Choose a 32-bit APK…** The app shows the architecture, the native libraries and any system functions the
   bridge does not provide yet.
3. **Patch & install** (or **Patch & save…** to get the patched APK as a file).
4. **Run bridge self test** runs a bundled 32-bit ARM test library through the bridge on your device.

Options shown when applicable:

- **Raise targetSdkVersion**: recent Android versions refuse to install apps that target very old SDKs.
- **Low-resolution rendering** (optional): runs the app in Android's screen compatibility mode
  (`anyDensity="false"`). Use it for very old games that draw tiny graphics on a modern high-resolution screen.

Notes:

- The patched APK is re-signed with a key generated on the device. Anything tied to the original signature
  (Google Play sign-in, licence checks, in-app purchases, some tamper checks) will not work, and Play Protect
  may warn about the patched app.
- Expansion data (OBB) has to be copied to `Android/obb/<package>/` by hand.
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
