#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QWheelEvent>
#include <QWidget>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <dlfcn.h>

#include <clocale>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define WLX_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr int kListPluginOk = 0;
constexpr int kListPluginError = 1;

constexpr const char* kDetectString =
    "(EXT=\"AVI\")|(EXT=\"MKV\")|(EXT=\"FLV\")|(EXT=\"MPG\")|(EXT=\"MPEG\")|(EXT=\"MP4\")|"
    "(EXT=\"VOB\")|(EXT=\"WEBM\")|(EXT=\"WMV\")|(EXT=\"MOV\")|(EXT=\"3GP\")|(EXT=\"BIK\")|"
    "(EXT=\"MP3\")|(EXT=\"FLAC\")|(EXT=\"APE\")|(EXT=\"ALAC\")|(EXT=\"OGG\")|(EXT=\"WMA\")|"
    "(EXT=\"WAV\")|(EXT=\"M4A\")|(EXT=\"AAC\")|(EXT=\"AC3\")|(EXT=\"AIFF\")|(EXT=\"VOC\")|"
    "(EXT=\"ROQ\")|(EXT=\"AMR\")|(EXT=\"OPUS\")";

constexpr const char* kLibmpvCandidates[] = {"libmpv.so.2", "libmpv.so"};
constexpr int kDlopenFlagsIsolatingLibmpvLua = RTLD_NOW | RTLD_DEEPBIND;

constexpr const char* kSoftwareFrameFormat = "bgr0";
constexpr QImage::Format kFrameImageFormat = QImage::Format_RGB32;
constexpr int kBytesPerPixel = 4;
constexpr int kSimdFriendlyAlignment = 64;
constexpr quint32 kOpaqueAlphaMask = 0xFF000000u;
constexpr GLenum kOpaqueFramebufferFormat = GL_RGB8;

constexpr const char* kEofReachedProperty = "eof-reached";
constexpr const char* kVideoOutputConfiguredProperty = "vo-configured";
constexpr uint64_t kEofObserverId = 1;
constexpr uint64_t kVideoOutputObserverId = 2;
constexpr qint64 kVideoOutputReleaseTimeoutMs = 1000;
constexpr const char* kPersistentOsdDurationMs = "2147483647";
constexpr const char* kAudioInfoTemplate =
    "${filename}\n"
    "${current-tracks/audio/codec} · ${current-tracks/audio/demux-samplerate} Hz · "
    "${current-tracks/audio/demux-channel-count} ch · ${duration}\n"
    "${filtered-metadata}";

struct MpvOption {
    const char* name;
    const char* value;
};

constexpr MpvOption kMpvOptions[] = {
    {"vo", "libmpv"},
    {"hwdec", "auto-safe"},
    {"keep-open", "yes"},
    {"osc", "yes"},
    {"osd-font-size", "32"},
    {"script-opts", "osc-visibility=always,osc-idlescreen=no"},
    {"input-default-bindings", "yes"},
    {"audio-client-name", "Double Commander"},
};

constexpr MpvOption kMpvOptionsRequiringRenderContext[] = {
    {"force-window", "yes"},
};

struct Libmpv {
    decltype(&mpv_create) create = nullptr;
    decltype(&mpv_initialize) initialize = nullptr;
    decltype(&mpv_set_option_string) set_option_string = nullptr;
    decltype(&mpv_command) command = nullptr;
    decltype(&mpv_get_property) get_property = nullptr;
    decltype(&mpv_observe_property) observe_property = nullptr;
    decltype(&mpv_wait_event) wait_event = nullptr;
    decltype(&mpv_set_wakeup_callback) set_wakeup_callback = nullptr;
    decltype(&mpv_terminate_destroy) terminate_destroy = nullptr;
    decltype(&mpv_render_context_create) render_context_create = nullptr;
    decltype(&mpv_render_context_set_update_callback) render_context_set_update_callback = nullptr;
    decltype(&mpv_render_context_update) render_context_update = nullptr;
    decltype(&mpv_render_context_render) render_context_render = nullptr;
    decltype(&mpv_render_context_free) render_context_free = nullptr;
};

template <typename Fn>
Fn symbol(void* library, const char* name)
{
    return reinterpret_cast<Fn>(dlsym(library, name));
}

void* openLibmpv()
{
    for (const char* name : kLibmpvCandidates) {
        void* library = dlopen(name, kDlopenFlagsIsolatingLibmpvLua);
        if (library)
            return library;
    }
    std::fprintf(stderr, "wlxmpv: %s\n", dlerror());
    return nullptr;
}

