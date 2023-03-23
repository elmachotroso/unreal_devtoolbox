// Copyright Epic Games, Inc. All Rights Reserved.

#include "PackageSubmissionChecks.h"

#include "Containers/UnrealString.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Internationalization.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/EditorBulkData.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/PackageResourceManager.h"
#include "UObject/PackageTrailer.h"
#include "UObject/UObjectGlobals.h"
#include "Virtualization/VirtualizationSystem.h"

#define LOCTEXT_NAMESPACE "Virtualization"

// When enabled we will validate truncated packages right after the truncation process to 
// make sure that the package format is still correct once the package trailer has been 
// removed.
#define UE_VALIDATE_TRUNCATED_PACKAGE 1

// When enabled we will check the payloads to see if they already exist in the persistent storage
// backends before trying to push them.
#define UE_PRECHECK_PAYLOAD_STATUS 1

namespace UE::Virtualization
{

/**
 * Check that the given package ends with PACKAGE_FILE_TAG. Intended to be used to make sure that
 * we have truncated a package correctly when removing the trailers.
 * 
 * @param PackagePath	The path of the package that should be checked
 * @param Errors [out] 	Errors created by the function will be added here
 * 
 * @return	True if the package is correctly terminated with a PACKAGE_FILE_TAG, false if the tag
 *			was not found or if we were unable to read the file's contents.
 */
bool ValidatePackage(const FString& PackagePath, TArray<FText>& Errors)
{
	TUniquePtr<IFileHandle> TempFileHandle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*PackagePath));
	if (!TempFileHandle.IsValid())
	{
		FText ErrorMsg = FText::Format(LOCTEXT("Virtualization_OpenValidationFailed", "Unable to open '{0}' so that it can be validated"),
			FText::FromString(PackagePath));
		Errors.Add(ErrorMsg);
		return false;
	}

	TempFileHandle->SeekFromEnd(-4);

	uint32 PackageTag = INDEX_NONE;
	if (!TempFileHandle->Read((uint8*)&PackageTag, 4) || PackageTag != PACKAGE_FILE_TAG)
	{
		FText ErrorMsg = FText::Format(LOCTEXT("Virtualization_ValidationFailed", "The package '{0}' does not end with a valid tag, the file is considered corrupt"),
			FText::FromString(PackagePath));
		Errors.Add(ErrorMsg);
		return false;
	}


	return true;
}

/** 
 * Creates a copy of the given package but the copy will not include the FPackageTrailer.
 * 
 * @param PackagePath	The path of the package to copy
 * @param CopyPath		The path where the copy should be created
 * @param Trailer		The trailer found in 'PackagePath' that is already loaded
 * @param Errors [out]	Errors created by the function will be added here
 * 
 * @return Returns true if the package was copied correctly, false otherwise. Note even when returning false a file might have been created at 'CopyPath'
 */
bool TryCopyPackageWithoutTrailer(const FPackagePath PackagePath, const FString& CopyPath, const FPackageTrailer& Trailer, TArray<FText>& Errors)
{
	// TODO: Consider adding a custom copy routine to only copy the data we want, rather than copying the full file then truncating

	const FString PackageFilePath = PackagePath.GetLocalFullPath();

	if (IFileManager::Get().Copy(*CopyPath, *PackageFilePath) != ECopyResult::COPY_OK)
	{
		FText Message = FText::Format(	LOCTEXT("Virtualization_CopyFailed", "Unable to copy package file '{0}' for virtualization"),
										FText::FromString(PackagePath.GetDebugName()));
		Errors.Add(Message);
		return false;
	}

	const int64 PackageSizeWithoutTrailer = IFileManager::Get().FileSize(*PackageFilePath) - Trailer.GetTrailerLength();

	{
		TUniquePtr<IFileHandle> TempFileHandle(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*CopyPath, true));
		if (!TempFileHandle.IsValid())
		{
			FText Message = FText::Format(LOCTEXT("Virtualization_TruncOpenFailed", "Failed to open package file for truncation'{0}' when virtualizing"),
				FText::FromString(CopyPath));
			Errors.Add(Message);
			return false;
		}

		if (!TempFileHandle->Truncate(PackageSizeWithoutTrailer))
		{
			FText Message = FText::Format(LOCTEXT("Virtualization_TruncFailed", "Failed to truncate '{0}' when virtualizing"),
				FText::FromString(CopyPath));
			Errors.Add(Message);
			return false;
		}
	}

