# PlatformIO pre-build script: compresses the web UI so the firmware stores and serves it gzipped
# (main/web/index.html ~101 KB -> index.html.gz ~29 KB of flash). The .gz file is generated, not tracked.
import gzip
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

src = os.path.join(env.subst("$PROJECT_DIR"), "main", "web", "index.html")  # noqa: F821
dst = src + ".gz"
if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
    with open(src, "rb") as f:
        data = gzip.compress(f.read(), 9, mtime=0)
    with open(dst, "wb") as f:
        f.write(data)
    print("gzip_web: %s -> %d bytes" % (os.path.basename(dst), len(data)))
