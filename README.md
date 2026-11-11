# ARM64Bridge

**32-bit Android uygulamalarını, 32-bit desteği olmayan (yalnızca 64-bit) yeni cihazlarda çalıştırmak için köprü.**
*Run 32-bit-only Android apps on 64-bit-only devices.* (English summary at the bottom.)

Yeni telefonların işlemcileri ve Android sürümleri 32-bit (armeabi-v7a) kodu artık hiç çalıştıramıyor; bu yüzden
güncellenmeyen eski uygulama ve oyunlar "uygulama cihazınızla uyumlu değil" hatasıyla kurulamıyor. ARM64Bridge, böyle bir
APK'yı cihazın üzerinde yamalayarak kurulabilir hâle getirir ve içindeki 32-bit yerel kodu bir ARMv7 emülatörü üzerinde
çalıştırır.

> **Durum: erken sürüm (0.1).** Köprünün çekirdeği çalışıyor ve otomatik testlerle doğrulanıyor (aşağıya bakın), ancak
> her uygulamanın çalışması garanti değildir. Özellikle ağır 3B oyunlar için eksik sistem fonksiyonları ve hız sınırları
> var. "Sınırlamalar" bölümünü mutlaka okuyun.

## Nasıl çalışır?

```
Orijinal APK                         Yamalı APK
lib/armeabi-v7a/libgame.so   ──►     lib/armeabi-v7a/libgame.so      (dokunulmadan kalır)
                                     lib/arm64-v8a/libgame.so        (küçük 64-bit "shim")
                                     lib/arm64-v8a/libarm64bridge.so (köprü çalışma zamanı)
```

1. ARM64Bridge uygulaması APK'ya `lib/arm64-v8a/` klasörü ekler: köprü çalışma zamanı ve her 32-bit kütüphane için aynı
   adı taşıyan küçük bir 64-bit *shim*. Android artık uygulamayı normal bir 64-bit uygulama olarak görür ve kurar.
