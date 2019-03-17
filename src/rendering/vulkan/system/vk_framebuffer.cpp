// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2010-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//

#include "volk/volk.h"

#include "v_video.h"
#include "m_png.h"
#include "templates.h"
#include "r_videoscale.h"
#include "actor.h"
#include "i_time.h"
#include "g_game.h"
#include "gamedata/fonts/v_text.h"

#include "hwrenderer/utility/hw_clock.h"
#include "hwrenderer/utility/hw_vrmodes.h"
#include "hwrenderer/utility/hw_cvars.h"
#include "hwrenderer/models/hw_models.h"
#include "hwrenderer/scene/hw_skydome.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/flatvertices.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hwrenderer/dynlights/hw_lightbuffer.h"

#include "swrenderer/r_swscene.h"

#include "vk_framebuffer.h"
#include "vk_buffers.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_renderbuffers.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/system/vk_builders.h"
#include "vulkan/system/vk_swapchain.h"
#include "doomerrors.h"

void Draw2D(F2DDrawer *drawer, FRenderState &state);
void DoWriteSavePic(FileWriter *file, ESSType ssformat, uint8_t *scr, int width, int height, sector_t *viewsector, bool upsidedown);

EXTERN_CVAR(Bool, vid_vsync)
EXTERN_CVAR(Bool, r_drawvoxels)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Bool, gl_no_skyclear)

extern bool NoInterpolateView;

VulkanFrameBuffer::VulkanFrameBuffer(void *hMonitor, bool fullscreen, VulkanDevice *dev) : 
	Super(hMonitor, fullscreen) 
{
	device = dev;

	swapChain = std::make_unique<VulkanSwapChain>(device);
	mSwapChainImageAvailableSemaphore.reset(new VulkanSemaphore(device));
	mRenderFinishedSemaphore.reset(new VulkanSemaphore(device));
	mRenderFinishedFence.reset(new VulkanFence(device));

	InitPalette();
}

VulkanFrameBuffer::~VulkanFrameBuffer()
{
	// All descriptors must be destroyed before the descriptor pool in renderpass manager is destroyed
	for (VkHardwareTexture *cur = VkHardwareTexture::First; cur; cur = cur->Next)
		cur->Reset();

	PPResource::ResetAll();

	delete MatricesUBO;
	delete StreamUBO;
	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	mShadowMap.Reset();

	DeleteFrameObjects();
}

void VulkanFrameBuffer::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	gl_vendorstring = "Vulkan";
	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.limits.maxUniformBufferRange;

	mUploadSemaphore.reset(new VulkanSemaphore(device));
	mGraphicsCommandPool.reset(new VulkanCommandPool(device, device->graphicsFamily));

	mScreenBuffers.reset(new VkRenderBuffers());
	mSaveBuffers.reset(new VkRenderBuffers());
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess());
	mRenderPassManager.reset(new VkRenderPassManager());

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new GLViewpointBuffer;
	mLights = new FLightBuffer();

	CreateFanToTrisIndexBuffer();

	// To do: move this to HW renderer interface maybe?
	MatricesUBO = (VKDataBuffer*)CreateDataBuffer(-1, false);
	StreamUBO = (VKDataBuffer*)CreateDataBuffer(-1, false);
	MatricesUBO->SetData(UniformBufferAlignedSize<::MatricesUBO>() * 50000, nullptr, false);
	StreamUBO->SetData(UniformBufferAlignedSize<::StreamUBO>() * 200, nullptr, false);

	mShaderManager.reset(new VkShaderManager(device));
	mSamplerManager.reset(new VkSamplerManager(device));
	mRenderPassManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten());
#else
	mRenderState.reset(new VkRenderState());
#endif
}

