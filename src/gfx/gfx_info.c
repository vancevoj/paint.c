/* gfx_info.c - the graphics adapter behind an SDL renderer, for Settings >
 * Diagnostics (lane SHELL, wave 3b; WINDOWS.md 8.9 "GPU and driver").
 *
 * OpenGL and OpenGL ES renderers: glGetString(GL_VENDOR, GL_RENDERER,
 * GL_VERSION) of the renderer's context (current on the main thread).
 * Vulkan renderer: VkPhysicalDeviceProperties of the renderer's physical
 * device (deviceName, API and driver versions) through the loader that SDL
 * opened. The GPU renderer reports its backend. Other back ends (Direct3D,
 * Metal) and the software renderer report the renderer only. No Vulkan
 * or OpenGL headers are needed: the few entry points are declared here
 * with their fixed C ABI.
 *
 * Thread rules: main thread (the renderer's thread). */
#include "gfx.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) && !defined(_WIN64) && defined(_MSC_VER)
#  define GFX_APIENTRY __stdcall
#elif defined(_WIN32) && !defined(_WIN64) && defined(__GNUC__)
#  define GFX_APIENTRY __attribute__((stdcall))
#else
#  define GFX_APIENTRY
#endif

typedef const unsigned char *(GFX_APIENTRY *gl_get_string_fn)(unsigned int name);
typedef void (*vk_void_fn)(void);
typedef vk_void_fn (GFX_APIENTRY *vk_get_instance_proc_fn)(void *instance, const char *name);
typedef void (GFX_APIENTRY *vk_get_props_fn)(void *physical_device, void *props);

#define GL_VENDOR_ID   0x1F00u
#define GL_RENDERER_ID 0x1F01u
#define GL_VERSION_ID  0x1F02u

static void gl_info(char *out, size_t cap)
{
    gl_get_string_fn get;
    const unsigned char *vendor, *renderer, *version;
    if (!SDL_GL_GetCurrentContext()) return;
    get = (gl_get_string_fn)SDL_GL_GetProcAddress("glGetString");
    if (!get) return;
    vendor = get(GL_VENDOR_ID);
    renderer = get(GL_RENDERER_ID);
    version = get(GL_VERSION_ID);
    if (!renderer) return;
    snprintf(out, cap, "%s (%s), OpenGL %s", (const char *)renderer,
             vendor ? (const char *)vendor : "?", version ? (const char *)version : "?");
}

static void vk_info(SDL_PropertiesID props, char *out, size_t cap)
{
    void *inst = SDL_GetPointerProperty(props, SDL_PROP_RENDERER_VULKAN_INSTANCE_POINTER, NULL);
    void *phys = SDL_GetPointerProperty(props, SDL_PROP_RENDERER_VULKAN_PHYSICAL_DEVICE_POINTER,
                                        NULL);
    vk_get_instance_proc_fn gip;
    vk_get_props_fn gp;
    /* VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID,
     * deviceID, deviceType (4 bytes each), deviceName[256], then more;
     * the whole struct is well below 1 KiB, 4 KiB leaves room */
    union { uint8_t b[4096]; uint64_t align; } p;
    uint32_t api, drv, vendor;
    char name[257];
    if (!inst || !phys) return;
    gip = (vk_get_instance_proc_fn)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gip) return;
    gp = (vk_get_props_fn)gip(inst, "vkGetPhysicalDeviceProperties");
    if (!gp) return;
    memset(&p, 0, sizeof p);
    gp(phys, p.b);
    memcpy(&api, p.b, 4);
    memcpy(&drv, p.b + 4, 4);
    memcpy(&vendor, p.b + 8, 4);
    memcpy(name, p.b + 20, 256);
    name[256] = '\0';
    snprintf(out, cap, "%s (vendor 0x%04X), Vulkan %u.%u.%u, driver 0x%08X", name,
             (unsigned)vendor, (unsigned)(api >> 22), (unsigned)((api >> 12) & 0x3FFu),
             (unsigned)(api & 0xFFFu), (unsigned)drv);
}

void gfx_renderer_describe(SDL_Renderer *r, char *out, size_t cap)
{
    const char *name;
    char adapter[512];
    SDL_PropertiesID props;
    if (!cap) return;
    out[0] = '\0';
    if (!r) {
        snprintf(out, cap, "no renderer");
        return;
    }
    name = SDL_GetRendererName(r);
    props = SDL_GetRendererProperties(r);
    adapter[0] = '\0';
    if (name && (strcmp(name, "opengl") == 0 || strcmp(name, "opengles2") == 0)) {
        gl_info(adapter, sizeof adapter);
    } else if (name && strcmp(name, "vulkan") == 0) {
        vk_info(props, adapter, sizeof adapter);
    } else if (name && strcmp(name, "gpu") == 0) {
        SDL_GPUDevice *dev = (SDL_GPUDevice *)SDL_GetPointerProperty(
            props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
        const char *drv = dev ? SDL_GetGPUDeviceDriver(dev) : NULL;
        if (drv) snprintf(adapter, sizeof adapter, "SDL GPU on %s", drv);
    } else if (name && strcmp(name, "software") == 0) {
        snprintf(adapter, sizeof adapter, "CPU (no graphics adapter in use)");
    }
    if (adapter[0])
        snprintf(out, cap, "%s, %s", name ? name : "?", adapter);
    else
        snprintf(out, cap, "%s (this renderer does not name the graphics adapter)",
                 name ? name : "?");
}
