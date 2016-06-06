#include "PulseOut.h"
#include <iostream>

#define BUFFER_COUNT 8

using namespace musik::core::audio;

class MainLoopLock {
    public:
        MainLoopLock(pa_threaded_mainloop* mainLoop) {
            this->mainLoop = mainLoop;
            pa_threaded_mainloop_lock(mainLoop);
        }

        ~MainLoopLock() {
            pa_threaded_mainloop_unlock(this->mainLoop);
        }

    private:
        pa_threaded_mainloop* mainLoop;
};

static bool waitForCompletion(pa_operation *op, pa_threaded_mainloop *loop) {
    if (op) {
        pa_operation_state_t state;
        while ((state == pa_operation_get_state(op)) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(loop);
        }

        pa_operation_unref(op);
        return (state == PA_OPERATION_DONE);
    }

    return false;
}

size_t PulseOut::CountBuffersWithProvider(IBufferProvider* provider) {
    boost::recursive_mutex::scoped_lock bufferLock(this->mutex);

    size_t count = 0;
    auto it = this->buffers.begin();
    while (it != this->buffers.end()) {
        if ((*it)->provider == provider) {
            ++count;
        }
        ++it;
    }
    return count;
}

bool PulseOut::RemoveBufferFromQueue(BufferContext* context) {
    boost::recursive_mutex::scoped_lock bufferLock(this->mutex);

    auto it = this->buffers.begin();
    while (it != this->buffers.end()) {
        if ((*it).get() == context) {
            this->buffers.erase(it);
            return true;
        }
        ++it;
    }

    return false;
}

void PulseOut::NotifyBufferCompleted(BufferContext* context) {
    IBufferProvider* provider = context->provider;
    IBuffer* buffer = context->buffer;

    if (this->RemoveBufferFromQueue(context)) {
        provider->OnBufferProcessed(buffer);
    }
}

PulseOut::PulseOut() {
    this->volume = 1.0f;
    this->pulseMainLoop = 0;;
    this->pulseContext = 0;
    this->pulseStream = 0;
    this->pulseStreamFormat.format = PA_SAMPLE_FLOAT32LE;
    this->pulseStreamFormat.rate = 0;
    this->pulseStreamFormat.channels = 0;

    boost::thread th(boost::bind(&PulseOut::ThreadProc,this));
    th.detach();

    this->InitPulse();
}

bool PulseOut::Play(IBuffer *buffer, IBufferProvider *provider) {
    if (!this->pulseStream ||
        this->pulseStreamFormat.rate != buffer->SampleRate() ||
        this->pulseStreamFormat.channels != buffer->Channels())
    {
        if (this->pulseStream) {
            std::cerr << "fixme: stream switched formats; not handled\n";
            return false;
        }

        this->DeinitPulseStream();
        if (!this->InitPulseStream(buffer->SampleRate(), buffer->Channels())) {
            std::cerr << "could not initialize stream for playback\n";
            return false;
        }
    }

    if (this->CountBuffersWithProvider(provider) >= BUFFER_COUNT) {
        std::cerr << "full!\n";
        return false;
    }

    std::shared_ptr<BufferContext> context(new BufferContext());
    context->output = this;
    context->buffer = buffer;
    context->provider = provider;

    {
        boost::recursive_mutex::scoped_lock bufferLock(this->mutex);
        this->buffers.push_back(context);
    }

    MainLoopLock loopLock(this->pulseMainLoop);

    //std::cerr << buffer->Bytes() << std::endl;

    int error =
        pa_stream_write_ext_free(
            this->pulseStream,
            static_cast<void*>(buffer->BufferPointer()),
            buffer->Bytes(),
            &PulseOut::OnPulseBufferPlayed,
            static_cast<void*>(context.get()),
            0,
            PA_SEEK_RELATIVE);

    if (error) {
        std::cerr << "FAILED!! " << error << std::endl;
        this->NotifyBufferCompleted(context.get());
    }

    // std::cerr << "wrote " << (error ? "unsuccessfully" : "successfully") << std::endl;

    return !error;
}

PulseOut::~PulseOut() {
    this->Stop();
    this->DeinitPulse();
}

void PulseOut::ThreadProc() {
    while (true) {
        pa_usec_t usec;
        if (this->pulseStream) {
            pa_stream_get_time(this->pulseStream, &usec);
            std::cerr << "time: " << usec << std::endl;
        }
        usleep(1000 * 1000);
    }
}

void PulseOut::Destroy() {
    delete this;
}

void PulseOut::Pause() {
    this->SetPaused(true);
}

void PulseOut::Resume() {
    this->SetPaused(false);
}

void PulseOut::SetPaused(bool paused) {
    if (this->pulseStream) {
        std::cerr << "resuming... ";
        MainLoopLock loopLock(this->pulseMainLoop);
        waitForCompletion(
            pa_stream_cork(
                this->pulseStream,
                paused ? 1 : 0,
                &PulseOut::OnPulseStreamSuccessCallback,
                this),
            this->pulseMainLoop);
        std::cerr << "resumed";
    }
}

void PulseOut::SetVolume(double volume) {
}

void PulseOut::Stop() {
}

void PulseOut::OnPulseBufferPlayed(void *data) {
    BufferContext* context = static_cast<BufferContext*>(data);
    context->output->NotifyBufferCompleted(context);
}