void VulkanFrameBuffer::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	int newWidth = GetClientWidth();
	int newHeight = GetClientHeight();
	if (lastSwapWidth != newWidth || lastSwapHeight != newHeight)
	{
		swapChain.reset();
		swapChain = std::make_unique<VulkanSwapChain>(device);

		lastSwapWidth = newWidth;
		lastSwapHeight = newHeight;
	}

	VkResult result = vkAcquireNextImageKHR(device->device, swapChain->swapChain, std::numeric_limits<uint64_t>::max(), mSwapChainImageAvailableSemaphore->semaphore, VK_NULL_HANDLE, &presentImageIndex);
	if (result != VK_SUCCESS)
		throw std::runtime_error("Failed to acquire next image!");

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	Clear2D();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	mPostprocess->DrawPresentTexture(mOutputLetterbox, true, true);

	SubmitCommands(true);

	Flush3D.Unclock();

	Finish.Reset();
	Finish.Clock();

	VkSemaphore waitSemaphores[] = { mRenderFinishedSemaphore->semaphore };
	VkSwapchainKHR swapChains[] = { swapChain->swapChain };
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = waitSemaphores;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &presentImageIndex;
	presentInfo.pResults = nullptr;
	vkQueuePresentKHR(device->presentQueue, &presentInfo);

	vkWaitForFences(device->device, 1, &mRenderFinishedFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(device->device, 1, &mRenderFinishedFence->fence);

	mDrawCommands.reset();
	mUploadCommands.reset();
	DeleteFrameObjects();

	Finish.Unclock();

	Super::Update();
}

void VulkanFrameBuffer::DeleteFrameObjects()
{
	FrameDeleteList.Images.clear();
	FrameDeleteList.ImageViews.clear();
	FrameDeleteList.Buffers.clear();
	FrameDeleteList.Descriptors.clear();
}

