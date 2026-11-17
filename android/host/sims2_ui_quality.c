/* Rasterize larger font sources while the original font retains all logical
 * metrics, ink rectangles and clipping. Hardware font draws use private copies
 * and a separate native atlas key; software and unsupported paths stay original.
 * The original font destructor invalidates weak cache keys before pointer reuse.
 * Only active Begin/End leases retain fonts: no permanent renderer reference cycle.
 */
#include "host.h"
#include "input.h"
#include "sims2_settings.h"

#define FONT_CLONE_CAPACITY 16
typedef struct {
    uint32_t size,style,mode,codepage,gamma,contrast,xscale,yscale;
    char filename[512];
} font_clone_config;
typedef struct {
    uint32_t original,renderer,clone,density,pins,dead;
    font_clone_config config;
} font_clone_entry;
static font_clone_entry font_clones[FONT_CLONE_CAPACITY];
static unsigned font_clone_next;

static uint32_t font_method(uint32_t font,unsigned slot,int count,const uint32_t *args)
{
    return void_call_guest_this(MEM32(MEM32(font)+slot),font,count,args);
}

static int font_clone_configure(uint32_t original,uint32_t renderer,unsigned density,font_clone_config *out)
{
    if(density<2||density>4||!original||!renderer||
        !void_vm_readable(original,0x190)||!void_vm_writable(original,0x190)||
        MEM32(original)!=0xA0E507ECu||!MEM32(original+8)||!MEM8(original+4)||
        !void_vm_readable(renderer,0x458)||MEM32(renderer)!=0xA0E4F09Cu||
        !MEM32(renderer+4)||MEM32(original+0xA4)!=renderer||MEM32(original+0xFC)!=32)return 0;
    memset(out,0,sizeof *out);
    out->size=MEM32(original+0x50);out->style=MEM32(original+0x58)&0x3C;
    out->mode=MEM32(original+0x64);out->codepage=MEM32(original+0x94);
    out->gamma=MEM32(original+0x6C);out->contrast=MEM32(original+0x70);
    out->xscale=MEM32(original+0x80);out->yscale=MEM32(original+0x84);
    if(!out->size||out->size>2048/density||!out->mode)return 0;
    float gamma,contrast,xscale,yscale;
    memcpy(&gamma,&out->gamma,4);memcpy(&contrast,&out->contrast,4);
    memcpy(&xscale,&out->xscale,4);memcpy(&yscale,&out->yscale,4);
    if(!isfinite(gamma)||gamma<0||gamma>10||!isfinite(contrast)||contrast<-1||contrast>1||
        !isfinite(xscale)||xscale<0.01f||xscale>1000||!isfinite(yscale)||yscale<0.01f||yscale>1000)return 0;
    uint32_t string=original+12,table=MEM32(string);
    if(!table||!void_vm_readable(table,24))return 0;
    uint32_t text=void_call_guest_this(MEM32(table+20),string,0,NULL);
    for(unsigned n=0;n<sizeof out->filename;++n){
        if(!void_vm_readable(text+n,1))return 0;
        out->filename[n]=(char)MEM8(text+n);
        if(!out->filename[n])return n!=0;
    }
    return 0;
}

static void font_clone_dispose(font_clone_entry *entry)
{
    uint32_t clone=entry->clone;
    /* Forget the weak key BEFORE original destruction invokes this hook again. */
    memset(entry,0,sizeof *entry);
    if(clone){uint32_t none=0;font_method(clone,0xDC,1,&none);font_method(clone,8,0,NULL);}
}

