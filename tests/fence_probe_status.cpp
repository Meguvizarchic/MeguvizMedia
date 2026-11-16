#include <meguviz/media_android.h>
#include "../src/android/fence_probe_status.hpp"
#include <cstdio>

static bool check(bool value,const char *name){if(!value)std::fprintf(stderr,"FAIL %s\n",name);return value;}
int main(){
    MgcCapability cap{};cap.enabled=1;cap.probe_result=MGC_SUCCESS;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_SUCCESS,"available"))return 1;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,false)==MGC_UNSUPPORTED_CAPABILITY,"missing EGL extension"))return 1;
    cap.enabled=0;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_UNSUPPORTED_CAPABILITY,"not enabled"))return 1;
    cap.probe_result=MGC_BUSY;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_UNSUPPORTED_CAPABILITY,"disabled overrides transient"))return 1;
    cap.enabled=1;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,false)==MGC_UNSUPPORTED_CAPABILITY,"missing EGL overrides transient"))return 1;
    if(!check(mgm_fence_probe_status(MGC_BUSY,cap,false)==MGC_UNSUPPORTED_CAPABILITY,"query busy without EGL"))return 1;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_BUSY,"enabled transient remains busy"))return 1;
    cap.enabled=1;cap.probe_result=MGC_INTERNAL_ERROR;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,false)==MGC_INTERNAL_ERROR,"hard error before missing EGL"))return 1;
    cap.probe_result=MGC_DEVICE_LOST;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_DEVICE_LOST,"device lost"))return 1;
    if(!check(mgm_fence_probe_status(MGC_OUT_OF_MEMORY,cap,true)==MGC_OUT_OF_MEMORY,"query failure"))return 1;
    cap.probe_result=MGC_ACCEPTED;
    if(!check(mgm_fence_probe_status(MGC_SUCCESS,cap,true)==MGC_INTERNAL_ERROR,"invalid positive probe"))return 1;
    const MgcStatus statuses[]={MGC_SUCCESS,MGC_BUSY,MGC_TIMEOUT,MGC_UNSUPPORTED_CAPABILITY,MGC_INTERNAL_ERROR,MGC_OUT_OF_MEMORY,MGC_DEVICE_LOST};
    for(auto a:statuses)for(auto b:statuses){
        if(!check(mgm_fence_worse(a,b)==mgm_fence_worse(b,a),"order independent"))return 1;
        auto gpu=mgm_fence_overall(MGC_SUCCESS,MGM_ANDROID_AHB_GPU_SYNC,a,b);
        if(!check(gpu==mgm_fence_worse(a,b),"gpu sync status"))return 1;
        auto host=mgm_fence_overall(MGC_SUCCESS,MGM_ANDROID_AHB_HOST_WAIT,a,b);
        if(!check(host==((a==MGC_DEVICE_LOST||b==MGC_DEVICE_LOST)?MGC_DEVICE_LOST:MGC_SUCCESS),"host wait optional"))return 1;
    }
    if(!check(mgm_fence_overall(MGC_OUT_OF_MEMORY,MGM_ANDROID_AHB_GPU_SYNC,MGC_BUSY,MGC_UNSUPPORTED_CAPABILITY)==MGC_OUT_OF_MEMORY,"base error"))return 1;
    if(!check(mgm_fence_overall(MGC_OUT_OF_MEMORY,MGM_ANDROID_AHB_HOST_WAIT,MGC_DEVICE_LOST,MGC_SUCCESS)==MGC_DEVICE_LOST,"lost overrides base"))return 1;
    std::puts("PASS native fence probe status and path precedence");return 0;
}
