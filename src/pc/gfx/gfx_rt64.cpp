#ifdef RAPI_RT64

#if defined(_WIN32) || defined(_WIN64)

#if !defined(EXTERNAL_DATA) && !defined(RENDER_96_ALPHA)
#error "RT64 requires EXTERNAL_DATA to be enabled."
#endif

extern "C" {
#	include "../configfile.h"
#	include "../../game/area.h"
#	include "../../game/level_update.h"
#	include "../fs/fs.h"
#	include "../pc_main.h"
#	include "../../goddard/gd_math.h"
#	include "gfx_cc.h"
}

#include <cassert>
#include <stdint.h>

#include <stb/stb_image.h>
#include "xxhash/xxhash64.h"

#include "gfx_rt64.h"
#include "gfx_rt64_context.h"
#include "gfx_rt64_serialization.h"
#include "gfx_rt64_geo_map.h"

const unsigned short MouseButtonFlags[MAX_MOUSE_BUTTONS][2] = {
	{ RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_1_UP },
	{ RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_2_UP },
	{ RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_3_UP },
	{ RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP },
	{ RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP },
};

void gfx_rt64_render_thread();

uint16_t shaderVariantKey(bool raytrace, int filter, int hAddr, int vAddr, bool normalMap, bool specularMap) {
	uint16_t key = 0, fact = 1;
	key += raytrace ? fact : 0; fact *= 2;
	key += filter * fact; fact *= 2;
	key += hAddr * fact; fact *= 3;
	key += vAddr * fact; fact *= 3;
	key += normalMap ? fact : 0; fact *= 2;
	key += specularMap ? fact : 0; fact *= 2;
	return key;
}

inline RT64_VECTOR3 transform_position_affine(RT64_MATRIX4 m, RT64_VECTOR3 v) {
	RT64_VECTOR3 o;
	o.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0];
	o.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1];
	o.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2];
	return o;
}

inline RT64_VECTOR3 transform_direction_affine(RT64_MATRIX4 m, RT64_VECTOR3 v) {
	RT64_VECTOR3 o;
	o.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0];
	o.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1];
	o.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2];
	return o;
}

inline float vector_length(RT64_VECTOR3 v) {
	return sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline RT64_VECTOR3 normalize_vector(RT64_VECTOR3 v) {
	float length = vector_length(v);
	if (length > 0.0f) {
		return { v.x / length, v.y / length, v.z / length };
	}
	else {
		return { 0.0f, 0.0f, 0.0f };
	}
}

inline float vector_dot_product(RT64_VECTOR3 a, RT64_VECTOR3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

void elapsed_time(const LARGE_INTEGER &start, const LARGE_INTEGER &end, const LARGE_INTEGER &frequency, LARGE_INTEGER &elapsed) {
	elapsed.QuadPart = end.QuadPart - start.QuadPart;
	elapsed.QuadPart *= 1000000;
	elapsed.QuadPart /= frequency.QuadPart;
}

static void gfx_rt64_rapi_unload_shader(struct ShaderProgram *old_prg) {
	
}

static void gfx_rt64_rapi_load_shader(struct ShaderProgram *new_prg) {
	RT64.shaderProgram = new_prg;
}

static struct ShaderProgram *gfx_rt64_rapi_create_and_load_new_shader(uint32_t shader_id) {
	ShaderProgram *shaderProgram = new ShaderProgram();
    int c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }

	shaderProgram->shaderId = shader_id;
    shaderProgram->usedTextures[0] = false;
    shaderProgram->usedTextures[1] = false;
    shaderProgram->numInputs = 0;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > shaderProgram->numInputs) {
                    shaderProgram->numInputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                shaderProgram->usedTextures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                shaderProgram->usedTextures[1] = true;
            }
        }
    }

	RT64.shaderPrograms[shader_id] = shaderProgram;

	gfx_rt64_rapi_load_shader(shaderProgram);

	return shaderProgram;
}

static struct ShaderProgram *gfx_rt64_rapi_lookup_shader(uint32_t shader_id) {
	auto it = RT64.shaderPrograms.find(shader_id);
    return (it != RT64.shaderPrograms.end()) ? it->second : nullptr;
}

static void gfx_rt64_rapi_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->numInputs;
    used_textures[0] = prg->usedTextures[0];
    used_textures[1] = prg->usedTextures[1];
}

void gfx_rt64_rapi_preload_shader(unsigned int shader_id, bool raytrace, int filter, int hAddr, int vAddr, bool normalMap, bool specularMap) {
	ShaderProgram *shaderProgram = gfx_rt64_rapi_lookup_shader(shader_id);
	if (shaderProgram == nullptr) {
		shaderProgram = gfx_rt64_rapi_create_and_load_new_shader(shader_id);
	}
};

void gfx_rt64_rapi_preload_shaders() {
	gfx_rt64_rapi_preload_shader(0x45, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x45, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x45, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x45, 1, 1, 2, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x200, 0, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x200, 1, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x38D, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x38D, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x551, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0xA00, 0, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0xA00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0xA00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 1, 1, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 2, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 0, 0, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 0, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 0, 0, true, false);
	gfx_rt64_rapi_preload_shader(0x1045045, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1045A00, 0, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1045A00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1081081, 0, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1200045, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200045, 0, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200200, 0, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1200200, 1, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1200A00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200A00, 0, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x120038D, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200A00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1A00045, 0, 1, 1, 1, false, false);
	gfx_rt64_rapi_preload_shader(0x1A00045, 0, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1A00A00, 0, 0, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1A00A6F, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x3200045, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x3200045, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x3200045, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x3200045, 1, 1, 2, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x3200200, 1, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x3200A00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x3200A00, 1, 1, 0, 0, true, false);
	gfx_rt64_rapi_preload_shader(0x5045045, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x5045045, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x5045045, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x5045045, 1, 1, 1, 1, false, false);
	gfx_rt64_rapi_preload_shader(0x5045045, 0, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x5200200, 1, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 1, 1, 2, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 0, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 0, 0, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x5A00A00, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x7A00A00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x7A00A00, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x7A00A00, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200045, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x1141045, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x1200045, 1, 1, 0, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x3200A00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x9200200, 1, 0, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x920038D, 1, 1, 2, 2, false, false);
	gfx_rt64_rapi_preload_shader(0x9200A00, 1, 1, 0, 0, false, false);
	gfx_rt64_rapi_preload_shader(0x9200045, 1, 1, 0, 0, false, false);
}

int gfx_rt64_get_level_index() {
    return (gPlayerSpawnInfos[0].areaIndex >= 0) ? gCurrLevelNum : 0;
}

int gfx_rt64_get_area_index() {
    return (gPlayerSpawnInfos[0].areaIndex >= 0) ? gCurrAreaIndex : 0;
}

void gfx_rt64_toggle_inspector() {
	RT64.renderInspectorActive = !RT64.renderInspectorActive;
}

static void onkeydown(WPARAM w_param, LPARAM l_param) {
    int key = ((l_param >> 16) & 0x1ff);
    if (RT64.on_key_down != nullptr) {
        RT64.on_key_down(key);
    }
}

static void onkeyup(WPARAM w_param, LPARAM l_param) {
    int key = ((l_param >> 16) & 0x1ff);
    if (RT64.on_key_up != nullptr) {
        RT64.on_key_up(key);
    }
}

