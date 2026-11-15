r"""Builds the button glyph atlas of the port from Kenney's "Input Prompts" pack (CC0, www.kenney.nl).

    python port\tools\make_glyphs.py <folder of the unpacked pack> [--preview]

Writes port\runtime\icons\glyphs.png (16 x 14 cells of 96 px); with --preview also port\build\glyphs_preview.png.
The cell numbers are fixed here and in port\runtime\pc_glyphs.c:
    0..95     pad glyphs: family * 32 + Wonder.DB.UI.ButtonIcon.eID (family 0 Xbox, 1 PlayStation 5, 2 PlayStation 4);
              the four face cells (13..16) hold the buttons by their own names, A B X Y / cross circle square triangle
    96..206   keyboard keys: 96 + UnityEngine.InputSystem.Key
    208..215  mouse (whole, left, right, middle, back, forward), WASD, the four arrow keys (key_cluster)

Style: the game draws its glyphs as a black shape with a white outline and white lettering. The pack's filled
icons are white shapes with the lettering cut out, so each icon is rebuilt from its silhouette: an outline ring in
white, the inside in black, the cut-outs and the coloured highlights in white.
"""
import os, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

CELL, COLS, ROWS = 96, 16, 14
RING = 8            # outline thickness at the pack's 128 px size
UP = 2              # work at twice the size for smooth edges


def restyle(path):
    src = Image.open(path).convert('RGBA')
    big = src.resize((src.width * UP, src.height * UP), Image.LANCZOS)
    r, g, b, a = big.split()
    solid = a.point(lambda v: 255 if v >= 128 else 0)
    # the silhouette: everything the outside cannot reach through transparent pixels
    pad = Image.new('L', (big.width + 2, big.height + 2), 0)
    pad.paste(solid, (1, 1))
    ImageDraw.floodfill(pad, (0, 0), 128)
    outside = pad.crop((1, 1, big.width + 1, big.height + 1)).point(lambda v: 255 if v == 128 else 0)
    shape = ImageChops.invert(outside)
    inner = shape.filter(ImageFilter.MinFilter(2 * RING * UP + 1))
    ring = ImageChops.subtract(shape, inner)
    holes = ImageChops.multiply(shape, ImageChops.invert(solid))                  # lettering cut out of the shape
    sat = ImageChops.subtract(ImageChops.lighter(ImageChops.lighter(r, g), b), ImageChops.darker(ImageChops.darker(r, g), b))
    colour = ImageChops.multiply(sat.point(lambda v: 255 if v > 60 else 0), solid)   # the pack's red/green highlights
    white = ImageChops.lighter(ImageChops.lighter(ring, holes), colour)
    out = Image.merge('RGBA', (white, white, white, shape))
    return out.resize(src.size, Image.LANCZOS)


