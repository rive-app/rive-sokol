#include <stdio.h>

#include <imgui.h>

#define SOKOL_IMPL
#define SOKOL_GLCORE33
#include <sokol_gfx.h>
#include <sokol_time.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <jc/array.h>
#include <jc/hashtable.h>
#include <linmath.h>

#include <animation/linear_animation_instance.hpp>
#include <artboard.hpp>

#include "rive/shared.h"
#include "rive/shader.glsl.h"

#define VIEWER_WINDOW_NAME "Rive Tessellation Viewer"

typedef ImVec2 vs_imgui_params_t;

static struct App
{
    static const int MAX_ARTBOARD_CONTEXTS = 8;
    static const int MAX_IMGUI_VERTICES    = (1<<16);
    static const int MAX_IMGUI_INDICES     = MAX_IMGUI_VERTICES * 3;

    struct ArtboardData
    {
        rive::Artboard*                m_Artboard;
        rive::LinearAnimationInstance* m_AnimationInstance;
    };

    struct ArtboardContext
    {
        jc::Array<ArtboardData> m_Artboards;
        uint8_t*                m_Data;
        uint32_t                m_DataSize;
        int32_t                 m_CloneCount;
    };

    struct GpuBuffer
    {
        sg_buffer    m_Handle;
        unsigned int m_DataSize;
    };

    struct Camera
    {
        static const int ZOOM_MULTIPLIER = 64;
        float m_X;
        float m_Y;
        float m_Zoom;

        void Reset()
        {
            m_X    = 0.0f;
            m_Y    = 0.0f;
            m_Zoom = (float) ZOOM_MULTIPLIER * 2.0f;
        }

        float Zoom()
        {
            return m_Zoom / (float)ZOOM_MULTIPLIER;
        }
    };

    // GLFW
    GLFWwindow*                m_Window;
    // Rive
    ArtboardContext            m_ArtboardContexts[MAX_ARTBOARD_CONTEXTS];
    rive::Renderer*            m_Renderer;
    // Sokol
    sg_shader                  m_MainShader;
    sg_pipeline                m_TessellationPipeline;
    sg_pipeline                m_StencilPipelineNonClippingCCW;
    sg_pipeline                m_StencilPipelineNonClippingCW;
    sg_pipeline                m_StencilPipelineClippingCCW;
    sg_pipeline                m_StencilPipelineClippingCW;
    sg_pipeline                m_StencilPipelineCoverNonClipping;
    sg_pipeline                m_StencilPipelineCoverClipping;
    sg_pass_action             m_PassAction;
    sg_bindings                m_Bindings;
    // Imgui
    sg_buffer                  m_ImguiVxBuffer;
    sg_buffer                  m_ImguiIxBuffer;
    sg_image                   m_ImguiFontImage;
    sg_shader                  m_ImguiShader;
    sg_pipeline                m_ImguiPipeline;
    // App state
    Camera                     m_Camera;
} g_app;

