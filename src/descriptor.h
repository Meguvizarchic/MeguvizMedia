#ifndef MEGUVIZ_MEDIA_DESCRIPTOR_H
#define MEGUVIZ_MEDIA_DESCRIPTOR_H
#include <meguviz/media.h>
#include <cstring>

namespace mgm_detail {
inline MgcStatus read_header(const void *source,size_t minimum,MgcHeader &header){
    if(!source)return MGC_INVALID_ARGUMENT;
    std::memcpy(&header,source,sizeof(header));
    if(header.struct_size<minimum)return MGC_INVALID_ARGUMENT;
    return header.abi_major==MGC_ABI_MAJOR?MGC_SUCCESS:MGC_UNSUPPORTED_ABI;
}
template<class T>MgcStatus copy_desc(const T *source,size_t minimum,T &copy,uint32_t *size=nullptr){
    MgcHeader header{};auto status=read_header(source,minimum,header);if(status)return status;
    copy={};std::memcpy(&copy,source,header.struct_size<sizeof(T)?header.struct_size:sizeof(T));
    if(size)*size=header.struct_size;
    copy.header.struct_size=sizeof(T);return MGC_SUCCESS;
}
/* Optional fields must be absent or complete; a truncated integer or
 * callback pointer must never be interpreted as a complete request value. */
inline bool effect_request_prefix_complete(uint32_t size){
    auto partial=[size](size_t offset,size_t width){return size>offset&&size<offset+width;};
    for(size_t i=0;i<3;++i)
        if(partial(offsetof(MgmEffectRequest,reserved)+i*sizeof(uint32_t),sizeof(uint32_t)))return false;
    return !partial(offsetof(MgmEffectRequest,algorithm_scale),sizeof(uint32_t))&&
        !partial(offsetof(MgmEffectRequest,output_sync_mode),sizeof(uint32_t))&&
        !partial(offsetof(MgmEffectRequest,batch_event),sizeof(MgcBatchEventFn));
}

/* A zero color record is the old unspecified-metadata form. Once any color
 * metadata is supplied, its own versioned prefix must be valid before fields
 * are interpreted; the enclosing Frame prefix does not version it for us. */
inline MgcStatus copy_color(const MgmColor &source,MgmColor &copy){
    if(source.header.abi_major!=MGC_ABI_MAJOR)return MGC_UNSUPPORTED_ABI;
    if(!source.header.struct_size&&!source.header.abi_minor&&!source.primaries&&!source.transfer&&
       !source.range&&!source.matrix&&!source.chroma_siting&&!source.bit_depth&&!source.premultiplied&&!source.reserved){
        copy={};return MGC_SUCCESS;
    }
    auto status=copy_desc(&source,offsetof(MgmColor,reserved),copy);if(status)return status;
    return copy.reserved?MGC_INVALID_ARGUMENT:MGC_SUCCESS;
}
template<class T>void write_desc(T *destination,T copy,uint32_t size){
    copy.header.struct_size=size;
    std::memcpy(destination,&copy,size<sizeof(T)?size:sizeof(T));
}
template<class T,size_t N>bool zero(const T (&values)[N]){for(auto value:values)if(value)return false;return true;}
}
#endif
