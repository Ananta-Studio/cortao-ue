// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// CORTAO is by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO
//
// CORTAO ambient occlusion on the engine's global illumination plugin hook.
//
// Up to five compute passes per view: prepass (linear depth + packed normal, 8 mips), main (the occluder
// trace), filter (optional sparse blur), temporal (optional accumulation, this port's own) and composite
// (strength / power into the engine's own ScreenSpaceAO texture). The engine's SSAO must still run for that
// texture to exist; a project keeps it at its cheapest level while CORTAO is on (r.AmbientOcclusionLevels 1,
// r.AmbientOcclusionMaxQuality 0), see the README.

#include "CORTAORendering.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "DeferredShadingRenderer.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "SceneViewState.h"
#include "SceneTextures.h"
#include "SystemTextures.h"
#include "ShaderParameterStruct.h"
#include "ShaderPermutation.h"

DEFINE_LOG_CATEGORY(LogCORTAO);

namespace CORTAO
{
	#include "CORTAOOccluderTable.inl"

	static TAutoConsoleVariable<int32> CVarEnable(
		TEXT("r.CORTAO"), 0,
		TEXT("CORTAO ambient occlusion in place of the engine's SSAO result. Needs the view's Global Illumination method to be Plugin ")
		TEXT("(r.DynamicGlobalIlluminationMethod 3) and the engine's SSAO enabled, which supplies the texture (r.AmbientOcclusionLevels 1 ")
		TEXT("and r.AmbientOcclusionMaxQuality 0 keep it cheap).\n")
		TEXT("0: off (default), 1: on"),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarSamples(
		TEXT("r.CORTAO.Samples"), 16,
		TEXT("Occluder samples per pixel: 4, 6, 8, 12, 16, 24 or 32 (others round to the nearest). Cost grows about linearly."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarRadius(
		TEXT("r.CORTAO.Radius"), 150.0f,
		TEXT("World-space radius of the occlusion search, in cm."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarScreenRadius(
		TEXT("r.CORTAO.ScreenRadius"), 0.2f,
		TEXT("Largest search radius on screen, as a fraction of the screen height (0.1 .. 0.25 recommended). Keeps close-up surfaces from sampling the whole screen."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarMipBias(
		TEXT("r.CORTAO.MipBias"), -4.0f,
		TEXT("Bias on the depth mip the samples read. Lower is sharper and slower (cache misses); higher is blurrier and faster."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarSlopeCompensation(
		TEXT("r.CORTAO.SlopeCompensation"), 4.0f,
		TEXT("Largest growth of an occluder seen at a grazing angle. Higher darkens creases on slanted surfaces more."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarBackfaceThreshold(
		TEXT("r.CORTAO.BackfaceThreshold"), 0.5f,
		TEXT("How much an occluder must face away to count as a backface (dot of its normal and the direction to it)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarBackfaceBoost(
		TEXT("r.CORTAO.BackfaceBoost"), 0.0f,
		TEXT("0..1: how much backface occluders are enlarged to stop light leaking through thin walls."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFoliageReceive(
		TEXT("r.CORTAO.FoliageReceive"), 1.0f,
		TEXT("0..1: how much CORTAO darkens foliage pixels (the prepass flag: Two Sided Foliage, the foliage lighting channel). ")
		TEXT("1: as every other pixel; 0: foliage receives no occlusion (it still occludes its surroundings)."),
		ECVF_RenderThreadSafe);

	// Foliage (see CORTAOMainPass.usf for why stylised grass and leaves come out dark and stippled without this). A pixel
	// is foliage when drawn with Two Sided Foliage or on one of r.CORTAO.FoliageChannels' lighting channels.
	static TAutoConsoleVariable<int32> CVarFoliageChannels(
		TEXT("r.CORTAO.FoliageChannels"), 4,
		TEXT("Lighting-channel mask that marks foliage for CORTAO besides the Two Sided Foliage shading model (bit n = channel n; ")
		TEXT("default 4 = channel 2). Lights all use channel 0, so a grass type on channels 0 and 2 is lit as before. 0: none."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFoliageMismatch(
		TEXT("r.CORTAO.FoliageMismatch"), 0.0f,
		TEXT("Foliage without a marker: a pixel whose shading normal is this far from the geometry's (|cos| below the value) ")
		TEXT("is foliage -- stylised grass lights upright blades with normals pointing up. 0: off; ~0.35 catches grass cards."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarFoliageDebug(
		TEXT("r.CORTAO.FoliageDebug"), 0,
		TEXT("1: the AO buffer shows foliage pixels (CORTAO's flag) as near black."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarPlaneReject(
		TEXT("r.CORTAO.PlaneReject"), 1,
		TEXT("An occluder must also lie above the receiver's geometric plane (from depth), so a flat blade or leaf card lit with ")
		TEXT("made-up normals does not occlude itself. 0: off, 1: foliage receivers, 2: every receiver."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarPlaneRejectThreshold(
		TEXT("r.CORTAO.PlaneRejectThreshold"), 0.1f,
		TEXT("Sine of the angle above the geometric plane an occluder needs for r.CORTAO.PlaneReject."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFoliageSelfRadius(
		TEXT("r.CORTAO.FoliageSelfRadius"), 30.0f,
		TEXT("cm: a foliage pixel ignores foliage occluders nearer than this (its own clump). 0: off."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFoliageOpacity(
		TEXT("r.CORTAO.FoliageOpacity"), 0.5f,
		TEXT("0..1: how opaque a foliage occluder is (its disc area is scaled by this: blades and leaves have gaps and let light through). 1: as CORTAO."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarFoliageSlope(
		TEXT("r.CORTAO.FoliageSlope"), 0,
		TEXT("Slope compensation for foliage occluders. 1: as CORTAO, 0: none (their shading normal is not their geometry and enlarged them as if seen edge-on)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarTemporalClamp(
		TEXT("r.CORTAO.TemporalClamp"), 1,
		TEXT("Where the temporal depth test fails, keep last frame's value clamped into this frame's 3x3 range instead of dropping it. ")
		TEXT("0: off, 1: foliage pixels, 2: everywhere."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarFilter(
		TEXT("r.CORTAO.Filter"), 2,
		TEXT("Spatial filter ahead of the temporal one. 0: off, 1: small, 2: large (default)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFilterDepthSensitivity(
		TEXT("r.CORTAO.FilterDepthSensitivity"), 512.0f,
		TEXT("How strongly the filter stops at depth edges."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarFilterNormalSensitivity(
		TEXT("r.CORTAO.FilterNormalSensitivity"), 128.0f,
		TEXT("How strongly the filter stops at normal edges (128 is CORTAO 1.0.3's recommendation; lower blurs across more of a curved or normal-mapped surface)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarTemporal(
		TEXT("r.CORTAO.Temporal"), 1,
		TEXT("CORTAO's own temporal accumulation (CORTAO ships none and leaves its noise to TAA/TSR). 0: off, 1: on (default)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarTemporalWeight(
		TEXT("r.CORTAO.TemporalWeight"), 0.9f,
		TEXT("0..0.98: the share of last frame's accumulated occlusion kept each frame. Higher is smoother and slower to react."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarTemporalDepthTolerance(
		TEXT("r.CORTAO.TemporalDepthTolerance"), 0.05f,
		TEXT("Relative depth difference above which last frame's value is thrown away (disocclusion)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<int32> CVarDepthNormals(
		TEXT("r.CORTAO.DepthNormals"), 0,
		TEXT("Surface normals for CORTAO. 0: the GBuffer's (normal maps included), 1: rebuilt from the depth buffer (the geometry only)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarIntensity(
		TEXT("r.CORTAO.Intensity"), 1.0f,
		TEXT("0..1: blend from no occlusion (0) to CORTAO's full, physically based result (1)."),
		ECVF_RenderThreadSafe);

	static TAutoConsoleVariable<float> CVarPower(
		TEXT("r.CORTAO.Power"), 1.0f,
		TEXT("Exponent on the occlusion (1 is physically based; above 1 darkens and adds contrast)."),
		ECVF_RenderThreadSafe);

	/**
	 * "r.CORTAO 1 and nothing happens" is nearly always the Global Illumination method: the hook this plugin renders
	 * from only fires for views whose method is Plugin. Said once per change, from the game thread, and not before the
	 * engine has finished starting: the renderer settings arrive after [SystemSettings], so the method reads 0 for a while.
	 */
	static bool GEngineStarted = false;

	static void WarnAboutGlobalIlluminationMethod()
	{
		if (!GEngineStarted)
		{
			return;
		}
		static int32 LastState = -1;
		static const IConsoleVariable* MethodCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
		const int32 Method = MethodCVar ? MethodCVar->GetInt() : 3;
		const int32 State = CVarEnable.GetValueOnGameThread() == 0 ? 0 : (Method == 3 ? 1 : 2);
		if (State != LastState)
		{
			LastState = State;
			if (State == 2)
			{
				UE_LOG(LogCORTAO, Warning, TEXT("r.CORTAO is on, but r.DynamicGlobalIlluminationMethod is %d. CORTAO only renders for views whose ")
					TEXT("Global Illumination method is Plugin (3): set it in Project Settings > Rendering > Global Illumination, or in a ")
					TEXT("post-process volume (in which case ignore this)."), Method);
			}
		}
	}

	static FAutoConsoleVariableSink CVarSink(FConsoleCommandDelegate::CreateStatic(&WarnAboutGlobalIlluminationMethod));

	void OnEngineStarted()
	{
		GEngineStarted = true;
		WarnAboutGlobalIlluminationMethod();
	}

	constexpr int32 MipCount = 8;
	constexpr int32 MinViewSize = 1 << (MipCount - 1); // every mip at least 1 x 1

	class FPrepassCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FPrepassCS);
		SHADER_USE_PARAMETER_STRUCT(FPrepassCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferBTexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, LightingChannelsTexture)
			SHADER_PARAMETER(uint32, FoliageChannelMask)
			SHADER_PARAMETER(float, FoliageMismatch)
			SHADER_PARAMETER(FUintVector2, ViewRectMin)
			SHADER_PARAMETER(uint32, PackedResolution)
			SHADER_PARAMETER(float, DepthScale)
			SHADER_PARAMETER(float, DepthOffset)
			SHADER_PARAMETER(uint32, RandomSeed)
			SHADER_PARAMETER(uint32, MipCount)
			SHADER_PARAMETER(uint32, UseDepthNormals)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip0)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip1)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip2)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip3)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip4)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip5)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip6)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, PackedMip7)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FMainCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FMainCS);
		SHADER_USE_PARAMETER_STRUCT(FMainCS, FGlobalShader);

		class FSampleCount : SHADER_PERMUTATION_SPARSE_INT("CORTAO_SAMPLE_COUNT", 4, 6, 8, 12, 16, 24, 32);
		using FPermutationDomain = TShaderPermutationDomain<FSampleCount>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER(FUintVector2, ViewRectMin)
			SHADER_PARAMETER(uint32, UseDepthNormals)
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PackedData)
			SHADER_PARAMETER_SAMPLER(SamplerState, PointBorderSampler)
			SHADER_PARAMETER(FUintVector2, PackedSize)
			SHADER_PARAMETER(uint32, MipCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, OccluderTable)
			SHADER_PARAMETER(uint32, PackedResolution)
			SHADER_PARAMETER(uint32, RandomSeed)
			SHADER_PARAMETER(float, NormalBias)
			SHADER_PARAMETER(float, Radius)
			SHADER_PARAMETER(FVector4f, UnprojectParams)
			SHADER_PARAMETER(FVector4f, ProjectionParams)
			SHADER_PARAMETER(float, MaxApparentCircleRadiusCoefficient)
			SHADER_PARAMETER(float, MipBias)
			SHADER_PARAMETER(float, InverseMaxSlopeCompensation)
			SHADER_PARAMETER(float, BackfaceThreshold)
			SHADER_PARAMETER(float, BackfaceBoost)
			SHADER_PARAMETER(uint32, PlaneRejectMode)
			SHADER_PARAMETER(float, PlaneRejectThreshold)
			SHADER_PARAMETER(float, FoliageSelfRadius)
			SHADER_PARAMETER(float, FoliageOccluderScale)
			SHADER_PARAMETER(uint32, FoliageSlope)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputAO)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FFilterCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FFilterCS);
		SHADER_USE_PARAMETER_STRUCT(FFilterCS, FGlobalShader);

		class FLargeFilter : SHADER_PERMUTATION_BOOL("CORTAO_LARGE_FILTER");
		using FPermutationDomain = TShaderPermutationDomain<FLargeFilter>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PackedData)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputAO)
			SHADER_PARAMETER_SAMPLER(SamplerState, PointBorderSampler)
			SHADER_PARAMETER(FUintVector2, PackedSize)
			SHADER_PARAMETER(uint32, PackedResolution)
			SHADER_PARAMETER(float, DepthSensitivity)
			SHADER_PARAMETER(float, NormalSensitivity)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputAO)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FTemporalCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FTemporalCS);
		SHADER_USE_PARAMETER_STRUCT(FTemporalCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
			SHADER_PARAMETER(FUintVector2, ViewRectMin)
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PackedData)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputAO)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, HistoryAO)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, HistoryPacked)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, LinearClampSampler)
			SHADER_PARAMETER(FUintVector2, ViewSize)
			SHADER_PARAMETER(float, HistoryWeight)
			SHADER_PARAMETER(float, DepthTolerance)
			SHADER_PARAMETER(uint32, ClampMode)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputAO)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FCompositeCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FCompositeCS);
		SHADER_USE_PARAMETER_STRUCT(FCompositeCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, PackedData)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, InputAO)
			SHADER_PARAMETER(FUintVector2, ViewRectMin)
			SHADER_PARAMETER(FUintVector2, ViewSize)
			SHADER_PARAMETER(float, Intensity)
			SHADER_PARAMETER(float, Power)
			SHADER_PARAMETER(float, FoliageReceive)
			SHADER_PARAMETER(uint32, FoliageDebug)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ScreenSpaceAOOutput)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FPrepassCS, "/Plugin/CORTAO/Private/CORTAOPrepass.usf", "CORTAOPrepassCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FMainCS, "/Plugin/CORTAO/Private/CORTAOMainPass.usf", "CORTAOMainCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FFilterCS, "/Plugin/CORTAO/Private/CORTAOFilterPass.usf", "CORTAOFilterCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FTemporalCS, "/Plugin/CORTAO/Private/CORTAOTemporal.usf", "CORTAOTemporalCS", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FCompositeCS, "/Plugin/CORTAO/Private/CORTAOComposite.usf", "CORTAOCompositeCS", SF_Compute);

	/** The R16 table widened to uint once, for a StructuredBuffer<uint>. */
	static const TArray<uint32>& OccluderTable32()
	{
		static const TArray<uint32> Table = []
		{
			TArray<uint32> Out;
			Out.SetNumUninitialized(UE_ARRAY_COUNT(GCORTAOOccluderTable16));
			for (int32 Index = 0; Index < Out.Num(); ++Index)
			{
				Out[Index] = GCORTAOOccluderTable16[Index];
			}
			return Out;
		}();
		return Table;
	}

	static int32 SnapSampleCount(int32 Requested)
	{
		static const int32 Counts[] = { 4, 6, 8, 12, 16, 24, 32 };
		int32 Best = Counts[0];
		for (int32 Count : Counts)
		{
			if (FMath::Abs(Count - Requested) < FMath::Abs(Best - Requested))
			{
				Best = Count;
			}
		}
		return Best;
	}

	static uint32 PackResolution(FIntPoint Size)
	{
		return uint32(Size.X) | (uint32(Size.Y) << 16u);
	}

	/**
	 * Last frame's result and packed depth for one view (keyed by its view state), for the temporal pass. Render thread
	 * only. Held by pointer so a later view adding an entry cannot move one whose extraction is still queued.
	 */
	struct FViewHistory
	{
		TRefCountPtr<IPooledRenderTarget> AO;
		TRefCountPtr<IPooledRenderTarget> Packed;
		FIntPoint Size = FIntPoint::ZeroValue;
		uint32 LastFrame = 0;
	};

	static TMap<uint32, TSharedPtr<FViewHistory>>& Histories()
	{
		static TMap<uint32, TSharedPtr<FViewHistory>> Map;
		return Map;
	}

	/** Drops histories of views not drawn for a while (closed editor viewports, finished scene captures). */
	static void PruneHistories(uint32 FrameNumber)
	{
		for (auto It = Histories().CreateIterator(); It; ++It)
		{
			if (FrameNumber - It.Value()->LastFrame > 600u)
			{
				It.RemoveCurrent();
			}
		}
	}

	void RenderForView(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FGlobalIlluminationPluginResources& Resources)
	{
		if (CVarEnable.GetValueOnRenderThread() == 0 || !View.IsPerspectiveProjection())
		{
			return;
		}

		// The engine's own SSAO texture is the destination: it exists only when the engine's SSAO ran
		// for this view (r.AmbientOcclusionLevels > 0, AO intensity > 0, r.AmbientOcclusion.Method 0).
		const FSceneTextures& SceneTextures = View.GetSceneTextures();
		FRDGTextureRef ScreenSpaceAO = SceneTextures.ScreenSpaceAO;
		if (!ScreenSpaceAO || !HasBeenProduced(ScreenSpaceAO) || !EnumHasAnyFlags(ScreenSpaceAO->Desc.Flags, TexCreate_UAV))
		{
			static bool bSaidOnce = false;
			if (!bSaidOnce && !View.bIsSceneCapture && !View.bIsReflectionCapture && !View.bIsPlanarReflection)
			{
				bSaidOnce = true;
				UE_LOG(LogCORTAO, Log, TEXT("CORTAO skipped a view: the engine did not produce a ScreenSpaceAO texture for it. The engine's SSAO must ")
					TEXT("run for CORTAO to have somewhere to write (r.AmbientOcclusionLevels not 0, post-process Ambient Occlusion Intensity ")
					TEXT("above 0 and Radius at least 0.1, a sky light or reflection environment in the scene, no ray traced AO). Said once."));
			}
			return;
		}

		const FIntRect ViewRect = View.ViewRect;
		const FIntPoint ViewSize = ViewRect.Size();
		if (ViewSize.X < MinViewSize || ViewSize.Y < MinViewSize
			|| ViewRect.Max.X > ScreenSpaceAO->Desc.Extent.X || ViewRect.Max.Y > ScreenSpaceAO->Desc.Extent.Y
			|| !Resources.SceneDepthZ || !Resources.GBufferA || !Resources.GBufferB)
		{
			return;
		}

		const int32 SampleCount = SnapSampleCount(CVarSamples.GetValueOnRenderThread());
		RDG_EVENT_SCOPE(GraphBuilder, "CORTAO %dx%d Samples=%d", ViewSize.X, ViewSize.Y, SampleCount);

		// The jittered projection: the depth buffer was drawn with it. Engine projection, row vectors:
		// NDC.x = (x M00 + z M20) / z, NDC.y = (y M11 + z M21) / z, UV = (NDC.x * 0.5 + 0.5, 0.5 - NDC.y * 0.5).
		const FMatrix& Projection = View.ViewMatrices.GetViewToClip();
		const float M00 = float(Projection.M[0][0]);
		const float M11 = float(Projection.M[1][1]);
		const float M20 = float(Projection.M[2][0]);
		const float M21 = float(Projection.M[2][1]);

		// CORTAO view space is (x, y, -z) of the engine's. Its unproject is xy = (uv * P.xy + P.zw) * depth.
		// Its projection is the 3x3 [[P.x,0,0],[0,P.y,0],[P.z,P.w,-1]] applied to a point with z = -d and then
		// divided by d, so uv = (P.x * x / d - P.z, P.y * y / d - P.w): the offsets enter NEGATED (with a +
		// every sample lands off screen and the whole image comes out unoccluded).
		const FVector4f UnprojectParams(2.0f / M00, -2.0f / M11, (-1.0f - M20) / M00, (1.0f - M21) / M11);
		const FVector4f ProjectionParams(0.5f * M00, -0.5f * M11, -0.5f * (1.0f + M20), -0.5f * (1.0f - M21));

		const float ScreenRadius = FMath::Clamp(CVarScreenRadius.GetValueOnRenderThread(), 0.01f, 1.0f);
		const float MaxApparentCircleRadius = (1.0f / M11) * ScreenRadius * 2.0f;

		const uint32 FrameSeed = View.Family ? View.Family->FrameNumber : 0u;
		const uint32 RandomSeed = (FrameSeed + 1u) * 2654435761u;

		// Linear depth = 1 / (DeviceZ * Scale + Offset): the engine's ConvertFromDeviceZ for a perspective view.
		const float DepthScale = View.InvDeviceZToWorldZTransform.Z;
		const float DepthOffset = -View.InvDeviceZToWorldZTransform.W;

		const uint32 PackedResolution = PackResolution(ViewSize);
		const FUintVector2 RectMin(uint32(ViewRect.Min.X), uint32(ViewRect.Min.Y));
		const FUintVector2 SizeU(uint32(ViewSize.X), uint32(ViewSize.Y));

		FRDGTextureRef Packed = GraphBuilder.CreateTexture(
			// R32_FLOAT holding CORTAO's uint bits, so the hardware can point-sample and gather it (integer sampling
			// needs SM 6.7; UE 5.8 compiles SM6 shaders as 6.6) -- the workaround CORTAO's own README describes.
			FRDGTextureDesc::Create2D(ViewSize, PF_R32_FLOAT, FClearValueBinding::None,
				TexCreate_ShaderResource | TexCreate_UAV, MipCount),
			TEXT("CORTAO.PackedData"));

		// Prepass.
		{
			FPrepassCS::FParameters* Params = GraphBuilder.AllocParameters<FPrepassCS::FParameters>();
			Params->ViewUniformBuffer = View.ViewUniformBuffer;
			Params->SceneDepthTexture = Resources.SceneDepthZ;
			Params->GBufferATexture = Resources.GBufferA;
			Params->GBufferBTexture = Resources.GBufferB;
			// The engine copies the stencil's lighting channels to a texture only while something in the view uses a
			// non-default channel -- tagging a grass type is what makes it appear.
			const uint32 FoliageChannels = uint32(FMath::Clamp(CVarFoliageChannels.GetValueOnRenderThread(), 0, 7));
			if (Resources.LightingChannelsTexture && FoliageChannels != 0u)
			{
				Params->LightingChannelsTexture = Resources.LightingChannelsTexture;
				Params->FoliageChannelMask = FoliageChannels;
			}
			else
			{
				FRDGTextureRef Dummy = GraphBuilder.CreateTexture(
					FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R8_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV),
					TEXT("CORTAO.NoLightingChannels"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Dummy), FUintVector4(0u, 0u, 0u, 0u));
				Params->LightingChannelsTexture = Dummy;
				Params->FoliageChannelMask = 0u;
			}
			Params->FoliageMismatch = FMath::Clamp(CVarFoliageMismatch.GetValueOnRenderThread(), 0.0f, 1.0f);
			Params->ViewRectMin = RectMin;
			Params->PackedResolution = PackedResolution;
			Params->DepthScale = DepthScale;
			Params->DepthOffset = DepthOffset;
			Params->RandomSeed = RandomSeed;
			Params->MipCount = MipCount;
			Params->UseDepthNormals = CVarDepthNormals.GetValueOnRenderThread() != 0 ? 1u : 0u;
			Params->PackedMip0 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 0));
			Params->PackedMip1 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 1));
			Params->PackedMip2 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 2));
			Params->PackedMip3 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 3));
			Params->PackedMip4 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 4));
			Params->PackedMip5 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 5));
			Params->PackedMip6 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 6));
			Params->PackedMip7 = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Packed, 7));

