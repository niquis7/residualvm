/* ResidualVM - A 3D game interpreter
 *
 * ResidualVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "common/scummsys.h"

#if defined(SDL_BACKEND)

#include "backends/graphics/openglsdl/openglsdl-graphics.h"

#include "common/config-manager.h"
#include "engines/engine.h"
#include "graphics/pixelbuffer.h"
#include "graphics/opengl/context.h"
#include "graphics/opengl/framebuffer.h"
#include "graphics/opengl/surfacerenderer.h"
#include "graphics/opengl/system_headers.h"
#include "graphics/opengl/texture.h"
#include "graphics/opengl/tiledsurface.h"

OpenGLSdlGraphicsManager::OpenGLSdlGraphicsManager(SdlEventSource *sdlEventSource, SdlWindow *window, const Capabilities &capabilities)
	:
	ResVmSdlGraphicsManager(sdlEventSource, window, capabilities),
#if SDL_VERSION_ATLEAST(2, 0, 0)
	_glContext(nullptr),
#endif
	_overlayScreen(nullptr),
	_overlayBackground(nullptr),
	_gameRect(),
	_frameBuffer(nullptr),
	_surfaceRenderer(nullptr) {
	ConfMan.registerDefault("antialiasing", 0);

	_sideTextures[0] = _sideTextures[1] = nullptr;

	// Don't start at zero so that the value is never the same as the surface graphics manager
	_screenChangeCount = 1 << (sizeof(int) * 8 - 2);
}

OpenGLSdlGraphicsManager::~OpenGLSdlGraphicsManager() {
	closeOverlay();
}

bool OpenGLSdlGraphicsManager::hasFeature(OSystem::Feature f) {
	return
		(f == OSystem::kFeatureFullscreenMode) ||
		(f == OSystem::kFeatureOpenGL) ||
		(f == OSystem::kFeatureAspectRatioCorrection) ||
		(f == OSystem::kFeatureOverlaySupportsAlpha && _overlayFormat.aBits() > 3);
}

void OpenGLSdlGraphicsManager::setupScreen(uint gameWidth, uint gameHeight, bool fullscreen, bool accel3d) {
	assert(accel3d);
	closeOverlay();

	_antialiasing = ConfMan.getInt("antialiasing");
	_fullscreen = fullscreen;
	_lockAspectRatio = ConfMan.getBool("aspect_ratio");

	bool engineSupportsArbitraryResolutions = g_engine && g_engine->hasFeature(Engine::kSupportsArbitraryResolutions);

	// Select how the game screen is going to be drawn
	GameRenderTarget gameRenderTarget = selectGameRenderTarget(_fullscreen, true, engineSupportsArbitraryResolutions,
	                                                           _capabilities.openGLFrameBuffer, _lockAspectRatio);

	// Choose the effective window size or fullscreen mode
	uint effectiveWidth;
	uint effectiveHeight;
	if (_fullscreen && canUsePreferredResolution(gameRenderTarget, engineSupportsArbitraryResolutions)) {
		Common::Rect fullscreenResolution = getPreferredFullscreenResolution();
		effectiveWidth = fullscreenResolution.width();
		effectiveHeight = fullscreenResolution.height();
	} else {
		effectiveWidth = gameWidth;
		effectiveHeight = gameHeight;
	}

	// Compute the rectangle where to draw the game inside the effective screen
	_gameRect = computeGameRect(gameRenderTarget, gameWidth, gameHeight, effectiveWidth, effectiveHeight);

	if (!createScreen(effectiveWidth, effectiveHeight, gameRenderTarget)) {
		warning("Error: %s", SDL_GetError());
		g_system->quit();
	}

	int glflag;
	const GLubyte *str;

	// apply atribute again for sure based on SDL docs
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	str = glGetString(GL_VENDOR);
	debug("INFO: OpenGL Vendor: %s", str);
	str = glGetString(GL_RENDERER);
	debug("INFO: OpenGL Renderer: %s", str);
	str = glGetString(GL_VERSION);
	debug("INFO: OpenGL Version: %s", str);
	SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &glflag);
	debug("INFO: OpenGL Red bits: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &glflag);
	debug("INFO: OpenGL Green bits: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &glflag);
	debug("INFO: OpenGL Blue bits: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &glflag);
	debug("INFO: OpenGL Alpha bits: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &glflag);
	debug("INFO: OpenGL Z buffer depth bits: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &glflag);
	debug("INFO: OpenGL Double Buffer: %d", glflag);
	SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &glflag);
	debug("INFO: OpenGL Stencil buffer bits: %d", glflag);

#ifdef USE_GLEW
	debug("INFO: GLEW Version: %s", glewGetString(GLEW_VERSION));
	GLenum err = glewInit();
	if (err != GLEW_OK) {
		warning("Error: %s", glewGetErrorString(err));
		g_system->quit();
	}
#endif

#ifdef USE_OPENGL_SHADERS
	debug("INFO: GLSL version: %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
#endif

	initializeOpenGLContext();
	_surfaceRenderer = OpenGL::createBestSurfaceRenderer();

#ifdef SCUMM_BIG_ENDIAN
	_overlayFormat = Graphics::PixelFormat(4, 8, 8, 8, 8, 24, 16, 8, 0);
#else
	_overlayFormat = Graphics::PixelFormat(4, 8, 8, 8, 8, 0, 8, 16, 24);
#endif
	_overlayScreen = new OpenGL::TiledSurface(effectiveWidth, effectiveHeight, _overlayFormat);

	_overlayWidth = effectiveWidth;
	_overlayHeight = effectiveHeight;
	_screenFormat = _overlayFormat;

	_screenChangeCount++;

#if !defined(AMIGAOS)
	if (gameRenderTarget == kFramebuffer) {
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		_frameBuffer = createFramebuffer(gameWidth, gameHeight);
		_frameBuffer->attach();
	}
#endif
}

Graphics::PixelBuffer OpenGLSdlGraphicsManager::getScreenPixelBuffer() {
	error("Direct screen buffer access is not allowed when using OpenGL");
}

void OpenGLSdlGraphicsManager::initializeOpenGLContext() const {
	OpenGL::ContextType type;

#ifdef USE_GLES2
	type = OpenGL::kContextGLES2;
#else
	type = OpenGL::kContextGL;
#endif

	OpenGLContext.initialize(type);
}

OpenGLSdlGraphicsManager::OpenGLPixelFormat::OpenGLPixelFormat(uint screenBytesPerPixel, uint red, uint blue, uint green, uint alpha, int samples) :
		bytesPerPixel(screenBytesPerPixel),
		redSize(red),
		blueSize(blue),
		greenSize(green),
		alphaSize(alpha),
		multisampleSamples(samples) {

}

bool OpenGLSdlGraphicsManager::createScreen(uint effectiveWidth, uint effectiveHeight,
                                            GameRenderTarget gameRenderTarget) {
	// Build a list of OpenGL pixel formats usable by ResidualVM
	Common::Array<OpenGLPixelFormat> pixelFormats;
	if (_antialiasing > 0 && gameRenderTarget == kScreen) {
		// Don't enable screen level multisampling when rendering to a framebuffer
		pixelFormats.push_back(OpenGLPixelFormat(32, 8, 8, 8, 8, _antialiasing));
		pixelFormats.push_back(OpenGLPixelFormat(16, 5, 5, 5, 1, _antialiasing));
		pixelFormats.push_back(OpenGLPixelFormat(16, 5, 6, 5, 0, _antialiasing));
	}
	pixelFormats.push_back(OpenGLPixelFormat(32, 8, 8, 8, 8, 0));
	pixelFormats.push_back(OpenGLPixelFormat(16, 5, 5, 5, 1, 0));
	pixelFormats.push_back(OpenGLPixelFormat(16, 5, 6, 5, 0, 0));

	// Unfortunatly, SDL does not provide a list of valid pixel formats
	// for the current OpenGL implementation and hardware.
	// SDL may not be able to create a screen with the preferred pixel format.
	// Try all the pixel formats in the list until SDL returns a valid screen.
	Common::Array<OpenGLPixelFormat>::const_iterator it = pixelFormats.begin();
	for (; it != pixelFormats.end(); it++) {
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, it->redSize);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, it->greenSize);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, it->blueSize);
		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, it->alphaSize);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, it->multisampleSamples > 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, it->multisampleSamples);
#if SDL_VERSION_ATLEAST(2, 0, 0)
#ifdef USE_GLES2
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
#endif

#if SDL_VERSION_ATLEAST(2, 0, 0)
		uint32 sdlflags = SDL_WINDOW_OPENGL;
		if (_fullscreen)
			sdlflags |= SDL_WINDOW_FULLSCREEN;

		if (_window->createWindow(effectiveWidth, effectiveHeight, sdlflags)) {
			_glContext = SDL_GL_CreateContext(_window->getSDLWindow());
			if (_glContext) {
				break;
			}
		}

		_window->destroyWindow();
#else
		uint32 sdlflags = SDL_OPENGL;
		if (_fullscreen)
			sdlflags |= SDL_FULLSCREEN;

		SDL_Surface *screen = SDL_SetVideoMode(effectiveWidth, effectiveHeight, it->bytesPerPixel, sdlflags);
		if (screen) {
			break;
		}
#endif
	}

	// Display a warning if the effective pixel format is not the preferred one
	if (it != pixelFormats.begin() && it != pixelFormats.end()) {
		bool wantsAA = pixelFormats.front().multisampleSamples > 0;
		bool gotAA = it->multisampleSamples > 0;

		warning("Couldn't create a %d-bit visual%s, using to %d-bit%s instead",
		        pixelFormats.front().bytesPerPixel,
		        wantsAA && !gotAA ? " with AA" : "",
		        it->bytesPerPixel,
		        wantsAA && !gotAA ? " without AA" : "");
	}

	return it != pixelFormats.end();
}

void OpenGLSdlGraphicsManager::drawOverlay() {
	glViewport(0, 0, _overlayWidth, _overlayHeight);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	_surfaceRenderer->prepareState();

	if (_overlayBackground) {
		_overlayBackground->draw(_surfaceRenderer);
	}

	_surfaceRenderer->enableAlphaBlending(true);
	_surfaceRenderer->setFlipY(true);
	_overlayScreen->draw(_surfaceRenderer);

	_surfaceRenderer->restorePreviousState();
}

void OpenGLSdlGraphicsManager::drawSideTextures() {
	if (_fullscreen && _lockAspectRatio) {
		_surfaceRenderer->setFlipY(true);

		const Math::Vector2d nudge(1.0 / float(_overlayWidth), 0);
		if (_sideTextures[0] != nullptr) {
			float left = _gameRect.getBottomLeft().getX() - (float(_overlayHeight) / float(_sideTextures[0]->getHeight())) * _sideTextures[0]->getWidth() / float(_overlayWidth);
			Math::Rect2d leftRect(Math::Vector2d(left, 0.0), _gameRect.getBottomLeft() + nudge);
			_surfaceRenderer->render(_sideTextures[0], leftRect);
		}

		if (_sideTextures[1] != nullptr) {
			float right = _gameRect.getTopRight().getX() + (float(_overlayHeight) / float(_sideTextures[1]->getHeight())) * _sideTextures[1]->getWidth() / float(_overlayWidth);
			Math::Rect2d rightRect(_gameRect.getTopRight() - nudge, Math::Vector2d(right, 1.0));
			_surfaceRenderer->render(_sideTextures[1], rightRect);
		}

		_surfaceRenderer->setFlipY(false);
	}
}

#ifndef AMIGAOS
OpenGL::FrameBuffer *OpenGLSdlGraphicsManager::createFramebuffer(uint width, uint height) {
#if !defined(USE_GLES2)
	if (_antialiasing && OpenGLContext.framebufferObjectMultisampleSupported) {
		return new OpenGL::MultiSampleFrameBuffer(width, height, _antialiasing);
	} else
#endif
	{
		return new OpenGL::FrameBuffer(width, height);
	}
}
#endif // AMIGAOS

void OpenGLSdlGraphicsManager::updateScreen() {
	if (_frameBuffer) {
		_frameBuffer->detach();
		glViewport(0, 0, _overlayWidth, _overlayHeight);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		_surfaceRenderer->prepareState();
		drawSideTextures();
		_surfaceRenderer->render(_frameBuffer, _gameRect);
		_surfaceRenderer->restorePreviousState();
	}

	if (_overlayVisible) {
		_overlayScreen->update();

		if (_overlayBackground) {
			_overlayBackground->update();
		}

		drawOverlay();
	}

#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_GL_SwapWindow(_window->getSDLWindow());
#else
	SDL_GL_SwapBuffers();
#endif

	if (_frameBuffer) {
		_frameBuffer->attach();
	}
}

int16 OpenGLSdlGraphicsManager::getHeight() {
	// ResidualVM specific
	if (_frameBuffer)
		return _frameBuffer->getHeight();
	else
		return _overlayHeight;
}

int16 OpenGLSdlGraphicsManager::getWidth() {
	// ResidualVM specific
	if (_frameBuffer)
		return _frameBuffer->getWidth();
	else
		return _overlayWidth;
}

#pragma mark -
#pragma mark --- Overlays ---
#pragma mark -

void OpenGLSdlGraphicsManager::suggestSideTextures(Graphics::Surface *left, Graphics::Surface *right) {
	delete _sideTextures[0];
	_sideTextures[0] = nullptr;
	delete _sideTextures[1];
	_sideTextures[1] = nullptr;
	if (left) {
		_sideTextures[0] = new OpenGL::Texture(*left);
	}
	if (right) {
		_sideTextures[1] = new OpenGL::Texture(*right);
	}
}

void OpenGLSdlGraphicsManager::showOverlay() {
	if (_overlayVisible) {
		return;
	}
	_overlayVisible = true;

	delete _overlayBackground;
	_overlayBackground = nullptr;

	if (g_engine) {
		// If there is a game running capture the screen, so that it can be shown "below" the overlay.
		_overlayBackground = new OpenGL::TiledSurface(_overlayWidth, _overlayHeight, _overlayFormat);
		Graphics::Surface *background = _overlayBackground->getBackingSurface();
		glReadPixels(0, 0, background->w, background->h, GL_RGBA, GL_UNSIGNED_BYTE, background->getPixels());
	}
}

void OpenGLSdlGraphicsManager::hideOverlay() {
	if (!_overlayVisible) {
		return;
	}
	_overlayVisible = false;

	delete _overlayBackground;
	_overlayBackground = nullptr;
}

void OpenGLSdlGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	_overlayScreen->copyRectToSurface(buf, pitch, x, y, w, h);
}

void OpenGLSdlGraphicsManager::clearOverlay() {
	_overlayScreen->fill(0);
}

void OpenGLSdlGraphicsManager::grabOverlay(void *buf, int pitch) {
	const Graphics::Surface *overlayData = _overlayScreen->getBackingSurface();

	const byte *src = (const byte *)overlayData->getPixels();
	byte *dst = (byte *)buf;

	for (uint h = overlayData->h; h > 0; --h) {
		memcpy(dst, src, overlayData->w * overlayData->format.bytesPerPixel);
		dst += pitch;
		src += overlayData->pitch;
	}
}

void OpenGLSdlGraphicsManager::closeOverlay() {
	delete _sideTextures[0];
	delete _sideTextures[1];
	_sideTextures[0] = _sideTextures[1] = nullptr;

	if (_overlayScreen) {
		delete _overlayScreen;
		_overlayScreen = nullptr;
	}

	delete _surfaceRenderer;
	_surfaceRenderer = nullptr;

	delete _frameBuffer;
	_frameBuffer = nullptr;

	OpenGL::Context::destroy();

#if SDL_VERSION_ATLEAST(2, 0, 0)
	deinitializeRenderer();
#endif
}

void OpenGLSdlGraphicsManager::warpMouse(int x, int y) {
	//ResidualVM specific
	if (_frameBuffer) {
		// Scale from game coordinates to screen coordinates
		x = (x * _gameRect.getWidth() * _overlayWidth) / _frameBuffer->getWidth();
		y = (y * _gameRect.getHeight() * _overlayHeight) / _frameBuffer->getHeight();

		x += _gameRect.getTopLeft().getX() * _overlayWidth;
		y += _gameRect.getTopLeft().getY() * _overlayHeight;
	}

	_window->warpMouseInWindow(x, y);
}

void OpenGLSdlGraphicsManager::transformMouseCoordinates(Common::Point &point) {
	if (_overlayVisible || !_frameBuffer)
		return;

	// Scale from screen coordinates to game coordinates
	point.x -= _gameRect.getTopLeft().getX() * _overlayWidth;
	point.y -= _gameRect.getTopLeft().getY() * _overlayHeight;

	point.x = (point.x * _frameBuffer->getWidth())  / (_gameRect.getWidth() * _overlayWidth);
	point.y = (point.y * _frameBuffer->getHeight()) / (_gameRect.getHeight() * _overlayHeight);

	// Make sure we only supply valid coordinates.
	point.x = CLIP<int16>(point.x, 0, _frameBuffer->getWidth() - 1);
	point.y = CLIP<int16>(point.y, 0, _frameBuffer->getHeight() - 1);
}

#if SDL_VERSION_ATLEAST(2, 0, 0)
void OpenGLSdlGraphicsManager::deinitializeRenderer() {
	SDL_GL_DeleteContext(_glContext);
	_glContext = nullptr;

	_window->destroyWindow();
}
#endif // SDL_VERSION_ATLEAST(2, 0, 0)

#endif