void VulkanFrameBuffer::SubmitCommands(bool finish)
{
	mDrawCommands->end();

	if (mUploadCommands)
	{
		mUploadCommands->end();

		// Submit upload commands immediately
		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &mUploadCommands->buffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &mUploadSemaphore->semaphore;
		VkResult result = vkQueueSubmit(device->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		if (result < VK_SUCCESS)
			I_FatalError("Failed to submit command buffer! Error %d\n", result);

		// Wait for upload commands to finish, then submit render commands
		VkSemaphore waitSemaphores[] = { mUploadSemaphore->semaphore, mSwapChainImageAvailableSemaphore->semaphore };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = finish ? 2 : 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &mDrawCommands->buffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &mRenderFinishedSemaphore->semaphore;
		result = vkQueueSubmit(device->graphicsQueue, 1, &submitInfo, mRenderFinishedFence->fence);
		if (result < VK_SUCCESS)
			I_FatalError("Failed to submit command buffer! Error %d\n", result);
	}
	else
	{
		VkSemaphore waitSemaphores[] = { mSwapChainImageAvailableSemaphore->semaphore };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = finish ? 1 : 0;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &mDrawCommands->buffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &mRenderFinishedSemaphore->semaphore;
		VkResult result = vkQueueSubmit(device->graphicsQueue, 1, &submitInfo, mRenderFinishedFence->fence);
		if (result < VK_SUCCESS)
			I_FatalError("Failed to submit command buffer! Error %d\n", result);
	}

	if (!finish)
	{
		vkWaitForFences(device->device, 1, &mRenderFinishedFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(device->device, 1, &mRenderFinishedFence->fence);
		mDrawCommands.reset();
		mUploadCommands.reset();
		DeleteFrameObjects();
	}
}

void VulkanFrameBuffer::WriteSavePic(player_t *player, FileWriter *file, int width, int height)
{
	if (!V_IsHardwareRenderer())
	{
		Super::WriteSavePic(player, file, width, height);
	}
	else
	{
		IntRect bounds;
		bounds.left = 0;
		bounds.top = 0;
		bounds.width = width;
		bounds.height = height;

		// we must be sure the GPU finished reading from the buffer before we fill it with new data.
		if (mDrawCommands)
			SubmitCommands(false);

		// Switch to render buffers dimensioned for the savepic
		mActiveRenderBuffers = mSaveBuffers.get();

		hw_ClearFakeFlat();
		GetRenderState()->SetVertexBuffer(screen->mVertexData);
		screen->mVertexData->Reset();
		screen->mLights->Clear();
		screen->mViewpoints->Clear();

		// This shouldn't overwrite the global viewpoint even for a short time.
		FRenderViewpoint savevp;
		sector_t *viewsector = RenderViewpoint(savevp, players[consoleplayer].camera, &bounds, r_viewpoint.FieldOfView.Degrees, 1.6f, 1.6f, true, false);
		GetRenderState()->EnableStencil(false);
		GetRenderState()->SetNoSoftLightLevel();

		int numpixels = width * height;
		uint8_t * scr = (uint8_t *)M_Malloc(numpixels * 3);
		CopyScreenToBuffer(width, height, scr);

		DoWriteSavePic(file, SS_RGB, scr, width, height, viewsector, false);
		M_Free(scr);

		// Switch back the screen render buffers
		screen->SetViewportRects(nullptr);
		mActiveRenderBuffers = mScreenBuffers.get();
	}
}

sector_t *VulkanFrameBuffer::RenderView(player_t *player)
{
	// To do: this is virtually identical to FGLRenderer::RenderView and should be merged.

	mRenderState->SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();

	sector_t *retsec;
	if (!V_IsHardwareRenderer())
	{
		if (!swdrawer) swdrawer.reset(new SWSceneDrawer);
		retsec = swdrawer->RenderView(player);
	}
	else
	{
		hw_ClearFakeFlat();

		iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;

		checkBenchActive();

		// reset statistics counters
		ResetProfilingData();

		// Get this before everything else
		if (cl_capfps || r_NoInterpolate) r_viewpoint.TicFrac = 1.;
		else r_viewpoint.TicFrac = I_GetTimeFrac();

		screen->mLights->Clear();
		screen->mViewpoints->Clear();

		// NoInterpolateView should have no bearing on camera textures, but needs to be preserved for the main view below.
		bool saved_niv = NoInterpolateView;
		NoInterpolateView = false;

		// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
		GetRenderState()->CheckTimer(player->camera->Level->ShaderStartTime);
		// prepare all camera textures that have been used in the last frame.
		// This must be done for all levels, not just the primary one!
		for (auto Level : AllLevels())
		{
			Level->canvasTextureInfo.UpdateAll([&](AActor *camera, FCanvasTexture *camtex, double fov)
			{
				RenderTextureView(camtex, camera, fov);
			});
		}
		NoInterpolateView = saved_niv;

		// now render the main view
		float fovratio;
		float ratio = r_viewwindow.WidescreenRatio;
		if (r_viewwindow.WidescreenRatio >= 1.3f)
		{
			fovratio = 1.333333f;
		}
		else
		{
			fovratio = ratio;
		}

		mPostprocess->ImageTransitionScene(true); // This is the only line that differs compared to FGLRenderer::RenderView

		retsec = RenderViewpoint(r_viewpoint, player->camera, NULL, r_viewpoint.FieldOfView.Degrees, ratio, fovratio, true, true);
	}
	All.Unclock();
	return retsec;
}

sector_t *VulkanFrameBuffer::RenderViewpoint(FRenderViewpoint &mainvp, AActor * camera, IntRect * bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	// To do: this is virtually identical to FGLRenderer::RenderViewpoint and should be merged.

	R_SetupFrame(mainvp, r_viewwindow, camera);

	if (mainview && toscreen)
		UpdateShadowMap();

	// Update the attenuation flag of all light defaults for each viewpoint.
	// This function will only do something if the setting differs.
	FLightDefaults::SetAttenuationForLevel(!!(camera->Level->flags3 & LEVEL3_ATTENUATE));

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	for (int eye_ix = 0; eye_ix < vrmode->mEyeCount; ++eye_ix)
	{
		const auto &eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			mRenderState->SetRenderTarget(GetBuffers()->SceneColorView.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), GetBuffers()->GetSceneSamples());
			bool useSSAO = (gl_ssao != 0);
			GetRenderState()->SetPassType(useSSAO ? GBUFFER_PASS : NORMAL_PASS);
			GetRenderState()->EnableDrawBuffers(GetRenderState()->GetPassDrawBufferCount());
		}

		auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
		auto &vp = di->Viewpoint;

		di->Set3DViewport(*GetRenderState());
		di->SetViewArea();
		auto cm = di->SetFullbrightFlags(mainview ? vp.camera->player : nullptr);
		di->Viewpoint.FieldOfView = fov;	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		// Stereo mode specific perspective projection
		di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio);
		// Stereo mode specific viewpoint adjustment
		vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees);
		di->SetupView(*GetRenderState(), vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);

		// std::function until this can be done better in a cross-API fashion.
		di->ProcessScene(toscreen, [&](HWDrawInfo *di, int mode) {
			DrawScene(di, mode);
		});

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(mainvp.sector, *GetRenderState()); // do not call this for camera textures.

			if (GetRenderState()->GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				GetRenderState()->SetPassType(NORMAL_PASS);
				GetRenderState()->EnableDrawBuffers(1);
			}

			mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture

			PostProcessScene(cm, [&]() { di->DrawEndScene2D(mainvp.sector, *GetRenderState()); });

			PostProcess.Unclock();
		}
		di->EndDrawInfo();