// Adapted from gfx_dxgi.cpp
static void gfx_rt64_toggle_full_screen(bool enable) {
    // Windows 7 + flip mode + waitable object can't go to exclusive fullscreen,
    // so do borderless instead. If DWM is enabled, this means we get one monitor
    // sync interval of latency extra. On Win 10 however (maybe Win 8 too), due to
    // "fullscreen optimizations" the latency is eliminated.
    if (enable == RT64.isFullScreen) {
        return;
    }

    if (!enable) {
        RECT r = RT64.lastWindowRect;

        // Set in window mode with the last saved position and size
        SetWindowLongPtr(RT64.hwnd, GWL_STYLE, WS_VISIBLE | WS_OVERLAPPEDWINDOW);

        if (RT64.lastMaximizedState) {
            SetWindowPos(RT64.hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
            ShowWindow(RT64.hwnd, SW_MAXIMIZE);
        } else {
            SetWindowPos(RT64.hwnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
            ShowWindow(RT64.hwnd, SW_RESTORE);
        }
    } else {
        // Save if window is maximized or not
        WINDOWPLACEMENT windowPlacement;
        windowPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(RT64.hwnd, &windowPlacement);
        RT64.lastMaximizedState = windowPlacement.showCmd == SW_SHOWMAXIMIZED;

        // Save window position and size if the window is not maximized
        GetWindowRect(RT64.hwnd, &RT64.lastWindowRect);
        configWindow.x = RT64.lastWindowRect.left;
        configWindow.y = RT64.lastWindowRect.top;
        configWindow.w = RT64.lastWindowRect.right - RT64.lastWindowRect.left;
        configWindow.h = RT64.lastWindowRect.bottom - RT64.lastWindowRect.top;

        // Get in which monitor the window is
        HMONITOR hmonitor = MonitorFromWindow(RT64.hwnd, MONITOR_DEFAULTTONEAREST);

        // Get info from that monitor
        MONITORINFOEX monitorInfo;
        monitorInfo.cbSize = sizeof(MONITORINFOEX);
        GetMonitorInfo(hmonitor, &monitorInfo);
        RECT r = monitorInfo.rcMonitor;

        // Set borderless full screen to that monitor
        SetWindowLongPtr(RT64.hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
        SetWindowPos(RT64.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    }

    RT64.isFullScreen = enable;
}

void gfx_rt64_apply_config() {
	{
    	const std::lock_guard<std::mutex> lock(RT64.renderViewDescMutex);
		RT64.renderViewDesc.resolutionScale = configRT64ResScale / 100.0f;
		RT64.renderViewDesc.maxLights = configRT64MaxLights;
		RT64.renderViewDesc.diSamples = configRT64SphereLights ? 1 : 0;
		RT64.renderViewDesc.giSamples = configRT64GI ? 1 : 0;
		RT64.renderViewDesc.denoiserEnabled = configRT64Denoiser;
		RT64.renderViewDesc.motionBlurStrength = configRT64MotionBlurStrength / 100.0f;
		RT64.renderViewDesc.dlssMode = configRT64DlssMode;
		RT64.useVsync = configWindow.vsync;
		RT64.targetFPS = configRT64TargetFPS;
		RT64.renderViewDescChanged = true;
	}

	// Adapted from gfx_dxgi.cpp
	if (configWindow.fullscreen != RT64.isFullScreen) {
        gfx_rt64_toggle_full_screen(configWindow.fullscreen);
	}

	if (!RT64.isFullScreen) {
		const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        const int xpos = (configWindow.x == WAPI_WIN_CENTERPOS) ? (screenWidth - configWindow.w) * 0.5 : configWindow.x;
        const int ypos = (configWindow.y == WAPI_WIN_CENTERPOS) ? (screenHeight - configWindow.h) * 0.5 : configWindow.y;
        RECT wr = { xpos, ypos, xpos + (int)configWindow.w, ypos + (int)configWindow.h };
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(RT64.hwnd, NULL, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, SWP_NOACTIVATE | SWP_NOZORDER);
	}
}

static void gfx_rt64_reset_logic_frame(void) {
	RT64.skyTextureId = 0;
    RT64.dynamicLightCount = 0;
}

static bool gfx_rt64_use_vsync() {
	return RT64.useVsync && !RT64.turboMode;
}

LRESULT CALLBACK gfx_rt64_wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (RT64.renderInspectorActive && (RT64.renderInspector != nullptr) && RT64.lib.HandleMessageInspector(RT64.renderInspector, message, wParam, lParam)) {
		return true;
	}

	switch (message) {
	case WM_SYSKEYDOWN:
		// Alt + Enter.
		if ((wParam == VK_RETURN) && ((lParam & 1 << 30) == 0)) {
			gfx_rt64_toggle_full_screen(!RT64.isFullScreen);
			break;
		} else {
			return DefWindowProcW(hWnd, message, wParam, lParam);
		}
	case WM_CLOSE:
		PostQuitMessage(0);
		game_exit();
		break;
	case WM_ACTIVATEAPP:
		RT64.windowActive = (wParam == TRUE);

        if (RT64.on_all_keys_up != nullptr) {
        	RT64.on_all_keys_up();
		}

        break;
	case WM_RBUTTONDOWN:
		if (RT64.renderInspectorActive) {
			RT64.pickedTextureHash = 0;
			RT64.pickTextureNextFrame = true;
			RT64.pickTextureHighlight = true;
		}

		break;
	case WM_RBUTTONUP:
		if (RT64.renderInspectorActive) {
			RT64.pickTextureHighlight = false;
		}

		break;
	case WM_KEYDOWN:
		if (wParam == VK_F1) {
			gfx_rt64_toggle_inspector();
		}

		if (wParam == VK_F2) {
			RT64.pauseMode = !RT64.pauseMode;
		}

		if (wParam == VK_F4) {
			RT64.turboMode = !RT64.turboMode;
		}
		
		if (RT64.renderInspectorActive) {
			if (wParam == VK_F5) {
				gfx_rt64_save_geo_layout_mods();
				gfx_rt64_save_texture_mods();
				gfx_rt64_save_level_lights();
			}
		}

		onkeydown(wParam, lParam);
		break;
	case WM_KEYUP:
		onkeyup(wParam, lParam);
		break;
	case WM_INPUT: {
		// Skip mouselook events if inspector is active.
		if (RT64.renderInspectorActive) {
			break;
		}
		
		UINT dwSize = sizeof(RAWINPUT);
		static BYTE lpb[sizeof(RAWINPUT)];
		GetRawInputData((HRAWINPUT)(lParam), RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER));
		RAWINPUT* raw = (RAWINPUT*)(lpb);
		if (raw->header.dwType == RIM_TYPEMOUSE) {
			RT64.deltaMouseX += raw->data.mouse.lLastX;
			RT64.deltaMouseY += raw->data.mouse.lLastY;

			// Detect mouse button states if any of them were enabled.
			if (raw->data.mouse.usButtonFlags & 0x3FF) {
				for (unsigned short b = 0; b < MAX_MOUSE_BUTTONS; b++) {
					if (raw->data.mouse.usButtonFlags & MouseButtonFlags[b][0]) {
						RT64.mouseButtons |= (1 << b);
					}
					else if (raw->data.mouse.usButtonFlags & MouseButtonFlags[b][1]) {
						RT64.mouseButtons &= ~(1 << b);
					}
				}
			}
    	}

		break;
	}
	case WM_PAINT: {
		LARGE_INTEGER ElapsedMicroseconds;

		// Apply configuration changes.
		if (configWindow.settings_changed) {
			gfx_rt64_apply_config();
			configWindow.settings_changed = false;
		}

		// Run one game iteration.
		if (!RT64.pauseMode && RT64.run_one_game_iter != nullptr) {
			LARGE_INTEGER StartTime, EndTime;
			QueryPerformanceCounter(&StartTime);
			gfx_rt64_reset_logic_frame();
			RT64.run_one_game_iter();
			elapsed_time(StartTime, EndTime, RT64.Frequency, ElapsedMicroseconds);
		}

		if (!RT64.turboMode) {
			// Try to maintain the fixed framerate.
			const int FixedFramerate = 30;
			const int FramerateMicroseconds = 1000000 / FixedFramerate;
			int cyclesWaited = 0;

			// Sleep if possible to avoid busy waiting too much.
			QueryPerformanceCounter(&RT64.EndingTime);
			elapsed_time(RT64.StartingTime, RT64.EndingTime, RT64.Frequency, ElapsedMicroseconds);
			int SleepMs = ((FramerateMicroseconds - ElapsedMicroseconds.QuadPart) - 500) / 1000;
			if (SleepMs > 0) {
				Sleep(SleepMs);
				cyclesWaited++;
			}

			// Busy wait to reach the desired framerate.
			do {
				QueryPerformanceCounter(&RT64.EndingTime);
				elapsed_time(RT64.StartingTime, RT64.EndingTime, RT64.Frequency, ElapsedMicroseconds);
				cyclesWaited++;
			} while (ElapsedMicroseconds.QuadPart < FramerateMicroseconds);

			RT64.StartingTime = RT64.EndingTime;

			// Drop the next frame if we didn't wait any cycles.
			RT64.dropNextFrame = (cyclesWaited == 1);
		}

		return 0;
	}
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

static void gfx_rt64_error_message(const char *window_title, const char *error_message) {
	MessageBox(NULL, error_message, window_title, MB_OK | MB_ICONEXCLAMATION);
}

static void gfx_rt64_wapi_init(const char *window_title) {
	// Setup library.
	RT64.lib = RT64_LoadLibrary();
	if (RT64.lib.handle == 0) {
		gfx_rt64_error_message(window_title, "Failed to load library. Please make sure rt64lib.dll and dxil.dll are placed next to the game's executable and are up to date.");
		abort();
	}

	// Register window class.
	WNDCLASS wc;
	memset(&wc, 0, sizeof(WNDCLASS));
	wc.lpfnWndProc = gfx_rt64_wnd_proc;
	wc.hInstance = GetModuleHandle(0);
	wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND);
	wc.lpszClassName = "RT64";
	RegisterClass(&wc);

	// Create window.
	const int Width = 1280;
	const int Height = 720;
	RECT rect;
	UINT dwStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	rect.left = (GetSystemMetrics(SM_CXSCREEN) - Width) / 2;
	rect.top = (GetSystemMetrics(SM_CYSCREEN) - Height) / 2;
	rect.right = rect.left + Width;
	rect.bottom = rect.top + Height;
	AdjustWindowRectEx(&rect, dwStyle, 0, 0);
	RT64.hwnd = CreateWindow(wc.lpszClassName, window_title, dwStyle, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0, 0, wc.hInstance, NULL);

	// Start timers.
	QueryPerformanceFrequency(&RT64.Frequency);
	QueryPerformanceCounter(&RT64.StartingTime);
	RT64.dropNextFrame = false;
	RT64.pauseMode = false;
	RT64.turboMode = false;

	// Load the default global lights and the ones from a file afterwards.
	gfx_rt64_default_level_lights();
	gfx_rt64_load_level_lights();
	
	// Load the texture mods from a file.
	gfx_rt64_load_texture_mods();

	// Initialize other attributes.
	RT64.scissorRect = { 0, 0, 0, 0 };
	RT64.viewportRect = { 0, 0, 0, 0 };
	RT64.fogColor.x = 0.0f;
	RT64.fogColor.y = 0.0f;
	RT64.fogColor.z = 0.0f;
	RT64.skyDiffuseMultiplier = { 1.0f, 1.0f, 1.0f };
	RT64.fogMul = RT64.fogOffset = 0;

	// Initialize the triangle list index array used by all meshes.
	unsigned int index = 0;
	while (index < GFX_MAX_BUFFERED) {
		RT64.indexTriangleList[index] = index;
		index++;
	}

	// Build identity matrix.
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			RT64.identityTransform.m[i][j] = (i == j) ? 1.0f : 0.0f;
		}
	}

	// Build a default material.
	RT64.defaultMaterial.ignoreNormalFactor = 0.0f;
    RT64.defaultMaterial.uvDetailScale = 1.0f;
	RT64.defaultMaterial.reflectionFactor = 0.0f;
	RT64.defaultMaterial.reflectionFresnelFactor = 1.0f;
    RT64.defaultMaterial.reflectionShineFactor = 0.0f;
	RT64.defaultMaterial.refractionFactor = 0.0f;
	RT64.defaultMaterial.specularColor = { 1.0f, 1.0f, 1.0f };
	RT64.defaultMaterial.specularExponent = 5.0f;
	RT64.defaultMaterial.solidAlphaMultiplier = 1.0f;
	RT64.defaultMaterial.shadowAlphaMultiplier = 1.0f;
	RT64.defaultMaterial.diffuseColorMix.x = 0.0f;
    RT64.defaultMaterial.diffuseColorMix.y = 0.0f;
    RT64.defaultMaterial.diffuseColorMix.z = 0.0f;
    RT64.defaultMaterial.diffuseColorMix.w = 0.0f;
	RT64.defaultMaterial.depthBias = 0.0f;
	RT64.defaultMaterial.shadowRayBias = 0.0f;
	RT64.defaultMaterial.selfLight.x = 0.0f;
    RT64.defaultMaterial.selfLight.y = 0.0f;
    RT64.defaultMaterial.selfLight.z = 0.0f;
	RT64.defaultMaterial.lightGroupMaskBits = RT64_LIGHT_GROUP_MASK_ALL;
	RT64.defaultMaterial.fogColor.x = 1.0f;
    RT64.defaultMaterial.fogColor.y = 1.0f;
    RT64.defaultMaterial.fogColor.z = 1.0f;
	RT64.defaultMaterial.fogMul = 0.0f;
	RT64.defaultMaterial.fogOffset = 0.0f;
	RT64.defaultMaterial.fogEnabled = false;

	// Initialize camera.
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];
	RecordedCamera defaultCamera;
	defaultCamera.viewMatrix = RT64.identityTransform;
    defaultCamera.nearDist = 1.0f;
    defaultCamera.farDist = 1000.0f;
    defaultCamera.fovRadians = 0.75f;
	CPUFrame->camera = defaultCamera;

	// Apply loaded configuration.
	gfx_rt64_apply_config();

	// Setup device.
	RT64.device = RT64.lib.CreateDevice(RT64.hwnd);
	if (RT64.device == nullptr) {
		gfx_rt64_error_message(window_title, RT64.lib.GetLastError());
		gfx_rt64_error_message(window_title, 
			"Failed to initialize RT64.\n\n"
			"Please make sure your GPU drivers are up to date and the Direct3D 12.1 feature level is supported.\n\n"
			"Windows 10 version 2004 or newer is also required for this feature level to work properly.\n\n"
			"If you're a mobile user, make sure that the high performance device is selected for this application on your system's settings.");
		
		abort();
	}

	// Preload shaders to avoid ingame stuttering.
	gfx_rt64_rapi_preload_shaders();

	// Create the render thread.
	RT64.renderThreadRunning = true;
	RT64.renderInspectorActive = false;
	RT64.renderThread = new std::thread(gfx_rt64_render_thread);
}

