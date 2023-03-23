// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once


class FDxcArguments
{
protected:
	FString ShaderProfile;
	FString EntryPoint;
	FString Exports;
	FString DumpDisasmFilename;
	FString BatchBaseFilename;
	FString DumpDebugInfoPath;
	bool bEnable16BitTypes = false;
	bool bKeepEmbeddedPDB = false;
	bool bDump = false;

	TArray<FString> ExtraArguments;

public:
	FDxcArguments(
		const FString& InEntryPoint,
		const TCHAR* InShaderProfile,
		const FString& InExports,
		const FString& InDumpDebugInfoPath,	// Optional, empty when not dumping shader debug info
		const FString& InBaseFilename,
		bool bInEnable16BitTypes,
		bool bGenerateSymbols,
		bool bSymbolsBasedOnSource,
		uint32 D3DCompileFlags,
		uint32 AutoBindingSpace,
		const TCHAR* InOptValidatorVersion
	)
	: ShaderProfile(InShaderProfile)
	, EntryPoint(InEntryPoint)
	, Exports(InExports)
	, DumpDebugInfoPath(InDumpDebugInfoPath)
	, bEnable16BitTypes(bInEnable16BitTypes)
	{
		BatchBaseFilename = FPaths::GetBaseFilename(InBaseFilename);

		if (InDumpDebugInfoPath.Len() > 0)
		{
			bDump = true;
			DumpDisasmFilename = InDumpDebugInfoPath / TEXT("Output.d3dasm");
		}

		if (AutoBindingSpace != ~0u)
		{
			ExtraArguments.Add(L"/auto-binding-space");
			ExtraArguments.Add(FString::Printf(TEXT("%d"), AutoBindingSpace));
		}

		if (Exports.Len() > 0)
		{
			// Ensure that only the requested functions exists in the output DXIL.
			// All other functions and their used resources must be eliminated.
			ExtraArguments.Add(L"/exports");
			ExtraArguments.Add(Exports);
		}

		if (D3DCompileFlags & D3DCOMPILE_PREFER_FLOW_CONTROL)
		{
			D3DCompileFlags &= ~D3DCOMPILE_PREFER_FLOW_CONTROL;
			ExtraArguments.Add(L"/Gfp");
		}

		if (D3DCompileFlags & D3DCOMPILE_SKIP_OPTIMIZATION)
		{
			D3DCompileFlags &= ~D3DCOMPILE_SKIP_OPTIMIZATION;
			ExtraArguments.Add(L"/Od");
		}

		if (D3DCompileFlags & D3DCOMPILE_SKIP_VALIDATION)
		{
			D3DCompileFlags &= ~D3DCOMPILE_SKIP_VALIDATION;
			ExtraArguments.Add(L"/Vd");
		}

		if (D3DCompileFlags & D3DCOMPILE_AVOID_FLOW_CONTROL)
		{
			D3DCompileFlags &= ~D3DCOMPILE_AVOID_FLOW_CONTROL;
			ExtraArguments.Add(L"/Gfa");
		}

		if (D3DCompileFlags & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
		{
			D3DCompileFlags &= ~D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
			ExtraArguments.Add(L"/Zpr");
		}

		if (D3DCompileFlags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
		{
			D3DCompileFlags &= ~D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
			ExtraArguments.Add(L"/Gec");
		}

		if (D3DCompileFlags & D3DCOMPILE_WARNINGS_ARE_ERRORS)
		{
			D3DCompileFlags &= ~D3DCOMPILE_WARNINGS_ARE_ERRORS;
			ExtraArguments.Add(L"/WX");
		}

		switch (D3DCompileFlags & SHADER_OPTIMIZATION_LEVEL_MASK)
		{
		case D3DCOMPILE_OPTIMIZATION_LEVEL0:
			D3DCompileFlags &= ~D3DCOMPILE_OPTIMIZATION_LEVEL0;
			ExtraArguments.Add(L"/O0");
			break;

		case D3DCOMPILE_OPTIMIZATION_LEVEL1:
			D3DCompileFlags &= ~D3DCOMPILE_OPTIMIZATION_LEVEL1;
			ExtraArguments.Add(L"/O1");
			break;

		case D3DCOMPILE_OPTIMIZATION_LEVEL2:
			D3DCompileFlags &= ~D3DCOMPILE_OPTIMIZATION_LEVEL2;
			ExtraArguments.Add(L"/O2");
			break;

		case D3DCOMPILE_OPTIMIZATION_LEVEL3:
			D3DCompileFlags &= ~D3DCOMPILE_OPTIMIZATION_LEVEL3;
			ExtraArguments.Add(L"/O3");
			break;

		default:
			break;
		}

		if (D3DCompileFlags & D3DCOMPILE_DEBUG)
		{
			D3DCompileFlags &= ~D3DCOMPILE_DEBUG;
			bGenerateSymbols = true;
		}

		if (bEnable16BitTypes)
		{
			ExtraArguments.Add(L"/enable-16bit-types");
		}

		checkf(D3DCompileFlags == 0, TEXT("Unhandled shader compiler flags 0x%x!"), D3DCompileFlags);

		if (InOptValidatorVersion)
		{
			ExtraArguments.Add(L"/validator-version");
			ExtraArguments.Add(FString(InOptValidatorVersion));
		}

		if (bGenerateSymbols)
		{
			// -Zsb Compute Shader Hash considering only output binary
			// -Zss Compute Shader Hash considering source information
			ExtraArguments.Add(bSymbolsBasedOnSource ? L"/Zss" : L"/Zsb");

			ExtraArguments.Add(L"/Qembed_debug");
			ExtraArguments.Add(L"/Zi");

			ExtraArguments.Add(L"/Fd");
			ExtraArguments.Add(L".\\");

			bKeepEmbeddedPDB = true;
		}

		// Reflection will be removed later, otherwise the disassembly won't contain variables
		//ExtraArguments.Add(L"/Qstrip_reflect");
	}

	inline FString GetDumpDebugInfoPath() const
	{
		return DumpDebugInfoPath;
	}

	inline bool ShouldKeepEmbeddedPDB() const
	{
		return bKeepEmbeddedPDB;
	}

	inline bool ShouldDump() const
	{
		return bDump;
	}

	FString GetEntryPointName() const
	{
		return Exports.Len() > 0 ? FString(L"") : EntryPoint;
	}

	const FString& GetShaderProfile() const
	{
		return ShaderProfile;
	}

	const FString& GetDumpDisassemblyFilename() const
	{
		return DumpDisasmFilename;
	}

	void GetCompilerArgsNoEntryNoProfileNoDisasm(TArray<const WCHAR*>& Out) const
	{
		for (const FString& Entry : ExtraArguments)
		{
			Out.Add(*Entry);
		}
	}

	void GetCompilerArgs(TArray<const WCHAR*>& Out) const
	{
		GetCompilerArgsNoEntryNoProfileNoDisasm(Out);
		if (Exports.Len() == 0)
		{
			Out.Add(L"/E");
			Out.Add(*EntryPoint);
		}

		Out.Add(L"/T");
		Out.Add(*ShaderProfile);

		Out.Add(L" /Fc ");
		Out.Add(TEXT("zzz.d3dasm"));	// Dummy

		Out.Add(L" /Fo ");
		Out.Add(TEXT("zzz.dxil"));	// Dummy
	}

	FString GetBatchCommandLineString(const FString& ShaderPath) const
	{
		FString DXCCommandline;
		for (const FString& Entry : ExtraArguments)
		{
			DXCCommandline += L" ";
			DXCCommandline += Entry;
		}

		DXCCommandline += L" /T ";
		DXCCommandline += ShaderProfile;

		if (Exports.Len() == 0)
		{
			DXCCommandline += L" /E ";
			DXCCommandline += EntryPoint;
		}

		DXCCommandline += L" /Fc ";
		DXCCommandline += BatchBaseFilename + TEXT(".d3dasm");

		DXCCommandline += L" /Fo ";
		DXCCommandline += BatchBaseFilename + TEXT(".dxil");

		return DXCCommandline;
	}
};


template <typename ID3D1xShaderReflection, typename D3D1x_SHADER_DESC, typename D3D1x_SHADER_INPUT_BIND_DESC,
	typename ID3D1xShaderReflectionConstantBuffer, typename D3D1x_SHADER_BUFFER_DESC,
	typename ID3D1xShaderReflectionVariable, typename D3D1x_SHADER_VARIABLE_DESC>
	inline void ExtractParameterMapFromD3DShader(
		const FShaderCompilerInput& Input,
		const FShaderParameterParser& ShaderParameterParser,
		uint32 BindingSpace,
		ID3D1xShaderReflection* Reflector, const D3D1x_SHADER_DESC& ShaderDesc,
		bool& bGlobalUniformBufferUsed, bool& bDiagnosticBufferUsed, uint32& NumSamplers, uint32& NumSRVs, uint32& NumCBs, uint32& NumUAVs,
		FShaderCompilerOutput& Output, TArray<FString>& UniformBufferNames, TBitArray<>& UsedUniformBufferSlots, TArray<FShaderCodeVendorExtension>& VendorExtensions)
{
	// Add parameters for shader resources (constant buffers, textures, samplers, etc. */
	for (uint32 ResourceIndex = 0; ResourceIndex < ShaderDesc.BoundResources; ResourceIndex++)
	{
		D3D1x_SHADER_INPUT_BIND_DESC BindDesc;
		Reflector->GetResourceBindingDesc(ResourceIndex, &BindDesc);

		if (!IsCompatibleBinding(BindDesc, BindingSpace))
		{
			continue;
		}

		if (BindDesc.Type == D3D_SIT_CBUFFER || BindDesc.Type == D3D_SIT_TBUFFER)
		{
			const uint32 CBIndex = BindDesc.BindPoint;
			ID3D1xShaderReflectionConstantBuffer* ConstantBuffer = Reflector->GetConstantBufferByName(BindDesc.Name);
			D3D1x_SHADER_BUFFER_DESC CBDesc;
			ConstantBuffer->GetDesc(&CBDesc);
			bool bGlobalCB = (FCStringAnsi::Strcmp(CBDesc.Name, "$Globals") == 0);
			const bool bIsRootCB = FCString::Strcmp(ANSI_TO_TCHAR(CBDesc.Name), FShaderParametersMetadata::kRootUniformBufferBindingName) == 0;

			if (bGlobalCB)
			{
				if (ShouldUseStableConstantBuffer(Input))
				{
					// Each member found in the global constant buffer means it was not in RootParametersStructure or
					// it would have been moved by ShaderParameterParser.ParseAndMoveShaderParametersToRootConstantBuffer().
					for (uint32 ConstantIndex = 0; ConstantIndex < CBDesc.Variables; ConstantIndex++)
					{
						ID3D1xShaderReflectionVariable* Variable = ConstantBuffer->GetVariableByIndex(ConstantIndex);
						D3D1x_SHADER_VARIABLE_DESC VariableDesc;
						Variable->GetDesc(&VariableDesc);
						if (VariableDesc.uFlags & D3D_SVF_USED)
						{
							AddUnboundShaderParameterError(
								Input,
								ShaderParameterParser,
								ANSI_TO_TCHAR(VariableDesc.Name),
								Output);
						}
					}
				}
				else
				{
					// Track all of the variables in this constant buffer.
					for (uint32 ConstantIndex = 0; ConstantIndex < CBDesc.Variables; ConstantIndex++)
					{
						ID3D1xShaderReflectionVariable* Variable = ConstantBuffer->GetVariableByIndex(ConstantIndex);
						D3D1x_SHADER_VARIABLE_DESC VariableDesc;
						Variable->GetDesc(&VariableDesc);
						if (VariableDesc.uFlags & D3D_SVF_USED)
						{
							bGlobalUniformBufferUsed = true;

							Output.ParameterMap.AddParameterAllocation(
								ANSI_TO_TCHAR(VariableDesc.Name),
								CBIndex,
								VariableDesc.StartOffset,
								VariableDesc.Size,
								EShaderParameterType::LooseData
							);
							UsedUniformBufferSlots[CBIndex] = true;
						}
					}
				}
			}
			else if (bIsRootCB && ShouldUseStableConstantBuffer(Input))
			{
				if (CBIndex == FShaderParametersMetadata::kRootCBufferBindingIndex)
				{
					int32 ConstantBufferSize = 0;

					// Track all of the variables in this constant buffer.
					for (uint32 ConstantIndex = 0; ConstantIndex < CBDesc.Variables; ConstantIndex++)
					{
						ID3D1xShaderReflectionVariable* Variable = ConstantBuffer->GetVariableByIndex(ConstantIndex);
						D3D1x_SHADER_VARIABLE_DESC VariableDesc;
						Variable->GetDesc(&VariableDesc);
						if (VariableDesc.uFlags & D3D_SVF_USED)
						{
							FString MemberName(ANSI_TO_TCHAR(VariableDesc.Name));
							int32 ReflectionSize = VariableDesc.Size;
							int32 ReflectionOffset = VariableDesc.StartOffset;

							ShaderParameterParser.ValidateShaderParameterType(Input, MemberName, ReflectionOffset, ReflectionSize, Output);

							ConstantBufferSize = FMath::Max(ConstantBufferSize, ReflectionOffset + ReflectionSize);
						}
					}

					if (ConstantBufferSize > 0)
					{
						Output.ParameterMap.AddParameterAllocation(
							FShaderParametersMetadata::kRootUniformBufferBindingName,
							FShaderParametersMetadata::kRootCBufferBindingIndex,
							/* Offset = */ uint16(0),
							/* Size = */ uint16(ConstantBufferSize),
							EShaderParameterType::LooseData);

						bGlobalUniformBufferUsed = true;
						UsedUniformBufferSlots[CBIndex] = true;
					}
				}
				else
				{
					FString ErrorMessage = FString::Printf(
						TEXT("Error: %s is expected to always be in the API slot %d, but is actually in slot %d."),
						FShaderParametersMetadata::kRootUniformBufferBindingName,
						FShaderParametersMetadata::kRootCBufferBindingIndex,
						CBIndex);
					Output.Errors.Add(FShaderCompilerError(*ErrorMessage));
					Output.bSucceeded = false;
				}
			}
			else
			{
				// Track just the constant buffer itself.
				Output.ParameterMap.AddParameterAllocation(
					ANSI_TO_TCHAR(CBDesc.Name),
					CBIndex,
					0,
					0,
					EShaderParameterType::UniformBuffer
				);
				UsedUniformBufferSlots[CBIndex] = true;

				if (UniformBufferNames.Num() <= (int32)CBIndex)
				{
					UniformBufferNames.AddDefaulted(CBIndex - UniformBufferNames.Num() + 1);
				}
				UniformBufferNames[CBIndex] = CBDesc.Name;
			}

			NumCBs = FMath::Max(NumCBs, BindDesc.BindPoint + BindDesc.BindCount);
		}
		else if (BindDesc.Type == D3D_SIT_TEXTURE || BindDesc.Type == D3D_SIT_SAMPLER)
		{
			check(BindDesc.BindCount == 1);

			// https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK/blob/master/ags_lib/hlsl/ags_shader_intrinsics_dx11.hlsl
			const bool bIsAMDTexExtension = (FCStringAnsi::Strcmp(BindDesc.Name, "AmdDxExtShaderIntrinsicsResource") == 0);
			const bool bIsAMDSmpExtension = (FCStringAnsi::Strcmp(BindDesc.Name, "AmdDxExtShaderIntrinsicsSamplerState") == 0);
			const bool bIsVendorParameter = bIsAMDTexExtension || bIsAMDSmpExtension;

			TCHAR OfficialName[1024];
			FCString::Strcpy(OfficialName, ANSI_TO_TCHAR(BindDesc.Name));

			const uint32 BindCount = 1;
			EShaderParameterType ParameterType = EShaderParameterType::Num;
			if (BindDesc.Type == D3D_SIT_SAMPLER)
			{
				ParameterType = EShaderParameterType::Sampler;
				NumSamplers = FMath::Max(NumSamplers, BindDesc.BindPoint + BindCount);
			}
			else // D3D_SIT_TEXTURE
			{
				ParameterType = EShaderParameterType::SRV;
				NumSRVs = FMath::Max(NumSRVs, BindDesc.BindPoint + BindCount);
			}

			if (bIsVendorParameter)
			{
				VendorExtensions.Emplace(0x1002 /*AMD*/, 0, BindDesc.BindPoint, BindCount, ParameterType);
			}
			else
			{
				// Add a parameter for the texture only, the sampler index will be invalid
				Output.ParameterMap.AddParameterAllocation(
					OfficialName,
					0,
					BindDesc.BindPoint,
					BindCount,
					ParameterType
				);
			}
		}
		else if (BindDesc.Type == D3D_SIT_UAV_RWTYPED || BindDesc.Type == D3D_SIT_UAV_RWSTRUCTURED ||
			BindDesc.Type == D3D_SIT_UAV_RWBYTEADDRESS || BindDesc.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER ||
			BindDesc.Type == D3D_SIT_UAV_APPEND_STRUCTURED)
		{
			check(BindDesc.BindCount == 1);

			// https://developer.nvidia.com/unlocking-gpu-intrinsics-hlsl
			const bool bIsNVExtension = (FCStringAnsi::Strcmp(BindDesc.Name, "g_NvidiaExt") == 0);

			// https://github.com/intel/intel-graphics-compiler/blob/master/inc/IntelExtensions.hlsl
			const bool bIsIntelExtension = (FCStringAnsi::Strcmp(BindDesc.Name, "g_IntelExt") == 0);

			// https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK/blob/master/ags_lib/hlsl/ags_shader_intrinsics_dx11.hlsl
			const bool bIsAMDExtensionDX11 = (FCStringAnsi::Strcmp(BindDesc.Name, "AmdDxExtShaderIntrinsicsUAV") == 0);

			// https://github.com/GPUOpen-LibrariesAndSDKs/AGS_SDK/blob/master/ags_lib/hlsl/ags_shader_intrinsics_dx12.hlsl
			const bool bIsAMDExtensionDX12 = (FCStringAnsi::Strcmp(BindDesc.Name, "AmdExtD3DShaderIntrinsicsUAV") == 0);

			const bool bIsVendorParameter = bIsNVExtension || bIsIntelExtension || bIsAMDExtensionDX11 || bIsAMDExtensionDX12;

			// See D3DCommon.ush
			const bool bIsDiagnosticBufferParameter = (FCStringAnsi::Strcmp(BindDesc.Name, "UEDiagnosticBuffer") == 0);

			TCHAR OfficialName[1024];
			FCString::Strcpy(OfficialName, ANSI_TO_TCHAR(BindDesc.Name));

			const uint32 BindCount = 1;
			if (bIsVendorParameter)
			{
				const uint32 VendorId =
					bIsNVExtension ? 0x10DE : // NVIDIA
					(bIsAMDExtensionDX11 || bIsAMDExtensionDX12) ? 0x1002 : // AMD
					bIsIntelExtension ? 0x8086 : // Intel
					0;
				VendorExtensions.Emplace(VendorId, 0, BindDesc.BindPoint, BindCount, EShaderParameterType::UAV);
			}
			else if (bIsDiagnosticBufferParameter)
			{
				bDiagnosticBufferUsed = true;
			}
			else
			{
				Output.ParameterMap.AddParameterAllocation(
					OfficialName,
					0,
					BindDesc.BindPoint,
					BindCount,
					EShaderParameterType::UAV
				);
			}

			NumUAVs = FMath::Max(NumUAVs, BindDesc.BindPoint + BindCount);
		}
		else if (BindDesc.Type == D3D_SIT_STRUCTURED || BindDesc.Type == D3D_SIT_BYTEADDRESS)
		{
			check(BindDesc.BindCount == 1);
			TCHAR OfficialName[1024];
			FCString::Strcpy(OfficialName, ANSI_TO_TCHAR(BindDesc.Name));

			const uint32 BindCount = 1;
			Output.ParameterMap.AddParameterAllocation(
				OfficialName,
				0,
				BindDesc.BindPoint,
				BindCount,
				EShaderParameterType::SRV
			);

			NumSRVs = FMath::Max(NumSRVs, BindDesc.BindPoint + BindCount);
		}
		else if (BindDesc.Type == (D3D_SHADER_INPUT_TYPE)(D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER + 1)) // D3D_SIT_RTACCELERATIONSTRUCTURE (12)
		{
			// Acceleration structure resources are treated as SRVs.
			check(BindDesc.BindCount == 1);

			TCHAR OfficialName[1024];
			FCString::Strcpy(OfficialName, ANSI_TO_TCHAR(BindDesc.Name));

			const uint32 BindCount = 1;
			Output.ParameterMap.AddParameterAllocation(
				OfficialName,
				0,
				BindDesc.BindPoint,
				BindCount,
				EShaderParameterType::SRV
			);

			NumSRVs = FMath::Max(NumSRVs, BindDesc.BindPoint + BindCount);
		}
	}
}

template <typename TBlob>
inline void GenerateFinalOutput(TRefCountPtr<TBlob>& CompressedData,
	const FShaderCompilerInput& Input, TArray<FShaderCodeVendorExtension>& VendorExtensions, 
	TBitArray<>& UsedUniformBufferSlots, TArray<FString>& UniformBufferNames,
	bool bProcessingSecondTime, const TArray<FString>& ShaderInputs,
	FShaderCodePackedResourceCounts& PackedResourceCounts, uint32 NumInstructions,
	FShaderCompilerOutput& Output,
	TFunction<void(FMemoryWriter&)> PostSRTWriterCallback,
	TFunction<void(FShaderCode&)> AddOptionalDataCallback)
{
	// Build the SRT for this shader.
	FD3D11ShaderResourceTable SRT;

	TArray<uint8> UniformBufferNameBytes;

	{
		// Build the generic SRT for this shader.
		FShaderCompilerResourceTable GenericSRT;
		BuildResourceTableMapping(Input.Environment.ResourceTableMap, Input.Environment.UniformBufferMap, UsedUniformBufferSlots, Output.ParameterMap, GenericSRT);

		// Ray generation shaders rely on a different binding model that aren't compatible with global uniform buffers.
		if (Input.Target.Frequency != SF_RayGen)
		{
			CullGlobalUniformBuffers(Input.Environment.UniformBufferMap, Output.ParameterMap);
		}

		if (UniformBufferNames.Num() < GenericSRT.ResourceTableLayoutHashes.Num())
		{
			UniformBufferNames.AddDefaulted(GenericSRT.ResourceTableLayoutHashes.Num() - UniformBufferNames.Num() + 1);
		}

		for (int32 Index = 0; Index < GenericSRT.ResourceTableLayoutHashes.Num(); ++Index)
		{
			if (GenericSRT.ResourceTableLayoutHashes[Index] != 0 && UniformBufferNames[Index].Len() == 0)
			{
				for (const auto& KeyValue : Input.Environment.UniformBufferMap)
				{
					const FUniformBufferEntry& UniformBufferEntry = KeyValue.Value;

					if (UniformBufferEntry.LayoutHash == GenericSRT.ResourceTableLayoutHashes[Index])
					{
						UniformBufferNames[Index] = KeyValue.Key;
						break;
					}
				}
			}
		}

		FMemoryWriter UniformBufferNameWriter(UniformBufferNameBytes);
		UniformBufferNameWriter << UniformBufferNames;

		// Copy over the bits indicating which resource tables are active.
		SRT.ResourceTableBits = GenericSRT.ResourceTableBits;

		SRT.ResourceTableLayoutHashes = GenericSRT.ResourceTableLayoutHashes;

		// Now build our token streams.
		BuildResourceTableTokenStream(GenericSRT.TextureMap, GenericSRT.MaxBoundResourceTable, SRT.TextureMap);
		BuildResourceTableTokenStream(GenericSRT.ShaderResourceViewMap, GenericSRT.MaxBoundResourceTable, SRT.ShaderResourceViewMap);
		BuildResourceTableTokenStream(GenericSRT.SamplerMap, GenericSRT.MaxBoundResourceTable, SRT.SamplerMap);
		BuildResourceTableTokenStream(GenericSRT.UnorderedAccessViewMap, GenericSRT.MaxBoundResourceTable, SRT.UnorderedAccessViewMap);
	}

	if (GD3DAllowRemoveUnused != 0 && Input.Target.Frequency == SF_Pixel && Input.bCompilingForShaderPipeline && bProcessingSecondTime)
	{
		Output.bSupportsQueryingUsedAttributes = true;
		if (GD3DAllowRemoveUnused == 1)
		{
			Output.UsedAttributes = ShaderInputs;
		}
	}

	// Generate the final Output
	FMemoryWriter Ar(Output.ShaderCode.GetWriteAccess(), true);
	Ar << SRT;

	PostSRTWriterCallback(Ar);

	Ar.Serialize(CompressedData->GetBufferPointer(), CompressedData->GetBufferSize());

	// Append data that is generate from the shader code and assist the usage, mostly needed for DX12 
	{
		Output.ShaderCode.AddOptionalData(PackedResourceCounts);
		Output.ShaderCode.AddOptionalData(FShaderCodeUniformBuffers::Key, UniformBufferNameBytes.GetData(), UniformBufferNameBytes.Num());
		AddOptionalDataCallback(Output.ShaderCode);
	}

	// Append information about optional hardware vendor extensions
	if (VendorExtensions.Num() > 0)
	{
		TArray<uint8> WriterBytes;
		FMemoryWriter Writer(WriterBytes);
		Writer << VendorExtensions;
		if (WriterBytes.Num() > 0)
		{
			Output.ShaderCode.AddOptionalData(FShaderCodeVendorExtension::Key, WriterBytes.GetData(), WriterBytes.Num());
		}
	}

	if (Input.Environment.CompilerFlags.Contains(CFLAG_ExtraShaderData))
	{
		Output.ShaderCode.AddOptionalData(FShaderCodeName::Key, TCHAR_TO_UTF8(*Input.GenerateShaderName()));
	}

	// Set the number of instructions.
	Output.NumInstructions = NumInstructions;

	Output.NumTextureSamplers = PackedResourceCounts.NumSamplers;

	// Pass the target through to the output.
	Output.Target = Input.Target;
}
