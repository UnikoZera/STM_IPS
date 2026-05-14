import numpy as np
from PIL import Image

# ========== 这里改成你的文件路径和参数 ==========
bin_file_path = ""  # 改成你的二进制文件路径
width = 160
height = 80
endian = "big"  # 大端字节序，先试这个
# ==============================================

# 读取二进制数据
with open(bin_file_path, "rb") as f:
    data = f.read()

# 计算像素数，检查数据长度是否正确
pixel_count = width * height
if len(data) != pixel_count * 2:
    raise ValueError(f"数据长度不对！预期 {pixel_count*2} 字节，实际 {len(data)} 字节")

# 转成16位无符号整数（按指定字节序）
pixels = np.frombuffer(data, dtype=np.uint16).newbyteorder(endian)

# 把RGB565拆分成R/G/B通道
# R: 高5位，G: 中间6位，B: 低5位
r = ((pixels >> 11) & 0x1F) * 8  # 5位转8位（*8）
g = ((pixels >> 5) & 0x3F) * 4   # 6位转8位（*4）
b = (pixels & 0x1F) * 8         # 5位转8位（*8）

# 合并成RGB三通道
img_data = np.stack([r, g, b], axis=-1).reshape((height, width, 3)).astype(np.uint8)

# 保存图片
img = Image.fromarray(img_data, "RGB")
img.save("rgb565_decoded.png")
print("解码完成，已保存为 rgb565_decoded.png")
