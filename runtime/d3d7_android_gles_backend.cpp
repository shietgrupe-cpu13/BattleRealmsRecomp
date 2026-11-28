#include "d3d7_gpu_backend.h"
#include "d3d7_fixed_function.h"
#include "d3d7_gl_state_cache.h"
#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
#include "d3d7_texture_probe.h"
#endif
#include "d3d7_vertex_batch.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include "android_diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
#include <unordered_set>
#endif
#include <vector>

namespace br::runtime::d3d7 {
namespace {

static_assert(std::is_standard_layout_v<GpuVertex>);
static_assert(sizeof(GpuVertex) == 28u * sizeof(float));
static_assert(offsetof(GpuVertex, x) == 0u);
static_assert(offsetof(GpuVertex, diffuse) == 4u * sizeof(float));
static_assert(offsetof(GpuVertex, specular) == 8u * sizeof(float));
static_assert(offsetof(GpuVertex, texcoord) == 12u * sizeof(float));

struct Target {
	GLuint fbo = 0, color = 0, depth = 0;
	GLuint depthReadFbo = 0, depthReadColor = 0;
	uint32_t depthReadWidth = 0, depthReadHeight = 0;
	GpuSurface colorView{}, depthView{};
	uint64_t colorRevision = UINT64_MAX, depthRevision = UINT64_MAX;
	bool colorDirty = false, depthDirty = false;
};
struct Texture {
	GLuint id = 0;
	uint64_t revision = UINT64_MAX;
	uint32_t width = 0, height = 0;
};
struct Program {
	GLuint handle = 0;
	GLint targetSize = -1;
	GLint fogColor = -1;
	GLint alphaRef = -1;
	std::array<GLint, 8> samplers{};
	CachedGlValue<std::array<uint32_t, 2>> appliedTargetSize{};
	CachedGlValue<uint32_t> appliedFogColor{};
	CachedGlValue<uint32_t> appliedAlphaRef{};
	std::array<CachedGlValue<uint32_t>, 8> appliedSamplers{};
};
struct Renderer {
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	std::thread::id owner{};
	std::unordered_map<uint32_t, Target> targets;
	std::unordered_map<uint32_t, Texture> textures;
	std::unordered_map<uint32_t, GLuint> samplers;
	std::unordered_map<std::string, Program> programs;
	GlStateCache stateCache{};
	GLuint vertexBuffer = 0;
	GLuint vertexArray = 0;
	bool vertexArrayReady = false;
	std::vector<uint8_t> pixelData;
	std::vector<uint16_t> depthData;
	std::vector<GpuVertex> pendingVertices;
	GpuDrawState pendingState{};
	Program* pendingProgram = nullptr;
	std::array<GLuint, 8> pendingTextures{};
	bool batchPending = false;
	uint64_t batchSubmissions = 0u;
	uint64_t batchSubmitNanoseconds = 0u;
	#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
	uint32_t textureDiagnosticEvents = 0u;
	std::unordered_set<uint64_t> drawDiagnosticKeys;
	#endif
	GLuint depthReadProgram = 0, depthReadVao = 0;
};
Renderer g;

void logSyncFailure(const char* reason, uint32_t handle, uint32_t width, uint32_t height,
	uint32_t bpp, uint32_t pitch, uint32_t bytes, uint32_t glError, EGLint eglError)
{
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_ERROR, "BattleRealmsNative",
		"D3D7_GPU_SYNC_FAILED", "reason=%s surface=%08x size=%ux%u bpp=%u pitch=%u bytes=%u gl_error=0x%x egl_error=0x%x",
		reason, handle, width, height, bpp, pitch, bytes, glError, static_cast<uint32_t>(eglError));
}

bool ensureContext()
{
	if (g.context != EGL_NO_CONTEXT) {
		if (g.owner != std::this_thread::get_id()) return false;
		if (eglGetCurrentContext() == g.context && eglGetCurrentDisplay() == g.display &&
			eglGetCurrentSurface(EGL_DRAW) == g.surface &&
			eglGetCurrentSurface(EGL_READ) == g.surface) return true;
		g.stateCache.invalidateDrawState();
		return eglMakeCurrent(g.display, g.surface, g.surface, g.context) == EGL_TRUE;
	}
	g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g.display == EGL_NO_DISPLAY || eglInitialize(g.display, nullptr, nullptr) != EGL_TRUE) return false;
	const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16, EGL_NONE};
	EGLConfig config{}; EGLint count = 0;
	if (eglChooseConfig(g.display, attributes, &config, 1, &count) != EGL_TRUE || count != 1) return false;
	const EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
	g.surface = eglCreatePbufferSurface(g.display, config, pbuffer);
	const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	g.context = eglCreateContext(g.display, config, EGL_NO_CONTEXT, contextAttributes);
	if (g.surface == EGL_NO_SURFACE || g.context == EGL_NO_CONTEXT ||
		eglMakeCurrent(g.display, g.surface, g.surface, g.context) != EGL_TRUE) return false;
	g.owner = std::this_thread::get_id();
	return true;
}

