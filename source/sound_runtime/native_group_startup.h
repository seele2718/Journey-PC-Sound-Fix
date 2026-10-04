/* Seed existing native group identities once, before startup releases the
 * sound-worker semaphore at VA 0x1402BE21B. The following instruction at
 * 0x1402BE221 is not the hook. By this point the six primary groups at
 * audio-wrapper+0x18 and 33 category slots at +0x48 are initialized.
 * Category type 5 is deliberately absent (type table at VA 0x140686D40).
 * The caller supplies these arrays and FMOD System's actual Master group.
 * This helper only updates our registry: no FMOD calls, native memory writes,
 * or registry reset. Startup/control owner only, before source publication.
 * Audited original PC EXE SHA-256: 16a7c176...a788f (provenance, not a gate).
 */
#ifndef JOURNEY_NATIVE_GROUP_STARTUP_H
#define JOURNEY_NATIVE_GROUP_STARTUP_H
#include "native_group_registry.h"
static int journey_group_seed_system(JourneyGroupRegistry *r,uintptr_t system,
        uintptr_t master,const uintptr_t primary[6],const uintptr_t category[33],
        const uint32_t category_types[33]) {
    uintptr_t groups[40];unsigned count=0,new_count=0;
    if(!system || !master || !primary || !category || !category_types)return 0;
    groups[count++]=master;
    for(unsigned i=0;i<6;i++)groups[count++]=primary[i];
    for(unsigned i=0;i<33;i++)if(category_types[i]!=5)groups[count++]=category[i];
    /* Preflight keeps failed or conflicting startup from partially replacing
     * identities. Repeated observation of this same System is idempotent. */
    for(unsigned i=0;i<count;i++) {
        if(!groups[i])return 0;
        unsigned duplicate=0;
        for(unsigned j=0;j<i;j++)if(groups[i]==groups[j]){duplicate=1;break;}
        if(duplicate)continue;
        JourneyGroupIdentity *e=journey_group_find(r,groups[i],0);
        if(e) {
            if(e->owner!=system || e->kind!=JOURNEY_GROUP_SYSTEM)return 0;
        } else new_count++;
    }
    if(new_count>JOURNEY_GROUP_CAPACITY-r->live || new_count>UINT32_MAX-r->serial)return 0;
    for(unsigned i=0;i<count;i++)if(!journey_group_generation(r,groups[i]))
        if(!journey_group_activate(r,groups[i],system,JOURNEY_GROUP_SYSTEM,0))return 0;
    return 1;
}
#endif
