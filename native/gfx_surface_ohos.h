// nx2android HarmonyOS NEXT: Vulkan presentation on the XComponent surface (G0b first frame).
//
// The platform layer (napi_ohos.c) hands the XComponent's OHNativeWindow to dm_gfx_surface_set(); this
// file owns a small Vulkan presenter of its own so the first frame does not depend on the game core
// (dm/host, dm/gfx/nvn, dm/android/gfx_vk_* are not linked into the P0 host yet). It creates the
// VK_OHOS_surface, a swapchain and a render pass that clears the picture; once dm/android/gfx_vk_*
// is linked (G1) the same window goes to gfx_vk_present.c's __OHOS__ branch and this presenter backs
// off (DM_OHOS_GFX=0 or surface hand-off to the core).
//
// Threading: dm_gfx_surface_set() is called from the ArkTS/UI thread and starts or joins the render
// thread itself; it holds a native-window reference for that thread's lifetime. Detaching joins the
// thread before returning, so the caller may release the window afterwards.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Attach the presenter to an OHOS XComponent surface. window is the OHNativeWindow* of
// OH_NativeXComponent_Callback::OnSurfaceCreated/Changed; width and height are the component size in
// pixels. window == NULL (or width/height <= 0) detaches: the render thread stops and is joined, the
// swapchain and surface are destroyed. Calling it again with the same window and a new size
// recreates only the swapchain.
void dm_gfx_surface_set(void *window, int width, int height);

// Frames presented successfully so far (0 while detached); for logs and the boot overlay.
int dm_gfx_surface_frames(void);

// Detach and stop (same as dm_gfx_surface_set(NULL, 0, 0)); safe to call repeatedly.
void dm_gfx_surface_stop(void);

#ifdef __cplusplus
}
#endif