Libmpv loadLibmpv()
{
    Libmpv api;
    void* library = openLibmpv();
    if (!library)
        return api;
    api.create = symbol<decltype(api.create)>(library, "mpv_create");
    api.initialize = symbol<decltype(api.initialize)>(library, "mpv_initialize");
    api.set_option_string = symbol<decltype(api.set_option_string)>(library, "mpv_set_option_string");
    api.command = symbol<decltype(api.command)>(library, "mpv_command");
    api.get_property = symbol<decltype(api.get_property)>(library, "mpv_get_property");
    api.observe_property = symbol<decltype(api.observe_property)>(library, "mpv_observe_property");
    api.wait_event = symbol<decltype(api.wait_event)>(library, "mpv_wait_event");
    api.set_wakeup_callback = symbol<decltype(api.set_wakeup_callback)>(library, "mpv_set_wakeup_callback");
    api.terminate_destroy = symbol<decltype(api.terminate_destroy)>(library, "mpv_terminate_destroy");
    api.render_context_create = symbol<decltype(api.render_context_create)>(library, "mpv_render_context_create");
    api.render_context_set_update_callback =
        symbol<decltype(api.render_context_set_update_callback)>(library, "mpv_render_context_set_update_callback");
    api.render_context_update = symbol<decltype(api.render_context_update)>(library, "mpv_render_context_update");
    api.render_context_render = symbol<decltype(api.render_context_render)>(library, "mpv_render_context_render");
    api.render_context_free = symbol<decltype(api.render_context_free)>(library, "mpv_render_context_free");
    return api;
}

bool isComplete(const Libmpv& api)
{
    return api.create && api.initialize && api.set_option_string && api.command && api.get_property &&
           api.observe_property &&
           api.wait_event && api.set_wakeup_callback && api.terminate_destroy && api.render_context_create &&
           api.render_context_set_update_callback && api.render_context_update && api.render_context_render &&
           api.render_context_free;
}

const Libmpv* libmpv()
{
    static const Libmpv api = loadLibmpv();
    if (!isComplete(api))
        return nullptr;
    return &api;
}

void useCNumericLocaleRequiredByLibmpv()
{
    std::setlocale(LC_NUMERIC, "C");
}

int alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

void replacePaddingByteWithOpaqueAlpha(uchar* buffer, int width, int height, int stride)
{
    for (int y = 0; y < height; ++y) {
        quint32* row = reinterpret_cast<quint32*>(buffer + size_t(stride) * y);
        for (int x = 0; x < width; ++x)
            row[x] |= kOpaqueAlphaMask;
    }
}

void freeAlignedBuffer(void* buffer)
{
    std::free(buffer);
}

struct FrameRequest {
    QSize size;
    qreal ratio = 1;
};

const QSize kFrameConsumptionSize(1, 1);

QImage wrapFrameBuffer(void* buffer, const QSize& size, int stride, qreal ratio)
{
    QImage frame(static_cast<uchar*>(buffer), size.width(), size.height(), stride, kFrameImageFormat,
                 &freeAlignedBuffer, buffer);
    frame.setDevicePixelRatio(ratio);
    return frame;
}

class RenderBackend {
public:
    virtual ~RenderBackend() = default;
    virtual mpv_render_context* createContext(const Libmpv& api, mpv_handle* mpv) = 0;
    virtual QImage render(const Libmpv& api, mpv_render_context* renderContext, const FrameRequest& request) = 0;
};

class SoftwareBackend final : public RenderBackend {
public:
    mpv_render_context* createContext(const Libmpv& api, mpv_handle* mpv) override;
    QImage render(const Libmpv& api, mpv_render_context* renderContext, const FrameRequest& request) override;
};

mpv_render_context* SoftwareBackend::createContext(const Libmpv& api, mpv_handle* mpv)
{
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* renderContext = nullptr;
    const int status = api.render_context_create(&renderContext, mpv, params);
    if (status < 0)
        return nullptr;
    return renderContext;
}