#if UE_VALIDATE_TRUNCATED_PACKAGE
	// Validate we didn't break the package
	if (!ValidatePackage(CopyPath, Errors))
	{
		return false;
	}
#endif //UE_VALIDATE_TRUNCATED_PACKAGE

	return true;
}

void OnPrePackageSubmission(const TArray<FString>& FilesToSubmit, TArray<FText>& DescriptionTags, TArray<FText>& Errors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UE::Virtualization::OnPrePackageSubmission);

	IVirtualizationSystem& System = IVirtualizationSystem::Get();

	// TODO: We could check to see if the package is virtualized even if it is disabled for the project
	// as a safety feature?
	if (!System.IsEnabled())
	{
		return;
	}

	// Can't virtualize if the payload trailer system is disabled
	if (!FPackageTrailer::IsEnabled())
	{
		return;
	}

	if (!System.IsPushingEnabled(EStorageType::Persistent))
	{
		UE_LOG(LogVirtualization, Verbose, TEXT("Pushing to persistent backend storage is disabled"));
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Other systems may have added errors to this array, we need to check so later we can determine if this function added any additional errors.
	const int32 NumErrors = Errors.Num();

	struct FPackageInfo
	{
		FPackagePath Path;
		FPackageTrailer Trailer;

		TArray<FIoHash> LocalPayloads;
		int32 PayloadIndex = INDEX_NONE;

		bool bWasTrailerUpdated = false;
	};

	UE_LOG(LogVirtualization, Display, TEXT("Considering %d file(s) for virtualization"), FilesToSubmit.Num());

	TArray<FPackageInfo> Packages;
	Packages.Reserve(FilesToSubmit.Num());

	TArray<FIoHash> AllLocalPayloads;
	AllLocalPayloads.Reserve(FilesToSubmit.Num());

	// From the list of files to submit we need to find all of the valid packages that contain
	// local payloads that need to be virtualized.
	int64 TotalPayloadsToCheck = 0;
	for (const FString& AbsoluteFilePath : FilesToSubmit)
	{
		FPackagePath PackagePath = FPackagePath::FromLocalPath(AbsoluteFilePath);

		// TODO: How to handle text packages?
		if (FPackageName::IsPackageExtension(PackagePath.GetHeaderExtension()) || FPackageName::IsTextPackageExtension(PackagePath.GetHeaderExtension()))
		{
			FPackageTrailer Trailer;
			if (FPackageTrailer::TryLoadFromPackage(PackagePath, Trailer))
			{
				// The following is not expected to ever happen, currently we give a user facing error but it generally means that the asset is broken somehow.
				ensureMsgf(Trailer.GetNumPayloads(EPayloadFilter::Referenced) == 0, TEXT("Trying to virtualize a package that already contains payload references which the workspace file should not ever contain!"));
				if (Trailer.GetNumPayloads(EPayloadFilter::Referenced) > 0)
				{
					FText Message = FText::Format(	LOCTEXT("Virtualization_PkgHasReferences", "Cannot virtualize the package '{1}' as it has referenced payloads in the trailer"),
													FText::FromString(PackagePath.GetDebugName()));
					Errors.Add(Message);
					return;
				}

				FPackageInfo PkgInfo;

				PkgInfo.Path = MoveTemp(PackagePath);
				PkgInfo.Trailer = MoveTemp(Trailer);
				PkgInfo.LocalPayloads = PkgInfo.Trailer.GetPayloads(EPayloadFilter::Local);

				TotalPayloadsToCheck += PkgInfo.LocalPayloads.Num();

				if (!PkgInfo.LocalPayloads.IsEmpty())
				{
					PkgInfo.PayloadIndex = AllLocalPayloads.Num();
					AllLocalPayloads.Append(PkgInfo.LocalPayloads);

					Packages.Emplace(MoveTemp(PkgInfo));
				}
			}
		}
	}

	UE_LOG(LogVirtualization, Display, TEXT("Found %" INT64_FMT " payload(s) in %d package(s) that need to be examined for virtualization"), TotalPayloadsToCheck, Packages.Num());

	TArray<FPayloadStatus> PayloadStatuses;
	if (!System.DoPayloadsExist(AllLocalPayloads, EStorageType::Persistent, PayloadStatuses))
	{
		FText Message = LOCTEXT("Virtualization_DoesExistFail", "Failed to find the status of the payloads in the packages being submitted");
		Errors.Add(Message);

		return;
	}

	// Update payloads that are already in persistent storage and don't need to be pushed
	int64 TotalPayloadsToVirtualize = 0;
	for (FPackageInfo& PackageInfo : Packages)
	{
		check(PackageInfo.LocalPayloads.IsEmpty() || PackageInfo.PayloadIndex != INDEX_NONE); // If we have payloads we should have an index

#if UE_PRECHECK_PAYLOAD_STATUS
		for (int32 Index = 0; Index < PackageInfo.LocalPayloads.Num(); ++Index)
		{
			if (PayloadStatuses[PackageInfo.PayloadIndex + Index] == FPayloadStatus::FoundAll)
			{
				if (PackageInfo.Trailer.UpdatePayloadAsVirtualized(PackageInfo.LocalPayloads[Index]))
				{
					PackageInfo.bWasTrailerUpdated = true;
				}
				else
				{
					FText Message = FText::Format(	LOCTEXT("Virtualization_UpdateStatusFailed", "Unable to update the status for the payload '{0}' in the package '{1}'"),
													FText::FromString(LexToString(PackageInfo.LocalPayloads[Index])),
													FText::FromString(PackageInfo.Path.GetDebugName()));
					Errors.Add(Message);
					return;
				}
			}
		}

		// If we made changes we should recalculate the local payloads left
		if (PackageInfo.bWasTrailerUpdated)
		{
			PackageInfo.LocalPayloads = PackageInfo.Trailer.GetPayloads(EPayloadFilter::Local);
		}
#endif

		PackageInfo.PayloadIndex = INDEX_NONE;
		TotalPayloadsToVirtualize += PackageInfo.LocalPayloads.Num();
	}

	UE_LOG(LogVirtualization, Display, TEXT("Found %" INT64_FMT " payload(s) that potentially need to be pushed to persistent virtualized storage"), TotalPayloadsToVirtualize);

	// TODO Optimization: In theory we could have many packages sharing the same payload and we only need to push once
	
	TArray<Virtualization::FPushRequest> PayloadsToSubmit;
	PayloadsToSubmit.Reserve(TotalPayloadsToVirtualize);

	// Push any remaining local payload to the persistent backends
	for (FPackageInfo& PackageInfo : Packages)
	{
		if (PackageInfo.LocalPayloads.IsEmpty())
		{
			continue;
		}

		TUniquePtr<FArchive> PackageAr = IPackageResourceManager::Get().OpenReadExternalResource(EPackageExternalResource::WorkspaceDomainFile, PackageInfo.Path.GetPackageName());

		if (!PackageAr.IsValid())
		{
			FText Message = FText::Format(	LOCTEXT("Virtualization_PkgOpen", "Failed to open the package '{1}' for reading"),
											FText::FromString(PackageInfo.Path.GetDebugName()));
			Errors.Add(Message);
			return;
		}

		PackageInfo.PayloadIndex = PayloadsToSubmit.Num();

		for (const FIoHash& PayloadId : PackageInfo.LocalPayloads)
		{
			checkf(!PayloadId.IsZero(), TEXT("PackageTrailer for package '%s' should not contain invalid FIoHashs"), *PackageInfo.Path.GetDebugName());
			
			FCompressedBuffer Payload = PackageInfo.Trailer.LoadLocalPayload(PayloadId, *PackageAr);

			if (PayloadId != FIoHash(Payload.GetRawHash()))
			{
				FText Message = FText::Format(	LOCTEXT("Virtualization_WrongPayload", "Package {0} loaded an incorrect payload from the trailer. Expected '{1}' Loaded  '{2}'"),
												FText::FromString(PackageInfo.Path.GetDebugName()),
												FText::FromString(LexToString(PayloadId)),
												FText::FromString(LexToString(Payload.GetRawHash())));
				Errors.Add(Message);
				return;
			}

			if (!Payload)
			{
				FText Message = FText::Format(	LOCTEXT("Virtualization_MissingPayload", "Unable to find the payload '{0}' in the local storage of package '{1}'"),
												FText::FromString(LexToString(PayloadId)),
												FText::FromString(PackageInfo.Path.GetDebugName()));
				Errors.Add(Message);
				return;

			}

			PayloadsToSubmit.Emplace(PayloadId, MoveTemp(Payload), PackageInfo.Path.GetDebugName());
		}
	}

	if (!System.PushData(PayloadsToSubmit, EStorageType::Persistent))
	{
		FText Message = LOCTEXT("Virtualization_PushFailure", "Failed to push payloads");
		Errors.Add(Message);
		return;
	}

	int64 TotalPayloadsVirtualized = 0;
	for (const Virtualization::FPushRequest& Request : PayloadsToSubmit)
	{
		TotalPayloadsVirtualized += Request.Status == FPushRequest::EStatus::Success ? 1 : 0;
	}
	UE_LOG(LogVirtualization, Display, TEXT("Pushed %" INT64_FMT " payload(s) to persistent virtualized storage"), TotalPayloadsVirtualized);

	// Update the package info for the submitted payloads
	for (FPackageInfo& PackageInfo : Packages)
	{
		for (int32 Index = 0; Index < PackageInfo.LocalPayloads.Num(); ++Index)
		{
			const Virtualization::FPushRequest& Request = PayloadsToSubmit[PackageInfo.PayloadIndex + Index];
			check(Request.Identifier == PackageInfo.LocalPayloads[Index]);

			if (Request.Status == Virtualization::FPushRequest::EStatus::Success)
			{
				if (PackageInfo.Trailer.UpdatePayloadAsVirtualized(Request.Identifier))
				{
					PackageInfo.bWasTrailerUpdated = true;
				}
				else
				{
					FText Message = FText::Format(	LOCTEXT("Virtualization_UpdateStatusFailed", "Unable to update the status for the payload '{0}' in the package '{1}'"),
													FText::FromString(LexToString(Request.Identifier)),
													FText::FromString(PackageInfo.Path.GetDebugName()));
					Errors.Add(Message);
					return;
				}
			}
		}
	}

	TArray<TPair<FPackagePath, FString>> PackagesToReplace;

	// Any package with an updated trailer needs to be copied and an updated trailer appended
	for (FPackageInfo& PackageInfo : Packages)
	{
		if (!PackageInfo.bWasTrailerUpdated)
		{
			continue;
		}

		const FPackagePath& PackagePath = PackageInfo.Path; // No need to validate path, we checked this earlier

		const FString PackageFilePath = PackagePath.GetLocalFullPath();
		const FString BaseName = FPaths::GetBaseFilename(PackagePath.GetPackageName());
		const FString TempFilePath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), *BaseName.Left(32));

		// TODO Optimization: Combine TryCopyPackageWithoutTrailer with the appending of the new trailer to avoid opening multiple handles

		// Create copy of package minus the trailer the trailer
		if (!TryCopyPackageWithoutTrailer(PackagePath, TempFilePath, PackageInfo.Trailer, Errors))
		{
			return;
		}			

		TUniquePtr<FArchive> PackageAr = IPackageResourceManager::Get().OpenReadExternalResource(EPackageExternalResource::WorkspaceDomainFile, PackagePath.GetPackageName());

		if (!PackageAr.IsValid())
		{
			FText Message = FText::Format(	LOCTEXT("Virtualization_PkgOpen", "Failed to open the package '{1}' for reading"),
											FText::FromString(PackagePath.GetDebugName()));
			Errors.Add(Message);
			return;
		}

		TUniquePtr<FArchive> CopyAr(IFileManager::Get().CreateFileWriter(*TempFilePath, EFileWrite::FILEWRITE_Append));
		if (!CopyAr.IsValid())
		{
			FText Message = FText::Format(	LOCTEXT("Virtualization_TrailerAppendOpen", "Unable to open '{0}' to append the trailer'"),
											FText::FromString(TempFilePath));
			Errors.Add(Message);
			return;
		}

		FPackageTrailerBuilder TrailerBuilder = FPackageTrailerBuilder::CreateFromTrailer(PackageInfo.Trailer, *PackageAr, PackagePath.GetPackageFName());
		if (!TrailerBuilder.BuildAndAppendTrailer(nullptr, *CopyAr))
		{
			FText Message = FText::Format(	LOCTEXT("Virtualization_TrailerAppend", "Failed to append the trailer to '{0}'"),
											FText::FromString(TempFilePath));
			Errors.Add(Message);
			return;
		}

		// Now that we have successfully created a new version of the package with an updated trailer 
		// we need to mark that it should replace the original package.
		PackagesToReplace.Emplace(PackagePath, TempFilePath);
	}

	UE_LOG(LogVirtualization, Display, TEXT("%d package(s) had their trailer container modified and need to be updated"), PackagesToReplace.Num());

	if (NumErrors == Errors.Num())
	{
		// TODO: Consider using the SavePackage model (move the original, then replace, so we can restore all of the original packages if needed)
		// having said that, once a package is in PackagesToReplace it should still be safe to submit so maybe we don't need this level of protection?

		// We need to reset the loader of any package that we want to re-save over
		for (const TPair<FPackagePath, FString>& Iterator : PackagesToReplace)
		{
			UPackage* Package = FindObjectFast<UPackage>(nullptr, Iterator.Key.GetPackageFName());
			if (Package != nullptr)
			{
				ResetLoadersForSave(Package, *Iterator.Key.GetLocalFullPath());
			}
		}

		// Since we had no errors we can now replace all of the packages that were virtualized data with the virtualized replacement file.
		for(const TPair<FPackagePath,FString>&  Iterator : PackagesToReplace)
		{
			const FString OriginalPackagePath = Iterator.Key.GetLocalFullPath();
			const FString& NewPackagePath = Iterator.Value;

			if (!IFileManager::Get().Move(*OriginalPackagePath, *NewPackagePath))
			{
				FText Message = FText::Format(	LOCTEXT("Virtualization_MoveFailed", "Unable to replace the package '{0}' with the virtualized version"),
												FText::FromString(Iterator.Key.GetDebugName()));
				Errors.Add(Message);
				continue;
			}
		}
	}

	// If we had no new errors add the validation tag to indicate that the packages are safe for submission. 
	// TODO: Currently this is a simple tag to make it easier for us to track which assets were submitted via the
	// virtualization process in a test project. This should be expanded when we add proper p4 server triggers.
	if (NumErrors == Errors.Num())
	{
		FText Tag = FText::FromString(TEXT("#virtualized"));
		DescriptionTags.Add(Tag);
	}

	const double TimeInSeconds = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogVirtualization, Verbose, TEXT("Virtualization pre submit check took %.3f(s)"), TimeInSeconds);
}

} // namespace UE::Virtualization

#undef LOCTEXT_NAMESPACE

