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
| GET/POST | `/api/motion` | Podešavanja i stanje detekcije pokreta, zone |
| GET | `/api/motion/debug` | Mreža razlika (heatmap) za podešavanje osetljivosti |
| GET/POST | `/api/ai` | Podešavanja i stanje eksternog AI-ja |
| POST | `/api/ai/test` | Jedna inferencija odmah (objekti + analizirani frame) |
| GET/POST | `/api/llm` | Podešavanja AI opisa (vision LLM) |
| POST | `/api/llm/test` | Opis trenutne slike (rezultat stiže u status) |
| GET | `/api/events` | Dnevnik događaja (`?limit=N`) |
| GET | `/api/events/snapshot?id=N` | Snapshot događaja |
| POST | `/api/events/clear` | Briše događaje |
| WS | `/ws` | Status svake sekunde i događaji u realnom vremenu |

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
| `motion`, `motion/zoneN` | `ON` / `OFF` (retained) |
| `motion/set` | `ON` / `OFF`: uključuje ili isključuje detekciju pokreta |
| `person`, `car`, … | JSON po praćenoj klasi: `{"detected":true,"confidence":0.92,"x","y","width","height","zone","timestamp"}` (retained) |
| `description` | JSON poslednjeg događaja sa AI opisom (retained) |
| `ai/set` | `ON` / `OFF`: uključuje ili isključuje AI; komande `ai_on` / `ai_off` na `command` |
| `camera/set` | JSON podešavanja kamere, npr. `{"vflip":1,"quality":10}` |

Kad je discovery uključen, HA automatski dobija uređaj sa kamerom (snapshot), dijagnostičkim senzorima i dugmadima. Ponovo ga šalje kad se HA restartuje (`homeassistant/status`).

## Detekcija pokreta

Tab **Motion**: zone se crtaju prevlačenjem preko slike. Heatmap prikazuje šta senzor vidi kao promenu.
- *Sensitivity*: koliko se pojedinačna ćelija mora promeniti.
- *Minimum area*: koji deo zone se mora promeniti.
- *Trigger frames*: koliko frame-ova zaredom.
- *Cooldown*: koliko mirnih sekundi pre nego što pokret završi.

Nagla promena većine slike, npr. kad se upali svetlo, ne pokreće alarm. Dok je detekcija uključena, senzor ne ide u standby.

## AI detekcija objekata

Klasični ESP32 je preslab za ozbiljnu lokalnu detekciju objekata (spec §42). Zato kamera šalje frame-ove na AI server u mreži, a server vraća detektovane objekte.
- **Režimi:** samo dok traje pokret (podrazumevano) ili stalno, na zadati interval.
- **Podržani serveri:**
  - priloženi `tools/ai_server` (YOLO): `pip install -r tools/ai_server/requirements.txt`, zatim `python tools/ai_server/server.py`, a u kameri URL `http://<host>:5005/detect`;
  - CodeProject.AI / DeepStack: URL `http://<host>:32168/v1/vision/detection`, API „DeepStack“.
- **Praćenje:** objekat se prijavljuje kao `object_detected` tek kad je viđen iznad *enter* praga tokom *persistence* vremena. Ostaje prisutan dok je iznad *keep* praga, a `object_left` stiže posle *left after* vremena bez detekcije (spec §60).

## AI opis događaja (Ollama / Open WebUI)

Kamera može da pošalje snapshot događaja (pokret ili AI objekat) vision modelu i da opis u jednoj rečenici priloži događaju. Opis se vidi u Events tabu, na MQTT-u `…/description` i u HA senzoru „Last description“, pa se može koristiti u obaveštenjima.
- **Ollama:** URL `http://<host>:11434/api/chat`, a model mora biti vision, npr. `qwen2.5vl:3b`, `gemma3:4b` ili `llava`.
- **Open WebUI / OpenAI API:** URL `http://<host>:3000/api/chat/completions` (ili Ollama `/v1/chat/completions`) i API ključ.

Šalje se samo jedna slika po događaju, uz cooldown, pa i spori CPU modeli rade.

## Status razvoja

- [x] **Faza 1, kamera:** OV5640 init, PSRAM, JPEG, snapshot, MJPEG stream, watchdog kamere (restart drajvera)
- [x] **Faza 2, web UI:** Dashboard, Live View, Camera (sva podešavanja senzora), Network, System
- [x] **Faza 3, MQTT + Home Assistant:** discovery (kamera, dijagnostički senzori, dugmad), telemetrija, komande, Last Will
- [x] **Faza 4, detekcija pokreta:** razlika frame-ova sa kompenzacijom osvetljenja, do 4 zone, event engine sa snapshot-ima, HA binary senzori
- [ ] Faza 5: lokalni AI (ESP-DL)
- [ ] Faza 6: microSD, timelapse, pregled događaja
- [x] **AI opis događaja** preko vision LLM-a (Ollama / Open WebUI)
- [x] **Faza 7, eksterni AI:** HTTP AI server (generički ili DeepStack/CodeProject.AI), praćenje objekata sa histerezom, HA senzori po klasi, okviri na Live View-u
- [ ] Faza 8: sigurnost (auth, API tokeni, zaštita OTA, TLS)

