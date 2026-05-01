/*
 * winvnc — a Windows VNC server using neatvnc and DXGI Desktop Duplication.
 *
 * Captures the screen via DXGI, feeds frames into neatvnc (which handles
 * the RFB protocol, Tight/JPEG encoding, and Continuous Updates), and
 * injects mouse/keyboard input via SendInput.
 *
 * Modes:
 *   Default:  Listens on 127.0.0.1:5900 (localhost only, no authentication).
 *   --stdio:  Bridges VNC protocol to stdin/stdout for embedding in a
 *             parent process (e.g. Pi Connect daemon). No TCP port exposed.
 *
 * Usage: winvnc [-p port] [-a address] [--stdio]
 */

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <windows.h>
#include <dbghelp.h>

#include <aml.h>
#include <neatvnc.h>
#include <pixman.h>

#include "dxgi-capture.h"
#include "win-input.h"

static struct dxgi_capture* capture;
static struct nvnc* server;
static struct nvnc_display* display;
static struct nvnc_frame_pool* fb_pool;
static struct aml* aml_loop;
static struct aml_ticker* capture_ticker;

static uint16_t screen_width;
static uint16_t screen_height;

/* stdio bridge state */
static int stdio_mode = 0;
static SOCKET bridge_sock = INVALID_SOCKET;
static HANDLE stdin_handle;
static HANDLE stdout_handle;
static volatile int bridge_running = 1;

/* neatvnc uses DRM fourcc. DXGI gives us DXGI_FORMAT_B8G8R8A8_UNORM which
 * stores bytes as [B,G,R,A] in memory. On little-endian this is the 32-bit
 * value 0xAARRGGBB, which is DRM_FORMAT_ARGB8888 (or XRGB8888 ignoring alpha).
 */
#define CAPTURE_FORMAT DRM_FORMAT_XRGB8888

/* neatvnc's default logger writes INFO/DEBUG/TRACE to stdout, but in
 * stdio mode stdout is the RFB byte stream. Force everything to stderr. */
static void stderr_logger(const struct nvnc_log_data* meta, const char* message)
{
	(void)meta;
	fprintf(stderr, "%s\n", message);
	fflush(stderr);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info)
{
	fprintf(stderr, "\n*** winvnc CRASH: exception 0x%08lx at %p\n",
			info->ExceptionRecord->ExceptionCode,
			info->ExceptionRecord->ExceptionAddress);
	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}

static void on_client_cleanup(void* userdata)
{
	(void)userdata;
	fprintf(stderr, "[winvnc] client disconnected\n");
	fflush(stderr);
	if (stdio_mode) {
		/* In stdio mode, when the client disconnects we should exit
		 * since the parent process will launch us again for the next
		 * session. */
		bridge_running = 0;
		aml_exit(aml_loop);
	}
}

static void on_client_new(struct nvnc_client* client)
{
	fprintf(stderr, "[winvnc] client connected\n");
	fflush(stderr);
	nvnc_client_set_userdata(client, NULL, on_client_cleanup);
}

static void on_pointer_event(struct nvnc_client* client, double x,
		double y, enum nvnc_button_mask button_mask)
{
	(void)client;
	win_input_mouse(x, y, (uint8_t)button_mask);
}

/* Reject client-driven desktop resize requests.
 *
 * noVNC sends SetDesktopSize with the viewport dimensions whenever
 * `resizeSession` is enabled (Pi Connect sets it). If we accept and apply
 * the requested width/height as logical_size, neatvnc anisotropically
 * scales the source 1920x1080 into the request box, which stretches the
 * image when the viewport's aspect ratio doesn't match the screen's.
 *
 * Returning false makes neatvnc reply RFB_RESIZE_STATUS_PROHIBITED, and
 * noVNC's `scaleViewport` (also enabled by Pi Connect) takes over with
 * client-side aspect-preserving fit — i.e. letterboxing. */
static bool on_desktop_layout(struct nvnc_client* client,
		const struct nvnc_desktop_layout* layout)
{
	(void)client;
	(void)layout;
	return false;
}