static bool LoadFileFromPath(const char* path, uint8_t** bytesOut, size_t* bytesLengthOut)
{
    FILE* fp = fopen(path, "rb");
    if (fp == 0)
    {
        fprintf(stderr, "Failed to open file from '%s'\n", path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileBytesLength = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t* fileBytes = new uint8_t[fileBytesLength];
    size_t bytesRead = fread(fileBytes, 1, fileBytesLength, fp);

    if (bytesRead != fileBytesLength)
    {
        delete[] fileBytes;
        fprintf(stderr, "Failed to read file from '%s' bytes read %zu, expected %zu\n", path, bytesRead, fileBytesLength);
        return false;
    }

    *bytesOut       = fileBytes;
    *bytesLengthOut = fileBytesLength;

    return true;
}

static void UpdateArtboardCloneCount(App::ArtboardContext& ctx)
{
    if (ctx.m_CloneCount != ctx.m_Artboards.Size())
    {
        if (ctx.m_CloneCount > ctx.m_Artboards.Size())
        {
            rive::Artboard* artboard = rive::loadArtboardFromData(ctx.m_Data, ctx.m_DataSize);
            App::ArtboardData data = {
                .m_Artboard          = artboard,
                .m_AnimationInstance = 0
            };

            if (artboard->animationCount() > 0)
            {
                data.m_AnimationInstance = new rive::LinearAnimationInstance(artboard->firstAnimation());
            }

            ctx.m_Artboards.SetCapacity(ctx.m_Artboards.Capacity() + 1);
            ctx.m_Artboards.Push(data);
        }
        else
        {
            for (int i = ctx.m_CloneCount; i < ctx.m_Artboards.Size(); ++i)
            {
                if (ctx.m_Artboards[i].m_Artboard)
                {
                    delete ctx.m_Artboards[i].m_Artboard;
                }

                if (ctx.m_Artboards[i].m_AnimationInstance)
                {
                    delete ctx.m_Artboards[i].m_AnimationInstance;
                }
            }

            ctx.m_Artboards.SetSize(ctx.m_CloneCount);
        }
    }
}

static void AddArtboardFromPath(const char* path)
{
    App::ArtboardContext* ctx = 0;

    for (int i = 0; i < App::MAX_ARTBOARD_CONTEXTS; ++i)
    {
        if (g_app.m_ArtboardContexts[i].m_Artboards.Size() == 0)
        {
            ctx = &g_app.m_ArtboardContexts[i];
            break;
        }
    }

    if (ctx == 0)
    {
        printf("Can't add more artboards");
        return;
    }

    uint8_t* bytes = 0;
    size_t bytesLength = 0;
    if (LoadFileFromPath(path, &bytes, &bytesLength))
    {
        rive::Artboard* artboard = rive::loadArtboardFromData(bytes, bytesLength);
        if (artboard)
        {
            assert(ctx->m_Data == 0);
            ctx->m_Data       = bytes;
            ctx->m_DataSize   = (uint32_t) bytesLength;
            ctx->m_CloneCount = 1;

            App::ArtboardData data = {
                .m_Artboard          = artboard,
                .m_AnimationInstance = 0
            };

            if (artboard->animationCount() > 0)
            {
                data.m_AnimationInstance = new rive::LinearAnimationInstance(artboard->firstAnimation());
            }

            ctx->m_Artboards.SetCapacity(1);
            ctx->m_Artboards.Push(data);

            printf("Added artboard from '%s'\n", path);
        }
    }
}

static void ReloadArtboardContext(App::ArtboardContext* ctx)
{
    for (int i = 0; i < (int)ctx->m_Artboards.Size(); ++i)
    {
        App::ArtboardData& data = ctx->m_Artboards[i];

        if (data.m_Artboard)
        {
            delete data.m_Artboard;
            data.m_Artboard = rive::loadArtboardFromData(ctx->m_Data, ctx->m_DataSize);
        }

        if (data.m_AnimationInstance)
        {
            delete data.m_AnimationInstance;
        }

        if (data.m_Artboard->animationCount() > 0)
        {
            data.m_AnimationInstance = new rive::LinearAnimationInstance(data.m_Artboard->firstAnimation());
        }
    }
}

static inline void Mat2DToMat4(const rive::Mat2D m2, mat4x4 m4)
{
    m4[0][0] = m2[0];
    m4[0][1] = m2[1];
    m4[0][2] = 0.0;
    m4[0][3] = 0.0;

    m4[1][0] = m2[2];
    m4[1][1] = m2[3];
    m4[1][2] = 0.0;
    m4[1][3] = 0.0;

    m4[2][0] = 0.0;
    m4[2][1] = 0.0;
    m4[2][2] = 1.0;
    m4[2][3] = 0.0;

    m4[3][0] = m2[4];
    m4[3][1] = m2[5];
    m4[3][2] = 0.0;
    m4[3][3] = 1.0;
}

static rive::HBuffer AppRequestBufferCallback(rive::HBuffer buffer, rive::BufferType type, void* data, unsigned int dataSize)
{
    if (dataSize == 0)
    {
        return buffer;
    }

    App::GpuBuffer* buf = (App::GpuBuffer*) buffer;

    if (buf == 0)
    {
        buf = new App::GpuBuffer();
    }

    if (buf->m_Handle.id == SG_INVALID_ID)
    {
        sg_buffer_desc sg_buf = {
            .size  = dataSize,
            .type  = type == rive::BUFFER_TYPE_VERTEX_BUFFER ? SG_BUFFERTYPE_VERTEXBUFFER : SG_BUFFERTYPE_INDEXBUFFER,
            .usage = SG_USAGE_DYNAMIC,
        };

        buf->m_Handle = sg_make_buffer(&sg_buf);
        buf->m_DataSize = dataSize;
    }
    else if (buf->m_DataSize != dataSize)
    {
        sg_destroy_buffer(buf->m_Handle);
        sg_buffer_desc sg_buf = {
            .size  = dataSize,
            .type  = type == rive::BUFFER_TYPE_VERTEX_BUFFER ? SG_BUFFERTYPE_VERTEXBUFFER : SG_BUFFERTYPE_INDEXBUFFER,
            .usage = SG_USAGE_DYNAMIC,
        };
        buf->m_Handle = sg_make_buffer(&sg_buf);
        buf->m_DataSize = dataSize;
    }

    sg_update_buffer(buf->m_Handle, { .ptr = data, .size = dataSize });

    return (rive::HBuffer) buf;
}

static void AppDestroyBufferCallback(rive::HBuffer buffer)
{
    App::GpuBuffer* buf = (App::GpuBuffer*) buffer;
    if (buf != 0)
    {
        sg_destroy_buffer(buf->m_Handle);
        delete buf;
        buffer = 0;
    }
}

static void AppCursorCallback(GLFWwindow* w, double x, double y)
{
    ImGui::GetIO().MousePos.x = float(x);
    ImGui::GetIO().MousePos.y = float(y);
}

static void AppMouseButtonCallback(GLFWwindow* w, int btn, int action, int mods)
{
    if ((btn >= 0) && (btn < 3))
    {
        ImGui::GetIO().MouseDown[btn] = (action == GLFW_PRESS);
    }
}

static void AppMouseWheelCallback(GLFWwindow* w, double x, double y)
{
    ImGui::GetIO().MouseWheel = float(y);
}

bool AppBootstrap(int argc, char const *argv[])
{
    ////////////////////////////////////////////////////
    // GLFW setup
    ////////////////////////////////////////////////////
    if (!glfwInit())
    {
        fprintf(stderr, "Failed to initialize glfw.\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, VIEWER_WINDOW_NAME, 0, 0);

    if (!window)
    {
        fprintf(stderr, "Failed to initialize glfw.\n");
        return false;
    }

    glfwSetCursorPosCallback(window, AppCursorCallback);
    glfwSetMouseButtonCallback(window, AppMouseButtonCallback);
    glfwSetScrollCallback(window, AppMouseWheelCallback);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    memset((void*)&g_app, 0, sizeof(g_app));
    g_app.m_Window = window;
    g_app.m_Camera.Reset();

    ////////////////////////////////////////////////////
    // Sokol setup
    ////////////////////////////////////////////////////
    stm_setup();
    sg_desc sg_setup_desc = {
        .buffer_pool_size = 4096
    };
    sg_setup(&sg_setup_desc);

    // Main tessellation pipeline
    sg_pipeline_desc tessellationPipeline               = {};
    tessellationPipeline.shader                         = sg_make_shader(rive_shader_shader_desc(sg_query_backend()));
    tessellationPipeline.index_type                     = SG_INDEXTYPE_UINT32;
    tessellationPipeline.layout.attrs[0]                = { .format = SG_VERTEXFORMAT_FLOAT2 };
    tessellationPipeline.colors[0].blend.enabled        = true;
    tessellationPipeline.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    tessellationPipeline.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    // Stencil to cover pipelines
    sg_pipeline_desc pipelineStencilDesc               = {};
    pipelineStencilDesc.shader                         = tessellationPipeline.shader;
    pipelineStencilDesc.index_type                     = SG_INDEXTYPE_UINT32;
    pipelineStencilDesc.layout.attrs[0]                = { .format = SG_VERTEXFORMAT_FLOAT2 };
    pipelineStencilDesc.colors[0].blend.enabled        = true;
    pipelineStencilDesc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pipelineStencilDesc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    pipelineStencilDesc.stencil = {
        .enabled = true,
        .front = {
            .compare       = SG_COMPAREFUNC_ALWAYS,
            .fail_op       = SG_STENCILOP_KEEP,
            .depth_fail_op = SG_STENCILOP_KEEP,
            .pass_op       = SG_STENCILOP_INCR_WRAP,
        },
        .back = {
            .compare       = SG_COMPAREFUNC_ALWAYS,
            .fail_op       = SG_STENCILOP_KEEP,
            .depth_fail_op = SG_STENCILOP_KEEP,
            .pass_op       = SG_STENCILOP_DECR_WRAP,
        },
        .read_mask  = 0xFF,
        .write_mask = 0xFF,
        .ref        = 0x0,
    };
    pipelineStencilDesc.face_winding         = SG_FACEWINDING_CCW;
    pipelineStencilDesc.colors[0].write_mask = SG_COLORMASK_NONE;

    sg_pipeline stencilPipelineNonClippingCCW = sg_make_pipeline(&pipelineStencilDesc);

    pipelineStencilDesc.face_winding = SG_FACEWINDING_CW;

    sg_pipeline stencilPipelineNonClippingCW = sg_make_pipeline(&pipelineStencilDesc);

    pipelineStencilDesc.stencil.front.compare = SG_COMPAREFUNC_EQUAL;
    pipelineStencilDesc.stencil.back.compare  = SG_COMPAREFUNC_EQUAL;
    pipelineStencilDesc.stencil.write_mask    = 0x7F;
    pipelineStencilDesc.stencil.read_mask     = 0x80;
    pipelineStencilDesc.stencil.ref           = 0x80;
    pipelineStencilDesc.face_winding          = SG_FACEWINDING_CCW;

    sg_pipeline stencilPipelineClippingCCW = sg_make_pipeline(&pipelineStencilDesc);

    pipelineStencilDesc.face_winding = SG_FACEWINDING_CW;

    sg_pipeline stencilPipelineClippingCW = sg_make_pipeline(&pipelineStencilDesc);

    pipelineStencilDesc.stencil.front.compare       = SG_COMPAREFUNC_NOT_EQUAL;
    pipelineStencilDesc.stencil.front.fail_op       = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.front.depth_fail_op = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.front.pass_op       = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.back.compare        = SG_COMPAREFUNC_NOT_EQUAL;
    pipelineStencilDesc.stencil.back.fail_op        = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.back.depth_fail_op  = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.back.pass_op        = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.ref                 = 0x0;
    pipelineStencilDesc.stencil.write_mask          = 0xFF;
    pipelineStencilDesc.stencil.read_mask           = 0xFF;
    pipelineStencilDesc.colors[0].write_mask        = SG_COLORMASK_RGBA;

    sg_pipeline coverPipelineNonClipping = sg_make_pipeline(&pipelineStencilDesc);
    pipelineStencilDesc.stencil.read_mask           = 0x7F;
    pipelineStencilDesc.stencil.write_mask          = 0x7F;
    sg_pipeline coverPipelineClipping = sg_make_pipeline(&pipelineStencilDesc);

    g_app.m_MainShader           = tessellationPipeline.shader;
    g_app.m_TessellationPipeline = sg_make_pipeline(&tessellationPipeline);
    g_app.m_PassAction           = {0};
    g_app.m_Bindings             = {};

    g_app.m_StencilPipelineNonClippingCCW   = stencilPipelineNonClippingCCW;
    g_app.m_StencilPipelineNonClippingCW    = stencilPipelineNonClippingCW;
    g_app.m_StencilPipelineClippingCCW      = stencilPipelineClippingCCW;
    g_app.m_StencilPipelineClippingCW       = stencilPipelineClippingCW;
    g_app.m_StencilPipelineCoverNonClipping = coverPipelineNonClipping;
    g_app.m_StencilPipelineCoverClipping    = coverPipelineClipping;

    ////////////////////////////////////////////////////
    // Rive setup
    ////////////////////////////////////////////////////
    rive::setRenderMode(rive::MODE_STENCIL_TO_COVER);
    rive::setBufferCallbacks(AppRequestBufferCallback, AppDestroyBufferCallback);
    g_app.m_Renderer = rive::makeRenderer();

    for (int i = 1; i < argc; ++i)
    {
        AddArtboardFromPath(argv[i]);
    }

    ////////////////////////////////////////////////////
    // Imgui setup
    ////////////////////////////////////////////////////
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();

    // dynamic vertex- and index-buffers for imgui-generated geometry
    sg_buffer_desc imguiVxBufferDesc = {};
    imguiVxBufferDesc.usage          = SG_USAGE_STREAM;
    imguiVxBufferDesc.size           = App::MAX_IMGUI_VERTICES * sizeof(ImDrawVert);
    sg_buffer_desc imguiIxBufferDesc = { };
    imguiIxBufferDesc.type           = SG_BUFFERTYPE_INDEXBUFFER;
    imguiIxBufferDesc.usage          = SG_USAGE_STREAM;
    imguiIxBufferDesc.size           = App::MAX_IMGUI_INDICES * sizeof(ImDrawIdx);

    unsigned char* fontPixels;
    int fontWidth, fontHeight;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    sg_image_desc imguiFontImageDesc       = {};
    imguiFontImageDesc.width               = fontWidth;
    imguiFontImageDesc.height              = fontHeight;
    imguiFontImageDesc.pixel_format        = SG_PIXELFORMAT_RGBA8;
    imguiFontImageDesc.wrap_u              = SG_WRAP_CLAMP_TO_EDGE;
    imguiFontImageDesc.wrap_v              = SG_WRAP_CLAMP_TO_EDGE;
    imguiFontImageDesc.data.subimage[0][0] = sg_range{fontPixels, size_t(fontWidth * fontHeight * 4)};

    sg_shader_desc imguiShaderDesc   = { };
    sg_shader_uniform_block_desc& ub = imguiShaderDesc.vs.uniform_blocks[0];
    ub.size                          = sizeof(vs_imgui_params_t);
    ub.uniforms[0].name              = "disp_size";
    ub.uniforms[0].type              = SG_UNIFORMTYPE_FLOAT2;
    imguiShaderDesc.vs.source =
        "#version 330\n"
        "uniform vec2 disp_size;\n"
        "layout(location=0) in vec2 position;\n"
        "layout(location=1) in vec2 texcoord0;\n"
        "layout(location=2) in vec4 color0;\n"
        "out vec2 uv;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "    gl_Position = vec4(((position/disp_size)-0.5)*vec2(2.0,-2.0), 0.5, 1.0);\n"
        "    uv = texcoord0;\n"
        "    color = color0;\n"
        "}\n";
    imguiShaderDesc.fs.images[0].name       = "tex";
    imguiShaderDesc.fs.images[0].image_type = SG_IMAGETYPE_2D;
    imguiShaderDesc.fs.source =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "in vec2 uv;\n"
        "in vec4 color;\n"
        "out vec4 frag_color;\n"
        "void main() {\n"
        "    frag_color = texture(tex, uv) * color;\n"
        "}\n";

    g_app.m_ImguiVxBuffer  = sg_make_buffer(&imguiVxBufferDesc);
    g_app.m_ImguiIxBuffer  = sg_make_buffer(&imguiIxBufferDesc);
    g_app.m_ImguiFontImage = sg_make_image(&imguiFontImageDesc);
    g_app.m_ImguiShader    = sg_make_shader(&imguiShaderDesc);

    sg_pipeline_desc imguiPipelineDesc               = {};
    imguiPipelineDesc.layout.buffers[0].stride       = sizeof(ImDrawVert);
    imguiPipelineDesc.layout.attrs[0].format         = SG_VERTEXFORMAT_FLOAT2;
    imguiPipelineDesc.layout.attrs[1].format         = SG_VERTEXFORMAT_FLOAT2;
    imguiPipelineDesc.layout.attrs[2].format         = SG_VERTEXFORMAT_UBYTE4N;
    imguiPipelineDesc.shader                         = g_app.m_ImguiShader;
    imguiPipelineDesc.index_type                     = SG_INDEXTYPE_UINT16;
    imguiPipelineDesc.colors[0].blend.enabled        = true;
    imguiPipelineDesc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    imguiPipelineDesc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    imguiPipelineDesc.colors[0].write_mask           = SG_COLORMASK_RGB;
    g_app.m_ImguiPipeline                            = sg_make_pipeline(&imguiPipelineDesc);

    return true;
}

void AppUpdateRive(float dt, uint32_t width, uint32_t height)
{
    rive::SharedRenderer* renderer = (rive::SharedRenderer*) g_app.m_Renderer;

    renderer->startFrame();

    float y = 0.0f;
    float x = 0.0f;
    for (int i = 0; i < App::MAX_ARTBOARD_CONTEXTS; ++i)
    {
        const App::ArtboardContext& ctx = g_app.m_ArtboardContexts[i];
        const int numArtboards          = (int)ctx.m_Artboards.Size();

        for (int j = 0; j < numArtboards; ++j)
        {
            renderer->save();
            const App::ArtboardData& data            = ctx.m_Artboards[j];
            rive::Artboard* artboard                 = data.m_Artboard;
            rive::LinearAnimationInstance* animation = data.m_AnimationInstance;
            rive::AABB artboardBounds                = artboard->bounds();
            x = artboardBounds.width() * j;

            renderer->align(rive::Fit::none,
               rive::Alignment::topLeft,
               rive::AABB(x - width/2, y - height/2 , artboardBounds.width(), artboardBounds.height()),
               artboardBounds);

            if (animation)
            {
                animation->advance(dt);
                animation->apply(artboard, 1);
            }

            artboard->advance(dt);
            artboard->draw(renderer);
            renderer->restore();

            if (j == (numArtboards-1))
            {
                y += artboardBounds.height();
            }
        }
    }
}

static void FillPaintData(rive::RenderPaint* paint, fs_paint_t& uniform)
{
    const rive::SharedRenderPaintData paintData = ((rive::SharedRenderPaint*) paint)->getData();
    //  Note: Have to use vectors for the stops here aswell, doesn't work otherwise (sokol issue?)
    for (int i = 0; i < paintData.m_StopCount; ++i)
    {
        uniform.stops[i][0] = paintData.m_Stops[i];
    }
    memcpy(uniform.colors, paintData.m_Colors, sizeof(paintData.m_Colors));
    uniform.stopCount        = (float) paintData.m_StopCount;
    uniform.fillType         = (float) paintData.m_FillType;
    uniform.gradientStart[0] = paintData.m_GradientLimits[0];
    uniform.gradientStart[1] = paintData.m_GradientLimits[1];
    uniform.gradientStop[0]  = paintData.m_GradientLimits[2];
    uniform.gradientStop[1]  = paintData.m_GradientLimits[3];
}

// Adapted from https://github.com/floooh/sokol-samples/blob/master/glfw/imgui-glfw.cc
static void AppDrawImgui(ImDrawData* drawData)
{
    assert(drawData);
    if (drawData->CmdListsCount == 0)
    {
        return;
    }

    sg_bindings bind       = {};
    bind.vertex_buffers[0] = g_app.m_ImguiVxBuffer;
    bind.index_buffer      = g_app.m_ImguiIxBuffer;
    bind.fs_images[0]      = g_app.m_ImguiFontImage;

    // render the command list
    sg_apply_pipeline(g_app.m_ImguiPipeline);
    vs_imgui_params_t vs_params;
    vs_params.x = ImGui::GetIO().DisplaySize.x;
    vs_params.y = ImGui::GetIO().DisplaySize.y;
    sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, SG_RANGE(vs_params));
    for (int cl_index = 0; cl_index < drawData->CmdListsCount; cl_index++)
    {
        const ImDrawList* cl = drawData->CmdLists[cl_index];

        // append vertices and indices to buffers, record start offsets in resource binding struct
        const uint32_t vtx_size  = cl->VtxBuffer.size() * sizeof(ImDrawVert);
        const uint32_t idx_size  = cl->IdxBuffer.size() * sizeof(ImDrawIdx);
        const uint32_t vb_offset = sg_append_buffer(bind.vertex_buffers[0], { &cl->VtxBuffer.front(), vtx_size });
        const uint32_t ib_offset = sg_append_buffer(bind.index_buffer, { &cl->IdxBuffer.front(), idx_size });
        /* don't render anything if the buffer is in overflow state (this is also
            checked internally in sokol_gfx, draw calls that attempt from
            overflowed buffers will be silently dropped)
        */
        if (sg_query_buffer_overflow(bind.vertex_buffers[0]) ||
            sg_query_buffer_overflow(bind.index_buffer))
        {
            continue;
        }

        bind.vertex_buffer_offsets[0] = vb_offset;
        bind.index_buffer_offset = ib_offset;
        sg_apply_bindings(&bind);

        int base_element = 0;
        for (const ImDrawCmd& pcmd : cl->CmdBuffer) {
            if (pcmd.UserCallback) {
                pcmd.UserCallback(cl, &pcmd);
            }
            else {
                const int scissor_x = (int) (pcmd.ClipRect.x);
                const int scissor_y = (int) (pcmd.ClipRect.y);
                const int scissor_w = (int) (pcmd.ClipRect.z - pcmd.ClipRect.x);
                const int scissor_h = (int) (pcmd.ClipRect.w - pcmd.ClipRect.y);
                sg_apply_scissor_rect(scissor_x, scissor_y, scissor_w, scissor_h, true);
                sg_draw(base_element, pcmd.ElemCount, 1);
            }
            base_element += pcmd.ElemCount;
        }
    }
}

#define IS_BUFFER_VALID(b) (b != 0 && b->m_Handle.id != SG_INVALID_ID)

static inline void GetCameraMatrix(mat4x4 M, uint32_t width, uint32_t height)
{
    mat4x4 view;
    mat4x4_identity(view);
    mat4x4_translate(view, g_app.m_Camera.m_X, g_app.m_Camera.m_Y, 0.0f);

    float zoom = g_app.m_Camera.Zoom();
    float hx = ((float) width) / 2.0f * zoom;
    float hy = ((float) height) / 2.0f * zoom;

    mat4x4 projection;
    mat4x4_ortho(projection, -hx, hx, hy, -hy, 0.0f, 1.0f);

    mat4x4_mul(M, projection, view);
}

static void StencilToCoverRenderFn(uint32_t width, uint32_t height)
{
    rive::StencilToCoverRenderer* renderer = (rive::StencilToCoverRenderer*) g_app.m_Renderer;
    sg_bindings& bindings                  = g_app.m_Bindings;
    vs_params_t vsUniforms                 = {};
    fs_paint_t fsUniforms                  = {};
    sg_range vsUniformsRange               = SG_RANGE(vsUniforms);
    sg_range fsUniformsRange               = SG_RANGE(fsUniforms);
    rive::RenderPaint* lastPaint           = 0;

    mat4x4 mtxCam;
    GetCameraMatrix(mtxCam, width, height);
    mat4x4_dup((float (*)[4]) vsUniforms.projection, mtxCam);

    mat4x4_identity((float (*)[4]) vsUniforms.transformLocal);
    sg_apply_viewport(0, 0, width, height, true);

    for (int i = 0; i < renderer->getDrawCallCount(); ++i)
    {
        const rive::PathDrawCall dc = renderer->getDrawCall(i);
        rive::RenderPath* path      = dc.m_Path;
        rive::RenderPaint* paint    = dc.m_Paint;
        rive::Mat2D transformWorld  = dc.m_TransformWorld;
        rive::Mat2D transformLocal  = dc.m_TransformLocal;

        rive::StencilToCoverRenderPath::Buffers buffers = ((rive::StencilToCoverRenderPath*) path)->getDrawBuffers();

        // fixme: clipping
        bool isClipping = false;
        if (dc.m_Tag == rive::TAG_STENCIL)
        {
            App::GpuBuffer* contourVertexBuffer = (App::GpuBuffer*) buffers.m_ContourVertexBuffer;
            App::GpuBuffer* contourIndexBuffer  = (App::GpuBuffer*) buffers.m_ContourIndexBuffer;

            if (!IS_BUFFER_VALID(contourVertexBuffer) ||
                !IS_BUFFER_VALID(contourIndexBuffer))
            {
                continue;
            }

            sg_pipeline pipeline = {};

            if (isClipping)
            {
                if (dc.m_IsEvenOdd && (dc.m_Idx % 2) != 0)
                {
                    pipeline = g_app.m_StencilPipelineClippingCW;
                }
                else
                {
                    pipeline = g_app.m_StencilPipelineClippingCCW;
                }
            }
            else
            {
                if (dc.m_IsEvenOdd && (dc.m_Idx % 2) != 0)
                {
                    pipeline = g_app.m_StencilPipelineNonClippingCW;
                }
                else
                {
                    pipeline = g_app.m_StencilPipelineNonClippingCCW;
                }
            }

            bindings.vertex_buffers[0] = contourVertexBuffer->m_Handle;
            bindings.index_buffer      = contourIndexBuffer->m_Handle;
            Mat2DToMat4(transformWorld, (float (*)[4]) vsUniforms.transform);
            sg_apply_pipeline(pipeline);
            sg_apply_bindings(&bindings);
            sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &vsUniformsRange);
            sg_draw(0, contourIndexBuffer->m_DataSize / sizeof(int), 1);

        }
        else if (dc.m_Tag == rive::TAG_COVER)
        {
            App::GpuBuffer* coverVertexBuffer = (App::GpuBuffer*) buffers.m_CoverVertexBuffer;
            App::GpuBuffer* coverIndexBuffer  = (App::GpuBuffer*) buffers.m_CoverIndexBuffer;

            if (!IS_BUFFER_VALID(coverVertexBuffer) ||
                !IS_BUFFER_VALID(coverIndexBuffer))
            {
                continue;
            }

            bindings.vertex_buffers[0] = coverVertexBuffer->m_Handle;
            bindings.index_buffer      = coverIndexBuffer->m_Handle;
            Mat2DToMat4(transformWorld, (float (*)[4]) vsUniforms.transform);
            Mat2DToMat4(transformLocal, (float (*)[4]) vsUniforms.transformLocal);

            sg_pipeline pipeline = isClipping ? g_app.m_StencilPipelineCoverClipping : g_app.m_StencilPipelineCoverNonClipping;
            sg_apply_pipeline(pipeline);
            sg_apply_bindings(&bindings);
            sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &vsUniformsRange);
            if (lastPaint != paint)
            {
                lastPaint = paint;
                FillPaintData(paint, fsUniforms);
                sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &fsUniformsRange);
            }
            sg_draw(0, 2 * 3, 1);
        }
    }
}

