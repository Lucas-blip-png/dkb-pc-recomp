// Original CPU routing/render fixtures. Included after the actual Mixer types.
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

enum { FRAMES = 240, MAX_MIXES = 256, MAX_DESTINATIONS = 8, MAX_SPLITTERS = 2, MIX_UNUSED = 0x7fffffff };
static unsigned checks, failures, cases, output_calls;
static const char *case_name;
static float captured[FRAMES * 6];
static jmp_buf fatal_jump;
static int expect_fatal;
static char fatal_message[128];
#define CHECK(c) do { checks++; if (!(c)) { if (failures < 12) fprintf(stderr,"%s:%d: %s\n",case_name,__LINE__,#c); failures++; } } while (0)
static void unexpected(void) { fputs("unexpected audio/DSP provider reached\n",stderr); abort(); }
void rt_log(const char *format, ...) { (void)format; unexpected(); }
void rt_fatal(const char *format, ...) {
    va_list ap; va_start(ap,format); vsnprintf(fatal_message,sizeof fatal_message,format,ap); va_end(ap);
    if (expect_fatal) longjmp(fatal_jump,1);
    fprintf(stderr,"unexpected fatal: %s\n",fatal_message); exit(2);
}
static float host_volume(void) { return 1.0f; }
static const char *cfg_get(const char *name) { (void)name; return NULL; }   // DKB_AUDIO_DUMP stays off
static void level_note(AudioRenderer *r,Mixer *m,const float *lr,int frames) {
    (void)r;(void)m;(void)lr;(void)frames; // output metering has no routing effect
}
static void output_push(Mixer *m,const float *lr,int frames) {
    (void)m; CHECK(frames==FRAMES); if(frames!=FRAMES)abort();
    memcpy(captured,lr,(size_t)m->channels*FRAMES*sizeof(float)); output_calls++;
}
static void mix_aux(AudioRenderer *r,int mix,const AudrenEffectIn *e) { (void)r;(void)mix;(void)e;unexpected(); }
static void mix_capture(AudioRenderer *r,int mix,const AudrenEffectIn *e) { (void)r;(void)mix;(void)e;unexpected(); }
static void mix_limiter(AudioRenderer *r,int mix,int id) { (void)r;(void)mix;(void)id;unexpected(); }
static void mix_simple_effect(AudioRenderer *r,int mix,int id) { (void)r;(void)mix;(void)id;unexpected(); }
static void mix_circular_sink(AudioRenderer *r,int id) { (void)r;(void)id;unexpected(); }
Reverb3d *audio_reverb_create(const Reverb3dParam *p) { (void)p;unexpected();return NULL; }
void audio_reverb_update(Reverb3d *s,const Reverb3dParam *p) { (void)s;(void)p;unexpected(); }
void audio_reverb_process(Reverb3d *s,float *const in[6],float *const out[6],int frames) {
    (void)s;(void)in;(void)out;(void)frames;unexpected();
}

/* @PRODUCTION@ */