QImage SoftwareBackend::render(const Libmpv& api, mpv_render_context* renderContext, const FrameRequest& request)
{
    const int width = request.size.width();
    const int height = request.size.height();
    const int stride = alignUp(width * kBytesPerPixel, kSimdFriendlyAlignment);
    void* buffer = std::aligned_alloc(kSimdFriendlyAlignment, size_t(stride) * height);

    int frameSize[2] = {width, height};
    size_t renderStride = stride;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, frameSize},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(kSoftwareFrameFormat)},
        {MPV_RENDER_PARAM_SW_STRIDE, &renderStride},
        {MPV_RENDER_PARAM_SW_POINTER, buffer},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    api.render_context_render(renderContext, params);
    replacePaddingByteWithOpaqueAlpha(static_cast<uchar*>(buffer), width, height, stride);
    return wrapFrameBuffer(buffer, request.size, stride, request.ratio);
}

class OpenGlBackend final : public RenderBackend {
public:
    explicit OpenGlBackend(QOffscreenSurface* surface) : surface_(surface) {}
    ~OpenGlBackend() override;

    mpv_render_context* createContext(const Libmpv& api, mpv_handle* mpv) override;
    QImage render(const Libmpv& api, mpv_render_context* renderContext, const FrameRequest& request) override;

private:
    static void* procAddress(void* context, const char* name);
    void ensureFramebuffer(const QSize& size);
    QImage readFramebuffer(const FrameRequest& request);

    QOffscreenSurface* surface_;
    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOpenGLFramebufferObject> framebuffer_;
};

OpenGlBackend::~OpenGlBackend()
{
    framebuffer_.reset();
    if (context_)
        context_->doneCurrent();
}

void* OpenGlBackend::procAddress(void* context, const char* name)
{
    return reinterpret_cast<void*>(static_cast<QOpenGLContext*>(context)->getProcAddress(name));
}

mpv_render_context* OpenGlBackend::createContext(const Libmpv& api, mpv_handle* mpv)
{
    context_ = std::make_unique<QOpenGLContext>();
    context_->setFormat(surface_->format());
    const bool created = context_->create();
    if (!created)
        return nullptr;
    const bool current = context_->makeCurrent(surface_);
    if (!current)
        return nullptr;

    mpv_opengl_init_params glParams{};
    glParams.get_proc_address = &OpenGlBackend::procAddress;
    glParams.get_proc_address_ctx = context_.get();
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glParams},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* renderContext = nullptr;
    const int status = api.render_context_create(&renderContext, mpv, params);
    if (status < 0)
        return nullptr;
    return renderContext;
}

void OpenGlBackend::ensureFramebuffer(const QSize& size)
{
    const bool reusable = framebuffer_ && framebuffer_->size() == size;
    if (reusable)
        return;
    framebuffer_.reset();
    framebuffer_ = std::make_unique<QOpenGLFramebufferObject>(size, QOpenGLFramebufferObject::NoAttachment,
                                                              GL_TEXTURE_2D, kOpaqueFramebufferFormat);
}

QImage OpenGlBackend::render(const Libmpv& api, mpv_render_context* renderContext, const FrameRequest& request)
{
    ensureFramebuffer(request.size);
    mpv_opengl_fbo target{};
    target.fbo = int(framebuffer_->handle());
    target.w = request.size.width();
    target.h = request.size.height();
    target.internal_format = int(kOpaqueFramebufferFormat);
    int flipY = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    api.render_context_render(renderContext, params);
    return readFramebuffer(request);
}

QImage OpenGlBackend::readFramebuffer(const FrameRequest& request)
{
    const int stride = request.size.width() * kBytesPerPixel;
    void* buffer = std::malloc(size_t(stride) * request.size.height());
    QOpenGLFunctions* gl = context_->functions();
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_->handle());
    gl->glPixelStorei(GL_PACK_ALIGNMENT, kBytesPerPixel);
    gl->glReadPixels(0, 0, request.size.width(), request.size.height(), GL_BGRA, GL_UNSIGNED_BYTE, buffer);
    return wrapFrameBuffer(buffer, request.size, stride, request.ratio);
}

class FrameRenderer {
public:
    using ReadyHandler = std::function<void(bool)>;
    using FrameReadyHandler = std::function<void()>;

    FrameRenderer(const Libmpv& api, mpv_handle* mpv, QOffscreenSurface* glSurface, ReadyHandler onReady,
                  FrameReadyHandler onFrameReady);
    ~FrameRenderer();

    void start();
    void stop();
    void requestSize(const QSize& size, qreal ratio);
    QImage latestFrame();

private:
    static void onRenderUpdate(void* context);

