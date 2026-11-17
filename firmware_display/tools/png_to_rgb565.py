from pathlib import Path
import sys

from PIL import Image


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: png_to_rgb565.py <input.png> <output.c> <symbol>")

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    symbol = sys.argv[3]

    image = Image.open(source).convert("RGB")
    if image.size != (800, 480):
        raise SystemExit(f"expected 800x480 input, got {image.size}")

    with output.open("w", encoding="utf-8", newline="\n") as file:
        file.write("#include \"lvgl.h\"\n\n")
        file.write(
            f"LV_ATTRIBUTE_MEM_ALIGN const uint8_t {symbol}_map[] = {{\n"
        )

        values = []
        for red, green, blue in image.getdata():
            rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            values.append(f"0x{rgb565 & 0xff:02x}, 0x{rgb565 >> 8:02x}")
            if len(values) == 12:
                file.write("  " + ", ".join(values) + ",\n")
                values.clear()

        if values:
            file.write("  " + ", ".join(values) + ",\n")

        file.write("};\n\n")
        file.write(f"const lv_image_dsc_t {symbol} = {{\n")
        file.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        file.write("    .header.cf = LV_COLOR_FORMAT_RGB565,\n")
        file.write("    .header.flags = 0,\n")
        file.write("    .header.w = 800,\n")
        file.write("    .header.h = 480,\n")
        file.write("    .header.stride = 1600,\n")
        file.write(f"    .data_size = sizeof({symbol}_map),\n")
        file.write(f"    .data = {symbol}_map,\n")
        file.write("    .reserved = NULL,\n")
        file.write("};\n")


if __name__ == "__main__":
    main()
