#include "sam.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "imgui-extra/imgui_impl.h"

#define SDL_DISABLE_ARM_NEON_H 1
#include <SDL.h>
#include <SDL_opengl.h>
#include <cmath>

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

/**
 * Get the size of screen where the SDL window runs in.
 *
 * SDL_Window* window could be NULL, which means we get the screen size of the default 0-index display.
 * If window is not NULL, the we need to get the screen size of the display where the window runs in.
 *
 */
static bool get_screen_size(SDL_DisplayMode &dm, SDL_Window* window) {
    int displayIndex = 0;
    if (window != NULL) {
        displayIndex = SDL_GetWindowDisplayIndex(window);
    }
    if (displayIndex < 0) {
        return false;
    }
    if (SDL_GetCurrentDisplayMode(displayIndex, &dm) != 0) {
        return false;
    }

    fprintf(stderr, "%s: screen size (%d x %d) \n", __func__, dm.w, dm.h);
    return true;
}

// downscale image with nearest-neighbor interpolation
static sam_image_u8 downscale_img(sam_image_u8 &img , float scale) {
    sam_image_u8 new_img;

    int width = img.nx;
    int height = img.ny;

    int new_width = img.nx / scale + 0.5f;
    int new_height = img.ny / scale + 0.5f;

    new_img.nx = new_width;
    new_img.ny = new_height;
    new_img.data.resize(new_img.nx*new_img.ny*3);

    fprintf(stderr, "%s: scale: %f\n", __func__, scale);
    fprintf(stderr, "%s: resize image from (%d x %d) to (%d x %d)\n", __func__, img.nx, img.ny, new_img.nx, new_img.ny);

    for (int y = 0; y < new_height; ++y) {
        for (int x = 0; x < new_width; ++x) {
            int src_x = (x + 0.5f) * scale - 0.5f;
            int src_y = (y + 0.5f) * scale - 0.5f;

            int src_index = (src_y * width + src_x) * 3;
            int dest_index = (y * new_width + x) * 3;

            for (int c = 0; c < 3; ++c) {
                new_img.data[dest_index + c] = img.data[src_index + c];
            }
        }
    }


    return new_img;
}

static bool downscale_img_to_screen(sam_image_u8 &img, SDL_Window* window) {
    SDL_DisplayMode dm = {};
    if (!get_screen_size(dm, window)) {
        fprintf(stderr, "%s: failed to get screen size of the display.\n", __func__);
        return false;
    }
    fprintf(stderr, "%s: screen size (%d x %d) \n", __func__,dm.w,dm.h);
    if (dm.h == 0 || dm.w == 0) {
        // This means the window is running in other display.
        return false;
    }

    // Add 5% margin between screen and window
    const float margin = 0.05f;
    const int max_width  = dm.w - margin * dm.w;
    const int max_height = dm.h - margin * dm.h;

    fprintf(stderr, "%s: img size (%d x %d) \n", __func__,img.nx,img.ny);

    if (img.ny > max_height || img.nx > max_width) {
        fprintf(stderr, "%s: img size (%d x %d) exceeds maximum allowed size (%d x %d) \n", __func__,img.nx,img.ny,max_width,max_height);
        const float scale_y = (float)img.ny / max_height;
        const float scale_x = (float)img.nx / max_width;
        const float scale = std::max(scale_x, scale_y);

        img = downscale_img(img, scale);
    }

    return true;
}

static bool load_image_from_file(const std::string & fname, sam_image_u8 & img) {
    int nx, ny, nc;
    auto data = stbi_load(fname.c_str(), &nx, &ny, &nc, 3);
    if (!data) {
        fprintf(stderr, "%s: failed to load '%s'\n", __func__, fname.c_str());
        return false;
    }
    if (nc != 3) {
        fprintf(stdout, "%s: converted '%s' %d channels to 3\n", __func__, fname.c_str(), nc);
    }

    img.nx = nx;
    img.ny = ny;
    img.data.resize(nx * ny * 3);
    memcpy(img.data.data(), data, nx * ny * 3);

    stbi_image_free(data);

    return true;
}

