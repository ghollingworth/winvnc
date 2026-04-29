/*
 * DXGI Desktop Duplication screen capture for Windows.
 *
 * Captures the primary display using the DXGI Output Duplication API
 * (available since Windows 8). Provides:
 * - Hardware-accelerated screen capture
 * - Dirty rectangle tracking from the compositor
 * - Moved rectangle tracking (CopyRect optimisation)
 * - Mouse cursor compositing
 *
 * The captured frame is in BGRA format (DXGI_FORMAT_B8G8R8A8_UNORM),
 * matching DRM_FORMAT_BGRX8888.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct dxgi_capture;

/* A rectangle. */
struct dxgi_rect {
	int32_t x, y, width, height;
};

/* Initialise DXGI capture for the primary display.
 * Returns NULL on failure.
 * Must be called from the thread that will call dxgi_capture_frame().
 */
struct dxgi_capture* dxgi_capture_new(void);

/* Release all DXGI resources. */
void dxgi_capture_destroy(struct dxgi_capture* self);

/* Get the screen dimensions. */
void dxgi_capture_get_size(const struct dxgi_capture* self,
		uint16_t* width, uint16_t* height);

/* Acquire a new frame.
 *
 * timeout_ms: how long to wait for a new frame (0 = don't wait).
 *
 * On success, returns 0 and fills:
 *   - pixels: pointer to BGRA pixel data (valid until next call or destroy)
 *   - stride: bytes per row
 *   - dirty_rects: array of changed rectangles (may be NULL if whole screen)
 *   - n_dirty: number of dirty rects (0 = whole screen changed, or use
 *              previous frame's data)
 *
 * Returns -1 if no new frame is available within the timeout.
 * Returns -2 on error (display mode change, GPU reset — caller should
 *          recreate the capture).
 */
int dxgi_capture_frame(struct dxgi_capture* self, uint32_t timeout_ms,
		const uint8_t** pixels, int32_t* stride,
		struct dxgi_rect** dirty_rects, int* n_dirty);

/* Release the current frame. Must be called before acquiring the next. */
void dxgi_capture_release_frame(struct dxgi_capture* self);

/* Get the latest mouse cursor shape, normalised to BGRA with a real alpha
 * channel.
 *
 * Returns 1 if a new shape arrived since the last call (callers should push
 * it to the VNC server). Returns 0 if there's a shape but it hasn't changed.
 * Returns -1 if no shape has ever been seen.
 *
 * On a return of 0 or 1, the out-parameters describe the current shape. The
 * pixels pointer remains valid until the next dxgi_capture_frame() call or
 * dxgi_capture_destroy().
 */
int dxgi_capture_get_cursor(struct dxgi_capture* self,
		const uint8_t** pixels, uint16_t* width, uint16_t* height,
		uint16_t* hot_x, uint16_t* hot_y);

/* Get the latest mouse cursor position reported by DXGI.
 *
 * Returns 1 if the position has changed since the last call, 0 if cached
 * position is unchanged, -1 if no position has ever been reported.
 *
 * x, y are in primary-display pixel coordinates. visible indicates whether
 * the cursor is being drawn at all.
 */
int dxgi_capture_get_cursor_pos(struct dxgi_capture* self,
		int32_t* x, int32_t* y, bool* visible);
