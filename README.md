# ESP32-WROVER-DEV v1.6 + OV5640: kamera za Home Assistant

ESP-IDF firmware koji od ESP32-WROVER ploče i OV5640 (5 MP) senzora pravi samostalnu Wi-Fi IP kameru bez cloud-a. Ima web interfejs, REST API, MJPEG stream i WebSocket telemetriju. MQTT/Home Assistant integracija, motion i AI detekcija dolaze u sledećim fazama.

## Hardver

- ESP32-D0WDQ6 (rev 1), 8 MB flash, 4 MB PSRAM, USB-serijski konvertor CH340
- OV5640 na SCCB adresi 0x3C. Pinout je WROVER-KIT/Freenove: XCLK 21, SDA 26, SCL 27, D7..D0 = 35, 34, 39, 36, 19, 18, 5, 4, VSYNC 25, HREF 23, PCLK 22. Pinovi se menjaju u `menuconfig` → *ESP32 Camera board*.
- Status LED na GPIO 2

## Build i flash (PlatformIO)

```sh
pio run                     # build
pio run -t upload           # flash (COM8, podešeno u platformio.ini)
pio device monitor          # serijska konzola, 115200
pio run -t menuconfig       # sdkconfig / pinovi
```

ESP-IDF 5.3.2 se instalira automatski kroz PlatformIO (pioarduino platforma). Komponente `esp32-camera` i `mdns` preuzima IDF component manager.

## Prvo pokretanje

1. Ako Wi-Fi nije podešen, kamera podiže access point **`ESP32-CAM-XXXX`** sa lozinkom **`esp32camera`**.
2. Poveži se na taj AP i otvori **http://192.168.4.1** → *Network* → upiši SSID i lozinku → *Save & connect*.
3. Kada se kamera poveže, dostupna je na **http://esp32-camera-XXXX.local**. Tačna IP adresa piše u serijskom logu i na *Network* tabu. Setup AP se gasi 2 minuta nakon povezivanja.

Wi-Fi se može podesiti i preko serijske konzole: `wifi <ssid> <lozinka>`.

## Endpointi

| Metod | Putanja | Opis |
|---|---|---|
| GET | `/` | Web UI |
| GET | `:81/stream` | MJPEG stream (do 3 klijenta) |
| GET | `/capture`, `/api/snapshot` | JPEG snapshot |
| GET | `/api/status` | Status (FPS, heap, PSRAM, CPU, Wi-Fi, kamera) |
| GET/POST | `/api/camera` | Sva podešavanja kamere sa opsezima; POST primenjuje `{"id": vrednost}` u RAM |
| POST | `/api/camera/save` | Snima trenutna podešavanja u NVS |
| POST | `/api/camera/defaults` | Vraća podrazumevane vrednosti (u RAM) |
| POST | `/api/camera/af` | Pokreće autofokus |
| GET/POST | `/api/wifi` | Wi-Fi podešavanja |
| GET | `/api/wifi/scan` | Skeniranje mreža |
| GET/POST | `/api/system` | Informacije o sistemu i lista taskova; POST `{"device_name"}` |
| POST | `/api/time` | `{"epoch": <unix s>}`: postavlja vreme iz browsera ako NTP ne radi |
| POST | `/api/reboot` | Restart |
| POST | `/api/factory-reset` | `{"wifi": true}` briše i Wi-Fi podešavanja |
| GET | `/api/telemetry` | Live registri senzora (ekspozicija, gain, AWB…) i telemetrija modula |
| GET/POST | `/api/mqtt` | MQTT/HA podešavanja |
| POST | `/api/mqtt/discovery` | Ponovo šalje HA discovery |
| WS | `/ws` | Status svake sekunde |

> Autentifikacija još ne postoji (Faza 8). Kameru drži samo u lokalnoj mreži.

## MQTT / Home Assistant

Podešava se na tabu **MQTT / HA**. Topici imaju oblik `camera/<device>/…`, gde je `<device>` hostname sa `_` umesto `-`, npr. `esp32_camera_0064`:

| Topic | Sadržaj |
|---|---|
| `availability` | `online` / `offline` (retained, Last Will) |
| `telemetry` | JSON: rssi, fps, heap, psram, uptime, cpu, camera_ok, URL-ovi (retained, period podesiv) |
| `snapshot` | JPEG slika za HA MQTT camera entitet (retained) |
| `event` | JSON događaji (motion/AI u sledećim fazama) |
| `command` | `snapshot`, `reboot`, `restart_camera` |
| `camera/set` | JSON podešavanja kamere, npr. `{"vflip":1,"quality":10}` |

Kad je discovery uključen, HA automatski dobija uređaj sa kamerom (snapshot), dijagnostičkim senzorima i dugmadima. Ponovo ga šalje kad se HA restartuje (`homeassistant/status`).

## Status razvoja

- [x] **Faza 1, kamera:** OV5640 init, PSRAM, JPEG, snapshot, MJPEG stream, watchdog kamere (restart drajvera)
- [x] **Faza 2, web UI:** Dashboard, Live View, Camera (sva podešavanja senzora), Network, System
- [x] **Faza 3, MQTT + Home Assistant:** discovery (kamera, dijagnostički senzori, dugmad), telemetrija, komande, Last Will
- [ ] Faza 4: motion detection, zone, event engine
- [ ] Faza 5: lokalni AI (ESP-DL)
- [ ] Faza 6: microSD, timelapse, pregled događaja
- [ ] Faza 7: eksterni AI / Frigate
- [ ] Faza 8: sigurnost (auth, API tokeni, zaštita OTA, TLS)

