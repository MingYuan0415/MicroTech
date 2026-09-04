/** @file MicroTech LVGL simulator entry.
 *
 * M2 scope: boot the sim runtime (real app_manager + apps + real LVGL 9.5),
 * pump SDL events on the main thread only (dummy driver under --headless),
 * and keep the process alive for interactive sessions or a bounded frame
 * count (--frames) for scripted checks. CI step gating becomes effective
 * after boot (mailbox handshake completes inside app_manager_init).
 */
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "app_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sim_agent.h"
#include "sim_quit.h"

#include "sim_bsp.h"
#include "sim_fs.h"
#include "sim_http.h"
#include "sim_nvs.h"
#include "sim_sd_host.h"
#include "sim_lv_adapter.h"
#include "sim_mmap_assets.h"
#include "sim_png.h"
#include "sim_runtime.h"
#include "sim_time.h"

typedef struct sim_options
{
    bool headless;
    bool ci;
    const char *res_dir;
    const char *nvs_dir;
    const char *sd_dir;
    const char *out_dir;
    const char *navigate;
    int brightness;
    const char *swipe;
    const char *key[8];
    int key_count;
    int ci_steps;
    int agent_port;
    int window_scale;
    int frames;
} sim_options_t;

_Atomic int sim_quit_flag;

static void _on_signal(int sig)
{
    (void)sig;
    atomic_store(&sim_quit_flag, 1);
}

static void _script_advance(bool ci_mode, int steps)
{
    for (int i = 0; i < steps; i++)
    {
        if (ci_mode)
        {
            if (sim_lv_ci_step(33U) != 0)
            {
                return;
            }
        }
        else
        {
            usleep(16000U);
        }
    }
}

static void _usage(const char *argv0)
{
    printf("usage: %s [--headless] [--ci] [--res-dir DIR] [--nvs-dir DIR]\n"
           "          [--sd-dir DIR]\n"
           "          [--out-dir DIR] [--agent-port N] [--window-scale N]\n"
           "          [--frames N] [--screenshot NAME]\n", argv0);
}

static bool _parse_nonnegative_int(const char *raw, int *value_out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(raw, &end, 10);
    if ((errno != 0) || (end == raw) || (*end != '\0') || (value < 0) ||
            (value > INT_MAX))
    {
        return false;
    }
    *value_out = (int)value;
    return true;
}

static bool _parse_args(int argc, char **argv, sim_options_t *opt,
                        const char **screenshot)
{
    opt->res_dir = "sim_res_fs";
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--headless") == 0)
        {
            opt->headless = true;
        }
        else if (strcmp(argv[i], "--ci") == 0)
        {
            opt->ci = true;
        }
        else if ((strcmp(argv[i], "--res-dir") == 0) && ((i + 1) < argc))
        {
            opt->res_dir = argv[++i];
        }
        else if ((strcmp(argv[i], "--nvs-dir") == 0) && ((i + 1) < argc))
        {
            opt->nvs_dir = argv[++i];
        }
        else if ((strcmp(argv[i], "--sd-dir") == 0) && ((i + 1) < argc))
        {
            opt->sd_dir = argv[++i];
        }
        else if ((strcmp(argv[i], "--out-dir") == 0) && ((i + 1) < argc))
        {
            opt->out_dir = argv[++i];
        }
        else if ((strcmp(argv[i], "--ci-step") == 0) && ((i + 1) < argc))
        {
            opt->ci_steps = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "--swipe") == 0) && ((i + 1) < argc))
        {
            opt->swipe = argv[++i];
        }
        else if ((strcmp(argv[i], "--key") == 0) && ((i + 1) < argc))
        {
            if (opt->key_count < (int)(sizeof(opt->key) / sizeof(opt->key[0])))
            {
                opt->key[opt->key_count++] = argv[++i];
            }
        }
        else if ((strcmp(argv[i], "--brightness") == 0) && ((i + 1) < argc))
        {
            opt->brightness = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "--navigate") == 0) && ((i + 1) < argc))
        {
            opt->navigate = argv[++i];
        }
        else if ((strcmp(argv[i], "--agent-port") == 0) && ((i + 1) < argc))
        {
            opt->agent_port = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "--window-scale") == 0) && ((i + 1) < argc))
        {
            opt->window_scale = atoi(argv[++i]);
        }
        else if ((strcmp(argv[i], "--frames") == 0) && ((i + 1) < argc))
        {
            if (!_parse_nonnegative_int(argv[++i], &opt->frames))
            {
                fprintf(stderr, "invalid --frames value: %s\n", argv[i]);
                return false;
            }
        }
        else if ((strcmp(argv[i], "--screenshot") == 0) && ((i + 1) < argc))
        {
            *screenshot = argv[++i];
        }
        else
        {
            _usage(argv[0]);
            return false;
        }
    }
    return true;
}