			TShaderMapRef<FPrepassCS> Shader(View.ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CORTAO Prepass"), Shader, Params,
				FComputeShaderUtils::GetGroupCount(ViewSize, 8));
		}

		FRDGTextureSRVRef PackedSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Packed));
		// Point, point mips, border 0 (transparent black): what CORTAO's samplers are.
		FRHISamplerState* PointBorderSampler = TStaticSamplerState<SF_Point, AM_Border, AM_Border, AM_Clamp>::GetRHI();
		const FRDGTextureDesc AODesc = FRDGTextureDesc::Create2D(ViewSize, PF_R8, FClearValueBinding::White,
			TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef TracedAO = GraphBuilder.CreateTexture(AODesc, TEXT("CORTAO.Occlusion"));

		// Main pass.
		{
			const TArray<uint32>& Table = OccluderTable32();
			FRDGBufferRef TableBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CORTAO.OccluderTable"),
				sizeof(uint32), Table.Num(), Table.GetData(), Table.Num() * sizeof(uint32), ERDGInitialDataFlags::NoCopy);

			FMainCS::FParameters* Params = GraphBuilder.AllocParameters<FMainCS::FParameters>();
			Params->ViewUniformBuffer = View.ViewUniformBuffer;
			Params->GBufferATexture = Resources.GBufferA;
			Params->SceneDepthTexture = Resources.SceneDepthZ;
			Params->ViewRectMin = RectMin;
			Params->UseDepthNormals = CVarDepthNormals.GetValueOnRenderThread() != 0 ? 1u : 0u;
			Params->PackedData = PackedSRV;
			Params->PointBorderSampler = PointBorderSampler;
			Params->PackedSize = SizeU;
			Params->MipCount = MipCount;
			Params->OccluderTable = GraphBuilder.CreateSRV(TableBuffer);
			Params->PackedResolution = PackedResolution;
			Params->RandomSeed = RandomSeed;
			Params->NormalBias = (1.0f / M00) / float(ViewSize.X) * UE_SQRT_2;
			Params->Radius = FMath::Max(1.0f, CVarRadius.GetValueOnRenderThread());
			Params->UnprojectParams = UnprojectParams;
			Params->ProjectionParams = ProjectionParams;
			Params->MaxApparentCircleRadiusCoefficient = MaxApparentCircleRadius / FMath::Sqrt(MaxApparentCircleRadius * MaxApparentCircleRadius + 1.0f);
			Params->MipBias = FMath::Log2(M11 * float(ViewSize.Y) * 0.5f) + CVarMipBias.GetValueOnRenderThread();
			Params->InverseMaxSlopeCompensation = 1.0f / FMath::Max(1.0f, CVarSlopeCompensation.GetValueOnRenderThread());
			Params->BackfaceThreshold = CVarBackfaceThreshold.GetValueOnRenderThread();
			Params->BackfaceBoost = FMath::Clamp(CVarBackfaceBoost.GetValueOnRenderThread(), 0.0f, 1.0f);
			Params->PlaneRejectMode = uint32(FMath::Clamp(CVarPlaneReject.GetValueOnRenderThread(), 0, 2));
			Params->PlaneRejectThreshold = FMath::Clamp(CVarPlaneRejectThreshold.GetValueOnRenderThread(), -1.0f, 0.9f);
			Params->FoliageSelfRadius = FMath::Max(0.0f, CVarFoliageSelfRadius.GetValueOnRenderThread());
			Params->FoliageOccluderScale = FMath::Sqrt(FMath::Clamp(CVarFoliageOpacity.GetValueOnRenderThread(), 0.0f, 1.0f));
			Params->FoliageSlope = CVarFoliageSlope.GetValueOnRenderThread() != 0 ? 1u : 0u;
			Params->OutputAO = GraphBuilder.CreateUAV(TracedAO);

			FMainCS::FPermutationDomain Permutation;
			Permutation.Set<FMainCS::FSampleCount>(SampleCount);
			TShaderMapRef<FMainCS> Shader(View.ShaderMap, Permutation);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CORTAO Main"), Shader, Params,
				FComputeShaderUtils::GetGroupCount(ViewSize, 16));
		}

		// Filter pass: one thread per 2 x 2 block.
		FRDGTextureRef ResultAO = TracedAO;
		const int32 Filter = CVarFilter.GetValueOnRenderThread();
		if (Filter > 0)
		{
			FRDGTextureRef FilteredAO = GraphBuilder.CreateTexture(AODesc, TEXT("CORTAO.Filtered"));

			FFilterCS::FParameters* Params = GraphBuilder.AllocParameters<FFilterCS::FParameters>();
			Params->PackedData = PackedSRV;
			Params->InputAO = TracedAO;
			Params->PointBorderSampler = PointBorderSampler;
			Params->PackedSize = SizeU;
			Params->PackedResolution = PackedResolution;
			Params->DepthSensitivity = CVarFilterDepthSensitivity.GetValueOnRenderThread();
			Params->NormalSensitivity = CVarFilterNormalSensitivity.GetValueOnRenderThread();
			Params->OutputAO = GraphBuilder.CreateUAV(FilteredAO);

			FFilterCS::FPermutationDomain Permutation;
			Permutation.Set<FFilterCS::FLargeFilter>(Filter >= 2);
			TShaderMapRef<FFilterCS> Shader(View.ShaderMap, Permutation);
			const FIntPoint Threads((ViewSize.X + 2) / 2, (ViewSize.Y + 2) / 2);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CORTAO Filter"), Shader, Params,
				FComputeShaderUtils::GetGroupCount(Threads, 8));
			ResultAO = FilteredAO;
		}

		// Temporal accumulation: blend with last frame's result where it still belongs to the same surface.
		const bool bTemporal = CVarTemporal.GetValueOnRenderThread() != 0 && View.ViewState != nullptr;
		if (bTemporal)
		{
			const uint32 ViewKey = View.ViewState->GetViewKey();
			TSharedPtr<FViewHistory>& Slot = Histories().FindOrAdd(ViewKey);
			if (!Slot.IsValid())
			{
				Slot = MakeShared<FViewHistory>();
			}
			TSharedPtr<FViewHistory> HistoryRef = Slot;
			FViewHistory& History = *HistoryRef;

			const bool bHistoryValid = History.AO.IsValid() && History.Packed.IsValid() && History.Size == ViewSize
				&& History.LastFrame + 1u == FrameSeed && !View.bCameraCut;

			FRDGTextureRef TemporalAO = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(ViewSize, PF_R16F, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_UAV),
				TEXT("CORTAO.Temporal"));

			FRDGTextureRef VelocityTexture = SceneTextures.Velocity;
			if (!VelocityTexture || !HasBeenProduced(VelocityTexture))
			{
				VelocityTexture = FRDGSystemTextures::Get(GraphBuilder).Black;
			}

			FTemporalCS::FParameters* Params = GraphBuilder.AllocParameters<FTemporalCS::FParameters>();
			Params->ViewUniformBuffer = View.ViewUniformBuffer;
			Params->SceneDepthTexture = Resources.SceneDepthZ;
			Params->ViewRectMin = RectMin;
			Params->PackedData = PackedSRV;
			Params->InputAO = ResultAO;
			Params->HistoryAO = bHistoryValid ? GraphBuilder.RegisterExternalTexture(History.AO) : ResultAO;
			Params->HistoryPacked = bHistoryValid ? GraphBuilder.RegisterExternalTexture(History.Packed) : Packed;
			Params->VelocityTexture = VelocityTexture;
			Params->LinearClampSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			Params->ViewSize = SizeU;
			Params->HistoryWeight = bHistoryValid ? FMath::Clamp(CVarTemporalWeight.GetValueOnRenderThread(), 0.0f, 0.98f) : 0.0f;
			Params->DepthTolerance = FMath::Max(0.001f, CVarTemporalDepthTolerance.GetValueOnRenderThread());
			Params->ClampMode = uint32(FMath::Clamp(CVarTemporalClamp.GetValueOnRenderThread(), 0, 2));
			Params->OutputAO = GraphBuilder.CreateUAV(TemporalAO);

			TShaderMapRef<FTemporalCS> Shader(View.ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CORTAO Temporal%s", bHistoryValid ? TEXT("") : TEXT(" (reset)")),
				Shader, Params, FComputeShaderUtils::GetGroupCount(ViewSize, 8));

			GraphBuilder.QueueTextureExtraction(TemporalAO, &History.AO);
			GraphBuilder.QueueTextureExtraction(Packed, &History.Packed);
			History.Size = ViewSize;
			History.LastFrame = FrameSeed;
			ResultAO = TemporalAO;

			PruneHistories(FrameSeed);
		}
		else if (View.ViewState)
		{
			Histories().Remove(View.ViewState->GetViewKey());
		}

		// Into the engine's texture, inside this view's rectangle.
		{
			FCompositeCS::FParameters* Params = GraphBuilder.AllocParameters<FCompositeCS::FParameters>();
			Params->PackedData = PackedSRV;
			Params->InputAO = ResultAO;
			Params->ViewRectMin = RectMin;
			Params->ViewSize = SizeU;
			Params->Intensity = FMath::Clamp(CVarIntensity.GetValueOnRenderThread(), 0.0f, 1.0f);
			Params->Power = FMath::Clamp(CVarPower.GetValueOnRenderThread(), 0.1f, 8.0f);
			Params->FoliageReceive = FMath::Clamp(CVarFoliageReceive.GetValueOnRenderThread(), 0.0f, 1.0f);
			Params->FoliageDebug = CVarFoliageDebug.GetValueOnRenderThread() != 0 ? 1u : 0u;
			Params->ScreenSpaceAOOutput = GraphBuilder.CreateUAV(ScreenSpaceAO);

			TShaderMapRef<FCompositeCS> Shader(View.ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CORTAO Composite"), Shader, Params,
				FComputeShaderUtils::GetGroupCount(ViewSize, 8));
		}
	}
}
