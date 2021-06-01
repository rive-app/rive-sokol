#ifndef RIVE_SHARED_H
#define RIVE_SHARED_H

namespace rive
{
    typedef uintptr_t HBuffer;

    enum BufferType
    {
        BUFFER_TYPE_VERTEX_BUFFER = 0,
        BUFFER_TYPE_INDEX_BUFFER  = 1,
    };

    enum PathCommandType
    {
        TYPE_MOVE  = 0,
        TYPE_LINE  = 1,
        TYPE_CUBIC = 2,
        TYPE_CLOSE = 3,
    };

    enum RenderMode
    {
        MODE_TESSELLATION     = 0,
        MODE_STENCIL_TO_COVER = 1,
    };

    enum FillType
    {
        FILL_TYPE_NONE   = 0,
        FILL_TYPE_SOLID  = 1,
        FILL_TYPE_LINEAR = 2,
        FILL_TYPE_RADIAL = 3,
    };

    struct PathCommand
    {
        PathCommandType m_Command;
        float           m_X;
        float           m_Y;
        float           m_OX;
        float           m_OY;
        float           m_IX;
        float           m_IY;
    };

    struct PathDescriptor
    {
        RenderPath* m_Path;
        Mat2D       m_Transform;
    };

    enum PathDrawCallTag
    {
        TAG_NONE    = 0,
        TAG_STENCIL = 1,
        TAG_COVER   = 2,
    };

    struct PathLimits
    {
        float    m_MinX;
        float    m_MinY;
        float    m_MaxX;
        float    m_MaxY;
    };

    enum PathDrawEventType
    {
        EVENT_NONE             = 0,
        EVENT_DRAW             = 1,
        EVENT_DRAW_STENCIL     = 2,
        EVENT_DRAW_COVER       = 3,
        EVENT_SET_PAINT        = 4,
        EVENT_CLIPPING_BEGIN   = 5,
        EVENT_CLIPPING_END     = 6,
        EVENT_CLIPPING_DISABLE = 7,
    };

    struct PathDrawEvent
    {
        PathDrawEventType m_Type;
        RenderPath*       m_Path;
        RenderPaint*      m_Paint;
        Mat2D             m_TransformWorld;
        Mat2D             m_TransformLocal;
        uint32_t          m_Idx              : 22;
        uint32_t          m_AppliedClipCount : 8;
        uint32_t          m_IsEvenOdd        : 1;
        uint32_t          m_IsClipping       : 1;
    };

    struct GradientStop
    {
        unsigned int m_Color;
        float        m_Stop;
    };

    struct SharedRenderPaintBuilder
    {
        jc::Array<GradientStop> m_Stops;
        unsigned int            m_Color;
        FillType                m_GradientType;
        float                   m_StartX;
        float                   m_StartY;
        float                   m_EndX;
        float                   m_EndY;
    };

    struct SharedRenderPaintData
    {
        static const int MAX_STOPS = 16;
        FillType     m_FillType;
        unsigned int m_StopCount;
        float        m_Stops[MAX_STOPS];
        float        m_Colors[MAX_STOPS * 4];
        float        m_GradientLimits[4];
    };

    class SharedRenderPaint : public RenderPaint
    {
    public:
        SharedRenderPaint();
        void color(unsigned int value)                              override;
        void style(RenderPaintStyle value)                          override { m_Style = value; }
        void thickness(float value)                                 override {}
        void join(StrokeJoin value)                                 override {}
        void cap(StrokeCap value)                                   override {}
        void blendMode(BlendMode value)                             override {}
        void linearGradient(float sx, float sy, float ex, float ey) override;
        void radialGradient(float sx, float sy, float ex, float ey) override;
        void addStop(unsigned int color, float stop)                override;
        void completeGradient()                                     override;
        inline SharedRenderPaintData getData()   { return m_Data; }
        inline RenderPaintStyle      getStyle()  { return m_Style; }
        inline bool                  isVisible() { return m_IsVisible; }
    private:
        SharedRenderPaintBuilder* m_Builder;
        SharedRenderPaintData     m_Data;
        RenderPaintStyle          m_Style;
        bool                      m_IsVisible;
    };

    class SharedRenderPath : public RenderPath
    {
    public:
        SharedRenderPath();
        void            reset()                                                           override;
        void            addRenderPath(RenderPath* path, const Mat2D& transform)           override;
        void            fillRule(FillRule value)                                          override;
        void            moveTo(float x, float y)                                          override;
        void            lineTo(float x, float y)                                          override;
        void            cubicTo(float ox, float oy, float ix, float iy, float x, float y) override;
        virtual void    close()                                                           override;
        inline FillRule getFillRule() { return m_FillRule; }
    protected:
        // TODO: use a global buffer or something else
        static const uint32_t COUNTOUR_BUFFER_ELEMENT_COUNT = 512;

        jc::Array<PathCommand>    m_PathCommands;
        jc::Array<PathDescriptor> m_Paths;
        float                     m_ContourVertexData[COUNTOUR_BUFFER_ELEMENT_COUNT * 2];
        uint32_t                  m_ContourVertexCount;
        FillRule                  m_FillRule;
        bool                      m_IsDirty;
        bool                      m_IsShapeDirty;
        bool isShapeDirty();
    };

