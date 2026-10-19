"""Check C rasterizer fixtures; black-only ablation is not a hardware simulation."""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


SMALL_TEXT_REGIONS = {
    "date": (8, 102, 156, 118),
    "temp_label": (188, 15, 228, 27),
    "battery_label": (256, 15, 288, 27),
    "wifi_label": (188, 79, 228, 91),
    "memory_label": (250, 79, 292, 91),
    "temp_value": (168, 32, 228, 61),
    "battery_value": (236, 32, 288, 61),
    "wifi_value": (168, 96, 228, 122),
    "memory_value": (236, 96, 288, 122),
}


def compare_readability(before_directory, directory):
    name = "clock-gray-board-reference.pgm"
    before = Image.open(before_directory / name).convert("L")
    after = Image.open(directory / name).convert("L")
    black_only = before.point([0 if value == 0 else 255 for value in range(256)])
    variants = (("Before", before), ("Before: software ablation, keep black only", black_only),
                ("After: aligned clock digits and native-size text", after))
    for scale in (1, 3):
        width, height = 296 * scale, 128 * scale
        sheet = Image.new("L", (width, (height + 20) * len(variants)), 255)
        draw = ImageDraw.Draw(sheet)
        for index, (label, image) in enumerate(variants):
            top = index * (height + 20)
            draw.text((0, top + 3), label, fill=0)
            sheet.paste(image.resize((width, height), Image.Resampling.NEAREST), (0, top + 20))
        sheet.save(directory / f"clock-readability-{scale}x.png")
    report = {}
    for label, image in (("before", before), ("after", after)):
        report[label] = {}
        for region, box in SMALL_TEXT_REGIONS.items():
            pixels = image.crop(box).tobytes()
            ink = sum(value != 255 for value in pixels)
            gray = sum(value in (85, 170) for value in pixels)
            report[label][region] = {
                "ink_pixels": ink,
                "gray_pixels": gray,
                "gray_fraction_of_ink": round(gray / ink, 4) if ink else 0,
            }
    (directory / "clock-readability.json").write_text(json.dumps(report, indent=2), encoding="ascii")
    print("PASS: before/black-only/after native and 3x typography comparisons exported")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compare-before", type=Path)
    args = parser.parse_args()
    names = ("sram", "iflash", "w25q", "04-44-am", "11-11", "04-44", "23-59",
             "board-reference", "longest-date",
             "missing", "seven-errors", "rtc-unavailable")
    images = []
    for name in names:
        path = args.directory / f"clock-gray-{name}.pgm"
        image = Image.open(path).convert("L")
        assert image.size == (296, 128), (name, image.size)
        assert set(image.tobytes()) == {0, 85, 170, 255}, name
        if name not in ("seven-errors", "rtc-unavailable"):
            for region, box in SMALL_TEXT_REGIONS.items():
                assert set(image.crop(box).tobytes()) <= {0, 255}, (name, region)
        image.save(path.with_suffix(".native.png"))
        scaled = image.resize((888, 384), Image.Resampling.NEAREST)
        scaled.save(path.with_suffix(".png"))
        images.append(scaled)
    rows = (len(images) + 1) // 2
    sheet = Image.new("L", (888 * 2, 384 * rows + 8 * (rows - 1)), 224)
    for index, image in enumerate(images):
        sheet.paste(image, ((index % 2) * 888, (index // 2) * 392))
    output = args.directory / "clock-gray-fixtures.png"
    sheet.save(output)
    print(f"PASS: {len(images)} production C fixtures, four levels: {output}")
    if args.compare_before:
        compare_readability(args.compare_before, args.directory)


if __name__ == "__main__":
    main()
