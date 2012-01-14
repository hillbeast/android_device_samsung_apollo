/*
 * libsgl/eglBase.cpp
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

#include "platform.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include "eglCommon.h"
#include "fglrendersurface.h"
#include "common.h"
#include "types.h"
#include "state.h"
#include "libfimg/fimg.h"
#include "fglsurface.h"
#include "glesFramebuffer.h"

#define FGL_EGL_MAJOR		1
#define FGL_EGL_MINOR		4

static const char *const gVendorString     = "OpenFIMG";
static const char *const gVersionString    = "1.4 pre-alpha";
static const char *const gClientApisString = "OpenGL_ES";
static const char *const gExtensionsString = "" PLATFORM_EXTENSIONS_STRING;

#ifndef PLATFORM_HAS_FAST_TLS
pthread_key_t eglContextKey = -1;
#endif

/*
 * Display
 */

struct FGLDisplay {
	EGLBoolean initialized;
	pthread_mutex_t lock;

	FGLDisplay() : initialized(0)
	{
		pthread_mutex_init(&lock, NULL);
	};
};

#define FGL_MAX_DISPLAYS	1
static FGLDisplay displays[FGL_MAX_DISPLAYS];

static inline EGLBoolean isDisplayValid(EGLDisplay dpy)
{
	EGLint disp = (EGLint)dpy;

	if(likely(disp == 1))
		return EGL_TRUE;

	return EGL_FALSE;
}

EGLBoolean fglEGLValidateDisplay(EGLDisplay dpy)
{
	return isDisplayValid(dpy);
}

static inline FGLDisplay *getDisplay(EGLDisplay dpy)
{
	EGLint disp = (EGLint)dpy;

	return &displays[disp - 1];
}

static inline EGLBoolean isDisplayInitialized(EGLDisplay dpy)
{
	EGLBoolean ret;
	FGLDisplay *disp = getDisplay(dpy);

	pthread_mutex_lock(&disp->lock);
	ret = disp->initialized;
	pthread_mutex_unlock(&disp->lock);

	return ret;
}

static pthread_mutex_t eglErrorKeyMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t eglErrorKey = (pthread_key_t)-1;

/*
 * Error handling
 */

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
	if(unlikely(eglErrorKey == (pthread_key_t)-1))
		return EGL_SUCCESS;

	EGLint error = (EGLint)pthread_getspecific(eglErrorKey);
	pthread_setspecific(eglErrorKey, (void *)EGL_SUCCESS);
	return error;
}

void fglEGLSetError(EGLint error)
{
	if(unlikely(eglErrorKey == (pthread_key_t)-1)) {
		pthread_mutex_lock(&eglErrorKeyMutex);
		if(eglErrorKey == (pthread_key_t)-1)
			pthread_key_create(&eglErrorKey, NULL);
		pthread_mutex_unlock(&eglErrorKeyMutex);
	}

	pthread_setspecific(eglErrorKey, (void *)error);
}

/*
 * Initialization
 */

EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType display_id)
{
	if(display_id != EGL_DEFAULT_DISPLAY)
		return EGL_NO_DISPLAY;

	return (EGLDisplay)1;
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
	EGLBoolean ret = EGL_TRUE;

	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if(major != NULL)
		*major = FGL_EGL_MAJOR;

	if(minor != NULL)
		*minor = FGL_EGL_MINOR;

	FGLDisplay *disp = getDisplay(dpy);

	pthread_mutex_lock(&disp->lock);

	if(likely(disp->initialized))
		goto finish;

#ifndef PLATFORM_HAS_FAST_TLS
	pthread_key_create(&eglContextKey, NULL);
#endif

	disp->initialized = EGL_TRUE;

finish:
	pthread_mutex_unlock(&disp->lock);
	return ret;
}

/*
 * FIXME:
 * Keep a list of all allocated contexts and surfaces and delete them
 * or mark for deletion here.
 */
EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy)
{
	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLDisplay *disp = getDisplay(dpy);

	pthread_mutex_lock(&disp->lock);

	if(unlikely(!disp->initialized))
		goto finish;

	disp->initialized = EGL_FALSE;

finish:
	pthread_mutex_unlock(&disp->lock);
	return EGL_TRUE;
}

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
	if(!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return NULL;
	}

	if(!isDisplayInitialized(dpy)) {
		setError(EGL_NOT_INITIALIZED);
		return NULL;
	}

	switch(name) {
	case EGL_CLIENT_APIS:
		return gClientApisString;
	case EGL_EXTENSIONS:
		return gExtensionsString;
	case EGL_VENDOR:
		return gVendorString;
	case EGL_VERSION:
		return gVersionString;
	}

	setError(EGL_BAD_PARAMETER);
	return NULL;
}

/*
 * Configurations
 */

struct FGLConfigMatcher {
	GLint key;
	bool (*match)(GLint reqValue, GLint confValue);

	static bool atLeast(GLint reqValue, GLint confValue)
	{
		return (reqValue == EGL_DONT_CARE) || (confValue >= reqValue);
	}
	static bool exact(GLint reqValue, GLint confValue)
	{
		return (reqValue == EGL_DONT_CARE) || (confValue == reqValue);
	}
	static bool mask(GLint reqValue, GLint confValue)
	{
		return (confValue & reqValue) == reqValue;
	}
	static bool ignore(GLint reqValue, GLint confValue)
	{
		return true;
	}
};

/*
* In the lists below, attributes names MUST be sorted.
* Additionally, all configs must be sorted according to
* the EGL specification.
*/

