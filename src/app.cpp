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

#include <rive/animation/linear_animation_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/file.hpp>

#include "rive/rive_render_api.h"
#include "shaders.glsl.h"

#define VIEWER_WINDOW_NAME "Rive Sokol Viewer"

typedef ImVec2 vs_imgui_params_t;

static struct App
{
    static const int MAX_ARTBOARD_CONTEXTS = 8;
    static const int MAX_IMGUI_VERTICES    = (1<<16);
    static const int MAX_IMGUI_INDICES     = MAX_IMGUI_VERTICES * 3;

    enum DebugView
    {
        DEBUG_VIEW_NONE    = 0,
        DEBUG_VIEW_CONTOUR = 1,
    };

    struct DebugViewData
    {
        float m_ContourSolidColor[4];
    };

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

    // Rive
    rive::HContext             m_Ctx;
    ArtboardContext            m_ArtboardContexts[MAX_ARTBOARD_CONTEXTS];
    rive::HRenderer            m_Renderer;
    // GLFW
    GLFWwindow*                m_Window;
    // Sokol
    sg_shader                  m_MainShader;
    sg_pipeline                m_TessellationIsClippingPipelines[256];
    sg_pipeline                m_TessellationPipeline;
    sg_pipeline                m_TessellationApplyClippingPipeline;
    sg_pipeline                m_StencilPipelineNonClippingCCW;
    sg_pipeline                m_StencilPipelineNonClippingCW;
    sg_pipeline                m_StencilPipelineClippingCCW;
    sg_pipeline                m_StencilPipelineClippingCW;
    sg_pipeline                m_StencilPipelineCoverNonClipping;
    sg_pipeline                m_StencilPipelineCoverClipping;
    sg_pipeline                m_StencilPipelineCoverIsApplyingCLipping;
    sg_pipeline                m_StrokePipeline;
    sg_pass_action             m_PassAction;
    sg_bindings                m_Bindings;
    sg_pipeline                m_DebugViewContourPipeline;
    // Imgui
    sg_buffer                  m_ImguiVxBuffer;
    sg_buffer                  m_ImguiIxBuffer;
    sg_image                   m_ImguiFontImage;
    sg_shader                  m_ImguiShader;
    sg_pipeline                m_ImguiPipeline;
    // App state
    Camera                     m_Camera;
    DebugView                  m_DebugView;
    DebugViewData              m_DebugViewData;
} g_app;

namespace rive
{
    RenderPath* makeRenderPath()
    {
        return createRenderPath(g_app.m_Ctx);
    }

    RenderPaint* makeRenderPaint()
    {
        return createRenderPaint(g_app.m_Ctx);
    }
}

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

rive::Artboard* LoadArtboardFromData(uint8_t* data, size_t dataLength)
{
    rive::File* file    = 0;
    rive::BinaryReader reader = rive::BinaryReader(data, dataLength);
    rive::ImportResult result = rive::File::import(reader, &file);

    if (result != rive::ImportResult::success)
    {
        return 0;
    }

    return file->artboard();
}

