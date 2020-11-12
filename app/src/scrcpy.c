#include "scrcpy.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <sys/time.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
// not needed here, but winsock2.h must never be included AFTER windows.h
# include <winsock2.h>
# include <windows.h>
#endif

#include "config.h"
#include "command.h"
#include "common.h"
#include "compat.h"
#include "controller.h"
#include "decoder.h"
#include "device.h"
#include "events.h"
#include "file_handler.h"
#include "fps_counter.h"
#include "input_manager.h"
#include "recorder.h"
#include "screen.h"
#include "server.h"
#include "stream.h"
#include "tiny_xpm.h"
#include "video_buffer.h"
#include "util/lock.h"
#include "util/log.h"
#include "util/net.h"

#define SOKOL_IMPL
#define SOKOL_GLCORE33
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_fetch.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#undef STB_IMAGE_IMPLEMENTATION

#include "scrcpy.glsl.h"

static struct server server = SERVER_INITIALIZER;
static struct screen screen = SCREEN_INITIALIZER;
static struct fps_counter fps_counter;
static struct video_buffer video_buffer;
static struct stream stream;
static struct decoder decoder;
static struct recorder recorder;
static struct controller controller;
static struct file_handler file_handler;

static struct input_manager input_manager = {
    .controller = &controller,
    .video_buffer = &video_buffer,
    .screen = &screen,
    .repeat = 0,

    // initialized later
    .prefer_text = false,
    .sdl_shortcut_mods = {
        .data = {0},
        .count = 0,
    },
};

#ifdef _WIN32
BOOL WINAPI windows_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT) {
        SDL_Event event;
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
        return TRUE;
    }
    return FALSE;
}
#endif // _WIN32

// init GLFW and set appropriate hints
static bool
glfw_init_and_configure(bool display, const char *render_driver,
                       bool disable_screensaver) {
    const char* description;

    if (!glfwInit()) {
        glfwGetError(&description);
        LOGC("Could not initialize GLFW: %s", description);
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    atexit(glfwTerminate);

#ifdef _WIN32
    // Clean up properly on Ctrl+C on Windows
    bool ok = SetConsoleCtrlHandler(windows_ctrl_handler, TRUE);
    if (!ok) {
        LOGW("Could not set Ctrl+C handler");
    }
#endif // _WIN32

    if (!display) {
        return true;
    }

    return true;
}


#if defined(__APPLE__) || defined(__WINDOWS__)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static int
event_watcher(void *data, SDL_Event *event) {
    (void) data;
    if (event->type == SDL_WINDOWEVENT
            && event->window.event == SDL_WINDOWEVENT_RESIZED) {
        // In practice, it seems to always be called from the same thread in
        // that specific case. Anyway, it's just a workaround.
        screen_render(&screen, true);
    }
    return 0;
}
#endif

static bool
is_apk(const char *file) {
    const char *ext = strrchr(file, '.');
    return ext && !strcmp(ext, ".apk");
}

enum event_result {
    EVENT_RESULT_CONTINUE,
    EVENT_RESULT_STOPPED_BY_USER,
    EVENT_RESULT_STOPPED_BY_EOS,
};

static enum event_result
handle_event(SDL_Event *event, const struct scrcpy_options *options) {
    switch (event->type) {
        case EVENT_STREAM_STOPPED:
            LOGD("Video stream stopped");
            return EVENT_RESULT_STOPPED_BY_EOS;
        case SDL_QUIT:
            LOGD("User requested to quit");
            return EVENT_RESULT_STOPPED_BY_USER;
        case EVENT_NEW_FRAME:
            LOGD("EVENT_NEW_FRAME");
            if (!screen.has_frame) {
                screen.has_frame = true;
                // this is the very first frame, show the window
                screen_show_window(&screen);
            }
            if (!screen_update_frame(&screen, &video_buffer)) {
                return EVENT_RESULT_CONTINUE;
            }
            break;
        case SDL_WINDOWEVENT:
            screen_handle_window_event(&screen, &event->window);
            break;
        case SDL_TEXTINPUT:
            if (!options->control) {
                break;
            }
            input_manager_process_text_input(&input_manager, &event->text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            // some key events do not interact with the device, so process the
            // event even if control is disabled
            input_manager_process_key(&input_manager, &event->key);
            break;
        case SDL_MOUSEMOTION:
            if (!options->control) {
                break;
            }
            input_manager_process_mouse_motion(&input_manager, &event->motion);
            break;
        case SDL_MOUSEWHEEL:
            if (!options->control) {
                break;
            }
            input_manager_process_mouse_wheel(&input_manager, &event->wheel);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            // some mouse events do not interact with the device, so process
            // the event even if control is disabled
            input_manager_process_mouse_button(&input_manager, &event->button);
            break;
        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
            input_manager_process_touch(&input_manager, &event->tfinger);
            break;
        case SDL_DROPFILE: {
            if (!options->control) {
                break;
            }
            file_handler_action_t action;
            if (is_apk(event->drop.file)) {
                action = ACTION_INSTALL_APK;
            } else {
                action = ACTION_PUSH_FILE;
            }
            file_handler_request(&file_handler, action, event->drop.file);
            break;
        }
    }
    return EVENT_RESULT_CONTINUE;
}

static bool
event_loop(const struct scrcpy_options *options) {
#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (options->display) {
        SDL_AddEventWatch(event_watcher, NULL);
    }
#endif
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        enum event_result result = handle_event(&event, options);
        switch (result) {
            case EVENT_RESULT_STOPPED_BY_USER:
                return true;
            case EVENT_RESULT_STOPPED_BY_EOS:
                LOGW("Device disconnected");
                return false;
            case EVENT_RESULT_CONTINUE:
                break;
        }
    }
    return false;
}