    void markUpdatePending();
    void run();
    mpv_render_context* createRenderContext();
    void renderLoop(mpv_render_context* renderContext);
    void consumeFrameWithoutDisplay(mpv_render_context* renderContext, const FrameRequest& request);
    void renderIfNeeded(mpv_render_context* renderContext, const FrameRequest& request);

    const Libmpv& api_;
    mpv_handle* mpv_;
    QOffscreenSurface* glSurface_;
    ReadyHandler onReady_;
    FrameReadyHandler onFrameReady_;
    std::unique_ptr<RenderBackend> backend_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool updatePending_ = false;
    FrameRequest request_;
    QImage latestFrame_;
    QSize renderedSize_;
    std::thread thread_;
};

FrameRenderer::FrameRenderer(const Libmpv& api, mpv_handle* mpv, QOffscreenSurface* glSurface, ReadyHandler onReady,
                             FrameReadyHandler onFrameReady)
    : api_(api), mpv_(mpv), glSurface_(glSurface), onReady_(std::move(onReady)),
      onFrameReady_(std::move(onFrameReady))
{
}

FrameRenderer::~FrameRenderer()
{
    stop();
}

void FrameRenderer::onRenderUpdate(void* context)
{
    static_cast<FrameRenderer*>(context)->markUpdatePending();
}

void FrameRenderer::start()
{
    thread_ = std::thread([this] { run(); });
}

void FrameRenderer::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable())
        thread_.join();
}

void FrameRenderer::markUpdatePending()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        updatePending_ = true;
    }
    wake_.notify_one();
}

void FrameRenderer::requestSize(const QSize& size, qreal ratio)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool unchanged = request_.size == size && qFuzzyCompare(request_.ratio, ratio);
        if (unchanged)
            return;
        request_ = FrameRequest{size, ratio};
        updatePending_ = true;
    }
    wake_.notify_one();
}

QImage FrameRenderer::latestFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latestFrame_;
}

void FrameRenderer::run()
{
    mpv_render_context* renderContext = createRenderContext();
    if (renderContext)
        api_.render_context_set_update_callback(renderContext, &FrameRenderer::onRenderUpdate, this);
    onReady_(renderContext != nullptr);
    if (!renderContext)
        return;
    renderLoop(renderContext);
    api_.render_context_free(renderContext);
    backend_.reset();
}

mpv_render_context* FrameRenderer::createRenderContext()
{
    if (glSurface_) {
        auto gl = std::make_unique<OpenGlBackend>(glSurface_);
        mpv_render_context* renderContext = gl->createContext(api_, mpv_);
        if (renderContext) {
            backend_ = std::move(gl);
            return renderContext;
        }
        std::fprintf(stderr, "wlxmpv: OpenGL rendering unavailable, falling back to software\n");
    }
    auto software = std::make_unique<SoftwareBackend>();
    mpv_render_context* renderContext = software->createContext(api_, mpv_);
    if (renderContext)
        backend_ = std::move(software);
    return renderContext;
}

void FrameRenderer::renderLoop(mpv_render_context* renderContext)
{
    for (;;) {
        FrameRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || updatePending_; });
            if (stopping_)
                return;
            updatePending_ = false;
            request = request_;
        }
        renderIfNeeded(renderContext, request);
    }
}

void FrameRenderer::consumeFrameWithoutDisplay(mpv_render_context* renderContext, const FrameRequest& request)
{
    backend_->render(api_, renderContext, FrameRequest{kFrameConsumptionSize, request.ratio});
    renderedSize_ = QSize();
}

void FrameRenderer::renderIfNeeded(mpv_render_context* renderContext, const FrameRequest& request)
{
    const uint64_t updateFlags = api_.render_context_update(renderContext);
    const bool hasNewFrame = updateFlags & MPV_RENDER_UPDATE_FRAME;
    if (request.size.isEmpty()) {
        if (hasNewFrame)
            consumeFrameWithoutDisplay(renderContext, request);
        return;
    }
    const bool resized = request.size != renderedSize_;
    if (!hasNewFrame && !resized)
        return;
    const QImage frame = backend_->render(api_, renderContext, request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latestFrame_ = frame;
    }
    renderedSize_ = request.size;
    onFrameReady_();
}

std::unique_ptr<QOffscreenSurface> createGlSurfaceForRenderThread()
{
    if (!QOpenGLContext::supportsThreadedOpenGL())
        return nullptr;
    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(QSurfaceFormat::defaultFormat());
    surface->create();
    if (!surface->isValid())
        return nullptr;
    return surface;
}