static void on_key_event(struct nvnc_client* client, uint32_t keysym,
		bool is_pressed)
{
	(void)client;
	win_input_keyboard(keysym, is_pressed);
}

static void on_capture_tick(struct aml_ticker* ticker)
{
	(void)ticker;

	const uint8_t* pixels = NULL;
	int32_t stride = 0;
	struct dxgi_rect* dirty_rects = NULL;
	int n_dirty = 0;
	static int frame_count = 0;

	int rc = dxgi_capture_frame(capture, 0, &pixels, &stride,
			&dirty_rects, &n_dirty);
	if (rc != 0)
		return; /* no new frame or error */

	frame_count++;

	/* Acquire a frame buffer from the pool */
	struct nvnc_frame* fb = nvnc_frame_pool_acquire(fb_pool);
	if (!fb) {
		dxgi_capture_release_frame(capture);
		return;
	}

	/* Copy pixels into the neatvnc frame buffer */
	uint8_t* dst = nvnc_frame_get_addr(fb);
	int32_t dst_stride = nvnc_frame_get_stride(fb);

	/* nvnc stride is in pixels; DXGI stride is in bytes */
	int32_t dst_bytes = dst_stride * 4;
	int32_t row_bytes = screen_width * 4;

	if (stride == dst_bytes) {
		memcpy(dst, pixels, (size_t)stride * screen_height);
	} else {
		for (int y = 0; y < screen_height; y++) {
			memcpy(dst + y * dst_bytes,
			       pixels + y * stride,
			       row_bytes);
		}
	}

	/* Composite the cursor into the framebuffer at the DXGI-reported
	 * position. Pi Connect's noVNC build doesn't advertise the
	 * Cursor-Position pseudo-encoding, so the only way to make the
	 * client see server-side cursor moves is to paint the cursor into
	 * the desktop image itself. */
	const uint8_t* cur_pixels = NULL;
	uint16_t cur_w = 0, cur_h = 0, cur_hx = 0, cur_hy = 0;
	int cur_rc = dxgi_capture_get_cursor(capture, &cur_pixels,
			&cur_w, &cur_h, &cur_hx, &cur_hy);
	int32_t pos_x = 0, pos_y = 0;
	bool pos_visible = false;
	int pos_rc = dxgi_capture_get_cursor_pos(capture, &pos_x, &pos_y,
			&pos_visible);

	static int prev_paint_x, prev_paint_y, prev_paint_w, prev_paint_h;
	int paint_x = 0, paint_y = 0, paint_w = 0, paint_h = 0;

	if (cur_rc >= 0 && cur_pixels && pos_rc >= 0 && pos_visible) {
		int top_x = (int)pos_x - (int)cur_hx;
		int top_y = (int)pos_y - (int)cur_hy;
		int x0 = top_x > 0 ? top_x : 0;
		int y0 = top_y > 0 ? top_y : 0;
		int x1 = top_x + (int)cur_w;
		int y1 = top_y + (int)cur_h;
		if (x1 > screen_width)  x1 = screen_width;
		if (y1 > screen_height) y1 = screen_height;

		for (int y = y0; y < y1; ++y) {
			const uint8_t* sp = cur_pixels +
				((y - top_y) * cur_w + (x0 - top_x)) * 4;
			uint8_t* dp = dst + y * dst_bytes + x0 * 4;
			for (int x = x0; x < x1; ++x, sp += 4, dp += 4) {
				uint8_t a = sp[3];
				if (a == 0)
					continue;
				if (a == 255) {
					dp[0] = sp[0];
					dp[1] = sp[1];
					dp[2] = sp[2];
				} else {
					uint8_t na = 255 - a;
					dp[0] = (sp[0] * a + dp[0] * na) / 255;
					dp[1] = (sp[1] * a + dp[1] * na) / 255;
					dp[2] = (sp[2] * a + dp[2] * na) / 255;
				}
			}
		}

		paint_x = x0;
		paint_y = y0;
		paint_w = x1 - x0;
		paint_h = y1 - y0;
	}

	/* Build damage region: DXGI dirty rects + cursor area (old & new) so
	 * the cursor's previous trail gets cleared even if DXGI didn't flag
	 * that area as dirty. */
	struct pixman_region16 damage;
	if (n_dirty > 0) {
		pixman_region_init(&damage);
		for (int i = 0; i < n_dirty; i++) {
			pixman_region_union_rect(&damage, &damage,
					dirty_rects[i].x,
					dirty_rects[i].y,
					dirty_rects[i].width,
					dirty_rects[i].height);
		}
	} else {
		pixman_region_init_rect(&damage, 0, 0,
				screen_width, screen_height);
	}
	if (paint_w > 0 && paint_h > 0)
		pixman_region_union_rect(&damage, &damage,
				paint_x, paint_y, paint_w, paint_h);
	if (prev_paint_w > 0 && prev_paint_h > 0)
		pixman_region_union_rect(&damage, &damage,
				prev_paint_x, prev_paint_y,
				prev_paint_w, prev_paint_h);
	prev_paint_x = paint_x;
	prev_paint_y = paint_y;
	prev_paint_w = paint_w;
	prev_paint_h = paint_h;

	nvnc_frame_set_damage(fb, &damage);
	pixman_region_fini(&damage);

	nvnc_display_feed_frame(display, fb);
	nvnc_frame_unref(fb);

	/* Cursor-Position pseudo-encoding (-232). Pi Connect's noVNC doesn't
	 * advertise it, so this is a no-op for them — but it costs us nothing
	 * and any spec-compliant client that requests it gets a real position
	 * stream. The above compositing is the actual fix for noVNC. */
	if (pos_rc == 1 && pos_visible &&
			pos_x >= 0 && pos_y >= 0 &&
			pos_x < screen_width && pos_y < screen_height) {
		nvnc_set_cursor_position(server, (uint16_t)pos_x,
				(uint16_t)pos_y);
	}

	dxgi_capture_release_frame(capture);
}

