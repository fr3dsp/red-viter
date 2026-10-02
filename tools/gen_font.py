import sys
from PIL import Image, ImageDraw, ImageFont

FONT = sys.argv[1]
SIZE = int(sys.argv[2])
OUT_H = sys.argv[3]
PREVIEW = sys.argv[4] if len(sys.argv) > 4 else None

font = ImageFont.truetype(FONT, SIZE)
glyphs = []
for code in range(32, 127):
    img = Image.new("1", (8, 16), 0)
    draw = ImageDraw.Draw(img)
    draw.fontmode = "1"
    ascent, descent = font.getmetrics()
    top = (16 - (ascent + descent)) // 2
    width = font.getlength(chr(code))
    left = max(0, int(round((8 - width) / 2)))
    draw.text((left, top), chr(code), fill=1, font=font)
    rows = []
    for y in range(16):
        byte = 0
        for x in range(8):
            if img.getpixel((x, y)):
                byte |= 0x80 >> x
        rows.append(byte)
    glyphs.append(rows)

with open(OUT_H, "w") as f:
    f.write("#ifndef VITA_FONT_H\n#define VITA_FONT_H\n\n")
    f.write("#define FONT_FIRST 32\n#define FONT_COUNT %d\n#define FONT_W 8\n#define FONT_H 16\n\n" % len(glyphs))
    f.write("static const unsigned char font_bits[FONT_COUNT][FONT_H] = {\n")
    for rows in glyphs:
        f.write("    {" + ", ".join("0x%02x" % b for b in rows) + "},\n")
    f.write("};\n\n#endif\n")

if PREVIEW:
    sheet = Image.new("L", (8 * 32, 16 * 3), 0)
    for i, rows in enumerate(glyphs):
        gx = (i % 32) * 8
        gy = (i // 32) * 16
        for y, byte in enumerate(rows):
            for x in range(8):
                if byte & (0x80 >> x):
                    sheet.putpixel((gx + x, gy + y), 255)
    sheet = sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST)
    sheet.save(PREVIEW)