static void print_usage(int argc, char ** argv, const sam_params & params) {
    fprintf(stderr, "usage: %s [options]\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -h, --help            show this help message and exit\n");
    fprintf(stderr, "  -s SEED, --seed SEED  RNG seed (default: -1)\n");
    fprintf(stderr, "  -t N, --threads N     number of threads to use during computation (default: %d)\n", params.n_threads);
    fprintf(stderr, "  -m FNAME, --model FNAME\n");
    fprintf(stderr, "                        model path (default: %s)\n", params.model.c_str());
    fprintf(stderr, "  -i FNAME, --inp FNAME\n");
    fprintf(stderr, "                        input file (default: %s)\n", params.fname_inp.c_str());
    fprintf(stderr, "  -o FNAME, --out FNAME\n");
    fprintf(stderr, "                        output file (default: %s)\n", params.fname_out.c_str());
    fprintf(stderr, "\n");
}

static bool params_parse(int argc, char ** argv, sam_params & params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-s" || arg == "--seed") {
            params.seed = std::stoi(argv[++i]);
        } else if (arg == "-t" || arg == "--threads") {
            params.n_threads = std::stoi(argv[++i]);
        } else if (arg == "-m" || arg == "--model") {
            params.model = argv[++i];
        } else if (arg == "-i" || arg == "--inp") {
            params.fname_inp = argv[++i];
        } else if (arg == "-o" || arg == "--out") {
            params.fname_out = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argc, argv, params);
            exit(0);
        } else {
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            print_usage(argc, argv, params);
            exit(0);
        }
    }

    return true;
}

bool ImGui_BeginFrame(SDL_Window * window) {
    ImGui_NewFrame(window);

    return true;
}

bool ImGui_EndFrame(SDL_Window * window) {
    // Rendering
    int display_w, display_h;
    SDL_GetWindowSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui::Render();
    ImGui_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window);

    return true;
}

GLuint createGLTexture(const sam_image_u8 & img, GLint format) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // Setup filtering parameters for display
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // This is required on WebGL for non power-of-two textures
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // Same

    // Upload pixels into texture
#if defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, format, img.nx, img.ny, 0, format, GL_UNSIGNED_BYTE, img.data.data());

    return tex;
}

void enable_blending(const ImDrawList*, const ImDrawCmd*) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
}

void disable_blending(const ImDrawList*, const ImDrawCmd*) {
    glDisable(GL_BLEND);
}

