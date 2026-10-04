"""Mock camera backend for documentation screenshots.

Serves the real firmware UI (main/web/index.html) against invented data: captured API
payloads with every network/device identifier replaced, and a synthetic scene instead of
camera footage. Port 8080: UI + /api + /ws, port 81: MJPEG stream.

    python tools/ui_mock/server.py            # dashboard etc., already signed in
    MOCK_LOGIN=1 python tools/ui_mock/server.py   # login screen
"""
import base64, copy, hashlib, io, json, os, socket, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from scene import scene, BOXES

HERE = os.path.dirname(os.path.abspath(__file__))
UI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'main', 'web', 'index.html')
SHOW_LOGIN = os.environ.get('MOCK_LOGIN') == '1'


def load(name):
    with open(os.path.join(HERE, 'api', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


def jpeg(im, q=85):
    b = io.BytesIO()
    im.save(b, 'JPEG', quality=q)
    return b.getvalue()


FRAME = jpeg(scene())
SNAPS = {i: jpeg(scene(person_dx=dx, seed=i).resize((512, 384)), 80) for i, dx in enumerate([0, -60, -120, 30, 60, -30, 90, 0, -90, 20], 1)}

IP, HOSTNAME, DEV = '192.168.1.50', 'esp32-camera-3456', 'esp32_camera_3456'
WIFI = {'connected': True, 'ssid': 'HomeNet', 'ip': IP, 'hostname': HOSTNAME, 'rssi': -33, 'channel': 6,
        'bssid': 'a4:91:b1:2c:3d:4e', 'reconnects': 0, 'ap_active': False, 'ap_ssid': 'ESP32-CAM-3456', 'ap_ip': ''}
UPTIME = 3 * 86400 + 4 * 3600 + 15
ZONES = [{'name': 'Entrance', 'x': 190, 'y': 430, 'w': 330, 'h': 560}, {'name': 'Driveway', 'x': 545, 'y': 600, 'w': 445, 'h': 380}]
LINE = {'x1': 240, 'y1': 640, 'x2': 560, 'y2': 900, 'invert': False}
OBJECTS = [{'label': 'person', 'present': True, 'count': 1, 'confidence': 91, 'today': 14},
           {'label': 'car', 'present': True, 'count': 1, 'confidence': 88, 'today': 6},
           {'label': 'dog', 'present': True, 'count': 1, 'confidence': 77, 'today': 4},
           {'label': 'cat', 'present': False, 'count': 0, 'confidence': 0, 'today': 1}]
TAMPER = {'enabled': True, 'state': 'ok', 'reason': '', 'edge_pct': 97, 'lost_pct': 6, 'ref_age_s': 41, 'alarms': 1}


def motion_state():
    return {'enabled': True, 'active': True, 'changed_pct': 7.4, 'analysed': 412873, 'scene_changes': 9, 'analyse_ms': 118,
            'events_today': 23, 'line': {'enabled': True, 'in': 9, 'out': 7, 'last': 'in'}, 'tamper': TAMPER,
            'zones': [{'name': 'Entrance', 'active': True, 'pct': 12.8}, {'name': 'Driveway', 'active': False, 'pct': 0.6}]}


def status():
    s = load('status')
    s.update({'device': 'ESP32 Camera', 'uptime': UPTIME + int(time.time() - START), 'heap': 112203, 'heap_min': 68420,
              'heap_largest': 45056, 'psram_free': 1290000, 'cpu': [38, 64], 'time': time.strftime('04.10.2026 14:%M:%S'),
              'time_source': 'ntp', 'wifi': WIFI})
    s['camera'].update({'fps': 12.4, 'frames': 2875012 + int((time.time() - START) * 12), 'quality': 10, 'frame_bytes': 61540,
                        'stream_clients': 1, 'stream_max': 3, 'errors': 0, 'restarts': 0})
    s['motion'] = motion_state()
    s['ai'] = {'state': 'ok', 'last_error': '', 'inferences': 18342, 'errors': 3, 'latency_ms': 212, 'fps': 1.0,
               'last_object': 'person', 'objects': OBJECTS}
    s['llm'] = {'state': 'idle', 'requests': 14, 'errors': 0, 'skipped': 31, 'last_seconds': 1.2, 'last_error': '',
                'last_text': 'Osoba u crvenoj jakni sa psom stoji na stazi ispred kuće, pored parkiranog plavog automobila.',
                'test_running': False, 'test_text': ''}
    s['person'] = {'state': 'running', 'present': True, 'score': 86, 'inference_ms': 640, 'total_ms': 702, 'inferences': 5521,
                   'today': 12, 'last_error': ''}
    s['modules'] = {'mqtt': 'ok', 'ha': 'ok', 'sd': 'n/a', 'ai': 'ok', 'llm': 'idle', 'person': 'running', 'motion': 'active', 'ota': 'ok'}
    s['default_password'] = False
    return s


def telemetry():
    t = load('telemetry')
    t['camera'].update({'fps': 12.4, 'frames': 2875012, 'last_frame_bytes': 61540})
    t['camera']['live'].update({'avg_luma': 118, 'analog_gain': 2.0, 'exposure_lines': 420, 'exposure_pct_of_frame': 42.7, 'light_index': 6.2})
    t['module'].update({'mac': '24:0a:c4:12:34:56', 'heap_total': 324271, 'dma_heap_total': 312479, 'dma_heap_free': 98100,
                        'psram_min_free': 1236000, 'nvs': {'used_entries': 174, 'free_entries': 582, 'namespaces': 12}})
    return t


def events():
    base = 1791117000
    ev = [
        (12, 'person_detected_local', 'Entrance', 86, '', 1, 'Osoba u crvenoj jakni sa psom stoji na stazi ispred kuće.'),
        (11, 'object_detected', 'Driveway', 88, 'car', 2, ''),
        (10, 'line_in', None, 9, '', None, ''),
        (9, 'motion_start', 'Entrance', 12.8, '', 3, 'Osoba prilazi ulaznim vratima, pas je pored nje.'),
        (8, 'object_detected', 'Entrance', 77, 'dog', 4, ''),
        (7, 'tamper_cleared', None, 0, 'restored', None, ''),
        (6, 'tamper', None, 18, 'covered, edges 18%, lost 92%', 5, ''),
        (5, 'motion_end', 'Driveway', 6.1, 'duration 34s, peak 6.1%', None, ''),
        (4, 'line_out', None, 7, '', None, ''),
        (3, 'mqtt_connected', None, 0, '192.168.1.10', None, ''),
    ]
    out = []
    for i, (eid, name, zone, val, detail, snap, desc) in enumerate(ev):
        e = {'id': eid, 'event': name, 'camera': 'ESP32 Camera', 'value': val, 'timestamp': base - i * 420,
             'time': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(base - i * 420)), 'uptime': UPTIME - i * 420}
        if zone: e['zone'] = zone
        if detail: e['detail'] = detail
        if name.startswith('object'): e['object'] = detail
        if snap: e['snapshot'] = f'/api/events/snapshot?id={snap}'
        if desc: e['description'] = desc
        out.append(e)
    return out


def stats():
    import math, random
    rnd = random.Random(7)
    now_h = int(time.time() // 3600)
    hours = []
    for k in range(now_h - 23, now_h + 1):
        hod = time.localtime(k * 3600).tm_hour
        day = 1.0 if 7 <= hod <= 21 else 0.15
        act = lambda m: int(rnd.random() * m * day + 0.3)
        hours.append({'start': k * 3600, 'label': f'{hod:02d}:00', 'motion': act(9), 'person': act(5), 'line_in': act(4),
                      'line_out': act(4), 'tamper': 1 if k == now_h - 3 else 0, 'objects': [act(5), act(3), act(2), act(1)],
                      'samples': 360, 'fps': round(12.4 - 2.5 * day * rnd.random(), 1), 'ai_fps': round(0.9 * day * rnd.random(), 2),
                      'cpu': round(30 + 25 * day + 5 * rnd.random()), 'rssi': -33 - rnd.randint(0, 6),
                      'heap_min': 68000 + rnd.randint(0, 9000), 'psram_min': 1180000 + rnd.randint(0, 90000)})
    return {'clock': 'wall', 'sample_s': 10, 'labels': ['person', 'car', 'dog', 'cat'], 'hours': hours}


def get_json(path):
    if path == '/api/status': return status()
    if path == '/api/stats': return stats()
    if path == '/api/session': return {'auth_enabled': True, 'authorized': not SHOW_LOGIN}
    if path == '/api/telemetry': return telemetry()
    if path == '/api/events': return events()
    if path == '/api/motion':
        m = load('motion')
        m['config'].update({'enabled': True, 'fps': 3, 'tamper': True, 'zones': ZONES, 'line': LINE})
        m['state'] = motion_state()
        return m
    if path == '/api/motion/debug':
        w, h = 96, 72
        cells = []
        for y in range(h):
            for x in range(w):
                fx, fy = x / w * 1024, y / h * 768
                v = 0
                if 404 <= fx <= 518 and 368 <= fy <= 662: v = 7 + (x + y) % 3
                elif 326 <= fx <= 440 and 566 <= fy <= 660: v = 4 + (x * y) % 3
                elif (x * 7 + y * 13) % 41 == 0: v = 1
                cells.append(str(v))
        return {'w': w, 'h': h, 'cells': ''.join(cells)}
    if path == '/api/ai':
        a = load('ai')
        a['config'].update({'enabled': True, 'mode': 1, 'url': 'http://192.168.1.20:5005/detect'})
        a['state'] = status()['ai']
        return a
    if path == '/api/llm':
        l = load('llm')
        l['config'].update({'url': 'http://192.168.1.20:11434/api/chat', 'model': 'gemma3:4b'})
        l['state'] = status()['llm']
        return l
    if path == '/api/person':
        p = load('person')
        p['state'] = status()['person']
        return p
    if path == '/api/mqtt':
        m = load('mqtt')
        m.update({'host': '192.168.1.10', 'user': 'mqtt_user', 'client_id': DEV, 'device_id': DEV, 'topic_base': 'camera/' + DEV})
        return m
    if path == '/api/security':
        s = load('security')
        s.update({'token': '0f3a9c2e7b1d4f6a8c5e2b9d7f1a3c6e', 'default_password': False})
        return s
    if path == '/api/system':
        s = load('system')
        s.update({'uptime_s': UPTIME, 'boot_count': 37, 'reset_reason': 'software', 'heap_free': 112203, 'heap_min': 68420,
                  'psram_free': 1290000})
        return s
    if path == '/api/wifi': return {'ssid': 'HomeNet', 'has_password': True, 'hostname': HOSTNAME}
    if path == '/api/wifi/scan':
        return [{'ssid': 'HomeNet', 'rssi': -33, 'auth': 'WPA2', 'channel': 6}, {'ssid': 'Neighbour-5G', 'rssi': -71, 'auth': 'WPA2', 'channel': 11}]
    if path == '/api/ota': return load('ota')
    if path == '/api/camera': return load('camera')
    return None


START = time.time()


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/':
            return self.send(200, open(UI, 'rb').read(), 'text/html; charset=utf-8')
        if path == '/ws':
            return self.websocket()
        if path in ('/capture', '/api/snapshot'):
            return self.send(200, FRAME, 'image/jpeg')
        if path == '/api/events/snapshot':
            sid = int(self.path.split('id=')[1])
            return self.send(200, SNAPS.get(sid, FRAME), 'image/jpeg')
        if path == '/stream':
            return self.stream()
        j = get_json(path)
        if j is None:
            return self.send(404, b'{"error":"not found"}', 'application/json')
        self.send(200, json.dumps(j, ensure_ascii=False).encode(), 'application/json')

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        self.rfile.read(n)
        path = self.path.split('?')[0]
        if path == '/api/ai/test':
            j = {'objects': BOXES, 'w': 1024, 'h': 768, 'ms': 214, 'image': base64.b64encode(FRAME).decode()}
        elif path == '/api/person/test':
            crop = scene().crop((300, 330, 620, 650)).convert('L').resize((96, 96))
            j = {'score': 86, 'present': True, 'inference_ms': 640, 'total_ms': 702, 'size': 96,
                 'image': base64.b64encode(crop.tobytes()).decode()}
        else:
            j = {'ok': True}
        self.send(200, json.dumps(j).encode(), 'application/json')

    def stream(self):
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.end_headers()
        try:
            while True:
                self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(FRAME) + FRAME + b'\r\n')
                time.sleep(0.2)
        except Exception:
            pass

    def websocket(self):
        key = self.headers['Sec-WebSocket-Key']
        acc = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', acc)
        self.end_headers()

        def frame(obj):
            data = json.dumps(obj, ensure_ascii=False).encode()
            hdr = bytes([0x81]) + (bytes([len(data)]) if len(data) < 126 else bytes([126]) + struct.pack('>H', len(data)))
            self.wfile.write(hdr + data)
            self.wfile.flush()
        try:
            while True:
                s = status()
                s['type'] = 'status'
                frame(s)
                frame({'type': 'detection', 'w': 1024, 'h': 768, 'ms': 212, 'objects': BOXES})
                time.sleep(1)
        except Exception:
            pass


def serve(port):
    ThreadingHTTPServer(('127.0.0.1', port), H).serve_forever()


if __name__ == '__main__':
    threading.Thread(target=serve, args=(81,), daemon=True).start()
    print('mock on http://127.0.0.1:8080', flush=True)
    serve(8080)
