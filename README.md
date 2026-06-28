# Kelompok 8 - Mikrokontroler

## Anggota Kelompok

| Nama | NPM |
|------|------|
| Mochammad Cahyahadi Fadhlurrahma | 23552011347 |
| Rizal Abdul Ghani | 23552011086 |
| Candra Lesmana | 23552011071 |


# Irigasi Pintar — Smart Irrigation Dashboard

Irigasi Pintar adalah sistem penyiram tanaman otomatis berbasis ESP32 yang dapat dimonitor dan dikontrol secara jarak jauh melalui browser. Sistem ini menggunakan protokol MQTT sebagai jembatan komunikasi antara perangkat keras (ESP32 + sensor kelembapan + relay + pompa air) dengan antarmuka web dashboard, sehingga pengguna dapat memantau kondisi tanah secara real-time dan mengendalikan pompa kapan saja tanpa harus berada di dekat tanaman.

---

## Fitur

### Monitoring kelembapan tanah
- Gauge lingkaran yang menampilkan persentase kelembapan secara real-time
- Nilai ADC mentah dari sensor ditampilkan sebagai referensi kalibrasi
- Badge status tanah: **KERING** / **LEMBAB** / **BASAH**
- Progress bar bergradasi dari merah (kering) ke biru (basah)
- Timestamp update terakhir

### Kontrol pompa
- Tampilan status pompa (ON / OFF) dengan animasi visual
- Timer durasi menyala (format MM:SS)
- Tiga mode kontrol:
  - **Nyalakan** — paksa pompa ON, sensor diabaikan
  - **Matikan** — kunci pompa OFF, tidak akan auto-nyala
  - **Otomatis** — pompa mengikuti pembacaan sensor
- Strip indikator mode aktif (hijau = auto, biru = manual ON, merah = manual OFF)

  ### Level Air Tangki
- Ilustrasi tangki yang terisi secara proporsional sesuai level air aktual
- Badge status kondisi tangki: **KOSONG / SEDANG / PENUH**
- Progress bar horizontal dengan tiga titik acuan: Kosong, Sedang, Penuh
- Timestamp update terakhir
  
### Grafik riwayat
- Grafik garis kelembapan hingga 60 titik data terakhir
- Overlay status pompa ON/OFF pada sumbu kedua
- Tombol hapus riwayat grafik

### Status koneksi
- Status bar 3 level: WiFi ESP32 → Broker MQTT → Data ESP32
- LED indikator berwarna untuk setiap layer koneksi
- Animasi pulse saat data baru diterima
- Deteksi otomatis ESP32 offline jika tidak ada data lebih dari 15 detik
- Notifikasi toast untuk setiap event penting

### Pengaturan MQTT
- Modal konfigurasi broker: host, port, path, username, password
- Mendukung koneksi **WSS** (port 443) maupun **WS** (port 9001)
- Kompatibel dengan Shiftr.io dan Mosquitto lokal

<img width="959" height="562" alt="image" src="https://github.com/user-attachments/assets/49d05b83-7a9b-4a26-ae29-41e73ea1f395" />


---

## Komponen Hardware

| Komponen | Fungsi |
|---|---|
| ESP32 | Mikrokontroler utama, WiFi, web server |
| Soil Moisture Sensor | Membaca kelembapan tanah (pin AO → GPIO34) |
| Relay Module SRD-05VDC-SL-C | Sakelar pompa, active LOW (pin IN → GPIO26) |
| Mini Water Pump DC | Memompa air ke tanaman |
| Powerbank / Adaptor 5V | Sumber daya ESP32 (terpisah dari daya pompa) |

---

## Topik MQTT

| Topik | Arah | Isi |
|---|---|---|
| `irrigation/sensor` | ESP32 → Dashboard | JSON: `moisture_pct`, `moisture_raw`, `soil_status`, `pump_state`, `mode` |
| `irrigation/status` | ESP32 → Dashboard | String: `ONLINE`, `ON`, `OFF` |
| `irrigation/alert` | ESP32 → Dashboard | String pesan peringatan |
| `irrigation/control` | Dashboard → ESP32 | String: `ON`, `OFF`, `AUTO` |

### Contoh payload sensor
```json
{
  "moisture_pct": 45,
  "moisture_raw": 1850,
  "soil_status": "LEMBAB",
  "pump_state": "OFF",
  "mode": "auto"
}
```

---

## Alur Sistem

```
[Sensor tanah] ──ADC──► [ESP32]
                             │
                    baca kelembapan
                    setiap N detik
                             │
                    kelembapan < threshold?
                    ├── Ya → nyalakan relay → pompa ON
                    └── Tidak → relay OFF → pompa OFF
                             │
                    publish ke MQTT broker
                    topic: irrigation/sensor
                             │
                    [MQTT Broker - Shiftr.io / Mosquitto]
                             │
                    subscribe dari browser
                             │
                    [Dashboard Web]
                    ├── update gauge & grafik
                    ├── tampilkan status pompa
                    └── tombol kontrol manual
                                  │
                         klik Nyalakan / Matikan / Otomatis
                                  │
                         publish ke irrigation/control
                                  │
                         [ESP32 terima perintah]
                         └── ubah mode & jalankan aksi
```

---

## Cara Menjalankan

1. Upload kode ESP32 dengan konfigurasi WiFi dan MQTT broker yang sesuai
2. Buka file `testing.html` di browser (Chrome / Firefox)
3. Klik ikon koneksi di pojok kanan atas untuk setting broker jika diperlukan
4. Dashboard otomatis terhubung dan menampilkan data saat ESP32 online

### Default broker (Shiftr.io)
```
Host     : smart-irrigation-system.cloud.shiftr.io
Port     : 443 (WSS)
Path     : /mqtt
Username : smart-irrigation-system
Password : 2TnGUiGgmfpPnVod
```

Untuk broker lokal Mosquitto, gunakan port `9001` dan path `/`.

---

## Catatan Penting

- Sumber daya pompa **harus terpisah** dari ESP32 — jangan ambil daya pompa dari pin VIN/5V ESP32 karena arus besar dapat merusak chip
- Relay SRD-05VDC-SL-C membutuhkan **5V** (bukan 3.3V) agar dapat aktif
- Kalibrasi nilai `DRY_VALUE` dan `WET_VALUE` di kode ESP32 sesuai kondisi tanah setempat sebelum digunakan
- Dashboard mendeteksi ESP32 offline otomatis setelah **15 detik** tidak ada data masuk