uint32_t channel(uint32_t pixel, uint32_t mask, uint32_t fallback)
{
	if (!mask) return fallback;
	const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(mask));
	const uint32_t maximum = mask >> shift;
	return maximum ? ((((pixel & mask) >> shift) * 255u + maximum / 2u) / maximum) : fallback;
}
uint32_t encode(uint32_t value, uint32_t mask)
{
	if (!mask) return 0u;
	const uint32_t shift = static_cast<uint32_t>(__builtin_ctz(mask));
	const uint32_t maximum = mask >> shift;
	return maximum ? (((value * maximum + 127u) / 255u) << shift) & mask : 0u;
}
bool supportedTarget(const GpuSurface& s)
{
	const uint64_t minimumPitch = uint64_t(s.width) * (s.bitsPerPixel / 8u);
	const uint64_t requiredBytes = s.height ? uint64_t(s.height - 1u) * s.pitch + minimumPitch : UINT64_MAX;
	return s.handle && s.width && s.height && s.pixels && s.pitch && minimumPitch <= s.pitch &&
		requiredBytes <= s.byteCount &&
		((s.bitsPerPixel == 16u && s.redMask == 0xf800u && s.greenMask == 0x07e0u && s.blueMask == 0x001fu) ||
		 (s.bitsPerPixel == 32u && s.redMask == 0x00ff0000u && s.greenMask == 0x0000ff00u &&
		  s.blueMask == 0x000000ffu && (s.alphaMask == 0u || s.alphaMask == 0xff000000u)));
}
bool uploadColor(Target& t, const GpuSurface& s)
{
	if (!supportedTarget(s)) return false;
	g.pixelData.resize(size_t(s.width) * s.height * 4u);
	auto& rgba = g.pixelData;
	const uint32_t bpp = s.bitsPerPixel / 8u;
	for (uint32_t y = 0; y < s.height; ++y) for (uint32_t x = 0; x < s.width; ++x) {
		uint32_t p = 0; std::memcpy(&p, s.pixels + size_t(y) * s.pitch + size_t(x) * bpp, bpp);
		auto* d = rgba.data() + (size_t(y) * s.width + x) * 4u;
		d[0] = uint8_t(channel(p, s.redMask, 0)); d[1] = uint8_t(channel(p, s.greenMask, 0));
		d[2] = uint8_t(channel(p, s.blueMask, 0)); d[3] = uint8_t(channel(p, s.alphaMask, 255));
	}
	g.stateCache.invalidateTextureBindings();
	glBindTexture(GL_TEXTURE_2D, t.color);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(s.width), GLsizei(s.height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	t.colorView = s; t.colorRevision = s.cpuRevision; t.colorDirty = false;
	return glGetError() == GL_NO_ERROR;
}
bool uploadDepth(Target& t, const GpuSurface& s)
{
	if (!s.handle || !s.pixels || s.width != t.colorView.width || s.height != t.colorView.height ||
		s.bitsPerPixel != 16u || uint64_t(s.pitch) < uint64_t(s.width) * 2u ||
		(uint64_t(s.height - 1u) * s.pitch + uint64_t(s.width) * 2u) > s.byteCount) return false;
	const uint32_t bpp = s.bitsPerPixel / 8u;
	g.depthData.resize(size_t(s.width) * s.height);
	for (uint32_t y = 0; y < s.height; ++y) for (uint32_t x = 0; x < s.width; ++x)
		std::memcpy(&g.depthData[size_t(y) * s.width + x], s.pixels + size_t(y) * s.pitch + size_t(x) * bpp, bpp);
	g.stateCache.invalidateBindings();
	glBindTexture(GL_TEXTURE_2D, t.depth);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, GLsizei(s.width), GLsizei(s.height), 0,
		GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, g.depthData.data());
	glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
	t.depthView = s; t.depthRevision = s.cpuRevision; t.depthDirty = false;
	return glGetError() == GL_NO_ERROR;
}

std::string vertexShader()
{
	std::string s = "#version 300 es\nprecision highp float;layout(location=0)in vec4 p;layout(location=1)in vec4 d;layout(location=2)in vec4 q;";
	for (int i=0;i<8;++i) s += "layout(location="+std::to_string(i+3)+")in vec2 u"+std::to_string(i)+";out vec2 v"+std::to_string(i)+";";
	s += "out vec4 vd;out vec4 vs;uniform vec2 targetSize;void main(){float w=1.0/p.w;vec2 n=vec2(p.x/targetSize.x*2.0-1.0,1.0-p.y/targetSize.y*2.0);gl_Position=vec4(n*w,(p.z*2.0-1.0)*w,w);vd=d;vs=q;";
	for (int i=0;i<8;++i) s += "v"+std::to_string(i)+"=u"+std::to_string(i)+";";
	return s+"}\n";
}
const char* argExpr(uint32_t arg, const std::string& tex)
{
	return arg == kTextureArgTexture ? tex.c_str() : arg == kTextureArgDiffuse ? "vd" : "cur";
}
std::string fragmentShader(const GpuDrawState& state)
{
	std::string s = "#version 300 es\nprecision highp float;in vec4 vd;in vec4 vs;";
	for (uint32_t i=0;i<8;++i) s += "in vec2 v"+std::to_string(i)+";uniform sampler2D t"+std::to_string(i)+";";
	s += "out vec4 outColor;uniform vec4 fogColor;uniform float alphaRef;void main(){vec4 cur=vd;";
	for (uint32_t i=0;i<state.activeTextureStages;++i) {
		const auto& st=state.textureStageState[i];
		const std::string tex="texture(t"+std::to_string(i)+",v"+std::to_string(st[11]&0xffffu)+")";
		if (st[1] == kTextureOpDisable) break;
		const std::string sample="s"+std::to_string(i);
		s += "vec4 "+sample+"="+tex+";";
		const std::string c1=argExpr(st[2],sample), c2=argExpr(st[3],sample);
		if (st[1]==kTextureOpSelectArg1) s += "cur.rgb="+c1+".rgb;";
		else if (st[1]==kTextureOpSelectArg2) s += "cur.rgb="+c2+".rgb;";
		else if (st[1]==kTextureOpModulate) s += "cur.rgb="+c1+".rgb*"+c2+".rgb;";
		else if (st[1]==kTextureOpModulate2x) s += "cur.rgb=2.0*"+c1+".rgb*"+c2+".rgb;";
		else return {};
		if (st[4]==kTextureOpDisable) continue;
		const std::string a1=argExpr(st[5],sample), a2=argExpr(st[6],sample);
		if (st[4]==kTextureOpSelectArg1) s += "cur.a="+a1+".a;";
		else if (st[4]==kTextureOpSelectArg2) s += "cur.a="+a2+".a;";
		else if (st[4]==kTextureOpModulate) s += "cur.a="+a1+".a*"+a2+".a;";
		else if (st[4]==kTextureOpModulate2x) s += "cur.a=2.0*"+a1+".a*"+a2+".a;";
		else return {};
	}
	if (state.renderState[29]) s += "cur.rgb=min(cur.rgb+vs.rgb,vec3(1.0));";
	if (state.renderState[28] && state.renderState[140]) s += "cur.rgb=mix(fogColor.rgb,cur.rgb,clamp(vs.a,0.0,1.0));";
	if (state.renderState[15]) {
		const uint32_t cmp=state.renderState[25]; const char* op=cmp==1?"false":cmp==2?"cur.a<alphaRef":cmp==3?"cur.a==alphaRef":cmp==4?"cur.a<=alphaRef":cmp==5?"cur.a>alphaRef":cmp==6?"cur.a!=alphaRef":cmp==7?"cur.a>=alphaRef":"true";
		s += "if(!("+std::string(op)+"))discard;";
	}
	return s+"outColor=clamp(cur,0.0,1.0);}\n";
}
GLuint compile(GLenum kind, const std::string& source)
{
	GLuint shader=glCreateShader(kind); const char* p=source.c_str(); glShaderSource(shader,1,&p,nullptr); glCompileShader(shader);
	GLint ok=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&ok); if(!ok){glDeleteShader(shader);return 0;} return shader;
}
Program* programFor(const GpuDrawState& state)
{
	const std::string fs=fragmentShader(state); if(fs.empty()) return 0;
	auto found=g.programs.find(fs); if(found!=g.programs.end()) return &found->second;
	GLuint vs=compile(GL_VERTEX_SHADER,vertexShader()), ps=compile(GL_FRAGMENT_SHADER,fs); if(!vs||!ps){if(vs)glDeleteShader(vs);if(ps)glDeleteShader(ps);return 0;}
	GLuint p=glCreateProgram(); glAttachShader(p,vs);glAttachShader(p,ps);glLinkProgram(p);glDeleteShader(vs);glDeleteShader(ps);
	GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glDeleteProgram(p);return 0;}
	Program entry{};
	entry.handle = p;
	entry.targetSize = glGetUniformLocation(p, "targetSize");
	entry.fogColor = glGetUniformLocation(p, "fogColor");
	entry.alphaRef = glGetUniformLocation(p, "alphaRef");
	for (uint32_t i = 0; i < entry.samplers.size(); ++i)
		entry.samplers[i] = glGetUniformLocation(p, ("t" + std::to_string(i)).c_str());
	return &g.programs.emplace(fs, entry).first->second;
}