static void gfx_rt64_wapi_shutdown(void) {
	RT64.renderThreadRunning = false;
	RT64.renderThread->join();
	delete RT64.renderThread;
}

static void gfx_rt64_wapi_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {
	RT64.on_key_down = on_key_down;
    RT64.on_key_up = on_key_up;
    RT64.on_all_keys_up = on_all_keys_up;
}

static void gfx_rt64_wapi_main_loop(void (*run_one_game_iter)(void)) {
	RT64.run_one_game_iter = run_one_game_iter;

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static void gfx_rt64_wapi_get_dimensions(uint32_t *width, uint32_t *height) {
	RECT rect;
	GetClientRect(RT64.hwnd, &rect);
	*width = rect.right - rect.left;
	*height = rect.bottom - rect.top;
}

static void gfx_rt64_wapi_handle_events(void) {
}

static bool gfx_rt64_wapi_start_frame(void) {
	if (RT64.dropNextFrame) {
		RT64.dropNextFrame = false;
		return false;
	}
	else {
		return true;
	}
}

static void gfx_rt64_wapi_swap_buffers_begin(void) {
}

static void gfx_rt64_wapi_swap_buffers_end(void) {
}

double gfx_rt64_wapi_get_time(void) {
    return 0.0;
}

static bool gfx_rt64_rapi_z_is_from_0_to_1(void) {
    return true;
}

static uint32_t gfx_rt64_rapi_new_texture(const char *name) {
	// We reserve 0 for unassigned textures.
	uint32_t textureKey = 1 + RT64.textures.size();
	auto &recordedTexture = RT64.textures[textureKey];
	recordedTexture.texture = nullptr;
	recordedTexture.linearFilter = 0;
	recordedTexture.cms = 0;
	recordedTexture.cmt = 0;
	recordedTexture.hash = gfx_rt64_get_texture_name_hash(name);
	RT64.textureHashIdMap[recordedTexture.hash] = textureKey;
    return textureKey;
}

static void gfx_rt64_rapi_select_texture(int tile, uint32_t texture_id) {
	assert(tile < 2);
	RT64.currentTile = tile;
    RT64.currentTextureIds[tile] = texture_id;
}

static void gfx_rt64_rapi_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
	uint32_t textureKey = RT64.currentTextureIds[RT64.currentTile];
	RT64_TEXTURE_DESC &texDesc = RT64.textures[textureKey].texDesc;
	texDesc.byteCount = texDesc.height * texDesc.rowPitch;
	texDesc.bytes = malloc(texDesc.byteCount);
	memcpy(texDesc.bytes, rgba32_buf, texDesc.byteCount);
	texDesc.width = width;
	texDesc.height = height;
	texDesc.rowPitch = texDesc.width * 4;
	texDesc.format = RT64_TEXTURE_FORMAT_RGBA8;

	RT64.textureUploadQueueMutex.lock();
	RT64.textureUploadQueue.push(textureKey);
	RT64.textureUploadQueueMutex.unlock();
}

static void gfx_rt64_rapi_upload_texture_file(const char *file_path, const uint8_t *file_buf, uint64_t file_buf_size) {
	uint32_t textureKey = RT64.currentTextureIds[RT64.currentTile];
	RT64_TEXTURE_DESC &texDesc = RT64.textures[textureKey].texDesc;

	// Use special case for loading DDS directly.
	if (strstr(file_path, ".dds") || strstr(file_path, ".DDS")) {
		texDesc.byteCount = (int)(file_buf_size);
		texDesc.bytes = malloc(texDesc.byteCount);
		memcpy(texDesc.bytes, file_buf, texDesc.byteCount);
		texDesc.width =  texDesc.height = texDesc.rowPitch = -1;
		texDesc.format = RT64_TEXTURE_FORMAT_DDS;

		RT64.textureUploadQueueMutex.lock();
		RT64.textureUploadQueue.push(textureKey);
		RT64.textureUploadQueueMutex.unlock();
	}
	// Use stb image to load the file from memory instead if possible.
	else {
		int width, height;
		stbi_uc *data = stbi_load_from_memory(file_buf, file_buf_size, &width, &height, NULL, 4);
        if (data != nullptr) {
			texDesc.bytes = data;
			texDesc.width = width;
			texDesc.height = height;
			texDesc.rowPitch = texDesc.width * 4;
			texDesc.byteCount = texDesc.height * texDesc.rowPitch;
			texDesc.format = RT64_TEXTURE_FORMAT_RGBA8;
			
            RT64.textureUploadQueueMutex.lock();
			RT64.textureUploadQueue.push(textureKey);
			RT64.textureUploadQueueMutex.unlock();
		}
		else {
			fprintf(stderr, "stb_image was unable to load the texture file.\n");
		}
	}
}

static void gfx_rt64_rapi_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
	uint32_t textureKey = RT64.currentTextureIds[tile];
	auto &recordedTexture = RT64.textures[textureKey];
	recordedTexture.linearFilter = linear_filter;
	recordedTexture.cms = cms;
	recordedTexture.cmt = cmt;
}

static void gfx_rt64_rapi_set_depth_test(bool depth_test) {
}

static void gfx_rt64_rapi_set_depth_mask(bool depth_mask) {
}

static void gfx_rt64_rapi_set_zmode_decal(bool zmode_decal) {
}

static void gfx_rt64_rapi_set_viewport(int x, int y, int width, int height) {
	RT64.viewportRect = { x, y, width, height };
}

static void gfx_rt64_rapi_set_scissor(int x, int y, int width, int height) {
	RT64.scissorRect = { x, y, width, height };
}

static void gfx_rt64_rapi_set_use_alpha(bool use_alpha) {
}

static inline float gfx_rt64_norm_texcoord(float s, uint8_t address_mode) {
	return s - long(s);
}

static void gfx_rt64_rapi_process_mesh(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris, bool raytrace, RenderDisplayList &displayList) {
	// Calculate the required size for each vertex based on the shader.
	const bool useTexture = RT64.shaderProgram->usedTextures[0] || RT64.shaderProgram->usedTextures[1];
	const int numInputs = RT64.shaderProgram->numInputs;
	const bool useAlpha = RT64.shaderProgram->shaderId & SHADER_OPT_ALPHA;
	unsigned int vertexCount = 0;
	unsigned int vertexStride = 0;
	unsigned int indexCount = buf_vbo_num_tris * 3;
	void *vertexBuffer = buf_vbo;
	const unsigned int vertexFixedStride = 16 + 12;
	vertexStride = vertexFixedStride + (useTexture ? 8 : 0) + numInputs * (useAlpha ? 16 : 12);
	assert(((buf_vbo_len * 4) % vertexStride) == 0);

	vertexCount = (buf_vbo_len * 4) / vertexStride;
	assert(buf_vbo_num_tris == (vertexCount / 3));

	// Calculate hash and use it as key.
    XXHash64 hashStream(0);
	size_t vertexBufferSize = buf_vbo_len * sizeof(float);
	hashStream.add(buf_vbo, vertexBufferSize);
    uint64_t hash = hashStream.hash();

	// Make the vector large enough to fit the required meshes.
	if (displayList.meshes.size() < (displayList.drawCount + 1)) {
		displayList.meshes.resize(displayList.drawCount + 1);
	}

	// Try reusing the mesh that was stored in this index first.
	auto &dynMesh = displayList.meshes[displayList.drawCount];
	uint64_t curHash = dynMesh.vertexBufferHash;

	// We can reuse the mesh if the hash is the same.
	if (hash == curHash) {
		return;
	}
	// We can only reuse the mesh if the vertex buffers are compatible.
	else if (
		(dynMesh.vertexCount == vertexCount) && 
		(dynMesh.vertexStride == vertexStride) && 
		(dynMesh.indexCount == indexCount) && 
		(dynMesh.raytrace == raytrace)
	) {
		memcpy(dynMesh.vertexBuffer, vertexBuffer, vertexBufferSize);
		dynMesh.vertexBufferHash = hash;
		return;
	}
	
	// Destroy any previous pointers if they exist.
	if (dynMesh.vertexBuffer != nullptr) {
		free(dynMesh.vertexBuffer);
		dynMesh.vertexBuffer = nullptr;
	}

	// Create the mesh.
	dynMesh.vertexCount = vertexCount;
	dynMesh.vertexStride = vertexStride;
	dynMesh.indexCount = indexCount;
	dynMesh.useTexture = useTexture;
	dynMesh.raytrace = raytrace;
	dynMesh.vertexBuffer = (float *)(malloc(vertexBufferSize));
	memcpy(dynMesh.vertexBuffer, vertexBuffer, vertexBufferSize);
	dynMesh.vertexBufferHash = hash;

	/*
	// Store the mesh in the display list.
	if (!interpolate) {
		// Search for a dynamic mesh that has the same hash.
		auto dynamicMeshIt = RT64.dynamicMeshPool.find(hash);
		if (dynamicMeshIt != RT64.dynamicMeshPool.end()) {
			dynamicMeshIt->second.inUse = true;
			return dynamicMeshIt->second.mesh;
		}

		// Search linearly for a compatible dynamic mesh.
		uint64_t foundHash = 0;
		for (auto dynamicMeshIt : RT64.dynamicMeshPool) {
			if (
				!dynamicMeshIt.second.inUse &&
				(dynamicMeshIt.second.vertexCount == vertexCount) && 
				(dynamicMeshIt.second.vertexStride == vertexStride) && 
				(dynamicMeshIt.second.indexCount == indexCount) && 
				(dynamicMeshIt.second.raytrace == raytrace)
			) 
			{
				foundHash = dynamicMeshIt.first;
				break;
			}
		}

		// If we found a valid hash, change the hash where the mesh is stored.
		if (foundHash != 0) {
			RT64.dynamicMeshPool[hash] = RT64.dynamicMeshPool[foundHash];
			RT64.dynamicMeshPool.erase(foundHash);
		}

		auto &dynamicMesh = RT64.dynamicMeshPool[hash];

		// Create the mesh if it hasn't been created yet.
		if (dynamicMesh.mesh == nullptr) {
			dynamicMesh.mesh = RT64.lib.CreateMesh(RT64.device, raytrace ? (RT64_MESH_RAYTRACE_ENABLED | RT64_MESH_RAYTRACE_UPDATABLE) : 0);
			dynamicMesh.vertexCount = vertexCount;
			dynamicMesh.vertexStride = vertexStride;
			dynamicMesh.indexCount = indexCount;
			dynamicMesh.raytrace = raytrace;
		}

		dynamicMesh.inUse = true;
		RT64.lib.SetMesh(dynamicMesh.mesh, vertexBuffer, vertexCount, vertexStride, RT64.indexTriangleList, indexCount);
		return dynamicMesh.mesh;
	}
	*/
}

