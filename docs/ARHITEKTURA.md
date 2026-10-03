# Arhitektura firmware-a

Firmware je ESP-IDF 5.3 aplikacija, buildovana kroz PlatformIO. Sastoji se od modula u `main/`. Svaki modul ima svoj NVS namespace, svoj FreeRTOS task (gde treba) i JSON API.

```text
            OV5640 ──► camera_mgr ──► frame hub (4 PSRAM slota, ref-count)
                                         │
     ┌──────────────┬──────────────┬─────┴────────┬──────────────┬──────────────┐
     ▼              ▼              ▼              ▼              ▼              ▼
 MJPEG stream   snapshot      motion_mgr     person_mgr      ai_mgr        mqtt snapshot
 (web :81)      (/capture)    zone, linija   TFLite Micro    eksterni AI
                              tamper*        (na ESP32)      (HTTP server)
                                   │              │              │
                                   └──────────────┴──────┬───────┘
                                                         ▼
                                                    event_mgr  ──► llm_mgr (vision LLM opis)
                                                         │
                                  ┌──────────────────────┼──────────────────────┐
                                  ▼                      ▼                      ▼
                              mqtt_mgr               WebSocket              Events tab
                         (HA discovery, stanja)     (web UI live)          (+ snapshot-i)
```
`*` planirano

## Moduli

| Modul | Uloga |
|---|---|
| `camera_mgr.c` | Init OV5640, **tabela parametara** (opseg, setter, NVS ključ) iz koje se generišu API i Camera tab, prepoznavanje mogućnosti senzora u runtime-u, **frame hub**, watchdog (restart drajvera posle 5 grešaka), standby senzora kad niko ne gleda, `cam_mgr_suspend()` oko upisa u flash |
| `web_server.c` | Dva HTTP servera: port 80 (UI, `/api/*`, `/capture`, WebSocket) i port 81 (MJPEG, task po klijentu, do 3). Svaki API handler prolazi kroz `guarded()` proveru prijave. OTA upload i rollback |
| `auth_mgr.c` | Lozinka (PBKDF2-SHA256 + salt), sesije (HttpOnly cookie), API token, HTTP Basic, zaključavanje posle pogrešnih pokušaja, inicijalni podaci `admin` / `espadmin` |
| `wifi_mgr.c` | STA sa ponovnim povezivanjem, setup AP kao rezerva, mDNS, SNTP (Europe/Belgrade), vreme iz browsera kao rezerva |
| `mqtt_mgr.c` | MQTT klijent i HA discovery; sve operacije nad klijentom rade u jednom worker task-u, ostali moduli samo šalju komande u red |
| `event_mgr.c` | Centralni event engine: neblokirajući red, 64 događaja u PSRAM-u, snapshot-i za poslednjih 12, slanje na MQTT, WebSocket i log, listener za LLM opise |
| `motion_mgr.c` | Detekcija pokreta: JPEG se dekodira u razmeri 1:8 u mrežu 96×72, poređenje sa prilagodljivom pozadinom, kompenzacija osvetljenja, do 4 zone, prelazak linije (težište promena, histereza) |
| `person_mgr.cc` | Lokalna detekcija osobe: TFLite Micro model 96×96, isečak oko pokreta, potvrda i timeout |
| `ai_mgr.c` | Eksterni AI: POST JPEG-a na server (generički ili DeepStack/CodeProject.AI), praćenje objekata sa histerezom |
| `llm_mgr.c` | Opis događaja vision LLM-om (Ollama `/api/chat` ili OpenAI-kompatibilni API), cooldown, bez gomilanja zahteva |
| `sysmon.c` | Opterećenje CPU-a po jezgru, heap/PSRAM, lista taskova, telemetrija modula |
| `status_led.c` | Statusni LED (GPIO 2) |
| `console_cmds.c` | Serijska konzola (UART, 115200) |
| `app_config.c` | NVS inicijalizacija, Wi-Fi i sistemska podešavanja, factory reset, brojač boot-ova |
| `web/index.html` | Ceo web UI (vanilla JS, ugrađen u firmware) |

## Tokovi i pravila

- **Frame hub.** Kamera snima samo dok neko traži slike (stream, snapshot, detekcija). Svaki frame se jednom kopira u PSRAM slot sa brojačem referenci, pa ga više potrošača čita bez kopiranja. Novi moduli moraju koristiti `cam_mgr_consumer_add` / `cam_mgr_frame_wait` / `cam_mgr_frame_release`, nikad direktno `esp_camera_fb_get`.
- **Apply → RAM, Save → NVS.** Promena podešavanja važi odmah, a u flash ide tek na Save, i to samo izmenjeni ključevi (spec §52).
- **Događaji se ne šalju za svaki frame** (spec §60): pokret ima broj frame-ova za okidanje i cooldown, a objekti i osobe imaju potvrdu, histerezu pragova i timeout odsustva.
- **HTTP handleri nikad ne blokiraju dugo.** Spore operacije (LLM opis) rade u svom task-u, a API samo pokreće posao i vraća stanje.
- **Upis u flash i kamera.** Na ovoj ploči (ESP32 rev1 + PSRAM) brisanje sektora flash-a dok radi DMA kamere zaglavi procesor. Interrupt watchdog tada resetuje čip, bez core dump-a. Zato se kamera zaustavlja oko OTA upisa i oko upisa `otadata` (`cam_mgr_suspend`).

## Memorija i particije

| Particija | Veličina | Namena |
|---|---|---|
| `nvs` | 24 KB | sva podešavanja (namespace-i: camera, wifi, mqtt, ai, llm, person, motion, security, system, diag…) |
| `otadata` | 8 KB | koji OTA slot je aktivan |
| `ota_0`, `ota_1` | 2 × 3 MB | firmware (A/B, rollback ako novi firmware ne može da se pokrene) |
| `coredump` | 64 KB | zapis o padu |
| `storage` | 1,8 MB | rezervisano (snapshot-i/timelapse bez SD kartice) |

PSRAM (4 MB): ~2 MB JPEG bafera kamere (inicijalizovani za 5 MP), frame hub, dnevnik događaja sa snapshot-ima, baferi za detekciju pokreta, radna memorija TFLite modela (160 KB).

## Dodavanje novog podešavanja kamere

Dodaj jedan red u `PARAMS[]` u `camera_mgr.c` (id, labela, grupa, tip, opseg, setter). NVS čuvanje, `/api/camera` i kontrola u Camera tabu nastaju automatski. Ako je opseg drugačiji za OV5640, podesi ga u `adapt_to_sensor()`.