#define FGL_MAX_VIEWPORT_PIXELS \
				(FGL_MAX_VIEWPORT_DIMS*FGL_MAX_VIEWPORT_DIMS)

static const FGLConfigPair baseConfigAttributes[] = {
	{ EGL_CONFIG_CAVEAT,              EGL_NONE                          },
	{ EGL_LEVEL,                      0                                 },
	{ EGL_MAX_PBUFFER_HEIGHT,         FGL_MAX_VIEWPORT_DIMS             },
	{ EGL_MAX_PBUFFER_PIXELS,         FGL_MAX_VIEWPORT_PIXELS           },
	{ EGL_MAX_PBUFFER_WIDTH,          FGL_MAX_VIEWPORT_DIMS             },
	{ EGL_NATIVE_RENDERABLE,          EGL_TRUE                          },
	{ EGL_NATIVE_VISUAL_ID,           0                                 },
	{ EGL_NATIVE_VISUAL_TYPE,         0                                 },
	{ EGL_SAMPLES,                    0                                 },
	{ EGL_SAMPLE_BUFFERS,             0                                 },
	{ EGL_SURFACE_TYPE,               EGL_WINDOW_BIT|EGL_PBUFFER_BIT    },
	{ EGL_TRANSPARENT_TYPE,           EGL_NONE                          },
	{ EGL_TRANSPARENT_BLUE_VALUE,     0                                 },
	{ EGL_TRANSPARENT_GREEN_VALUE,    0                                 },
	{ EGL_TRANSPARENT_RED_VALUE,      0                                 },
	{ EGL_BIND_TO_TEXTURE_RGBA,       EGL_FALSE                         },
	{ EGL_BIND_TO_TEXTURE_RGB,        EGL_FALSE                         },
	{ EGL_MIN_SWAP_INTERVAL,          1                                 },
	{ EGL_MAX_SWAP_INTERVAL,          1                                 },
	{ EGL_LUMINANCE_SIZE,             0                                 },
	{ EGL_ALPHA_MASK_SIZE,            0                                 },
	{ EGL_COLOR_BUFFER_TYPE,          EGL_RGB_BUFFER                    },
	{ EGL_RENDERABLE_TYPE,            EGL_OPENGL_ES_BIT                 },
	{ EGL_CONFORMANT,                 0                                 }
};

/* Configs are platform specific. See egl[Platform].cpp. */

static const FGLConfigMatcher gConfigManagement[] = {
	{ EGL_BUFFER_SIZE,                FGLConfigMatcher::atLeast },
	{ EGL_ALPHA_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_BLUE_SIZE,                  FGLConfigMatcher::atLeast },
	{ EGL_GREEN_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_RED_SIZE,                   FGLConfigMatcher::atLeast },
	{ EGL_DEPTH_SIZE,                 FGLConfigMatcher::atLeast },
	{ EGL_STENCIL_SIZE,               FGLConfigMatcher::atLeast },
	{ EGL_CONFIG_CAVEAT,              FGLConfigMatcher::exact   },
	{ EGL_CONFIG_ID,                  FGLConfigMatcher::exact   },
	{ EGL_LEVEL,                      FGLConfigMatcher::exact   },
	{ EGL_MAX_PBUFFER_HEIGHT,         FGLConfigMatcher::ignore   },
	{ EGL_MAX_PBUFFER_PIXELS,         FGLConfigMatcher::ignore   },
	{ EGL_MAX_PBUFFER_WIDTH,          FGLConfigMatcher::ignore   },
	{ EGL_NATIVE_RENDERABLE,          FGLConfigMatcher::exact   },
	{ EGL_NATIVE_VISUAL_ID,           FGLConfigMatcher::ignore   },
	{ EGL_NATIVE_VISUAL_TYPE,         FGLConfigMatcher::exact   },
	{ EGL_SAMPLES,                    FGLConfigMatcher::exact   },
	{ EGL_SAMPLE_BUFFERS,             FGLConfigMatcher::exact   },
	{ EGL_SURFACE_TYPE,               FGLConfigMatcher::mask    },
	{ EGL_TRANSPARENT_TYPE,           FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_BLUE_VALUE,     FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_GREEN_VALUE,    FGLConfigMatcher::exact   },
	{ EGL_TRANSPARENT_RED_VALUE,      FGLConfigMatcher::exact   },
	{ EGL_BIND_TO_TEXTURE_RGBA,       FGLConfigMatcher::exact   },
	{ EGL_BIND_TO_TEXTURE_RGB,        FGLConfigMatcher::exact   },
	{ EGL_MIN_SWAP_INTERVAL,          FGLConfigMatcher::exact   },
	{ EGL_MAX_SWAP_INTERVAL,          FGLConfigMatcher::exact   },
	{ EGL_LUMINANCE_SIZE,             FGLConfigMatcher::atLeast },
	{ EGL_ALPHA_MASK_SIZE,            FGLConfigMatcher::atLeast },
	{ EGL_COLOR_BUFFER_TYPE,          FGLConfigMatcher::exact   },
	{ EGL_RENDERABLE_TYPE,            FGLConfigMatcher::mask    },
	{ EGL_CONFORMANT,                 FGLConfigMatcher::mask    }
};

static const FGLConfigPair defaultConfigAttributes[] = {
// attributes that are not specified are simply ignored, if a particular
// one needs not be ignored, it must be specified here, eg:
// { EGL_SURFACE_TYPE, EGL_WINDOW_BIT },
};

/*
 * Internal configuration management
 */