/* --- stdio bridge ---
 * Two threads copy data between the bridge socket (connected to neatvnc)
 * and stdin/stdout (connected to the parent process).
 */

static DWORD WINAPI bridge_stdin_to_sock(LPVOID arg)
{
	(void)arg;
	char buf[16384];
	uint64_t total = 0;
	while (bridge_running) {
		DWORD n;
		if (!ReadFile(stdin_handle, buf, sizeof(buf), &n, NULL) || n == 0) {
			fprintf(stderr, "[winvnc] bridge: stdin EOF (read %llu bytes total)\n",
					(unsigned long long)total);
			break;
		}
		int sent = 0;
		while (sent < (int)n) {
			int rc = send(bridge_sock, buf + sent, n - sent, 0);
			if (rc <= 0) {
				fprintf(stderr, "[winvnc] bridge: send failed (err=%d)\n", WSAGetLastError());
				goto done;
			}
			sent += rc;
		}
		total += n;
	}
done:
	bridge_running = 0;
	/* Only shut down the send side so neatvnc can still send us any
	 * buffered data (e.g. the RFB greeting) before we exit. */
	shutdown(bridge_sock, SD_SEND);
	return 0;
}

static DWORD WINAPI bridge_sock_to_stdout(LPVOID arg)
{
	(void)arg;
	char buf[16384];
	uint64_t total = 0;
	while (bridge_running) {
		int n = recv(bridge_sock, buf, sizeof(buf), 0);
		if (n < 0) {
			fprintf(stderr, "[winvnc] bridge: recv failed (err=%d)\n", WSAGetLastError());
			break;
		}
		if (n == 0) {
			fprintf(stderr, "[winvnc] bridge: recv EOF (got %llu bytes total)\n",
					(unsigned long long)total);
			break;
		}
		if (total == 0) {
			fprintf(stderr, "[winvnc] bridge: first %d bytes from neatvnc: %.*s\n",
					n < 32 ? n : 32, n < 32 ? n : 32, buf);
		}
		DWORD written = 0;
		while (written < (DWORD)n) {
			DWORD w;
			if (!WriteFile(stdout_handle, buf + written,
				       n - written, &w, NULL) || w == 0) {
				fprintf(stderr, "[winvnc] bridge: WriteFile to stdout failed (err=%lu, wrote %lu/%lu)\n",
						GetLastError(), written, (unsigned long)n);
				goto done;
			}
			written += w;
		}
		total += n;
	}
done:
	bridge_running = 0;
	shutdown(bridge_sock, SD_BOTH);
	return 0;
}

