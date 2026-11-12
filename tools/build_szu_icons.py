"""Prepare macOS icon assets from SZU's original vector paths; source is preserved.
Run with the Codex bundled Python (PyMuPDF), then iconutil builds AppIcon.icns.
"""
from pathlib import Path
import hashlib
import subprocess
import pymupdf as fitz

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'src-tree/res/szu'
REVIEW = ROOT / 'build/icon-review'
REVIEW.mkdir(parents=True, exist_ok=True)
source = ASSETS / 'sources/20211124深圳大学标志updata.pdf'
doc = fitz.open(source)
page = doc[0]
bounds = fitz.Rect()
for drawing in page.get_drawings():
    bounds |= drawing['rect']

badge = fitz.open()
canvas = badge.new_page(width=384, height=384)
canvas.show_pdf_page(fitz.Rect(8, 8, 376, 376), doc, 0, clip=bounds)
badge.save(ASSETS / 'SZUEmblem.pdf', garbage=4, deflate=True)
canvas.get_pixmap(matrix=fitz.Matrix(2, 2), alpha=True).save(REVIEW / 'szu-emblem.png')

icon = fitz.open()
canvas = icon.new_page(width=1024, height=1024)
# Native macOS silhouette with a quiet white plate and the original full emblem.
canvas.draw_rect(fitz.Rect(88, 96, 936, 944), radius=0.22,
                 color=None, fill=(0, 0, 0), fill_opacity=0.10)
canvas.draw_rect(fitz.Rect(88, 88, 936, 936), radius=0.22,
                 color=(0.88, 0.88, 0.90), fill=(0.99, 0.99, 0.995), width=1.5)
canvas.show_pdf_page(fitz.Rect(188, 188, 836, 836), doc, 0, clip=bounds)
icon.save(REVIEW / 'AppIconDesign.pdf', garbage=4, deflate=True)
iconset = REVIEW / 'AppIcon.iconset'
iconset.mkdir(exist_ok=True)
for logical in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        size = logical * scale
        name = f'icon_{logical}x{logical}' + ('@2x' if scale == 2 else '') + '.png'
        canvas.get_pixmap(matrix=fitz.Matrix(size / 1024, size / 1024), alpha=True).save(iconset / name)
subprocess.run(['/usr/bin/iconutil', '-c', 'icns', str(iconset), '-o', str(ASSETS / 'AppIcon.icns')], check=True)
canvas.get_pixmap(alpha=True).save(REVIEW / 'AppIcon-preview.png')

(ASSETS / 'SOURCE.md').write_text(
    '# 深圳大学校徽素材来源\n\n'
    '- 官方标识页面：https://www.szu.edu.cn/xxgk/sdbs.htm\n'
    '- 官方压缩包：https://www.szu.edu.cn/system/_content/download.jsp?urltype=news.DownloadAttachUrl&owner=1680698289&wbfileid=4745306\n'
    '- 获取日期：2026-11-06。\n'
    '- 原件：sources/szu_logo.zip，及其中的 20211124深圳大学标志updata.pdf。\n'
    '- 使用第 1 页校徽的原有矢量路径；保留形状和原始颜色。菜单栏由 macOS 模板图像机制按系统外观显示单色。\n'
    '- 应用图标只增加原生风格底板，未修改校徽图案；没有使用 AI 重绘。\n'
    '- 原始文件 SHA-256：' + hashlib.sha256(source.read_bytes()).hexdigest() + '\n'
    '- 复现脚本：tools/build_szu_icons.py，需要 PyMuPDF 和 macOS iconutil。\n'
    '- 此来源记录不将第三方 AutoSZUWeb 项目表述为学校官方应用。\n', encoding='utf-8')
print('Official emblem bounds:', bounds)
print('Generated SZUEmblem.pdf and AppIcon.icns')