static SDL_LogPriority
sdl_priority_from_av_level(int level) {
    switch (level) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
            return SDL_LOG_PRIORITY_CRITICAL;
        case AV_LOG_ERROR:
            return SDL_LOG_PRIORITY_ERROR;
        case AV_LOG_WARNING:
            return SDL_LOG_PRIORITY_WARN;
        case AV_LOG_INFO:
            return SDL_LOG_PRIORITY_INFO;
    }
    // do not forward others, which are too verbose
    return 0;
}

static void
av_log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    (void) avcl;
    SDL_LogPriority priority = sdl_priority_from_av_level(level);
    if (priority == 0) {
        return;
    }
    char *local_fmt = SDL_malloc(strlen(fmt) + 10);
    if (!local_fmt) {
        LOGC("Could not allocate string");
        return;
    }
    // strcpy is safe here, the destination is large enough
    strcpy(local_fmt, "[FFmpeg] ");
    strcpy(local_fmt + 9, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_VIDEO, priority, local_fmt, vl);
    SDL_free(local_fmt);
}

/* application state */
static struct {
    sg_pipeline pip;
    sg_bindings bind;
    sg_pass_action pass_action;
    uint8_t file_buffer[512 * 1024];
} state;

/* The fetch-callback is called by sokol_fetch.h when the data is loaded,
   or when an error has occurred.
*/
static void fetch_callback(const sfetch_response_t* response) {
    LOGW("fetch_callback");
    if (response->fetched) {
        LOGW("response->fetched");
        /* the file data has been fetched, since we provided a big-enough
           buffer we can be sure that all data has been loaded here
        */
        int img_width, img_height, num_channels;
        const int desired_channels = 4;
        stbi_uc* pixels = stbi_load_from_memory(
            response->buffer_ptr,
            (int)response->fetched_size,
            &img_width, &img_height,
            &num_channels, desired_channels);
        if (pixels) {
            /* initialize the sokol-gfx texture */
            sg_init_image(state.bind.fs_images[SLOT_ourTexture], &(sg_image_desc){
                .width = img_width,
                .height = img_height,
                /* set pixel_format to RGBA8 for WebGL */
                .pixel_format = SG_PIXELFORMAT_RGBA8,
                .wrap_u = SG_WRAP_REPEAT,
                .wrap_v = SG_WRAP_REPEAT,
                .min_filter = SG_FILTER_LINEAR,
                .mag_filter = SG_FILTER_LINEAR,
                .content.subimage[0][0] = {
                    .ptr = pixels,
                    .size = img_width * img_height * 4,
                }
            });
            stbi_image_free(pixels);
        }
    }
    else if (response->failed) {
        // if loading the file failed, set clear color to red
        state.pass_action = (sg_pass_action) {
            .colors[0] = { .action = SG_ACTION_CLEAR, .val = { 1.0f, 0.0f, 0.0f, 1.0f } }
        };
    }
}