static bool _window_size_matches(const sim_options_t *opt, SDL_Window *window)
{
    int width;
    int height;

    if (window == NULL)
    {
        return true;
    }
    SDL_GetWindowSize(window, &width, &height);
    return (width == (int)SIM_BSP_H_RES * opt->window_scale) &&
           (height == (int)SIM_BSP_V_RES * opt->window_scale);
}

static void _restore_window_size(const sim_options_t *opt, SDL_Window *window)
{
    if ((window != NULL) && !_window_size_matches(opt, window))
    {
        SDL_SetWindowSize(window,
                          (int)SIM_BSP_H_RES * opt->window_scale,
                          (int)SIM_BSP_V_RES * opt->window_scale);
    }
}

static void _pump_sdl(const sim_options_t *opt, SDL_Window *window)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_WINDOWEVENT:
            if (window != NULL &&
                    (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                     event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
            {
                _restore_window_size(opt, window);
            }
            break;
        case SDL_QUIT:
            atomic_store(&sim_quit_flag, 1);
            break;
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            if (!_window_size_matches(opt, window))
            {
                _restore_window_size(opt, window);
                break;
            }
            const int scale = opt->window_scale;
            const int16_t x = (int16_t)(event.motion.x / scale);
            const int16_t y = (int16_t)(event.motion.y / scale);
            bool pressed;
            if (event.type == SDL_MOUSEBUTTONUP)
            {
                pressed = false;
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN)
            {
                pressed = event.button.button == SDL_BUTTON_LEFT;
            }
            else
            {
                pressed = (event.motion.state & SDL_BUTTON_LMASK) != 0U;
            }
            sim_lv_touch_update((x < 0) ? 0 : ((x >= (int16_t)SIM_BSP_H_RES)
                                               ? (int16_t)(SIM_BSP_H_RES - 1)
                                               : x),
                                (y < 0) ? 0 : ((y >= (int16_t)SIM_BSP_V_RES)
                                               ? (int16_t)(SIM_BSP_V_RES - 1)
                                               : y),
                                pressed);
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            const bool down = event.type == SDL_KEYDOWN;
            if (event.key.keysym.sym == SDLK_F1)
            {
                (void)sim_bsp_key(SIM_KEY_BOOT, down ? SIM_KEY_ACTION_PRESS
                                  : SIM_KEY_ACTION_RELEASE);
            }
            else if (event.key.keysym.sym == SDLK_F2)
            {
                (void)sim_bsp_key(SIM_KEY_POWER, down ? SIM_KEY_ACTION_PRESS
                                  : SIM_KEY_ACTION_RELEASE);
            }
            else if (down && (event.key.keysym.sym == SDLK_ESCAPE))
            {
                atomic_store(&sim_quit_flag, 1);
            }
            break;
        }
        default:
            break;
        }
    }
}