int main_loop(sam_image_u8 img, const sam_params & params, sam_state & state) {
    ImGui_PreInit();

    const char * title = "SAM.cpp";
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);

    SDL_Window * window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, img.nx, img.ny, window_flags);

    if (!window) {
        fprintf(stderr, "Error: %s\n", SDL_GetError());
        return -1;
    }

    void * gl_context = SDL_GL_CreateContext(window);

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    GLuint tex = createGLTexture(img, GL_RGB);

    ImGui_Init(window, gl_context);
    ImGui::GetIO().IniFilename = nullptr;

    ImGui_BeginFrame(window);
    ImGui::NewFrame();
    ImGui::EndFrame();
    ImGui_EndFrame(window);

    bool done = false;
    float x = 0.f;
    float y = 0.f;
    float xLast = 0.f;
    float yLast = 0.f;
    std::vector<sam_image_u8> masks;
    std::vector<GLuint> maskTextures;
    bool segmentOnMove = false;
    bool outputMultipleMasks = false;

    while (!done) {
        bool computeMasks = false;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    computeMasks = true;
                    x = event.button.x;
                    y = event.button.y;
                }
            }
            if (segmentOnMove && event.type == SDL_MOUSEMOTION) {
                x = event.motion.x;
                y = event.motion.y;
            }
            if (event.type == SDL_DROPFILE) {
                sam_image_u8 new_img;
                if (!load_image_from_file(std::string(event.drop.file), new_img)) {
                    printf("failed to load image from '%s'\n", event.drop.file);
                }
                else {
                    SDL_SetWindowTitle(window, "Encoding new img...");
                    downscale_img_to_screen(new_img, window);
                    if (!sam_compute_embd_img(new_img, params.n_threads, state)) {
                        printf("failed to compute encoded image\n");
                    }
                    printf("t_compute_img_ms = %d ms\n", state.t_compute_img_ms);

                    tex = createGLTexture(new_img, GL_RGB);

                    SDL_SetWindowSize(window, new_img.nx, new_img.ny);
                    SDL_SetWindowTitle(window, title);
                    img = std::move(new_img);
                    computeMasks = true;
                }
            }
        }

        if (segmentOnMove && (x != xLast || y != yLast)) {
            computeMasks = true;
        }

        xLast = x;
        yLast = y;

        if (computeMasks) {
            sam_point pt { x, y};
            printf("pt = (%f, %f)\n", pt.x, pt.y);

            masks = sam_compute_masks(img, params.n_threads, pt, state);

            if (!maskTextures.empty()) {
                glDeleteTextures(maskTextures.size(), maskTextures.data());
                maskTextures.clear();
            }

            for (auto& mask : masks) {
                sam_image_u8 mask_rgb = { mask.nx, mask.ny, };
                mask_rgb.data.resize(3*mask.nx*mask.ny);
                for (int i = 0; i < mask.nx*mask.ny; ++i) {
                    mask_rgb.data[3*i+0] = mask.data[i];
                    mask_rgb.data[3*i+1] = mask.data[i];
                    mask_rgb.data[3*i+2] = mask.data[i];
                }

                maskTextures.push_back(createGLTexture(mask_rgb, GL_RGB));
            }
        }

        ImGui_BeginFrame(window);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin(title, NULL, ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddImage((void*)(intptr_t)tex, ImVec2(0,0), ImVec2(img.nx, img.ny));

        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
        ImGui::Checkbox("Segment on hover", &segmentOnMove);
        ImGui::Checkbox("Output multiple masks", &outputMultipleMasks);
        ImGui::PopStyleColor();

        draw_list->AddCircleFilled(ImVec2(x, y), 5, IM_COL32(255, 0, 0, 255));

        draw_list->AddCallback(enable_blending, {});

        if (outputMultipleMasks) {
            for (int i = 0; i < int(maskTextures.size()); ++i) {
                const int r = i == 0 ? 255 : 0;
                const int g = i == 1 ? 255 : 0;
                const int b = i == 2 ? 255 : 0;
                draw_list->AddImage((void*)(intptr_t)maskTextures[i], ImVec2(0,0), ImVec2(img.nx, img.ny), ImVec2(0,0), ImVec2(1,1), IM_COL32(r, g, b, 172));
            }
        }
        else if (!maskTextures.empty()) {
            draw_list->AddImage((void*)(intptr_t)maskTextures[0], ImVec2(0,0), ImVec2(img.nx,img.ny), ImVec2(0,0), ImVec2(1,1), IM_COL32(0, 0, 255, 128));
        }

        draw_list->AddCallback(disable_blending, {});


        ImGui::End();
        ImGui::EndFrame();
        ImGui_EndFrame(window);
    }

    SDL_DestroyWindow(window);

    return 0;
}

int main(int argc, char ** argv) {
    sam_params params;
    if (!params_parse(argc, argv, params)) {
        return 1;
    }

    if (params.seed < 0) {
        params.seed = time(NULL);
    }
    fprintf(stderr, "%s: seed = %d\n", __func__, params.seed);

    // load the image
    sam_image_u8 img0;
    if (!load_image_from_file(params.fname_inp, img0)) {
        fprintf(stderr, "%s: failed to load image from '%s'\n", __func__, params.fname_inp.c_str());
        return 1;
    }
    fprintf(stderr, "%s: loaded image '%s' (%d x %d)\n", __func__, params.fname_inp.c_str(), img0.nx, img0.ny);

    // init SDL video subsystem to get the screen size
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "Error: %s\n", SDL_GetError());
        return -1;
    }

    // resize img when exceeds the screen
    downscale_img_to_screen(img0, NULL);

    std::shared_ptr<sam_state> state = sam_load_model(params);
    if (!state) {
        fprintf(stderr, "%s: failed to load model\n", __func__);
        return 1;
    }
    printf("t_load_ms = %d ms\n", state->t_load_ms);


    if (!sam_compute_embd_img(img0, params.n_threads, *state)) {
        fprintf(stderr, "%s: failed to compute encoded image\n", __func__);
        return 1;
    }
    printf("t_compute_img_ms = %d ms\n", state->t_compute_img_ms);

    int res = main_loop(std::move(img0), params, *state);

    sam_deinit(*state);

    return res;
}
