# UI mock za snimke ekrana

Pravi web UI iz firmware-a (`main/web/index.html`) uz lažni backend: izmišljeni mrežni podaci i nacrtana scena (kuća, osoba, pas, auto) umesto snimka kamere. Služi za slike u `docs/screenshots/`.

```sh
pip install pillow
python tools/ui_mock/server.py               # http://127.0.0.1:8080, stream na :81
MOCK_LOGIN=1 python tools/ui_mock/server.py  # ekran za prijavu
```

- `api/*.json`: odgovori snimljeni sa prave kamere, anonimizovani. `server.py` preko njih upisuje stanje za slike (pokret, osoba, AI objekti, tamper, događaji).
- WebSocket `/ws` šalje status i AI okvire svake sekunde, isto kao firmware.
- Live View prikazuje `127.0.0.1` u URL-ovima streama. Pre snimka ga zameni u stranici (npr. `192.168.1.50`).
- Slike su 1568×705 (snimci iz Chrome-a). Kursor pomeri u ugao pre snimanja.
- Ne snimaj pravu kameru za dokumentaciju: repo je javan.