static void gfx_rt64_rapi_cache_static_rt_mesh(uint64_t key, const RecordedMesh &dynMesh) {
	/*
	RT64_MESH *mesh = RT64.lib.CreateMesh(RT64.device, RT64_MESH_RAYTRACE_ENABLED);
	RT64.lib.SetMesh(mesh, dynMesh.prevVertexBuffer, dynMesh.vertexCount, dynMesh.vertexStride, RT64.indexTriangleList, dynMesh.indexCount);
	RT64.staticMeshCache[key] = mesh;
	*/
}

static void gfx_rt64_add_light(RT64_LIGHT *lightMod, RT64_MATRIX4 transform, uint32_t dlUid, int32_t dlInstIndex) {
    assert(RT64.dynamicLightCount < MAX_DYNAMIC_LIGHTS);
    auto &dynLight = RT64.dynamicLights[RT64.dynamicLightCount++];

	dynLight.light = *lightMod;
	dynLight.light.position = transform_position_affine(transform, lightMod->position);
	dynLight.dlUid = dlUid;
	dynLight.dlInstIndex = dlInstIndex;

	// Use a vector that points in all three axes in case the node uses non-uniform scaling to get an estimate.
	RT64_VECTOR3 scaleVector = transform_direction_affine(transform, { 1.0f, 1.0f, 1.0f });
	float scale = vector_length(scaleVector) / sqrt(3);
	dynLight.light.attenuationRadius *= scale;
	dynLight.light.pointRadius *= scale;
	dynLight.light.shadowOffset *= scale;
}

static void gfx_rt64_rapi_apply_mod(RT64_MATERIAL *material, uint32_t *normal, uint32_t *specular, bool *interpolate,
	 RecordedMod *mod, RT64_MATRIX4 transform, bool applyLight, uint32_t dlUid, int32_t dlInstIndex) 
{
	if (!mod->interpolationEnabled) {
		*interpolate = false;
	}
	
	if (mod->materialMod != NULL) {
		RT64_ApplyMaterialAttributes(material, mod->materialMod);
	}

	if (applyLight && (mod->lightMod != NULL)) {
        gfx_rt64_add_light(mod->lightMod, transform, dlUid, dlInstIndex);
    }

	if (mod->normalMapHash != 0) {
		auto hashIt = RT64.textureHashIdMap.find(mod->normalMapHash);
		if (hashIt != RT64.textureHashIdMap.end()) {
			if (RT64.textures.find(hashIt->second) != RT64.textures.end()) {
				*normal = hashIt->second;
			}
		}
	}

	if (mod->specularMapHash != 0) {
		auto hashIt = RT64.textureHashIdMap.find(mod->specularMapHash);
		if (hashIt != RT64.textureHashIdMap.end()) {
			if (RT64.textures.find(hashIt->second) != RT64.textures.end()) {
				*specular = hashIt->second;
			}
		}
	}
}

static void gfx_rt64_rapi_draw_triangles_common(RT64_MATRIX4 transform, float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris, bool double_sided, bool raytrace, uint32_t uid) {
	assert(RT64.shaderProgram != nullptr);

	// Retrieve the previous transform for the display list with this UID and store the current one.
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];
	auto &displayList = CPUFrame->displayLists[uid];
	RecordedMod *textureMod = nullptr;
	bool linearFilter = false;
	bool interpolate = (uid != 0);
	uint32_t cms = 0, cmt = 0;

	// Make the vector large enough to fit the required instances.
	if (displayList.instances.size() < (displayList.drawCount + 1)) {
		displayList.instances.resize(displayList.drawCount + 1);
	}

	auto &displayListInstance = displayList.instances[displayList.drawCount];
	RT64_INSTANCE_DESC &instDesc = displayListInstance.desc;
	instDesc.scissorRect = RT64.scissorRect;
	instDesc.viewportRect = RT64.viewportRect;
	instDesc.transform = transform;
	instDesc.material = RT64.defaultMaterial;

	// Find all parameters associated to the texture if it's used.
	bool highlightMaterial = false;
	if (RT64.shaderProgram->usedTextures[0]) {
		uint32_t diffuseKey = RT64.currentTextureIds[RT64.currentTile];
		RecordedTexture &recordedTexture = RT64.textures[diffuseKey];
		linearFilter = recordedTexture.linearFilter; 
		cms = recordedTexture.cms; 
		cmt = recordedTexture.cmt;

		if (recordedTexture.texture != nullptr) {
			displayListInstance.textures.diffuse = diffuseKey;
		}

		// Use the hash from the texture alias if it exists.
		uint64_t textureHash = recordedTexture.hash;
		auto texAliasIt = RT64.texHashAliasMap.find(textureHash);
		if (texAliasIt != RT64.texHashAliasMap.end()) {
			textureHash = texAliasIt->second;
		}

		// Use the texture mod for the matching texture hash.
		auto texModIt = RT64.texMods.find(textureHash);
		if (texModIt != RT64.texMods.end()) {
			textureMod = texModIt->second;
		}
		
		// Update data for ray picking.
		/*
		if (RT64.pickTextureHighlight && (recordedTexture.hash == RT64.pickedTextureHash)) {
			highlightMaterial = true;
		}

		RT64.lastInstanceTextureHashes[instance] = recordedTexture.hash;
		*/
	}

	// Build material with applied mods.
	if (RT64.graphNodeMod != nullptr) {
		gfx_rt64_rapi_apply_mod(
			&instDesc.material,
			&displayListInstance.textures.normal,
			&displayListInstance.textures.specular,
			&interpolate,
			RT64.graphNodeMod,
			transform,
			false,
			uid,
			displayList.drawCount);
	}

	if (textureMod != nullptr) {
		gfx_rt64_rapi_apply_mod(
			&instDesc.material,
			&displayListInstance.textures.normal,
			&displayListInstance.textures.specular,
			&interpolate,
			textureMod,
			transform,
			true,
			uid,
			displayList.drawCount);
	}

	// Apply a higlight color if the material is selected.
	if (highlightMaterial) {
		instDesc.material.diffuseColorMix = { 1.0f, 0.0f, 1.0f, 0.5f };
		instDesc.material.selfLight = { 1.0f, 1.0f, 1.0f };
		instDesc.material.lightGroupMaskBits = 0;
	}

	// Copy the fog to the material.
	instDesc.material.fogColor = RT64.fogColor;
	instDesc.material.fogMul = RT64.fogMul;
	instDesc.material.fogOffset = RT64.fogOffset;
	instDesc.material.fogEnabled = (RT64.shaderProgram->shaderId & SHADER_OPT_FOG) != 0;

	unsigned int filter = linearFilter ? RT64_SHADER_FILTER_LINEAR : RT64_SHADER_FILTER_POINT;
	unsigned int hAddr = (cms & G_TX_CLAMP) ? RT64_SHADER_ADDRESSING_CLAMP : (cms & G_TX_MIRROR) ? RT64_SHADER_ADDRESSING_MIRROR : RT64_SHADER_ADDRESSING_WRAP;
	unsigned int vAddr = (cmt & G_TX_CLAMP) ? RT64_SHADER_ADDRESSING_CLAMP : (cmt & G_TX_MIRROR) ? RT64_SHADER_ADDRESSING_MIRROR : RT64_SHADER_ADDRESSING_WRAP;
	bool normalMap = (displayListInstance.textures.normal > 0);
	bool specularMap = (displayListInstance.textures.specular > 0);
	displayListInstance.shader.program = RT64.shaderProgram;
	displayListInstance.shader.raytrace = raytrace;
	displayListInstance.shader.filter = filter;
	displayListInstance.shader.hAddr = hAddr;
	displayListInstance.shader.vAddr = vAddr;
	displayListInstance.shader.normalMap = normalMap;
	displayListInstance.shader.specularMap = specularMap;
	displayListInstance.interpolate = interpolate;

	// Mark the right instance flags.
	instDesc.flags = 0;
	if (RT64.background) {
		instDesc.flags |= RT64_INSTANCE_RASTER_BACKGROUND;
	}

	if (double_sided) {
		instDesc.flags |= RT64_INSTANCE_DISABLE_BACKFACE_CULLING;
	}

	gfx_rt64_rapi_process_mesh(buf_vbo, buf_vbo_len, buf_vbo_num_tris, raytrace, displayList);
	
	// Increase the counter.
	displayList.drawCount++;
}