static uint32_t font_clone_create(uint32_t original,uint32_t renderer,unsigned density,const font_clone_config *config)
{
    uint32_t size=0x190;
    uint32_t clone=void_call_guest(0xA0824632u,1,&size,0); /* original cdecl operator new */
    if(!clone)return 0;
    void_call_guest_this(0xA090A7DBu,clone,0,NULL); /* normal ctor returns only */
    font_method(clone,4,0,NULL);
    uint32_t info=MEM32(clone+0x104);
    if(!info||!void_vm_readable(info,0x30)){font_method(clone,8,0,NULL);return 0;}
    uint32_t filename=original+12,scaled=config->size*density,style[2]={config->style,1};
    font_method(clone,0x58,1,&filename);
    font_method(clone,0x2C,1,&scaled);
    font_method(clone,0x20,2,style);
    font_method(clone,0x40,1,&config->mode);
    font_method(clone,0x60,1,&config->codepage);
    font_method(clone,0xDC,1,&renderer);
    int ready=font_method(clone,0xC,0,NULL)&255;
    if(ready){
        uint32_t scale[2]={config->xscale,config->yscale};
        /* This method dereferences the loaded face, so it MUST follow Init. */
        font_method(clone,0x78,2,scale);
        font_method(clone,0x48,1,&config->gamma);
        font_method(clone,0x50,1,&config->contrast);
        font_clone_config after;
        if(font_clone_configure(original,renderer,density,&after)&&!memcmp(&after,config,sizeof after))return clone;
    }
    /* Failed Init leaves initialized=false: do not rely on its UnInit to detach. */
    uint32_t none=0;font_method(clone,0xDC,1,&none);font_method(clone,8,0,NULL);return 0;
}

static uint32_t font_clone_acquire(uint32_t original,uint32_t renderer,unsigned density,unsigned *lease)
{
    void_regs saved;void_regs_save(&saved);uint32_t result=0;*lease=0;
    if(!original||!void_vm_readable(original,0x190)||MEM32(original)!=0xA0E507ECu||!MEM32(original+8))goto done;
    font_method(original,4,0,NULL); /* scoped lifetime, never a persistent cache ref */
    font_clone_config config;
    if(!font_clone_configure(original,renderer,density,&config))goto release;
    for(unsigned n=0;n<FONT_CLONE_CAPACITY;++n){
        font_clone_entry *entry=font_clones+n;
        if(!entry->dead&&entry->original==original&&entry->renderer==renderer&&entry->density==density&&
            !memcmp(&entry->config,&config,sizeof config)){
            ++entry->pins;*lease=n+1;result=entry->clone;goto done;
        }
    }
    for(unsigned i=0;i<FONT_CLONE_CAPACITY;++i){
        unsigned n=(font_clone_next+i)%FONT_CLONE_CAPACITY;font_clone_entry *entry=font_clones+n;
        if(entry->pins)continue;
        font_clone_dispose(entry);
        uint32_t clone=font_clone_create(original,renderer,density,&config);
        if(!clone)goto release;
        entry->original=original;entry->renderer=renderer;entry->clone=clone;entry->density=density;
        entry->pins=1;entry->config=config;font_clone_next=(n+1)%FONT_CLONE_CAPACITY;
        *lease=n+1;result=clone;goto done;
    }
release:
    font_method(original,8,0,NULL);
done:
    void_regs_load(&saved);return result;
}

static void font_clone_release(unsigned lease)
{
    if(!lease||lease>FONT_CLONE_CAPACITY)return;
    void_regs saved;void_regs_save(&saved);font_clone_entry *entry=font_clones+lease-1;
    if(entry->pins){
        uint32_t original=entry->original;
        if(!--entry->pins&&entry->dead)font_clone_dispose(entry);
        /* A last ref may run the original-font destructor hook: no entry access after. */
        font_method(original,8,0,NULL);
    }
    void_regs_load(&saved);
}

static void font_clone_drop_original(uint32_t original)
{
    void_regs saved;void_regs_save(&saved);
    for(unsigned n=0;n<FONT_CLONE_CAPACITY;++n){
        font_clone_entry *entry=font_clones+n;
        if(entry->original==original){entry->dead=1;if(!entry->pins)font_clone_dispose(entry);}
    }
    void_regs_load(&saved);
}



