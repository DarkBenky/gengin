from PIL import Image
import os
import struct

CHAR_SIZE = 8
CHAR_CODE_OFFSET = 32
path = "01.png"
out_dir = "../assets/chars"

# FILE FORMAT
# uint32 width
# uint32 height
# uint8[height][width]

if __name__ == "__main__":
    if not os.path.exists(out_dir):
        os.makedirs(out_dir)

    img = Image.open(path).convert("RGBA")
    width, height = img.size
    alpha_min, alpha_max = img.getchannel("A").getextrema()
    has_transparency = alpha_min < alpha_max

    for y in range(0, height, CHAR_SIZE):
        for x in range(0, width, CHAR_SIZE):
            char_img = img.crop((x, y, x + CHAR_SIZE, y + CHAR_SIZE))
            char_data = bytearray()

            for j in range(CHAR_SIZE):
                row_data = 0
                for i in range(CHAR_SIZE):
                    r, g, b, a = char_img.getpixel((i, j))
                    if has_transparency:
                        pixel_on = a > 128
                    else:
                        luminance = (r + g + b) // 3
                        pixel_on = luminance < 128

                    if pixel_on:
                        row_data |= (1 << (7 - i))
                char_data.append(row_data)

            char_index = (y // CHAR_SIZE) * (width // CHAR_SIZE) + (x // CHAR_SIZE)
            char_code = char_index + CHAR_CODE_OFFSET
            if char_code > 255:
                continue

            filename = f"{char_code:03d}.bin"
            with open(os.path.join(out_dir, filename), "wb") as f:
                f.write(struct.pack("II", CHAR_SIZE, CHAR_SIZE))
                f.write(char_data)