static void TessellationRenderFn(uint32_t width, uint32_t height)
{
    rive::TessellationRenderer* renderer = (rive::TessellationRenderer*) g_app.m_Renderer;
    sg_bindings& bindings                = g_app.m_Bindings;
    rive::RenderPaint* lastPaint         = 0;
    vs_params_t vsUniforms               = {};
    fs_paint_t fsUniforms                = {};
    sg_range vsUniformsRange             = SG_RANGE(vsUniforms);
    sg_range fsUniformsRange             = SG_RANGE(fsUniforms);

    mat4x4 mtxCam;
    GetCameraMatrix(mtxCam, width, height);
    mat4x4_dup((float (*)[4]) vsUniforms.projection, mtxCam);
    mat4x4_identity((float (*)[4]) vsUniforms.transformLocal);

    sg_apply_pipeline(g_app.m_TessellationPipeline);
    sg_apply_viewport(0, 0, width, height, true);

    for (int i = 0; i < renderer->getDrawCallCount(); ++i)
    {
        const rive::PathDrawCall dc = renderer->getDrawCall(i);
        rive::RenderPath* path      = dc.m_Path;
        rive::RenderPaint* paint    = dc.m_Paint;
        rive::Mat2D transform       = dc.m_TransformWorld;
        rive::TessellationRenderPath::Buffers buffers = ((rive::TessellationRenderPath*) path)->getDrawBuffers();

        App::GpuBuffer* vertexBuffer = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* indexBuffer  = (App::GpuBuffer*) buffers.m_IndexBuffer;

        if (!IS_BUFFER_VALID(vertexBuffer) || !IS_BUFFER_VALID(indexBuffer))
        {
            continue;
        }

        bindings.vertex_buffers[0] = vertexBuffer->m_Handle;
        bindings.index_buffer      = indexBuffer->m_Handle;
        Mat2DToMat4(transform, (float (*)[4]) vsUniforms.transform);

        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &vsUniformsRange);

        if (lastPaint != paint)
        {
            lastPaint = paint;
            FillPaintData(paint, fsUniforms);
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &fsUniformsRange);
        }

        sg_draw(0, (indexBuffer->m_DataSize / sizeof(int)) * 3, 1);
    }
}
#undef IS_BUFFER_VALID

