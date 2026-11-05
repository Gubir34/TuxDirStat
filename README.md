# TuxDirStat

Linux için disk kullanım analizörü: klasör ağacı, uzantı listesi ve gölgeli (cushion)
treemap. Çok iş parçacıklı tarayıcısı milyonlarca dosyayı saniyenin altında tarar.

Aynı çekirdeğin dört arayüzü vardır; her biri ayrı bir programdır:

| Program        | Arayüz | CMake seçeneği              |
|----------------|--------|-----------------------------|
| `tuxdirstat`      | Qt 6   | `TUXDIRSTAT_QT` (varsayılan açık) |
| `tuxdirstat-gtk3` | GTK 3  | `TUXDIRSTAT_GTK3`              |
| `tuxdirstat-gtk4` | GTK 4  | `TUXDIRSTAT_GTK4`              |
| `tuxdirstat-tk`   | Tcl/Tk | `TUXDIRSTAT_TK`                |

## Kurulum

```sh
./install.sh             # yalnızca Qt
./install.sh gtk4 tk     # seçilen arayüzler
./install.sh all         # hepsi
```

Programlar `~/.local/bin` altına, menü girdileri `~/.local/share/applications` altına kurulur.
Gerekenler: `cmake`, `ninja`, bir C++20 derleyicisi ve seçilen arayüzün geliştirme paketleri
(`qt6-base`, `gtk3`, `gtk4`, `tk`; Tk arayüzü ayrıca `glib2`/GIO kullanır).

Elle derleme:

```sh
cmake -S . -B build -G Ninja -DTUXDIRSTAT_GTK4=ON -DTUXDIRSTAT_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Kullanım

```sh
tuxdirstat                  # konum seçiciyle açılır
tuxdirstat ~/Oyunlar        # doğrudan tarar
```

- Ağaçta ya da treemap'te bir öğeye tıklayınca diğerinde de seçilir.
- Treemap'te çift tıklama klasöre yakınlaşır, **Backspace** geri çıkar.
- Uzantı listesinde bir satır seçmek o türdeki dosyaları (ve grupları) treemap'te işaretler.
- Sağ tık: aç, dosya yöneticisinde göster, yolu kopyala, yeniden tara, çöpe taşı, kalıcı sil.

| Kısayol | İşlem |
|---|---|
| Ctrl+O | Konum seç |
| F5 | Yeniden tara |
| Esc | Taramayı durdur |
| Ctrl++ / Backspace / Ctrl+0 | Yakınlaştır / uzaklaştır / köke dön |
| Ctrl+G | Dosyaları grupla |
| F9 | Treemap'i göster/gizle |
| Delete / Shift+Delete | Çöpe taşı / kalıcı sil |
| Ctrl+C | Yolu kopyala |

### Gruplama

Oyun klasörleri gibi binlerce küçük dosya içeren yerlerde **Dosyaları grupla** açılınca,
bir klasörde en az *eşik* kadar (varsayılan 16; 8–128 arası seçilebilir) benzer öğe tek bir
sanal düğümde toplanır:

- **Aynı uzantılı dosyalar:** `*.dds (1.500 dosya)`
- **Küçük dosyalar:** uzantıya göre toplanamayan, klasör toplamının %1'inden küçük dosyalar
- **Küçük klasörler:** klasör toplamının %1'inden küçük alt klasörler (önbellek klasörleri gibi)

Treemap'te grup tek blok olarak çizilir; çift tıklayınca içine girilir. Grubu silmek
içindeki dosyaları siler, grubu yeniden taramak bulunduğu klasörü tarar.

### "Tek dosya sistemi"

Açıkken başka disklere ve bağlama noktalarına geçilmez; aynı aygıttaki btrfs alt birimleri
(ör. `/home`, `/var/log`) yine dahil edilir. `/proc`, `/sys` gibi sanal dosya sistemleri
her zaman atlanır.

## Komut satırı raporu

Arayüz açmadan özet verir (dört programda da aynıdır):

```sh
tuxdirstat --report --group --top 10 ~/.local/share/Steam/steamapps/common/War\ Thunder
```

En büyük klasörleri, dosyaları, uzantıları ve (`--group` ile) en kalabalık grupları listeler.
Diğer seçenekler: `--apparent` (görünür boyut), `--group-min N`, `--all-fs`, `--help`, `--version`.

## Tema

**Sistem** (varsayılan) masaüstünün görünümünü izler. **Açık** ve **Koyu** her arayüzde
zorlanabilir: Qt'de *Görünüm → Tema*, GTK'de sağ üstteki menü, Tk'de araç çubuğundaki
*Tema* düğmesi. Qt bu iki modda Fusion stili üzerine TuxDirStat'ın kendi paletlerini kullanır;
GTK, KDE'nin dayattığı renk dosyasını da ezer ve simge temasını (ör. breeze ↔ breeze-dark)
buna göre değiştirir.

## Ayarlar

Görünür boyut, tek dosya sistemi, gruplama, eşik, tema ve treemap görünürlüğü
`~/.config/tuxdirstat/settings.conf` dosyasında saklanır; dört arayüz aynı dosyayı paylaşır.

## Proje yapısı

```
src/core/   tarayıcı, veri modeli, gruplama, treemap çizimi, rapor, ayarlar
src/qt/     Qt 6 arayüzü
src/gtk/    GTK 3 / GTK 4 arayüzü (tek kaynak)
src/tk/     Tk arayüzü (gömülü Tcl betiği + C++)
tests/      çekirdek birim testleri
```
