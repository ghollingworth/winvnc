/*
 * DXGI Desktop Duplication screen capture implementation.
 *
 * Uses the IDXGIOutputDuplication interface to capture the desktop.
 * All COM calls must happen on the same thread.
 */

#ifdef _WIN32

#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxgi-capture.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct dxgi_capture {
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	IDXGIOutputDuplication* duplication;
	ID3D11Texture2D* staging;

	uint16_t width;
	uint16_t height;
	int32_t stride;

	uint8_t* pixels;       /* mapped staging texture data */
	bool frame_acquired;

	struct dxgi_rect* dirty_rects;
	int n_dirty;
	int dirty_capacity;

	/* Latest cursor shape from GetFramePointerShape, normalised to BGRA
	 * with a real alpha channel. Reallocated on shape change. */
	uint8_t* cursor_pixels;
	uint16_t cursor_width;
	uint16_t cursor_height;
	uint16_t cursor_hot_x;
	uint16_t cursor_hot_y;
	bool cursor_changed;
	bool cursor_have_shape;

	/* Scratch buffer for the raw shape bytes from DXGI before normalisation. */
	uint8_t* cursor_raw;
	UINT cursor_raw_capacity;

	/* Latest reported cursor position from DXGI. cursor_pos_changed flips
	 * when LastMouseUpdateTime advances. */
	int32_t cursor_pos_x;
	int32_t cursor_pos_y;
	bool cursor_pos_visible;
	bool cursor_pos_changed;
	bool cursor_pos_have;
};

static HRESULT create_staging_texture(struct dxgi_capture* self)
{
	D3D11_TEXTURE2D_DESC desc = {
		.Width = self->width,
		.Height = self->height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_STAGING,
		.CPUAccessFlags = D3D11_CPU_ACCESS_READ,
	};

	return ID3D11Device_CreateTexture2D(self->device, &desc, NULL,
			&self->staging);
}

struct dxgi_capture* dxgi_capture_new(void)
{
	struct dxgi_capture* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;

	/* Create D3D11 device */
	D3D_FEATURE_LEVEL feature_level;
	HRESULT hr = D3D11CreateDevice(
		NULL,
		D3D_DRIVER_TYPE_HARDWARE,
		NULL,
		0,
		NULL, 0,
		D3D11_SDK_VERSION,
		&self->device,
		&feature_level,
		&self->context
	);
	if (FAILED(hr))
		goto fail;

	/* Get DXGI device -> adapter -> output */
	IDXGIDevice* dxgi_device = NULL;
	hr = ID3D11Device_QueryInterface(self->device, &IID_IDXGIDevice,
			(void**)&dxgi_device);
	if (FAILED(hr))
		goto fail_device;

	IDXGIAdapter* adapter = NULL;
	hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
	IDXGIDevice_Release(dxgi_device);
	if (FAILED(hr))
		goto fail_device;

	IDXGIOutput* output = NULL;
	hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
	IDXGIAdapter_Release(adapter);
	if (FAILED(hr))
		goto fail_device;

	/* Get output dimensions */
	DXGI_OUTPUT_DESC output_desc;
	IDXGIOutput_GetDesc(output, &output_desc);
	self->width = (uint16_t)(output_desc.DesktopCoordinates.right -
			output_desc.DesktopCoordinates.left);
	self->height = (uint16_t)(output_desc.DesktopCoordinates.bottom -
			output_desc.DesktopCoordinates.top);

	/* Create output duplication */
	IDXGIOutput1* output1 = NULL;
	hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
			(void**)&output1);
	IDXGIOutput_Release(output);
	if (FAILED(hr))
		goto fail_device;

	hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown*)self->device,
			&self->duplication);
	IDXGIOutput1_Release(output1);
	if (FAILED(hr))
		goto fail_device;

	/* Create staging texture for CPU read */
	self->stride = self->width * 4;
	hr = create_staging_texture(self);
	if (FAILED(hr))
		goto fail_dup;

	self->dirty_capacity = 64;
	self->dirty_rects = malloc(sizeof(struct dxgi_rect) * self->dirty_capacity);

	return self;

fail_dup:
	IDXGIOutputDuplication_Release(self->duplication);