bool sameSurface(const GpuSurface& a, const GpuSurface& b)
{
	return a.handle == b.handle && a.width == b.width && a.height == b.height &&
		a.pitch == b.pitch && a.bitsPerPixel == b.bitsPerPixel &&
		a.redMask == b.redMask && a.greenMask == b.greenMask &&
		a.blueMask == b.blueMask && a.alphaMask == b.alphaMask &&
		a.cpuRevision == b.cpuRevision && a.pixels == b.pixels && a.byteCount == b.byteCount;
}

bool sameBatchState(const GpuDrawState& a, const GpuDrawState& b)
{
	if (!sameSurface(a.target, b.target) || !sameSurface(a.depth, b.depth) ||
		a.activeTextureStages != b.activeTextureStages ||
		a.viewportX != b.viewportX || a.viewportY != b.viewportY ||
		a.viewportWidth != b.viewportWidth || a.viewportHeight != b.viewportHeight ||
		a.renderState != b.renderState || a.textureStageState != b.textureStageState) return false;
	for (uint32_t i = 0; i < a.activeTextureStages; ++i)
		if (!sameSurface(a.textures[i], b.textures[i])) return false;
	return true;
}

GLenum compare(uint32_t value);
GLenum blendFactor(uint32_t value);

GLuint samplerFor(const std::array<uint32_t, 32>& stage)
{
	TextureSamplerState state{};
	if (!decodeD3d7TextureSamplerState(stage[17], stage[16], stage[13], stage[14], state)) return 0u;
	const uint32_t key = stage[17] | (stage[16] << 2u) | (stage[13] << 4u) | (stage[14] << 6u);
	auto found = g.samplers.find(key);
	if (found != g.samplers.end()) return found->second;
	GLuint sampler = 0u;
	glGenSamplers(1, &sampler);
	if (!sampler) return 0u;
	const GLint minFilter = state.minFilter == TextureFilterMode::Linear ? GL_LINEAR : GL_NEAREST;
	const GLint magFilter = state.magFilter == TextureFilterMode::Linear ? GL_LINEAR : GL_NEAREST;
	const GLint addressU = state.addressU == TextureAddressMode::Wrap ? GL_REPEAT :
		state.addressU == TextureAddressMode::Mirror ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
	const GLint addressV = state.addressV == TextureAddressMode::Wrap ? GL_REPEAT :
		state.addressV == TextureAddressMode::Mirror ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
	glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, minFilter);
	glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, magFilter);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, addressU);
	glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, addressV);
	if (glGetError() != GL_NO_ERROR) { glDeleteSamplers(1, &sampler); return 0u; }
	return g.samplers.emplace(key, sampler).first->second;
}

