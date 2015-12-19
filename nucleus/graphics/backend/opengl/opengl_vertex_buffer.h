/**
 * (c) 2015 Alexandro Sanchez Bach. All rights reserved.
 * Released under GPL v2 license. Read LICENSE for more details.
 */

#pragma once

#include "nucleus/graphics/vertex_buffer.h"
#include "nucleus/graphics/backend/opengl/opengl.h"

namespace gfx {
namespace opengl {

class OpenGLVertexBuffer : public VertexBuffer {
public:
    GLuint id;

    OpenGLVertexBuffer(GLsizeiptr size, GLenum usage);
    ~OpenGLVertexBuffer();

    virtual void* map() override;
    virtual bool unmap() override;
};

}  // namespace opengl
}  // namespace gfx
