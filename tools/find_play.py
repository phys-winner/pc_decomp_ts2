"""Find things on screenshots of the 1390x632 game (any storage scale).

    python tools/find_play.py SHOT.png
The family panel's Play button (the panel opens over the lot, so it moves
with the camera, and an apartment's panel is taller): the left one of the
two round deep-blue buttons side by side at its bottom. Prints "X Y" in game
pixels, or nothing.

    python tools/find_play.py --button SHOT.png X Y
Whether a dialog's text button (light periwinkle, about 174 189 255) covers
game pixel X Y: exit status 0 if so. Clicking where no dialog is pans the
neighborhood camera, so scripted dismissals check first.

    python tools/find_play.py --panel SHOT.png Y
Whether a dialog's white body crosses game row Y (the neighborhood welcome).

    python tools/find_play.py --plumbobs SHOT.png
Points under the green plumbobs over the neighborhood's family lots.

    python tools/find_play.py --tooltip SHOT.png X Y
Whether a hover tooltip (a family name) shows above game pixel X Y.
"""
import sys

from PIL import Image


def button_at(image, x, y):
    scale = image.width / 1390.0
    hits = 0
    for dx in (-16, -10, 10, 16):
        for dy in (-5, 5):
            r, g, b = image.getpixel((int((x + dx) * scale), int((y + dy) * scale)))
            hits += 140 <= r <= 205 and 160 <= g <= 215 and b >= 235
    return hits >= 4


def white_run(image, y, gap=12):
    """The longest run of light grey pixels on game row y, in game pixels,
    through gaps of up to `gap` (text): dialog bodies are a translucent
    near-white, about 225 227 213 over the neighborhood's grass."""
    scale = image.width / 1390.0
    best = run = miss = 0
    for x in range(image.width):
        r, g, b = image.getpixel((x, int(y * scale)))
        if min(r, g, b) > 195 and max(r, g, b) - min(r, g, b) < 40:
            run = run + 1 + miss if run else 1
            miss = 0
        elif run and miss < gap * scale:
            miss += 1
        else:
            run = miss = 0
        best = max(best, run)
    return best / scale


def plumbobs(image):
    """Game-pixel points just under the green plumbobs that float over the
    neighborhood's family lots, nearest the centre first."""
    scale = image.width / 1390.0
    small = image.resize((695, 316))
    seen, found = set(), []
    for y in range(40, 300):                      # below the top bar
        for x in range(150, 695):                 # right of the camera wheel
            if (x, y) in seen:
                continue
            r, g, b = small.getpixel((x, y))
            if not (g > 190 and r < 150 and b < 150):
                continue
            stack, blob = [(x, y)], []
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                blob.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < 695 and 0 <= ny < 316 and (nx, ny) not in seen:
                        pr, pg, pb = small.getpixel((nx, ny))
                        if pg > 150 and pr < 170 and pb < 170:
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            xs = [p[0] for p in blob]
            ys = [p[1] for p in blob]
            w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
            if 8 <= len(blob) <= 400 and h >= 1.4 * w:  # a diamond standing upright, not a cube
                found.append((sum(xs) * 2 // len(xs), max(ys) * 2 + 34))
    found.sort(key=lambda p: (p[0] - 695) ** 2 + (p[1] - 316) ** 2)
    return found


def tooltip_at(image, x, y):
    """A hover tooltip (a lot's family name on a translucent white box) 12-55
    game pixels above the cursor: most of that box is bright and grey."""
    scale = image.width / 1390.0
    hits = total = 0
    for gy in range(max(0, y - 55), max(0, y - 12), 3):
        for gx in range(max(0, x - 50), min(1389, x + 50), 3):
            r, g, b = image.getpixel((int(gx * scale), int(gy * scale)))
            total += 1
            hits += min(r, g, b) > 195 and max(r, g, b) - min(r, g, b) < 30
    return total > 0 and hits >= 0.4 * total


def main():
    if sys.argv[1] == "--plumbobs":
        for x, y in plumbobs(Image.open(sys.argv[2]).convert("RGB")):
            print(x, y)
        return
    if sys.argv[1] == "--tooltip":
        image = Image.open(sys.argv[2]).convert("RGB")
        sys.exit(0 if tooltip_at(image, int(sys.argv[3]), int(sys.argv[4])) else 1)
    if sys.argv[1] == "--panel":
        # A dialog's white body across game row Y (the neighborhood welcome: row 473).
        image = Image.open(sys.argv[2]).convert("RGB")
        sys.exit(0 if white_run(image, int(sys.argv[3])) >= 300 else 1)
    if sys.argv[1] == "--button":
        image = Image.open(sys.argv[2]).convert("RGB")
        sys.exit(0 if button_at(image, int(sys.argv[3]), int(sys.argv[4])) else 1)
    # The panel's round buttons (Play, then Close) are filled with a deep blue
    # (about 0 0 110) nothing else on the neighborhood view has, in the band
    # the panel bottom sits in (house and apartment panels differ in height).
    image = Image.open(sys.argv[1]).convert("RGB")
    scale = image.width / 1390.0
    points = []
    for gy in range(515, 580, 2):
        for gx in range(300, 1350, 2):
            r, g, b = image.getpixel((int(gx * scale), int(gy * scale)))
            if r < 30 and g < 50 and 90 < b < 140:
                points.append((gx, gy))
    points.sort()
    clusters, current = [], []
    for p in points:
        if current and p[0] - current[-1][0] > 16:
            clusters.append(current)
            current = []
        current.append(p)
    if current:
        clusters.append(current)
    centres = [(sum(p[0] for p in c) / len(c), sum(p[1] for p in c) / len(c)) for c in clusters if len(c) >= 12]
    # Play and Close sit side by side, about 96 game pixels apart (the info
    # icon above them is the same blue).
    for i, (x0, y0) in enumerate(centres):
        for x1, y1 in centres[i + 1:]:
            if 80 <= x1 - x0 <= 115 and abs(y1 - y0) < 8:
                print(int(x0), int(y0))
                return


if __name__ == "__main__":
    main()