bool flushPending() noexcept
{
	if (!g.batchPending) return true;
	struct SubmitTimer {
		using Clock = std::chrono::steady_clock;
		uint64_t& total;
		Clock::time_point start = Clock::now();
		~SubmitTimer()
		{
			total += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now() - start).count());
		}
	} submitTimer{g.batchSubmitNanoseconds};
	try {
		if (!ensureContext()) return false;
		const GpuDrawState& s = g.pendingState;
		auto targetIt = g.targets.find(s.target.handle);
		if (targetIt == g.targets.end() || !targetIt->second.fbo ||
			!g.pendingProgram || !g.pendingProgram->handle || g.pendingVertices.empty() || g.pendingVertices.size() % 3u) return false;
		Target& target = targetIt->second;
		Program& program = *g.pendingProgram;
		if (g.stateCache.apply(g.stateCache.framebuffer, target.fbo))
			glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
		const std::array<int32_t, 4> viewport = {0, 0, int32_t(s.target.width), int32_t(s.target.height)};
		if (g.stateCache.apply(g.stateCache.viewport, viewport))
			glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		if (g.stateCache.apply(g.stateCache.program, program.handle))
			glUseProgram(program.handle);
		if (program.targetSize >= 0 && program.appliedTargetSize.update({s.target.width, s.target.height}))
			glUniform2f(program.targetSize, float(s.target.width), float(s.target.height));
		const uint32_t fog = s.renderState[34];
		if (program.fogColor >= 0 && program.appliedFogColor.update(fog))
			glUniform4f(program.fogColor, float((fog >> 16) & 255) / 255.f,
				float((fog >> 8) & 255) / 255.f, float(fog & 255) / 255.f, float(fog >> 24) / 255.f);
		const uint32_t alphaRef = s.renderState[24] & 255u;
		if (program.alphaRef >= 0 && program.appliedAlphaRef.update(alphaRef))
			glUniform1f(program.alphaRef, float(alphaRef) / 255.f);
		for (uint32_t i = 0; i < s.activeTextureStages; ++i) {
			const auto& stage = s.textureStageState[i];
			if (stage[1] == kTextureOpDisable) break;
			const GLuint sampler = samplerFor(stage);
			if (!sampler) return false;
			if (g.stateCache.apply(g.stateCache.samplers[i], sampler))
				glBindSampler(i, sampler);
			if (!g.pendingTextures[i]) continue;
			const GLenum textureUnit = GL_TEXTURE0 + i;
			if (g.stateCache.apply(g.stateCache.activeTexture, uint32_t(textureUnit)))
				glActiveTexture(textureUnit);
			if (g.stateCache.apply(g.stateCache.textures[i], g.pendingTextures[i]))
				glBindTexture(GL_TEXTURE_2D, g.pendingTextures[i]);
			if (program.samplers[i] >= 0 && program.appliedSamplers[i].update(i))
				glUniform1i(program.samplers[i], GLint(i));
		}
		const bool depthTest = s.renderState[7] != 0;
		if (g.stateCache.apply(g.stateCache.depthTest, depthTest)) {
			if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
		}
		const GLenum depthFunction = compare(s.renderState[23]);
		if (g.stateCache.apply(g.stateCache.depthFunction, uint32_t(depthFunction))) glDepthFunc(depthFunction);
		const bool depthWrite = s.renderState[14] != 0;
		if (g.stateCache.apply(g.stateCache.depthWrite, depthWrite)) glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
		if (s.renderState[27]) {
			const GLenum src = blendFactor(s.renderState[19]), dst = blendFactor(s.renderState[20]);
			if (!src || !dst) return false;
			if (g.stateCache.apply(g.stateCache.blend, true)) glEnable(GL_BLEND);
			if (g.stateCache.apply(g.stateCache.blendFunction, std::array<uint32_t, 2>{uint32_t(src), uint32_t(dst)}))
				glBlendFunc(src, dst);
		} else if (g.stateCache.apply(g.stateCache.blend, false)) glDisable(GL_BLEND);
		const bool cull = s.renderState[22] != 1u;
		if (g.stateCache.apply(g.stateCache.cull, cull)) {
			if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
		}
		if (cull) {
			if (g.stateCache.apply(g.stateCache.cullFace, uint32_t(GL_BACK))) glCullFace(GL_BACK);
			const GLenum frontFace = s.renderState[22] == 2u ? GL_CCW : GL_CW;
			if (g.stateCache.apply(g.stateCache.frontFace, uint32_t(frontFace))) glFrontFace(frontFace);
		}
		constexpr GLsizei stride = GLsizei(sizeof(GpuVertex));
		if (!g.vertexBuffer) glGenBuffers(1, &g.vertexBuffer);
		if (!g.vertexArray) glGenVertexArrays(1, &g.vertexArray);
		if (!g.vertexBuffer || !g.vertexArray) return false;
		if (g.stateCache.apply(g.stateCache.vertexArray, g.vertexArray)) glBindVertexArray(g.vertexArray);
		if (g.stateCache.apply(g.stateCache.arrayBuffer, g.vertexBuffer)) glBindBuffer(GL_ARRAY_BUFFER, g.vertexBuffer);
		if (!g.vertexArrayReady) {
			for (GLuint loc = 0; loc < 11; ++loc) {
				const GLint components = loc == 0 ? 4 : loc < 3 ? 4 : 2;
				const GLuint offset = loc == 0 ? GLuint(offsetof(GpuVertex, x)) :
					loc == 1 ? GLuint(offsetof(GpuVertex, diffuse)) :
					loc == 2 ? GLuint(offsetof(GpuVertex, specular)) :
					GLuint(offsetof(GpuVertex, texcoord) + (loc - 3u) * 2u * sizeof(float));
				glEnableVertexAttribArray(loc);
				glVertexAttribPointer(loc, components, GL_FLOAT, GL_FALSE, stride,
					reinterpret_cast<void*>(uintptr_t(offset)));
			}
			g.vertexArrayReady = true;
		}
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g.pendingVertices.size() * sizeof(GpuVertex)),
			g.pendingVertices.data(), GL_STREAM_DRAW);
		if (glGetError() != GL_NO_ERROR) return false;
		target.colorDirty = true;
		target.depthDirty = s.renderState[14] != 0;
		glDrawArrays(GL_TRIANGLES, 0, GLsizei(g.pendingVertices.size()));
		const GLenum drawError = glGetError();
		g.batchPending = false;
		g.pendingVertices.clear();
		g.pendingTextures.fill(0u);
		if (drawError == GL_NO_ERROR) ++g.batchSubmissions;
		return drawError == GL_NO_ERROR;
	} catch (...) {
		g.batchPending = false;
		g.pendingVertices.clear();
		return false;
	}
}

