#!/usr/bin/env python3
"""Generate original nine-slice UI assets. Optional development dependency: Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw
palettes = {
    'dark': dict(background='#242424', highlight='#565656', border='#686868', text='#e8e8e8'),
    'light': dict(background='#f5f5f5', highlight='#c4c4c4', border='#b0b0b0', text='#333333'),
}
for mode, palette in palettes.items():
    out = Path(__file__).resolve().parents[1] / ('data/themes/sogou-soft-' + mode)
    out.mkdir(parents=True, exist_ok=True)
    scale = 4
    for name, color, radius in [('background', palette['background'], 12), ('highlight', palette['highlight'], 7)]:
        image = Image.new('RGBA', (48 * scale, 48 * scale))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle((0, 0, 48 * scale - 1, 48 * scale - 1), radius * scale, fill=color,
                               outline=palette['border'] if name == 'background' else None,
                               width=scale if name == 'background' else 1)
        image.resize((48, 48), Image.Resampling.LANCZOS).save(out / (name + '.png'))
    for name, points in [('prev', [(8, 2), (3, 7), (8, 12)]),
                         ('next', [(3, 2), (8, 7), (3, 12)]),
                         ('arrow', [(3, 2), (8, 7), (3, 12)]),
                         ('radio', [(2, 7), (5, 10), (11, 3)])]:
        image = Image.new('RGBA', (14 * scale, 14 * scale))
        ImageDraw.Draw(image).line([(x * scale, y * scale) for x, y in points], fill=palette['text'], width=2 * scale)
        image.resize((14, 14), Image.Resampling.LANCZOS).save(out / (name + '.png'))