/* Connect to neatvnc's listen port and start bridging to stdio. */
static int start_stdio_bridge(uint16_t port)
{
	stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
	stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);

	if (stdin_handle == INVALID_HANDLE_VALUE ||
	    stdout_handle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "[winvnc] Cannot get stdio handles\n");
		return -1;
	}

	/* Connect to neatvnc's internal listen socket */
	bridge_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (bridge_sock == INVALID_SOCKET)
		return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	if (connect(bridge_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "[winvnc] Failed to connect to internal VNC port %d\n", port);
		closesocket(bridge_sock);
		return -1;
	}

	fprintf(stderr, "[winvnc] stdio bridge connected to internal port %d\n", port);

	/* Start bridge threads */
	CreateThread(NULL, 0, bridge_stdin_to_sock, NULL, 0, NULL);
	CreateThread(NULL, 0, bridge_sock_to_stdout, NULL, 0, NULL);

	return 0;
}

/* Bind to a random port and return the actual port number. */
static uint16_t bind_random_port(void)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
		return 0;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(s);
		return 0;
	}

	int addrlen = sizeof(addr);
	getsockname(s, (struct sockaddr*)&addr, &addrlen);
	uint16_t port = ntohs(addr.sin_port);
	closesocket(s);
	return port;
}

static void usage(const char* prog)
{
	fprintf(stderr, "Usage: %s [-p port] [-a address] [--stdio]\n", prog);
	fprintf(stderr, "  -p port     Listen port (default: 5900)\n");
	fprintf(stderr, "  -a address  Listen address (default: 127.0.0.1)\n");
	fprintf(stderr, "  --stdio     Bridge VNC to stdin/stdout (no TCP port exposed)\n");
}

