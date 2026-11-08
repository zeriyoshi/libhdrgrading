# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""Downloads CC0 HDR images for the light predictor.

    python3 -I scripts/predictor/fetch.py OUT [COUNT]

COUNT (default 40) equirectangular HDRIs of Poly Haven (https://polyhaven.com, every asset CC0),
spread over its list in name order (the same ones on every run), at 1k as Radiance .hdr, into
OUT (a directory of their own). OUT/SOURCES.txt lists them. The images are not committed: the
weights trained from them are (src/plugins/predict/predict_weights.c).
"""

import json
import os
import sys
import urllib.request

API = "https://api.polyhaven.com"
HEADERS = {"User-Agent": "libhdrgrading-predictor-data/0.1"}


def get(url):
    request = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = argv[1]
    count = int(argv[2]) if len(argv) > 2 else 40
    os.makedirs(out, exist_ok=True)
    ids = sorted(json.loads(get(f"{API}/assets?t=hdris")))
    step = max(1, len(ids) // count)
    chosen = ids[::step][:count]
    sources = []
    for name in chosen:
        files = json.loads(get(f"{API}/files/{name}"))
        url = files["hdri"]["1k"]["hdr"]["url"]
        path = os.path.join(out, f"{name}.hdr")
        if not os.path.exists(path):
            data = get(url)
            with open(path, "wb") as f:
                f.write(data)
        sources.append(f"{name} {url}")
        print(name)
    with open(os.path.join(out, "SOURCES.txt"), "w", encoding="utf-8") as f:
        f.write("Poly Haven HDRIs (CC0, https://polyhaven.com/license)\n" + "\n".join(sources) + "\n")
    print(f"{len(chosen)} HDRIs in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
