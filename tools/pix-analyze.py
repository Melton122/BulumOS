#!/usr/bin/env python3
import re, sys
data = open(sys.argv[1], "rb").read()
m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)", data)
w, h, maxv = int(m.group(1)), int(m.group(2)), int(m.group(3))
pix = data[m.end():m.end() + w * h * 3]

def px(x, y):
    o = (y * w + x) * 3
    return pix[o], pix[o + 1], pix[o + 2]

def reg(x0, y0, x1, y1, step=4):
    cols = {}
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            c = px(x, y)
            cols[c] = cols.get(c, 0) + 1
            n += 1
    top = sorted(cols.items(), key=lambda kv: -kv[1])[:5]
    return "n=%d most=%s" % (n, [(c, k) for k, c in top])

print("dims:", w, "x", h)
print("corner TL:", px(2, 2), "TR:", px(w - 3, 2))
print("center:", px(w // 2, h // 2))
print("bottom mid:", px(w // 2, h - 5))
print("unique colors:", len(set(pix)) // 3)
print("mid row strip (y=200):", reg(0, 200, w, 240))
print("top strip (y=10):", reg(0, 10, w, 20))
print("bottom strip (y=470):", reg(0, 470, w, 480))
print("vert variance center x:", [px(w // 2, y) for y in range(200, 300, 10)])