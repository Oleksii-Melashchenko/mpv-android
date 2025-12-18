#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>
#include <atomic>

#include <mpv/client.h>

#include <pthread.h>

extern "C" {
    #include <libavcodec/jni.h>
}

#include "log.h"
#include "jni_utils.h"
#include "event.h"

#define ARRAYLEN(a) (sizeof(a)/sizeof(a[0]))

extern "C" {
    jni_func(void, create, jobject appctx);
    jni_func(void, init);
    jni_func(void, destroy);

    jni_func(void, command, jobjectArray jarray);
};

JavaVM *g_vm;
mpv_handle *g_mpv;
std::atomic<bool> g_event_thread_request_exit(false);

static pthread_t event_thread_id;

static void prepare_environment(JNIEnv *env, jobject appctx) {
    setlocale(LC_NUMERIC, "C");

    if (!env->GetJavaVM(&g_vm) && g_vm)
        av_jni_set_java_vm(g_vm, NULL);

    jobject global_appctx = env->NewGlobalRef(appctx);
    if (global_appctx)
        av_jni_set_android_app_ctx(global_appctx, NULL);

    init_methods_cache(env);
}

jni_func(void, create, jobject appctx) {
    prepare_environment(env, appctx);

    if (g_mpv)
        die("mpv is already initialized");

    g_mpv = mpv_create();
    if (!g_mpv)
        die("context init failed");
    typedef struct {
        const char* name;
        const char* shaders[5];
        int shader_count;
        const char* scale;
        const char* cscale;
        const char* dscale;
        const char* deband;
        const char* hwdec;
    } mpv_profile_t;
    mpv_profile_t profiles[] = {
            { // слабый
                    "light",
                    { "Anime4K_Clamp_Highlights.glsl" },
                    1,
                    "bilinear",
                    "bilinear",
                    "bilinear",
                    "no",
                    "auto-safe"
            },
            { // нормальный
                    "normal",
                    {
                      "Anime4K_Clamp_Highlights.glsl",
                            "Anime4K_Darken_Fast.glsl",
                            "Anime4K_Thin_Fast.glsl",
                            "Anime4K_Upscale_Original_x2.glsl"
                    },
                    4,
                    "ewa_lanczos",
                    "ewa_lanczos",
                    "mitchell",
                    "yes",
                    "auto-safe"
            },
            { // мощный
                    "high",
                    {
                      "Anime4K_Clamp_Highlights.glsl",
                            "Anime4K_Darken_HQ.glsl",
                            "Anime4K_Thin_HQ.glsl",
                            "Anime4K_Upscale_DoG_x2.glsl"
                    },
                    4,
                    "ewa_lanczossharp",
                    "ewa_lanczossharp",
                    "mitchell",
                    "yes",
                    "auto"
            }
    };
    void apply_profile(int profile_index) {
        mpv_profile_t* p = &profiles[profile_index];

        mpv_set_option_string(g_mpv, "hwdec", p->hwdec);
        mpv_set_option_string(g_mpv, "scale", p->scale);
        mpv_set_option_string(g_mpv, "cscale", p->cscale);
        mpv_set_option_string(g_mpv, "dscale", p->dscale);
        mpv_set_option_string(g_mpv, "deband", p->deband);

        mpv_command_string(g_mpv, "glsl-shaders-clear");

        for (int i = 0; i < p->shader_count; i++) {
            mpv_set_option_string(g_mpv, "glsl-shaders-append", p->shaders[i]);
        }
    }
    mpv_set_option_string(g_mpv, "vo", "gpu");
    mpv_set_option_string(g_mpv, "gpu-api", "opengl");
    mpv_set_option_string(g_mpv, "gpu-context", "android");

    mpv_set_option_string(g_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(g_mpv, "hwdec-codecs", "all");

    mpv_set_option_string(g_mpv, "profile", "gpu-hq");
    mpv_set_option_string(g_mpv, "scale", "ewa_lanczossharp");
    mpv_set_option_string(g_mpv, "cscale", "ewa_lanczossharp");
    mpv_set_option_string(g_mpv, "dscale", "mitchell");

    mpv_set_option_string(g_mpv, "sigmoid-upscaling", "yes");
    mpv_set_option_string(g_mpv, "correct-downscaling", "yes");
    mpv_set_option_string(g_mpv, "linear-downscaling", "no");

    mpv_set_option_string(g_mpv, "deband", "yes");
    mpv_set_option_string(g_mpv, "deband-iterations", "2");
    mpv_set_option_string(g_mpv, "deband-threshold", "35");
    mpv_set_option_string(g_mpv, "deband-range", "16");
    mpv_set_option_string(g_mpv, "deband-grain", "8");

    mpv_set_option_string(g_mpv, "glsl-shaders-append",
                          "/storage/emulated/0/Android/media/is.xyz.mpv/shaders/Anime4K_Clamp_Highlights.glsl");

    mpv_set_option_string(g_mpv, "glsl-shaders-append",
                          "/storage/emulated/0/Android/media/is.xyz.mpv/shaders/Anime4K_Darken_Fast.glsl");

    mpv_set_option_string(g_mpv, "glsl-shaders-append",
                          "/storage/emulated/0/Android/media/is.xyz.mpv/shaders/Anime4K_Thin_Fast.glsl");
    mpv_set_option_string(g_mpv, "glsl-shaders-append",
                          "/storage/emulated/0/Android/media/is.xyz.mpv/shaders/Anime4K_Upscale_Original_x2.glsl");

    // use terminal log level but request verbose messages
    // this way --msg-level can be used to adjust later
    mpv_request_log_messages(g_mpv, "terminal-default");
    mpv_set_option_string(g_mpv, "msg-level", "all=v");
}

jni_func(void, init) {
    if (!g_mpv)
        die("mpv is not created");
    mpv_set_option_string(g_mpv, "config", "no");
    if (mpv_initialize(g_mpv) < 0)
        die("mpv init failed");

    g_event_thread_request_exit = false;
    if (pthread_create(&event_thread_id, NULL, event_thread, NULL) != 0)
        die("thread create failed");
    pthread_setname_np(event_thread_id, "event_thread");
}

jni_func(void, destroy) {
    if (!g_mpv) {
        ALOGV("mpv destroy called but it's already destroyed");
        return;
    }

    // poke event thread and wait for it to exit
    g_event_thread_request_exit = true;
    mpv_wakeup(g_mpv);
    pthread_join(event_thread_id, NULL);

    mpv_terminate_destroy(g_mpv);
    g_mpv = NULL;
}

jni_func(void, command, jobjectArray jarray) {
    CHECK_MPV_INIT();

    const char *arguments[128] = {0};
    int len = env->GetArrayLength(jarray);
    if (len >= ARRAYLEN(arguments))
        die("too many command arguments");

    for (int i = 0; i < len; ++i)
        arguments[i] = env->GetStringUTFChars((jstring)env->GetObjectArrayElement(jarray, i), NULL);

    mpv_command(g_mpv, arguments);

    for (int i = 0; i < len; ++i)
        env->ReleaseStringUTFChars((jstring)env->GetObjectArrayElement(jarray, i), arguments[i]);
}