#define FONT_SCOPE_MAX 8
#define FONT_DRAW_MAX 4
#define FONT_GLYPHS_MAX 256
typedef struct {
    uint32_t renderer,font,canvas,software,offset_x,offset_y;
    int retained,font_retained;
} font_renderer_state;
typedef struct {
    uint32_t renderer,original,target,canvas,clone,clone_node,epoch;
    unsigned lease;
    int width,height,physical_width,physical_height;
    font_renderer_state parent;
} font_scope;
typedef struct {uint32_t renderer,canvas,font,records,count;} font_draw_scope;
static _Thread_local font_scope font_scopes[FONT_SCOPE_MAX];
static _Thread_local unsigned font_scope_count,font_overflow,font_draw_count;
static _Thread_local font_renderer_state font_overflow_state[FONT_SCOPE_MAX];
static _Thread_local unsigned font_overflow_states;
static _Thread_local font_draw_scope font_draw_scopes[FONT_DRAW_MAX];
static uint32_t font_logged_clones[8];
static unsigned font_clone_log_count,font_draw_log_count;
void sub_A08C2670_lifted(void);
void sub_A08C2840_lifted(void);
void sub_A08C27E0_lifted(void);
void sub_A08BB970_lifted(void);
void sub_A090A94E_lifted(void);
static uint32_t font_existing_cache(uint32_t renderer,uint32_t font);
static uint32_t font_select_cache(uint32_t renderer,uint32_t font);

