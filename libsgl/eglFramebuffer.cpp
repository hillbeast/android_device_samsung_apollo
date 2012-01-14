/*
 * libsgl/eglFramebuffer.cpp
 *
 * SAMSUNG S3C6410 FIMG-3DSE (PROPER) EGL IMPLEMENTATION
 *
 * Copyrights:	2010 by Tomasz Figa < tomasz.figa at gmail.com >
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty off
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "eglCommon.h"
#include "platform.h"
#include "fglrendersurface.h"
#include "fglimage.h"
#include "common.h"
#include "types.h"
#include "libfimg/fimg.h"
#include "fglsurface.h"

/*
 * Configurations available for direct rendering into frame buffer
 */

/*
 * In the lists below, attributes names MUST be sorted.
 * Additionally, all configs must be sorted according to
 * the EGL specification.
 */

/*
 * These configs can override the base attribute list
 * NOTE: when adding a config here, don't forget to update eglCreate*Surface()
 */

/* RGB 565 configs */
static const FGLConfigPair configAttributes0[] = {
	{ EGL_BUFFER_SIZE,     16 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        5 },
	{ EGL_GREEN_SIZE,       6 },
	{ EGL_RED_SIZE,         5 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_STENCIL_SIZE,     0 },
};

static const FGLConfigPair configAttributes1[] = {
	{ EGL_BUFFER_SIZE,     16 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        5 },
	{ EGL_GREEN_SIZE,       6 },
	{ EGL_RED_SIZE,         5 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_STENCIL_SIZE,     8 },
};

/* RGB 888 configs */
static const FGLConfigPair configAttributes2[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_STENCIL_SIZE,     0 },
};

static const FGLConfigPair configAttributes3[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       0 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_STENCIL_SIZE,     8 },
};

/* ARGB 8888 configs */
static const FGLConfigPair configAttributes4[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_STENCIL_SIZE,     0 },
};

static const FGLConfigPair configAttributes5[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_STENCIL_SIZE,     8 },
};

static const FGLConfigPair configAttributes6[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,       0 },
	{ EGL_STENCIL_SIZE,     0 },
};

static const FGLConfigPair configAttributes7[] = {
	{ EGL_BUFFER_SIZE,     32 },
	{ EGL_ALPHA_SIZE,       8 },
	{ EGL_BLUE_SIZE,        8 },
	{ EGL_GREEN_SIZE,       8 },
	{ EGL_RED_SIZE,         8 },
	{ EGL_DEPTH_SIZE,      24 },
	{ EGL_STENCIL_SIZE,     8 },
};

const FGLConfigs gPlatformConfigs[] = {
	{ configAttributes0, NELEM(configAttributes0) },
	{ configAttributes1, NELEM(configAttributes1) },
	{ configAttributes2, NELEM(configAttributes2) },
	{ configAttributes3, NELEM(configAttributes3) },
	{ configAttributes4, NELEM(configAttributes4) },
	{ configAttributes5, NELEM(configAttributes5) },
	{ configAttributes6, NELEM(configAttributes6) },
	{ configAttributes7, NELEM(configAttributes7) },
};

const int gPlatformConfigsNum = NELEM(gPlatformConfigs);

/*
 * Frame buffer operations
 */

class FGLFramebufferSurface : public FGLSurface {
public:
	FGLFramebufferSurface(unsigned long paddr,
					void *vaddr, unsigned long length) :
		FGLSurface(paddr, vaddr, length) {}
	virtual ~FGLFramebufferSurface() {}

	virtual void flush(void) {}

	virtual int lock(int usage = 0)
	{
		return 0;
	}

	virtual int unlock(void)
	{
		return 0;
	}

	virtual bool isValid(void)
	{
		return true;
	}
};

class FGLFramebufferManager {
	int _fd;
	int _count;
	int *_buffers;
	int _write;
	int _read;
	bool _empty;
public:
	FGLFramebufferManager(int fd, int bufCount, int yres);
	~FGLFramebufferManager();

	int put(unsigned int yoffset);
	int get(void);
};

FGLFramebufferManager::FGLFramebufferManager(int fd, int bufCount, int yres) :
	_fd(fd), _count(bufCount),
	_write(0), _read(0), _empty(false)
{
	int i, offset;

	_buffers = new int[bufCount];
	for (i = 0, offset = 0; i < bufCount; ++i, offset += yres)
		_buffers[i] = offset;
}

FGLFramebufferManager::~FGLFramebufferManager()
{
	delete[] _buffers;
}

#define FRAMEBUFFER_USE_VSYNC