void gfx_rt64_rapi_set_fog(uint8_t fog_r, uint8_t fog_g, uint8_t fog_b, int16_t fog_mul, int16_t fog_offset) {
	RT64.fogColor.x = fog_r / 255.0f;
	RT64.fogColor.y = fog_g / 255.0f;
	RT64.fogColor.z = fog_b / 255.0f;
	RT64.fogMul = fog_mul;
	RT64.fogOffset = fog_offset;
}

static void gfx_rt64_rapi_draw_triangles_ortho(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris, bool double_sided, uint32_t uid) {
	gfx_rt64_rapi_draw_triangles_common(RT64.identityTransform, buf_vbo, buf_vbo_len, buf_vbo_num_tris, double_sided, false, uid);
}

static void gfx_rt64_rapi_draw_triangles_persp(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris, float transform_affine[4][4], bool double_sided, uint32_t uid) {
	// Stop considering the orthographic projection triangles as background as soon as perspective triangles are drawn.
	if (RT64.background) {
		RT64.background = false;
	}

	RT64_MATRIX4 transform;
	memcpy(transform.m, transform_affine, sizeof(float) * 16);
	gfx_rt64_rapi_draw_triangles_common(transform, buf_vbo, buf_vbo_len, buf_vbo_num_tris, double_sided, true, uid);
}

static void gfx_rt64_rapi_init(void) {
}

static void gfx_rt64_rapi_on_resize(void) {
	if (!RT64.isFullScreen) {
		uint32_t w = 0, h = 0;
		gfx_rt64_wapi_get_dimensions(&w, &h);
		configWindow.w = w;
		configWindow.h = h;
	}
}

static void gfx_rt64_rapi_shutdown(void) {
}

static void gfx_rt64_rapi_start_frame(void) {
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];

	// Reset frame camera interpolation.
	CPUFrame->interpolateCamera = true;

	// Determine cursor visibility base on the current camera mouselook support and the inspector.
	bool newCursorVisible = (!RT64.isFullScreen && !RT64.mouselookEnabled) || (RT64.renderInspectorActive);
	if (RT64.cursorVisible != newCursorVisible) {
		ShowCursor(newCursorVisible);
		RT64.cursorVisible = newCursorVisible;
	}
	
	RT64.background = true;
    RT64.graphNodeMod = nullptr;

	// Reset display lists on the current CPU frame.
	auto dlIt = CPUFrame->displayLists.begin();
	while (dlIt != CPUFrame->displayLists.end()) {
		auto &dl = dlIt->second;
		dl.drawCount = 0;
		dlIt++;
	}

	/*
	if (RT64.inspector != nullptr) {
		char marioMessage[256] = "";
		char levelMessage[256] = "";
        int levelIndex = gfx_rt64_get_level_index();
        int areaIndex = gfx_rt64_get_area_index();
		sprintf(marioMessage, "Mario pos: %.1f %.1f %.1f", gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2]);
        sprintf(levelMessage, "Level #%d Area #%d", levelIndex, areaIndex);
		RT64.lib.PrintMessageInspector(RT64.inspector, marioMessage);
		RT64.lib.PrintMessageInspector(RT64.inspector, levelMessage);
		RT64.lib.PrintMessageInspector(RT64.inspector, "F1: Toggle inspectors");
		RT64.lib.PrintMessageInspector(RT64.inspector, "F5: Save all configuration");

		// Inspect the current scene.
		RT64.lib.SetSceneInspector(RT64.inspector, &RT64.levelAreaLighting[levelIndex][areaIndex].sceneDesc);

		// Inspect the current level's lights.
        RT64_LIGHT *lights = RT64.levelAreaLighting[levelIndex][areaIndex].lights;
        int *lightCount = &RT64.levelAreaLighting[levelIndex][areaIndex].lightCount;
		RT64.lib.SetLightsInspector(RT64.inspector, lights, lightCount, MAX_LEVEL_LIGHTS);
	}
	*/
}

static inline int gfx_rt64_lerp_int(int a, int b, float t) {
	return a + lround(t * (b - a));
}

static inline float gfx_rt64_lerp_float(float a, float b, float t) {
	return a + t * (b - a);
}

static inline RT64_VECTOR3 gfx_rt64_lerp_vector3(RT64_VECTOR3 a, RT64_VECTOR3 b, float t) {
	return {
		gfx_rt64_lerp_float(a.x, b.x, t),
		gfx_rt64_lerp_float(a.y, b.y, t),
		gfx_rt64_lerp_float(a.z, b.z, t)
	};
}

static inline RT64_RECT gfx_rt64_lerp_rect(RT64_RECT a, RT64_RECT b, float t) {
	return {
		gfx_rt64_lerp_int(a.x, b.x, t),
		gfx_rt64_lerp_int(a.y, b.y, t),
		gfx_rt64_lerp_int(a.w, b.w, t),
		gfx_rt64_lerp_int(a.h, b.h, t)
	};
}

static inline RT64_MATRIX4 gfx_rt64_lerp_matrix(const RT64_MATRIX4 &a, const RT64_MATRIX4 &b, float t) {
	// TODO: This is just a hacky way to see some interpolated values, but it is NOT the proper way
	// to interpolate a transformation matrix. That will likely require decomposition of both the matrices.
	RT64_MATRIX4 c;
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			c.m[i][j] = gfx_rt64_lerp_float(a.m[i][j], b.m[i][j], t);
		}
	}
	return c;
}

static inline bool gfx_rt64_skip_matrix_lerp(const RT64_MATRIX4 &a, const RT64_MATRIX4 &b, const float minDot) {
	RT64_VECTOR3 prevX, prevY, prevZ;
	RT64_VECTOR3 newX, newY, newZ;
	float dotX, dotY, dotZ;
	prevX = normalize_vector(transform_direction_affine(a, { 1.0f, 0.0f, 0.0f } ));
	prevY = normalize_vector(transform_direction_affine(a, { 0.0f, 1.0f, 0.0f } ));
	prevZ = normalize_vector(transform_direction_affine(a, { 0.0f, 0.0f, 1.0f } ));
	newX = normalize_vector(transform_direction_affine(b, { 1.0f, 0.0f, 0.0f } ));
	newY = normalize_vector(transform_direction_affine(b, { 0.0f, 1.0f, 0.0f } ));
	newZ = normalize_vector(transform_direction_affine(b, { 0.0f, 0.0f, 1.0f } ));
	dotX = vector_dot_product(prevX, newX);
	dotY = vector_dot_product(prevY, newY);
	dotZ = vector_dot_product(prevZ, newZ);
	return (dotX < minDot) || (dotY < minDot) || (dotZ < minDot);
}

static void gfx_rt64_rapi_set_special_stage_lights(int levelIndex, int areaIndex) {
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];

	// Dynamic Lakitu camera light for Shifting Sand Land Pyramid.
	if ((levelIndex == 8) && (areaIndex == 2)) {
        // Build the dynamic light.
		// TODO: Add interpolation support.
        auto &dynLight = RT64.dynamicLights[RT64.dynamicLightCount++];
		RT64_VECTOR3 viewPos = { CPUFrame->camera.invViewMatrix.m[3][0], CPUFrame->camera.invViewMatrix.m[3][1], CPUFrame->camera.invViewMatrix.m[3][2] };
		RT64_VECTOR3 marioPos = { gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2] };
		dynLight.light.diffuseColor.x = 1.0f;
		dynLight.light.diffuseColor.y = 0.9f;
		dynLight.light.diffuseColor.z = 0.5f;
		dynLight.light.position.x = viewPos.x + (viewPos.x - marioPos.x);
		dynLight.light.position.y = viewPos.y + 150.0f;
		dynLight.light.position.z = viewPos.z + (viewPos.z - marioPos.z);
		dynLight.light.attenuationRadius = 4000.0f;
		dynLight.light.attenuationExponent = 1.0f;
		dynLight.light.pointRadius = 25.0f;
		dynLight.light.specularColor = { 0.65f, 0.585f, 0.325f };
		dynLight.light.shadowOffset = 1000.0f;
		dynLight.light.groupBits = RT64_LIGHT_GROUP_DEFAULT;
		dynLight.dlUid = 0;
		dynLight.dlInstIndex = -1;
	}
}

void gfx_rt64_rapi_draw_frame(float frameWeight) {
	/*
	// Interpolate the display lists.
	auto displayListIt = RT64.displayLists.begin();
	while (displayListIt != RT64.displayLists.end()) {
		for (auto &dynMesh : displayListIt->second.meshes) {
			if (!dynMesh.newVertexBufferValid) {
				continue;
			}

			// Recreate the temporal buffer if required.
			size_t requiredVertexBufferSize = dynMesh.vertexCount * dynMesh.vertexStride;
			if (requiredVertexBufferSize > tempVertexBufferSize) {
				free(tempVertexBuffer);
				tempVertexBuffer = (float *)(malloc(requiredVertexBufferSize));
				tempVertexBufferSize = requiredVertexBufferSize;
			}

			// Interpolate all the floats in the temporal vertex buffer.
			size_t f = 0;
			size_t floatCount = requiredVertexBufferSize / sizeof(float);
			float *tempPtr = tempVertexBuffer;
			float *prevPtr = dynMesh.prevVertexBuffer;
			float *newPtr = dynMesh.newVertexBuffer;
			while (f < floatCount) {
				*tempPtr = gfx_rt64_lerp_float(*prevPtr, *newPtr, frameWeight);
				tempPtr++;
				prevPtr++;
				newPtr++;
				f++;
			}

			// Update the mesh using the temporal vertex buffer.
			RT64.lib.SetMesh(dynMesh.mesh, tempVertexBuffer, dynMesh.vertexCount, dynMesh.vertexStride, RT64.indexTriangleList, dynMesh.indexCount);
		}

		displayListIt++;
	}

	// Interpolate the dynamic lights.
	int levelIndex = gfx_rt64_get_level_index();
	int areaIndex = gfx_rt64_get_area_index();
	int areaLightCount = RT64.levelAreaLighting[levelIndex][areaIndex].lightCount;
	for (int i = 0; i < RT64.dynamicLightCount; i++) {
		auto &light = RT64.lights[areaLightCount + i];
		const auto &prevLight = RT64.dynamicLights[i].prevLight;
		const auto &newLight = RT64.dynamicLights[i].newLight;
		light.position = gfx_rt64_lerp_vector3(prevLight.position, newLight.position, frameWeight);
		light.attenuationRadius = gfx_rt64_lerp_float(prevLight.attenuationRadius, newLight.attenuationRadius, frameWeight);
		light.pointRadius = gfx_rt64_lerp_float(prevLight.pointRadius, newLight.pointRadius, frameWeight);
		light.shadowOffset = gfx_rt64_lerp_float(prevLight.shadowOffset, newLight.shadowOffset, frameWeight);
	}
	*/
}

