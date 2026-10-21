/* RDX interval, release ownership, and within-Part dual-core regression. */
#include "c1_host.h"
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>

static void reset(void) { memset(trk,0,sizeof trk); memset(eng_mem_owner,0,sizeof eng_mem_owner); rdx_track_valid=rdx_track_custom=0; host_tracks_init(); }
static void blocks(unsigned n) {
 int32_t o[CTL*2]; while(n--) mix_block(o,CTL);
}
static void glide_case(unsigned mode, unsigned from, unsigned to, int gap) {
 reset(); c1h_off=1; host_preset(&trk[0],ENGI_RDX,7);
 track_t *t=&trk[0]; t->p[P_VOICE]=mode; t->p[P_GLIDE]=48;
 trk_note_on(t,from,100); blocks(1800);
 if(gap){trk_note_off(t,from); blocks(1800);}
 trk_note_on(t,to,100); blocks(1);
 rdx_note_t *n=rdx_note(t,&t->v[0]);
 int32_t a=from*65536,b=to*65536;
 assert(n->pitch_target==b);
 if(gap && mode==2) assert(n->pitch==b && !n->porta_left);
 else {
   assert(n->porta_left && (a<b ? (n->pitch>a && n->pitch<b) : (n->pitch<a && n->pitch>b)));
   int32_t prev=n->pitch;
   while(n->porta_left){blocks(1);assert(a<b ? n->pitch>=prev && n->pitch<=b : n->pitch<=prev && n->pitch>=b);prev=n->pitch;}
   assert(n->pitch==b);
 }
}
static uint64_t render_case(unsigned voices,int dual,unsigned fault) {
 reset(); c1h_off=!dual;c1h_fail_wait_at=fault;setenv("C1_MASK","auto",1);
 host_preset(&trk[0],ENGI_RDX,0);
 track_t *t=&trk[0];t->p[P_SPRD]=100;t->p[P_REV]=30;
 for(unsigned i=0;i<voices;i++)trk_note_on(t,48+i*3,100);
 uint64_t h=0xcbf29ce484222325ULL;int32_t o[CTL*2];
 for(unsigned b=0;b<300;b++){
   if(b==60)for(unsigned i=0;i<voices;i++)trk_note_off(t,48+i*3);
   mix_block(o,CTL);
   for(unsigned i=0;i<CTL*2;i++){h^=(uint32_t)o[i];h*=0x100000001b3ULL;}
 }
 const unsigned char *state=ENG_MEM(0);
 for(unsigned i=0;i<sizeof(rdx_note_t)*RDX_POLY;i++){h^=state[i];h*=0x100000001b3ULL;}
 if(dual&&voices)assert(c1h_taken>0);
 if(fault&&voices)assert(c1h_off);
 return h;
}
static uint64_t isolated(unsigned voices,int dual,unsigned fault){
 int fd[2],status;uint64_t h=0;assert(!pipe(fd));pid_t pid=fork();assert(pid>=0);
 if(!pid){h=render_case(voices,dual,fault);assert(write(fd[1],&h,sizeof h)==sizeof h);_exit(0);}
 close(fd[1]);assert(read(fd[0],&h,sizeof h)==sizeof h);close(fd[0]);waitpid(pid,&status,0);assert(status==0);return h;
}
int main(void){
 static const unsigned split[9]={0,1,2,2,3,3,3,4,4};
 for(unsigned n=0;n<=8;n++)assert(rdx_c1_voice_count(n)==split[n]);
 puts("RDX: 1..8 voice Core1 split is 1/2/2/3/3/3/4/4 ok");
 for(unsigned mode=1;mode<=2;mode++)for(unsigned gap=0;gap<2;gap++)
  for(unsigned interval=7;interval<=48;interval+= interval==7?5:12){glide_case(mode,48,48+interval,gap);glide_case(mode,48+interval,48,gap);}
 puts("RDX: FULL/LEG, overlap/gap, bidirectional 7/12/24/36/48 semitone glides ok");
 reset();c1h_off=1;host_preset(&trk[0],ENGI_RDX,7);
 assert(trk[0].p[P_GLIDE]==0);puts("RDX: original DynaLead PORTA is zero (no glide) ok");
 /* Even a zero generic release must defer to the RDX envelope. */
 trk_note_on(&trk[0],60,100);blocks(100);trk[0].p[P_REL]=0;trk_note_off(&trk[0],60);blocks(1);
 assert(trk[0].v[0].active);assert(!rdx_note(&trk[0],&trk[0].v[0])->down);
 puts("RDX: native release remains active with generic REL=0 ok");
 for(unsigned n=1;n<=8;n++){
  uint64_t h=isolated(n,0,0);assert(h==isolated(n,1,0));
  assert(h==isolated(n,1,1));assert(h==isolated(n,1,17));
  printf("RDX: %u voices serial/parallel/recovery audio and state identical\n",n);
 }
 return 0;
}