static void UpdateArtboardCloneCount(App::ArtboardContext& ctx)
{
    if (ctx.m_CloneCount != (int) ctx.m_Artboards.Size())
    {
        if (ctx.m_CloneCount > (int) ctx.m_Artboards.Size())
        {
            rive::Artboard* artboard = LoadArtboardFromData(ctx.m_Data, ctx.m_DataSize);
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
            for (int i = ctx.m_CloneCount; i < (int) ctx.m_Artboards.Size(); ++i)
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
        rive::Artboard* artboard = LoadArtboardFromData(bytes, bytesLength);
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
            data.m_Artboard = LoadArtboardFromData(ctx->m_Data, ctx->m_DataSize);
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

static void RemoveArtboardContext(uint8_t index)
{
    App::ArtboardContext& ctx = g_app.m_ArtboardContexts[index];

    for (int i = 0; i < (int)ctx.m_Artboards.Size(); ++i)
    {
        App::ArtboardData& data = ctx.m_Artboards[i];
        if (data.m_Artboard)
        {
            delete data.m_Artboard;
            data.m_Artboard = 0;
        }

        if (data.m_AnimationInstance)
        {
            delete data.m_AnimationInstance;
            data.m_AnimationInstance = 0;
        }
    }

    ctx.m_Artboards.SetSize(0);
    ctx.m_Artboards.SetCapacity(0);
    ctx.m_Data       = 0;
    ctx.m_DataSize   = 0;
    ctx.m_CloneCount = 0;
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

static rive::HBuffer AppRequestBufferCallback(rive::HBuffer buffer, rive::BufferType type, void* data, unsigned int dataSize, void* userData)
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

static void AppDestroyBufferCallback(rive::HBuffer buffer, void* userData)
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

static void AppDropCallback(GLFWwindow* window, int count, const char** paths)
{
    for (int i = 0; i < count; ++i)
    {
        AddArtboardFromPath(paths[i]);
    }
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
    glfwSetDropCallback(window, AppDropCallback);

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

    sg_pipeline_desc tessellationApplyingClippingPipeline     = {};
    tessellationApplyingClippingPipeline.shader               = tessellationPipeline.shader;
    tessellationApplyingClippingPipeline.index_type           = SG_INDEXTYPE_UINT32;
    tessellationApplyingClippingPipeline.layout.attrs[0]      = { .format = SG_VERTEXFORMAT_FLOAT2 };
    tessellationApplyingClippingPipeline.colors[0].write_mask = SG_COLORMASK_NONE;

    tessellationApplyingClippingPipeline.stencil = {
        .enabled = true,
        .front = {
            .compare       = SG_COMPAREFUNC_ALWAYS,
            .fail_op       = SG_STENCILOP_KEEP,
            .depth_fail_op = SG_STENCILOP_KEEP,
            .pass_op       = SG_STENCILOP_INCR_CLAMP,
        },
        .back = {
            .compare       = SG_COMPAREFUNC_ALWAYS,
            .fail_op       = SG_STENCILOP_KEEP,
            .depth_fail_op = SG_STENCILOP_KEEP,
            .pass_op       = SG_STENCILOP_INCR_CLAMP,
        },
        .read_mask  = 0xFF,
        .write_mask = 0xFF,
        .ref        = 0x0,
    };

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

    pipelineStencilDesc.stencil.front.compare       = SG_COMPAREFUNC_NOT_EQUAL;
    pipelineStencilDesc.stencil.front.fail_op       = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.front.depth_fail_op = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.front.pass_op       = SG_STENCILOP_REPLACE;
    pipelineStencilDesc.stencil.back.compare        = SG_COMPAREFUNC_NOT_EQUAL;
    pipelineStencilDesc.stencil.back.fail_op        = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.back.depth_fail_op  = SG_STENCILOP_ZERO;
    pipelineStencilDesc.stencil.back.pass_op        = SG_STENCILOP_REPLACE;
    pipelineStencilDesc.stencil.ref                 = 0x80;
    pipelineStencilDesc.stencil.write_mask          = 0xFF;
    pipelineStencilDesc.stencil.read_mask           = 0x7F;
    pipelineStencilDesc.colors[0].write_mask        = SG_COLORMASK_NONE;
    sg_pipeline coverPipelineIsApplyingClipping     = sg_make_pipeline(&pipelineStencilDesc);

    g_app.m_StencilPipelineNonClippingCCW          = stencilPipelineNonClippingCCW;
    g_app.m_StencilPipelineNonClippingCW           = stencilPipelineNonClippingCW;
    g_app.m_StencilPipelineClippingCCW             = stencilPipelineClippingCCW;
    g_app.m_StencilPipelineClippingCW              = stencilPipelineClippingCW;
    g_app.m_StencilPipelineCoverNonClipping        = coverPipelineNonClipping;
    g_app.m_StencilPipelineCoverClipping           = coverPipelineClipping;
    g_app.m_StencilPipelineCoverIsApplyingCLipping = coverPipelineIsApplyingClipping;

    // Stroke pipeline
    sg_pipeline_desc strokePipeline               = {};
    strokePipeline.shader                         = tessellationPipeline.shader;
    strokePipeline.primitive_type                 = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    strokePipeline.index_type                     = SG_INDEXTYPE_NONE;
    strokePipeline.layout.attrs[0]                = { .format = SG_VERTEXFORMAT_FLOAT2 };
    strokePipeline.colors[0].blend.enabled        = true;
    strokePipeline.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    strokePipeline.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    // Debug pipelines
    sg_pipeline_desc debugViewContourPipelineDesc               = {};
    debugViewContourPipelineDesc.shader                         = sg_make_shader(rive_debug_contour_shader_desc(sg_query_backend()));
    debugViewContourPipelineDesc.index_type                     = SG_INDEXTYPE_UINT32;
    debugViewContourPipelineDesc.layout.attrs[0]                = { .format = SG_VERTEXFORMAT_FLOAT2 };
    debugViewContourPipelineDesc.colors[0].blend.enabled        = true;
    debugViewContourPipelineDesc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    debugViewContourPipelineDesc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    debugViewContourPipelineDesc.primitive_type                 = SG_PRIMITIVETYPE_POINTS;

    sg_pass_action passAction   = {0};
    passAction.colors[0].action = SG_ACTION_CLEAR;
    passAction.colors[0].value  = { 0.25f, 0.25f, 0.25f, 1.0f};

    g_app.m_MainShader                        = tessellationPipeline.shader;
    g_app.m_StrokePipeline                    = sg_make_pipeline(&strokePipeline);
    g_app.m_TessellationPipeline              = sg_make_pipeline(&tessellationPipeline);
    g_app.m_TessellationApplyClippingPipeline = sg_make_pipeline(&tessellationApplyingClippingPipeline);
    g_app.m_DebugViewContourPipeline          = sg_make_pipeline(&debugViewContourPipelineDesc);
    g_app.m_PassAction                        = passAction;
    g_app.m_Bindings                          = {};

    ////////////////////////////////////////////////////
    // Rive setup
    ////////////////////////////////////////////////////
    g_app.m_Ctx = rive::createContext();
    rive::setBufferCallbacks(g_app.m_Ctx, AppRequestBufferCallback, AppDestroyBufferCallback);
    rive::setRenderMode(g_app.m_Ctx, rive::MODE_STENCIL_TO_COVER);
    g_app.m_Renderer = rive::createRenderer(g_app.m_Ctx);
    rive::setClippingSupport(g_app.m_Renderer, true);

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
    sg_buffer_desc imguiIxBufferDesc = {};
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

    sg_shader_desc imguiShaderDesc   = {};
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
    rive::newFrame(g_app.m_Renderer);
    rive::Renderer* renderer = (rive::Renderer*) g_app.m_Renderer;

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

static void FillPaintData(rive::HRenderPaint paint, fs_paint_t& uniform)
{
    const rive::PaintData paintData = rive::getPaintData(paint);

    //  Note: Have to use vectors for the stops here aswell, doesn't work otherwise (sokol issue?)
    for (int i = 0; i < (int) paintData.m_StopCount; ++i)
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

static void DebugViewContour(App::GpuBuffer* vxBuffer, App::GpuBuffer* ixBuffer, int numElements, vs_params_t& vsParams, fs_contour_t& fsParams)
{
    memcpy(fsParams.solidColor, g_app.m_DebugViewData.m_ContourSolidColor, sizeof(g_app.m_DebugViewData.m_ContourSolidColor));
    sg_pipeline& pipeline      = g_app.m_DebugViewContourPipeline;
    sg_bindings& bindings      = g_app.m_Bindings;
    bindings.vertex_buffers[0] = vxBuffer->m_Handle;
    bindings.index_buffer      = ixBuffer->m_Handle;
    sg_range vsUniformsRange   = SG_RANGE(vsParams);
    sg_range fsUniformsRange   = SG_RANGE(fsParams);
    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&bindings);
    sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &vsUniformsRange);
    sg_apply_uniforms(SG_SHADERSTAGE_FS, 0, &fsUniformsRange);
    sg_draw(6, numElements, 1);
}

struct AppTessellationRenderer
{
    vs_params_t        m_VsUniforms;
    fs_paint_t         m_FsUniforms;
    sg_range           m_VsUniformsRange;
    sg_range           m_FsUniformsRange;
    rive::HRenderPaint m_Paint;
    uint32_t           m_Width              : 16;
    uint32_t           m_Height             : 16;
    uint32_t           m_AppliedClipCount   : 8;
    uint32_t           m_PaintDirty         : 1;
    uint32_t           m_IsApplyingClipping : 1;
    uint32_t           m_IsClipping         : 1;

    static void Frame(uint32_t width, uint32_t height)
    {
        AppTessellationRenderer obj(width, height);
        for (int i = 0; i < (int) rive::getDrawEventCount(g_app.m_Renderer); ++i)
        {
            const rive::PathDrawEvent evt = rive::getDrawEvent(g_app.m_Renderer, i);
            switch(evt.m_Type)
            {
                case rive::EVENT_SET_PAINT:
                    obj.SetPaint(evt);
                    break;
                case rive::EVENT_DRAW:
                    if (g_app.m_DebugView != App::DEBUG_VIEW_NONE)
                         obj.HandleDebugViews(evt);
                    else obj.DrawPass(evt);
                    break;
                case rive::EVENT_DRAW_STROKE:
                    obj.DrawStroke(evt);
                    break;
                case rive::EVENT_CLIPPING_BEGIN:
                    obj.BeginClipping(evt);
                    break;
                case rive::EVENT_CLIPPING_END:
                    obj.EndClipping(evt);
                    break;
                case rive::EVENT_CLIPPING_DISABLE:
                    obj.CancelClipping(evt);
                default:break;
            }
        }
    }

    AppTessellationRenderer(uint32_t width, uint32_t height)
    {
        m_VsUniforms      = {};
        m_FsUniforms      = {};
        m_Paint           = 0;
        m_VsUniformsRange = SG_RANGE(m_VsUniforms);
        m_FsUniformsRange = SG_RANGE(m_FsUniforms);
        m_Width           = width;
        m_Height          = height;

        m_IsApplyingClipping = 0;
        m_IsClipping         = 0;
        m_PaintDirty         = 0;

        mat4x4 mtxCam;
        GetCameraMatrix(mtxCam, width, height);
        mat4x4_dup((float (*)[4]) m_VsUniforms.projection, mtxCam);
        mat4x4_identity((float (*)[4]) m_VsUniforms.transformLocal);
        sg_apply_viewport(0, 0, width, height, true);
    }

    void SetPaint(const rive::PathDrawEvent& evt)
    {
        if (evt.m_Paint != 0 && m_Paint != evt.m_Paint)
        {
            m_Paint      = evt.m_Paint;
            m_PaintDirty = true;
        }
    }

    void BeginClipping(const rive::PathDrawEvent& evt)
    {
        m_IsApplyingClipping  = true;
        m_IsClipping          = true;
        sg_pass_action action = {};
        action.colors[0]      = { .action = SG_ACTION_DONTCARE };
        action.depth          = { .action = SG_ACTION_DONTCARE };
        action.stencil        = { .action = SG_ACTION_CLEAR    };

        sg_end_pass();
        sg_begin_default_pass(&action, m_Width, m_Height);
    }

    void EndClipping(const rive::PathDrawEvent& evt)
    {
        m_IsApplyingClipping  = false;
        m_AppliedClipCount    = evt.m_AppliedClipCount;

        sg_pass_action action = {};
        action.colors[0]      = { .action = SG_ACTION_DONTCARE };
        action.depth          = { .action = SG_ACTION_DONTCARE };
        action.stencil        = { .action = SG_ACTION_DONTCARE };

        sg_end_pass();
        sg_begin_default_pass(&action, m_Width, m_Height);
    }

    void CancelClipping(const rive::PathDrawEvent& evt)
    {
        m_IsClipping = false;
    }

    sg_pipeline GetIsClippingPipeline(uint8_t v)
    {
        sg_pipeline* p = &g_app.m_TessellationIsClippingPipelines[v];
        if (p->id == SG_INVALID_ID)
        {
            sg_pipeline_desc pDesc               = {};
            pDesc.shader                         = g_app.m_MainShader;
            pDesc.index_type                     = SG_INDEXTYPE_UINT32;
            pDesc.layout.attrs[0]                = { .format = SG_VERTEXFORMAT_FLOAT2 };
            pDesc.colors[0].blend.enabled        = true;
            pDesc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            pDesc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

            pDesc.stencil.enabled                = true;
            pDesc.stencil.front.compare          = SG_COMPAREFUNC_EQUAL;
            pDesc.stencil.front.fail_op          = SG_STENCILOP_KEEP;
            pDesc.stencil.front.depth_fail_op    = SG_STENCILOP_KEEP;
            pDesc.stencil.front.pass_op          = SG_STENCILOP_KEEP;
            pDesc.stencil.back.compare           = SG_COMPAREFUNC_EQUAL;
            pDesc.stencil.back.fail_op           = SG_STENCILOP_KEEP;
            pDesc.stencil.back.depth_fail_op     = SG_STENCILOP_KEEP;
            pDesc.stencil.back.pass_op           = SG_STENCILOP_KEEP;
            pDesc.stencil.ref                    = v;
            pDesc.stencil.write_mask             = 0xFF;
            pDesc.stencil.read_mask              = 0xFF;
            pDesc.colors[0].write_mask           = SG_COLORMASK_RGBA;

            *p = sg_make_pipeline(&pDesc);
        }

        return *p;
    }

    void DrawPass(const rive::PathDrawEvent& evt)
    {
        const rive::DrawBuffers buffers = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, evt.m_Path);
        App::GpuBuffer* vertexBuffer    = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* indexBuffer     = (App::GpuBuffer*) buffers.m_IndexBuffer;

        if (!IS_BUFFER_VALID(vertexBuffer) || !IS_BUFFER_VALID(indexBuffer))
        {
            return;
        }

        Mat2DToMat4(evt.m_TransformWorld, (float (*)[4]) m_VsUniforms.transform);
        int drawLength = (indexBuffer->m_DataSize / sizeof(int)) * 3;

        sg_bindings& bindings      = g_app.m_Bindings;
        bindings.vertex_buffers[0] = vertexBuffer->m_Handle;
        bindings.index_buffer      = indexBuffer->m_Handle;

        sg_pipeline pipeline = g_app.m_TessellationPipeline;

        if (m_IsApplyingClipping)
        {
            pipeline = g_app.m_TessellationApplyClippingPipeline;
        }
        else if (m_IsClipping)
        {
            pipeline = GetIsClippingPipeline(m_AppliedClipCount);
        }

        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &m_VsUniformsRange);

        if (m_PaintDirty && !m_IsApplyingClipping)
        {
            FillPaintData(m_Paint, m_FsUniforms);
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &m_FsUniformsRange);
            m_PaintDirty = false;
        }

        sg_draw(0, drawLength, 1);
    }

    void DrawStroke(const rive::PathDrawEvent& evt)
    {
        const rive::DrawBuffers buffers = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, m_Paint);
        App::GpuBuffer* strokebuffer    = (App::GpuBuffer*) buffers.m_VertexBuffer;
        if (!IS_BUFFER_VALID(strokebuffer))
        {
            return;
        }

        sg_bindings& bindings      = g_app.m_Bindings;
        bindings.vertex_buffers[0] = strokebuffer->m_Handle;
        bindings.index_buffer      = {};

        rive::Mat2D transformWorld  = evt.m_TransformWorld;
        rive::Mat2D transformLocal  = evt.m_TransformLocal;
        Mat2DToMat4(transformWorld, (float (*)[4]) m_VsUniforms.transform);
        Mat2DToMat4(transformLocal, (float (*)[4]) m_VsUniforms.transformLocal);

        sg_apply_pipeline(g_app.m_StrokePipeline);
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &m_VsUniformsRange);
        if (!m_IsApplyingClipping && m_PaintDirty)
        {
            FillPaintData(m_Paint, m_FsUniforms);
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &m_FsUniformsRange);
            m_PaintDirty = false;
        }
        sg_draw(evt.m_OffsetStart, evt.m_OffsetEnd - evt.m_OffsetStart, 1);
    }

    void HandleDebugViews(const rive::PathDrawEvent& evt)
    {
        assert(g_app.m_DebugView == App::DEBUG_VIEW_CONTOUR);
        const rive::DrawBuffers buffers = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, evt.m_Path);
        App::GpuBuffer* vertexBuffer    = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* indexBuffer     = (App::GpuBuffer*) buffers.m_IndexBuffer;

        if (!IS_BUFFER_VALID(vertexBuffer) || !IS_BUFFER_VALID(indexBuffer))
        {
            return;
        }

        const rive::PaintData paintData = getPaintData(m_Paint);

        fs_contour_t fsContourParams                = {};
        memcpy(fsContourParams.color, paintData.m_Colors, sizeof(float) * 4);
        Mat2DToMat4(evt.m_TransformWorld, (float (*)[4]) m_VsUniforms.transform);
        DebugViewContour(
            vertexBuffer,
            indexBuffer,
            (indexBuffer->m_DataSize / sizeof(int)) * 3,
            m_VsUniforms,
            fsContourParams);
    }
};