void AppRenderRive(uint32_t width, uint32_t height)
{
    switch(rive::getRenderMode())
    {
        case rive::MODE_TESSELLATION:
            TessellationRenderFn(width, height);
            break;
        case rive::MODE_STENCIL_TO_COVER:
            StencilToCoverRenderFn(width, height);
            break;
        default:break;
    }
}

void AppConfigure(rive::RenderMode renderMode, float contourQuality)
{
    if (rive::getRenderMode() != renderMode)
    {
        rive::setRenderMode(renderMode);
        for (int i = 0; i < App::MAX_ARTBOARD_CONTEXTS; ++i)
        {
            ReloadArtboardContext(&g_app.m_ArtboardContexts[i]);
        }
        delete g_app.m_Renderer;
        g_app.m_Renderer = rive::makeRenderer();
    }

    rive::setContourQuality(contourQuality);
}

void AppShutdown()
{
    sg_shutdown();
    glfwTerminate();
}

void AppRun()
{
    int window_width, window_height;
    float dt             = 0.0f;
    float contourQuality = 0.8888888888888889f;
    int renderModeChoice = (int) rive::getRenderMode();

    uint64_t timeFrame;
    uint64_t timeUpdateRive;
    uint64_t timeRenderRive;

    float mouseLastX = 0.0f;
    float mouseLastY = 0.0f;

    while (!glfwWindowShouldClose(g_app.m_Window))
    {
        glfwGetFramebufferSize(g_app.m_Window, &window_width, &window_height);

        dt             = (float) stm_sec(stm_laptime(&timeFrame));
        ImGuiIO& io    = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(window_width), float(window_height));
        io.DeltaTime   = dt;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::Begin("Configuration");
        ImGui::SliderFloat("Quality", &contourQuality, 0.0f, 1.0f);
        ImGui::RadioButton("Tessellation", &renderModeChoice, (int) rive::MODE_TESSELLATION);
        ImGui::RadioButton("StencilToCover", &renderModeChoice, (int) rive::MODE_STENCIL_TO_COVER);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        for (int i = 0; i < App::MAX_ARTBOARD_CONTEXTS; ++i)
        {
            App::ArtboardContext& ctx = g_app.m_ArtboardContexts[i];

            if (ctx.m_Artboards.Size() == 0)
            {
                continue;
            }

            char cloneCountLabel[64];
            snprintf(cloneCountLabel, sizeof(cloneCountLabel), "%d: Clone Count", i);

            ImGui::Text("Artboard %d: '%s'", i, ctx.m_Artboards[0].m_Artboard->name().c_str());
            ImGui::SliderInt(cloneCountLabel, &ctx.m_CloneCount, 1, 10);
            UpdateArtboardCloneCount(ctx);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("App  Frame  Time: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);
        ImGui::Text("Rive Update Time: %.3f ms", (float) stm_ms(timeUpdateRive));
        ImGui::Text("Rive Render Time: %.3f ms", (float) stm_ms(timeRenderRive));
        ImGui::End();

        if (!io.WantCaptureMouse)
        {
            if (io.MouseDown[0])
            {
                g_app.m_Camera.m_X    += io.MousePos.x - mouseLastX;
                g_app.m_Camera.m_Y    += io.MousePos.y - mouseLastY;
            }

            g_app.m_Camera.m_Zoom += io.MouseWheel;
        }

        mouseLastX = io.MousePos.x;
        mouseLastY = io.MousePos.y;

        AppConfigure((rive::RenderMode) renderModeChoice, contourQuality);

        timeUpdateRive = stm_now();
        AppUpdateRive(dt, window_width, window_height);
        timeUpdateRive = stm_since(timeUpdateRive);

        sg_begin_default_pass(&g_app.m_PassAction, window_width, window_height);

        timeRenderRive = stm_now();
        AppRenderRive(window_width, window_height);
        timeRenderRive = stm_since(timeRenderRive);

        ImGui::Render();
        AppDrawImgui(ImGui::GetDrawData());

        sg_end_pass();
        sg_commit();

        glfwSwapBuffers(g_app.m_Window);
        glfwPollEvents();
    }
}