static bool _run_iteration(const sim_options_t *opt, SDL_Window *window,
                           SDL_Texture *texture, SDL_Renderer *renderer)
{
    if (opt->ci)
    {
        return sim_lv_ci_step(33U) == 0;
    }
    if (!opt->headless)
    {
        _restore_window_size(opt, window);
        _pump_sdl(opt, window);
        _restore_window_size(opt, window);
        if (texture != NULL)
        {
            int update_result;
            if (esp_lv_adapter_lock(-1) != ESP_OK)
            {
                return false;
            }
            update_result = SDL_UpdateTexture(
                                texture, NULL, sim_bsp_framebuffer(),
                                (int)(SIM_BSP_H_RES * sizeof(uint16_t)));
            esp_lv_adapter_unlock();
            if (update_result != 0)
            {
                fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
                return false;
            }
            if (SDL_RenderClear(renderer) != 0)
            {
                fprintf(stderr, "SDL_RenderClear failed: %s\n", SDL_GetError());
                return false;
            }
            if (SDL_RenderCopy(renderer, texture, NULL, NULL) != 0)
            {
                fprintf(stderr, "SDL_RenderCopy failed: %s\n", SDL_GetError());
                return false;
            }
            /* SDL2 exposes no return value for Present; clear its error state
             * first so backend presentation failures are still observable. */
            SDL_ClearError();
            SDL_RenderPresent(renderer);
            if (SDL_GetError()[0] != '\0')
            {
                fprintf(stderr, "SDL_RenderPresent failed: %s\n", SDL_GetError());
                return false;
            }
        }
    }
    usleep(10000U);
    return true;
}

static bool _save_exit_screenshot(const sim_options_t *opt, const char *name)
{
    char path[512];
    uint16_t *copy;
    int path_length;
    int saved;

    if (opt->out_dir != NULL)
    {
        if ((mkdir(opt->out_dir, 0775) != 0) && (errno != EEXIST))
        {
            fprintf(stderr, "screenshot directory failed: %s\n", opt->out_dir);
            return false;
        }
        path_length = snprintf(path, sizeof(path), "%s/%s", opt->out_dir, name);
    }
    else
    {
        path_length = snprintf(path, sizeof(path), "%s", name);
    }
    if ((path_length < 0) || (path_length >= (int)sizeof(path)))
    {
        fprintf(stderr, "screenshot path too long\n");
        return false;
    }

    copy = malloc((size_t)SIM_BSP_H_RES * SIM_BSP_V_RES * sizeof(*copy));
    if (copy == NULL)
    {
        fprintf(stderr, "screenshot allocation failed\n");
        return false;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK)
    {
        fprintf(stderr, "screenshot framebuffer lock failed\n");
        free(copy);
        return false;
    }
    memcpy(copy, sim_bsp_framebuffer(),
           (size_t)SIM_BSP_H_RES * SIM_BSP_V_RES * sizeof(*copy));
    esp_lv_adapter_unlock();

    saved = sim_png_save_rgb565(path, copy, SIM_BSP_H_RES, SIM_BSP_V_RES,
                                opt->window_scale);
    free(copy);
    if (saved != 0)
    {
        fprintf(stderr, "screenshot failed: %s\n", path);
        return false;
    }
    printf("screenshot: %s\n", path);
    return true;
}