fail_device:
	if (self->context)
		ID3D11DeviceContext_Release(self->context);
	if (self->device)
		ID3D11Device_Release(self->device);
fail:
	free(self);
	return NULL;
}

void dxgi_capture_destroy(struct dxgi_capture* self)
{
	if (!self)
		return;

	if (self->frame_acquired)
		IDXGIOutputDuplication_ReleaseFrame(self->duplication);

	free(self->dirty_rects);
	free(self->cursor_pixels);
	free(self->cursor_raw);

	if (self->staging)
		ID3D11Texture2D_Release(self->staging);
	IDXGIOutputDuplication_Release(self->duplication);
	ID3D11DeviceContext_Release(self->context);
	ID3D11Device_Release(self->device);
	free(self);
}

void dxgi_capture_get_size(const struct dxgi_capture* self,
		uint16_t* width, uint16_t* height)
{
	*width = self->width;
	*height = self->height;
}

/* Decode a DXGI monochrome cursor (1bpp AND mask over 1bpp XOR mask, stacked
 * vertically) into BGRA. Width is in pixels; the buffer's row stride is in
 * bytes; total height is 2*output_height.
 *
 * RFB's cursor pseudo-encoding has no XOR concept, so "invert-screen" pixels
 * are rendered as opaque black with a one-pixel white halo where they border
 * transparent area. That makes the I-beam (which is almost entirely invert
 * pixels) readable on both light and dark backgrounds. */
static void decode_mono_cursor(uint8_t* dst, const uint8_t* src,
		UINT src_pitch, uint16_t width, uint16_t height)
{
	enum { CL_TRANSP = 0, CL_BLACK, CL_WHITE, CL_INVERT };

	uint8_t* cls = malloc((size_t)width * height);
	if (!cls)
		return;

	for (uint16_t y = 0; y < height; ++y) {
		const uint8_t* and_row = src + y * src_pitch;
		const uint8_t* xor_row = src + (height + y) * src_pitch;
		for (uint16_t x = 0; x < width; ++x) {
			uint8_t bit = 0x80 >> (x & 7);
			uint8_t a_bit = and_row[x >> 3] & bit;
			uint8_t x_bit = xor_row[x >> 3] & bit;
			uint8_t c;
			if (a_bit && !x_bit)       c = CL_TRANSP;
			else if (!a_bit && !x_bit) c = CL_BLACK;
			else if (!a_bit && x_bit)  c = CL_WHITE;
			else                       c = CL_INVERT;
			cls[(size_t)y * width + x] = c;
		}
	}

	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			uint8_t c = cls[(size_t)y * width + x];
			uint8_t r = 0, g = 0, b = 0, a = 0;
			switch (c) {
			case CL_BLACK:
			case CL_INVERT:
				a = 255;
				break;
			case CL_WHITE:
				r = g = b = a = 255;
				break;
			case CL_TRANSP: {
				/* Halo: if any 8-neighbour is INVERT, paint white. */
				int x0 = x > 0 ? x - 1 : x;
				int x1 = x + 1 < width ? x + 1 : x;
				int y0 = y > 0 ? y - 1 : y;
				int y1 = y + 1 < height ? y + 1 : y;
				bool halo = false;
				for (int yy = y0; yy <= y1 && !halo; ++yy)
					for (int xx = x0; xx <= x1 && !halo; ++xx)
						if (cls[(size_t)yy * width + xx]
								== CL_INVERT)
							halo = true;
				if (halo)
					r = g = b = a = 255;
				break;
			}
			}
			uint8_t* out = dst + ((size_t)y * width + x) * 4;
			out[0] = b;
			out[1] = g;
			out[2] = r;
			out[3] = a;
		}
	}

	free(cls);
}

/* Decode a DXGI masked-color cursor: BGRA pixels where the alpha byte is 0 →
 * direct copy, alpha byte = 0xFF → "invert screen" (rendered as opaque black). */