2. Uygulama `System.loadLibrary("game")` çağırdığında Android 64-bit shim'i yükler. Shim, köprüye haber verir.
3. Köprü asıl 32-bit `libgame.so` dosyasını APK'nın içinden okur, 4 GB altındaki bir bellek bölgesine yükler
   (kendi ELF yükleyicisi) ve kodu emüle edilen bir ARMv7 işlemcide ([dynarmic](https://github.com/yuzu-mirror/dynarmic) dinamik yeniden derleyici) çalıştırır.
4. 32-bit kodun çağırdığı sistem fonksiyonları (libc, libm, pthread, liblog, OpenGL ES, EGL, ...) 64-bit gerçek
   karşılıklarına çevrilir. JNI iki yönde de köprülenir: Java → yerel metotlar ve yerel kod → `JNIEnv` çağrıları.

Uygulamanın Java/Kotlin kısmı, kaynakları ve ekranları hiç değişmeden doğal hızda çalışır; yalnızca yerel (C/C++) kod
emüle edilir.

## Kullanım

1. [Releases](../../releases) sayfasından son `arm64bridge-*.apk` dosyasını indirip kurun.
2. **"32-bit APK seç"** ile eski uygulamanın APK dosyasını seçin. Uygulama APK'yı inceler: hangi mimari, kaç yerel
   kütüphane, köprüde henüz olmayan sistem fonksiyonları.
3. **"Yamala ve kur"** (veya dosya olarak almak için "Yamala ve kaydet").
4. **"Köprü öz sınamasını çalıştır"** düğmesi, uygulamayla gelen 32-bit ARM test kütüphanesini cihazınızda köprü üzerinden
   çalıştırır; köprünün o cihazda çalışıp çalışmadığını gösterir.

Notlar:

- Yamalı APK cihazda üretilen bir anahtarla yeniden imzalanır. Bu yüzden orijinal imzaya bağlı şeyler (Google Play
  oturumu, lisans denetimi, uygulama içi satın alma, bazı kurcalama korumaları) çalışmaz.
- Oyunun ek verisi (OBB) varsa onu ayrıca `Android/obb/<paket adı>/` altına koymanız gerekir.
- Android 14+ çok eski `targetSdkVersion` değerine sahip uygulamaları kurmaz. Uygulama gerekirse bu değeri yükseltmeyi
  önerir (işaret kutusu).

## Bir uygulama çalışmazsa

Yamalı her uygulama, her açılışta köprünün günlüğünü telefonun **İndirilenler/ARM64Bridge/** klasörüne yazar
(`arm64bridge-<paket adı>-<tarih>.log`; Android 9 ve öncesinde `Android/data/<paket adı>/files/`). Çökme olursa
dosyanın sonunda hangi 32-bit kodun, hangi sistem fonksiyonunu çağırırken çöktüğü yazar. Sorun bildirirken bu dosyayı ekleyin.

## Sınırlamalar (dürüst liste)

- **Hız:** yerel kod JIT ile emüle edilir ve aynı anda tek iş parçacığı misafir kodu yürütür (atomik işlemlerin doğru
  olması için). Basit/2B oyunlar ve araçlar için yeterli olabilir; ağır 3B oyunlar yavaş kalabilir.
- **Eksik API'ler:** `NativeActivity`, sensör/girdi NDK API'leri, ELF TLS, ağ adı çözümleme
  (`getaddrinfo`) ve OpenGL ES 3 henüz köprülenmedi. İnceleme ekranı eksik fonksiyonları listeler.
- `System.load()` ile çalışma anında indirilen kütüphaneler ve doğrudan bellek tamponları (`GetDirectBufferAddress`)
  desteklenmiyor.
- Sinyal işleyicileri ve `fork` yok. Minion Rush gibi büyük oyunların çalışması henüz **doğrulanmadı**; hedef bu, ancak
  yukarıdaki eksiklerin kapatılması gerekiyor.

## Derleme

```bash
# Köprü testleri (masaüstü, gerçek 32-bit ARM kodu emülatörde çalışır)
core/test/run-host-test.sh

# Android uygulaması (Android SDK + NDK 27 gerekir)
./gradlew assembleRelease
```

GitHub Actions (`.github/workflows/build.yml`) her gönderimde testleri çalıştırır ve APK'yı derler. Varsayılan dala
yapılan her gönderim ve her `v*` etiketi otomatik olarak bir **GitHub Release** yayınlar (`v0.1.<derleme no>`).

İmzalama: depo ile gelen `app/ci-signing.jks` herkese açık bir CI anahtarıdır (parola `arm64bridge`), yalnızca
sürümlerin birbirinin üzerine güncellenebilmesi içindir. Kendi anahtarınızı kullanmak için depo gizli değişkenlerini
tanımlayın: `A64B_KEYSTORE_BASE64`, `A64B_KEYSTORE_PASSWORD`, `A64B_KEY_ALIAS`, `A64B_KEY_PASSWORD`.

## Depo düzeni

| Yol | İçerik |
| --- | --- |
| `core/src` | Çalışma zamanı: bellek (`guest_mem`), CPU (`cpu`), ELF yükleyici, JNI köprüsü, libc/pthread/GL çeviricileri |
| `core/shim` | Her 32-bit kütüphanenin yerine geçen 64-bit shim |
| `core/guest` | 32-bit ARM test/demo kütüphanesi |
| `core/test` | Masaüstü JVM üzerinde uçtan uca test |
| `app` | Android uygulaması (APK inceleme, yamalama, imzalama, kurma) |

## English summary

ARM64Bridge patches a 32-bit-only (armeabi-v7a / armeabi) APK on the device so that it installs on 64-bit-only Android:
it adds `lib/arm64-v8a/` with a bridge runtime and one tiny shim per original library. At run time the shim makes the
runtime load the untouched 32-bit library from the APK, execute it through an ARMv7 dynamic recompiler (dynarmic) and translate its
libc / pthread / JNI / OpenGL ES calls to the 64-bit host. This is an early release: the core is covered by automated
tests, but large games are not verified yet and several NDK APIs (OpenSL ES, NativeActivity, GLES3) are still missing.

## Lisans

GPL-2.0-or-later. Yalnızca yasal olarak sahip olduğunuz uygulamalarla kullanın.
