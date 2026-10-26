#!/usr/bin/env python3
"""Attach optional bitmap composition beneath the original fbcon renderer."""
from pathlib import Path
import argparse,shutil
a=argparse.ArgumentParser();a.add_argument('source',type=Path);args=a.parse_args()
overlay=Path(__file__).resolve().parent.parent/'source-overlay/drivers/video/fbdev/core'
p=args.source/'drivers/video/fbdev/core/fbcon.c';t=p.read_text()
if '#include "moze-fbcon.h"' not in t:
    marker='#include "fb_internal.h"'
    assert t.count(marker)==1
    t=t.replace(marker,marker+'\n#include "moze-fbcon.h"',1)
    start=t.index('static void fbcon_init(struct vc_data *vc, bool init)')
    end=t.index('static void fbcon_deinit(',start)
    part=t[start:end]
    needle='\tinfo = fbcon_info_from_console(vc->vc_num);'
    assert part.count(needle)==1
    part=part.replace(needle,needle+'\n\tmoze_native_attach(info);',1)
    t=t[:start]+part+t[end:]
    start=t.index('void fbcon_fb_unregistered(struct fb_info *info)')
    end=t.index('\n}',start)
    part=t[start:end]
    needle='\tconsole_lock();';assert part.count(needle)==1
    part=part.replace(needle,needle+'\n\tmoze_native_detach(info, false);',1)
    t=t[:start]+part+t[end:]
    p.write_text(t)
shutil.copyfile(overlay/'moze-fbcon.h',p.parent/'moze-fbcon.h')
print('DEFAULT_FBCON_RASTERIZERS_WITH_OPTIONAL_BITMAP_BACKGROUND')