static void gfx_rt64_rapi_end_frame(void) {
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];

	// Add all dynamic lights for this stage first.
	{
    	int levelIndex = gfx_rt64_get_level_index();
    	int areaIndex = gfx_rt64_get_area_index();
		gfx_rt64_rapi_set_special_stage_lights(levelIndex, areaIndex);

		// Update the scene's description.
		const auto &areaLighting = RT64.levelAreaLighting[levelIndex][areaIndex];
		CPUFrame->sceneDesc = areaLighting.sceneDesc;
		CPUFrame->sceneDesc.skyDiffuseMultiplier.x *= RT64.skyDiffuseMultiplier.x;
		CPUFrame->sceneDesc.skyDiffuseMultiplier.y *= RT64.skyDiffuseMultiplier.y;
		CPUFrame->sceneDesc.skyDiffuseMultiplier.z *= RT64.skyDiffuseMultiplier.z;
		CPUFrame->skyTextureId = RT64.skyTextureId;

		// Build lights array out of the static level lights and the dynamic lights.
		int areaLightCount = areaLighting.lightCount;
		CPUFrame->lightCount = areaLightCount/* + RT64.dynamicLightCount*/;
		assert(CPUFrame->lightCount <= MAX_LIGHTS);
		memcpy(CPUFrame->lights, areaLighting.lights, sizeof(RT64_LIGHT) * areaLightCount);
		/*
		for (int i = 0; i < RT64.dynamicLightCount; i++) {
			memcpy(&RT64.lights[areaLightCount + i], &RT64.dynamicLights[i].newLight, sizeof(RT64_LIGHT));
		}
		*/
	}

	// Clean up any unused instances or meshes from the display lists.
	auto dlIt = CPUFrame->displayLists.begin();
	while (dlIt != CPUFrame->displayLists.end()) {
		auto &dl = dlIt->second;
		
		// Destroy all unused instances.
		while (dl.instances.size() > dl.drawCount) {
			auto &dynInst = dl.instances.back();
			dl.instances.pop_back();
		}

		// Destroy all unused meshes.
		while (dl.meshes.size() > dl.drawCount) {
			auto &dynMesh = dl.meshes.back();
			free(dynMesh.vertexBuffer);
			dl.meshes.pop_back();
		}

		// Keep the display list if it's not empty. Erase it otherwise.
		if (!dl.instances.empty() || !dl.meshes.empty()) {
			dlIt++;
		}
		else {
			dlIt = CPUFrame->displayLists.erase(dlIt);
		}
	}

	// Submit the current CPU frame as the next frame to draw and start writing on the next CPU frame.
	bool waitForBarrier = false;
	{
		const std::lock_guard<std::mutex> lock(RT64.renderFrameIndexMutex);
		RT64.GPUFrameIndex = RT64.CPUFrameIndex;
		RT64.CPUFrameIndex = (RT64.CPUFrameIndex + 1) % MAX_RENDER_FRAMES;
		waitForBarrier = (RT64.CPUFrameIndex == RT64.BarrierFrameIndex);
	}

	// Stall the thread until the barrier is lifted if we're trying to write on a frame being used by the GPU.
	while (waitForBarrier) {
		const std::lock_guard<std::mutex> lock(RT64.renderFrameIndexMutex);
		waitForBarrier = (RT64.CPUFrameIndex == RT64.BarrierFrameIndex);
	}
	
	/*
	// Print debugging messages.
	if (RT64.inspector != nullptr) {
		char statsMessage[256] = "";
    	sprintf(statsMessage, "RT %d Raster %d Lights %d Cached %d DynPoolSz %d", rtInstanceCount, rasterInstanceCount, RT64.lightCount, RT64.staticMeshCache.size(), RT64.dynamicMeshPool.size());
    	RT64.lib.PrintMessageInspector(RT64.inspector, statsMessage);
	}

	// Left click allows to pick a texture for editing from the viewport.
	if (RT64.pickTextureNextFrame) {
		POINT cursorPos = {};
		GetCursorPos(&cursorPos);
		ScreenToClient(RT64.hwnd, &cursorPos);
		RT64_INSTANCE *instance = RT64.lib.GetViewRaytracedInstanceAt(RT64.view, cursorPos.x, cursorPos.y);
		if (instance != nullptr) {
			auto instIt = RT64.lastInstanceTextureHashes.find(instance);
			if (instIt != RT64.lastInstanceTextureHashes.end()) {
				RT64.pickedTextureHash = instIt->second;
			}
		}
		else {
			RT64.pickedTextureHash = 0;
		}

		RT64.pickTextureNextFrame = false;
	}

	RT64.lastInstanceTextureHashes.clear();

	// Edit last picked texture.
	if (RT64.pickedTextureHash != 0) {
		const std::string textureName = RT64.texNameMap[RT64.pickedTextureHash];
		RecordedMod *texMod = RT64.texMods[RT64.pickedTextureHash];
		if (texMod == nullptr) {
			texMod = new RecordedMod();
			RT64.texMods[RT64.pickedTextureHash] = texMod;
		}

		if (texMod->materialMod == nullptr) {
			texMod->materialMod = new RT64_MATERIAL();
			texMod->materialMod->enabledAttributes = RT64_ATTRIBUTE_NONE;
		}

		if (RT64.inspector != nullptr) {
			RT64.lib.SetMaterialInspector(RT64.inspector, texMod->materialMod, textureName.c_str());
		}
	}

	// Display list cleanup.
	int maxCaches = CACHED_MESH_MAX_PER_FRAME;
	dlIt = RT64.displayLists.begin();
	while (dlIt != RT64.displayLists.end()) {
		auto &dl = dlIt->second;

		// Move attributes from new to prev for instances.
		for (auto &dynInst : dl.instances) {
			dynInst.prevTransform = dynInst.newTransform;
			dynInst.prevScissorRect = dynInst.newScissorRect;
			dynInst.prevViewportRect = dynInst.newViewportRect;
			dynInst.prevValid = true;
			dynInst.newValid = false;
		}

		// Move attributes from new to prev for meshes.
		for (auto &dynMesh : dl.meshes) {
			if (!dynMesh.newVertexBufferValid) {
				if (
					configRT64StaticMeshCache &&
					(maxCaches > 0) && 
					(dynMesh.staticFrames >= CACHED_MESH_REQUIRED_FRAMES) && 
					(RT64.staticMeshCache.find(dynMesh.prevVertexBufferHash) == RT64.staticMeshCache.end())
				) 
				{
					gfx_rt64_rapi_cache_static_rt_mesh(dynMesh.prevVertexBufferHash, dynMesh);
					maxCaches--;
				}

				continue;
			}

			float *swapBuffer = dynMesh.prevVertexBuffer;
			uint64_t swapHash = dynMesh.prevVertexBufferHash;
			dynMesh.prevVertexBuffer = dynMesh.newVertexBuffer;
			dynMesh.prevVertexBufferHash = dynMesh.newVertexBufferHash;
			dynMesh.newVertexBuffer = swapBuffer;
			dynMesh.newVertexBufferHash = swapHash;
			dynMesh.newVertexBufferValid = false;
		}

		dlIt++;
	}

	// Dynamic mesh pool cleanup.
	auto dynamicMeshIt = RT64.dynamicMeshPool.begin();
	while (dynamicMeshIt != RT64.dynamicMeshPool.end()) {
		if (dynamicMeshIt->second.inUse) {
			dynamicMeshIt->second.inUse = false;
			dynamicMeshIt++;
        }
		else {
			RT64.lib.DestroyMesh(dynamicMeshIt->second.mesh);
			dynamicMeshIt = RT64.dynamicMeshPool.erase(dynamicMeshIt);
		}
	}
	*/
}

static void gfx_rt64_rapi_finish_render(void) {

}

static void gfx_rt64_rapi_set_camera_perspective(float fov_degrees, float near_dist, float far_dist, bool can_interpolate) {
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];
    CPUFrame->camera.fovRadians = (fov_degrees / 180.0f) * M_PI;
	CPUFrame->camera.nearDist = near_dist;
    CPUFrame->camera.farDist = far_dist;
	CPUFrame->interpolateCamera = CPUFrame->interpolateCamera && can_interpolate;
}

static void gfx_rt64_rapi_set_camera_matrix(float matrix[4][4]) {
	RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];
	memcpy(&CPUFrame->camera.viewMatrix.m, matrix, sizeof(float) * 16);
    gd_inverse_mat4f(&CPUFrame->camera.viewMatrix.m, &CPUFrame->camera.invViewMatrix.m);
}

static void gfx_rt64_rapi_register_layout_graph_node(void *geoLayout, void *graphNode) {
    if (graphNode != nullptr) {
        // Delete the previous graph node mod if it exists already.
        // Graph node addresses can be reused, so it's important to remove any previous mods
        // and only keep the most up to date version of them.
        auto graphNodeIt = RT64.graphNodeMods.find(graphNode);
        if (graphNodeIt != RT64.graphNodeMods.end()) {
            delete graphNodeIt->second;
            RT64.graphNodeMods.erase(graphNodeIt);
        }
    }

	if ((geoLayout != nullptr) && (graphNode != nullptr)) {
        // Find the mod for the specified geoLayout.
        auto it = RT64.geoLayoutMods.find(geoLayout);
		RecordedMod *geoMod = (it != RT64.geoLayoutMods.end()) ? it->second : nullptr;
		if (geoMod != nullptr) {
			RecordedMod *graphMod = RT64.graphNodeMods[graphNode];
			if (graphMod == nullptr) {
				graphMod = new RecordedMod();
				RT64.graphNodeMods[graphNode] = graphMod;
			}

			if (geoMod->materialMod != nullptr) {
				if (graphMod->materialMod == nullptr) {
					graphMod->materialMod = new RT64_MATERIAL();
					graphMod->materialMod->enabledAttributes = RT64_ATTRIBUTE_NONE;
				}

				RT64_ApplyMaterialAttributes(graphMod->materialMod, geoMod->materialMod);
				graphMod->materialMod->enabledAttributes |= geoMod->materialMod->enabledAttributes;
			}

			if (geoMod->lightMod != nullptr) {
				if (graphMod->lightMod == nullptr) {
					graphMod->lightMod = new RT64_LIGHT();
				}

				memcpy(graphMod->lightMod, geoMod->lightMod, sizeof(RT64_LIGHT));
			}

			if (geoMod->normalMapHash != 0) {
				graphMod->normalMapHash = geoMod->normalMapHash;
			}

			if (geoMod->specularMapHash != 0) {
				graphMod->specularMapHash = geoMod->specularMapHash;
			}

			if (!geoMod->interpolationEnabled) {
				graphMod->interpolationEnabled = geoMod->interpolationEnabled;
			}
		}
	}
}