GLuint depthReadProgram()
{
	if (g.depthReadProgram) return g.depthReadProgram;
	const std::string vs = "#version 300 es\nprecision highp float;out vec2 uv;"
		"void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
		"uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
	const std::string fs = "#version 300 es\nprecision highp float;precision highp int;in vec2 uv;"
		"uniform sampler2D depthTex;out vec4 color;"
		"void main(){uint d=uint(clamp(floor(texture(depthTex,uv).r*65535.0+0.5),0.0,65535.0));"
		"color=vec4(float((d>>8u)&255u),float(d&255u),0.0,255.0)/255.0;}";
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
	if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
	GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
	glDeleteShader(v); glDeleteShader(f);
	GLint ok = GL_FALSE; glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) { glDeleteProgram(p); return 0; }
	glGenVertexArrays(1, &g.depthReadVao);
	if (!g.depthReadVao) { glDeleteProgram(p); return 0; }
	g.depthReadProgram = p;
	return p;
}

bool ensureDepthReadTarget(Target& t, uint32_t width, uint32_t height)
{
	if (!depthReadProgram()) return false;
	if (!t.depthReadFbo) {
		g.stateCache.invalidateBindings();
		glGenFramebuffers(1, &t.depthReadFbo);
		glGenTextures(1, &t.depthReadColor);
		if (!t.depthReadFbo || !t.depthReadColor) return false;
		glBindTexture(GL_TEXTURE_2D, t.depthReadColor);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (t.depthReadWidth != width || t.depthReadHeight != height) {
		g.stateCache.framebuffer.invalidate();
		g.stateCache.invalidateTextureBindings();
		glBindTexture(GL_TEXTURE_2D, t.depthReadColor);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(width), GLsizei(height), 0,
			GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindFramebuffer(GL_FRAMEBUFFER, t.depthReadFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.depthReadColor, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
		t.depthReadWidth = width; t.depthReadHeight = height;
	}
	return glGetError() == GL_NO_ERROR;
}

bool ensureTarget(const GpuDrawState& s, Target*& out)
{
	if (!supportedTarget(s.target) || !s.depth.handle || !s.depth.pixels ||
		s.depth.width!=s.target.width || s.depth.height!=s.target.height ||
		s.depth.bitsPerPixel!=16u) return false;
	auto& t=g.targets[s.target.handle];
	if (!t.fbo) {
		g.stateCache.invalidateBindings();
		glGenFramebuffers(1,&t.fbo);glGenTextures(1,&t.color);glGenTextures(1,&t.depth);
		glBindTexture(GL_TEXTURE_2D,t.color);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,GLsizei(s.target.width),GLsizei(s.target.height),0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
		glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.color,0);
		glBindTexture(GL_TEXTURE_2D,t.depth);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
		const GLint depthInternal=s.depth.bitsPerPixel==16?GL_DEPTH_COMPONENT16:GL_DEPTH_COMPONENT24;
		const GLenum depthType=s.depth.bitsPerPixel==16?GL_UNSIGNED_SHORT:GL_UNSIGNED_INT;
		glTexImage2D(GL_TEXTURE_2D,0,depthInternal,GLsizei(s.target.width),GLsizei(s.target.height),0,GL_DEPTH_COMPONENT,depthType,nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,t.depth,0);
		if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
	}
	t.depthView=s.depth;
	if(t.colorRevision!=s.target.cpuRevision && !uploadColor(t,s.target))return false;
	if(t.depthRevision!=s.depth.cpuRevision && !uploadDepth(t,s.depth))return false;
	out=&t;return true;
}

bool ensureTexture(const GpuSurface& s, GLuint& id)
{
	if(!s.handle || !s.width || !s.height || !s.pixels ||
		(s.bitsPerPixel!=16u&&s.bitsPerPixel!=32u) ||
		uint64_t(s.pitch)<uint64_t(s.width)*(s.bitsPerPixel/8u) ||
		(uint64_t(s.height-1u)*s.pitch+uint64_t(s.width)*(s.bitsPerPixel/8u))>s.byteCount)return false;
	auto& t=g.textures[s.handle];
	if(!t.id){glGenTextures(1,&t.id);t.width=s.width;t.height=s.height;}
	if(t.width!=s.width||t.height!=s.height){g.stateCache.invalidateTextureBindings();glDeleteTextures(1,&t.id);glGenTextures(1,&t.id);t.width=s.width;t.height=s.height;t.revision=UINT64_MAX;}
	if(t.revision!=s.cpuRevision){
		g.pixelData.resize(size_t(s.width)*s.height*4u);auto& rgba=g.pixelData;const uint32_t bpp=s.bitsPerPixel/8u;
		#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
		TextureContentProbe content{};
		#endif
		for(uint32_t y=0;y<s.height;++y)for(uint32_t x=0;x<s.width;++x){uint32_t p=0;std::memcpy(&p,s.pixels+size_t(y)*s.pitch+size_t(x)*bpp,bpp);auto*d=rgba.data()+(size_t(y)*s.width+x)*4u;d[0]=uint8_t(channel(p,s.redMask,0));d[1]=uint8_t(channel(p,s.greenMask,0));d[2]=uint8_t(channel(p,s.blueMask,0));d[3]=uint8_t(channel(p,s.alphaMask,255));
			#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
			content.add(p,bpp,d[0],d[1],d[2],d[3]);
			#endif
		}
		g.stateCache.invalidateTextureBindings();
		glBindTexture(GL_TEXTURE_2D,t.id);
		// GLES defaults the minification filter to a mipmapped mode, which makes
		// a level-zero-only texture incomplete. Match D3D7's point-filter default.
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,GLsizei(s.width),GLsizei(s.height),0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
		#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
		const GLenum uploadError=glGetError();
		if(g.textureDiagnosticEvents<512u){++g.textureDiagnosticEvents;const uint32_t opaqueBlackPermille=content.pixels?uint32_t(content.opaqueBlackPixels*1000u/content.pixels):0u;const uint32_t alphaZeroPermille=content.pixels?uint32_t(content.alphaZeroPixels*1000u/content.pixels):0u;br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO,"BattleRealmsNative","D3D7_TEXTURE_UPLOAD","surface=%08x size=%ux%u pitch=%u bpp=%u masks=%08x/%08x/%08x/%08x revision=%llu pixels=%llu source_fnv=%08x rgba_fnv=%08x rgb_black=%llu opaque_black_permille=%u alpha_zero_permille=%u gl_error=0x%x",s.handle,s.width,s.height,s.pitch,s.bitsPerPixel,s.redMask,s.greenMask,s.blueMask,s.alphaMask,static_cast<unsigned long long>(s.cpuRevision),static_cast<unsigned long long>(content.pixels),content.sourceFnv1a,content.rgbaFnv1a,static_cast<unsigned long long>(content.rgbBlackPixels),opaqueBlackPermille,alphaZeroPermille,uploadError);}
		if(uploadError!=GL_NO_ERROR)return false;
		#endif
		t.revision=s.cpuRevision;
	}
	id=t.id;return glGetError()==GL_NO_ERROR;
}

