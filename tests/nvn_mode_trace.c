// Original CPU recorder/trace controls; no renderer, guest modules or GPU are linked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include "nvn_api.h"
#include "nvn_mode_trace.h"
#include "platform/platform.h"

#define PROC(n) void nvnp_##n(Cpu *)
PROC(nvnCommandBufferInitialize); PROC(nvnCommandBufferFinalize);
PROC(nvnCommandBufferAddCommandMemory); PROC(nvnCommandBufferAddControlMemory);
PROC(nvnCommandBufferBeginRecording); PROC(nvnCommandBufferEndRecording);
PROC(nvnCommandBufferSetRecordingMode); PROC(nvnCommandBufferSetGeometryProcessingMode);
unsigned rt_trace;
static unsigned checks, failures, normal_calls, config_calls, thread_calls, event_count, summary_count;
static int on, race_mode;
static uint64_t total_seen, total_emitted, total_dropped;
static unsigned overflow;
static unsigned config_int_calls, clock_calls, window_count, window_start, window_duration;
static int configured_start, configured_duration;
static uint64_t fake_clock=1000000000, window_elapsed, window_before, window_after;
static uint64_t kind_seen[NVN_MODE_EVENT_COUNT], kind_emitted[NVN_MODE_EVENT_COUNT], kind_dropped[NVN_MODE_EVENT_COUNT];
static const char *const kind_names[]={"initialize","finalize","begin","end","recording_mode","geometry_mode","prepare","handle"};
typedef struct { uint64_t sequence, cb, handle, submit, elapsed; unsigned thread, value, kind; } Captured;
static Captured captured[NVN_MODE_TRACE_LIMIT];
static HANDLE start_event, writer_inside, writer_release, flush_waiting;
static DWORD writer_tid, flush_tid;
static unsigned profile_flushes;
void perf_summary_flush(void) { profile_flushes++; }
void prof_sampler_write(void) {}
@PROF_WRITE@
#define CHECK(c) do {checks++;if(!(c)){failures++;fprintf(stderr,"line%d: %s\n",__LINE__,#c);}}while(0)
static unsigned kind(const char *name) {
    for(unsigned i=0;i<NVN_MODE_EVENT_COUNT;i++)if(!strcmp(name,kind_names[i]))return i;
    abort();
}
void rt_log(const char *format,...) {
    char text[512],name[32];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
    if(!strncmp(text,"nvn2-mode event ",16)) {
        if(event_count>=NVN_MODE_TRACE_LIMIT)abort();Captured *e=&captured[event_count++];
        if(sscanf(text,"nvn2-mode event seq=%llu type=%31s thread=%u cb=%llx handle=%llx value=%u submit=%llu elapsed_ns=%llu",
                  &e->sequence,name,&e->thread,&e->cb,&e->handle,&e->value,&e->submit,&e->elapsed)!=8)abort();
        e->kind=kind(name);
    } else if(!strncmp(text,"nvn2-mode window ",17)) {
        if(sscanf(text,"nvn2-mode window schema=2 origin=first_hook start_ms=%u duration_ms=%u elapsed_ns=%llu excluded_before=%llu excluded_after=%llu",
                  &window_start,&window_duration,&window_elapsed,&window_before,&window_after)!=5)abort();
        window_count++;
    } else if(!strncmp(text,"nvn2-mode capture ",18)) {
        unsigned limit;
        if(sscanf(text,"nvn2-mode capture seen=%llu emitted=%llu dropped=%llu limit=%u overflow=%u",
                  &total_seen,&total_emitted,&total_dropped,&limit,&overflow)!=5||limit!=NVN_MODE_TRACE_LIMIT)abort();
    } else if(!strncmp(text,"nvn2-mode summary ",18)) {
        unsigned long long a,b,c;
        if(sscanf(text,"nvn2-mode summary type=%31s seen=%llu emitted=%llu dropped=%llu",name,&a,&b,&c)!=4)abort();
        unsigned k=kind(name);kind_seen[k]=a;kind_emitted[k]=b;kind_dropped[k]=c;summary_count++;
    }
}
void rt_fatal(const char *format,...) {(void)format;abort();}
int hdr_trace_hit(const Cpu *c,const char *name) {(void)c;(void)name;return 0;}   // DKB_HDR_TRACE off
void hdr_trace_dump(const char *name,const void *p) {(void)name;(void)p;}
void nvn_called(uint8_t *seen,const char *name) {(void)seen;(void)name;__atomic_fetch_add(&normal_calls,1,__ATOMIC_RELAXED);}
int cfg_bool(const char *name) {
    if(strcmp(name,"TOTK_NVN2_MODE_TRACE"))abort();__atomic_fetch_add(&config_calls,1,__ATOMIC_RELAXED);return on;
}
int cfg_int(const char *name) {
    __atomic_fetch_add(&config_int_calls,1,__ATOMIC_RELAXED);
    if(!strcmp(name,"TOTK_NVN2_MODE_TRACE_START_MS"))return configured_start;
    if(!strcmp(name,"TOTK_NVN2_MODE_TRACE_DURATION_MS"))return configured_duration;
    abort();
}
uint64_t plat_now_ns(void) {
    __atomic_fetch_add(&clock_calls,1,__ATOMIC_RELAXED);
    return __atomic_load_n(&fake_clock,__ATOMIC_RELAXED);
}
void plat_once(PlatOnce *o,void(*fn)(void*),void *arg) {
    int32_t expected=0;
    if(__atomic_compare_exchange_n(&o->state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        fn(arg);__atomic_store_n(&o->state,2,__ATOMIC_RELEASE);
    } else while(__atomic_load_n(&o->state,__ATOMIC_ACQUIRE)!=2)SwitchToThread();
}
void plat_mutex_lock(PlatMutex *m) {
    if(race_mode&&GetCurrentThreadId()==flush_tid)SetEvent(flush_waiting);
    AcquireSRWLockExclusive((PSRWLOCK)m);
}
void plat_mutex_unlock(PlatMutex *m) {ReleaseSRWLockExclusive((PSRWLOCK)m);}
void plat_mutex_lock_shared(PlatMutex *m) {AcquireSRWLockShared((PSRWLOCK)m);}
void plat_mutex_unlock_shared(PlatMutex *m) {ReleaseSRWLockShared((PSRWLOCK)m);}
uint32_t plat_thread_id(void) {
    __atomic_fetch_add(&thread_calls,1,__ATOMIC_RELAXED);
    if(race_mode&&GetCurrentThreadId()==writer_tid) {
        SetEvent(writer_inside);if(WaitForSingleObject(writer_release,10000)!=WAIT_OBJECT_0)abort();
    }
    return GetCurrentThreadId();
}
uint64_t gfx_replayed_submits(void) {return 0;}
const GfxBackend *gfx_backend(void) {abort();}
uint64_t rt_call(Cpu *c,uint64_t fn,int n,...) {(void)c;(void)fn;(void)n;abort();}
static uint64_t call(void(*fn)(Cpu*),uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    Cpu cpu={0};cpu.x[0]=a;cpu.x[1]=b;cpu.x[2]=d;cpu.x[3]=e;fn(&cpu);return cpu.x[0];
}
static void mode(cmd_t *cb,uint32_t value,int geometry) {
    Cpu cpu={0},before;cpu.x[0]=(uintptr_t)cb;cpu.x[1]=UINT64_C(0x123400000000)|value;before=cpu;
    cmd_t saved=*cb;
    (geometry?nvnp_nvnCommandBufferSetGeometryProcessingMode:nvnp_nvnCommandBufferSetRecordingMode)(&cpu);
    CHECK(!memcmp(cb,&saved,sizeof saved));CHECK(!memcmp(&cpu,&before,sizeof cpu));
}
static cmd_t command[2];static uint8_t control[2][256],backing[4096];
static DWORD WINAPI warm(void *arg) {
    if(WaitForSingleObject(start_event,10000)!=WAIT_OBJECT_0)abort();
    nvn_mode_trace(NVN_MODE_RECORDING,(uintptr_t)arg,0,99,0);return 0;
}
static DWORD WINAPI worker(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<20000;i++) {
        Cpu c={0};c.x[0]=(uintptr_t)&command[id%2];c.x[1]=i;
        nvnp_nvnCommandBufferSetGeometryProcessingMode(&c);
    }return 0;
}
static void join(HANDLE thread) {
    if(WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0)abort();CloseHandle(thread);
}
static DWORD WINAPI race_writer(void *arg) {
    (void)arg;writer_tid=GetCurrentThreadId();nvn_mode_trace(NVN_MODE_GEOMETRY,123,0,7,0);return 0;
}
static DWORD WINAPI race_flush(void *arg) {
    (void)arg;flush_tid=GetCurrentThreadId();prof_write();return 0;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;on=strcmp(argv[1],"off")!=0;race_mode=!strcmp(argv[1],"race");
    int window_case=!strcmp(argv[1],"window")||!strcmp(argv[1],"empty")||
        !strcmp(argv[1],"open")||!strcmp(argv[1],"max")||!strcmp(argv[1],"bad-start")||!strcmp(argv[1],"bad-duration");
    if(window_case) {
        configured_start=100;configured_duration=5;
        if(!strcmp(argv[1],"bad-start"))configured_start=-1;
        if(!strcmp(argv[1],"bad-duration"))configured_duration=600001;
        if(!strcmp(argv[1],"open"))configured_duration=0;
        if(!strcmp(argv[1],"max"))configured_start=configured_duration=600000;
        nvn_mode_trace(NVN_MODE_RECORDING,1,0,0,0); // origin, excluded before start
        if(configured_start<0||configured_duration>600000) {
            nvn_mode_trace_finish();CHECK(!clock_calls&&!event_count&&!window_count&&!summary_count);
        } else if(!strcmp(argv[1],"empty")) {
            fake_clock+=20000000;nvn_mode_trace(NVN_MODE_RECORDING,1,0,0,0);
            fake_clock=1050000000;nvn_mode_trace_finish();
            CHECK(!total_seen&&!event_count&&window_before==2&&!window_after&&window_elapsed==50000000);
        } else if(!strcmp(argv[1],"max")) {
            fake_clock+=UINT64_C(600000000000);nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,1,0);
            nvn_mode_trace_finish();CHECK(total_seen==1&&event_count==1&&window_before==1&&!window_after);
            CHECK(window_start==600000&&window_duration==600000&&captured[0].elapsed==600000000000);
        } else if(!strcmp(argv[1],"open")) {
            fake_clock=1100000000;nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,1,0);
            fake_clock=UINT64_C(701000000000);nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,2,0);
            nvn_mode_trace_finish();CHECK(total_seen==2&&window_before==1&&!window_after&&window_duration==0);
        } else {
            fake_clock=1099000000;nvn_mode_trace(NVN_MODE_RECORDING,1,0,0,0);
            fake_clock=1100000000;nvn_mode_trace(NVN_MODE_RECORDING,1,0,1,0);
            fake_clock=1104999999;nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,2,0);
            fake_clock=1105000000;nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,3,0);
            fake_clock=1106000000;nvn_mode_trace(NVN_MODE_GEOMETRY,1,0,4,0);
            fake_clock=1200000000;nvn_mode_trace_finish();
            CHECK(total_seen==2&&event_count==2&&!total_dropped&&window_before==2&&window_after==2);
            CHECK(captured[0].value==1&&captured[0].elapsed==100000000&&captured[0].sequence==1);
            CHECK(captured[1].value==2&&captured[1].elapsed==104999999&&captured[1].sequence==2);
            CHECK(window_elapsed==200000000&&kind_seen[NVN_MODE_RECORDING]==1&&kind_seen[NVN_MODE_GEOMETRY]==1);
        }
        CHECK(config_calls==1&&config_int_calls==2&&!normal_calls);
    } else if(race_mode) {
        writer_inside=CreateEvent(NULL,TRUE,FALSE,NULL);writer_release=CreateEvent(NULL,TRUE,FALSE,NULL);
        flush_waiting=CreateEvent(NULL,TRUE,FALSE,NULL);
        HANDLE w=CreateThread(NULL,0,race_writer,NULL,0,NULL);
        if(WaitForSingleObject(writer_inside,10000)!=WAIT_OBJECT_0)abort();
        HANDLE f=CreateThread(NULL,0,race_flush,NULL,0,NULL);
        if(WaitForSingleObject(flush_waiting,10000)!=WAIT_OBJECT_0)abort();
        SetEvent(writer_release);join(w);join(f);
        CHECK(event_count==1&&captured[0].value==7&&captured[0].sequence==1);
        CHECK(total_seen==1&&total_emitted==1&&!total_dropped);
        CHECK(summary_count==NVN_MODE_EVENT_COUNT&&profile_flushes==1&&window_count==1&&clock_calls==3);
        nvn_mode_trace(NVN_MODE_RECORDING,456,0,9,0);nvn_mode_trace_finish();
        CHECK(event_count==1&&summary_count==NVN_MODE_EVENT_COUNT&&config_calls==1);
        CloseHandle(writer_inside);CloseHandle(writer_release);CloseHandle(flush_waiting);
    } else {
        HANDLE threads[4];start_event=CreateEvent(NULL,TRUE,FALSE,NULL);
        for(unsigned i=0;i<4;i++)threads[i]=CreateThread(NULL,0,warm,(void*)(uintptr_t)(i+1),0,NULL);
        SetEvent(start_event);for(unsigned i=0;i<4;i++)join(threads[i]);CloseHandle(start_event);
        pool_t pool={.mem=backing};uint64_t handles[2];
        for(unsigned i=0;i<2;i++) {
            cmd_t *cb=&command[i];CHECK(call(nvnp_nvnCommandBufferInitialize,(uintptr_t)cb,0,0,0)==1);
            call(nvnp_nvnCommandBufferAddCommandMemory,(uintptr_t)cb,(uintptr_t)&pool,0,sizeof backing);
            call(nvnp_nvnCommandBufferAddControlMemory,(uintptr_t)cb,(uintptr_t)control[i],sizeof control[i],0);
            call(nvnp_nvnCommandBufferBeginRecording,(uintptr_t)cb,0,0,0);
            mode(cb,i+3,0);mode(cb,i+7,1);
            handles[i]=call(nvnp_nvnCommandBufferEndRecording,(uintptr_t)cb,0,0,0);
            CHECK(nvn_handle_bytes(handles[i])==16);
            const op_hdr *h=nvn_op_first(handles[i]);CHECK(h->op==OP_BEGIN&&nvn_op_next(h)->op==OP_END);
        }
        nvn_submit_prepare(handles,2);uint64_t prepared=0;nvn_submit_prepare_async(handles+1,&prepared,1);
        // A still-queued recording with unknown deferred classification takes
        // the existing snapshot-clone path; tracing must retain that behavior.
        CHECK(prepared!=handles[1]&&prepared&&
              !memcmp((const void*)(uintptr_t)prepared,(const void*)(uintptr_t)handles[1],16));
        nvn_submit_prepare(NULL,0);CHECK(nvn_submit_count()==3);
        for(unsigned i=0;i<4;i++)threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);
        for(unsigned i=0;i<4;i++)join(threads[i]);
        for(unsigned i=0;i<2;i++) {
            call(nvnp_nvnCommandBufferFinalize,(uintptr_t)&command[i],0,0,0);
            CHECK(nvn_handle_live(handles[i]));
        }
        prof_write();CHECK(config_calls==1&&normal_calls==80016&&profile_flushes==1);
        if(!on)CHECK(!event_count&&!summary_count&&!thread_calls&&!clock_calls&&!config_int_calls);
        else {
            CHECK(window_count==1&&!window_start&&!window_duration&&!window_before&&!window_after&&config_int_calls==2);
            CHECK(event_count==NVN_MODE_TRACE_LIMIT&&total_seen==80022&&total_emitted==event_count);
            CHECK(total_dropped==total_seen-total_emitted&&overflow&&summary_count==NVN_MODE_EVENT_COUNT);
            const uint64_t expected[]={2,2,2,2,6,80002,3,3};unsigned ends=0,linked=0,modes=0;
            for(unsigned i=0;i<4;i++) {
                CHECK(captured[i].kind==NVN_MODE_RECORDING&&captured[i].value==99&&captured[i].cb>=1&&captured[i].cb<=4);
                for(unsigned j=0;j<i;j++)CHECK(captured[i].thread!=captured[j].thread&&captured[i].cb!=captured[j].cb);
            }
            for(unsigned k=0;k<NVN_MODE_EVENT_COUNT;k++) {
                CHECK(kind_seen[k]==expected[k]);CHECK(kind_seen[k]==kind_emitted[k]+kind_dropped[k]);
            }
            for(unsigned i=0;i<event_count;i++) {
                Captured *e=&captured[i];CHECK(e->sequence==i+1&&e->thread);
                if(e->kind==NVN_MODE_END){CHECK(e->handle==handles[ends]&&e->cb==(uintptr_t)&command[ends]);ends++;}
                if(e->kind==NVN_MODE_HANDLE){CHECK(e->handle==handles[linked==0?0:1]&&e->submit==(linked<2?1:2));linked++;}
                if(modes<4&&(e->kind==NVN_MODE_RECORDING||e->kind==NVN_MODE_GEOMETRY)&&e->cb==(uintptr_t)&command[modes/2]) {
                    CHECK(e->value==3+modes/2+(modes%2?4:0));modes++;
                }
            }
            CHECK(ends==2&&linked==3&&modes==4);
        }
    }
    printf("mode trace %s: %u checks, %u failures; normal_calls=%u\n",argv[1],checks,failures,normal_calls);
    return failures?1:0;
}
