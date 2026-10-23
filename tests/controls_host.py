#!/usr/bin/env python3
"""Run production battle bridge and SDL mapping code with host-only platform stubs.
No game assets or SDK required. Includes are replaced, function bodies are unchanged.
"""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
stubs = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <assert.h>
#define l_error(...) ((void)0)
#define l_warn(...) ((void)0)
#define l_info(...) ((void)0)
#define filelog_printf(...) ((void)0)
#define APK_PATH "unused"
#define ICONS_PS3_PREFIX "PS3__"
static int so_mod, setting_controls_gamepad = 1, setting_profiler;
static uintptr_t so_symbol(void *m,const char *s) {return 0;}
static void hook_addr(uintptr_t a,uintptr_t b) {}
static uintptr_t hook_arm_trampoline(void *m,uintptr_t a,uintptr_t b) {return 0;}
static int hook_arm_short(void *m,uintptr_t a,uintptr_t b) {return -1;}
static void kuKernelCpuUnrestrictedMemcpy(void *d,const void *s,size_t n) {memcpy(d,s,n);}
static int apk_index_find(const char *a,const char *b,uint32_t *c,uint32_t *d){return -1;}
static size_t icons_movie_build(uint8_t *b,size_t n){return 0;}
static int icons_movie_count(void){return 0;}
static int fdcache_set_apk_overlay(uint32_t a,void*b,uint32_t c){return -1;}
typedef struct {uint32_t buttons; uint8_t lx,ly,rx,ry;} SceCtrlData;
static int sceCtrlPeekBufferPositive(int a,SceCtrlData *p,int n){memset(p,0,sizeof(*p));return 1;}
'''
battle_test = r'''
static int calls, seed_calls, expected_cursor;
static float motion_progress, motion_max_step, timer;
static Vec2 engine_previous;
static Vec2 to_native(Vec2 p){
    float y=p.y*720.0f;
    float correction=(1280.0f/720.0f)/(960.0f/544.0f)-1.0f;
    return (Vec2){p.x*1280.0f,y+(y-360.0f)*correction};
}
static int fallback(const uint8_t *p,Vec2 *v,float *d){
    if(battle_cursor_active && p[184]){
        assert(!p[234] && !p[284]);
        mouse_position_hook(v,0,0);engine_previous=to_native(*v);seed_calls++;
        return INPUT_TAP;
    }
    return 99;
}
static void box(HitBox *out,int slot){calls++; *out=(HitBox){100*slot,100,100*slot+80,180};}
static void mouse(Vec2 *out,const void *r,int pixels){*out=(Vec2){-1,-1};}
static void handle(const uint8_t *p){
    assert(battle_cursor_active == expected_cursor);
    if (expected_cursor == 2) {
        assert(p[184] || p[234] || p[284]);
        Vec2 v;mouse_position_hook(&v,0,0);
        assert(fabsf(v.x-(0.5f-1.5f*0.0007f/0.12f))<0.00001f);
        return;
    }
    if (expected_cursor) {
        assert(!p[184] && !p[234]); // no fake click reaches action selection
        if(p[284]){
            Vec2 v;mouse_position_hook(&v,0,0);v=to_native(v);
            float dx=v.x-engine_previous.x,dy=v.y-engine_previous.y;
            float distance=sqrtf(dx*dx+dy*dy);
            if(distance>motion_max_step)motion_max_step=distance;
            // Native gamepad gain and duration ramp, with tuning=1, duration=3.
            motion_progress+=distance*0.12f/((3.0f-0.2f)*0.85f);
            engine_previous=v;
        } else engine_previous=(Vec2){0,0}; // native idle/reset path
    }
}
static void motion_step(uint8_t *p,float x,float y,float dt){
    memcpy(p+108,&x,4);memcpy(p+112,&y,4);timer-=dt;
    battle_console_input(p);
    assert(!battle_cursor_active && !p[184] && !p[284]);
}
static float circle_progress(uint8_t *p,int fps){
    battle_motion_initialized=0;motion_progress=0;timer=3;
    motion_step(p,1,0,1.0f/fps);
    for(int i=1;i<=fps;i++){
        float angle=i*6.28318530718f/fps;
        motion_step(p,cosf(angle),sinf(angle),1.0f/fps);
    }
    return motion_progress;
}
int main(void){
    int32_t state=6, mg=0, prompts[]={2,0,3,1};
    BattlePromptArray array={4<<6,4,prompts};
    uint8_t p[724]={0}; Vec2 v; float d;
    battle_input_state=&state; battle_minigame=&mg; battle_mg_array=&array;
    get_hit_box=box; orig_get_input_type=fallback;
    for(int b=0;b<4;b++){
        memset(p,0,sizeof(p)); p[170+b]=1;
        assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_TAP);
        int slot=b==0?1:b==1?3:b==2?0:2;
        assert(v.x==slot*100+40 && v.y==140 && d==0);
    }
    assert(calls==4); // one hitbox query per press, no screen scan
    array.count_flags=1<<6; memset(p,0,sizeof(p)); p[170]=1;
    assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_TAP && v.x==-1000);
    array.count_flags=4<<6;
    memset(p,0,sizeof(p)); p[270]=1;
    assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_NONE); // held != repeated tap
    mg=12; assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_NONE); // meter still needs a press
    p[170]=1; assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_TAP);
    memset(p,0,sizeof(p)); assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_NONE);
    mg=4;
    int directions[]={SWIPE_UP,SWIPE_RIGHT,SWIPE_DOWN,SWIPE_LEFT};
    for(int i=0;i<4;i++){memset(p,0,sizeof(p));p[180+i]=1;
        assert(Battle_GetInputType_hook(p,&v,&d)==directions[i] && d==200);}
    memset(p,0,sizeof(p)); p[184]=p[234]=p[284]=1;
    assert(Battle_GetInputType_hook(p,&v,&d)==99); // Physical touch reaches the native gesture path
    state=3; assert(Battle_GetInputType_hook(p,&v,&d)==99);
    // Every concrete face-prompt family and each resolved random variant.
    for(int family=0;family<=12;family+=4){
        if(family==4)continue;
        for(int button=0;button<4;button++){
            state=6;mg=family+button;memset(p,0,sizeof(p));p[170+button]=1;
            assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_TAP);
            p[170+button]=0;p[270+button]=1;
            assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_NONE);
        }
    }
    for(int family=4;family<=16;family+=12){
        for(int direction=0;direction<4;direction++){
            mg=family+direction;memset(p,0,sizeof(p));p[180+direction]=1;
            assert(Battle_GetInputType_hook(p,&v,&d)==directions[direction]);
        }
    }
    // Analog directional gestures: four directions, hysteresis and no hold repeat.
    Vec2 flicks[]={{0,1},{1,0},{0,-1},{-1,0}};
    memset(p,0,sizeof(p));mg=4;
    for(int i=0;i<4;i++){
        battle_update_stick_direction((Vec2){0,0});
        battle_update_stick_direction(flicks[i]);
        assert(Battle_GetInputType_hook(p,&v,&d)==directions[i]);
        battle_update_stick_direction(flicks[i]);
        assert(Battle_GetInputType_hook(p,&v,&d)==INPUT_NONE);
    }
    battle_update_stick_direction((Vec2){0,0});
    state=6;mg=20;battle_mg_timer=&timer;
    orig_mouse_position=mouse;orig_battle_input=handle;expected_cursor=1;
    memset(p,0,sizeof(p));timer=10;
    motion_step(p,1,0,1.0f/60); // already deflected at entry: zero progress
    for(int i=0;i<120;i++)motion_step(p,1,0,1.0f/60);
    assert(motion_progress==0 && seed_calls==0);
    motion_step(p,0.75f,0,1.0f/60);
    assert(motion_progress>0 && motion_progress<0.02f && seed_calls==1);
    float saved=motion_progress;
    for(int i=0;i<120;i++)motion_step(p,0.75f,0,1.0f/60);
    assert(motion_progress==saved);
    timer=10;motion_step(p,-1,0,1.0f/60); // next QTE: no stale-position jump
    assert(motion_progress==saved);
    motion_step(p,1,0,1.0f/60);
    assert(motion_max_step<2.001f && motion_progress<0.2f);
    float circle30=circle_progress(p,30),circle60=circle_progress(p,60);
    assert(circle30>0.30f && circle30<0.33f);
    assert(fabsf(circle30-circle60)<0.002f);
    // Normalized diagonal stays in the unit disc; no multi-hundred-pixel jumps.
    motion_step(p,1,1,1.0f/60);assert(motion_max_step<2.001f);
    expected_cursor=2;
    p[184]=p[284]=1;battle_console_input(p);
    assert(!battle_motion_initialized && battle_motion_touch && !battle_cursor_active);
    p[184]=0;battle_console_input(p);
    p[284]=0;p[234]=1;battle_console_input(p);
    assert(!battle_motion_touch);
    p[234]=0;expected_cursor=1;
    saved=motion_progress;motion_step(p,-1,0,1.0f/60);
    assert(motion_progress==saved); // touch -> stick must not jump
    expected_cursor=0;setting_controls_gamepad=0;battle_console_input(p);
    assert(!battle_motion_initialized);
    mouse_position_hook(&v,0,0);assert(v.x==-1);
    puts("Battle bridge: taps, holds, directions, wrong buttons, touch fallback, analog flicks and movement gain at 30/60 FPS OK");
}
'''
with tempfile.TemporaryDirectory() as td:
    source = (root/'source/controls_ui.c').read_text()
    source = re.sub(r'^#include .*$', '', source, flags=re.M)
    c = Path(td)/'battle.c'; c.write_text(stubs+source+battle_test)
    exe = Path(td)/'battle'
    subprocess.run(['cc','-std=c11','-Werror=implicit-function-declaration',str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
pad_stubs = stubs + '\n'.join(f'#define SCE_CTRL_{name} (1u << {i})' for i,name in enumerate([
    'CROSS','CIRCLE','SQUARE','TRIANGLE','START','SELECT','LTRIGGER','RTRIGGER','UP','DOWN','LEFT','RIGHT'])) + r'''
