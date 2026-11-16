#ifndef MEGUVIZ_MEDIA_FENCE_PROBE_STATUS_HPP
#define MEGUVIZ_MEDIA_FENCE_PROBE_STATUS_HPP

/* The caller includes media_android.h. A failed query or probe is not the
 * same as a capability that was actually reported unsupported. */
inline MgcStatus mgm_fence_probe_status(MgcStatus query,const MgcCapability &capability,bool egl_native_fence){
    if(query!=MGC_SUCCESS){
        if(!egl_native_fence&&(query==MGC_BUSY||query==MGC_TIMEOUT))return MGC_UNSUPPORTED_CAPABILITY;
        return query<0?query:MGC_INTERNAL_ERROR;
    }
    if(capability.probe_result!=MGC_SUCCESS&&capability.probe_result!=MGC_UNSUPPORTED_CAPABILITY&&
       capability.probe_result!=MGC_BUSY&&capability.probe_result!=MGC_TIMEOUT)
        return capability.probe_result<0?capability.probe_result:MGC_INTERNAL_ERROR;
    if(!egl_native_fence||!capability.enabled)return MGC_UNSUPPORTED_CAPABILITY;
    return capability.probe_result;
}
inline int mgm_fence_status_rank(MgcStatus value){
    if(value==MGC_SUCCESS)return 0;
    if(value==MGC_BUSY)return 1;
    if(value==MGC_TIMEOUT)return 2;
    if(value==MGC_UNSUPPORTED_CAPABILITY)return 3;
    if(value==MGC_DEVICE_LOST)return 5;
    return 4;
}
inline MgcStatus mgm_fence_worse(MgcStatus a,MgcStatus b){
    const int x=mgm_fence_status_rank(a),y=mgm_fence_status_rank(b);
    return x==y?(a<b?a:b):(x>y?a:b);
}
inline MgcStatus mgm_fence_overall(MgcStatus base,uint32_t path,MgcStatus imported,MgcStatus exported){
    // An observed device loss is never a successful fallback, even when the
    // optional native-fence path is not selected.
    if(base==MGC_DEVICE_LOST||imported==MGC_DEVICE_LOST||exported==MGC_DEVICE_LOST)return MGC_DEVICE_LOST;
    if(base!=MGC_SUCCESS)return base;
    if(path!=MGM_ANDROID_AHB_GPU_SYNC)return MGC_SUCCESS;
    return mgm_fence_worse(imported,exported);
}
#endif