template<typename T>
static int binarySearch(const T sortedArray[], int first, int last, EGLint key)
{
	while (first <= last) {
		int mid = (first + last) / 2;

		if (key > sortedArray[mid].key) {
			first = mid + 1;
		} else if (key < sortedArray[mid].key) {
			last = mid - 1;
		} else {
			return mid;
		}
	}

	return -1;
}

static EGLBoolean getConfigAttrib(EGLConfig config,
						EGLint attribute, EGLint *value)
{
	size_t numConfigs =  gPlatformConfigsNum;
	int index = (int)config;

	if (uint32_t(index) >= numConfigs) {
		setError(EGL_BAD_CONFIG);
		return EGL_FALSE;
	}

	if (attribute == EGL_CONFIG_ID) {
		*value = index;
		return EGL_TRUE;
	}

	int attrIndex;
	attrIndex = binarySearch<FGLConfigPair>(
		gPlatformConfigs[index].array,
		0, gPlatformConfigs[index].size-1,
		attribute);

	if (attrIndex>=0) {
		*value = gPlatformConfigs[index].array[attrIndex].value;
		return EGL_TRUE;
	}

	attrIndex = binarySearch<FGLConfigPair>(
		baseConfigAttributes,
		0, NELEM(baseConfigAttributes)-1,
		attribute);

	if (attrIndex>=0) {
		*value = baseConfigAttributes[attrIndex].value;
		return EGL_TRUE;
	}

	setError(EGL_BAD_ATTRIBUTE);
	return EGL_FALSE;
}

static EGLBoolean getConfigFormatInfo(EGLint configID,
				int32_t *pixelFormat, int32_t *depthFormat)
{
	EGLint color, alpha, depth, stencil;
	EGLBoolean ret;

	ret = getConfigAttrib((EGLConfig)configID, EGL_BUFFER_SIZE, &color);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib((EGLConfig)configID, EGL_ALPHA_SIZE, &alpha);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib((EGLConfig)configID, EGL_DEPTH_SIZE, &depth);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib((EGLConfig)configID, EGL_STENCIL_SIZE, &stencil);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	switch (color) {
	case 32:
		if (alpha)
			*pixelFormat = FGPF_COLOR_MODE_8888;
		else
			*pixelFormat = FGPF_COLOR_MODE_0888;
		break;
	case 16:
	default:
		*pixelFormat = FGPF_COLOR_MODE_565;
		break;
	}

	*depthFormat = (stencil << 8) | depth;

	return EGL_TRUE;
}

static FGLint bppFromFormat(EGLint format)
{
	switch(format) {
	case FGPF_COLOR_MODE_565:
		return 2;
	case FGPF_COLOR_MODE_0888:
	case FGPF_COLOR_MODE_8888:
		return 4;
	default:
		return 0;
	}
}

EGLBoolean fglEGLValidatePixelFormat(EGLConfig config, FGLPixelFormat *fmt)
{
	EGLBoolean ret;
	EGLint bpp, red, green, blue, alpha;

	if (!fmt)
		return EGL_FALSE;

	ret = getConfigAttrib(config, EGL_BUFFER_SIZE, &bpp);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib(config, EGL_RED_SIZE, &red);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib(config, EGL_GREEN_SIZE, &green);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib(config, EGL_BLUE_SIZE, &blue);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	ret = getConfigAttrib(config, EGL_ALPHA_SIZE, &alpha);
	if (ret == EGL_FALSE)
		return EGL_FALSE;

	if (fmt->bpp != bpp)
		return EGL_FALSE;

	if (fmt->red != red)
		return EGL_FALSE;

	if (fmt->green != green)
		return EGL_FALSE;

	if (fmt->blue != blue)
		return EGL_FALSE;

	if (fmt->alpha != alpha)
		return EGL_FALSE;

	return EGL_TRUE;
}

static int isAttributeMatching(int i, EGLint attr, EGLint val)
{
	// look for the attribute in all of our configs
	const FGLConfigPair *configFound = gPlatformConfigs[i].array;
	int index = binarySearch<FGLConfigPair>(
		gPlatformConfigs[i].array,
		0, gPlatformConfigs[i].size-1,
		attr);

	if (index < 0) {
		configFound = baseConfigAttributes;
		index = binarySearch<FGLConfigPair>(
			baseConfigAttributes,
			0, NELEM(baseConfigAttributes)-1,
			attr);
	}

	if (index >= 0) {
		// attribute found, check if this config could match
		int cfgMgtIndex = binarySearch<FGLConfigMatcher>(
			gConfigManagement,
			0, NELEM(gConfigManagement)-1,
			attr);

		if (cfgMgtIndex >= 0) {
			bool match = gConfigManagement[cfgMgtIndex].match(
				val, configFound[index].value);
			if (match) {
				// this config matches
				return 1;
			}
		} else {
		// attribute not found. this should NEVER happen.
		}
	} else {
		// error, this attribute doesn't exist
	}

	return 0;
}