GLenum compare(uint32_t c){switch(c){case 1:return GL_NEVER;case 2:return GL_LESS;case 3:return GL_EQUAL;case 4:return GL_LEQUAL;case 5:return GL_GREATER;case 6:return GL_NOTEQUAL;case 7:return GL_GEQUAL;default:return GL_ALWAYS;}}
GLenum blendFactor(uint32_t f){switch(f){case kBlendZero:return GL_ZERO;case kBlendOne:return GL_ONE;case kBlendSrcColor:return GL_SRC_COLOR;case kBlendInvSrcColor:return GL_ONE_MINUS_SRC_COLOR;case kBlendSrcAlpha:return GL_SRC_ALPHA;case kBlendInvSrcAlpha:return GL_ONE_MINUS_SRC_ALPHA;case kBlendDestAlpha:return GL_DST_ALPHA;case kBlendInvDestAlpha:return GL_ONE_MINUS_DST_ALPHA;case kBlendDestColor:return GL_DST_COLOR;case kBlendInvDestColor:return GL_ONE_MINUS_DST_COLOR;default:return 0;}}
bool stageUsesTexture(const std::array<uint32_t, 32>& s)
{
	const auto uses = [](uint32_t op, uint32_t a, uint32_t b) {
		return (op == kTextureOpSelectArg1 && a == kTextureArgTexture) ||
			(op == kTextureOpSelectArg2 && b == kTextureArgTexture) ||
			((op == kTextureOpModulate || op == kTextureOpModulate2x) &&
				(a == kTextureArgTexture || b == kTextureArgTexture));
	};
	return uses(s[1], s[2], s[3]) || uses(s[4], s[5], s[6]);
}

#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
uint64_t drawDiagnosticKey(const GpuDrawState& state, const TextureUvProbe& uv) noexcept
{
	uint64_t hash = 14695981039346656037ull;
	const auto add = [&hash](uint64_t value) {
		for (uint32_t i = 0u; i < 8u; ++i) {
			hash = (hash ^ uint8_t(value >> (i * 8u))) * 1099511628211ull;
		}
	};
	add(state.guestCaller); add(state.guestFvf); add(state.target.handle);
	add(state.activeTextureStages); add(state.viewportX); add(state.viewportY);
	add(state.viewportWidth); add(state.viewportHeight);
	// Keep otherwise-identical draws distinct when guest blend, alpha-test,
	// depth, or lighting state changes. These values decide whether transparent
	// terrain texels reveal the framebuffer or write black into it.
	for (const uint32_t index : {7u, 14u, 15u, 19u, 20u, 22u, 23u, 24u, 25u, 27u, 137u, 140u})
		add(state.renderState[index]);
	add(uv.valid ? 1u : 0u);
	if (uv.valid) {
		const auto floatBits = [](float value) { uint32_t bits = 0u; std::memcpy(&bits, &value, sizeof(bits)); return bits; };
		add(floatBits(uv.minU)); add(floatBits(uv.minV));
		add(floatBits(uv.maxU)); add(floatBits(uv.maxV));
	}
	for (uint32_t stage = 0u; stage < state.activeTextureStages; ++stage) {
		const GpuSurface& texture = state.textures[stage];
		add(texture.handle); add(texture.cpuRevision); add(texture.width); add(texture.height);
		add(texture.bitsPerPixel); add(texture.redMask); add(texture.greenMask);
		add(texture.blueMask); add(texture.alphaMask);
		const auto& ts = state.textureStageState[stage];
		for (const uint32_t index : {1u, 2u, 3u, 4u, 5u, 6u, 11u, 13u, 14u, 16u, 17u, 18u}) add(ts[index]);
	}
	return hash;
}

void logDrawTextureState(const GpuDrawState& state, uint32_t vertexCount, const GpuVertex* vertices)
{
	if (g.drawDiagnosticKeys.size() >= 256u) return;
	const TextureUvProbe uv = probeTextureUv(state, vertices, vertexCount);
	const uint64_t key = drawDiagnosticKey(state, uv);
	if (!g.drawDiagnosticKeys.insert(key).second) return;
	const auto textureInfo = [&state](uint32_t stage, uint32_t* handle, uint32_t* width,
		uint32_t* height, uint32_t* bpp, uint32_t* revision) {
		if (stage >= state.activeTextureStages) { *handle = *width = *height = *bpp = *revision = 0u; return; }
		const GpuSurface& s = state.textures[stage];
		*handle = s.handle; *width = s.width; *height = s.height; *bpp = s.bitsPerPixel;
		*revision = static_cast<uint32_t>(s.cpuRevision);
	};
	uint32_t t0=0u,w0=0u,h0=0u,b0=0u,r0=0u,t1=0u,w1=0u,h1=0u,b1=0u,r1=0u;
	textureInfo(0u,&t0,&w0,&h0,&b0,&r0); textureInfo(1u,&t1,&w1,&h1,&b1,&r1);
	const auto& s0=state.textureStageState[0]; const auto& s1=state.textureStageState[1];
	const uint32_t uvBlackPermille = uv.sampledPixels
		? uv.blackPixels * 1000u / uv.sampledPixels : 0u;
	br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO,"BattleRealmsNative","D3D7_DRAW_TEXTURE_STATE",
		"caller=%08x fvf=%08x vertices=%u target=%08x size=%ux%u stages=%u texture0=%08x/%ux%u/%ubpp/rev%u texture1=%08x/%ux%u/%ubpp/rev%u rs_alpha_test=%u/%u/%u rs_alpha_blend=%u/%u/%u rs_depth=%u/%u/%u rs_cull=%u rs_lighting=%u rs_fog=%u stage0=%u,%u,%u,%u,%u,%u uv=%u stage1=%u,%u,%u,%u,%u,%u uv=%u uv_range_valid=%u uv_unit=%u uv_min=%.6f,%.6f uv_max=%.6f,%.6f uv_region_pixels=%u uv_region_black=%u uv_region_black_permille=%u",
		state.guestCaller,state.guestFvf,vertexCount,state.target.handle,state.target.width,state.target.height,
		state.activeTextureStages,t0,w0,h0,b0,r0,t1,w1,h1,b1,r1,
		state.renderState[15],state.renderState[24],state.renderState[25],
		state.renderState[27],state.renderState[19],state.renderState[20],
		state.renderState[7],state.renderState[14],state.renderState[23],
		state.renderState[22],state.renderState[137],state.renderState[140],
		s0[1],s0[2],s0[3],s0[4],s0[5],s0[6],s0[11]&0xffffu,
		s1[1],s1[2],s1[3],s1[4],s1[5],s1[6],s1[11]&0xffffu,
		uv.valid ? 1u : 0u,uv.withinUnitRange ? 1u : 0u,
		uv.minU,uv.minV,uv.maxU,uv.maxV,uv.sampledPixels,uv.blackPixels,uvBlackPermille);
}
#endif

} // namespace