void PulseOut::OnPulseContextStateChanged(pa_context *context, void *data) {
    PulseOut* out = static_cast<PulseOut*>(data);
    const pa_context_state_t state = pa_context_get_state(context);

    std::cerr << "context connection state changed: " << state << std::endl;

    switch (state) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            pa_threaded_mainloop_signal(out->pulseMainLoop, 0);
        default:
            return;
    }
}

void PulseOut::OnPulseStreamStateChanged(pa_stream *stream, void *data) {
    PulseOut* out = static_cast<PulseOut*>(data);
    const pa_stream_state_t state = pa_stream_get_state(stream);

    std::cerr << "stream connection state changed: " << state << std::endl;

    switch (state) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(out->pulseMainLoop, 0);
        default:
            return;
    }
}

void PulseOut::OnPulseStreamSuccessCallback(pa_stream *s, int success, void *data) {
    PulseOut* out = static_cast<PulseOut*>(data);
    pa_threaded_mainloop_signal(out->pulseMainLoop, 0);
}


void PulseOut::InitPulse() {
    if (!this->InitPulseEventLoopAndContext()) {
        this->DeinitPulse();
    }
}

bool PulseOut::InitPulseEventLoopAndContext() {
    std::cerr << "init...\n";
    this->pulseMainLoop = pa_threaded_mainloop_new();
    if (this->pulseMainLoop) {
        std::cerr << "init ok, starting...\n";

        int error = pa_threaded_mainloop_start(this->pulseMainLoop);
        if (error) {
            pa_threaded_mainloop_free(this->pulseMainLoop);
            this->pulseMainLoop = NULL;
            return false;
        }

        std::cerr << "started ok.\n";
    }

    pa_mainloop_api* api = pa_threaded_mainloop_get_api(this->pulseMainLoop);

    MainLoopLock loopLock(this->pulseMainLoop);

    this->pulseContext = pa_context_new(api, "musikcube");

    if (this->pulseContext) {
        std::cerr << "context created";

        pa_context_set_state_callback(
            this->pulseContext,
            &PulseOut::OnPulseContextStateChanged,
            this);

        int error =
            pa_context_connect(
                this->pulseContext,
                NULL,
                PA_CONTEXT_NOFAIL,
                NULL);

        bool connected = false;
        while (!error && !connected) {
            pa_context_state_t state =
                pa_context_get_state(this->pulseContext);

            if (state == PA_CONTEXT_READY) {
                std::cerr << "connected!\n";
                connected = true;
            }
            else if (!PA_CONTEXT_IS_GOOD(state)) {
                std::cerr << "corrupted state! bailing.\n";
                error = true;
            }
            else {
                std::cerr << "waiting for connection...\n";
                pa_threaded_mainloop_wait(this->pulseMainLoop);
            }
        }

        if (connected && !error) {
            return true;
        }
    }

    return false;
}

bool PulseOut::InitPulseStream(size_t rate, size_t channels) {
    MainLoopLock loopLock(this->pulseMainLoop);

    this->pulseStreamFormat.rate = rate;
    this->pulseStreamFormat.channels = channels;

    this->pulseStream = pa_stream_new(
        this->pulseContext,
        "musikcube PulseOut stream",
        &this->pulseStreamFormat,
        NULL); /* channel mapping */

    std::cerr << "creating stream...\n";

    if (this->pulseStream) {
        std::cerr << "stream created.\n";

        pa_stream_set_state_callback(
            this->pulseStream,
            &PulseOut::OnPulseStreamStateChanged,
            this);

        std::cerr << "connecting the stream for playing...\n";

        int error = pa_stream_connect_playback(
            this->pulseStream,
            NULL, /* device id */
            NULL, /* buffering attributes */
            PA_STREAM_NOFLAGS, /* additional flags */
            NULL, /* initial volume. docs suggest NULL. */
            NULL); /* stream to synchronize with. */

        if (!error) {
            std::cerr << "connected. waiting for the stream to become ready\n";

            pa_threaded_mainloop_wait(this->pulseMainLoop);
            bool ready = pa_stream_get_state(this->pulseStream) == PA_STREAM_READY;

            std::cerr << (ready ? "stream is ready!" : "stream failed") << std::endl;

            if (ready) {
                this->Resume();
            }

            return ready;
        }
    }

    return false;
}

void PulseOut::DeinitPulseStream() {
    if (this->pulseStream) {
        std::cerr << "freeing stream...\n";
        MainLoopLock loopLock(this->pulseMainLoop);
        pa_stream_disconnect(this->pulseStream);
        pa_stream_unref(this->pulseStream);
        this->pulseStream = NULL;
    }
}

void PulseOut::DeinitPulse() {
    this->DeinitPulseStream();

    if (this->pulseContext) {
        std::cerr << "freeing context...\n";
        MainLoopLock loopLock(this->pulseMainLoop);
        pa_context_disconnect(this->pulseContext);
        pa_context_unref(this->pulseContext);
        this->pulseContext = NULL;
    }

    if (this->pulseMainLoop) {
        std::cerr << "stopping...\n";
        pa_threaded_mainloop_stop(this->pulseMainLoop);
        pa_threaded_mainloop_free(this->pulseMainLoop);
        this->pulseMainLoop = NULL;
    }
}