static void *gfx_rt64_rapi_build_graph_node_mod(void *graphNode, float modelview_matrix[4][4], uint32_t uid) {
    auto graphNodeIt = RT64.graphNodeMods.find(graphNode);
    if (graphNodeIt != RT64.graphNodeMods.end()) {
        RecordedMod *graphNodeMod = (RecordedMod *) (graphNodeIt->second);
        if (graphNodeMod != nullptr) {
            if (graphNodeMod->lightMod != nullptr) {
				RenderFrame *CPUFrame = &RT64.renderFrames[RT64.CPUFrameIndex];
				RT64_MATRIX4 transform;
                gfx_matrix_mul(transform.m, modelview_matrix, CPUFrame->camera.invViewMatrix.m);
                gfx_rt64_add_light(graphNodeMod->lightMod, transform, uid, -1);
            }

            return graphNodeMod;
        }
    }

    return nullptr;
}

static void gfx_rt64_rapi_set_graph_node_mod(void *graph_node_mod) {
	RT64.graphNodeMod = (RecordedMod *)(graph_node_mod);
}

static void gfx_rt64_rapi_set_skybox(uint32_t texture_id, float diffuse_color[3]) {
	RT64.skyTextureId = texture_id;
	RT64.skyDiffuseMultiplier = { diffuse_color[0], diffuse_color[1], diffuse_color[2] };
}

extern "C" bool gfx_rt64_dlss_supported() {
	return false;
	/*
	return RT64.lib.GetViewFeatureSupport(RT64.view, RT64_FEATURE_DLSS);
	*/
}

extern "C" void gfx_register_layout_graph_node(void *geoLayout, void *graphNode) {
	static bool loadedLayoutMods = false;
	if (!loadedLayoutMods) {
		gfx_rt64_load_geo_layout_mods();
		loadedLayoutMods = true;
	}

    gfx_rt64_rapi_register_layout_graph_node(geoLayout, graphNode);
}

extern "C" void *gfx_build_graph_node_mod(void *graphNode, float modelview_matrix[4][4], uint32_t uid) {
    return gfx_rt64_rapi_build_graph_node_mod(graphNode, modelview_matrix, uid);
}

RT64_TEXTURE *gfx_rt64_render_thread_find_texture(uint32_t textureKey) {
	auto texIt = RT64.textures.find(textureKey);
	if (texIt != RT64.textures.end()) {
		return texIt->second.texture;
	}
	else {
		return RT64.blankTexture;
	}
}

inline void gfx_rt64_render_thread_draw_display_list(uint32_t uid, RenderFrame *curFrame, RenderFrame *prevFrame, float curFrameWeight) {
	auto &gpuDl = RT64.GPUDisplayLists[uid];
	const auto &curDisplayList = curFrame->displayLists[uid];
	const auto &prevDisplayList = prevFrame->displayLists[uid];

	// Make the vectors large enough to fit all the instances and meshes.
	if (gpuDl.instances.size() < curDisplayList.drawCount) {
		gpuDl.instances.resize(curDisplayList.drawCount);
	}

	if (gpuDl.meshes.size() < curDisplayList.drawCount) {
		gpuDl.meshes.resize(curDisplayList.drawCount);
	}

	for (int i = 0; i < curDisplayList.drawCount; i++) {
		auto &dstMesh = gpuDl.meshes[i];
		auto &dstInstance = gpuDl.instances[i];
		const auto &curMesh = curDisplayList.meshes[i];
		const auto &curInstance = curDisplayList.instances[i];
		const auto &prevInstance = (i < prevDisplayList.instances.size()) ? prevDisplayList.instances[i] : curDisplayList.instances[i];

		// Destroy the existing mesh if the raytracing mode is different.
		if ((dstMesh.mesh != nullptr) && (dstMesh.raytrace != curMesh.raytrace)) {
			RT64.lib.DestroyMesh(dstMesh.mesh);
			dstMesh.mesh = nullptr;
		}

		// Create the mesh if it doesn't exist yet.
		if (dstMesh.mesh == nullptr) {
			dstMesh.mesh = RT64.lib.CreateMesh(RT64.device, curMesh.raytrace ? (RT64_MESH_RAYTRACE_ENABLED | RT64_MESH_RAYTRACE_UPDATABLE) : 0);
			dstMesh.raytrace = curMesh.raytrace;
		}

		// Update the mesh if the vertex buffer hash is different.
		if (dstMesh.vertexBufferHash != curMesh.vertexBufferHash) {
			RT64.lib.SetMesh(dstMesh.mesh, curMesh.vertexBuffer, curMesh.vertexCount, curMesh.vertexStride, RT64.indexTriangleList, curMesh.indexCount);
			dstMesh.vertexBufferHash = curMesh.vertexBufferHash;
		}

		// Create the instance if it doesn't exist yet.
		if (dstInstance.instance == nullptr) {
			dstInstance.instance = RT64.lib.CreateInstance(RT64.scene);
			dstInstance.transform = curInstance.desc.transform;
		}

		// Create the shader if necessary.
		const auto &shader = curInstance.shader;
		uint16_t variantKey = shaderVariantKey(shader.raytrace, shader.filter, shader.hAddr, shader.vAddr, shader.normalMap, shader.specularMap);
		if (shader.program->shaderVariantMap[variantKey] == nullptr) {
			int flags = shader.raytrace ? RT64_SHADER_RAYTRACE_ENABLED : RT64_SHADER_RASTER_ENABLED;
			if (shader.normalMap) {
				flags |= RT64_SHADER_NORMAL_MAP_ENABLED;
			}

			if (shader.specularMap) {
				flags |= RT64_SHADER_SPECULAR_MAP_ENABLED;
			}

			shader.program->shaderVariantMap[variantKey] = RT64.lib.CreateShader(RT64.device, shader.program->shaderId, shader.filter, shader.hAddr, shader.vAddr, flags);
		}

		/*
			// Compute the delta vertex buffer.
			for (auto &dynMesh : dl.meshes) {
				if (dynMesh.raytrace) {
					rtInstanceCount++;
				}
				else {
					rasterInstanceCount++;
				}

				if (!dynMesh.newVertexBufferValid) {
					continue;
				}

				float *prevPtr = dynMesh.prevVertexBuffer;
				float *newPtr = dynMesh.newVertexBuffer;
				float *deltaPtr = dynMesh.deltaVertexBuffer;
				size_t f = 0, i = 0;
				size_t imax = dynMesh.vertexStride / sizeof(float);
				size_t floatCount = dynMesh.vertexCount * imax;
				float deltaValue = 0.0f;
				const float Epsilon = 1e-6f;
				const float MagnitudeThreshold = 10.0f;
				while (f < floatCount) {
					deltaValue = *newPtr - *prevPtr;

					switch (i) {
					// Position interpolation.
					case 0:
					case 1:
					case 2:
						// Skip interpolating objects that suddenly teleport the vertices around.
						// This helps with effects like lava bubbles, snow, and other types of effects without
						// having to generate UIDs for each individual particle.
						// Since this relies on an arbitrary value to detect the magnitude difference, it might
						// break depending on the game. The minimum value of 1.0 is also reliant on the fact
						// the game never sends vertices with non-integer values when untransformed, making it
						// the smallest possible value that isn't zero.
						if ((fabsf(deltaValue) / std::max(fabsf(*deltaPtr), 1.0f)) >= MagnitudeThreshold) {
							*prevPtr = *newPtr;
						}

						break;
					// Texture coordinate interpolation.
					case 7:
					case 8:
						if (dynMesh.useTexture) {
							// Reuse previous delta if the delta values have different signs.
							// This helps with textures that scroll and eventually reset to their starting
							// position. Since the intended effect is usually to continue the scrolling motion,
							// just reusing the previously known delta value that actually worked is usually a
							// good enough strategy. This might break depending on the game if the UVs are used
							// for anything that doesn't resemble this type of effect.
							if ((deltaValue * (*deltaPtr)) < 0.0f) {
								deltaValue = *deltaPtr;
								*prevPtr = *newPtr - deltaValue;
							}
						}

						break;
					// Any other vertex element.
					default:
						break;
					}

					*deltaPtr = deltaValue;
					prevPtr++;
					newPtr++;
					deltaPtr++;
					f++;
					i = (i + 1) % imax;
				}
			}
		*/

		// Update the instance.
		RT64_INSTANCE_DESC instDesc = curInstance.desc;
		instDesc.scissorRect = gfx_rt64_lerp_rect(prevInstance.desc.scissorRect, curInstance.desc.scissorRect, curFrameWeight);
		instDesc.viewportRect = gfx_rt64_lerp_rect(prevInstance.desc.viewportRect, curInstance.desc.viewportRect, curFrameWeight);
		instDesc.diffuseTexture = gfx_rt64_render_thread_find_texture(curInstance.textures.diffuse);
		instDesc.normalTexture = gfx_rt64_render_thread_find_texture(curInstance.textures.normal);
		instDesc.specularTexture = gfx_rt64_render_thread_find_texture(curInstance.textures.specular);
		instDesc.mesh = dstMesh.mesh;
		instDesc.shader = shader.program->shaderVariantMap[variantKey];

		// Detect sudden transformation changes and skip interpolation if necessary.
		const float MinDot = sqrt(2.0f) / -2.0f;
		if (!curInstance.interpolate || gfx_rt64_skip_matrix_lerp(prevInstance.desc.transform, curInstance.desc.transform, MinDot)) {
			instDesc.previousTransform = curInstance.desc.transform;
			instDesc.transform = curInstance.desc.transform;
		}
		else {
			instDesc.previousTransform = dstInstance.transform;
			instDesc.transform = gfx_rt64_lerp_matrix(prevInstance.desc.transform, curInstance.desc.transform, curFrameWeight);
		}

		RT64.lib.SetInstanceDescription(dstInstance.instance, instDesc);
		dstInstance.transform = instDesc.transform;

		gpuDl.drawCount++;
	}
}

