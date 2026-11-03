"""Render the production overlay geometry offline; never contacts a device."""
from pathlib import Path
import argparse
import hashlib
import json
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tests.test_overlay_phone import compile_layout, layout, separation


def draw(path, rows, width, height, dpi, insets):
    image = Image.new('RGB', (width, height), '#141c26')
    d = ImageDraw.Draw(image)
    left, top, right, bottom = insets
    for box in [(0, 0, left, height), (0, 0, width, top), (width-right, 0, width, height), (0, height-bottom, width, height)]:
        d.rectangle(box, fill='#552c36')
    font_path = Path('C:/Windows/Fonts/segoeui.ttf')
    font = ImageFont.truetype(str(font_path) if font_path.exists() else 'DejaVuSans.ttf', round(dpi/160*12))
    for row in rows:
        x, y, hw, hh = row['box']
        color, line = '#dce8ef', max(2, round(dpi/160))
        radius = min(hw, hh) if abs(hw-hh)<.001 or row['kind']==4 else min(hw, hh)*.35
        if row['kind']==3:
            for dx, dy, text in [(0.504, 0, '>'), (-0.504, 0, '<'), (0, .504, 'v'), (0, -.504, '^')]:
                cx, cy, r = x+dx*hw, y+dy*hw, .315*hw
                d.ellipse((cx-r, cy-r, cx+r, cy+r), outline=color, width=line)
                d.text((cx,cy), text, font=font, fill=color, anchor='mm')
        else:
            d.rounded_rectangle((x-hw,y-hh,x+hw,y+hh),radius=radius,outline=color,width=line)
            if row['kind'] in (0,1):
                r=min(hw,hh)*.38
                d.ellipse((x-r,y-r,x+r,y+r),outline=color,width=line)
            else:
                label={'minus':'-','plus':'+','menu':'Menu','ls':'L3','rs':'R3'}.get(row['name'],row['name'].upper())
                d.text((x,y),label,font=font,fill=color,anchor='mm')
    d.text((width/2,height*.43),'Offline overlay geometry',font=font,fill='#8197a8',anchor='mm')
    d.text((width/2,height*.49),f'{width} x {height}, {dpi} dpi, size 100%',font=font,fill='#8197a8',anchor='mm')
    image.save(path)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    exe=compile_layout(a.out);report={}
    for name,w,h,dpi,insets in [('phone',2800,1260,560,(88,0,0,56)),('tablet',3040,1904,440,(0,0,0,56))]:
        rows=layout(exe,w,h,dpi,insets);path=a.out/(name+'.png')
        draw(path,rows,w,h,dpi,insets)
        report[name]=dict(width=w,height=h,dpi=dpi,insets=insets,rows=rows,path=str(path),
                          minimum_clearance_px=min(separation(x,y) for i,x in enumerate(rows) for y in rows[i+1:]),
                          sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        if name=='tablet':
            old=layout(exe,w,h,dpi,insets,legacy=True);before=a.out/'tablet-before.png';draw(before,old,w,h,dpi,insets)
            assert rows==old and before.read_bytes()==path.read_bytes()
            report[name]['legacy_geometry_and_PNG_identical']=True
            report[name]['legacy_preview']=str(before)
    (a.out/'previews.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
    print(json.dumps({k:{f:v for f,v in x.items() if f!='rows'} for k,x in report.items()},indent=2))


if __name__=='__main__':main()
