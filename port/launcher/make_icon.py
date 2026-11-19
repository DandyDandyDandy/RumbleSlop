"""Make the game's program icon (build/obj/icon.ico) from a title-screen picture of the user's own game dump.

    python make_icon.py

The picture is the texture paths.ICON_TEXTURE of the title screen's bundle (a monkey in a ball, 760 x 760), read from
the extracted game (setup step "extract"). The icon is made at build time and never kept in the repository, so the
repository holds none of the game's art. build_launcher.py runs this before building the exe.

Needs UnityPy and Pillow (both in the setup's environment).
"""
import os
import sys

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import paths  # noqa: E402

BUNDLE = paths.ROMFS_DATA / 'StreamingAssets' / 'aa' / 'Switch' / 'ui_assets_ui' / 'scenetitleloop' / 'ui_view_titleloop_title.prefab.bundle'
ICO = paths.OBJ / 'icon.ico'
SIZES = (256, 64, 48, 32, 24, 16)


def picture():
    import UnityPy
    # the dump's bundle, or its converted copy in a finished game folder (a build made from the game folder alone)
    rel = BUNDLE.relative_to(paths.ROMFS_DATA)
    bundle = next((b for b in (BUNDLE, paths.DATA / rel) if b.exists()), None)
    if bundle is None:
        sys.exit(f'make_icon: {BUNDLE} is missing (run the setup step "extract" first)')
    for obj in UnityPy.load(str(bundle)).objects:
        if obj.type.name == 'Texture2D':
            tex = obj.read()
            if tex.m_Name == paths.ICON_TEXTURE:
                return tex.image.convert('RGBA')
    sys.exit(f'make_icon: no texture {paths.ICON_TEXTURE} in {BUNDLE}')


def make():
    im = picture()
    im = im.crop(im.getbbox() or (0, 0) + im.size)
    side = max(im.size)                                   # square, the picture centred
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    frames = []
    for s in SIZES:
        f = sq.resize((s, s), Image.LANCZOS)
        if s <= 48:                                       # small sizes go soft when scaled this far down
            f = f.filter(ImageFilter.UnsharpMask(radius=1, percent=60, threshold=2))
        frames.append(f)
    paths.OBJ.mkdir(parents=True, exist_ok=True)
    frames[0].save(ICO, sizes=[(s, s) for s in SIZES], append_images=frames[1:])
    return ICO


if __name__ == '__main__':
    print('wrote', make())