bool isVideoOutputReleased(const mpv_event& event)
{
    if (event.event_id != MPV_EVENT_PROPERTY_CHANGE || event.reply_userdata != kVideoOutputObserverId)
        return false;
    const auto* property = static_cast<const mpv_event_property*>(event.data);
    return property->format == MPV_FORMAT_FLAG && *static_cast<const int*>(property->data) == 0;
}

const char* mpvMouseButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return "MBTN_LEFT";
    case Qt::RightButton:
        return "MBTN_RIGHT";
    case Qt::MiddleButton:
        return "MBTN_MID";
    default:
        return nullptr;
    }
}

class MpvView final : public QWidget {
public:
    MpvView(QWidget* parent, const Libmpv& api);
    ~MpvView() override;

    bool isReady() const { return renderer_ != nullptr; }
    bool load(const QByteArray& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    static void onMpvWakeup(void* context);

    QSize deviceSize() const;
    void onRendererReady(bool ready);
    void releaseVideoOutputWhileRendering();
    void waitUntilVideoOutputReleased();
    void scheduleRepaint();
    int command(std::initializer_list<const char*> args);
    void drainEvents();
    void rewindWhenPlaybackEnds(const mpv_event_property& property);
    bool hasRealVideo();
    void showAudioInfoUnlessVideo();
    void sendMousePosition(const QPointF& position);
    void sendMouseButton(Qt::MouseButton button, const char* action);

    const Libmpv& api_;
    mpv_handle* mpv_ = nullptr;
    bool rendererReady_ = false;
    QByteArray pendingPath_;
    std::unique_ptr<QOffscreenSurface> glSurface_;
    std::unique_ptr<FrameRenderer> renderer_;
};

MpvView::MpvView(QWidget* parent, const Libmpv& api) : QWidget(parent), api_(api)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);

    useCNumericLocaleRequiredByLibmpv();
    mpv_ = api_.create();
    if (!mpv_)
        return;
    for (const MpvOption& option : kMpvOptions)
        api_.set_option_string(mpv_, option.name, option.value);
    const int initStatus = api_.initialize(mpv_);
    if (initStatus < 0)
        return;

    api_.set_wakeup_callback(mpv_, &MpvView::onMpvWakeup, this);
    api_.observe_property(mpv_, kEofObserverId, kEofReachedProperty, MPV_FORMAT_FLAG);

    glSurface_ = createGlSurfaceForRenderThread();
    renderer_ = std::make_unique<FrameRenderer>(
        api_, mpv_, glSurface_.get(),
        [this](bool ready) { QMetaObject::invokeMethod(this, [this, ready] { onRendererReady(ready); }, Qt::QueuedConnection); },
        [this] { scheduleRepaint(); });
    renderer_->start();
}

MpvView::~MpvView()
{
    if (renderer_) {
        releaseVideoOutputWhileRendering();
        renderer_->stop();
    }
    if (mpv_)
        api_.terminate_destroy(mpv_);
}

void MpvView::onMpvWakeup(void* context)
{
    auto* view = static_cast<MpvView*>(context);
    QMetaObject::invokeMethod(view, [view] { view->drainEvents(); }, Qt::QueuedConnection);
}

void MpvView::releaseVideoOutputWhileRendering()
{
    api_.set_option_string(mpv_, "force-window", "no");
    api_.observe_property(mpv_, kVideoOutputObserverId, kVideoOutputConfiguredProperty, MPV_FORMAT_FLAG);
    command({"stop"});
    waitUntilVideoOutputReleased();
}

void MpvView::waitUntilVideoOutputReleased()
{
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        const qint64 remainingMs = kVideoOutputReleaseTimeoutMs - elapsed.elapsed();
        if (remainingMs <= 0)
            return;
        const mpv_event* event = api_.wait_event(mpv_, remainingMs / 1000.0);
        if (isVideoOutputReleased(*event))
            return;
    }
}

