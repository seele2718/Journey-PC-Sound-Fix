/* Read the initialized PC audio-wrapper layout before the original startup
 * ReleaseSemaphore call at VA 0x1402BE21B. Manager is written at wrapper+0x10
 * by 0x1402BE11E, before either native group loop. Primary groups are +0x18[6],
 * category groups +0x48[33]. The caller supplies the category-type table at
 * 0x140686D40 and the actual FMOD Master. This helper writes only our ownership
 * state; it does not alter native objects or add another lifecycle hook.
 */
#ifndef JOURNEY_NATIVE_OWNER_STARTUP_H
#define JOURNEY_NATIVE_OWNER_STARTUP_H
#include "native_owner_domain.h"
#include "native_group_startup.h"
static inline int journey_domain_startup(JourneyOwnerDomain *d,void *wrapper,
        void *expected_system,void *master,const uint32_t category_types[33]) {
    void *system=NULL,*manager=NULL;
    uintptr_t primary[6],category[33];
    if(!wrapper || !expected_system || !master || !category_types)return 0;
    memcpy(&system,wrapper,8);
    memcpy(&manager,(unsigned char *)wrapper+0x10,8);
    if(system!=expected_system || !manager)return 0;
    void *old=__atomic_load_n(&d->manager,__ATOMIC_ACQUIRE);
    if(old && old!=manager)return 0;
    memcpy(primary,(unsigned char *)wrapper+0x18,sizeof(primary));
    memcpy(category,(unsigned char *)wrapper+0x48,sizeof(category));
    if(!journey_group_seed_system(&d->groups,(uintptr_t)system,(uintptr_t)master,
                                  primary,category,category_types))return 0;
    return journey_domain_initialize(d,manager);
}
#endif
