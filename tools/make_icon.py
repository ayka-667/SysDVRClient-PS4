# Generates sce_sys/icon0.png (512x512) and sce_sys/pic1.png (1920x1080) for the PS4 package.
import os
import sys
from PIL import Image, ImageDraw, ImageFont

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sce_sys")


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf", "segoeuib.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def switch_logo(d, cx, cy, s):
    # two stylised Joy-Cons (neon red / neon blue) around a screen
    w, h = int(150 * s), int(250 * s)
    d.rounded_rectangle([cx - w - 40 * s, cy - h // 2, cx - 40 * s, cy + h // 2], radius=int(60 * s), fill=(0, 190, 240))
    d.rounded_rectangle([cx + 40 * s, cy - h // 2, cx + w + 40 * s, cy + h // 2], radius=int(60 * s), fill=(255, 60, 40))
    d.rounded_rectangle([cx - 60 * s, cy - h // 2 + 10 * s, cx + 60 * s, cy + h // 2 - 10 * s], radius=int(12 * s), fill=(30, 30, 36))


icon = Image.new("RGB", (512, 512), (20, 20, 28))
d = ImageDraw.Draw(icon)
switch_logo(d, 256, 200, 1.0)
d.text((256, 400), "SysDVR", font=font(78), fill=(240, 240, 240), anchor="mm")
d.text((256, 465), "for PS4", font=font(40), fill=(150, 150, 160), anchor="mm")
icon.save(os.path.join(root, "icon0.png"))

pic = Image.new("RGB", (1920, 1080), (18, 18, 24))
d = ImageDraw.Draw(pic)
switch_logo(d, 960, 430, 2.0)
d.text((960, 850), "SysDVR for PS4", font=font(110), fill=(235, 235, 235), anchor="mm")
pic.save(os.path.join(root, "pic1.png"))
print("ok", file=sys.stderr)