struct AppSTCRenderer
{
    vs_params_t        m_VsUniforms;
    fs_paint_t         m_FsUniforms;
    sg_range           m_VsUniformsRange;
    sg_range           m_FsUniformsRange;
    rive::HRenderPaint m_Paint;
    mat4x4             m_CameraMtx;
    uint32_t           m_Width              : 16;
    uint32_t           m_Height             : 16;
    uint8_t            m_PaintDirty         : 1;
    uint8_t            m_IsApplyingClipping : 1;

    static void Frame(uint32_t width, uint32_t height)
    {
        AppSTCRenderer obj(width, height);
        for (int i = 0; i < (int) rive::getDrawEventCount(g_app.m_Renderer); ++i)
        {
            const rive::PathDrawEvent evt = rive::getDrawEvent(g_app.m_Renderer, i);
            switch(evt.m_Type)
            {
                case rive::EVENT_SET_PAINT:
                    obj.SetPaint(evt);
                    break;
                case rive::EVENT_DRAW_STENCIL:
                    if (g_app.m_DebugView != App::DEBUG_VIEW_NONE)
                         obj.HandleDebugViews(evt);
                    else obj.StencilPass(evt);
                    break;
                case rive::EVENT_DRAW_COVER:
                    if (g_app.m_DebugView == App::DEBUG_VIEW_NONE)
                        obj.CoverPass(evt);
                    break;
                case rive::EVENT_DRAW_STROKE:
                    obj.DrawStroke(evt);
                    break;
                case rive::EVENT_CLIPPING_BEGIN:
                    obj.BeginClipping(evt);
                    break;
                case rive::EVENT_CLIPPING_END:
                    obj.EndClipping(evt);
                    break;
                default:break;
            }
        }
    }