#if 0
		if (vrmode->mEyeCount > 1)
			mBuffers->BlitToEyeTexture(eye_ix);
#endif
	}

	return mainvp.sector;
}

void VulkanFrameBuffer::RenderTextureView(FCanvasTexture *tex, AActor *Viewpoint, double FOV)
{
	// This doesn't need to clear the fake flat cache. It can be shared between camera textures and the main view of a scene.
	FMaterial *mat = FMaterial::ValidateTexture(tex, false);
	auto BaseLayer = static_cast<VkHardwareTexture*>(mat->GetLayer(0, 0));

	int width = mat->TextureWidth();
	int height = mat->TextureHeight();
	VulkanImage *image = BaseLayer->GetImage(tex, 0, 0);
	VulkanImageView *view = BaseLayer->GetImageView(tex, 0, 0);

	mRenderState->EndRenderPass();
	auto cmdbuffer = GetDrawCommands();

	PipelineBarrier barrier0;
	barrier0.addImage(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	barrier0.execute(cmdbuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	mRenderState->SetRenderTarget(view, image->width, image->height, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = MIN(mat->GetWidth(), image->width);
	bounds.height = MIN(mat->GetHeight(), image->height);

	FRenderViewpoint texvp;
	RenderViewpoint(texvp, Viewpoint, &bounds, FOV, (float)width / height, (float)width / height, false, false);

	mRenderState->EndRenderPass();

	PipelineBarrier barrier1;
	barrier1.addImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier1.execute(cmdbuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	mRenderState->SetRenderTarget(GetBuffers()->SceneColorView.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

void VulkanFrameBuffer::DrawScene(HWDrawInfo *di, int drawmode)
{
	// To do: this is virtually identical to FGLRenderer::DrawScene and should be merged.

	static int recursion = 0;
	static int ssao_portals_available = 0;
	const auto &vp = di->Viewpoint;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = true;
		ssao_portals_available--;
	}

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		di->CreateScene(drawmode == DM_MAINVIEW);
		vp.camera->renderflags = savedflags;
	}
	else
	{
		di->CreateScene(false);
	}

	GetRenderState()->SetDepthMask(true);
	if (!gl_no_skyclear) screen->mPortalState->RenderFirstSkyPortal(recursion, di, *GetRenderState());

	di->RenderScene(*GetRenderState());

	if (applySSAO && GetRenderState()->GetPassType() == GBUFFER_PASS)
	{
		mPostprocess->AmbientOccludeScene(di->VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(*GetRenderState(), di->vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	recursion++;
	screen->mPortalState->EndFrame(di, *GetRenderState());
	recursion--;
	di->RenderTranslucent(*GetRenderState());
}

void VulkanFrameBuffer::PostProcessScene(int fixedcm, const std::function<void()> &afterBloomDrawEndScene2D)
{
	mPostprocess->PostProcessScene(fixedcm, afterBloomDrawEndScene2D);
}

uint32_t VulkanFrameBuffer::GetCaps()
{
	if (!V_IsHardwareRenderer())
		return Super::GetCaps();

	// describe our basic feature set
	ActorRenderFeatureFlags FlagSet = RFF_FLATSPRITES | RFF_MODELS | RFF_SLOPE3DFLOORS |
		RFF_TILTPITCH | RFF_ROLLSPRITES | RFF_POLYGONAL | RFF_MATSHADER | RFF_POSTSHADER | RFF_BRIGHTMAP;
	if (r_drawvoxels)
		FlagSet |= RFF_VOXELS;

	if (gl_tonemap != 5) // not running palette tonemap shader
		FlagSet |= RFF_TRUECOLOR;

	return (uint32_t)FlagSet;
}

void VulkanFrameBuffer::SetVSync(bool vsync)
{
	if (swapChain->vsync != vsync)
	{
		swapChain.reset();
		swapChain = std::make_unique<VulkanSwapChain>(device);
	}
}

void VulkanFrameBuffer::CleanForRestart()
{
	// force recreation of the SW scene drawer to ensure it gets a new set of resources.
	swdrawer.reset();
}

IHardwareTexture *VulkanFrameBuffer::CreateHardwareTexture()
{
	return new VkHardwareTexture();
}

FModelRenderer *VulkanFrameBuffer::CreateModelRenderer(int mli) 
{
	return new FGLModelRenderer(nullptr, *GetRenderState(), mli);
}

IVertexBuffer *VulkanFrameBuffer::CreateVertexBuffer()
{
	return new VKVertexBuffer();
}

IIndexBuffer *VulkanFrameBuffer::CreateIndexBuffer()
{
	return new VKIndexBuffer();
}

IDataBuffer *VulkanFrameBuffer::CreateDataBuffer(int bindingpoint, bool ssbo)
{
	auto buffer = new VKDataBuffer(bindingpoint, ssbo);

	auto fb = GetVulkanFrameBuffer();
	switch (bindingpoint)
	{
	case LIGHTBUF_BINDINGPOINT: LightBufferSSO = buffer; break;
	case VIEWPOINT_BINDINGPOINT: ViewpointUBO = buffer; break;
	case LIGHTNODES_BINDINGPOINT: LightNodes = buffer; break;
	case LIGHTLINES_BINDINGPOINT: LightLines = buffer; break;
	case LIGHTLIST_BINDINGPOINT: LightList = buffer; break;
	case POSTPROCESS_BINDINGPOINT: break;
	default: break;
	}

	return buffer;
}

void VulkanFrameBuffer::SetTextureFilterMode()
{
	TextureFilterChanged();
}

void VulkanFrameBuffer::TextureFilterChanged()
{
	if (mSamplerManager)
	{
		// Destroy the texture descriptors as they used the old samplers
		for (VkHardwareTexture *cur = VkHardwareTexture::First; cur; cur = cur->Next)
			cur->ResetDescriptors();

		mSamplerManager->SetTextureFilterMode();
	}
}

void VulkanFrameBuffer::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanFrameBuffer::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanFrameBuffer::WipeStartScreen()
{
	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(viewport.width, viewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanFrameBuffer::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	Clear2D();

	const auto &viewport = screen->mScreenViewport;
	auto tex = new FWrapperTexture(viewport.width, viewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(viewport.width, viewport.height, "WipeEndScreen");

	return tex;
}

void VulkanFrameBuffer::CopyScreenToBuffer(int w, int h, void *data)
{
	// Convert from rgba16f to rgba8 using the GPU:
	ImageBuilder imgbuilder;
	imgbuilder.setFormat(VK_FORMAT_R8G8B8A8_UNORM);
	imgbuilder.setUsage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	imgbuilder.setSize(w, h);
	auto image = imgbuilder.create(device);
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	GetPostprocess()->BlitCurrentToImage(image.get(), &layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	BufferBuilder bufbuilder;
	bufbuilder.setSize(w * h * 4);
	bufbuilder.setUsage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	auto staging = bufbuilder.create(device);

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	GetDrawCommands()->copyImageToBuffer(image->image, layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	SubmitCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

TArray<uint8_t> VulkanFrameBuffer::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 2.2f;
	return ScreenshotBuffer;
}

void VulkanFrameBuffer::BeginFrame()
{
	SetViewportRects(nullptr);
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mPostprocess->BeginFrame();
	mRenderState->BeginFrame();
	mRenderPassManager->UpdateDynamicSet();
}

void VulkanFrameBuffer::Draw2D()
{
	::Draw2D(&m2DDrawer, *mRenderState);
}

VulkanCommandBuffer *VulkanFrameBuffer::GetUploadCommands()
{
	if (!mUploadCommands)
	{
		mUploadCommands = mGraphicsCommandPool->createBuffer();
		mUploadCommands->SetDebugName("VulkanFrameBuffer.mUploadCommands");
		mUploadCommands->begin();
	}
	return mUploadCommands.get();
}

VulkanCommandBuffer *VulkanFrameBuffer::GetDrawCommands()
{
	if (!mDrawCommands)
	{
		mDrawCommands = mGraphicsCommandPool->createBuffer();
		mDrawCommands->SetDebugName("VulkanFrameBuffer.mDrawCommands");
		mDrawCommands->begin();
	}
	return mDrawCommands.get();
}

unsigned int VulkanFrameBuffer::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

void VulkanFrameBuffer::PrintStartupLog()
{
	const auto props = device->PhysicalDevice.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %d\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanFrameBuffer::CreateFanToTrisIndexBuffer()
{
	TArray<uint32_t> data;
	for (int i = 2; i < 1000; i++)
	{
		data.Push(0);
		data.Push(i - 1);
		data.Push(i);
	}

	FanToTrisIndexBuffer.reset(CreateIndexBuffer());
	FanToTrisIndexBuffer->SetData(sizeof(uint32_t) * data.Size(), data.Data());
}

void VulkanFrameBuffer::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}