static int font_renderer_valid(uint32_t renderer)
{
    return renderer&&void_vm_readable(renderer,0x458)&&void_vm_writable(renderer,0x458)&&MEM32(renderer)==0xA0E4F09Cu;
}
static int font_canvas_ref(uint32_t canvas,unsigned slot)
{
    if(!canvas||!void_vm_readable(canvas,4)||!void_vm_readable(MEM32(canvas),12))return 0;
    void_call_guest_this(MEM32(MEM32(canvas)+slot),canvas,0,NULL);return 1;
}
static void font_renderer_save(font_renderer_state *state,uint32_t renderer)
{
    memset(state,0,sizeof *state);if(!font_renderer_valid(renderer))return;
    state->renderer=renderer;uint32_t node=MEM32(renderer+0x40);state->canvas=MEM32(renderer+0x448);
    if(node!=MEM32(renderer+36)&&void_vm_readable(node,12)){
        uint32_t font=MEM32(node+8);
        if(font&&void_vm_readable(font,0x190)&&MEM32(font)==0xA0E507ECu&&MEM32(font+8)){
            state->font=font;font_method(font,4,0,NULL);state->font_retained=1;
        }
    }
    state->software=MEM8(renderer+0x44C);state->offset_x=MEM32(renderer+0x450);state->offset_y=MEM32(renderer+0x454);
    state->retained=font_canvas_ref(state->canvas,4);
}
static void font_renderer_restore(font_renderer_state *state)
{
    uint32_t renderer=state->renderer;
    if(font_renderer_valid(renderer)){
        uint32_t current=MEM32(renderer+0x448);
        if(current!=state->canvas&&(!state->canvas||state->retained)){
            if(current)font_canvas_ref(current,8);
            if(!font_renderer_valid(renderer)||MEM32(renderer+0x448)!=current)goto release;
            MEM32(renderer+0x448)=state->canvas;state->retained=0; /* transfer held reference */
        }
        uint32_t node=state->font?font_select_cache(renderer,state->font):MEM32(renderer+36);
        if(!font_renderer_valid(renderer))goto release;
        MEM32(renderer+0x40)=node?node:MEM32(renderer+36);MEM8(renderer+0x44C)=state->software;
        MEM32(renderer+0x450)=state->offset_x;MEM32(renderer+0x454)=state->offset_y;
    }
release:
    if(state->retained)font_canvas_ref(state->canvas,8);
    uint32_t font=state->font;int release_font=state->font_retained;memset(state,0,sizeof *state);
    if(release_font)font_method(font,8,0,NULL);
}
static int font_scope_current(font_scope *scope)
{
    int w,h,pw,ph;void_input_screen_size(&w,&h);s2_physical_size(&pw,&ph);
    if(!scope->lease||scope->lease>FONT_CLONE_CAPACITY)return 0;
    font_clone_entry *entry=font_clones+scope->lease-1;font_clone_config config;
    return entry->pins&&!entry->dead&&entry->original==scope->original&&entry->clone==scope->clone&&
        font_renderer_valid(scope->renderer)&&scope->epoch==s2_touch_layout_epoch()&&
        w==scope->width&&h==scope->height&&pw==scope->physical_width&&ph==scope->physical_height&&
        font_clone_configure(scope->original,scope->renderer,entry->density,&config)&&!memcmp(&config,&entry->config,sizeof config);
}
static int font_scope_hardware(font_scope *scope)
{
    return font_renderer_valid(scope->renderer)&&!MEM8(scope->renderer+0x44C)&&
        MEM32(scope->renderer+0x448)==scope->canvas&&scope->canvas&&
        void_vm_readable(scope->canvas,4)&&MEM32(scope->canvas)==0xA0E4ED70u;
}
static int font_scale_representable(uint32_t original,unsigned density)
{
    if(!original||!void_vm_readable(original,0x190)||MEM32(original)!=0xA0E507ECu)return 0;
    float x,y;uint32_t xb=MEM32(original+0x80),yb=MEM32(original+0x84);
    memcpy(&x,&xb,4);memcpy(&y,&yb,4);float size=(float)MEM32(original+0x50)*density;
    float mx=(x*65536.0f)*size,my=(y*65536.0f)*size;
    return isfinite(mx)&&isfinite(my)&&mx>=0&&my>=0&&mx<2147483648.0f&&my<2147483648.0f;
}
static unsigned font_density(int width,int height,int pw,int ph)
{
    if(width<=0||height<=0||pw<=0||ph<=0)return 1;
    unsigned n=(int64_t)pw*height<=(int64_t)ph*width?
        (unsigned)(((int64_t)pw+width-1)/width):(unsigned)(((int64_t)ph+height-1)/height);
    return n>4?4:n;
}
static uint32_t font_existing_cache(uint32_t renderer,uint32_t font)
{
    uint32_t stack=g_esp;if(stack<0x1000Cu||!void_vm_writable(stack-12,12))return 0;
    uint32_t scratch=stack-12,previous[3];memcpy(previous,GPTR(scratch),12);g_esp=scratch;
    MEM32(scratch+8)=font;uint32_t args[3]={scratch,scratch+8,font};
    uint32_t result=void_call_guest_this(0xA015AC20u,renderer+32,3,args);
    uint32_t node=MEM32(scratch+4);
    int valid=result==scratch&&node&&void_vm_readable(node,0x2C)&&MEM32(node+8)==font&&font_renderer_valid(renderer);
    memcpy(GPTR(scratch),previous,12);g_esp=stack;return valid?node:0;
}
static uint32_t font_select_cache(uint32_t renderer,uint32_t font)
{
    uint32_t stack=g_esp;if(stack<0x1000Cu||!void_vm_writable(stack-12,12))return 0;
    uint32_t scratch=stack-12,previous[3];memcpy(previous,GPTR(scratch),12);g_esp=scratch;
    MEM32(scratch+8)=font;uint32_t args[2]={scratch,scratch+8};
    uint32_t result=void_call_guest_this(0xA08C33C0u,renderer+32,2,args);
    uint32_t node=MEM32(scratch);
    int valid=result==scratch&&node&&void_vm_readable(node,0x2C)&&MEM32(node+8)==font&&font_renderer_valid(renderer);
    memcpy(GPTR(scratch),previous,12);g_esp=stack;
    if(!valid||font_existing_cache(renderer,font)!=node)return 0;
    MEM32(renderer+0x40)=node; /* exact original Begin selection field, fresh map membership */
    return node;
}

