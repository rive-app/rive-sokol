#include <float.h>
#include <jc/array.h>

#include <renderer.hpp>
#include <artboard.hpp>
#include "rive/shared.h"

namespace rive
{
    void StencilToCoverRenderer::applyClipping()
    {

    }


    void StencilToCoverRenderer::drawPath(RenderPath* path, RenderPaint* paint)
    {
        StencilToCoverRenderPath* p = (StencilToCoverRenderPath*) path;
        SharedRenderPaint*       rp = (SharedRenderPaint*) paint;

        if (rp->getStyle() != RenderPaintStyle::fill || !rp->isVisible())
        {
            return;
        }

        bool isEvenOdd = p->getFillRule() == FillRule::evenOdd;
        p->stencil(this, rp, m_Transform, 0, isEvenOdd);
        p->cover(this, rp, m_Transform, Mat2D());
    }

    /* StencilToCoverRenderPath impl */
    StencilToCoverRenderPath::StencilToCoverRenderPath()
    : m_RenderData({})
    , m_ContourError(0.0f)
    {
    }

    StencilToCoverRenderPath::~StencilToCoverRenderPath()
    {
        destroyBuffer(m_RenderData.m_ContourVertexBuffer);
        destroyBuffer(m_RenderData.m_ContourIndexBuffer);
        destroyBuffer(m_RenderData.m_CoverVertexBuffer);
        destroyBuffer(m_RenderData.m_CoverIndexBuffer);
    }

    void StencilToCoverRenderPath::drawMesh(const Mat2D& transform)
    {

    }

    void StencilToCoverRenderPath::computeContour()
    {
        const float minSegmentLength = m_ContourError * m_ContourError;
        const float distTooFar       = m_ContourError;

        m_IsDirty              = false;
        m_ContourIndexCount    = 0;
        m_ContourVertexCount   = 1;
        m_ContourVertexData[0] = 0.0f;
        m_ContourVertexData[1] = 0.0f;

        m_Limits = {
            .m_MinX =  FLT_MAX,
            .m_MinY =  FLT_MAX,
            .m_MaxX = -FLT_MAX,
            .m_MaxY = -FLT_MAX,
        };

        float penX          = 0.0f;
        float penY          = 0.0f;
        float penDownX      = 0.0f;
        float penDownY      = 0.0f;
        float isPenDown     = false;
        int penDownIndex    = 1;
        int nextVertexIndex = 1;

        #define ADD_VERTEX(x,y) \
            { \
                m_ContourVertexData[m_ContourVertexCount * 2]     = x; \
                m_ContourVertexData[m_ContourVertexCount * 2 + 1] = y; \
                m_Limits.m_MinX = fmin(m_Limits.m_MinX, x); \
                m_Limits.m_MinY = fmin(m_Limits.m_MinY, y); \
                m_Limits.m_MaxX = fmax(m_Limits.m_MaxX, x); \
                m_Limits.m_MaxY = fmax(m_Limits.m_MaxY, y); \
                m_ContourVertexCount++; \
            }

        #define ADD_TRIANGLE(p0,p1,p2) \
            { \
                m_ContourIndexData[m_ContourIndexCount++] = p0; \
                m_ContourIndexData[m_ContourIndexCount++] = p1; \
                m_ContourIndexData[m_ContourIndexCount++] = p2; \
            }

        #define PEN_DOWN() \
            if (!isPenDown) \
            { \
                isPenDown = true; \
                penDownX = penX; \
                penDownY = penY; \
                ADD_VERTEX(penX, penY); \
                penDownIndex = nextVertexIndex; \
                nextVertexIndex++; \
            }

        for (int i=0; i < m_PathCommands.Size(); i++)
        {
            const PathCommand& pc = m_PathCommands[i];
            switch(pc.m_Command)
            {
                case TYPE_MOVE:
                    penX = pc.m_X;
                    penY = pc.m_Y;
                    break;
                case TYPE_LINE:
                    PEN_DOWN();
                    ADD_VERTEX(pc.m_X, pc.m_Y);
                    ADD_TRIANGLE(0, nextVertexIndex - 1, nextVertexIndex++);
                    break;
                case TYPE_CUBIC:
                {
                    PEN_DOWN();
                    const int size = m_ContourVertexCount;
                    segmentCubic(
                        Vec2D(penX, penY),
                        Vec2D(pc.m_OX, pc.m_OY),
                        Vec2D(pc.m_IX, pc.m_IY),
                        Vec2D(pc.m_X, pc.m_Y),
                        0.0f,
                        1.0f,
                        minSegmentLength,
                        distTooFar,
                        m_ContourVertexData,
                        m_ContourVertexCount,
                        &m_Limits);

                    const int addedVertices = m_ContourVertexCount - size;
                    for (int i = 0; i < addedVertices; ++i)
                    {
                        ADD_TRIANGLE(0, nextVertexIndex - 1, nextVertexIndex++);
                    }

                    penX = pc.m_X;
                    penY = pc.m_Y;
                } break;
                case TYPE_CLOSE:
                    if (isPenDown)
                    {
                        penX      = penDownX;
                        penY      = penDownY;
                        isPenDown = false;

                        if (m_ContourIndexCount > 0)
                        {
                            // Add the first vertex back to the indices to draw the
                            // close.
                            const int lastIndex = m_ContourIndexData[m_ContourIndexCount - 1];
                            ADD_TRIANGLE(0, lastIndex, penDownIndex);
                        }
                    }
                    break;
            }
        }

        // Always close the fill...
        if (isPenDown)
        {
            const int lastIndex = m_ContourIndexData[m_ContourIndexCount - 1];
            ADD_TRIANGLE(0, lastIndex, penDownIndex);
        }

        m_ContourVertexData[0] = m_Limits.m_MinX;
        m_ContourVertexData[1] = m_Limits.m_MinY;

        #undef ADD_VERTEX
        #undef PEN_DOWN
    }