int main(int argc, char **argv)
{
    sim_options_t opt =
    {
        .headless = false,
        .ci = false,
        .res_dir = NULL,
        .nvs_dir = NULL,
        .sd_dir = "/tmp/mt_sim_sdcard",
        .out_dir = NULL,
        .navigate = NULL,
        .brightness = -1,
        .swipe = NULL,
        .key_count = 0,
        .ci_steps = 0,
        .agent_port = 5002,
        .window_scale = 1,
        .frames = 0
    };
    const char *screenshot = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    uint32_t iterations = 0U;
    int rc = EXIT_SUCCESS;

    if (!_parse_args(argc, argv, &opt, &screenshot))
    {
        return EXIT_FAILURE;
    }
    if (opt.window_scale < 1)
    {
        opt.window_scale = 1;
    }
    if (opt.res_dir == NULL)
    {
        _usage(argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, _on_signal);
    signal(SIGTERM, _on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (opt.headless ||
            ((getenv("DISPLAY") == NULL) && (getenv("WAYLAND_DISPLAY") == NULL)))
    {
        (void)SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    if (opt.headless &&
            (strstr(SDL_GetCurrentVideoDriver(), "dummy") == NULL))
    {
        fprintf(stderr, "headless requested but driver is '%s'\n",
                SDL_GetCurrentVideoDriver());
        rc = EXIT_FAILURE;
        goto exit_sdl;
    }
    printf("microtech_sim: SDL driver=%s headless=%d ci=%d agent-port=%d\n",
           SDL_GetCurrentVideoDriver(), opt.headless ? 1 : 0, opt.ci ? 1 : 0,
           opt.agent_port);

    if (opt.ci)
    {
        /* CI never touches the real network even with production endpoint
         * config: data arrives only through sim.set_* script injection. */
        (void)sim_http_set_script_only(true);
    }
    sim_mmap_assets_set_dir(opt.res_dir);
    if (opt.nvs_dir != NULL)
    {
        sim_nvs_set_dir(opt.nvs_dir);
    }
    if (opt.sd_dir != NULL)
    {
        char sd_abs[PATH_MAX];
        const char *sd_path = opt.sd_dir;
        if (sd_path[0] != '/' && realpath(".", sd_abs) != NULL)
        {
            if (snprintf(sd_abs + strlen(sd_abs), sizeof(sd_abs) - strlen(sd_abs),
                         "/%s", sd_path) < (int)sizeof(sd_abs))
            {
                sd_path = sd_abs;
            }
        }
        if (host_sd_boot(sd_path) != ESP_OK)
        {
            fprintf(stderr, "sim boot: host_sd_boot(%s) failed\n", sd_path);
        }
    }
    host_audio_set_available(true);

    if (sim_runtime_boot() != ESP_OK)
    {
        fprintf(stderr, "sim runtime boot failed\n");
        rc = EXIT_FAILURE;
        goto exit_sdl;
    }

    if (opt.ci)
    {
        /* Boot completed the mailbox handshake under monotonic tick; only
         * now is it safe to freeze tick into explicit step mode. */
        sim_lv_ci_mode_set(true);
        sim_time_set_sntp_enabled(false);
    }

    if (!sim_agent_start(opt.agent_port, opt.out_dir))
    {
        fprintf(stderr, "agent: failed to listen on 127.0.0.1:%d\n",
                opt.agent_port);
        rc = EXIT_FAILURE;
        goto exit_runtime;
    }

    if (opt.brightness >= 0)
    {
        printf("sim brightness set: %s -> %u\n",
               esp_err_to_name(app_manager_screen_set_brightness(
                                   (uint8_t)opt.brightness)),
               (unsigned)app_manager_screen_get_brightness());
        _script_advance(opt.ci, 5);
    }

    /* Scripted session: navigate -> key -> swipe, then optional steps. */
    if (opt.navigate != NULL)
    {
        const app_manager_nav_request_t nav =
        {
            .operation = APP_MANAGER_NAV_OP_RUN,
            .app_id = opt.navigate,
            .transition = { .effect = APP_MANAGER_TRANSITION_NONE },
        };
        esp_err_t nav_result;
        if (opt.ci)
        {
            /* A synchronous navigate would block this thread while the
             * mailbox drain needs steps issued by this same thread. */
            nav_result = app_manager_navigate_async(&nav, NULL, NULL);
        }
        else
        {
            nav_result = app_manager_navigate(&nav, UINT32_MAX);
        }
        printf("sim navigate %s: %s\n", opt.navigate,
               esp_err_to_name(nav_result));
        if (nav_result != ESP_OK)
        {
            rc = EXIT_FAILURE;
        }
        _script_advance(opt.ci, 20);
    }

    for (int k = 0; k < opt.key_count; k++)
    {
        char kname[16];
        char kact[16];
        if (sscanf(opt.key[k], "%15[^,],%15s", kname, kact) != 2)
        {
            continue;
        }
        const sim_key_t key = (strcmp(kname, "power") == 0)
                              ? SIM_KEY_POWER : SIM_KEY_BOOT;
        if (strcmp(kact, "click") == 0)
        {
            (void)sim_bsp_key(key, SIM_KEY_ACTION_PRESS);
            _script_advance(opt.ci, 3);
            (void)sim_bsp_key(key, SIM_KEY_ACTION_RELEASE);
            if (strcmp(kname, "boot") == 0)
            {
                /* Single-click home resolves after the double-click window
                 * in real time; wait, then drain the resulting action. */
                usleep(800000U);
                _script_advance(opt.ci, 40);
            }
        }
        else
        {
            const sim_key_action_t act =
                (strcmp(kact, "release") == 0)
                ? SIM_KEY_ACTION_RELEASE : SIM_KEY_ACTION_PRESS;
            (void)sim_bsp_key(key, act);
        }
        _script_advance(opt.ci, 20);
    }

    if (opt.swipe != NULL)
    {
        int x1;
        int y1;
        int x2;
        int y2;
        int nsteps = 8;
        if (sscanf(opt.swipe, "%d,%d,%d,%d,%d", &x1, &y1, &x2, &y2,
                   &nsteps) >= 4)
        {
            for (int i = 0; i <= nsteps; i++)
            {
                const int x = x1 + ((x2 - x1) * i) / nsteps;
                const int y = y1 + ((y2 - y1) * i) / nsteps;
                sim_lv_touch_update((int16_t)x, (int16_t)y, true);
                _script_advance(opt.ci, 1);
            }
            sim_lv_touch_update((int16_t)x2, (int16_t)y2, false);
            _script_advance(opt.ci, 20);
        }
    }

    if (opt.ci_steps > 0)
    {
        _script_advance(opt.ci, opt.ci_steps);
    }

    if (!opt.headless)
    {
        window = SDL_CreateWindow("microtech_sim",
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  (int)SIM_BSP_H_RES * opt.window_scale,
                                  (int)SIM_BSP_V_RES * opt.window_scale,
                                  SDL_WINDOW_SHOWN);
        if (window == NULL)
        {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            rc = EXIT_FAILURE;
            goto exit_runtime;
        }
        const int window_width = (int)SIM_BSP_H_RES * opt.window_scale;
        const int window_height = (int)SIM_BSP_V_RES * opt.window_scale;
        SDL_SetWindowResizable(window, SDL_FALSE);
        SDL_SetWindowMinimumSize(window, window_width, window_height);
        SDL_SetWindowMaximumSize(window, window_width, window_height);

        /* Software rendering is reliable on WSL/X11 copy-mode servers and
         * keeps the RGB565 texture path independent of host GL support. */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == NULL)
        {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            rc = EXIT_FAILURE;
            goto exit_runtime;
        }
        SDL_RendererInfo renderer_info;
        if (SDL_GetRendererInfo(renderer, &renderer_info) == 0)
        {
            printf("SDL renderer=%s flags=0x%08x\n",
                   renderer_info.name != NULL ? renderer_info.name : "unknown",
                   (unsigned)renderer_info.flags);
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    SIM_BSP_H_RES, SIM_BSP_V_RES);
        if (texture == NULL)
        {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            rc = EXIT_FAILURE;
            goto exit_runtime;
        }
    }

    if (opt.frames > 0)
    {
        while (!atomic_load(&sim_quit_flag) &&
                (iterations < (uint32_t)opt.frames))
        {
            if (!_run_iteration(&opt, window, texture, renderer))
            {
                rc = EXIT_FAILURE;
                break;
            }
            iterations++;
        }
    }
    else if (opt.ci)
    {
        /* CI sessions are resident: only a signal or sim.exit stops us. */
        while (!atomic_load(&sim_quit_flag))
        {
            usleep(50000U);
        }
    }
    else
    {
        while (!atomic_load(&sim_quit_flag))
        {
            if (!_run_iteration(&opt, window, texture, renderer))
            {
                rc = EXIT_FAILURE;
                break;
            }
            iterations++;
        }
    }

    if ((screenshot != NULL) && !_save_exit_screenshot(&opt, screenshot))
    {
        rc = EXIT_FAILURE;
    }
    printf("sim exiting: iterations=%u blits=%llu screen_suspended=%d brightness=%u pm_state=%d\n",
           (unsigned)iterations,
           (unsigned long long)sim_lv_frame_count(),
           sim_bsp_screen_is_suspended() ? 1 : 0,
           (unsigned)app_manager_screen_get_brightness(),
           (int)app_manager_pm_get_state());

exit_runtime:
    sim_agent_stop();
    (void)esp_lv_adapter_pause(-1);
    (void)esp_lv_adapter_deinit();

exit_sdl:
    if (texture != NULL)
    {
        SDL_DestroyTexture(texture);
    }
    if (renderer != NULL)
    {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL)
    {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return rc;
}