bool renderGpuTriangles(const GpuDrawState& s,const GpuVertex* v,uint32_t count) noexcept
{
	try {
		if (!v || count < 3u || count % 3u || s.activeTextureStages > 8u ||
			count > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max())) return false;
		if (g.batchPending && g.owner == std::this_thread::get_id() && sameBatchState(g.pendingState, s)) {
			if (g.pendingVertices.size() > size_t(std::numeric_limits<GLsizei>::max()) - count) return false;
			return appendGpuVertices(g.pendingVertices, v, count);
		}
		if (!flushPending() || !ensureContext()) return false;
		Target* target = nullptr;
		if (!ensureTarget(s, target)) return false;
		std::array<GLuint, 8> textures{};
		for (uint32_t i = 0; i < s.activeTextureStages; ++i) {
			const auto& stage = s.textureStageState[i];
			if (stage[1] == kTextureOpDisable) break;
			const uint32_t coord = stage[11] & 0xffffu;
			if (coord >= 8u || (stage[11] & 0xffff0000u) != 0u) return false;
			if (stageUsesTexture(stage)) {
				if (s.textures[i].handle == s.target.handle ||
					!synchronizeGpuSurface(s.textures[i]) || !ensureTexture(s.textures[i], textures[i])) return false;
			}
		}
		Program* program = programFor(s);
		if (!program) return false;
		if (!g.vertexBuffer) glGenBuffers(1, &g.vertexBuffer);
		if (!g.vertexArray) glGenVertexArrays(1, &g.vertexArray);
		if (!g.vertexBuffer || !g.vertexArray || glGetError() != GL_NO_ERROR) return false;
		#if defined(BR_ANDROID_D3D7_TEXTURE_DIAGNOSTICS)
		logDrawTextureState(s,count,v);
		#endif
		g.pendingVertices.clear();
		if (!appendGpuVertices(g.pendingVertices, v, count)) return false;
		g.pendingState = s;
		g.pendingProgram = program;
		g.pendingTextures = textures;
		g.batchPending = true;
		return true;
	} catch (...) { return false; }
}