/*
 * EGL configuration queries
 */

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
			EGLint config_size, EGLint *num_config)
{
	if(unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if(unlikely(!isDisplayInitialized(dpy))) {
		setError(EGL_NOT_INITIALIZED);
		return EGL_FALSE;
	}

	if(unlikely(!num_config)) {
		setError(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}

	EGLint num = gPlatformConfigsNum;

	if(!configs) {
		*num_config = num;
		return EGL_TRUE;
	}

	num = min(num, config_size);

	EGLint i;
	for(i = 0; i < num; i++)
		*(configs)++ = (EGLConfig)i;

	*num_config = i;

	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
			EGLConfig *configs, EGLint config_size,
			EGLint *num_config)
{
	if (unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (unlikely(!num_config)) {
		setError(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}

	if (unlikely(attrib_list==0)) {
		/*
		 * A NULL attrib_list should be treated as though it was an empty
		 * one (terminated with EGL_NONE) as defined in
		 * section 3.4.1 "Querying Configurations" in the EGL specification.
		 */
		static const EGLint dummy = EGL_NONE;
		attrib_list = &dummy;
	}

	int numAttributes = 0;
	int numConfigs = gPlatformConfigsNum;
	uint32_t possibleMatch = (1<<numConfigs)-1;
	while(possibleMatch && *attrib_list != EGL_NONE) {
		numAttributes++;
		EGLint attr = *attrib_list++;
		EGLint val  = *attrib_list++;
		for (int i=0 ; possibleMatch && i<numConfigs ; i++) {
			if (!(possibleMatch & (1<<i)))
				continue;
			if (isAttributeMatching(i, attr, val) == 0) {
				possibleMatch &= ~(1<<i);
			}
		}
	}

	// now, handle the attributes which have a useful default value
	for (int j=0 ; possibleMatch && j<(int)NELEM(defaultConfigAttributes) ; j++) {
		// see if this attribute was specified, if not, apply its
		// default value
		if (binarySearch<FGLConfigPair>(
			(const FGLConfigPair *)attrib_list,
			0, numAttributes-1,
			defaultConfigAttributes[j].key) < 0)
		{
			for (int i=0 ; possibleMatch && i<numConfigs ; i++) {
				if (!(possibleMatch & (1<<i)))
					continue;
				if (isAttributeMatching(i,
					defaultConfigAttributes[j].key,
					defaultConfigAttributes[j].value) == 0)
				{
					possibleMatch &= ~(1<<i);
				}
			}
		}
	}

	// return the configurations found
	int n=0;
	if (possibleMatch) {
		if (configs) {
			for (int i=0 ; config_size && i<numConfigs ; i++) {
				if (possibleMatch & (1<<i)) {
					*configs++ = (EGLConfig)i;
					config_size--;
					n++;
				}
			}
		} else {
			for (int i=0 ; i<numConfigs ; i++) {
				if (possibleMatch & (1<<i)) {
					n++;
				}
			}
		}
	}

	*num_config = n;
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
			EGLint attribute, EGLint *value)
{
	if (unlikely(!isDisplayValid(dpy))) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	return getConfigAttrib(config, attribute, value);
}

/*
 * Render surface base class
 */

FGLRenderSurface::FGLRenderSurface(EGLDisplay dpy,
	EGLConfig config, int32_t pixelFormat, int32_t depthFormat) :
magic(MAGIC), flags(0), dpy(dpy), config(config), ctx(0), color(0), depth(0),
depthFormat(depthFormat), format(pixelFormat)
{
}

FGLRenderSurface::~FGLRenderSurface()
{
	magic = 0;
	delete depth;
	delete color;
}

EGLBoolean FGLRenderSurface::bindDrawSurface(FGLContext *gl)
{
	fglSetColorBuffer(gl, color, width, height, stride, format);
	fglSetDepthBuffer(gl, depth, depthFormat);

	return EGL_TRUE;
}

EGLBoolean FGLRenderSurface::bindReadSurface(FGLContext *gl)
{
	fglSetReadBuffer(gl, color);

	return EGL_TRUE;
}

bool FGLRenderSurface::isValid() const {
	if (magic != MAGIC)
		LOGE("invalid EGLSurface (%p)", this);
	return magic == MAGIC;
}

void FGLRenderSurface::terminate() {
	flags |= TERMINATED;
}

bool FGLRenderSurface::isTerminated() const {
	return flags & TERMINATED;
}

EGLBoolean FGLRenderSurface::swapBuffers() {
	return EGL_FALSE;
}

EGLint FGLRenderSurface::getHorizontalResolution() const {
	return (0 * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLRenderSurface::getVerticalResolution() const {
	return (0 * EGL_DISPLAY_SCALING) * (1.0f / 25.4f);
}

EGLint FGLRenderSurface::getRefreshRate() const {
	return (60 * EGL_DISPLAY_SCALING);
}

EGLint FGLRenderSurface::getSwapBehavior() const {
	return EGL_BUFFER_PRESERVED;
}

EGLBoolean FGLRenderSurface::setSwapRectangle(
	EGLint l, EGLint t, EGLint w, EGLint h)
{
	return EGL_FALSE;
}

EGLClientBuffer FGLRenderSurface::getRenderBuffer() const {
	return 0;
}

/*
 * EGL Pixmap surface
 */

#if 0
/* FIXME: Implement pixmap support */
struct FGLPixmapSurface : public FGLRenderSurface
{
	FGLPixmapSurface(
		EGLDisplay dpy, EGLConfig config,
		int32_t depthFormat,
		const 	egl_native_pixmap_t *pixmap);

	virtual ~FGLPixmapSurface() { }

	virtual     bool        initCheck() const { return !depth.format || depth.vaddr!=0; }
	virtual     EGLBoolean  bindDrawSurface(FGLContext *gl);
	virtual     EGLBoolean  bindReadSurface(FGLContext *gl);
	virtual     EGLint      getWidth() const    { return nativePixmap.width;  }
	virtual     EGLint      getHeight() const   { return nativePixmap.height; }
	private:
	egl_native_pixmap_t     nativePixmap;
};

FGLPixmapSurface::FGLPixmapSurface(EGLDisplay dpy,
	EGLConfig config,
	int32_t depthFormat,
	const egl_native_pixmap_t *pixmap)
	: FGLRenderSurface(dpy, config, depthFormat), nativePixmap(*pixmap)
{
	FUNC_UNIMPLEMENTED;

	if (depthFormat) {
		depth.width   = pixmap->width;
		depth.height  = pixmap->height;
		depth.stride  = depth.width; // use the width here
		depth.size    = depth.stride*depth.height*4;
		if (fglCreatePmemSurface(&depth)) {
			setError(EGL_BAD_ALLOC);
		}
	}
}

EGLBoolean FGLPixmapSurface::bindDrawSurface(FGLContext *gl)
{
	FGLSurface buffer;

	FUNC_UNIMPLEMENTED;

	buffer.width   = nativePixmap.width;
	buffer.height  = nativePixmap.height;
	buffer.stride  = nativePixmap.stride;
	buffer.vaddr   = nativePixmap.data;
	buffer.paddr   = 0;

	buffer.format  = nativePixmap.format;

	fglSetColorBuffer(gl, &buffer);
	fglSetDepthBuffer(gl, &depth);

	return EGL_TRUE;
}

EGLBoolean FGLPixmapSurface::bindReadSurface(FGLContext *gl)
{
	FGLSurface buffer;

	FUNC_UNIMPLEMENTED;

	buffer.width   = nativePixmap.width;
	buffer.height  = nativePixmap.height;
	buffer.stride  = nativePixmap.stride;
	buffer.vaddr   = nativePixmap.data;
	buffer.paddr   = 0;
	buffer.size    = 0;
	buffer.format  = nativePixmap.format;

	fglSetReadBuffer(gl, &buffer);

	return EGL_TRUE;
}
/* FIXME: Implement pixmap support. */
#endif

/*
 * EGL PBuffer surface
 */

struct FGLPbufferSurface : public FGLRenderSurface
{
	FGLPbufferSurface(
		EGLDisplay dpy, EGLConfig config, int32_t depthFormat,
		int32_t w, int32_t h, int32_t f);

	virtual ~FGLPbufferSurface();

	virtual     bool        initCheck() const
	{
		return color && color->isValid() && (!depth || depth->isValid());
	}
	virtual     EGLint      getWidth() const    { return width;  }
	virtual     EGLint      getHeight() const   { return height; }
};

FGLPbufferSurface::FGLPbufferSurface(EGLDisplay dpy,
	EGLConfig config, int32_t depthFormat,
	int32_t w, int32_t h, int32_t f)
: FGLRenderSurface(dpy, config, f, depthFormat)
{
	unsigned int size = w * h * bppFromFormat(f);

	color = new FGLLocalSurface(size);
	if (!color || !color->isValid()) {
		setError(EGL_BAD_ALLOC);
		return;
	}

	width   = w;
	height  = h;
	stride  = w;

	if (depthFormat) {
		size = w * h * 4;

		depth = new FGLLocalSurface(size);
		if (!depth || !depth->isValid()) {
			setError(EGL_BAD_ALLOC);
			return;
		}
	}
}

FGLPbufferSurface::~FGLPbufferSurface()
{
}

/*
 * EGL surface management
 */

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy,
	EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_NO_SURFACE;
	}

	if (win == 0) {
		setError(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	EGLint surfaceType;
	if (getConfigAttrib(config, EGL_SURFACE_TYPE, &surfaceType) == EGL_FALSE)
		return EGL_NO_SURFACE;

	if (!(surfaceType & EGL_WINDOW_BIT)) {
		setError(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	EGLint configID;
	if (getConfigAttrib(config, EGL_CONFIG_ID, &configID) == EGL_FALSE)
		return EGL_NO_SURFACE;

	int32_t depthFormat;
	int32_t pixelFormat;
	if (getConfigFormatInfo(configID, &pixelFormat, &depthFormat) == EGL_FALSE) {
		setError(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	FGLRenderSurface *surface = platformCreateWindowSurface(dpy,
					config, depthFormat, win, pixelFormat);
	if (surface == NULL)
		/* platform code should have set error value for us */
		return EGL_NO_SURFACE;

	if (!surface->initCheck()) {
		// there was a problem in the ctor, the error
		// flag has been set.
		delete surface;
		return EGL_NO_SURFACE;
	}

	return (EGLSurface)surface;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
				const EGLint *attrib_list)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_NO_SURFACE;
	}

	EGLint surfaceType;
	if (getConfigAttrib(config, EGL_SURFACE_TYPE, &surfaceType) == EGL_FALSE)
		return EGL_NO_SURFACE;

	if (!(surfaceType & EGL_PBUFFER_BIT)) {
		setError(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	EGLint configID;
	if (getConfigAttrib(config, EGL_CONFIG_ID, &configID) == EGL_FALSE)
		return EGL_NO_SURFACE;

	int32_t depthFormat;
	int32_t pixelFormat;
	if (getConfigFormatInfo(configID, &pixelFormat, &depthFormat) == EGL_FALSE) {
		setError(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}

	int32_t w = 0;
	int32_t h = 0;
	if(attrib_list) {
		while (attrib_list[0]) {
			if (attrib_list[0] == EGL_WIDTH)  w = attrib_list[1];
			if (attrib_list[0] == EGL_HEIGHT) h = attrib_list[1];
			attrib_list+=2;
		}
	}

	FGLRenderSurface *surface;
	surface = new FGLPbufferSurface(dpy, config, depthFormat, w, h,
								pixelFormat);
	if (surface == NULL) {
		setError(EGL_BAD_ALLOC);
		return EGL_NO_SURFACE;
	}

	if (!surface->initCheck()) {
		// there was a problem in the ctor, the error
		// flag has been set.
		delete surface;
		surface = 0;
	}
	return surface;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config,
				EGLNativePixmapType pixmap,
				const EGLint *attrib_list)
{
	FUNC_UNIMPLEMENTED;
	return EGL_NO_SURFACE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (surface == EGL_NO_SURFACE)
		return EGL_TRUE;

	FGLRenderSurface *fglSurface = (FGLRenderSurface *)surface;

	if (fglSurface->isTerminated()) {
		setError(EGL_BAD_SURFACE);
		return EGL_FALSE;
	}

	if (!fglSurface->isValid()) {
		setError(EGL_BAD_SURFACE);
		return EGL_FALSE;
	}

	if (fglSurface->dpy != dpy) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (fglSurface->ctx) {
		// Mark the surface for destruction on context detach
		fglSurface->terminate();
		return EGL_TRUE;
	}

	delete fglSurface;
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
			EGLint attribute, EGLint *value)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLRenderSurface *fglSurface = static_cast<FGLRenderSurface *>(surface);

	if (!fglSurface->isValid()) {
		setError(EGL_BAD_SURFACE);
		return EGL_FALSE;
	}

	if (fglSurface->dpy != dpy) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	EGLBoolean ret = EGL_TRUE;

	switch (attribute) {
		case EGL_CONFIG_ID:
			ret = getConfigAttrib(fglSurface->config,
							EGL_CONFIG_ID, value);
			break;
		case EGL_WIDTH:
			*value = fglSurface->getWidth();
			break;
		case EGL_HEIGHT:
			*value = fglSurface->getHeight();
			break;
		case EGL_LARGEST_PBUFFER:
			// not modified for a window or pixmap surface
			break;
		case EGL_TEXTURE_FORMAT:
			*value = EGL_NO_TEXTURE;
			break;
		case EGL_TEXTURE_TARGET:
			*value = EGL_NO_TEXTURE;
			break;
		case EGL_MIPMAP_TEXTURE:
			*value = EGL_FALSE;
			break;
		case EGL_MIPMAP_LEVEL:
			*value = 0;
			break;
		case EGL_RENDER_BUFFER:
			// TODO: return the real RENDER_BUFFER here
			*value = EGL_BACK_BUFFER;
			break;
		case EGL_HORIZONTAL_RESOLUTION:
			// pixel/mm * EGL_DISPLAY_SCALING
			*value = fglSurface->getHorizontalResolution();
			break;
		case EGL_VERTICAL_RESOLUTION:
			// pixel/mm * EGL_DISPLAY_SCALING
			*value = fglSurface->getVerticalResolution();
			break;
		case EGL_PIXEL_ASPECT_RATIO: {
			// w/h * EGL_DISPLAY_SCALING
			int wr = fglSurface->getHorizontalResolution();
			int hr = fglSurface->getVerticalResolution();
			*value = (wr * EGL_DISPLAY_SCALING) / hr;
			} break;
		case EGL_SWAP_BEHAVIOR:
			*value = fglSurface->getSwapBehavior();
			break;
		default:
			setError(EGL_BAD_ATTRIBUTE);
			ret = EGL_FALSE;
	}

	return ret;
}

/**
	Client APIs
*/

EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
	if (api != EGL_OPENGL_ES_API) {
		setError(EGL_BAD_PARAMETER);
		return EGL_FALSE;
	}

	return EGL_TRUE;
}

EGLAPI EGLenum EGLAPIENTRY eglQueryAPI(void)
{
	return EGL_OPENGL_ES_API;
}


EGLAPI EGLBoolean EGLAPIENTRY eglWaitClient(void)
{
	glFinish();
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseThread(void)
{
	EGLContext ctx = eglGetCurrentContext();
	if (ctx != EGL_NO_CONTEXT) {
		EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglMakeCurrent(dpy, EGL_NO_CONTEXT, EGL_NO_SURFACE,
								EGL_NO_SURFACE);
	}

	return EGL_TRUE;
}

// TODO: Implement following functions
EGLAPI EGLSurface EGLAPIENTRY eglCreatePbufferFromClientBuffer(
	EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
	EGLConfig config, const EGLint *attrib_list)
{
	FUNC_UNIMPLEMENTED;
	return EGL_NO_SURFACE;
}


EGLAPI EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
			EGLint attribute, EGLint value)
{
	FUNC_UNIMPLEMENTED;
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
	FUNC_UNIMPLEMENTED;
	setError(EGL_BAD_SURFACE);
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
	FUNC_UNIMPLEMENTED;
	setError(EGL_BAD_SURFACE);
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
	FUNC_UNIMPLEMENTED;
	return EGL_FALSE;
}

extern FGLContext *fglCreateContext(void);
extern void fglDestroyContext(FGLContext *ctx);


EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config,
			EGLContext share_context,
			const EGLint *attrib_list)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_NO_SURFACE;
	}

	FGLContext *gl = fglCreateContext();
	if (!gl) {
		setError(EGL_BAD_ALLOC);
		return EGL_NO_CONTEXT;
	}

	gl->egl.flags	= FGL_NEVER_CURRENT;
	gl->egl.dpy	= dpy;
	gl->egl.config	= config;

	return (EGLContext)gl;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
	FGLContext *c = (FGLContext *)ctx;

	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (c->egl.flags & FGL_TERMINATE) {
		// already scheduled for deletion
		setError(EGL_BAD_CONTEXT);
		return EGL_FALSE;
	}

	if (c->egl.flags & FGL_IS_CURRENT) {
		// mark the context for deletion on context switch
		c->egl.flags |= FGL_TERMINATE;
		return EGL_TRUE;
	}

	fglDestroyContext(c);
	return EGL_TRUE;
}

static void fglUnbindContext(FGLContext *c)
{
	// mark the current context as not current, and flush
	glFinish();
	c->egl.flags &= ~FGL_IS_CURRENT;

	// Unbind the draw surface
	FGLRenderSurface *d = (FGLRenderSurface *)c->egl.draw;
	d->disconnect();
	d->ctx = EGL_NO_CONTEXT;
	c->egl.draw = EGL_NO_SURFACE;
	// Delete it if it's terminated
	if(d->isTerminated())
		delete d;

	// Unbind the read surface if it's different than draw
	FGLRenderSurface *r = (FGLRenderSurface *)c->egl.read;
	if (r != d) {
		r->disconnect();
		r->ctx = EGL_NO_CONTEXT;
	}
	c->egl.draw = EGL_NO_SURFACE;
	if(r->isTerminated())
		delete r;

	if (c->egl.flags & FGL_TERMINATE)
		fglDestroyContext(c);
}

static int fglMakeCurrent(FGLContext *gl, FGLRenderSurface *d,
							FGLRenderSurface *r)
{
	FGLContext *current = getGlThreadSpecific();

	// Current context (if available) should get detached
	if (!gl) {
		if (!current) {
			// Nothing changed
			return EGL_TRUE;
		}

		fglUnbindContext(current);

		// this thread has no context attached to it from now on
		setGlThreadSpecific(0);

		return EGL_TRUE;
	}

	// New context should get attached
	if (gl->egl.flags & FGL_IS_CURRENT) {
		if (current != gl) {
			// it is an error to set a context current, if it's
			// already current to another thread
			setError(EGL_BAD_ACCESS);
			return EGL_FALSE;
		}

		// Nothing changed
		return EGL_TRUE;
	}

	// Detach old context if present
	if (current)
		fglUnbindContext(current);

	// Attach draw surface
	gl->egl.draw = (EGLSurface)d;
	if (d->connect() == EGL_FALSE) {
		// connect() already set the error
		return EGL_FALSE;
	}
	d->ctx = (EGLContext)gl;
	d->bindDrawSurface(gl);

	// Attach read surface
	gl->egl.read = (EGLSurface)r;
	if (r != d && d->connect() == EGL_FALSE) {
		// connect() already set the error
		return EGL_FALSE;
	}
	r->ctx = (EGLContext)gl;
	r->bindReadSurface(gl);

	// Make the new context current
	setGlThreadSpecific(gl);
	gl->egl.flags |= FGL_IS_CURRENT;

	// Perform first time initialization if needed
	if (gl->egl.flags & FGL_NEVER_CURRENT) {
		gl->egl.flags &= ~FGL_NEVER_CURRENT;
		GLint w = 0;
		GLint h = 0;

		w = d->getWidth();
		h = d->getHeight();

		uint32_t depth = (gl->surface.depthFormat & 0xff) ? 1 : 0;
		uint32_t stencil = (gl->surface.depthFormat >> 8) ? 0xff : 0;

		fimgSetZBufWriteMask(gl->fimg, depth);
		fimgSetStencilBufWriteMask(gl->fimg, 0, stencil);
		fimgSetStencilBufWriteMask(gl->fimg, 1, stencil);

		glViewport(0, 0, w, h);
		glScissor(0, 0, w, h);
		glDisable(GL_SCISSOR_TEST);
	}

	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
			EGLSurface read, EGLContext ctx)
{
	/*
	 * Do all the EGL sanity checks
	 */

	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (draw) {
		FGLRenderSurface *s = (FGLRenderSurface *)draw;

		if (s->isTerminated()) {
			setError(EGL_BAD_SURFACE);
			return EGL_FALSE;
		}

		if (!s->isValid()) {
			setError(EGL_BAD_SURFACE);
			return EGL_FALSE;
		}

		if (s->dpy != dpy) {
			setError(EGL_BAD_DISPLAY);
			return EGL_FALSE;
		}
	}

	if (read && read!=draw) {
		FGLRenderSurface *s = (FGLRenderSurface *)read;

		if (s->isTerminated()) {
			setError(EGL_BAD_SURFACE);
			return EGL_FALSE;
		}

		if (!s->isValid()) {
			setError(EGL_BAD_SURFACE);
			return EGL_FALSE;
		}

		if (s->dpy != dpy) {
			setError(EGL_BAD_DISPLAY);
			return EGL_FALSE;
		}
	}

	if ((read == EGL_NO_SURFACE || draw == EGL_NO_SURFACE)
	    && (ctx != EGL_NO_CONTEXT)) {
		setError(EGL_BAD_MATCH);
		return EGL_FALSE;
	}

	if ((read != EGL_NO_SURFACE || draw != EGL_NO_SURFACE)
	    && (ctx == EGL_NO_CONTEXT)) {
		setError(EGL_BAD_MATCH);
		return EGL_FALSE;
	}

	if (ctx != EGL_NO_CONTEXT) {
		FGLRenderSurface *d = (FGLRenderSurface *)draw;
		FGLRenderSurface *r = (FGLRenderSurface *)read;

		if ((d->ctx && d->ctx != ctx) || (r->ctx && r->ctx != ctx)) {
			// already bound to another thread
			setError(EGL_BAD_ACCESS);
			return EGL_FALSE;
		}
	}

	/*
	 * Proceed with the main part
	 */

	FGLContext *gl = (FGLContext *)ctx;
	FGLRenderSurface *d = (FGLRenderSurface *)draw;
	FGLRenderSurface *r = (FGLRenderSurface *)read;
	return fglMakeCurrent(gl, d, r);
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext(void)
{
	return (EGLContext)getGlThreadSpecific();
}

EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw)
{
	EGLContext ctx = (EGLContext)getGlThreadSpecific();

	if (ctx == EGL_NO_CONTEXT)
		return EGL_NO_SURFACE;

	FGLContext *c = (FGLContext *)ctx;

	if (readdraw == EGL_READ)
		return c->egl.read;
	else if (readdraw == EGL_DRAW)
		return c->egl.draw;

	setError(EGL_BAD_ATTRIBUTE);
	return EGL_NO_SURFACE;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay(void)
{
	EGLContext ctx = (EGLContext)getGlThreadSpecific();

	if (ctx == EGL_NO_CONTEXT)
		return EGL_NO_DISPLAY;

	FGLContext *c = (FGLContext *)ctx;
	return c->egl.dpy;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQueryContext(EGLDisplay dpy, EGLContext ctx,
			EGLint attribute, EGLint *value)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLContext *c = (FGLContext *)ctx;

	switch (attribute) {
	case EGL_CONFIG_ID:
		// Returns the ID of the EGL frame buffer configuration with
		// respect to which the context was created
		return getConfigAttrib(c->egl.config, EGL_CONFIG_ID, value);
	}

	setError(EGL_BAD_ATTRIBUTE);
	return EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglWaitGL(void)
{
	glFinish();
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglWaitNative(EGLint engine)
{
	FUNC_UNIMPLEMENTED;
	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
	if (!isDisplayValid(dpy)) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	FGLRenderSurface *d = static_cast<FGLRenderSurface *>(surface);

	if (!d->isValid()) {
		setError(EGL_BAD_SURFACE);
		return EGL_FALSE;
	}

	if (d->dpy != dpy) {
		setError(EGL_BAD_DISPLAY);
		return EGL_FALSE;
	}

	if (d->ctx != EGL_NO_CONTEXT) {
		FGLContext *c = (FGLContext *)d->ctx;
		if (c->egl.flags & FGL_IS_CURRENT)
			glFinish();
	}

	// post the surface
	if (d->swapBuffers() != EGL_TRUE)
		/* Error code should have been set */
		return EGL_FALSE;

	// if it's bound to a context, update the buffer
	if (d->ctx != EGL_NO_CONTEXT) {
		FGLContext *c = (FGLContext *)d->ctx;
		d->bindDrawSurface(c);
		// if this surface is also the read surface of the context
		// it is bound to, make sure to update the read buffer as well.
		// The EGL spec is a little unclear about this.

		if (c->egl.read == surface)
			d->bindReadSurface(c);
	}

	return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglCopyBuffers(EGLDisplay dpy, EGLSurface surface,
			EGLNativePixmapType target)
{
	FUNC_UNIMPLEMENTED;
	return EGL_FALSE;
}

/*
 * Extension management
 */

#define EGLFunc	__eglMustCastToProperFunctionPointerType

static const FGLExtensionMap gExtensionMap[] = {
	{ "glDrawTexsOES", (EGLFunc)&glDrawTexsOES },
	{ "glDrawTexiOES", (EGLFunc)&glDrawTexiOES },
	{ "glDrawTexfOES", (EGLFunc)&glDrawTexfOES },
	{ "glDrawTexxOES", (EGLFunc)&glDrawTexxOES },
	{ "glDrawTexsvOES", (EGLFunc)&glDrawTexsvOES },
	{ "glDrawTexivOES", (EGLFunc)&glDrawTexivOES },
	{ "glDrawTexfvOES", (EGLFunc)&glDrawTexfvOES },
	{ "glDrawTexxvOES", (EGLFunc)&glDrawTexxvOES },
#if 0
	{ "glQueryMatrixxOES", (EGLFunc)&glQueryMatrixxOES },
	{ "glEGLImageTargetRenderbufferStorageOES",
			(EGLFunc)&glEGLImageTargetRenderbufferStorageOES },
	{ "glClipPlanef", (EGLFunc)&glClipPlanef },
	{ "glClipPlanex", (EGLFunc)&glClipPlanex },
#endif
	{ "glBindBuffer", (EGLFunc)&glBindBuffer },
	{ "glBufferData", (EGLFunc)&glBufferData },
	{ "glBufferSubData", (EGLFunc)&glBufferSubData },
	{ "glDeleteBuffers", (EGLFunc)&glDeleteBuffers },
	{ "glGenBuffers", (EGLFunc)&glGenBuffers },
	{ "glEGLImageTargetTexture2DOES",
				(EGLFunc)&glEGLImageTargetTexture2DOES },
	{ NULL, NULL },
};

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY
eglGetProcAddress(const char *procname)
{
	const FGLExtensionMap *map;

	for (map = gExtensionMap; map->name; ++map) {
		if (!strcmp(procname, map->name))
			return map->address;
	}

	for (map = gPlatformExtensionMap; map->name; ++map) {
		if (!strcmp(procname, map->name))
			return map->address;
	}

	//LOGE("Requested not implemented function %s address", procname);

	return NULL;
}