typedef struct { uint8_t *base;size_t size; } Allocation;
typedef struct {
    AudioRenderer r;
    Mixer *m;
    AudrenMixIn *wire_mix;
    Allocation allocations[24];unsigned allocated;
    int ids[MAX_SPLITTERS][MAX_DESTINATIONS], id_count[MAX_SPLITTERS];
    int destinations[MAX_DESTINATIONS], used[MAX_DESTINATIONS];
    float gains[MAX_DESTINATIONS];
    float *samples[2];
    int count;
} Fixture;
static void *owned(Fixture *f,size_t bytes) {
    if(f->allocated>=24)abort();
    uint8_t *p=calloc(1,bytes+32);if(!p)abort();memset(p,0xa5,16);memset(p+16+bytes,0xa5,16);
    f->allocations[f->allocated++]=(Allocation){p,bytes};return p+16;
}
static void guards(Fixture *f) {
    uint8_t sentinel[16];memset(sentinel,0xa5,sizeof sentinel);
    for(unsigned i=0;i<f->allocated;i++) {
        CHECK(!memcmp(f->allocations[i].base,sentinel,16));
        CHECK(!memcmp(f->allocations[i].base+16+f->allocations[i].size,sentinel,16));
    }
}
static void init(Fixture *f,int count,const char *name) {
    memset(f,0,sizeof *f);case_name=name;cases++;f->count=count;
    f->m=owned(f,sizeof *f->m);f->r.mixer=f->m;
    f->r.param=(AudrenParameter){.sample_rate=48000,.sample_count=FRAMES,.mix_buffer_count=count,
        .submix_count=count-1,.voice_count=2,.sink_count=1,.splitter_count=MAX_SPLITTERS,
        .splitter_destination_count=MAX_DESTINATIONS,.revision=AUDREN_REV(15)};
    f->r.revision=AUDREN_REV(15);f->m->frame=FRAMES;f->m->nbuf=count;f->m->channels=2;
    f->m->nsplit=MAX_SPLITTERS;f->m->ndest=MAX_DESTINATIONS;
    f->m->offset=owned(f,(size_t)count*sizeof *f->m->offset);
    f->m->buf=owned(f,(size_t)count*FRAMES*sizeof(float));
    f->m->split=owned(f,MAX_SPLITTERS*sizeof *f->m->split);
    f->m->dest=owned(f,MAX_DESTINATIONS*sizeof *f->m->dest);
    f->m->sink_address=owned(f,sizeof(uint64_t));
    f->m->sink_size=owned(f,sizeof(uint32_t));f->m->sink_offset=owned(f,sizeof(uint32_t));
    f->m->sink_old=owned(f,sizeof(uint32_t));f->m->sink_written=owned(f,sizeof(uint32_t));
    f->r.mixes=owned(f,(size_t)count*sizeof *f->r.mixes);
    f->wire_mix=owned(f,(size_t)count*sizeof *f->wire_mix);
    f->r.voices=owned(f,2*sizeof *f->r.voices);f->r.channels=owned(f,12*sizeof *f->r.channels);
    f->r.sinks=owned(f,sizeof *f->r.sinks);
    f->r.sinks[0]=(AudrenSinkIn){.type=SINK_DEVICE,.in_use=1,.input_count=1};
    for(int i=0;i<count;i++) {
        f->wire_mix[i]=(AudrenMixIn){.volume=1,.sample_rate=48000,.buffer_count=1,
            .in_use=1,.mix_id=i,.dest_mix_id=MIX_UNUSED,.dest_splitter_id=SPLITTER_UNUSED};
        f->wire_mix[i].mix[0][0]=1;
    }
    for(int d=0;d<MAX_DESTINATIONS;d++)f->destinations[d]=MIX_UNUSED;
    for(int v=0;v<2;v++)f->samples[v]=owned(f,FRAMES*sizeof(float));
}
static void voice(Fixture *f,int index,int mix,float value) {
    for(int i=0;i<FRAMES;i++)f->samples[index][i]=value;
    ServerVoice *v=&f->r.voices[index];v->in.in_use=1;v->in.sample_rate=48000;
    v->in.sample_format=FMT_PCMF32;v->in.channel_count=1;v->in.pitch=v->in.volume=1;
    v->in.mix_id=mix;v->in.splitter_id=SPLITTER_UNUSED;v->in.channel_ids[0]=index;
    v->state=SV_STARTED;v->valid[0]=1;
    v->buffers[0]=(AudrenWaveBuffer){.address=(uintptr_t)f->samples[index],.size=FRAMES*sizeof(float),.end=FRAMES};
    f->r.channels[index].in_use=1;f->r.channels[index].mix[0]=1;
}
static void direct(Fixture *f,int source,int destination) { f->wire_mix[source].dest_mix_id=destination; }
static void split(Fixture *f,int source,int splitter,int destination_id,int mix,float gain) {
    f->wire_mix[source].dest_splitter_id=(uint32_t)splitter;
    f->ids[splitter][f->id_count[splitter]++]=destination_id;
    f->destinations[destination_id]=mix;f->used[destination_id]=1;f->gains[destination_id]=gain;
}
static void routing(Fixture *f) {
    uint8_t wire[2048]={0},before[2048];uint32_t at=0x20;
    uint32_t header[3]={0x48444e53,MAX_SPLITTERS,MAX_DESTINATIONS};memcpy(wire,header,sizeof header);
    for(int s=0;s<MAX_SPLITTERS;s++) {
        uint32_t info[4]={0x49444e53,(uint32_t)s,48000,(uint32_t)f->id_count[s]};
        memcpy(wire+at,info,sizeof info);at+=sizeof info;
        memcpy(wire+at,f->ids[s],(size_t)f->id_count[s]*4);at+=(uint32_t)f->id_count[s]*4+12;
    }
    for(int d=0;d<MAX_DESTINATIONS;d++) {
        uint32_t magic=0x44444e53,id=(uint32_t)d;
        memcpy(wire+at,&magic,4);memcpy(wire+at+4,&id,4);
        memcpy(wire+at+8,&f->gains[d],4);memcpy(wire+at+0x68,&f->destinations[d],4);
        wire[at+0x9c]=(uint8_t)f->used[d];at+=0xa8;
    }
    at=(at+15u)&~15u;CHECK(at<=sizeof wire);memcpy(before,wire,sizeof wire);
    audio_mixer_update_routing(&f->r,wire,at,(const uint8_t *)f->wire_mix,
        (uint32_t)((size_t)f->count*sizeof *f->wire_mix),f->count);
    CHECK(!memcmp(before,wire,sizeof wire));guards(f);
}
static void render(Fixture *f,float constant,int impulse) {
    unsigned calls=output_calls;audio_mixer_render(&f->r,FRAMES/48000.0);CHECK(output_calls==calls+1);
    unsigned before=failures;
    for(int i=0;i<FRAMES;i++) {
        float expected=impulse?(i==0?0.5f:i==FRAMES-1?-0.25f:0):constant;
        CHECK(captured[2*i]==expected);CHECK(captured[2*i+1]==expected);
    }
    if(failures!=before)fprintf(stderr,"%s: expected first=%g, actual first=%g, mismatched samples=%u\n",
        case_name,(double)(impulse?.5f:constant),(double)captured[0],failures-before);
    guards(f);
}
static void done(Fixture *f) {
    for(int s=0;s<MAX_SPLITTERS;s++)free(f->m->split[s].ids);
    for(unsigned i=0;i<f->allocated;i++)free(f->allocations[i].base);
}
static void simple_cases(void) {
    Fixture f;
    init(&f,3,"splitter-submix-loss");voice(&f,0,2,.25f);direct(&f,1,0);split(&f,2,0,0,1,1);
    routing(&f);render(&f,.25f,0);
    f.destinations[0]=0;routing(&f);render(&f,.25f,0);
    f.destinations[0]=1;routing(&f);render(&f,.25f,0);
    f.r.voices[0].in.in_use=0;render(&f,0,0);
    f.r.voices[0].in.in_use=1;memset(f.samples[0],0,FRAMES*sizeof(float));f.samples[0][0]=.5f;f.samples[0][FRAMES-1]=-.25f;
    render(&f,0,1);done(&f);
    init(&f,3,"direct-chain-control");voice(&f,0,2,.25f);direct(&f,2,1);direct(&f,1,0);routing(&f);render(&f,.25f,0);done(&f);
    init(&f,3,"splitter-final-control");voice(&f,0,2,.25f);split(&f,2,0,0,0,1);routing(&f);render(&f,.25f,0);done(&f);
    init(&f,3,"reverse-IDs-control");voice(&f,0,1,.25f);split(&f,1,0,0,2,1);direct(&f,2,0);routing(&f);render(&f,.25f,0);done(&f);
    init(&f,4,"fanout");voice(&f,0,3,.25f);split(&f,3,0,0,1,.5f);split(&f,3,0,1,2,.25f);
    direct(&f,1,0);direct(&f,2,0);routing(&f);render(&f,.1875f,0);done(&f);
    init(&f,4,"shared-destination");voice(&f,0,2,.125f);voice(&f,1,3,.25f);
    split(&f,2,0,0,1,1);f.wire_mix[3].dest_splitter_id=0;direct(&f,1,0);routing(&f);render(&f,.375f,0);done(&f);
    init(&f,3,"duplicate-destination");voice(&f,0,2,.25f);direct(&f,1,0);
    split(&f,2,0,0,1,.5f);f.ids[0][f.id_count[0]++]=0;routing(&f);render(&f,.25f,0);done(&f);
    init(&f,3,"unused-self-destination");voice(&f,0,2,.25f);split(&f,2,0,0,2,1);f.used[0]=0;
    routing(&f);render(&f,0,0);done(&f);
    init(&f,3,"inactive-destination");voice(&f,0,2,.25f);split(&f,2,0,0,1,1);f.wire_mix[1].in_use=0;
    routing(&f);render(&f,0,0);done(&f);
    init(&f,3,"unused-mix-destination");voice(&f,0,2,.25f);split(&f,2,0,0,MIX_UNUSED,1);
    routing(&f);render(&f,0,0);done(&f);
    init(&f,3,"empty-splitter");voice(&f,0,2,.25f);f.wire_mix[2].dest_splitter_id=0;
    routing(&f);render(&f,0,0);done(&f);
    init(&f,3,"host-invalid-splitter-fallback");voice(&f,0,2,.25f);direct(&f,2,1);direct(&f,1,0);
    f.wire_mix[2].dest_splitter_id=MAX_SPLITTERS;routing(&f);render(&f,.25f,0);done(&f);
    init(&f,1,"final-only");voice(&f,0,0,.25f);routing(&f);render(&f,.25f,0);done(&f);
    init(&f,MAX_MIXES,"maximum-direct-chain");voice(&f,0,MAX_MIXES-1,.25f);
    for(int i=1;i<MAX_MIXES;i++)direct(&f,i,i-1);routing(&f);render(&f,.25f,0);done(&f);
}
static void cycle_case(int splitter) {
    Fixture f;init(&f,3,splitter?"splitter-cycle":"direct-cycle");
    direct(&f,1,2);
    if(splitter)split(&f,2,0,0,1,1);else direct(&f,2,1);
    routing(&f);unsigned before=output_calls;expect_fatal=1;fatal_message[0]=0;
    if(setjmp(fatal_jump)==0) {audio_mixer_render(&f.r,FRAMES/48000.0);CHECK(0);}
    else CHECK(!strcmp(fatal_message,"audio submix routing cycle"));
    expect_fatal=0;CHECK(output_calls==before);guards(&f);done(&f);
}
static void surround_case(int out_channels) {
    Fixture f;int oc=out_channels;
    init(&f,6,oc==6?"surround-passthrough":"surround-downmix");
    f.m->channels=oc;voice(&f,0,2,.25f);voice(&f,1,4,.5f);   // center and rear left; the rest is silent
    f.r.sinks[0].input_count=6;for(int c=0;c<6;c++)f.r.sinks[0].inputs[c]=(int8_t)c;
    if(oc==2){f.r.sinks[0].downmix_enabled=1;
        f.r.sinks[0].downmix[0]=1;f.r.sinks[0].downmix[1]=.5f;f.r.sinks[0].downmix[2]=.25f;f.r.sinks[0].downmix[3]=.125f;}
    routing(&f);unsigned calls=output_calls;audio_mixer_render(&f.r,FRAMES/48000.0);CHECK(output_calls==calls+1);
    for(int i=0;i<FRAMES;i++) {
        if(oc==6) {
            const float want[6]={0,0,.25f,0,.5f,0};
            for(int c=0;c<6;c++)CHECK(captured[6*i+c]==want[c]);
        } else {   // center x downmix[1], rear x downmix[3] (Eglantine DownMix6chTo2chCommand::Process)
            CHECK(captured[2*i]==.5f*.25f+.125f*.5f);CHECK(captured[2*i+1]==.5f*.25f);
        }
    }
    guards(&f);done(&f);
}
int main(void) {
    simple_cases();cycle_case(0);cycle_case(1);surround_case(6);surround_case(2);
    printf("%u checks, %u failures, %u cases (actual routing/render; no audio device)\n",checks,failures,cases);
    return failures?1:0;
}
