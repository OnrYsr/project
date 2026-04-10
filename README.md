# ESP32 Hydroponic Monitor (OLED + TDS/EC + pH)

Bu proje ESP32 ile su kalitesi takibi yapar:
- `TDS (ppm)`
- `EC (uS/cm)`
- `pH`
- OLED ekranda anlik gosterim

Kod dosyasi: `esp32_oled_status.ino`

## WiFi ve web arayuzu

- Varsayilan WiFi bilgileri `esp32_oled_status.ino` icinde (`#else` blogu) tanimlidir; sadece `.ino` kopyalasaniz da baglanir.
- Isterseniz `wifi_secrets.h` ekleyerek bunlari ezer ve sifreyi repodan uzak tutarsiniz (`wifi_secrets.example.h` ornek).
- **Guvenlik:** Repo public ise `.ino` icindeki sifre herkese acik olur; farkli ag/sifre icin `wifi_secrets.h` kullanin veya sifreyi degistirin.

### Arduino IDE: `wifi_secrets.h: No such file` hatasi

Sketch sadece `.ino` olarak baska klasore tasinmissa `wifi_secrets.h` gelmez ve eski surumlerde derleme kirilirdi. Guncel kodda dosya yoksa **derleme yine basarili** olur; ancak WiFi/web **kapali** kalir ve Serial’da uyari gorursunuz.

WiFi’yi acmak icin:

1. Arduino IDE’de sketch’e sag ustten **+** ile yeni sekme acin, adi tam olarak **`wifi_secrets.h`** olsun.
2. Icerigi `wifi_secrets.example.h` dosyasindan kopyalayip SSID ve sifreyi doldurun.

Alternatif: Tum proje klasorunu (`.ino` + `wifi_secrets.h` birlikte) sketch klasoru olarak kullanin.
- ESP32 acildiktan sonra WiFi'ye baglanir; Serial Monitor'da `Web: http://...` IP adresini gorursunuz.
- Ayni WiFi'deki telefon veya bilgisayardan bu IP'ye gidince basit bir ozet sayfa acilir (pH, EC, TDS, RAW, mod, ADC/voltaj). Sayfa yaklasik 5 saniyede bir yenilenir.

Guvenlik: WiFi sifresini public repoya koymayin; `wifi_secrets.h` sadece kendi bilgisayarinizda kalsin.

## Donanim

- Kart: ESP32
- Ekran: 0.96" I2C OLED (SSD1306, 128x64, mavi)
- TDS modul (analog cikis)
- pH modul (analog cikis, BNC kartli)

## Pin Baglantilari

- OLED `SDA` -> GPIO `21`
- OLED `SCL` -> GPIO `22`
- TDS analog cikis -> GPIO `34`
- pH analog cikis -> GPIO `35`
- Mod degisim butonu -> GPIO `27` (diger ucu GND)
- Tum GND hatlari ortak olmali

## Ekran Bilgileri

OLED 180 derece dondurulmustur (`setRotation(2)`).

Buton ile iki gorunum vardir:
- `NORMAL`: sadece `pH` ve `EC` (sade ekran)
- `DEBUG/KALIBRASYON`: `RAW`, `pA`, `pV` dahil detayli ekran

Not: Buton `INPUT_PULLUP` ile calisir. Butona basinca mod degisir.

Olcum periyotlari:
- `NORMAL` mod: 30 saniyede 1 olcum (dakikada 2)
- `DEBUG` mod: 3 saniyede 1 olcum

## TDS/EC Kalibrasyon Notlari

TDS kismi tek katsayi yerine cok nokta kalibrasyonla (interpolasyon) hesaplanir.

Kullanilan referans tablosu (`RAW -> PPM`):
- 45 -> 83
- 183 -> 218
- 236 -> 268
- 469 -> 487
- 911 -> 816
- 1019 -> 992

Not: Asiri tutarsiz oldugu gozlenen bir referans noktasi tabloya dahil edilmemistir.

EC donusumu:
- `EC (uS/cm) = PPM * 2.0`

Bu katsayi kullanilan el tipi referans cihaz verilerinden alinmistir.

## pH Kalibrasyon Notlari

pH olcumu 3 nokta ile yapilmistir ve kodda parcali lineer model kullanilir:

- pH 4.00 buffer: `pV = 3.300`
- pH 7.00 buffer: `pV = 2.528`
- pH 10.00 buffer: `pV = 2.012`

Model:
- 4-7 arasi ayri egim
- 7-10 arasi ayri egim

Bu sayede 4, 7 ve 10 tamponlarinda daha dogru sonuc alinmistir.

## Gecmis Olcum Kayitlari

### Ilk TDS kalibrasyon ciftleri

- ESP 13 ppm -> Referans 57 ppm
- ESP 800 ppm -> Referans 660 ppm

### Sonraki kontrol ciftleri

- ESP 63 ppm -> Referans 61 ppm
- ESP 665 ppm -> Referans 689 ppm

### Detayli referans seti (TDS/EC)

Asagidaki degerler ayni suya kademeli besin eklenerek alinmistir:

1. Cihaz: 83 ppm / 166 EC, ESP: 86 ppm, RAW: 45
2. Cihaz: 218 ppm / 436 EC, ESP: 200 ppm, RAW: 183
3. Cihaz: 268 ppm / 536 EC, ESP: 244 ppm, RAW: 236
4. Cihaz: 487 ppm / 974 EC, ESP: 435 ppm, RAW: 469
5. Cihaz: 816 ppm / 1632 EC, ESP: 802 ppm, RAW: 911
6. Cihaz: 992 ppm / 1984 EC, ESP: 917 ppm, RAW: 1019
7. (Not edilen ama tutarsiz) Cihaz: 1586 ppm / 3172 EC, ESP: 965 ppm, RAW: 1010

## Kalibrasyon Akisi (Onerilen)

1. El tipi referans cihazi kalibre et.
2. pH modulu once pot ile pH7 civarina getir.
3. Buffer 4/7/10 ile `pV` topla.
4. Kod icinde pH kalibrasyon voltajlarini guncelle.
5. TDS/EC icin kademeli referans seti topla ve tabloyu guncelle.
6. Son kontrol: normal su + besinli su araliginda farki kontrol et.

## Derleme Notu

Arduino IDE'de gereken kutuphaneler:
- `Adafruit GFX Library`
- `Adafruit SSD1306`

OLED gorunmuyor ise adresi kontrol et (`0x3C` / `0x3D`).

## Kullanim Ozet

1. Cihaz acildiktan sonra varsayilan ekran modu `NORMAL` olur.
2. `NORMAL` modda sade gorunum ve seyrek olcum yapilir.
3. Dokunmatik butona basinca `DEBUG` moda gecer.
4. `DEBUG` modda kalibrasyon icin hizli olcum ve ham degerler gorulur.
5. Butona tekrar basinca tekrar `NORMAL` moda doner.