    void StencilToCoverRenderPath::updateBuffers()
    {
        const float coverVertexData[] = {
            m_Limits.m_MinX, m_Limits.m_MinY, m_Limits.m_MaxX, m_Limits.m_MinY,
            m_Limits.m_MaxX, m_Limits.m_MaxY, m_Limits.m_MinX, m_Limits.m_MaxY,
        };

        const int coverIndexData[] = {
            0, 1, 2,
            2, 3, 0,
        };

        m_RenderData.m_ContourVertexBuffer = requestBuffer(m_RenderData.m_ContourVertexBuffer, BUFFER_TYPE_VERTEX_BUFFER, m_ContourVertexData, m_ContourVertexCount * sizeof(float) * 2);
        m_RenderData.m_ContourIndexBuffer  = requestBuffer(m_RenderData.m_ContourIndexBuffer, BUFFER_TYPE_INDEX_BUFFER, m_ContourIndexData, m_ContourIndexCount * sizeof(int));
        m_RenderData.m_CoverVertexBuffer   = requestBuffer(m_RenderData.m_CoverVertexBuffer, BUFFER_TYPE_VERTEX_BUFFER, (void*) coverVertexData, sizeof(coverVertexData));
        m_RenderData.m_CoverIndexBuffer    = requestBuffer(m_RenderData.m_CoverIndexBuffer, BUFFER_TYPE_INDEX_BUFFER, (void*) coverIndexData, sizeof(coverIndexData));
    }

    void StencilToCoverRenderPath::stencil(SharedRenderer* renderer, SharedRenderPaint* paint, const Mat2D& transform, unsigned int idx, bool isEvenOdd)
    {
        if (m_Paths.Size() > 0)
        {
            for (int i = 0; i < m_Paths.Size(); ++i)
            {
                StencilToCoverRenderPath* stcPath = (StencilToCoverRenderPath*) m_Paths[i].m_Path;
                if (stcPath)
                {
                    Mat2D stcPathTransform;
                    Mat2D::multiply(stcPathTransform, transform, m_Paths[i].m_Transform);
                    stcPath->stencil(renderer, paint, stcPathTransform, idx++, isEvenOdd);
                }
            }
            return;
        }

        float currentContourError = getContourError();
        m_IsDirty                 = m_IsDirty || currentContourError != m_ContourError;
        m_ContourError            = currentContourError;

        if (m_IsDirty)
        {
            computeContour();
            updateBuffers();
        }

        renderer->pushDrawCall({
            .m_Path           = this,
            .m_Paint          = paint,
            .m_TransformWorld = transform,
            .m_IsEvenOdd      = isEvenOdd,
            .m_Idx            = idx,
            .m_Tag            = TAG_STENCIL,
        });
    }

    void StencilToCoverRenderPath::cover(SharedRenderer* renderer, SharedRenderPaint* paint, const Mat2D transform, const Mat2D transformLocal)
    {
        if (m_Paths.Size() > 0)
        {
            for (int i = 0; i < m_Paths.Size(); ++i)
            {
                StencilToCoverRenderPath* stcPath = (StencilToCoverRenderPath*) m_Paths[i].m_Path;
                if (stcPath)
                {
                    Mat2D stcWorldTransform;
                    Mat2D::multiply(stcWorldTransform, transform, m_Paths[i].m_Transform);
                    stcPath->cover(renderer, paint, stcWorldTransform, m_Paths[i].m_Transform);
                }
            }
            return;
        }

        float currentContourError = getContourError();
        m_IsDirty                 = m_IsDirty || currentContourError != m_ContourError;
        m_ContourError            = currentContourError;

        if (m_IsDirty)
        {
            computeContour();
            updateBuffers();
        }

        renderer->pushDrawCall({
            .m_Path           = this,
            .m_Paint          = paint,
            .m_TransformWorld = transform,
            .m_TransformLocal = transformLocal,
            .m_Tag            = TAG_COVER,
        });
    }
}
