#ifndef GFX_RT64_CONTEXT_H
#define GFX_RT64_CONTEXT_H

#include "rt64/rt64.h"

#include "gfx_rendering_api_config.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <Windows.h>

#define MAX_GEO_LAYOUT_STACK_SIZE		32
#define CACHED_MESH_REQUIRED_FRAMES		5
#define CACHED_MESH_MAX_PER_FRAME		1
#define MAX_LIGHTS						512
#define MAX_LEVEL_LIGHTS				128
#define MAX_DYNAMIC_LIGHTS				MAX_LIGHTS - MAX_LEVEL_LIGHTS
#define MAX_LEVELS						40
#define MAX_AREAS						3

struct ShaderProgram {
    uint32_t shaderId;
    uint8_t numInputs;
    bool usedTextures[2];
	std::unordered_map<uint16_t, RT64_SHADER *> shaderVariantMap;
};

struct RecordedMesh {
	float *prevVertexBuffer = nullptr;
	uint64_t prevVertexBufferHash = 0;
	float *newVertexBuffer = nullptr;
	uint64_t newVertexBufferHash = 0;
	bool newVertexBufferValid = false;
	float *deltaVertexBuffer = nullptr;
    RT64_MESH *mesh = nullptr;
    uint32_t vertexCount = 0;
	uint32_t vertexStride = 0;
    uint32_t indexCount = 0;
	uint32_t staticFrames = 0;
	bool useTexture = false;
    bool raytrace = false;
};

struct DynamicMesh {
    RT64_MESH *mesh = nullptr;
    uint32_t vertexCount = 0;
	uint32_t vertexStride = 0;
    uint32_t indexCount = 0;
    bool raytrace = false;
	bool inUse = false;
};

struct RecordedInstance {
	RT64_INSTANCE *instance;
	RT64_INSTANCE_DESC desc;

	// Interpolated data.
	RT64_MATRIX4 prevTransform;
	RT64_MATRIX4 newTransform;
	RT64_RECT prevScissorRect;
	RT64_RECT newScissorRect;
	RT64_RECT prevViewportRect;
	RT64_RECT newViewportRect;
	bool prevValid = false;
	bool newValid = false;
};

struct RecordedTexture {
	RT64_TEXTURE *texture;
	bool linearFilter;
	uint32_t cms;
	uint32_t cmt;
	uint64_t hash;
};

struct RecordedMod {
    RT64_MATERIAL *materialMod = nullptr;
    RT64_LIGHT *lightMod = nullptr;
	uint64_t normalMapHash = 0;
	uint64_t specularMapHash = 0;
	bool interpolationEnabled = true;
};

struct RecordedCamera {
	RT64_MATRIX4 viewMatrix;
	RT64_MATRIX4 invViewMatrix;
	float fovRadians;
	float nearDist;
	float farDist;
};

struct RecordedDisplayList {
	std::vector<RecordedInstance> instances;
	std::vector<RecordedMesh> meshes;
	RT64_MATRIX4 prevTransform;
	RT64_MATRIX4 newTransform;
	bool prevValid = false;
	bool newValid = false;
	int newCount = 0;
};

struct RecordedLight {
	RT64_LIGHT prevLight;
	RT64_LIGHT newLight;
};

struct AreaLighting {
	RT64_SCENE_DESC sceneDesc;
	RT64_LIGHT lights[MAX_LEVEL_LIGHTS];
	int lightCount = 0;
};

//	Convention of bits for different lights.
//		1 	- Directional Tier A
//		2 	- Directional Tier B
//		4 	- Stage Tier A 
//		8 	- Stage Tier B
//		16 	- Objects Tier A
//		32 	- Objects Tier B
//		64 	- Particles Tier A
//		128 - Particles Tier B

struct RT64Context {
	// Window data.
	HWND hwnd = NULL;
	bool isFullScreen = false;
	bool lastMaximizedState = false;
	bool useVsync = true;
	bool cursorVisible = true;
	bool windowActive = true;
	RECT lastWindowRect;
	
	// Mouselook support.
	bool mouselookEnabled = false;
	int deltaMouseX = 0;
	int deltaMouseY = 0;

	// Game data.
	RT64_MATERIAL defaultMaterial;
	RT64_TEXTURE *blankTexture = nullptr;
	AreaLighting levelAreaLighting[MAX_LEVELS][MAX_AREAS];
	std::unordered_map<void *, std::string> geoLayoutNameMap;
	std::map<std::string, void *> nameGeoLayoutMap;
	std::unordered_map<void *, RecordedMod *> geoLayoutMods;
	std::unordered_map<uint64_t, std::string> texNameMap;
	std::map<std::string, uint64_t> nameTexMap;
	std::unordered_map<uint64_t, RecordedMod *> texMods;
	std::map<uint64_t, uint64_t> texHashAliasMap;
	std::map<uint64_t, std::vector<uint64_t>> texHashAliasesMap;
	std::unordered_map<uint32_t, uint64_t> textureHashIdMap;
	
	// Runtime data.
	RT64_LIBRARY lib;
	RT64_DEVICE *device = nullptr;
	RT64_INSPECTOR *inspector = nullptr;
	RT64_SCENE *scene = nullptr;
	RT64_VIEW *view = nullptr;
	std::unordered_map<uint32_t, RecordedTexture> textures;
	std::unordered_map<uint32_t, ShaderProgram *> shaderPrograms;
	std::unordered_map<uint32_t, RecordedDisplayList> displayLists;
	std::unordered_map<void *, RecordedMod *> graphNodeMods;
	std::unordered_map<uint64_t, RT64_MESH *> staticMeshCache;
	std::unordered_map<uint64_t, DynamicMesh> dynamicMeshPool;
	unsigned int indexTriangleList[GFX_MAX_BUFFERED];
	RT64_LIGHT lights[MAX_LIGHTS];
    unsigned int lightCount;
    RecordedLight dynamicLights[MAX_DYNAMIC_LIGHTS];
    unsigned int dynamicLightCount;

	// Ray picking data.
	bool pickTextureNextFrame;
	bool pickTextureHighlight;
	uint64_t pickedTextureHash;
	std::unordered_map<RT64_INSTANCE *, uint64_t> lastInstanceTextureHashes;

	// Camera.
	RecordedCamera camera;
	RecordedCamera prevCamera;
	bool prevCameraValid = false;

	// Matrices.
	RT64_MATRIX4 identityTransform;

	// Rendering state.
	int currentTile;
    uint32_t currentTextureIds[2];
	ShaderProgram *shaderProgram;
	bool background;
	RT64_VECTOR3 fogColor;
	RT64_VECTOR3 skyboxDiffuseMultiplier;
	RT64_RECT scissorRect;
	RT64_RECT viewportRect;
	int16_t fogMul;
	int16_t fogOffset;
	RecordedMod *graphNodeMod;

	// Timing.
	std::vector<double> prevFrametimes;
	unsigned int targetFPS = 30;
	LARGE_INTEGER StartingTime, EndingTime;
	LARGE_INTEGER Frequency;
	bool dropNextFrame;
	bool pauseMode;
	bool turboMode;

	// Function pointers for game.
    void (*run_one_game_iter)(void);
    bool (*on_key_down)(int scancode);
    bool (*on_key_up)(int scancode);
    void (*on_all_keys_up)(void);
};

extern RT64Context RT64;

#endif