void sub_A08C2670(void)
{
    void_regs entry;void_regs_save(&entry);
    uint32_t renderer=g_ecx,original=ARG(0),target=ARG(1);
    if(font_scope_count==FONT_SCOPE_MAX||font_overflow){
        if(!font_overflow){
            font_overflow_states=0;uint32_t seen[FONT_SCOPE_MAX];unsigned seen_count=0;
            for(unsigned n=font_scope_count;n>0;--n){
                font_scope *parent=font_scopes+n-1;uint32_t r=parent->renderer;int duplicate=0;
                for(unsigned k=0;k<seen_count;++k)if(seen[k]==r)duplicate=1;
                if(duplicate)continue;seen[seen_count++]=r;
                if(parent->clone_node)font_renderer_save(font_overflow_state+font_overflow_states++,r);
            }
        }
        ++font_overflow;void_regs_load(&entry);sub_A08C2670_lifted();
        void_regs result;void_regs_save(&result);
        if(!(g_eax&255)&&!--font_overflow){
            for(unsigned n=0;n<font_overflow_states;++n)font_renderer_restore(font_overflow_state+n);
            font_overflow_states=0;
        }
        void_regs_load(&result);return;
    }
    font_scope *scope=font_scopes+font_scope_count++;memset(scope,0,sizeof *scope);
    scope->renderer=renderer;scope->original=original;scope->target=target;
    for(unsigned n=font_scope_count-1;n>0;--n)if(font_scopes[n-1].renderer==renderer){
        if(font_scopes[n-1].clone_node)font_renderer_save(&scope->parent,renderer);break;
    }
    void_regs_load(&entry);sub_A08C2670_lifted();
    void_regs result;void_regs_save(&result);
    if(!(g_eax&255)){
        if(scope->parent.renderer)font_renderer_restore(&scope->parent);
        --font_scope_count;void_regs_load(&result);return;
    }
    if(font_renderer_valid(renderer)&&!MEM8(renderer+0x44C)){
        scope->canvas=MEM32(renderer+0x448);scope->epoch=s2_touch_layout_epoch();
        void_input_screen_size(&scope->width,&scope->height);s2_physical_size(&scope->physical_width,&scope->physical_height);
        unsigned density=font_density(scope->width,scope->height,scope->physical_width,scope->physical_height);
        if(font_scope_hardware(scope)&&font_scale_representable(original,density))
            scope->clone=font_clone_acquire(original,renderer,density,&scope->lease);
        if(scope->clone&&font_scope_current(scope)&&font_scope_hardware(scope))
            scope->clone_node=font_select_cache(renderer,scope->clone);
        if(!scope->clone_node){
            if(scope->lease)font_clone_release(scope->lease);
            scope->clone=scope->lease=0;
        }else if(font_clone_log_count<8){
            int seen=0;for(unsigned n=0;n<font_clone_log_count;++n)if(font_logged_clones[n]==scope->clone)seen=1;
            if(!seen){font_logged_clones[font_clone_log_count++]=scope->clone;
                void_log("[ui-font] clone originalSize=%u cloneSize=%u density=%u hardware=1",
                    MEM32(original+0x50),MEM32(scope->clone+0x50),density);}
        }
    }
    void_regs_load(&result);
}

void sub_A08C27E0(void)
{
    uint32_t renderer=g_ecx,original=ARG(0),target=ARG(1);
    sub_A08C27E0_lifted();void_regs result;void_regs_save(&result);
    if(font_overflow){
        if(!--font_overflow){for(unsigned n=0;n<font_overflow_states;++n)font_renderer_restore(font_overflow_state+n);font_overflow_states=0;}
    }else if(font_scope_count){
        font_scope *scope=font_scopes+font_scope_count-1;
        if(scope->renderer==renderer&&scope->original==original&&scope->target==target){
            if(scope->parent.renderer)font_renderer_restore(&scope->parent);
            if(scope->lease)font_clone_release(scope->lease);
            memset(scope,0,sizeof *scope);--font_scope_count;
        }
    }
    void_regs_load(&result);
}

