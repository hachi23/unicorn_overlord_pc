/* Offline AppContent provider for the configured base-game profile.
 * Add-ons: the entitlement labels in VW_ADDCONT (run.py, scripts/dlc.py) are listed as owned
 * with no extra data; nothing is mounted or downloaded, no license is checked. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
/* SceAppContentAddcontInfo: the entitlement label (16 characters, NUL, 3 bytes padding), then
 * the download status. Add-on content without data reports NO_EXTRA_DATA, as on a console. */
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddonInfo;
_Static_assert(sizeof(AddonInfo)==24,"SceAppContentAddcontInfo");
enum { NO_EXTRA_DATA=0, MAX_ADDONS=64 };
static AddonInfo addons[MAX_ADDONS];
static unsigned addon_count;
static void load_addons(void) {
    const char *list=getenv("VW_ADDCONT");
    addon_count=0;
    while (list && *list) {
        const char *end=strchr(list,',');
        size_t n=end ? (size_t)(end-list) : strlen(list);
        if (n==16 && addon_count<MAX_ADDONS) {
            AddonInfo *a=&addons[addon_count++];
            memset(a,0,sizeof(*a)); memcpy(a->label,list,16); a->status=NO_EXTRA_DATA;
            printf("Runtime: add-on %s owned (no extra data)\n",a->label);
        } else if (n) fprintf(stderr,"Runtime: add-on label '%.*s' ignored (16 characters expected)\n",(int)n,list);
        list=end ? end+1 : NULL;
    }
}
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* The same load with module arguments and a result slot (newer SDKs). */
static ABI int32_t module_load_with_arg(uint16_t id,uint64_t argc,const void *argv,uint32_t flags,int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    int32_t r=module_load(id);
    if (result) *result=0;
    return r;
}
/* System modules are host code: no guest unwind tables (ESRCH as the kernel's variant). */
static ABI int32_t module_unwind_info(uint64_t address,int32_t flags,void *info) {
    (void)address; (void)flags;
    return info ? (int32_t)0x80020003 : (int32_t)0x8002000e;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    load_addons();
    printf("Runtime: AppContent initialized; offline base-game profile, %u add-on(s), none mounted\n",addon_count);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* Only the listed add-ons; never touch unused list slots. */
    unsigned n=0;
    if (list && capacity) {
        n=addon_count<capacity ? addon_count : capacity;
        memcpy(list,addons,n*sizeof(AddonInfo));
    } else n=addon_count;
    if (hits) *hits=n;
    if (!lists++) printf("Runtime: AppContent add-on list: %u of %u\n",n,addon_count);
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_with_arg;
    if (!strcmp(name,"4fU5yvOkVG4#libSceSysmodule")) return (uintptr_t)module_unwind_info;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