def fit(img, size=CELL - 4):
    box = img.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    if not box:
        return Image.new('RGBA', (CELL, CELL), (0, 0, 0, 0))
    img = img.crop(box)
    k = size / max(img.size)
    img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    cell = Image.new('RGBA', (CELL, CELL), (0, 0, 0, 0))
    cell.alpha_composite(img, ((CELL - img.width) // 2, (CELL - img.height) // 2))
    return cell


def system_button(kind):
    """The PlayStation Options and Create/Share buttons: the pack draws them as small pills, too thin to read at
    glyph size, so they are drawn here like the Xbox Menu and View buttons: a disc with the button's mark."""
    S = 4
    n = CELL * S
    img = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = n // 2
    r = 46 * S
    d.ellipse([c - r, c - r, c + r, c + r], fill=(255, 255, 255, 255))
    r -= 6 * S
    d.ellipse([c - r, c - r, c + r, c + r], fill=(0, 0, 0, 255))
    w = (255, 255, 255, 255)
    if kind == 'options':                                   # three bars
        for k in (-1, 0, 1):
            d.rounded_rectangle([c - 22 * S, c + k * 15 * S - 4 * S, c + 22 * S, c + k * 15 * S + 4 * S], radius=4 * S, fill=w)
    else:                                                   # create / share: three rays over a bar
        for dx, dy in ((-20, -8), (0, -14), (20, -8)):
            d.line([c + dx * S * 0.55, c - 2 * S, c + dx * S, c + dy * S - 12 * S], fill=w, width=7 * S)
        d.rounded_rectangle([c - 20 * S, c + 10 * S, c + 20 * S, c + 22 * S], radius=6 * S, fill=w)
    return img.resize((CELL, CELL), Image.LANCZOS)

def key_mark(path):
    """A key's lettering (or arrow) alone, white on transparent, cut tight: the restyled key without shape and ring."""
    img = restyle(path)
    a = img.getchannel('A')
    inner = a.point(lambda v: 255 if v > 250 else 0).filter(ImageFilter.MinFilter(2 * 12 + 1))   # inside the ring
    m = ImageChops.multiply(ImageChops.multiply(img.getchannel('R'), a.point(lambda v: 255 if v > 128 else 0)), inner)
    m = m.crop(m.point(lambda v: 255 if v > 40 else 0).getbbox())
    out = Image.new('RGBA', m.size, (255, 255, 255, 0))
    out.putalpha(m)
    return out


def key_cluster(marks, key=29, gap=2, ring=2.5, mark_part=0.58, S=8):
    """WASD or the arrow keys as one glyph: four keycaps, one above three, apart from each other (shrunk pack keys
    side by side overlapped and their outlines ran together). Drawn here at S times the size: rounded keycaps in the
    game's style (white ring, black inside) with the pack's lettering made big enough to read at small sizes."""
    n = CELL * S
    img = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0 = (CELL - 3 * key - 2 * gap) / 2, (CELL - 2 * key - gap) / 2
    spots = [(x0 + key + gap, y0)] + [(x0 + i * (key + gap), y0 + key + gap) for i in range(3)]
    for (x, y), m in zip(spots, marks):
        r = key * 0.24
        d.rounded_rectangle([x * S, y * S, (x + key) * S, (y + key) * S], radius=r * S, fill=(255, 255, 255, 255))
        d.rounded_rectangle([(x + ring) * S, (y + ring) * S, (x + key - ring) * S, (y + key - ring) * S],
                            radius=(r - ring * 0.7) * S, fill=(0, 0, 0, 255))
        k = mark_part * key * S / max(m.size)
        m = m.resize((round(m.width * k), round(m.height * k)), Image.LANCZOS)
        img.alpha_composite(m, (round((x + key / 2) * S - m.width / 2), round((y + key / 2) * S - m.height / 2)))
    return img.resize((CELL, CELL), Image.LANCZOS)


def pad_family(prefix, plus, minus):
    p = prefix
    face = ['a', 'b', 'x', 'y'] if p == 'xbox' else ['cross', 'circle', 'square', 'triangle']
    shoulder = {'xbox': ['xbox_lb', 'xbox_rb', 'xbox_lt', 'xbox_rt'],
                'playstation': ['playstation_trigger_l1', 'playstation_trigger_r1', 'playstation_trigger_l2', 'playstation_trigger_r2']}[p]
    d = {1: f'{p}_dpad_up', 2: f'{p}_dpad_down', 3: f'{p}_dpad_left', 4: f'{p}_dpad_right',
         5: f'{p}_dpad_up', 6: f'{p}_dpad_down', 7: f'{p}_dpad_left', 8: f'{p}_dpad_right',
         9: f'{p}_stick_l_up', 10: f'{p}_stick_l_down', 11: f'{p}_stick_l_left', 12: f'{p}_stick_l_right',
         13: f'{p}_button_{face[0]}', 14: f'{p}_button_{face[1]}', 15: f'{p}_button_{face[2]}', 16: f'{p}_button_{face[3]}',
         17: shoulder[0], 18: shoulder[1], 19: shoulder[2], 20: shoulder[3],
         21: f'{p}_stick_l', 22: f'{p}_stick_r', 23: plus, 24: minus,
         25: f'{p}_stick_r_up', 26: f'{p}_stick_r_down', 27: f'{p}_stick_r_left', 28: f'{p}_stick_r_right',
         29: f'{p}_stick_l_press', 30: f'{p}_stick_r_press', 31: f'{p}_dpad'}
    return d


KEYS = {1: 'space', 2: 'enter', 3: 'tab', 4: 'tilde', 5: 'apostrophe', 6: 'semicolon', 7: 'comma', 8: 'period', 9: 'slash_forward',
        10: 'slash_back', 11: 'bracket_open', 12: 'bracket_close', 13: 'minus', 14: 'equals',
        51: 'shift', 52: 'shift', 53: 'alt', 54: 'alt', 55: 'ctrl', 56: 'ctrl', 57: 'win', 58: 'win', 59: 'function', 60: 'escape',
        61: 'arrow_left', 62: 'arrow_right', 63: 'arrow_up', 64: 'arrow_down', 65: 'backspace', 66: 'page_down', 67: 'page_up',
        68: 'home', 69: 'end', 70: 'insert', 71: 'delete', 72: 'capslock', 73: 'numlock', 74: 'printscreen', 75: 'scroll_lock',
        76: 'pause', 77: 'numpad_enter', 78: 'slash_forward', 79: 'asterisk', 80: 'numpad_plus', 81: 'minus', 82: 'period', 83: 'equals'}
KEYS.update({15 + i: chr(ord('a') + i) for i in range(26)})
KEYS.update({41 + i: str(i + 1) for i in range(9)})
KEYS[50] = '0'
KEYS.update({84 + i: str(i) for i in range(10)})
KEYS.update({94 + i: 'f%d' % (i + 1) for i in range(12)})
MOUSE = {208: 'mouse', 209: 'mouse_left', 210: 'mouse_right', 211: 'mouse_scroll', 212: 'mouse_side_back', 213: 'mouse_side_forward'}


def main():
    root = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(here, '..', 'runtime', 'icons')
    atlas = Image.new('RGBA', (CELL * COLS, CELL * ROWS), (0, 0, 0, 0))
    missing = []

    def put(cell, folder, name):
        path = os.path.join(root, folder, 'Double', name + '.png')
        if not os.path.exists(path):
            missing.append(name)
            return None
        img = fit(restyle(path))
        atlas.paste(img, ((cell % COLS) * CELL, (cell // COLS) * CELL))
        return img

    for fam, (folder, table) in enumerate([('Xbox Series', pad_family('xbox', 'xbox_button_menu', 'xbox_button_view')),
                                           ('PlayStation Series', pad_family('playstation', 'playstation5_button_options', 'playstation5_button_create')),
                                           ('PlayStation Series', pad_family('playstation', 'playstation4_button_options', 'playstation4_button_share'))]):
        for icon, name in table.items():
            put(fam * 32 + icon, folder, name)
    for fam in (1, 2):                                      # PlayStation 5 and 4: Options (Plus), Create / Share (Minus)
        for icon, kind in ((23, 'options'), (24, 'create')):
            cell = fam * 32 + icon
            atlas.paste(system_button(kind), ((cell % COLS) * CELL, (cell // COLS) * CELL))
    for key, name in KEYS.items():
        put(96 + key, 'Keyboard & Mouse', 'keyboard_' + name)
    for cell, name in MOUSE.items():
        put(cell, 'Keyboard & Mouse', name)
    for cell, names in ((214, ('w', 'a', 's', 'd')), (215, ('arrow_up', 'arrow_left', 'arrow_down', 'arrow_right'))):
        marks = [key_mark(os.path.join(root, 'Keyboard & Mouse', 'Double', 'keyboard_' + n + '.png')) for n in names]
        atlas.paste(key_cluster(marks), ((cell % COLS) * CELL, (cell // COLS) * CELL))
    atlas.save(os.path.join(out_dir, 'glyphs.png'), optimize=True)
    if '--preview' in sys.argv:                             # the atlas on a colour, to look at
        preview = Image.new('RGBA', atlas.size, (70, 110, 150, 255))
        preview.alpha_composite(atlas)
        preview.convert('RGB').save(os.path.join(out_dir, '..', '..', 'build', 'glyphs_preview.png'))
    print('glyphs.png: %d x %d, %d bytes; missing: %s' % (atlas.width, atlas.height, os.path.getsize(os.path.join(out_dir, 'glyphs.png')), missing or 'none'))


if __name__ == '__main__':
    main()
