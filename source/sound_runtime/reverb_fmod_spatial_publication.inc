/* Optional bootstrap-only boundary for Journey's exact FMOD 1.10.08.
 * Verified against SHA256 8e5ec483da986ced5bb229426cf21746824f12123e1a34388083a48ca52f9943.
 * No DLL bytes are changed. Never use these private RVAs before the file and
 * loaded entry bytes pass the identity gate.
 */
#include "reverb_sha256.h"
static int (__cdecl *g_spatial_validate)(void *, void **, void **);
static int (__cdecl *g_spatial_update)(void *, unsigned int, int);
static void (__cdecl *g_spatial_scope_end)(void *);
static int (__cdecl *g_spatial_lock)(void *);
static int (__cdecl *g_spatial_unlock)(void *);

static int spatial_bytes_equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i; for (i=0;i<n;++i) if(a[i]!=b[i]) return 0; return 1;
}
static int spatial_fmod_identity(JourneyModule fmod)
{
    typedef unsigned long (__cdecl *ModulePath)(void *, uint16_t *, unsigned long);
    typedef void *(__cdecl *OpenFile)(const uint16_t *, unsigned long, unsigned long, void *, unsigned long, unsigned long, void *);
    typedef int (__cdecl *ReadFile)(void *, void *, unsigned long, unsigned long *, void *);
    typedef int (__cdecl *CloseFile)(void *);
    /* Full-file identity plus three entry-point fingerprints protects the
     * private FMOD RVAs below. These bytes are validation data, not DSP/audio
     * content. */
    static const uint8_t expected[32]={0x8e,0x5e,0xc4,0x83,0xda,0x98,0x6c,0xed,0x5b,0xb2,0x29,0x42,0x6c,0xf2,0x17,0x46,0x82,0x4f,0x12,0x12,0x3e,0x1a,0x34,0x38,0x80,0x83,0xa4,0x8c,0xa5,0x2f,0x99,0x43};
    static const uint8_t entry[3][16]={
        {0xf6,0xc1,0x01,0x0f,0x85,0xa7,0xc4,0x0b,0x00,0xe9,0xf2,0x84,0x0b,0x00,0xcc,0xcc},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x40,0x45},
        {0x48,0x83,0xec,0x28,0x48,0x8b,0x89,0xe8,0x14,0x01,0x00,0x48,0x85,0xc9,0x74,0x09}};
    /* validate/publication-scope entry, internal graph update, scope release */
    static const uint32_t rva[3]={0x11850,0xc9850,0xfe900};
    JourneyModule kernel=GetModuleHandleA("kernel32.dll");
    ModulePath module_path; OpenFile open_file; ReadFile read_file; CloseFile close_file;
    JourneyProc p; uint16_t path[1024]; uint8_t buffer[512], digest[32];
    unsigned long length, count; unsigned i; int ok=1;
    void *file; JourneySha256 sha;
#define SPATIAL_RESOLVE(variable,name) do { p=resolve(kernel,name); if(!p)return 0; memcpy(&(variable),&p,sizeof(p)); } while(0)
    SPATIAL_RESOLVE(module_path,"GetModuleFileNameW");
    SPATIAL_RESOLVE(open_file,"CreateFileW");
    SPATIAL_RESOLVE(read_file,"ReadFile");
    SPATIAL_RESOLVE(close_file,"CloseHandle");
#undef SPATIAL_RESOLVE
    length=module_path(fmod,path,1024);
    if(!length || length>=1024) return 0;
    /* GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL. */
    file=open_file(path,0x80000000UL,1UL,NULL,3UL,0x80UL,NULL);
    if(file==(void *)(intptr_t)-1) return 0;
    journey_sha_init(&sha);
    do {
        count=0;
        if(!read_file(file,buffer,sizeof(buffer),&count,NULL) || count>sizeof(buffer)) {ok=0;break;}
        journey_sha_update(&sha,buffer,count);
        if(sha.bytes>1756672U) {ok=0;break;}
    } while(count);
    (void)close_file(file);
    if(!ok || sha.bytes!=1756672U) return 0;
    journey_sha_finish(&sha,digest);
    if(!spatial_bytes_equal(digest,expected,32))return 0;
    for(i=0;i<3;++i) if(!spatial_bytes_equal((const uint8_t *)fmod+rva[i],entry[i],16))return 0;
    return 1;
}

/* Called only for a newly built wet group, before any source tap connects.
 * Publication follows this exact DLL's System_Update API-scope lifetime:
 * ab5a0 -> e4b50 (API scope), ff3a0 -> fecb0 -> c9850, fe900 (scope end).
 * That caller does not take LockDSP around c9850. Retain validation, scope
 * lifetime and synchronous initialization BEFORE connecting source taps.
 * This is not lock-free access, a skipped update, or a new deferred queue.
 */
static JourneyFmodResult publish_new_wet_group(void *system, void *group)
{
    void *internal=NULL, *scope=NULL;
    int result=g_spatial_validate(group,&internal,&scope);
    if(!result) {
        (void)system;
        result=g_spatial_update(internal,0U,0);
    }
    if(scope)g_spatial_scope_end(scope);
    return result;
}
/* One publication fence for the complete new wet chain. Avoid repeated
 * DSP-lock fences during attachment; retain the required single initial
 * fence, not one per ancestor. This is not claimed to remove FMOD's first
 * 64-sample original dry-group initialization transient. Same
 * identity-gated native scope/update functions, no additional private RVA. */
static JourneyFmodResult publish_new_wet_groups(void *system, void **groups, unsigned count)
{
    void *internals[9]={0}, *scopes[9]={0};
    unsigned i;
    int result=0;
    if(!groups || !count || count>9U)return 31; /* FMOD_ERR_INVALID_PARAM */
    for(i=0U;i<count;++i) {
        result=g_spatial_validate(groups[i],&internals[i],&scopes[i]);
        if(result)break;
    }
    if(!result) {
        (void)system;
        {
            for(i=0U;i<count;++i) {
                result=g_spatial_update(internals[i],0U,0);
                if(result)break;
            }
        }
    }
    for(i=count;i>0U;--i)if(scopes[i-1U])g_spatial_scope_end(scopes[i-1U]);
    return result;
}
static int spatial_publication_initialize(JourneyModule fmod)
{
    JourneyProc p;
    if(!spatial_fmod_identity(fmod)) return 0;
    p=resolve(fmod,"FMOD_System_LockDSP"); if(!p)return 0; memcpy(&g_spatial_lock,&p,sizeof(p));
    p=resolve(fmod,"FMOD_System_UnlockDSP"); if(!p)return 0; memcpy(&g_spatial_unlock,&p,sizeof(p));
    p=(uint8_t *)fmod+0x11850; memcpy(&g_spatial_validate,&p,sizeof(p));
    p=(uint8_t *)fmod+0xc9850; memcpy(&g_spatial_update,&p,sizeof(p));
    p=(uint8_t *)fmod+0xfe900; memcpy(&g_spatial_scope_end,&p,sizeof(p));
    return 1;
}