void gfx_rt64_render_thread_draw_frame(RenderFrame *curFrame, RenderFrame *prevFrame, float curFrameWeight) {
	// Reset the draw counter for all active display lists.
	auto gpuDlIt = RT64.GPUDisplayLists.begin();
	while (gpuDlIt != RT64.GPUDisplayLists.end()) {
		auto &dl = gpuDlIt->second;
		dl.drawCount = 0;
		gpuDlIt++;
	}

	// Queue up all display lists first.
	auto dlIt = curFrame->displayLists.begin();
	while (dlIt != curFrame->displayLists.end()) {
		gfx_rt64_render_thread_draw_display_list(dlIt->first, curFrame, prevFrame, curFrameWeight);
		dlIt++;
	}

	// Clean up any unused instances or meshes from the GPU display lists.
	gpuDlIt = RT64.GPUDisplayLists.begin();
	while (gpuDlIt != RT64.GPUDisplayLists.end()) {
		auto &dl = gpuDlIt->second;
		
		// Destroy all unused instances.
		while (dl.instances.size() > dl.drawCount) {
			auto &dynInst = dl.instances.back();
			RT64.lib.DestroyInstance(dynInst.instance);
			dl.instances.pop_back();
		}

		// Destroy all unused meshes.
		while (dl.meshes.size() > dl.drawCount) {
			auto &dynMesh = dl.meshes.back();
			RT64.lib.DestroyMesh(dynMesh.mesh);
			dl.meshes.pop_back();
		}
		
		gpuDlIt++;
	}

	// Interpolate and update the view.
	RT64_MATRIX4 viewMatrix;
	float fovRadians;
	bool interpolateCamera = curFrame->interpolateCamera;

	// Detect if camera interpolation should be skipped.
	// Attempts to fix sudden camera changes like the ones in BBH.
	if (interpolateCamera && gfx_rt64_skip_matrix_lerp(prevFrame->camera.viewMatrix, curFrame->camera.viewMatrix, 0.0f)) {
		interpolateCamera = false;
	}

	if (interpolateCamera) {
		viewMatrix = gfx_rt64_lerp_matrix(prevFrame->camera.viewMatrix, curFrame->camera.viewMatrix, curFrameWeight);
		fovRadians = gfx_rt64_lerp_float(prevFrame->camera.fovRadians, curFrame->camera.fovRadians, curFrameWeight);
	}
	else {
		viewMatrix = curFrame->camera.viewMatrix;
		fovRadians = curFrame->camera.fovRadians;
	}

	RT64.lib.SetViewPerspective(RT64.view, viewMatrix, fovRadians, curFrame->camera.nearDist, curFrame->camera.farDist, curFrame->interpolateCamera);

	// Update the scene.
	RT64.lib.SetSceneLights(RT64.scene, curFrame->lights, curFrame->lightCount);
	RT64.lib.SetSceneDescription(RT64.scene, curFrame->sceneDesc);
	RT64.lib.SetViewSkyPlane(RT64.view, (curFrame->skyTextureId > 0) ? RT64.textures[curFrame->skyTextureId].texture : nullptr);

	// Draw everything and update the window.
	RT64.lib.DrawDevice(RT64.device, gfx_rt64_use_vsync() ? 1 : 0);
}

void gfx_rt64_render_thread_upload_texture_queue() {
	// Upload all textures on the queue. Work with a copy of the queue so it's free to keep 
	// loading more textures during the next frame.
	RT64.textureUploadQueueMutex.lock();
	auto textureUploadQueue = RT64.textureUploadQueue;
	RT64.textureUploadQueue = { };
	RT64.textureUploadQueueMutex.unlock();

	while (!textureUploadQueue.empty()) {
		uint32_t textureKey = textureUploadQueue.front();
		auto &recordedTexture = RT64.textures[textureKey];
		assert(recordedTexture.texture == nullptr);
		assert(recordedTexture.texDesc.bytes != nullptr);
		recordedTexture.texture = RT64.lib.CreateTexture(RT64.device, recordedTexture.texDesc);
		free(recordedTexture.texDesc.bytes);
		recordedTexture.texDesc.bytes = nullptr;
		textureUploadQueue.pop();
	}
}

void gfx_rt64_render_thread() {
	LARGE_INTEGER StartTime, EndTime, ElapsedMicroseconds;

	// Setup scene and view.
	RT64.scene = RT64.lib.CreateScene(RT64.device);
	RT64.view = RT64.lib.CreateView(RT64.scene);

	// Preload a blank texture.
	const int BlankTextureSize = 64;
	int blankBytesCount = BlankTextureSize * BlankTextureSize * 4;
	unsigned char *blankBytes = (unsigned char *)(malloc(blankBytesCount));
	memset(blankBytes, 0xFF, blankBytesCount);

	RT64_TEXTURE_DESC texDesc;
	texDesc.bytes = blankBytes;
	texDesc.byteCount = blankBytesCount;
	texDesc.format = RT64_TEXTURE_FORMAT_RGBA8;
	texDesc.width = BlankTextureSize;
	texDesc.height = BlankTextureSize;
	texDesc.rowPitch = texDesc.width * 4;
	RT64.blankTexture = RT64.lib.CreateTexture(RT64.device, texDesc);
	free(blankBytes);

	int curFrameIndex = -1;
	int prevFrameIndex = -1;
	int renderTargetFPS = 30;
	while (RT64.renderThreadRunning) {
		// Create or destroy the inspector depending on the current state of the flag.
		if (RT64.renderInspectorActive && (RT64.renderInspector == nullptr)) {
			RT64.renderInspector = RT64.lib.CreateInspector(RT64.device);
		}
		else if (!RT64.renderInspectorActive && (RT64.renderInspector != nullptr)) {
			RT64.lib.DestroyInspector(RT64.renderInspector);
			RT64.renderInspector = nullptr;
		}

		// Update the view description if modified.
		{
			const std::lock_guard<std::mutex> lock(RT64.renderViewDescMutex);
			if (RT64.renderViewDescChanged) {
				renderTargetFPS = RT64.targetFPS;
				RT64.lib.SetViewDescription(RT64.view, RT64.renderViewDesc);
				RT64.renderViewDescChanged = false;
			}
		}

		gfx_rt64_render_thread_upload_texture_queue();

		// Retrieve the frame to draw if there's any and clear the last submitted frame.
		// Indicate there's a barrier in the previous frame being used and the CPU should not write to it.
		{
			const std::lock_guard<std::mutex> lock(RT64.renderFrameIndexMutex);
			curFrameIndex = RT64.GPUFrameIndex;
			prevFrameIndex = (curFrameIndex == 0) ? (MAX_RENDER_FRAMES - 1) : (curFrameIndex - 1);
			RT64.BarrierFrameIndex = prevFrameIndex;
			RT64.GPUFrameIndex = -1;
		}

		// Draw as many frames as the target framerate indicates if there's a valid frame to be used.
		if (curFrameIndex >= 0) {
			const unsigned int framesPerUpdate = renderTargetFPS / 30;
			const float weightPerFrame = 1.0f / framesPerUpdate;
			for (int f = 0; f < framesPerUpdate; f++) {
				gfx_rt64_render_thread_draw_frame(&RT64.renderFrames[curFrameIndex], &RT64.renderFrames[prevFrameIndex], (f + 1) * weightPerFrame);
			}

			// Clear the barrier.
			{
				const std::lock_guard<std::mutex> lock(RT64.renderFrameIndexMutex);
				RT64.BarrierFrameIndex = -1;
			}
		}
	}
}

struct GfxWindowManagerAPI gfx_rt64_wapi = {
    gfx_rt64_wapi_init,
    gfx_rt64_wapi_set_keyboard_callbacks,
    gfx_rt64_wapi_main_loop,
    gfx_rt64_wapi_get_dimensions,
    gfx_rt64_wapi_handle_events,
    gfx_rt64_wapi_start_frame,
    gfx_rt64_wapi_swap_buffers_begin,
    gfx_rt64_wapi_swap_buffers_end,
    gfx_rt64_wapi_get_time,
    gfx_rt64_wapi_shutdown,
};

struct GfxRenderingAPI gfx_rt64_rapi = {
    gfx_rt64_rapi_z_is_from_0_to_1,
    gfx_rt64_rapi_unload_shader,
    gfx_rt64_rapi_load_shader,
    gfx_rt64_rapi_create_and_load_new_shader,
    gfx_rt64_rapi_lookup_shader,
    gfx_rt64_rapi_shader_get_info,
    gfx_rt64_rapi_new_texture,
    gfx_rt64_rapi_select_texture,
    gfx_rt64_rapi_upload_texture,
    gfx_rt64_rapi_upload_texture_file,
    gfx_rt64_rapi_set_sampler_parameters,
    gfx_rt64_rapi_set_depth_test,
    gfx_rt64_rapi_set_depth_mask,
    gfx_rt64_rapi_set_zmode_decal,
    gfx_rt64_rapi_set_viewport,
    gfx_rt64_rapi_set_scissor,
    gfx_rt64_rapi_set_use_alpha,
	gfx_rt64_rapi_set_fog,
	gfx_rt64_rapi_set_camera_perspective,
	gfx_rt64_rapi_set_camera_matrix,
	gfx_rt64_rapi_draw_triangles_ortho,
    gfx_rt64_rapi_draw_triangles_persp,
	gfx_rt64_rapi_set_graph_node_mod,
	gfx_rt64_rapi_set_skybox,
    gfx_rt64_rapi_init,
	gfx_rt64_rapi_on_resize,
    gfx_rt64_rapi_start_frame,
	gfx_rt64_rapi_end_frame,
	gfx_rt64_rapi_finish_render,
    gfx_rt64_rapi_shutdown
};

#else

#error "RT64 is only supported on Windows"

#endif // _WIN32

#endif