    AppSTCRenderer(uint32_t width, uint32_t height)
    {
        m_VsUniforms      = {};
        m_FsUniforms      = {};
        m_Paint           = 0;
        m_PaintDirty      = false;
        m_VsUniformsRange = SG_RANGE(m_VsUniforms);
        m_FsUniformsRange = SG_RANGE(m_FsUniforms);
        m_Width           = width;
        m_Height          = height;

        mat4x4 mtxCam;
        GetCameraMatrix(mtxCam, width, height);
        mat4x4_dup((float (*)[4]) m_VsUniforms.projection, mtxCam);
        mat4x4_dup(m_CameraMtx, mtxCam);

        mat4x4_identity((float (*)[4]) m_VsUniforms.transformLocal);
        sg_apply_viewport(0, 0, width, height, true);
    }

    void SetPaint(const rive::PathDrawEvent& evt)
    {
        if (evt.m_Paint != 0 && m_Paint != evt.m_Paint)
        {
            m_Paint      = evt.m_Paint;
            m_PaintDirty = true;
        }
    }

    void BeginClipping(const rive::PathDrawEvent& evt)
    {
        m_IsApplyingClipping  = true;
        sg_pass_action action = {};
        action.colors[0]      = { .action = SG_ACTION_DONTCARE };
        action.depth          = { .action = SG_ACTION_DONTCARE };
        action.stencil        = { .action = SG_ACTION_CLEAR, .value = 0x00 };

        sg_end_pass();
        sg_begin_default_pass(&action, m_Width, m_Height);
    }

