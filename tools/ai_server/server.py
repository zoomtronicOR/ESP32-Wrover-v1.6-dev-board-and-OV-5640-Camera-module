"""Reference object-detection server for the ESP32 camera (spec §44, "EXTERNAL AI").

Runs a YOLO model (Ultralytics) and speaks both APIs the camera supports:

  POST /detect                 body: raw image/jpeg
       -> {"objects": [{"label", "confidence", "x", "y", "w", "h"}]}
  POST /v1/vision/detection    multipart/form-data, field "image"   (DeepStack / CodeProject.AI)
       -> {"success": true, "predictions": [{"label", "confidence", "x_min", "y_min", "x_max", "y_max"}]}

Usage:
  pip install -r requirements.txt
  python server.py --port 5005 --model yolo11n.pt --conf 0.35

Camera setting (AI tab): server URL  http://<this-host>:5005/detect  with API "generic".
"""
import argparse
import io
import json
import time
from email.parser import BytesParser
from email.policy import default as email_policy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image
from ultralytics import YOLO

MAX_BODY = 10 * 1024 * 1024


def detect(model, conf, jpeg):
    img = Image.open(io.BytesIO(jpeg)).convert("RGB")
    t0 = time.time()
    res = model.predict(img, conf=conf, verbose=False)[0]
    ms = (time.time() - t0) * 1000
    objs = []
    for box in res.boxes:
        x1, y1, x2, y2 = (float(v) for v in box.xyxy[0])
        objs.append({
            "label": res.names[int(box.cls[0])],
            "confidence": round(float(box.conf[0]), 3),
            "x": round(x1), "y": round(y1), "w": round(x2 - x1), "h": round(y2 - y1),
        })
    return objs, img.size, ms


def multipart_image(content_type, body):
    msg = BytesParser(policy=email_policy).parsebytes(
        b"Content-Type: " + content_type.encode() + b"\r\n\r\n" + body)
    for part in msg.iter_parts():
        if part.get_param("name", header="content-disposition") == "image":
            return part.get_payload(decode=True)
    return None


class Handler(BaseHTTPRequestHandler):
    model = None
    conf = 0.35

    def _reply(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._reply(200, {"status": "ok", "model": str(self.model.ckpt_path), "endpoints": ["/detect", "/v1/vision/detection"]})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        if not 0 < length <= MAX_BODY:
            return self._reply(400, {"error": "missing or too large body"})
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        try:
            if self.path.startswith("/v1/vision/detection"):
                jpeg = multipart_image(ctype, body) if ctype.startswith("multipart/") else body
                if not jpeg:
                    return self._reply(400, {"success": False, "error": "no 'image' field"})
                objs, _, ms = detect(self.model, self.conf, jpeg)
                preds = [{"label": o["label"], "confidence": o["confidence"], "x_min": o["x"], "y_min": o["y"],
                          "x_max": o["x"] + o["w"], "y_max": o["y"] + o["h"]} for o in objs]
                self._reply(200, {"success": True, "predictions": preds, "inferenceMs": round(ms)})
            elif self.path.startswith("/detect"):
                objs, size, ms = detect(self.model, self.conf, body)
                self._reply(200, {"objects": objs, "width": size[0], "height": size[1], "inference_ms": round(ms)})
            else:
                self._reply(404, {"error": "unknown endpoint"})
        except Exception as exc:  # report bad images etc. to the camera instead of dropping the connection
            self._reply(500, {"error": str(exc)})

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} {fmt % args}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--model", default="yolo11n.pt", help="any Ultralytics detection model")
    ap.add_argument("--conf", type=float, default=0.35, help="minimum confidence returned to the camera")
    args = ap.parse_args()
    Handler.model = YOLO(args.model)
    Handler.conf = args.conf
    print(f"AI server on http://{args.host}:{args.port}  model={args.model}")
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
