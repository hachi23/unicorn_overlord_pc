"""pairs.py OUT.png: a grid of (reference | native) image pairs from the last drive, in capture order."""
import glob, os, sys
from PIL import Image
refs = sorted(glob.glob('run_uo/drive/dumps/output_*.png'), key=os.path.getmtime)
nats = sorted(glob.glob('run_uo/drive/native/native_shot_*.png'), key=os.path.getmtime)
W, H = 480, 270
rows = list(zip(refs, nats))
grid = Image.new('RGB', (2 * W + 8, len(rows) * (H + 6)), (255, 0, 255))
for i, (r, n) in enumerate(rows):
    grid.paste(Image.open(r).convert('RGB').resize((W, H)), (0, i * (H + 6)))
    grid.paste(Image.open(n).convert('RGB').resize((W, H)), (W + 8, i * (H + 6)))
grid.save(sys.argv[1])
print(len(rows), 'pairs')
