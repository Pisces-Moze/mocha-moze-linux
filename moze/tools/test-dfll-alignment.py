#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual DFLL regulator alignment routine with regulator fixtures."""
from pathlib import Path
import subprocess
import tempfile

tree = Path(__file__).resolve().parents[2]
source = (tree / 'drivers/clk/tegra/clk-tegra124-dfll-fcpu.c').read_text()
start = source.index('static int get_alignment_from_regulator(')
end = source.index('\nstatic int tegra124_dfll_fcpu_probe(', start)
routine = source[start:end]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
struct device { int unused; };
struct regulator { int unused; } regulator;
struct rail_alignment { unsigned int step_uv, offset_uv; };
static int values[128], count, explicit_step, puts;
#define IS_ERR(x) 0
#define PTR_ERR(x) (-EINVAL)
static struct regulator *regulator_get(struct device *d, const char *s) { return &regulator; }
static void regulator_put(struct regulator *r) { puts++; }
static int regulator_list_voltage(struct regulator *r, unsigned int s) { return values[s]; }
static int regulator_count_voltages(struct regulator *r) { return count; }
static unsigned int regulator_get_linear_step(struct regulator *r) { return explicit_step; }
'''.replace('puts', 'put_count')
checks = r'''
static void reset(void) {
    for (int i=0; i<128; i++) values[i]=0;
    count=128; explicit_step=0; put_count=0;
}
static void check(int error, unsigned int offset, unsigned int step) {
    struct rail_alignment a={0};
    assert(get_alignment_from_regulator(NULL, &a)==error);
    assert(put_count==1);
    if (!error) { assert(a.offset_uv==offset); assert(a.step_uv==step); }
}
int main(void) {
    /* Existing single-range drivers retain their explicit alignment. */
    reset(); values[0]=0; explicit_step=10000; check(0,0,10000);
    /* Palmas low range, constrained to 700-1400 mV; zero selectors skipped. */
    reset(); for (int i=26; i<=96; i++) values[i]=700000+(i-26)*10000;
    check(0,700000,10000);
    /* High range plus repeated plateau selectors. */
    reset(); values[1]=values[2]=1000000;
    for (int i=7; i<=26; i++) values[i]=1020000+(i-7)*20000;
    check(0,1000000,20000);
    reset(); check(-EINVAL,0,0);
    reset(); values[1]=values[2]=900000; check(-EINVAL,0,0);
    reset(); values[0]=900000; values[1]=890000; check(-EINVAL,0,0);
    reset(); values[0]=900000; values[1]=910000; values[2]=930000; check(-EINVAL,0,0);
    reset(); values[10]=-EIO; check(-EIO,0,0);
    reset(); count=-EIO; check(-EIO,0,0);
    reset(); explicit_step=10000; values[0]=-EIO; check(-EIO,0,0);
    printf("10 actual-source DFLL alignment cases passed\n");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness + routine + checks)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Werror', '-Wno-unused-parameter',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