int FGLFramebufferManager::put(unsigned int yoffset)
{
	if (!_empty && _read == _write)
		return -1;

	fb_var_screeninfo vinfo;

	if (ioctl(_fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
		LOGE("%s: FBIOGET_VSCREENINFO failed.", __func__);
	vinfo.yoffset = yoffset;
	if (ioctl(_fd, FBIOPAN_DISPLAY, &vinfo) < 0)
		LOGE("%s: FBIOPAN_DISPLAY failed.", __func__);

#ifdef FRAMEBUFFER_USE_VSYNC
	int crtc = 0;
	if (ioctl(_fd, FBIO_WAITFORVSYNC, &crtc) < 0)
		LOGW("%s: FBIO_WAITFORVSYNC failed.", __func__);
#endif

	_buffers[_write++] = yoffset;
	_write %= _count;
	_empty = false;

	return 0;
}

int FGLFramebufferManager::get(void)
{
	int ret;

	if (_empty)
		return -1;

	ret = _buffers[_read++];
	_read %= _count;

	if (_read == _write)
		_empty = true;

	return ret;
}

/*
 * Frame buffer window surface
 */

struct FGLWindowSurface : public FGLRenderSurface {
	FGLWindowSurface(EGLDisplay dpy, EGLConfig config,
			int32_t depthFormat, int fileDesc, int32_t pixelFormat);
	~FGLWindowSurface();

	virtual EGLBoolean swapBuffers();
	virtual EGLBoolean connect();
	virtual void disconnect();

	virtual bool initCheck() const
	{
		return vbase != NULL;
	}

	virtual EGLint getWidth() const
	{
		return width;
	}

	virtual EGLint getHeight() const
	{
		return height;
	}

	virtual EGLint getSwapBehavior() const
	{
		return EGL_BUFFER_DESTROYED;
	}

private:
	int	bytesPerPixel;
	int	fd;
	int	bufferCount;
	int	yoffset;

	unsigned long	pbase;
	void		*vbase;
	unsigned long	vlen;
	unsigned long	lineLength;

	FGLFramebufferManager *manager;
};

FGLWindowSurface::FGLWindowSurface(EGLDisplay dpy, EGLConfig config,
		int32_t depthFormat, int fileDesc, int32_t pixelFormat) :
	FGLRenderSurface(dpy, config, pixelFormat, depthFormat),
	bytesPerPixel(0), fd(fileDesc)
{
	fb_var_screeninfo vinfo;
	fb_fix_screeninfo finfo;

	ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);
	ioctl(fd, FBIOGET_FSCREENINFO, &finfo);

	stride		= vinfo.xres;
	width		= vinfo.xres;
	height		= vinfo.yres;
	bytesPerPixel	= vinfo.bits_per_pixel / 8;
	bufferCount	= 2;
	pbase		= finfo.smem_start;
	lineLength	= finfo.line_length;

	vinfo.yres_virtual = 2*vinfo.yres;
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo) < 0) {
		vinfo.yres_virtual = vinfo.yres;
		bufferCount = 1;
		LOGW("FBIOPUT_VSCREENINFO failed, page flipping not supported");
	}

	unsigned long fbSize = vinfo.yres_virtual * finfo.line_length;
	unsigned long pageSize = getpagesize();
	vlen = (fbSize + pageSize - 1) & ~(pageSize - 1);

	vbase = mmap(NULL, vlen, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (vbase == MAP_FAILED) {
		vbase = 0;
		LOGE("mmap failed");
		setError(EGL_BAD_ALLOC);
		return;
	}

	manager = new FGLFramebufferManager(fd, bufferCount, height); 
}

FGLWindowSurface::~FGLWindowSurface()
{
	if (vbase != NULL)
		munmap(vbase, vlen);
	delete manager;
	delete color;
	delete depth;
}

EGLBoolean FGLWindowSurface::connect()
{
	if (depthFormat) {
		unsigned int size = stride * height * 4;

		delete depth;
		depth = new FGLLocalSurface(size);
		if (!depth || !depth->isValid()) {
			setError(EGL_BAD_ALLOC);
			return EGL_FALSE;
		}
	}

	if (color) {
		manager->put(yoffset);
		delete color;
		color = 0;
	}

	yoffset = manager->get();
	if (yoffset < 0) {
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
	}

	color = new FGLFramebufferSurface(pbase + yoffset*lineLength,
			(char *)vbase + yoffset*lineLength, height*lineLength);
	if (!color->isValid()) {
		delete color;
		color = 0;
		manager->put(yoffset);
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
	}

	return EGL_TRUE;
}

void FGLWindowSurface::disconnect()
{
	if (color) {
		manager->put(yoffset);
		delete color;
		color = 0;
	}

	delete depth;
	depth = 0;
}

EGLBoolean FGLWindowSurface::swapBuffers()
{
	FGLSurface *newColor;
	int newYOffset;

	if (bufferCount < 2) {
		setError(EGL_BAD_ACCESS);
		return EGL_FALSE;
	}

	newYOffset = manager->get();
	if (newYOffset < 0) {
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
	}

	newColor = new FGLFramebufferSurface(pbase + newYOffset*lineLength,
			(char *)vbase + newYOffset*lineLength, height*lineLength);
	if (!newColor->isValid()) {
		delete newColor;
		newColor = 0;
		manager->put(newYOffset);
		setError(EGL_BAD_ALLOC);
		return EGL_FALSE;
	}

	if (color) {
		manager->put(yoffset);
		delete color;
	}

	color = newColor;
	yoffset = newYOffset;

	return EGL_TRUE;
}

FGLRenderSurface *platformCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
	int32_t depthFormat, EGLNativeWindowType window, int32_t pixelFormat)
{
	fb_var_screeninfo vinfo;
	int fd = (int)window;

	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		LOGE("%s: Invalid native window handle specified", __func__);
		setError(EGL_BAD_NATIVE_WINDOW);
		return NULL;
	}

	FGLPixelFormat fmt;
	fmt.bpp		= vinfo.bits_per_pixel;
	fmt.red		= vinfo.red.length;
	fmt.green	= vinfo.green.length;
	fmt.blue	= vinfo.blue.length;
	fmt.alpha	= vinfo.transp.length;

	if (fglEGLValidatePixelFormat(config, &fmt) != EGL_TRUE) {
		setError(EGL_BAD_MATCH);
		return NULL;
	}

	return new FGLWindowSurface(dpy, config, depthFormat, fd, pixelFormat);
}

#define EGLFunc	__eglMustCastToProperFunctionPointerType

const FGLExtensionMap gPlatformExtensionMap[] = {
	{ NULL, NULL },
};