static void decode_masked_color_cursor(uint8_t* dst, const uint8_t* src,
		UINT src_pitch, uint16_t width, uint16_t height)
{
	for (uint16_t y = 0; y < height; ++y) {
		const uint8_t* in = src + y * src_pitch;
		uint8_t* out = dst + y * width * 4;
		for (uint16_t x = 0; x < width; ++x, in += 4, out += 4) {
			uint8_t b = in[0], g = in[1], r = in[2], a = in[3];
			if (a == 0) {
				out[0] = b; out[1] = g; out[2] = r; out[3] = 255;
			} else {
				/* invert — render as opaque black */
				out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 255;
			}
		}
	}
}

/* Decode a DXGI true-color cursor: BGRA with real alpha; just unpack rows. */
static void decode_color_cursor(uint8_t* dst, const uint8_t* src,
		UINT src_pitch, uint16_t width, uint16_t height)
{
	for (uint16_t y = 0; y < height; ++y) {
		memcpy(dst + y * width * 4, src + y * src_pitch, width * 4);
	}
}

/* Pull the latest pointer shape from the duplication object and decode it
 * into self->cursor_pixels. Sets self->cursor_changed if the shape changed. */
static void update_cursor_shape(struct dxgi_capture* self,
		const DXGI_OUTDUPL_FRAME_INFO* frame_info)
{
	UINT needed = frame_info->PointerShapeBufferSize;
	if (needed == 0)
		return; /* no shape update this frame */

	if (needed > self->cursor_raw_capacity) {
		uint8_t* p = realloc(self->cursor_raw, needed);
		if (!p)
			return;
		self->cursor_raw = p;
		self->cursor_raw_capacity = needed;
	}

	UINT got = 0;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO info = { 0 };
	HRESULT hr = IDXGIOutputDuplication_GetFramePointerShape(
			self->duplication, self->cursor_raw_capacity,
			self->cursor_raw, &got, &info);
	if (FAILED(hr) || got == 0)
		return;

	uint16_t w = (uint16_t)info.Width;
	uint16_t h = (uint16_t)info.Height;
	if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
		h /= 2; /* monochrome stacks AND on top of XOR */

	if (w == 0 || h == 0)
		return;

	size_t out_size = (size_t)w * h * 4;
	uint8_t* out = realloc(self->cursor_pixels, out_size);
	if (!out)
		return;
	self->cursor_pixels = out;

	switch (info.Type) {
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
		decode_mono_cursor(out, self->cursor_raw, info.Pitch, w, h);
		break;
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
		decode_masked_color_cursor(out, self->cursor_raw, info.Pitch, w, h);
		break;
	case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
	default:
		decode_color_cursor(out, self->cursor_raw, info.Pitch, w, h);
		break;
	}

	self->cursor_width = w;
	self->cursor_height = h;
	self->cursor_hot_x = (uint16_t)info.HotSpot.x;
	self->cursor_hot_y = (uint16_t)info.HotSpot.y;
	self->cursor_changed = true;
	self->cursor_have_shape = true;
}

int dxgi_capture_frame(struct dxgi_capture* self, uint32_t timeout_ms,
		const uint8_t** pixels, int32_t* stride,
		struct dxgi_rect** dirty_rects, int* n_dirty)
{
	if (self->frame_acquired) {
		ID3D11DeviceContext_Unmap(self->context,
				(ID3D11Resource*)self->staging, 0);
		IDXGIOutputDuplication_ReleaseFrame(self->duplication);
		self->frame_acquired = false;
	}

	DXGI_OUTDUPL_FRAME_INFO frame_info;
	IDXGIResource* resource = NULL;

	HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(
			self->duplication, timeout_ms,
			&frame_info, &resource);

	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return -1; /* no new frame */

	if (FAILED(hr))
		return -2; /* error — need recreation */

	self->frame_acquired = true;

	/* Pick up any cursor shape change that came with this frame. */
	update_cursor_shape(self, &frame_info);

