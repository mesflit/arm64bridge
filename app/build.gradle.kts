import org.apache.tools.ant.taskdefs.condition.Os

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// CI passes the build number; local builds get 1.
val buildNumber = (System.getenv("A64B_BUILD_NUMBER") ?: "1").toInt()
val baseVersion = "0.1"

android {
    namespace = "com.mesflit.arm64bridge"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.mesflit.arm64bridge"
        minSdk = 26
        targetSdk = 35
        versionCode = buildNumber
        versionName = "$baseVersion.$buildNumber"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release", "-DA64B_VERSION=$baseVersion.$buildNumber")
                targets += listOf("arm64bridge", "a64bshim")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../core/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        create("release") {
            // A real key can be supplied through the environment (see README). Otherwise the
            // public CI key from the repository is used so that every build can update the last.
            val ksPath = System.getenv("A64B_KEYSTORE_FILE")
            storeFile = if (ksPath.isNullOrEmpty()) file("ci-signing.jks") else file(ksPath)
            storePassword = System.getenv("A64B_KEYSTORE_PASSWORD") ?: "arm64bridge"
            keyAlias = System.getenv("A64B_KEY_ALIAS") ?: "arm64bridge"
            keyPassword = System.getenv("A64B_KEY_PASSWORD") ?: "arm64bridge"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    // The patcher copies libarm64bridge.so / liba64bshim.so out of its own native library
    // directory into patched APKs, so they must be extracted at install time.
    packaging { jniLibs { useLegacyPackaging = true } }

    lint { abortOnError = false; checkReleaseBuilds = false }
}

dependencies {
    implementation("com.android.tools.build:apksig:8.7.3")
}

// ---- 32-bit ARM demo library, shipped as an asset and run through the bridge by the self test ----
val guestAssets = layout.buildDirectory.dir("generated/guestAssets")
val buildGuestDemo = tasks.register<Exec>("buildGuestDemo") {
    val src = rootProject.file("core/guest/guestdemo.c")
    val out = guestAssets.map { it.file("guest/libguestdemo.so") }
    inputs.file(src)
    outputs.file(out)
    doFirst {
        val hostTag = when {
            Os.isFamily(Os.FAMILY_WINDOWS) -> "windows-x86_64"
            Os.isFamily(Os.FAMILY_MAC) -> "darwin-x86_64"
            else -> "linux-x86_64"
        }
        val exe = if (Os.isFamily(Os.FAMILY_WINDOWS)) "clang.exe" else "clang"
        val clang = File(android.ndkDirectory, "toolchains/llvm/prebuilt/$hostTag/bin/$exe")
        out.get().asFile.parentFile.mkdirs()
        commandLine(
            clang.absolutePath, "--target=armv7a-linux-androideabi21", "-march=armv7-a", "-mfloat-abi=softfp",
            "-mfpu=neon", "-mthumb", "-O2", "-fPIC", "-shared", "-Wl,--hash-style=both",
            "-o", out.get().asFile.absolutePath, src.absolutePath, "-llog", "-lm", "-lz"
        )
    }
}
android.sourceSets.getByName("main").assets.srcDir(guestAssets)
tasks.named("preBuild") { dependsOn(buildGuestDemo) }