static uint32_t font_high_records(font_scope *scope,uint32_t records,unsigned count)
{
    if(!count||count>FONT_GLYPHS_MAX||!void_vm_readable(records,count*36))return 0;
    uint32_t scratch=void_galloc(count*36);if(!scratch)return 0;
    memcpy(GPTR(scratch),GPTR(records),count*36);
    for(unsigned n=0;n<count;++n){
        uint32_t record=scratch+n*36,code=MEM32(record+32);
        if(code>0xFFFFu)goto fail;
        uint32_t glyph=font_method(scope->clone,0xF8,1,&code);
        if(!glyph||!void_vm_readable(glyph,24))goto fail;
        MEM32(record+16)=glyph; /* deferred CPU pointer conversion until all glyph loads finish */
    }
    uint32_t cache=font_method(scope->clone,0xF4,0,NULL);
    if(!cache||!void_vm_readable(cache,20))goto fail;
    uint32_t pixels=MEM32(cache+4),pitch=MEM32(cache+16),cache_width=MEM32(cache+8),cache_height=MEM32(cache+12);
    if(!cache_width||!cache_height||pitch<cache_width)goto fail;
    for(unsigned n=0;n<count;++n){
        uint32_t record=scratch+n*36,glyph=MEM32(record+16);
        if(!void_vm_readable(glyph,24))goto fail;
        int64_t width=(int64_t)(int32_t)MEM32(glyph+8)-(int32_t)MEM32(glyph);
        int64_t height=(int64_t)(int32_t)MEM32(glyph+12)-(int32_t)MEM32(glyph+4);
        int64_t left=(int32_t)MEM32(glyph),top=(int32_t)MEM32(glyph+4);
        int64_t ink_width=(int64_t)(int32_t)MEM32(record+8)-(int32_t)MEM32(record);
        int64_t ink_height=(int64_t)(int32_t)MEM32(record+12)-(int32_t)MEM32(record+4);
        if(width<=0||height<=0||width>4096||height>4096||left<0||top<0||
            (uint64_t)left+(uint64_t)width>cache_width||(uint64_t)top+(uint64_t)height>cache_height||
            ink_width<=0||ink_height<=0)goto fail;
        uint64_t pointer=(uint64_t)pixels+(uint64_t)top*pitch+(uint64_t)left;
        uint64_t bytes=(uint64_t)(height-1)*pitch+(uint64_t)width;
        if(pointer>0xFFFFFFFFu||bytes>0xFFFFFFFFu||!void_vm_readable((uint32_t)pointer,(uint32_t)bytes))goto fail;
        MEM32(record+16)=(uint32_t)pointer;MEM32(record+20)=pitch;
        MEM32(record+24)=(uint32_t)width;MEM32(record+28)=(uint32_t)height;
        if(font_draw_log_count<16){++font_draw_log_count;
            void_log("[ui-font] source ink=%lldx%lld bitmap=%lldx%lld atlas=%ux%u hardware=1",
                (long long)ink_width,(long long)ink_height,(long long)width,(long long)height,MEM32(cache+8),MEM32(cache+12));}
    }
    return scratch;
fail:
    void_gfree(scratch);return 0;
}
static int font_high_fits_atlas(uint32_t renderer,uint32_t records,unsigned count)
{
    uint32_t atlas=MEM32(renderer+16);
    if(!atlas||!void_vm_readable(atlas,4)||!void_vm_readable(MEM32(atlas),44))return 0;
    uint32_t table=MEM32(atlas);
    uint32_t width=void_call_guest_this(MEM32(table+36),atlas,0,NULL);
    uint32_t height=void_call_guest_this(MEM32(table+40),atlas,0,NULL);
    if(!width||!height||width>8192||height>8192||!font_renderer_valid(renderer)||MEM32(renderer+16)!=atlas)return 0;
    for(unsigned n=0;n<count;++n)if(MEM32(records+n*36+24)>width||MEM32(records+n*36+28)>height)return 0;
    return 1;
}