bool
scrcpy(const struct scrcpy_options *options) {
    bool record = !!options->record_filename;
    struct server_params params = {
        .log_level = options->log_level,
        .crop = options->crop,
        .port_range = options->port_range,
        .max_size = options->max_size,
        .bit_rate = options->bit_rate,
        .max_fps = options->max_fps,
        .lock_video_orientation = options->lock_video_orientation,
        .control = options->control,
        .display_id = options->display_id,
        .show_touches = options->show_touches,
        .stay_awake = options->stay_awake,
        .codec_options = options->codec_options,
        .force_adb_forward = options->force_adb_forward,
    };
    if (!server_start(&server, options->serial, &params)) {
        return false;
    }

    bool ret = false;

    bool fps_counter_initialized = false;
    bool video_buffer_initialized = false;
    bool file_handler_initialized = false;
    bool recorder_initialized = false;
    bool stream_started = false;
    bool controller_initialized = false;
    bool controller_started = false;

    if (!glfw_init_and_configure(options->display, options->render_driver,
                                options->disable_screensaver)) {
        goto end;
    }

    if (!server_connect_to(&server)) {
        goto end;
    }

    char device_name[DEVICE_NAME_FIELD_LENGTH];
    struct size frame_size;

    // screenrecord does not send frames when the screen content does not
    // change therefore, we transmit the screen size before the video stream,
    // to be able to init the window immediately
    if (!device_read_info(server.video_socket, device_name, &frame_size)) {
        goto end;
    }

    struct decoder *dec = NULL;
    if (options->display) {
        if (!fps_counter_init(&fps_counter)) {
            goto end;
        }
        fps_counter_initialized = true;

        if (!video_buffer_init(&video_buffer, &fps_counter,
                               options->render_expired_frames)) {
            goto end;
        }
        video_buffer_initialized = true;

        if (options->control) {
            if (!file_handler_init(&file_handler, server.serial,
                                   options->push_target)) {
                goto end;
            }
            file_handler_initialized = true;
        }

        decoder_init(&decoder, &video_buffer);
        dec = &decoder;
    }

    struct recorder *rec = NULL;
    if (record) {
        if (!recorder_init(&recorder,
                           options->record_filename,
                           options->record_format,
                           frame_size)) {
            goto end;
        }
        rec = &recorder;
        recorder_initialized = true;
    }

    av_log_set_callback(av_log_callback);

    stream_init(&stream, server.video_socket, dec, rec);

    // now we consumed the header values, the socket receives the video stream
    // start the stream
    if (!stream_start(&stream)) {
        goto end;
    }
    stream_started = true;

    if (options->display) {
        if (options->control) {
            if (!controller_init(&controller, server.control_socket)) {
                goto end;
            }
            controller_initialized = true;

            if (!controller_start(&controller)) {
                goto end;
            }
            controller_started = true;
        }

        const char *window_title =
            options->window_title ? options->window_title : device_name;

        if (!screen_init_rendering(&screen, window_title, frame_size,
                                   options->always_on_top, options->window_x,
                                   options->window_y, options->window_width,
                                   options->window_height,
                                   options->window_borderless,
                                   options->rotation, options->mipmaps)) {
            goto end;
        }

        if (options->turn_screen_off) {
            struct control_msg msg;
            msg.type = CONTROL_MSG_TYPE_SET_SCREEN_POWER_MODE;
            msg.set_screen_power_mode.mode = SCREEN_POWER_MODE_OFF;

            if (!controller_push_msg(&controller, &msg)) {
                LOGW("Could not request 'set screen power mode'");
            }
        }

        if (options->fullscreen) {
            screen_switch_fullscreen(&screen);
        }
    }

    /* setup sokol_gfx */
    sg_setup(&(sg_desc){0});

     /* setup sokol-fetch */
    sfetch_setup(&(sfetch_desc_t){
        .max_requests = 1,
        .num_channels = 1,
        .num_lanes = 1
    });

    /* Allocate an image handle, but don't actually initialize the image yet,
       this happens later when the asynchronous file load has finished.
       Any draw calls containing such an "incomplete" image handle
       will be silently dropped.
    */
    state.bind.fs_images[SLOT_ourTexture] = sg_alloc_image();

    float vertices[] = {
        // positions         // colors           // texture coords
        0.5f,  0.5f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 1.0f,   // top right
        0.5f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f,   1.0f, 0.0f,   // bottom right
        -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, 1.0f,   0.0f, 0.0f,   // bottom left
        -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 0.0f,   0.0f, 1.0f    // top left
    };
    state.bind.vertex_buffers[0] = sg_make_buffer(&(sg_buffer_desc){
        .size = sizeof(vertices),
        .content = vertices,
        .label = "quad-vertices"
    });

    /* an index buffer with 2 triangles */
    uint16_t indices[] = {
            0, 1, 3,   // first triangle
            1, 2, 3    // second triangle
    };
    state.bind.index_buffer = sg_make_buffer(&(sg_buffer_desc){
        .type = SG_BUFFERTYPE_INDEXBUFFER,
        .size = sizeof(indices),
        .content = indices,
        .label = "quad-indices"
    });

    /* create shader from code-generated sg_shader_desc */
    sg_shader shd = sg_make_shader(simple_shader_desc());

    /* create a pipeline object (default render states are fine for triangle) */
    state.pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        /* if the vertex layout doesn't have gaps, don't need to provide strides and offsets */
        .layout = {
            .attrs = {
                [ATTR_vs_position].format = SG_VERTEXFORMAT_FLOAT3,
                [ATTR_vs_aColor].format = SG_VERTEXFORMAT_FLOAT3,
                [ATTR_vs_aTexCoord].format = SG_VERTEXFORMAT_FLOAT2
            }
        },
        .label = "triangle-pipeline"
    });

    /* a pass action to clear framebuffer */
    state.pass_action = (sg_pass_action) {
        .colors[0] = { .action=SG_ACTION_CLEAR, .val={0.2f, 0.3f, 0.3f, 1.0f} }
    };

    /* start loading the PNG file */
    sfetch_send(&(sfetch_request_t){
        .path = "container.jpg",
        .callback = fetch_callback,
        .buffer_ptr = state.file_buffer,
        .buffer_size = sizeof(state.file_buffer)
    });

    /* draw loop */
    while (!glfwWindowShouldClose(screen.window)) {
        int cur_width, cur_height;
        glfwGetFramebufferSize(screen.window, &cur_width, &cur_height);

        sfetch_dowork();
        sg_begin_default_pass(&state.pass_action, cur_width, cur_height);
        sg_apply_pipeline(state.pip);
        sg_apply_bindings(&state.bind);
        sg_draw(0, 6, 1);
        sg_end_pass();
        sg_commit();

        glfwSwapBuffers(screen.window);
        glfwPollEvents();
    }

    /* cleanup */
    sg_shutdown();
    sfetch_shutdown();

    // glfw: terminate, clearing all previously allocated GLFW resources.
    // ------------------------------------------------------------------
    glfwTerminate();

    screen_destroy(&screen);

end:
    // stop stream and controller so that they don't continue once their socket
    // is shutdown
    if (stream_started) {
        stream_stop(&stream);
    }
    if (controller_started) {
        controller_stop(&controller);
    }
    if (file_handler_initialized) {
        file_handler_stop(&file_handler);
    }
    if (fps_counter_initialized) {
        fps_counter_interrupt(&fps_counter);
    }

    // shutdown the sockets and kill the server
    server_stop(&server);

    // now that the sockets are shutdown, the stream and controller are
    // interrupted, we can join them
    if (stream_started) {
        stream_join(&stream);
    }
    if (controller_started) {
        controller_join(&controller);
    }
    if (controller_initialized) {
        controller_destroy(&controller);
    }

    if (recorder_initialized) {
        recorder_destroy(&recorder);
    }

    if (file_handler_initialized) {
        file_handler_join(&file_handler);
        file_handler_destroy(&file_handler);
    }

    if (video_buffer_initialized) {
        video_buffer_destroy(&video_buffer);
    }

    if (fps_counter_initialized) {
        fps_counter_join(&fps_counter);
        fps_counter_destroy(&fps_counter);
    }

    server_destroy(&server);

    return ret;
}
