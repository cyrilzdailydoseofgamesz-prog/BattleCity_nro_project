#include <stdio.h>
#include <stdlib.h>
#include "../source/nes.h"
static void dump(const char *fn){
  FILE *f=fopen(fn,"wb"); int i; fprintf(f,"P6\n256 240\n255\n");
  for(i=0;i<256*240;i++){ uint32_t c=nes_fb[i]; fputc((c>>16)&255,f); fputc((c>>8)&255,f); fputc(c&255,f);} fclose(f);
}
int main(int argc,char**argv){
  FILE *f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t *d=malloc(n); fread(d,1,n,f); fclose(f);
  int r=nes_load(d,n); printf("load=%d\n",r); if(r) return 1;
  for(int fr=0;fr<700;fr++){
    nes_set_pad( (fr>=300&&fr<305)?NES_START:0 );
    if(fr>=500) nes_set_pad(((fr/30)%2)?(NES_RIGHT|NES_B):(NES_UP|NES_A));
    nes_run_frame();
    if(fr==80) dump("/tmp/f80.ppm");
    if(fr==699) dump("/tmp/f699.ppm");
  }
  return 0;
}