    void EndClipping(const rive::PathDrawEvent& evt)
    {
        m_IsApplyingClipping  = false;
        sg_pass_action action = {};
        action.colors[0]      = { .action = SG_ACTION_DONTCARE };
        action.depth          = { .action = SG_ACTION_DONTCARE };
        action.stencil        = { .action = SG_ACTION_DONTCARE };

        sg_end_pass();
        sg_begin_default_pass(&action, m_Width, m_Height);
    }

    void StencilPass(const rive::PathDrawEvent& evt)
    {
        const rive::DrawBuffers buffers     = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, evt.m_Path);
        App::GpuBuffer* contourVertexBuffer = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* contourIndexBuffer  = (App::GpuBuffer*) buffers.m_IndexBuffer;

        if (!IS_BUFFER_VALID(contourVertexBuffer) ||
            !IS_BUFFER_VALID(contourIndexBuffer))
        {
            return;
        }

        sg_bindings& bindings = g_app.m_Bindings;
        sg_pipeline pipeline = {};

        if (evt.m_IsClipping)
        {
            if (evt.m_IsEvenOdd && (evt.m_Idx % 2) != 0)
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
            if (evt.m_IsEvenOdd && (evt.m_Idx % 2) != 0)
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

        int vertexCount   = contourVertexBuffer->m_DataSize / (sizeof(float) * 2);
        int triangleCount = vertexCount - 5;

        if (vertexCount < 5)
            return;

        rive::Mat2D transformWorld  = evt.m_TransformWorld;
        Mat2DToMat4(transformWorld, (float (*)[4]) m_VsUniforms.transform);
        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &m_VsUniformsRange);
        sg_draw(6, triangleCount * 3, 1);
    }

    void CoverPass(const rive::PathDrawEvent& evt)
    {
        const rive::DrawBuffers buffers   = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, evt.m_Path);
        App::GpuBuffer* coverVertexBuffer = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* coverIndexBuffer  = (App::GpuBuffer*) buffers.m_IndexBuffer;

        if (!IS_BUFFER_VALID(coverVertexBuffer) ||
            !IS_BUFFER_VALID(coverIndexBuffer))
        {
            return;
        }

        sg_bindings& bindings = g_app.m_Bindings;
        bindings.vertex_buffers[0] = coverVertexBuffer->m_Handle;
        bindings.index_buffer      = coverIndexBuffer->m_Handle;

        rive::Mat2D transformWorld  = evt.m_TransformWorld;
        rive::Mat2D transformLocal  = evt.m_TransformLocal;
        Mat2DToMat4(transformWorld, (float (*)[4]) m_VsUniforms.transform);
        Mat2DToMat4(transformLocal, (float (*)[4]) m_VsUniforms.transformLocal);

        sg_pipeline pipeline = {};
        bool restoreCamera = false;

        if (m_IsApplyingClipping)
        {
            pipeline = g_app.m_StencilPipelineCoverIsApplyingCLipping;

            if (evt.m_IsClipping)
            {
                mat4x4_identity((float (*)[4]) m_VsUniforms.projection);
                mat4x4_identity((float (*)[4]) m_VsUniforms.transform);
                restoreCamera = true;
            }
        }
        else
        {
            if (evt.m_IsClipping)
            {
                pipeline = g_app.m_StencilPipelineCoverClipping;
            }
            else
            {
                pipeline = g_app.m_StencilPipelineCoverNonClipping;
            }
        }

        sg_apply_pipeline(pipeline);
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &m_VsUniformsRange);
        if (!m_IsApplyingClipping && m_PaintDirty)
        {
            FillPaintData(m_Paint, m_FsUniforms);
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &m_FsUniformsRange);
            m_PaintDirty = false;
        }
        sg_draw(0, 2 * 3, 1);

        if (restoreCamera)
        {
            mat4x4_dup((float (*)[4]) m_VsUniforms.projection, m_CameraMtx);
        }
    }

    void DrawStroke(const rive::PathDrawEvent& evt)
    {
        const rive::DrawBuffers buffers = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, m_Paint);
        App::GpuBuffer* strokebuffer    = (App::GpuBuffer*) buffers.m_VertexBuffer;
        if (!IS_BUFFER_VALID(strokebuffer))
        {
            return;
        }

        sg_bindings& bindings      = g_app.m_Bindings;
        bindings.vertex_buffers[0] = strokebuffer->m_Handle;
        bindings.index_buffer      = {};

        rive::Mat2D transformWorld  = evt.m_TransformWorld;
        rive::Mat2D transformLocal  = evt.m_TransformLocal;
        Mat2DToMat4(transformWorld, (float (*)[4]) m_VsUniforms.transform);
        Mat2DToMat4(transformLocal, (float (*)[4]) m_VsUniforms.transformLocal);

        sg_apply_pipeline(g_app.m_StrokePipeline);
        sg_apply_bindings(&bindings);
        sg_apply_uniforms(SG_SHADERSTAGE_VS, 0, &m_VsUniformsRange);
        if (!m_IsApplyingClipping && m_PaintDirty)
        {
            FillPaintData(m_Paint, m_FsUniforms);
            sg_apply_uniforms(SG_SHADERSTAGE_FS, SLOT_fs_paint, &m_FsUniformsRange);
            m_PaintDirty = false;
        }
        sg_draw(evt.m_OffsetStart, evt.m_OffsetEnd - evt.m_OffsetStart, 1);
    }

    void HandleDebugViews(const rive::PathDrawEvent& evt)
    {
        assert(g_app.m_DebugView == App::DEBUG_VIEW_CONTOUR);
        const rive::DrawBuffers buffers     = rive::getDrawBuffers(g_app.m_Ctx, g_app.m_Renderer, evt.m_Path);
        App::GpuBuffer* contourVertexBuffer = (App::GpuBuffer*) buffers.m_VertexBuffer;
        App::GpuBuffer* contourIndexBuffer  = (App::GpuBuffer*) buffers.m_IndexBuffer;
        if (IS_BUFFER_VALID(contourVertexBuffer) && IS_BUFFER_VALID(contourIndexBuffer))
        {
            const rive::PaintData paintData = rive::getPaintData(m_Paint);

            fs_contour_t fsContourParams = {};
            memcpy(fsContourParams.color, paintData.m_Colors, sizeof(float) * 4);

            rive::Mat2D transformWorld  = evt.m_TransformWorld;
            Mat2DToMat4(transformWorld, (float (*)[4]) m_VsUniforms.transform);

            DebugViewContour(
                contourVertexBuffer,
                contourIndexBuffer,
                contourIndexBuffer->m_DataSize / sizeof(int) - 5,
                m_VsUniforms,
                fsContourParams);
        }
    }
};
#undef IS_BUFFER_VALID