	/* And the cursor position. LastMouseUpdateTime is non-zero whenever
	 * DXGI delivers a fresh position. */
	if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
		self->cursor_pos_x = frame_info.PointerPosition.Position.x;
		self->cursor_pos_y = frame_info.PointerPosition.Position.y;
		self->cursor_pos_visible = !!frame_info.PointerPosition.Visible;
		self->cursor_pos_changed = true;
		self->cursor_pos_have = true;
	}

	/* Get the desktop texture */
	ID3D11Texture2D* desktop_tex = NULL;
	hr = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D,
			(void**)&desktop_tex);
	IDXGIResource_Release(resource);
	if (FAILED(hr))
		return -2;

	/* Copy to staging texture */
	ID3D11DeviceContext_CopyResource(self->context,
			(ID3D11Resource*)self->staging,
			(ID3D11Resource*)desktop_tex);
	ID3D11Texture2D_Release(desktop_tex);

	/* Map staging texture for CPU read */
	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = ID3D11DeviceContext_Map(self->context,
			(ID3D11Resource*)self->staging, 0,
			D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr))
		return -2;

	*pixels = (const uint8_t*)mapped.pData;
	*stride = (int32_t)mapped.RowPitch;

	/* Do NOT unmap here — the caller needs the pixel pointer to remain
	 * valid. It will be unmapped in dxgi_capture_release_frame() or
	 * at the start of the next dxgi_capture_frame() call.
	 */

	/* Extract dirty rects from the frame metadata */
	self->n_dirty = 0;
	if (frame_info.TotalMetadataBufferSize > 0) {
		UINT buf_size = frame_info.TotalMetadataBufferSize;
		BYTE* meta_buf = (BYTE*)_alloca(buf_size);

		/* Get moved rects (we ignore them for now, treat as dirty) */
		UINT move_size = buf_size;
		hr = IDXGIOutputDuplication_GetFrameMoveRects(
				self->duplication, move_size,
				(DXGI_OUTDUPL_MOVE_RECT*)meta_buf, &move_size);

		/* Get dirty rects */
		UINT dirty_size = buf_size;
		RECT* rects = (RECT*)meta_buf;
		hr = IDXGIOutputDuplication_GetFrameDirtyRects(
				self->duplication, dirty_size,
				rects, &dirty_size);

		if (SUCCEEDED(hr) && dirty_size > 0) {
			int count = dirty_size / sizeof(RECT);
			if (count > self->dirty_capacity) {
				self->dirty_capacity = count;
				self->dirty_rects = realloc(self->dirty_rects,
						sizeof(struct dxgi_rect) * count);
			}
			for (int i = 0; i < count; i++) {
				self->dirty_rects[i].x = rects[i].left;
				self->dirty_rects[i].y = rects[i].top;
				self->dirty_rects[i].width =
					rects[i].right - rects[i].left;
				self->dirty_rects[i].height =
					rects[i].bottom - rects[i].top;
			}
			self->n_dirty = count;
		}
	}

	if (dirty_rects)
		*dirty_rects = self->dirty_rects;
	if (n_dirty)
		*n_dirty = self->n_dirty;

	return 0;
}

void dxgi_capture_release_frame(struct dxgi_capture* self)
{
	if (self->frame_acquired) {
		ID3D11DeviceContext_Unmap(self->context,
				(ID3D11Resource*)self->staging, 0);
		IDXGIOutputDuplication_ReleaseFrame(self->duplication);
		self->frame_acquired = false;
	}
}

int dxgi_capture_get_cursor(struct dxgi_capture* self,
		const uint8_t** pixels, uint16_t* width, uint16_t* height,
		uint16_t* hot_x, uint16_t* hot_y)
{
	if (!self->cursor_have_shape)
		return -1;

	*pixels = self->cursor_pixels;
	*width = self->cursor_width;
	*height = self->cursor_height;
	*hot_x = self->cursor_hot_x;
	*hot_y = self->cursor_hot_y;

	int changed = self->cursor_changed ? 1 : 0;
	self->cursor_changed = false;
	return changed;
}

int dxgi_capture_get_cursor_pos(struct dxgi_capture* self,
		int32_t* x, int32_t* y, bool* visible)
{
	if (!self->cursor_pos_have)
		return -1;

	*x = self->cursor_pos_x;
	*y = self->cursor_pos_y;
	*visible = self->cursor_pos_visible;

	int changed = self->cursor_pos_changed ? 1 : 0;
	self->cursor_pos_changed = false;
	return changed;
}

#endif /* _WIN32 */
