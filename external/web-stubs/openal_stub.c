/* Web (Emscripten) build: a no-op OpenAL. See CMakeLists.txt in this directory.
 *
 * Only the functions the engine calls are defined. No device opens, so there is never a context, and (like OpenAL with
 * no context) every object function reports AL_INVALID_OPERATION through alGetError and does nothing else. No extension
 * is present (alcIsExtensionPresent is false), so the engine's loopback device cannot open either and the audio system
 * stays disabled.
 */
#include <AL/al.h>
#include <AL/alc.h>

#include <stddef.h>

static ALenum g_error = AL_NO_ERROR;

static void SetNoContextError(void) { g_error = AL_INVALID_OPERATION; }

ALCdevice *alcOpenDevice(const ALCchar *devicename) { (void)devicename; return NULL; }
ALCboolean alcCloseDevice(ALCdevice *device) { (void)device; return ALC_FALSE; }
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist) { (void)device; (void)attrlist; return NULL; }
void alcDestroyContext(ALCcontext *context) { (void)context; }
ALCboolean alcMakeContextCurrent(ALCcontext *context) { (void)context; return ALC_FALSE; }
ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname)
{
    (void)device; (void)extname;
    return ALC_FALSE;
}
ALCvoid *alcGetProcAddress(ALCdevice *device, const ALCchar *funcname)
{
    (void)device; (void)funcname;
    return NULL;
}

ALenum alGetError(void)
{
    const ALenum error = g_error;
    g_error = AL_NO_ERROR;
    return error;
}

void alGenBuffers(ALsizei n, ALuint *buffers) { (void)n; (void)buffers; SetNoContextError(); }
void alDeleteBuffers(ALsizei n, const ALuint *buffers) { (void)n; (void)buffers; }
void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq)
{
    (void)buffer; (void)format; (void)data; (void)size; (void)freq;
    SetNoContextError();
}

void alGenSources(ALsizei n, ALuint *sources) { (void)n; (void)sources; SetNoContextError(); }
void alDeleteSources(ALsizei n, const ALuint *sources) { (void)n; (void)sources; }
void alSourcePlay(ALuint source) { (void)source; SetNoContextError(); }
void alSourcePause(ALuint source) { (void)source; SetNoContextError(); }
void alSourceStop(ALuint source) { (void)source; SetNoContextError(); }
void alSourcef(ALuint source, ALenum param, ALfloat value) { (void)source; (void)param; (void)value; SetNoContextError(); }
void alSourcei(ALuint source, ALenum param, ALint value) { (void)source; (void)param; (void)value; SetNoContextError(); }
void alGetSourcei(ALuint source, ALenum param, ALint *value)
{
    (void)source; (void)param;
    if (value) *value = 0;
    SetNoContextError();
}

void alListenerf(ALenum param, ALfloat value) { (void)param; (void)value; SetNoContextError(); }
void alListenerfv(ALenum param, const ALfloat *values) { (void)param; (void)values; SetNoContextError(); }