void sub_A08C2840(void)
{
    void_regs entry;void_regs_save(&entry);
    uint32_t renderer=g_ecx,original=ARG(0),target=ARG(1),records=ARG(2),count=ARG(3);
    font_scope *scope=!font_overflow&&font_scope_count?font_scopes+font_scope_count-1:NULL;
    uint32_t scratch=0,slot=g_esp+12,font_slot=g_esp+4;int dispatch=1;
    if(scope&&scope->clone_node&&scope->renderer==renderer&&scope->original==original&&scope->target==target&&
        font_draw_count<FONT_DRAW_MAX&&font_scope_current(scope)&&font_scope_hardware(scope)&&
        font_select_cache(renderer,scope->clone)){
        scratch=font_high_records(scope,records,count);
        if(scratch&&(!font_high_fits_atlas(renderer,scratch,count)||!font_scope_current(scope)||!font_scope_hardware(scope)||
            !font_select_cache(renderer,scope->clone))){
            void_gfree(scratch);scratch=0;
        }
    }
    if(scratch){
        font_draw_scope *draw=font_draw_scopes+font_draw_count++;
        draw->renderer=renderer;draw->canvas=MEM32(renderer+0x448);draw->font=scope->clone;draw->records=scratch;draw->count=count;
        MEM32(slot)=scratch;MEM32(font_slot)=scope->clone; /* miss packer keys by Draw's font argument */
    }else if(scope&&scope->clone_node&&font_renderer_valid(renderer)&&!MEM8(renderer+0x44C))
        dispatch=font_select_cache(renderer,original)!=0; /* low records must never hit an unverified high-font key */
    void_regs_load(&entry);
    if(dispatch)sub_A08C2840_lifted();
    else{
        g_eax&=~255u;g_esp+=32; /* original thiscall7 failure return, no unsafe draw */
        if(font_draw_log_count<16){++font_draw_log_count;void_log("[ui-font] draw skipped: original font key unavailable");}
    }
    void_regs result;void_regs_save(&result);
    if(scratch){--font_draw_count;memset(font_draw_scopes+font_draw_count,0,sizeof *font_draw_scopes);MEM32(slot)=records;MEM32(font_slot)=original;void_gfree(scratch);}
    if(font_draw_count&&scope&&font_scope_current(scope)&&font_scope_hardware(scope)){
        font_draw_scope *parent=font_draw_scopes+font_draw_count-1;
        if(parent->renderer==renderer&&parent->canvas==MEM32(renderer+0x448))font_select_cache(renderer,parent->font);
    }
    void_regs_load(&result);
}

void sub_A08BB970(void)
{
    uint32_t arguments[5],sp=g_esp;int changed=0;
    font_draw_scope *draw=font_draw_count?font_draw_scopes+font_draw_count-1:NULL;
    uint32_t record=g_edi;
    if(draw&&MEM32(sp)==0xA08C2BCFu&&g_cur_func==0xA08C2840u&&g_ecx==draw->canvas&&
        font_renderer_valid(draw->renderer)&&MEM32(draw->renderer+0x448)==draw->canvas&&
        void_vm_readable(draw->canvas,4)&&MEM32(draw->canvas)==0xA0E4ED70u&&ARG(0)==MEM32(draw->renderer+16)&&
        record>=draw->records&&(record-draw->records)%36==0&&(record-draw->records)/36<draw->count){
        int64_t width=(int64_t)(int32_t)MEM32(record+8)-(int32_t)MEM32(record);
        int64_t height=(int64_t)(int32_t)MEM32(record+12)-(int32_t)MEM32(record+4);
        uint32_t high_width=MEM32(record+24),high_height=MEM32(record+28);
        if(width>0&&height>0&&high_width&&high_height&&high_width<=4096&&high_height<=4096){
            for(unsigned n=0;n<5;++n)arguments[n]=MEM32(sp+4+n*4);
            float tx,ty;memcpy(&tx,arguments+1,4);memcpy(&ty,arguments+2,4);
            float sx=(float)width/high_width,sy=(float)height/high_height;
            float left=(float)(int32_t)MEM32(record),top=(float)(int32_t)MEM32(record+4);
            tx=left-sx*(left-tx);ty=top-sy*(top-ty);
            if(isfinite(tx)&&isfinite(ty)&&isfinite(sx)&&isfinite(sy)){
                memcpy(GPTR(sp+8),&tx,4);memcpy(GPTR(sp+12),&ty,4);
                memcpy(GPTR(sp+16),&sx,4);memcpy(GPTR(sp+20),&sy,4);changed=1;
            }
        }
    }
    sub_A08BB970_lifted();
    if(changed)for(unsigned n=0;n<5;++n)MEM32(sp+4+n*4)=arguments[n];
}

void sub_A090A94E(void)
{
    void_regs entry;void_regs_save(&entry);font_clone_drop_original(g_ecx);void_regs_load(&entry);
    sub_A090A94E_lifted();
}

int s2_font_drawing(void) { return font_draw_count!=0; }