bool synchronizeGpuSurface(const GpuSurface& s) noexcept
{
	try {
		if (g.batchPending && (g.pendingState.target.handle == s.handle ||
			g.pendingState.depth.handle == s.handle) && !flushPending()) {
			logSyncFailure("pending_d3d_batch", s.handle, s.width, s.height, s.bitsPerPixel,
				s.pitch, s.byteCount, glGetError(), eglGetError());
			return false;
		}
		Target* pending = nullptr;
		bool colorPending = false;
		for (auto& entry : g.targets) {
			Target& target = entry.second;
			if (entry.first == s.handle && target.colorDirty) {
				pending = &target; colorPending = true; break;
			}
			if (target.depthView.handle == s.handle && target.depthDirty) {
				pending = &target; colorPending = false; break;
			}
		}
		// Most guest surface accesses are ordinary CPU accesses. Do not create or
		// bind an EGL context for surfaces the GPU backend has never modified.
		// In particular, an unsupported first GLES draw can safely fall back to
		// software without turning a no-op synchronization into a fatal error.
		if (!pending) return true;
		g.stateCache.invalidateDrawState();
		if (!ensureContext()) {
			logSyncFailure("egl_context_unavailable", s.handle, s.width, s.height, s.bitsPerPixel,
				s.pitch, s.byteCount, glGetError(), eglGetError());
			return false;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, pending->fbo);
		if (colorPending) {
			if (!supportedTarget(s) || s.width != pending->colorView.width ||
				s.height != pending->colorView.height) {
				logSyncFailure("invalid_color_surface_layout", s.handle, s.width, s.height, s.bitsPerPixel,
					s.pitch, s.byteCount, glGetError(), eglGetError());
				return false;
			}
			g.pixelData.resize(size_t(s.width) * s.height * 4u);
			auto& rgba = g.pixelData;
			glReadPixels(0, 0, GLsizei(s.width), GLsizei(s.height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
			const uint32_t bpp = s.bitsPerPixel / 8u;
			for (uint32_t y = 0; y < s.height; ++y) for (uint32_t x = 0; x < s.width; ++x) {
				const auto* p = rgba.data() + (size_t(s.height - 1u - y) * s.width + x) * 4u;
				uint32_t packed = encode(p[0], s.redMask) | encode(p[1], s.greenMask) |
					encode(p[2], s.blueMask) | encode(p[3], s.alphaMask);
				std::memcpy(s.pixels + size_t(y) * s.pitch + size_t(x) * bpp, &packed, bpp);
			}
			const GLenum error = glGetError();
			if (error != GL_NO_ERROR) {
				logSyncFailure("color_glReadPixels", s.handle, s.width, s.height, s.bitsPerPixel,
					s.pitch, s.byteCount, error, eglGetError());
				return false;
			}
			pending->colorDirty = false; pending->colorRevision = s.cpuRevision; pending->colorView = s;
			return true;
		}
		if (!s.width || !s.height || !s.pixels || s.bitsPerPixel != 16u ||
			s.width != pending->depthView.width || s.height != pending->depthView.height ||
			uint64_t(s.pitch) < uint64_t(s.width) * 2u ||
			uint64_t(s.height - 1u) * s.pitch + uint64_t(s.width) * 2u > s.byteCount) {
			logSyncFailure("invalid_depth_surface_layout", s.handle, s.width, s.height, s.bitsPerPixel,
				s.pitch, s.byteCount, glGetError(), eglGetError());
			return false;
		}
		// OpenGL ES 3 excludes DEPTH_COMPONENT from glReadPixels formats. Sample
		// the depth texture into a color scratch FBO and encode the guest Z16 value
		// across R/G, then read back the legal RGBA/UNSIGNED_BYTE pair.
		if (!ensureDepthReadTarget(*pending, s.width, s.height)) {
			logSyncFailure("depth_readback_target", s.handle, s.width, s.height, s.bitsPerPixel,
				s.pitch, s.byteCount, glGetError(), eglGetError());
			return false;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, pending->depthReadFbo);
		glViewport(0, 0, GLsizei(s.width), GLsizei(s.height));
		glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDisable(GL_BLEND);
		glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		const GLuint readProgram = depthReadProgram();
		glUseProgram(readProgram);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, pending->depth);
		glUniform1i(glGetUniformLocation(readProgram, "depthTex"), 0);
		glBindVertexArray(g.depthReadVao);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		g.pixelData.resize(size_t(s.width) * s.height * 4u);
		auto& encoded = g.pixelData;
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, GLsizei(s.width), GLsizei(s.height), GL_RGBA, GL_UNSIGNED_BYTE, encoded.data());
		for (uint32_t y = 0; y < s.height; ++y) for (uint32_t x = 0; x < s.width; ++x) {
			const auto* p = encoded.data() + (size_t(s.height - 1u - y) * s.width + x) * 4u;
			const uint16_t packed = static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
			std::memcpy(s.pixels + size_t(y) * s.pitch + size_t(x) * 2u, &packed, sizeof(packed));
		}
		const GLenum error = glGetError();
		if (error != GL_NO_ERROR) {
			logSyncFailure("depth_shader_readback", s.handle, s.width, s.height, s.bitsPerPixel,
				s.pitch, s.byteCount, error, eglGetError());
			return false;
		}
		pending->depthDirty = false; pending->depthRevision = s.cpuRevision; pending->depthView = s;
		return true;
	} catch (...) {
		logSyncFailure("cxx_exception", s.handle, s.width, s.height, s.bitsPerPixel,
			s.pitch, s.byteCount, glGetError(), eglGetError());
		return false;
	}
}
uint64_t consumeGpuBackendBatchSubmissions() noexcept
{
	const uint64_t count = g.batchSubmissions;
	g.batchSubmissions = 0u;
	return count;
}
uint64_t consumeGpuBackendSubmitNanoseconds() noexcept
{
	const uint64_t total = g.batchSubmitNanoseconds;
	g.batchSubmitNanoseconds = 0u;
	return total;
}
uint64_t consumeGpuBackendSkippedStateCalls() noexcept
{
	const uint64_t count = g.stateCache.skippedCalls;
	g.stateCache.skippedCalls = 0u;
	return count;
}
void markGpuSurfaceCpuWrite(uint32_t h) noexcept
{
	if (g.batchPending) {
		bool referenced = g.pendingState.target.handle == h || g.pendingState.depth.handle == h;
		for (uint32_t i = 0; i < g.pendingState.activeTextureStages && !referenced; ++i)
			referenced = g.pendingState.textures[i].handle == h;
		if (referenced && !flushPending())
			logSyncFailure("pending_batch_before_cpu_write", h, 0u, 0u, 0u, 0u, 0u,
				glGetError(), eglGetError());
	}
	for (auto& entry : g.targets) {
		Target& target = entry.second;
		if (entry.first == h) { target.colorRevision = UINT64_MAX; target.colorDirty = false; }
		if (target.depthView.handle == h) { target.depthRevision = UINT64_MAX; target.depthDirty = false; }
	}
	auto texture = g.textures.find(h);
	if (texture != g.textures.end()) texture->second.revision = UINT64_MAX;
}
void invalidateGpuSurface(uint32_t h) noexcept {markGpuSurfaceCpuWrite(h);}
void resetGpuRenderer() noexcept
{
	if(g.context!=EGL_NO_CONTEXT&&g.owner==std::this_thread::get_id()){flushPending();for(auto&e:g.targets){if(e.second.fbo)glDeleteFramebuffers(1,&e.second.fbo);if(e.second.depthReadFbo)glDeleteFramebuffers(1,&e.second.depthReadFbo);if(e.second.color)glDeleteTextures(1,&e.second.color);if(e.second.depth)glDeleteTextures(1,&e.second.depth);if(e.second.depthReadColor)glDeleteTextures(1,&e.second.depthReadColor);}for(auto&e:g.textures)if(e.second.id)glDeleteTextures(1,&e.second.id);for(auto&e:g.samplers)if(e.second)glDeleteSamplers(1,&e.second);for(auto&e:g.programs)if(e.second.handle)glDeleteProgram(e.second.handle);if(g.depthReadProgram)glDeleteProgram(g.depthReadProgram);if(g.depthReadVao)glDeleteVertexArrays(1,&g.depthReadVao);if(g.vertexArray)glDeleteVertexArrays(1,&g.vertexArray);if(g.vertexBuffer)glDeleteBuffers(1,&g.vertexBuffer);eglMakeCurrent(g.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(g.display,g.context);eglDestroySurface(g.display,g.surface);eglTerminate(g.display);}
	g=Renderer{};
}

} // namespace br::runtime::d3d7