void MpvView::scheduleRepaint()
{
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

QSize MpvView::deviceSize() const
{
    return size() * devicePixelRatioF();
}

int MpvView::command(std::initializer_list<const char*> args)
{
    std::vector<const char*> argv(args);
    argv.push_back(nullptr);
    return api_.command(mpv_, argv.data());
}

void MpvView::onRendererReady(bool ready)
{
    if (!ready)
        return;
    rendererReady_ = true;
    for (const MpvOption& option : kMpvOptionsRequiringRenderContext)
        api_.set_option_string(mpv_, option.name, option.value);
    if (!pendingPath_.isEmpty())
        command({"loadfile", pendingPath_.constData()});
    pendingPath_.clear();
}

bool MpvView::load(const QByteArray& path)
{
    if (!rendererReady_) {
        pendingPath_ = path;
        return true;
    }
    const int status = command({"loadfile", path.constData()});
    return status >= 0;
}

void MpvView::drainEvents()
{
    for (;;) {
        const mpv_event* event = api_.wait_event(mpv_, 0);
        if (event->event_id == MPV_EVENT_NONE)
            return;
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            showAudioInfoUnlessVideo();
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE)
            rewindWhenPlaybackEnds(*static_cast<const mpv_event_property*>(event->data));
    }
}

void MpvView::rewindWhenPlaybackEnds(const mpv_event_property& property)
{
    const bool isEofFlag = property.format == MPV_FORMAT_FLAG && std::strcmp(property.name, kEofReachedProperty) == 0;
    if (!isEofFlag)
        return;
    const bool reachedEnd = *static_cast<const int*>(property.data) != 0;
    if (!reachedEnd)
        return;
    command({"set", "pause", "yes"});
    command({"seek", "0", "absolute"});
}

bool MpvView::hasRealVideo()
{
    int albumArt = 0;
    const int status = api_.get_property(mpv_, "current-tracks/video/albumart", MPV_FORMAT_FLAG, &albumArt);
    return status >= 0 && !albumArt;
}

void MpvView::showAudioInfoUnlessVideo()
{
    if (hasRealVideo()) {
        command({"show-text", ""});
        return;
    }
    command({"expand-properties", "show-text", kAudioInfoTemplate, kPersistentOsdDurationMs});
}

void MpvView::paintEvent(QPaintEvent*)
{
    const QSize targetSize = deviceSize();
    renderer_->requestSize(targetSize, devicePixelRatioF());
    const QImage frame = renderer_->latestFrame();
    QPainter painter(this);
    const bool frameCoversWidget = frame.size() == targetSize;
    if (!frameCoversWidget)
        painter.fillRect(rect(), Qt::black);
    if (!frame.isNull())
        painter.drawImage(QPoint(0, 0), frame);
}

void MpvView::sendMousePosition(const QPointF& position)
{
    const qreal ratio = devicePixelRatioF();
    const QByteArray x = QByteArray::number(qRound(position.x() * ratio));
    const QByteArray y = QByteArray::number(qRound(position.y() * ratio));
    command({"mouse", x.constData(), y.constData()});
}

void MpvView::sendMouseButton(Qt::MouseButton button, const char* action)
{
    const char* key = mpvMouseButton(button);
    if (key)
        command({action, key});
}

void MpvView::mouseMoveEvent(QMouseEvent* event)
{
    sendMousePosition(event->localPos());
}

void MpvView::mousePressEvent(QMouseEvent* event)
{
    sendMousePosition(event->localPos());
    sendMouseButton(event->button(), "keydown");
}

void MpvView::mouseReleaseEvent(QMouseEvent* event)
{
    sendMousePosition(event->localPos());
    sendMouseButton(event->button(), "keyup");
}

void MpvView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;
    sendMousePosition(event->position());
    command({"keypress", delta > 0 ? "WHEEL_UP" : "WHEEL_DOWN"});
}

}

WLX_EXPORT void* ListLoad(void* parentWin, char* fileToLoad, int)
{
    const Libmpv* api = libmpv();
    if (!api)
        return nullptr;
    auto* view = new MpvView(static_cast<QWidget*>(parentWin), *api);
    if (!view->isReady()) {
        delete view;
        return nullptr;
    }
    const bool loaded = view->load(fileToLoad);
    if (!loaded) {
        delete view;
        return nullptr;
    }
    view->show();
    return view;
}

WLX_EXPORT int ListLoadNext(void*, void* listWin, char* fileToLoad, int)
{
    auto* view = static_cast<MpvView*>(listWin);
    const bool loaded = view->load(fileToLoad);
    return loaded ? kListPluginOk : kListPluginError;
}

WLX_EXPORT void ListCloseWindow(void* listWin)
{
    delete static_cast<MpvView*>(listWin);
}

WLX_EXPORT void ListGetDetectString(char* detectString, int maxLength)
{
    std::snprintf(detectString, maxLength, "%s", kDetectString);
}