void AppRenderRive(uint32_t width, uint32_t height)
{
    switch(rive::getRenderMode(g_app.m_Ctx))
    {
        case rive::MODE_TESSELLATION:
            AppTessellationRenderer::Frame(width, height);
            break;
        case rive::MODE_STENCIL_TO_COVER:
            AppSTCRenderer::Frame(width, height);
            break;
        default:break;
    }
}

void AppConfigure(rive::RenderMode renderMode, float contourQuality, float* backgroundColor, bool clippingSupported)
{
    g_app.m_PassAction.colors[0].value.r = backgroundColor[0];
    g_app.m_PassAction.colors[0].value.g = backgroundColor[1];
    g_app.m_PassAction.colors[0].value.b = backgroundColor[2];

    if (rive::getRenderMode(g_app.m_Ctx) != renderMode)
    {
        rive::setRenderMode(g_app.m_Ctx, renderMode);

        for (int i = 0; i < App::MAX_ARTBOARD_CONTEXTS; ++i)
        {
            ReloadArtboardContext(&g_app.m_ArtboardContexts[i]);
        }

        rive::destroyRenderer(g_app.m_Renderer);
        g_app.m_Renderer = rive::createRenderer(g_app.m_Ctx);
    }

    rive::setClippingSupport(g_app.m_Renderer, g_app.m_DebugView == App::DEBUG_VIEW_NONE && clippingSupported);
    rive::setContourQuality(g_app.m_Renderer, contourQuality);
}

