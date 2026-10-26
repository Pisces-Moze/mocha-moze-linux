#!/usr/bin/env python3
"""Install background-only hooks into the existing Mocha default earlycon."""
from pathlib import Path
import argparse, shutil
a=argparse.ArgumentParser();a.add_argument('source',type=Path);args=a.parse_args()
overlay=Path(__file__).resolve().parent.parent/'source-overlay/drivers/tty/serial'
p=args.source/'drivers/tty/serial/earlycon.c';t=p.read_text()
marker='/* Moze default-console background hook. */'
if marker not in t:
    # Replace only our previous candidate hooks, preserving the base console.
    t=t.replace('#include "moze-earlycon.h"\n','')
    t=t.replace('''    if (moze_early_enabled) {
        moze_early_write(fb, s, n);
        return;
    }
''','')
    t=t.replace('''    if (options && !strcmp(options, "moze"))
        moze_early_init((volatile u16 *)0xfda00000);
''','')
    required=['EARLYCON_DECLARE(mochafb, mocha_fb_setup);',
              'volatile u16 *fb = (volatile u16 *)0xfda00000;',
              'static unsigned int mocha_fb_row = 16, mocha_fb_col;']
    assert all(x in t for x in required),'Verified Mocha console required'
    def replace(old,new):
        global t
        assert t.count(old)==1,old
        t=t.replace(old,new,1)
    replace('static unsigned int mocha_fb_row = 16, mocha_fb_col;',
            marker+'\n#include "moze-earlycon.h"\nstatic unsigned int mocha_fb_row = 16, mocha_fb_col;')
    replace('''    for (i = 0; i < 1536 * 16; i++)
        fb[mocha_fb_row * 16 * 1536 + i] = 0;''','''    if (moze_early_enabled)
        moze_early_clear(fb, mocha_fb_row);
    else
        for (i = 0; i < 1536 * 16; i++)
            fb[mocha_fb_row * 16 * 1536 + i] = 0;''')
    replace("        if (c == '\\t') c = ' ';", "        if (c == '\\t') c = ' ';\n        if (moze_early_enabled)\n            moze_early_note(mocha_fb_col, mocha_fb_row, c);")
    replace('''(font[c * 16 + y] & (0x80 >> x)) ? 0xffff : 0;''',
            '''(font[c * 16 + y] & (0x80 >> x)) ? 0xffff :
                    moze_early_enabled ? moze_early_background(
                        mocha_fb_col * 8 + x, mocha_fb_row * 16 + y) : 0;''')
    replace('''    asm volatile("dsb sy" ::: "memory");''','''    if (moze_early_enabled)
        moze_early_tick(fb);
    asm volatile("dsb sy" ::: "memory");''')
    replace('    dev->con->write = mocha_fb_write;', '''    if (options && !strcmp(options, "moze"))
        moze_early_init((volatile u16 *)0xfda00000);
    dev->con->write = mocha_fb_write;''')
    p.write_text(t)
for name in ['moze-earlycon.h','moze-boot-art.h','moze-boot-assets.h']:
    shutil.copyfile(overlay/name,p.parent/name)
print('DEFAULT_MOCHA_EARLYCON_PRESERVED_WITH_BACKGROUND_ONLY')
