"""Draw the game's program icon (icon.ico next to this file): a ball with a banana, drawn here from plain shapes.

    python make_icon.py

Needs Pillow. The .ico is kept in the repository, so the setup does not run this; run it again to change the drawing.
The icon is the port's own picture: nothing of the game's art is used.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024                                  # drawn large, scaled down for each size
HERE = os.path.dirname(os.path.abspath(__file__))


def crescent(size, box, cut, fill):
    """An ellipse `box` minus the ellipse `cut`: the banana's body."""
    layer = Image.new('L', (size, size), 0)
    d = ImageDraw.Draw(layer)
    d.ellipse(box, fill=255)
    d.ellipse(cut, fill=0)
    return layer


def draw():
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = 40
    # the ball: dark rim, orange body, a lighter upper half, a soft highlight
    d.ellipse((m, m, S - m, S - m), fill=(92, 46, 10, 255))
    d.ellipse((m + 34, m + 34, S - m - 34, S - m - 34), fill=(250, 150, 30, 255))
    top = Image.new('L', (S, S), 0)
    td = ImageDraw.Draw(top)
    td.ellipse((m + 34, m + 34, S - m - 34, S - m - 34), fill=255)
    td.rectangle((0, S // 2 + 30, S, S), fill=0)
    im.paste((255, 196, 84, 255), (0, 0), top)
    d.rectangle((m + 34, S // 2 + 14, S - m - 34, S // 2 + 46), fill=(92, 46, 10, 255))      # the seam of the two halves
    ring = Image.new('L', (S, S), 0)
    ImageDraw.Draw(ring).ellipse((m, m, S - m, S - m), fill=255)
    im.putalpha(ring)                         # nothing outside the ball
    # the banana: outline first, then the body, the same shape a little smaller
    for grow, colour in ((30, (92, 46, 10, 255)), (0, (255, 226, 60, 255))):
        body = crescent(S, (250 - grow, 190 - grow, 830 + grow, 770 + grow), (330 + grow, 90 + grow, 960 - grow, 640 - grow), 255)
        im.paste(colour, (0, 0), body)
    shine = Image.new('L', (S, S), 0)
    ImageDraw.Draw(shine).ellipse((170, 130, 470, 330), fill=120)
    shine = shine.filter(ImageFilter.GaussianBlur(40))
    im.paste((255, 255, 255, 255), (0, 0), Image.composite(shine, Image.new('L', (S, S), 0), ring))
    return im


if __name__ == '__main__':
    big = draw()
    out = os.path.join(HERE, 'icon.ico')
    big.resize((256, 256), Image.LANCZOS).save(out, sizes=[(256, 256), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
    print('wrote', out)
