// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MultiGPU.h"

class FRHITexture2D;
class FRHICommandListImmediate;

using PathTracingDenoiserFunction = void(FRHICommandListImmediate& RHICmdList, FRHITexture2D* ColorTex, FRHITexture2D* AlbedoTex, FRHITexture2D* NormalTex, FRHITexture2D* OutputTex, FRHIGPUMask GPUMask);

extern RENDERER_API PathTracingDenoiserFunction* GPathTracingDenoiserFunc;