int main(int argc, char* argv[])
{
	const char* address = "127.0.0.1";
	uint16_t port = 5900;

	/* Make sure we see any prints before a crash. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	SetUnhandledExceptionFilter(crash_handler);

	/* Redirect neatvnc logs to stderr so they don't corrupt the RFB
	 * protocol stream on stdout in --stdio mode. */
	nvnc_set_log_fn(stderr_logger);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			port = (uint16_t)atoi(argv[++i]);
		} else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
			address = argv[++i];
		} else if (strcmp(argv[i], "--stdio") == 0) {
			stdio_mode = 1;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	/* Initialise DXGI capture */
	capture = dxgi_capture_new();
	if (!capture) {
		fprintf(stderr, "Failed to initialise DXGI screen capture\n");
		return 1;
	}

	dxgi_capture_get_size(capture, &screen_width, &screen_height);
	fprintf(stderr, "[winvnc] Screen: %dx%d\n", screen_width, screen_height);

	/* Initialise event loop */
	aml_loop = aml_new();
	if (!aml_loop) {
		fprintf(stderr, "Failed to create event loop\n");
		return 1;
	}
	aml_set_default(aml_loop);

	if (stdio_mode) {
		/* In stdio mode, pick a random ephemeral port for the
		 * internal neatvnc listener. We'll connect to it ourselves
		 * and bridge to stdin/stdout. No external access.
		 */
		port = bind_random_port();
		if (port == 0) {
			fprintf(stderr, "Failed to find free port\n");
			return 1;
		}
		address = "127.0.0.1";
	}

	/* Create neatvnc server */
	server = nvnc_new();
	if (!server) {
		fprintf(stderr, "Failed to create VNC server\n");
		return 1;
	}

	if (nvnc_listen_tcp(server, address, port, NVNC_STREAM_NORMAL) < 0) {
		fprintf(stderr, "Failed to listen on %s:%d\n", address, port);
		return 1;
	}

	if (!stdio_mode) {
		/* Only print to stdout in non-stdio mode (stdout is the VNC
		 * stream in stdio mode). */
		printf("VNC server listening on %s:%d\n", address, port);
		fflush(stdout);
	} else {
		fprintf(stderr, "[winvnc] Internal VNC on port %d (stdio mode)\n", port);
	}

	/* Create display */
	display = nvnc_display_new(0, 0);
	nvnc_add_display(server, display);
	nvnc_set_name(server, "Windows Desktop");

	/* Set up input callbacks. The normalised pointer callback gives us
	 * coords in [0,1) regardless of any client-driven desktop resize,
	 * so SendInput hits the right Windows pixel either way. */
	nvnc_set_normalised_pointer_fn(server, on_pointer_event);
	nvnc_set_key_fn(server, on_key_event);
	nvnc_set_new_client_fn(server, on_client_new);
	nvnc_set_desktop_layout_fn(server, on_desktop_layout);

	/* Send a fully-transparent cursor via the RFB cursor pseudo-encoding.
	 * The cursor is composited into the framebuffer (see on_capture_tick),
	 * so the pseudo-encoding shape itself is invisible. With Pi Connect's
	 * noVNC build (showDotCursor=true), this transparent cursor triggers
	 * the dot-cursor fallback so the user gets a small marker at their
	 * local mouse position alongside the composited server cursor — the
	 * same UX they get from wayvnc on a Pi. */
	{
		struct nvnc_frame* tcur = nvnc_frame_new(1, 1,
				DRM_FORMAT_ARGB8888, 1);
		if (tcur) {
			memset(nvnc_frame_get_addr(tcur), 0, 4);
			nvnc_set_cursor(server, tcur, 0, 0, true);
			nvnc_frame_unref(tcur);
		}
	}

	/* Create frame pool — stride is in pixels, not bytes */
	fb_pool = nvnc_frame_pool_new(screen_width, screen_height,
			CAPTURE_FORMAT, screen_width);
	if (!fb_pool) {
		fprintf(stderr, "Failed to create frame pool\n");
		return 1;
	}

	/* Feed an initial blank frame so neatvnc has a display buffer before
	 * any client connects. Without this, the first client is rejected
	 * with "No display buffer has been set" — critical in stdio mode
	 * where we self-connect immediately. The capture ticker will replace
	 * it with real content on its first tick. */
	fprintf(stderr, "[winvnc] feeding initial blank frame\n");
	{
		struct nvnc_frame* fb = nvnc_frame_pool_acquire(fb_pool);
		if (fb) {
			uint8_t* dst = nvnc_frame_get_addr(fb);
			/* Zero = black BGRX */
			memset(dst, 0, (size_t)screen_width * screen_height * 4);

			struct pixman_region16 damage;
			pixman_region_init_rect(&damage, 0, 0,
					screen_width, screen_height);
			nvnc_frame_set_damage(fb, &damage);
			pixman_region_fini(&damage);
			nvnc_display_feed_frame(display, fb);
			nvnc_frame_unref(fb);
			fprintf(stderr, "[winvnc] initial blank frame fed\n");
		} else {
			fprintf(stderr, "[winvnc] failed to acquire initial frame\n");
		}
	}

	/* Start capture ticker — capture at ~30fps.
	 * This matches neatvnc's bandwidth estimator max_delay of 33ms.
	 * DXGI only provides a new frame when the screen changes, so most
	 * ticks are no-ops when the screen is idle.
	 */
	capture_ticker = aml_ticker_new(33333, on_capture_tick, NULL, NULL);
	aml_start(aml_loop, capture_ticker);

	/* In stdio mode, connect to our own server and bridge to stdin/stdout */
	if (stdio_mode) {
		if (start_stdio_bridge(port) < 0) {
			fprintf(stderr, "Failed to start stdio bridge\n");
			return 1;
		}
	}

	/* Run the event loop */
	aml_run(aml_loop);
	fprintf(stderr, "[winvnc] exiting\n");

	/* Cleanup */
	aml_unref(capture_ticker);
	nvnc_frame_pool_unref(fb_pool);
	nvnc_display_unref(display);
	nvnc_del(server);
	aml_unref(aml_loop);
	dxgi_capture_destroy(capture);

	return 0;
}

#else /* !_WIN32 */

#include <stdio.h>

int main(void)
{
	printf("winvnc is Windows-only\n");
	return 1;
}

#endif
