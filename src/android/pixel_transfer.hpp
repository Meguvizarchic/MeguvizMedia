#ifndef MEGUVIZ_MEDIA_PIXEL_TRANSFER_HPP
#define MEGUVIZ_MEDIA_PIXEL_TRANSFER_HPP
#include <cstdint>
#include <limits>

/* Experimental, not linked by the bridge: error recovery and outer GL state
 * ownership are not closed. See coordination/verification/media-pixel-transfer-20261101. */
struct PixelTransferGuard {
    GLint64 pack_buffer=0,unpack_buffer=0;
    GLint alignment=0,row_length=0,skip_rows=0,skip_pixels=0;
    bool changed=false;
    PixelTransferGuard()=default;
    PixelTransferGuard(const PixelTransferGuard&)=delete;
    PixelTransferGuard& operator=(const PixelTransferGuard&)=delete;

    bool prepare() {
        glGetInteger64v(GL_PIXEL_PACK_BUFFER_BINDING,&pack_buffer);
        glGetInteger64v(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack_buffer);
        glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH,&row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);
        // This consumes an existing GL error if present. Do not mutate state
        // or issue client-memory reads when any snapshot is untrusted.
        if(glGetError()!=GL_NO_ERROR||pack_buffer<0||unpack_buffer<0||
           uint64_t(pack_buffer)>std::numeric_limits<GLuint>::max()||
           uint64_t(unpack_buffer)>std::numeric_limits<GLuint>::max())return false;
        changed=true;
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        glPixelStorei(GL_PACK_ROW_LENGTH,0);
        glPixelStorei(GL_PACK_SKIP_ROWS,0);
        glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        return glGetError()==GL_NO_ERROR;
    }

    bool restore() {
        if(!changed)return true;
        changed=false;
        glPixelStorei(GL_PACK_ALIGNMENT,alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH,row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS,skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS,skip_pixels);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,static_cast<GLuint>(pack_buffer));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,static_cast<GLuint>(unpack_buffer));
        return glGetError()==GL_NO_ERROR;
    }
    ~PixelTransferGuard(){if(changed)restore();}
};
#endif
