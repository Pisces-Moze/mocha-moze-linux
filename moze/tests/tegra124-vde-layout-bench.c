// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#define tegra_vde_t124_linearize original_linearize
#include "../source-overlay/drivers/media/platform/nvidia/tegra-vde/tegra124-layout.h"
#undef tegra_vde_t124_linearize
#undef TEGRA_VDE_TEGRA124_LAYOUT_H
#include "candidates/tegra124-layout-row64.h"
static unsigned long long now(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t); return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec; }
typedef int (*fn)(void*,size_t,const void*,size_t,unsigned int,unsigned int);
static unsigned long long run(fn f,unsigned char *dst, unsigned char *src,size_t n,unsigned int pitch,unsigned int height) {
 unsigned long long start=now(); for(int i=0;i<48;i++)assert(!f(dst,n,src,n,pitch,height)); return now()-start;
}
int main(void) {
 unsigned int pitches[]={1536,768}, heights[]={1088,544};
 for(int p=0;p<2;p++) {
  size_t n=(size_t)pitches[p]*heights[p]; unsigned char *src=malloc(n),*a=malloc(n),*b=malloc(n); assert(src&&a&&b);
  for(size_t i=0;i<n;i++)src[i]=(i*13+(i>>9)*37)&255;
  original_linearize(a,n,src,n,pitches[p],heights[p]); tegra_vde_t124_linearize(b,n,src,n,pitches[p],heights[p]);assert(!memcmp(a,b,n));
  for(int trial=0;trial<5;trial++) {
   unsigned long long old,new;
   if(trial&1){new=run(tegra_vde_t124_linearize,b,src,n,pitches[p],heights[p]);old=run(original_linearize,a,src,n,pitches[p],heights[p]);}
   else {old=run(original_linearize,a,src,n,pitches[p],heights[p]);new=run(tegra_vde_t124_linearize,b,src,n,pitches[p],heights[p]);}
   assert(!memcmp(a,b,n));printf("%u %u %d %llu %llu\n",pitches[p],heights[p],trial,old,new);fflush(stdout);
  }
  free(src);free(a);free(b);
 }
}