void AppShutdown()
{
    rive::destroyRenderer(g_app.m_Renderer);
    rive::destroyContext(g_app.m_Ctx);
    sg_shutdown();
    glfwTerminate();
}

void AppRun()
{
    int windowWidth          = 0;
    int windowHeight         = 0;
    float dt                 = 0.0f;
    float contourQuality     = 0.8888888888888889f;
    int renderModeChoice     = (int) rive::getRenderMode(g_app.m_Ctx);
    float mouseLastX         = 0.0f;
    float mouseLastY         = 0.0f;
    float backgroundColor[3] = { 0.25f, 0.25f, 0.25f };
    bool clippingSupported   = rive::getClippingSupport(g_app.m_Renderer);

    uint64_t timeFrame;
    uint64_t timeUpdateRive;
    uint64_t timeRenderRive;

    while (!glfwWindowShouldClose(g_app.m_Window))
    {
        glfwGetFramebufferSize(g_app.m_Window, &windowWidth, &windowHeight);

        dt             = (float) stm_sec(stm_laptime(&timeFrame));
        ImGuiIO& io    = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(windowWidth), float(windowHeight));
        io.DeltaTime   = dt;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::Begin("Viewer Configuration");
        ImGui::ColorEdit3("Background Color", backgroundColor);
        ImGui::SliderFloat("Path Quality", &contourQuality, 0.0f, 1.0f);
        ImGui::Checkbox("Clipping", &clippingSupported);

        ImGui::Text("Render Mode");
        ImGui::RadioButton("Tessellation", &renderModeChoice, (int) rive::MODE_TESSELLATION);
        ImGui::RadioButton("Stencil To Cover", &renderModeChoice, (int) rive::MODE_STENCIL_TO_COVER);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Debug View");
        ImGui::RadioButton("None", (int*)&g_app.m_DebugView, (int) App::DEBUG_VIEW_NONE);
        ImGui::RadioButton("Contour", (int*)&g_app.m_DebugView, (int) App::DEBUG_VIEW_CONTOUR);

        if (g_app.m_DebugView == App::DEBUG_VIEW_CONTOUR)
        {
            ImGui::ColorEdit4("Solid Color", g_app.m_DebugViewData.m_ContourSolidColor);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool artboardLoaded = false;
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
            if (ImGui::Button("x"))
            {
                RemoveArtboardContext(i);
            }
            ImGui::SameLine();
            ImGui::SliderInt(cloneCountLabel, &ctx.m_CloneCount, 1, 10);
            UpdateArtboardCloneCount(ctx);

            artboardLoaded = true;
        }

        if (!artboardLoaded)
        {
            ImGui::Text("Drag and drop .riv file(s) to preview them.");
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

        AppConfigure((rive::RenderMode) renderModeChoice, contourQuality, backgroundColor, clippingSupported);

        timeUpdateRive = stm_now();
        AppUpdateRive(dt, windowWidth, windowHeight);
        timeUpdateRive = stm_since(timeUpdateRive);

        sg_begin_default_pass(&g_app.m_PassAction, windowWidth, windowHeight);

        timeRenderRive = stm_now();
        AppRenderRive(windowWidth, windowHeight);
        timeRenderRive = stm_since(timeRenderRive);

        ImGui::Render();
        AppDrawImgui(ImGui::GetDrawData());

        sg_end_pass();
        sg_commit();

        glfwSwapBuffers(g_app.m_Window);
        glfwPollEvents();
    }
}