#define SCE_CTRL_MODE_ANALOG_WIDE 0
#define SCE_TOUCH_PORT_BACK 1
#define SCE_TOUCH_SAMPLING_STATE_START 1
static int hardware_reads, read_ok=1;
static SceCtrlData hardware={0,128,128,128,128};
typedef struct {int reportNum; struct {int x,y;} report[4];} SceTouchData;
static SceTouchData hardware_touch;
static uint64_t sceKernelGetProcessTimeWide(void){static uint64_t t;t+=3000;return t;}
static int sceCtrlPeekBufferPositiveExt2(int a,SceCtrlData*p,int n){hardware_reads++;*p=hardware;return read_ok;}
static void sceTouchSetSamplingState(int a,int b){}
static int sceTouchPeek(int a,SceTouchData*p,int n){*p=hardware_touch;return 1;}
static void sceCtrlSetSamplingModeExt(int m){}
'''
pad_tests=r'''
static void update(void *device){
    int reads=hardware_reads;
    assert(GameControllerGetButton(fake_controller,GC_A));
    hardware.buttons=0; // hardware changes mid-update; all reads must stay coherent
    assert(GameControllerGetButton(fake_controller,GC_A));
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_LEFTX)==0);
    assert(hardware_reads==reads);
}
int main(void){
    assert(stick_axis(0)==-32768 && stick_axis(128)==0 && stick_axis(255)==32767);
    hardware.buttons=SCE_CTRL_CIRCLE;
    assert(vita_pad_set_mapping("map_a","circle"));
    assert(GameControllerGetButton(fake_controller,GC_A));
    assert(!GameControllerGetButton(fake_controller,-1));
    int before=vita_pad_map_get(MAP_A);vita_pad_map_set(MAP_A,SRC_NUM);
    assert(vita_pad_map_get(MAP_A)==before);
    hardware.buttons=SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER;
    hardware_touch.reportNum=4;
    hardware_touch.report[0].x=100;hardware_touch.report[0].y=100;
    hardware_touch.report[1].x=1000;hardware_touch.report[1].y=100;
    hardware_touch.report[2].x=100;hardware_touch.report[2].y=500;
    hardware_touch.report[3].x=1000;hardware_touch.report[3].y=500;
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_TRIGGERLEFT)==32767);
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_TRIGGERRIGHT)==32767);
    assert(GameControllerGetButton(fake_controller,GC_LEFTSTICK));
    assert(GameControllerGetButton(fake_controller,GC_RIGHTSTICK));
    hardware.buttons=0;hardware_touch.reportNum=0;
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_TRIGGERLEFT)==0);
    hardware.buttons=SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER;
    assert(GameControllerGetButton(fake_controller,GC_LEFTSHOULDER));
    assert(GameControllerGetButton(fake_controller,GC_RIGHTSHOULDER));
    hardware.buttons=0;
    assert(vita_pad_map_get(MAP_LB)==SRC_L && vita_pad_map_get(MAP_RB)==SRC_R);
    assert(vita_pad_map_get(MAP_LT)==SRC_REAR_TL && vita_pad_map_get(MAP_RT)==SRC_REAR_TR);
    hardware.buttons=SCE_CTRL_START|SCE_CTRL_LTRIGGER|SCE_CTRL_RTRIGGER;
    assert(!GameControllerGetButton(fake_controller,GC_START));
    vita_pad_set_mapping("map_a","circle");
    hardware.buttons=SCE_CTRL_CIRCLE;orig_update_controller=update;
    update_controller_hook(0);assert(!snapshot_active);
    read_ok=0;assert(GameControllerGetAxis(fake_controller,GC_AXIS_LEFTX)==0);
    assert(!GameControllerGetButton(fake_controller,GC_A));
    read_ok=1;hardware_touch.reportNum=0;
    // All remappable SDL buttons, not just A; the game itself ignores Guide.
    for(int b=0;b<GC_DPAD_UP;b++){
        vita_pad_reset_mapping();
        vita_pad_map_set(button_to_map[b],SRC_CROSS);
        hardware.buttons=SCE_CTRL_CROSS;
        assert(GameControllerGetButton(fake_controller,b));
        hardware.buttons=0;assert(!GameControllerGetButton(fake_controller,b));
    }
    int buttons[]={SCE_CTRL_UP,SCE_CTRL_DOWN,SCE_CTRL_LEFT,SCE_CTRL_RIGHT};
    for(int b=0;b<4;b++){
        hardware.buttons=buttons[b];
        assert(GameControllerGetButton(fake_controller,GC_DPAD_UP+b));
    }
    hardware.lx=0;hardware.ly=255;hardware.rx=255;hardware.ry=0;
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_LEFTX)==-32768);
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_LEFTY)==32767);
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_RIGHTX)==32767);
    assert(GameControllerGetAxis(fake_controller,GC_AXIS_RIGHTY)==-32768);
    vita_pad_reset_mapping();assert(vita_pad_map_get(MAP_A)==SRC_CROSS);
    puts("SDL bridge: remapping, rear quadrants, full axes, menu chord, coherent sampling and read failure OK");
}
'''
with tempfile.TemporaryDirectory() as td:
    source=re.sub(r'^#include .*$', '', (root/'source/vita_pad.c').read_text(),flags=re.M)
    c=Path(td)/'pad.c';c.write_text(pad_stubs+source+pad_tests)
    exe=Path(td)/'pad'
    subprocess.run(['cc','-std=c11','-Werror=implicit-function-declaration',str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
