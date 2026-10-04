"""Synthetic camera scene (no real footage): house, tree, fence, path, person, dog, car."""
from PIL import Image, ImageDraw, ImageFilter
import random

W, H = 1024, 768


def scene(person_dx=0, seed=1):
    random.seed(seed)
    im = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):  # sky gradient
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(120 + 80 * t), int(170 + 60 * t), int(230 + 20 * t)))
    d.rectangle([0, 340, W, H], fill=(84, 138, 74))                       # lawn
    for x in range(0, W, 26):                                              # fence
        d.rectangle([x, 330, x + 10, 395], fill=(232, 226, 210))
    d.rectangle([0, 345, W, 355], fill=(232, 226, 210))
    d.rectangle([0, 372, W, 380], fill=(232, 226, 210))
    d.polygon([(40, 210), (260, 95), (480, 210)], fill=(150, 62, 52))      # roof
    d.rectangle([60, 210, 460, 470], fill=(226, 206, 160))                 # house
    d.rectangle([200, 335, 265, 470], fill=(110, 72, 44))                  # door
    d.ellipse([253, 400, 259, 406], fill=(240, 200, 60))
    for wx in (100, 340):                                                   # windows
        d.rectangle([wx, 255, wx + 80, 320], fill=(170, 205, 235), outline=(245, 245, 245), width=5)
        d.line([(wx + 40, 255), (wx + 40, 320)], fill=(245, 245, 245), width=4)
        d.line([(wx, 288), (wx + 80, 288)], fill=(245, 245, 245), width=4)
    d.rectangle([905, 250, 925, 420], fill=(110, 76, 46))                  # tree
    d.ellipse([845, 120, 985, 270], fill=(64, 118, 64))
    d.polygon([(250, 470), (330, 470), (480, H), (300, H)], fill=(168, 160, 146))   # path
    d.polygon([(560, H), (700, 520), (W, 520), (W, H)], fill=(128, 128, 132))      # driveway
    # car
    d.rounded_rectangle([565, 545, 905, 640], 22, fill=(40, 70, 150))
    d.polygon([(620, 548), (665, 485), (800, 485), (850, 548)], fill=(40, 70, 150))
    d.polygon([(640, 545), (675, 497), (735, 497), (735, 545)], fill=(180, 210, 235))
    d.polygon([(745, 545), (745, 497), (795, 497), (830, 545)], fill=(180, 210, 235))
    for cx in (640, 830):
        d.ellipse([cx - 34, 600, cx + 34, 668], fill=(30, 30, 30))
        d.ellipse([cx - 15, 619, cx + 15, 649], fill=(170, 170, 170))
    # person
    px = 450 + person_dx
    d.ellipse([px - 22, 375, px + 22, 420], fill=(232, 196, 168))
    d.rounded_rectangle([px - 38, 418, px + 38, 545], 12, fill=(196, 52, 52))
    d.rectangle([px - 30, 545, px - 6, 655], fill=(36, 44, 80))
    d.rectangle([px + 6, 545, px + 30, 655], fill=(36, 44, 80))
    d.rectangle([px + 34, 490, px + 62, 515], fill=(160, 112, 60))
    # dog
    d.ellipse([330, 590, 410, 630], fill=(150, 96, 52))
    d.ellipse([395, 570, 435, 605], fill=(150, 96, 52))
    for lx in (340, 355, 385, 398):
        d.rectangle([lx, 620, lx + 7, 655], fill=(150, 96, 52))
    im = im.filter(ImageFilter.GaussianBlur(0.8))
    px_ = im.load()                                                          # light sensor noise
    for _ in range(25000):
        x, y = random.randrange(W), random.randrange(H)
        r, g, b = px_[x, y]
        n = random.randint(-10, 10)
        px_[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)), max(0, min(255, b + n)))
    return im


# Bounding boxes (frame pixels) for the AI overlay.
BOXES = [
    {'label': 'person', 'confidence': 0.91, 'bbox': [404, 368, 518, 662]},
    {'label': 'car', 'confidence': 0.88, 'bbox': [560, 478, 912, 670]},
    {'label': 'dog', 'confidence': 0.77, 'bbox': [326, 566, 440, 660]},
]

if __name__ == '__main__':
    scene().save('scene.jpg', quality=88)