    class SharedRenderer : public Renderer
    {
    public:
        SharedRenderer();
        void save()                            override;
        void restore()                         override;
        void transform(const Mat2D& transform) override;
        void clipPath(RenderPath* path)        override;
        void startFrame();

        void                       pushDrawEvent(const PathDrawEvent& evt);
        inline uint32_t            getDrawEventCount()            { return m_DrawEvents.Size(); }
        inline const PathDrawEvent getDrawEvent(uint32_t index)   { return m_DrawEvents[index]; }
        inline void                setClippingSupport(bool state) { m_IsClippingSupported = state; };
    protected:
        static const int STACK_ENTRY_MAX_CLIP_PATHS = 16;
        struct StackEntry
        {
            Mat2D          m_Transform;
            PathDescriptor m_ClipPaths[STACK_ENTRY_MAX_CLIP_PATHS];
            uint8_t        m_ClipPathsCount;
        };

        jc::Array<StackEntry>     m_ClipPathStack;
        jc::Array<PathDescriptor> m_ClipPaths;
        jc::Array<PathDescriptor> m_AppliedClips;
        jc::Array<PathDrawEvent>  m_DrawEvents;
        Mat2D                     m_Transform;
        SharedRenderPaint*        m_RenderPaint;
        uint8_t                   m_IsClippingSupported : 1;
        uint8_t                   m_IsClippingDirty     : 1;
        uint8_t                   m_IsClipping          : 1;

        void setPaint(SharedRenderPaint* rp);
    };

    ////////////////////////////////////////////////////
    // Stencil To Cover
    ////////////////////////////////////////////////////
    class StencilToCoverRenderPath;
    class StencilToCoverRenderer : public SharedRenderer
    {
    public:
        void drawPath(RenderPath* path, RenderPaint* paint) override;
    private:
        void applyClipping();
        void applyClipPath(StencilToCoverRenderPath* path, const Mat2D& transform);
    };

    class StencilToCoverRenderPath : public SharedRenderPath
    {
    public:
        struct Buffers
        {
            HBuffer m_ContourVertexBuffer;
            HBuffer m_ContourIndexBuffer;
            HBuffer m_CoverVertexBuffer;
            HBuffer m_CoverIndexBuffer;
        };

        StencilToCoverRenderPath();
        ~StencilToCoverRenderPath();
        void drawMesh(const Mat2D& transform);
        void stencil(SharedRenderer* renderer, const Mat2D& transform, unsigned int idx, bool isEvenOdd);
        void cover(SharedRenderer* renderer, const Mat2D transform, const Mat2D transformLocal);
        inline const Buffers getDrawBuffers() { return m_RenderData; }

    private:
        // TODO: use a global buffer or something else
        static const uint32_t COUNTOUR_BUFFER_ELEMENT_COUNT = 128 * 3;
        uint32_t          m_ContourIndexData[COUNTOUR_BUFFER_ELEMENT_COUNT];
        uint32_t          m_ContourIndexCount;
        Buffers           m_RenderData;
        float             m_ContourError;
        PathLimits        m_Limits;

        void computeContour();
        void updateBuffers();
    };

    ////////////////////////////////////////////////////
    // Triangle Tessellation
    ////////////////////////////////////////////////////
    class TessellationRenderer : public SharedRenderer
    {
    public:
        void drawPath(RenderPath* path, RenderPaint* paint) override;
    private:
        void applyClipping();
    };

    class TessellationRenderPath : public SharedRenderPath
    {
    public:
        struct Buffers
        {
            HBuffer m_VertexBuffer;
            HBuffer m_IndexBuffer;
        };

        TessellationRenderPath();
        ~TessellationRenderPath();
        void                 drawMesh(const Mat2D& transform);
        inline const Buffers getDrawBuffers() { return m_RenderData; }
    private:
        float   m_ContourError;
        Buffers m_RenderData;
        void updateContour();
        void computeContour();
        void addContours(void* tess, const Mat2D& m);
        void updateTesselation();
    };

    ////////////////////////////////////////////////////
    // Helper Functions
    ////////////////////////////////////////////////////
    void segmentCubic(const Vec2D& from,
                      const Vec2D& fromOut,
                      const Vec2D& toIn,
                      const Vec2D& to,
                      float t1,
                      float t2,
                      float minSegmentLength,
                      float distTooFar,
                      float* vertices,
                      uint32_t& verticesCount,
                      PathLimits* pathLimits);

    typedef HBuffer (*RequestBufferCb)(HBuffer buffer, BufferType type, void* data, unsigned int dataSize);
    typedef void    (*DestroyBufferCb)(HBuffer buffer);

    Artboard*  loadArtboardFromData(uint8_t* data, size_t dataLength);
    void       setBufferCallbacks(RequestBufferCb rcb, DestroyBufferCb dcb);
    void       setRenderMode(RenderMode mode);
    void       setContourQuality(float quality);
    float      getContourError();
    RenderMode getRenderMode();
    Renderer*  makeRenderer();
    HBuffer    requestBuffer(HBuffer buffer, BufferType bufferType, void* data, unsigned int dataSize);
    void       destroyBuffer(HBuffer buffer);
}

#endif
