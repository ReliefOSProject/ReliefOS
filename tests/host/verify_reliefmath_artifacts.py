#!/usr/bin/env python3
"""Independent test-only SVG/PNG checker; never used in a production build."""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);args=p.parse_args()
for n in range(5):
    root=ET.parse(args.directory/f'probe-{n}.svg').getroot()
    paths=0
    for el in root.iter():
        assert el.tag.split('}')[-1] in {'svg','g','path'}
        for key,value in el.attrib.items():
            assert not key.lower().startswith('on') and not key.endswith('href') and 'url(' not in value.lower()
        if el.tag.endswith('path'):
            assert len(el.attrib.get('d',''))>100;paths+=1
    assert paths>0
    image=Image.open(args.directory/f'probe-{n}.png').convert('RGBA')
    low,high=image.getchannel('A').getextrema()
    assert low==0 and high>0
    assert image.size==(int(root.attrib['width']),int(root.attrib['height']))
print('Five independent SVG outline / PNG alpha and geometry checks passed')
