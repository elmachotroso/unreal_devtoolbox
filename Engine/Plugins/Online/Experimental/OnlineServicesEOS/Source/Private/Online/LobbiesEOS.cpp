// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/LobbiesEOS.h"
#include "Online/LobbiesEOSTypes.h"
#include "Online/OnlineAsyncOp.h"
#include "Online/OnlineIdEOS.h"
#include "Online/OnlineServicesEOS.h"
#include "Online/OnlineServicesEOSTypes.h"
#include "Online/AuthEOS.h"
#include "IEOSSDKManager.h"

#include "eos_lobby.h"

namespace UE::Online {

static const FString LobbyDataKeyName = TEXT("LobbyData");
static const FString LobbyDetailsKeyName = TEXT("LobbyDetails");
static const FString LobbyChangesKeyName = TEXT("LobbyChanges");
static const FString LobbyMemberChangesKeyName = TEXT("LobbyMemberChanges");

static const int32 MaxAttributeSize = 1000;

template <typename DataType, typename OpType>
const DataType& GetOpDataChecked(const TOnlineAsyncOp<OpType>& Op, const FString& Key)
{
	const DataType* Data = Op.Data.template Get<DataType>(Key);
	check(Data);
	return *Data;
}

FLobbiesEOS::FLobbiesEOS(FOnlineServicesEOS& InServices)
	: FLobbiesCommon(InServices)
{
}

void FLobbiesEOS::Initialize()
{
	FLobbiesCommon::Initialize();

	IEOSSDKManager* SDKManager = IEOSSDKManager::Get();
	check(SDKManager);

	EOS_HLobby LobbyInterfaceHandle = EOS_Platform_GetLobbyInterface(static_cast<FOnlineServicesEOS&>(GetServices()).GetEOSPlatformHandle());
	check(LobbyInterfaceHandle != nullptr);

	LobbyPrerequisites = MakeShared<FLobbyPrerequisitesEOS>(FLobbyPrerequisitesEOS{
		LobbyInterfaceHandle,
		StaticCastSharedPtr<FAuthEOS>(Services.GetAuthInterface()),
		LobbySchemaRegistry.ToSharedRef(),
		ServiceSchema.ToSharedRef(),
		{SDKManager->GetProductName(), GetBuildUniqueId()}});

	LobbyDataRegistry = MakeShared<FLobbyDataRegistryEOS>(LobbyPrerequisites.ToSharedRef());

	RegisterHandlers();
}

void FLobbiesEOS::PreShutdown()
{
	UnregisterHandlers();

	LobbyDataRegistry = nullptr;
	LobbyPrerequisites = nullptr;
}

TOnlineAsyncOpHandle<FCreateLobby> FLobbiesEOS::CreateLobby(FCreateLobby::Params&& InParams)
{
	TOnlineAsyncOpRef<FCreateLobby> Op = GetOp<FCreateLobby>(MoveTemp(InParams));
	const FCreateLobby::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	auto DestroyLobbyDuringCreate = [this](
		TOnlineAsyncOp<FCreateLobby>& InAsyncOp,
		const TSharedPtr<FLobbyDataEOS>& LobbyData,
		FOnlineAccountIdHandle LocalUserId,
		FOnlineError ErrorResult) -> TFuture<void>
	{
		FDestroyLobbyImpl::Params DestroyLobbyParams;
		DestroyLobbyParams.LobbyData = LobbyData;
		DestroyLobbyParams.LocalUserId = LocalUserId;

		TPromise<void> Promise;
		auto Future = Promise.GetFuture();

		DestroyLobbyImpl(MoveTemp(DestroyLobbyParams))
		.Then([this, InAsyncOp = InAsyncOp.AsShared(), ErrorResult = MoveTemp(ErrorResult), Promise = MoveTemp(Promise)](TFuture<TDefaultErrorResult<FDestroyLobbyImpl>>&& Future) mutable
		{
			if (Future.Get().IsError())
			{
				// Todo: complain about having an error while handling an error.
			}

			// Todo: Errors.
			InAsyncOp->SetError(Errors::Unknown(MoveTemp(ErrorResult)));
			Promise.EmplaceValue();
		});

		return Future;
	};

	Op->Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp)
	{
		// Step 1: Call create lobby.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		const FLobbyBucketIdTranslator<ELobbyTranslationType::ToService> BucketTanslator(LobbyPrerequisites->BucketId);

		// The lobby will be created as invitation only.
		// Once all local members are joined and the lobby attributes have been set the privacy setting will be moved to the user setting.

		EOS_Lobby_CreateLobbyOptions CreateLobbyOptions = {};
		CreateLobbyOptions.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
		CreateLobbyOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
		CreateLobbyOptions.MaxLobbyMembers = Params.MaxMembers;
		CreateLobbyOptions.PermissionLevel = TranslateJoinPolicy(ELobbyJoinPolicy::InvitationOnly);
		CreateLobbyOptions.bPresenceEnabled = false; // todo: handle
		CreateLobbyOptions.bAllowInvites = true; // todo: handle
		CreateLobbyOptions.BucketId = BucketTanslator.GetBucketIdEOS();
		CreateLobbyOptions.bDisableHostMigration = false; // todo: handle
		CreateLobbyOptions.bEnableRTCRoom = false; // todo: handle

		return EOS_Async<EOS_Lobby_CreateLobbyCallbackInfo>(EOS_Lobby_CreateLobby, LobbyPrerequisites->LobbyInterfaceHandle, CreateLobbyOptions); 
	})
	.Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp, const EOS_Lobby_CreateLobbyCallbackInfo* Data)
	{
		// Step 2: Start creating the lobby data from the EOS lobby details object.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();

		if (Data->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: errors
			InAsyncOp.SetError(Errors::Unknown(FromEOSError(Data->ResultCode)));
			return MakeFulfilledPromise<TDefaultErrorResultInternal<TSharedRef<FLobbyDataEOS>>>().GetFuture();
		}

		TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsEOS>> LobbyDetailsResult =
			FLobbyDetailsEOS::CreateFromLobbyId(LobbyPrerequisites.ToSharedRef(), Params.LocalUserId, Data->LobbyId);
		if (LobbyDetailsResult.IsError())
		{
			// Todo: manually call eos destroy lobby here.

			// Todo: errors
			InAsyncOp.SetError(MoveTemp(LobbyDetailsResult.GetErrorValue()));
			return MakeFulfilledPromise<TDefaultErrorResultInternal<TSharedRef<FLobbyDataEOS>>>().GetFuture();
		}
		
		return LobbyDataRegistry->FindOrCreateFromLobbyDetails(InAsyncOp.GetParams().LocalUserId, LobbyDetailsResult.GetOkValue());
	})
	.Then([this, DestroyLobbyDuringCreate](TOnlineAsyncOp<FCreateLobby>& InAsyncOp, TDefaultErrorResultInternal<TSharedRef<FLobbyDataEOS>>&& Result)
	{
		// Step 3: Store the lobby data on the async op properties.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();

		if (Result.IsError())
		{
			// Todo: destroy lobby.
			// Change DestroyLobbyImpl to take an EOS_LobbyId instead of an FLobbyDataEOS.

			// Todo: errors
			InAsyncOp.SetError(Errors::Unknown(MoveTemp(Result.GetErrorValue())));
			return;
		}

		// Store lobby data on the operation.
		TSharedRef<FLobbyDataEOS> LobbyData = Result.GetOkValue();
		InAsyncOp.Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, MoveTemp(LobbyData));
	})
	.Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp)
	{
		// Step 4: Set attributes for the lobby creator.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		// Add creator attributes if set.
		// Todo: make this nicer.
		const FJoinLobbyLocalUserData* UserData = Params.LocalUsers.FindByPredicate(
		[LocalUserId = Params.LocalUserId](const FJoinLobbyLocalUserData& Data)
		{
			return LocalUserId == Data.LocalUserId;
		});

		TSharedRef<FClientLobbyMemberDataChanges> LobbyOwnerAttributes = MakeShared<FClientLobbyMemberDataChanges>();

		// Add owner attributes to operation data.
		// CreatingMemberData is used to update the local lobby data and for dispatching notifications once creation has completed.
		TMap<FOnlineAccountIdHandle, TSharedRef<FClientLobbyMemberDataChanges>> CreatingMemberData;
		CreatingMemberData.Reserve(Params.LocalUsers.Num());
		CreatingMemberData.Add(Params.LocalUserId, LobbyOwnerAttributes);
		InAsyncOp.Data.Set<TMap<FOnlineAccountIdHandle, TSharedRef<FClientLobbyMemberDataChanges>>>(LobbyMemberChangesKeyName, MoveTemp(CreatingMemberData));

		if (UserData)
		{
			LobbyOwnerAttributes->MutatedAttributes = UserData->Attributes;

			FModifyLobbyMemberDataImpl::Params ModifyLobbyMemberDataParams;
			ModifyLobbyMemberDataParams.LobbyData = LobbyData;
			ModifyLobbyMemberDataParams.LocalUserId = Params.LocalUserId;
			ModifyLobbyMemberDataParams.Changes = LobbyOwnerAttributes;

			return ModifyLobbyMemberDataImpl(MoveTemp(ModifyLobbyMemberDataParams));
		}
		else
		{
			return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyMemberDataImpl>>().GetFuture();
		}
	})
	.Then([this, DestroyLobbyDuringCreate](TOnlineAsyncOp<FCreateLobby>& InAsyncOp, TDefaultErrorResult<FModifyLobbyMemberDataImpl>&& ModifyLobbyOwnerResult)
	{
		// Step 5: Handle result

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		TSharedRef<FLobbyDataEOS> LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		if (ModifyLobbyOwnerResult.IsError())
		{
			return DestroyLobbyDuringCreate(InAsyncOp, LobbyData, Params.LocalUserId, MoveTemp(ModifyLobbyOwnerResult.GetErrorValue()));
		}
		else
		{
			return MakeFulfilledPromise<void>().GetFuture();
		}
	})
	.Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp)
	{
		// Step 6: Todo: Add other local members.

		// Store member attributes on the operation.
	})
	.Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp)
	{
		// Step 7: Add lobby attributes, set lobby join policy to user setting.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		// Track lobby changes to sync client-side cache at operation completion.
		TSharedRef<FClientLobbyDataChanges> LobbyChanges = MakeShared<FClientLobbyDataChanges>();
		InAsyncOp.Data.Set<TSharedRef<FClientLobbyDataChanges>>(LobbyChangesKeyName, LobbyChanges);
		LobbyChanges->MutatedAttributes = Params.Attributes;
		LobbyChanges->JoinPolicy = Params.JoinPolicy;

		// Add lobby attributes.
		// Set lobby privacy setting to the user provided value.
		FModifyLobbyDataImpl::Params ModifyLobbyDataParams;
		ModifyLobbyDataParams.LobbyData = LobbyData;
		ModifyLobbyDataParams.LocalUserId = Params.LocalUserId;
		ModifyLobbyDataParams.Changes = LobbyChanges;

		return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
	})
	.Then([this, DestroyLobbyDuringCreate](TOnlineAsyncOp<FCreateLobby>& InAsyncOp, TDefaultErrorResult<FModifyLobbyDataImpl>&& ModifyLobbyResult) mutable
	{
		// Step 8: Handle result.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		if (ModifyLobbyResult.IsError())
		{
			return DestroyLobbyDuringCreate(InAsyncOp, LobbyData, Params.LocalUserId, MoveTemp(ModifyLobbyResult.GetErrorValue()));
		}
		else
		{
			return MakeFulfilledPromise<void>().GetFuture();
		}
	})
	.Then([this](TOnlineAsyncOp<FCreateLobby>& InAsyncOp)
	{
		// Step 9: Add the lobby to the active list for each member, apply changes to the cached
		// lobby object, and signal notifications.
		// The active lobbies list holds a reference to the lobby data to keep it from being cleaned up.

		const FCreateLobby::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		// Mark the lobby active for each member.
		for (const FJoinLobbyLocalUserData& LocalUser : Params.LocalUsers)
		{
			AddActiveLobby(LocalUser.LocalUserId, LobbyData);
		}

		// Add member changes to the lobby changes object.
		const TSharedRef<FClientLobbyDataChanges>& LobbyChanges = GetOpDataChecked<TSharedRef<FClientLobbyDataChanges>>(InAsyncOp, LobbyChangesKeyName);
		const TMap<FOnlineAccountIdHandle, TSharedRef<FClientLobbyMemberDataChanges>>& MemberChanges =
			GetOpDataChecked<TMap<FOnlineAccountIdHandle, TSharedRef<FClientLobbyMemberDataChanges>>>(InAsyncOp, LobbyMemberChangesKeyName);
		LobbyChanges->MutatedMembers = MemberChanges;
		LobbyChanges->LocalName = InAsyncOp.GetParams().LocalName;

		// Make local changes to lobby data and generate notifications.
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(*LobbyChanges), LobbyEvents);

		InAsyncOp.SetResult(FCreateLobby::Result{LobbyData->GetClientLobbyData()->GetPublicDataPtr()});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FFindLobbies> FLobbiesEOS::FindLobbies(FFindLobbies::Params&& InParams)
{
	TOnlineAsyncOpRef<FFindLobbies> Op = GetOp<FFindLobbies>(MoveTemp(InParams));
	const FFindLobbies::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	// Invalidate previous search results.
	ActiveSearchResults.Remove(Params.LocalUserId);

	Op->Then([this](TOnlineAsyncOp<FFindLobbies>& InAsyncOp)
	{
		return FLobbySearchEOS::Create(LobbyPrerequisites.ToSharedRef(), LobbyDataRegistry.ToSharedRef(), InAsyncOp.GetParams());
	})
	.Then([this](TOnlineAsyncOp<FFindLobbies>& InAsyncOp, TDefaultErrorResultInternal<TSharedRef<FLobbySearchEOS>> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
		else
		{
			ActiveSearchResults.Add(InAsyncOp.GetParams().LocalUserId, Result.GetOkValue());
			InAsyncOp.SetResult(FFindLobbies::Result{Result.GetOkValue()->GetLobbyResults()});
		}
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FJoinLobby> FLobbiesEOS::JoinLobby(FJoinLobby::Params&& InParams)
{
	TOnlineAsyncOpRef<FJoinLobby> Op = GetOp<FJoinLobby>(MoveTemp(InParams));
	const FJoinLobby::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::Unknown());
		return Op->GetHandle();
	}

	Op->Then([this, LobbyData](TOnlineAsyncOp<FJoinLobby>& InAsyncOp)
	{
		// Join all users to the lobby.

		const FJoinLobby::Params& Params = InAsyncOp.GetParams();

		FJoinLobbyImpl::Params JoinParams;
		JoinParams.LobbyData = LobbyData;
		JoinParams.LocalUserId = Params.LocalUserId;
		JoinParams.LocalName = Params.LocalName;
		JoinParams.LocalUsers = Params.LocalUsers;
		return JoinLobbyImpl(MoveTemp(JoinParams));
	})
	.Then([this](TOnlineAsyncOp<FJoinLobby>& InAsyncOp, TDefaultErrorResult<FJoinLobbyImpl> Result)
	{
		// Handle result.
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this, LobbyData](TOnlineAsyncOp<FJoinLobby>& InAsyncOp)
	{
		// Mark the lobby active for each member.
		// Add users to local lobby data and dispatch notifications.

		const FJoinLobby::Params& Params = InAsyncOp.GetParams();

		// Mark the lobby active for each member.
		for (const FJoinLobbyLocalUserData& LocalUser : Params.LocalUsers)
		{
			AddActiveLobby(LocalUser.LocalUserId, LobbyData.ToSharedRef());
		}

		// todo: figure out a butter way to handle attribute parameters.
		FClientLobbyDataChanges LobbyChanges;
		LobbyChanges.LocalName = InAsyncOp.GetParams().LocalName;

		for (const FJoinLobbyLocalUserData& LocalUser : Params.LocalUsers)
		{
			TSharedRef<FClientLobbyMemberDataChanges> MemberDataChanges = MakeShared<FClientLobbyMemberDataChanges>();
			MemberDataChanges->MutatedAttributes = LocalUser.Attributes;
			LobbyChanges.MutatedMembers.Add(LocalUser.LocalUserId, MoveTemp(MemberDataChanges));
		}
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(LobbyChanges), LobbyEvents);

		InAsyncOp.SetResult(FJoinLobby::Result{ LobbyData->GetClientLobbyData()->GetPublicDataPtr()});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FLeaveLobby> FLobbiesEOS::LeaveLobby(FLeaveLobby::Params&& InParams)
{
	TOnlineAsyncOpRef<FLeaveLobby> Op = GetOp<FLeaveLobby>(MoveTemp(InParams));
	const FLeaveLobby::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::Unknown());
		return Op->GetHandle();
	}

	Op->Then([this, LobbyData](TOnlineAsyncOp<FLeaveLobby>& InAsyncOp)
	{
		// Remove the user from the EOS lobby.

		const FLeaveLobby::Params& Params = InAsyncOp.GetParams();

		FLeaveLobbyImpl::Params LeaveParams;
		LeaveParams.LobbyData = LobbyData;
		LeaveParams.LocalUserId = Params.LocalUserId;
		return LeaveLobbyImpl(MoveTemp(LeaveParams));
	})
	.Then([this, LobbyData](TOnlineAsyncOp<FLeaveLobby>& InAsyncOp, TDefaultErrorResult<FLeaveLobbyImpl> Result)
	{
		const FLeaveLobby::Params& Params = InAsyncOp.GetParams();

		// Remove the user from the local lobby data and dispatch notifications.
		FClientLobbyDataChanges LobbyChanges;
		LobbyChanges.LeavingMembers.Add(Params.LocalUserId, ELobbyMemberLeaveReason::Left);
		FApplyLobbyUpdateResult ApplyResult = LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(LobbyChanges), LobbyEvents);

		// Remove the lobby from the active list for the user.
		// The lobby data will be cleaned up once all references are removed.
		for (FOnlineAccountIdHandle LeavingMember : ApplyResult.LeavingLocalMembers)
		{
			RemoveActiveLobby(LeavingMember, LobbyData.ToSharedRef());
		}

		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
		else
		{
			InAsyncOp.SetResult(FLeaveLobby::Result{});
		}
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FInviteLobbyMember> FLobbiesEOS::InviteLobbyMember(FInviteLobbyMember::Params&& InParams)
{
	TOnlineAsyncOpRef<FInviteLobbyMember> Op = GetOp<FInviteLobbyMember>(MoveTemp(InParams));
	const FInviteLobbyMember::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	Op->Then([this](TOnlineAsyncOp<FInviteLobbyMember>& InAsyncOp)
	{
		const FInviteLobbyMember::Params& Params = InAsyncOp.GetParams();

		FInviteLobbyMemberImpl::Params InviteParams;
		InviteParams.LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
		InviteParams.LocalUserId = Params.LocalUserId;
		InviteParams.TargetUserId = Params.TargetUserId;
		return InviteLobbyMemberImpl(MoveTemp(InviteParams));
	})
	.Then([this](TOnlineAsyncOp<FInviteLobbyMember>& InAsyncOp, TDefaultErrorResult<FInviteLobbyMemberImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
		else
		{
			InAsyncOp.SetResult(FInviteLobbyMember::Result{});
		}
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FDeclineLobbyInvitation> FLobbiesEOS::DeclineLobbyInvitation(FDeclineLobbyInvitation::Params&& InParams)
{
	TOnlineAsyncOpRef<FDeclineLobbyInvitation> Op = GetOp<FDeclineLobbyInvitation>(MoveTemp(InParams));
	const FDeclineLobbyInvitation::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	Op->Then([this](TOnlineAsyncOp<FDeclineLobbyInvitation>& InAsyncOp)
	{
		const FDeclineLobbyInvitation::Params& Params = InAsyncOp.GetParams();

		FDeclineLobbyInvitationImpl::Params DeclineParams;
		DeclineParams.LocalUserId = Params.LocalUserId;
		DeclineParams.LobbyId = Params.LobbyId;
		return DeclineLobbyInvitationImpl(MoveTemp(DeclineParams));
	})
	.Then([this](TOnlineAsyncOp<FDeclineLobbyInvitation>& InAsyncOp, TDefaultErrorResult<FDeclineLobbyInvitationImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
		else
		{
			InAsyncOp.SetResult(FDeclineLobbyInvitation::Result{});
		}
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FKickLobbyMember> FLobbiesEOS::KickLobbyMember(FKickLobbyMember::Params&& InParams)
{
	TOnlineAsyncOpRef<FKickLobbyMember> Op = GetOp<FKickLobbyMember>(MoveTemp(InParams));
	const FKickLobbyMember::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}
	Op->Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, LobbyData.ToSharedRef());

	Op->Then([this](TOnlineAsyncOp<FKickLobbyMember>& InAsyncOp)
	{
		const FKickLobbyMember::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		FKickLobbyMemberImpl::Params KickParams;
		KickParams.LobbyData = LobbyData;
		KickParams.LocalUserId = Params.LocalUserId;
		KickParams.TargetUserId = Params.TargetUserId;
		return KickLobbyMemberImpl(MoveTemp(KickParams));
	})
	.Then([this](TOnlineAsyncOp<FKickLobbyMember>& InAsyncOp, TDefaultErrorResult<FKickLobbyMemberImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this](TOnlineAsyncOp<FKickLobbyMember>& InAsyncOp)
	{
		const FKickLobbyMember::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		// Update local cache and fire events.
		FClientLobbyDataChanges LobbyChanges;
		LobbyChanges.LeavingMembers.Add(Params.TargetUserId, ELobbyMemberLeaveReason::Kicked);
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(LobbyChanges), LobbyEvents);
		InAsyncOp.SetResult(FKickLobbyMember::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FPromoteLobbyMember> FLobbiesEOS::PromoteLobbyMember(FPromoteLobbyMember::Params&& InParams)
{
	TOnlineAsyncOpRef<FPromoteLobbyMember> Op = GetOp<FPromoteLobbyMember>(MoveTemp(InParams));
	const FPromoteLobbyMember::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}
	Op->Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, LobbyData.ToSharedRef());

	Op->Then([this](TOnlineAsyncOp<FPromoteLobbyMember>& InAsyncOp)
	{
		const FPromoteLobbyMember::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		FPromoteLobbyMemberImpl::Params PromoteParams;
		PromoteParams.LobbyData = LobbyData;
		PromoteParams.LocalUserId = Params.LocalUserId;
		PromoteParams.TargetUserId = Params.TargetUserId;
		return PromoteLobbyMemberImpl(MoveTemp(PromoteParams));
	})
	.Then([this](TOnlineAsyncOp<FPromoteLobbyMember>& InAsyncOp, TDefaultErrorResult<FPromoteLobbyMemberImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this](TOnlineAsyncOp<FPromoteLobbyMember>& InAsyncOp)
	{
		const FPromoteLobbyMember::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		// Update local cache and fire events.
		FClientLobbyDataChanges LobbyChanges;
		LobbyChanges.OwnerAccountId = Params.TargetUserId;
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(LobbyChanges), LobbyEvents);
		InAsyncOp.SetResult(FPromoteLobbyMember::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FModifyLobbyJoinPolicy> FLobbiesEOS::ModifyLobbyJoinPolicy(FModifyLobbyJoinPolicy::Params&& InParams)
{
	TOnlineAsyncOpRef<FModifyLobbyJoinPolicy> Op = GetOp<FModifyLobbyJoinPolicy>(MoveTemp(InParams));
	const FModifyLobbyJoinPolicy::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}
	Op->Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, LobbyData.ToSharedRef());

	Op->Then([this](TOnlineAsyncOp<FModifyLobbyJoinPolicy>& InAsyncOp)
	{
		const FModifyLobbyJoinPolicy::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		TSharedRef<FClientLobbyDataChanges> LobbyChanges = MakeShared<FClientLobbyDataChanges>();
		InAsyncOp.Data.Set<TSharedRef<FClientLobbyDataChanges>>(LobbyChangesKeyName, LobbyChanges);
		LobbyChanges->JoinPolicy = Params.JoinPolicy;

		FModifyLobbyDataImpl::Params ModifyLobbyDataParams;
		ModifyLobbyDataParams.LobbyData = LobbyData;
		ModifyLobbyDataParams.LocalUserId = Params.LocalUserId;
		ModifyLobbyDataParams.Changes = LobbyChanges;
		return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyJoinPolicy>& InAsyncOp, TDefaultErrorResult<FModifyLobbyDataImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyJoinPolicy>& InAsyncOp)
	{
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);
		const TSharedRef<FClientLobbyDataChanges>& LobbyChanges = GetOpDataChecked<TSharedRef<FClientLobbyDataChanges>>(InAsyncOp, LobbyChangesKeyName);

		// Update local cache and fire events.
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(*LobbyChanges), LobbyEvents);
		InAsyncOp.SetResult(FModifyLobbyJoinPolicy::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FModifyLobbyAttributes> FLobbiesEOS::ModifyLobbyAttributes(FModifyLobbyAttributes::Params&& InParams)
{
	TOnlineAsyncOpRef<FModifyLobbyAttributes> Op = GetOp<FModifyLobbyAttributes>(MoveTemp(InParams));
	const FModifyLobbyAttributes::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}
	Op->Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, LobbyData.ToSharedRef());

	Op->Then([this](TOnlineAsyncOp<FModifyLobbyAttributes>& InAsyncOp)
	{
		const FModifyLobbyAttributes::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		TSharedRef<FClientLobbyDataChanges> LobbyChanges = MakeShared<FClientLobbyDataChanges>();
		InAsyncOp.Data.Set<TSharedRef<FClientLobbyDataChanges>>(LobbyChangesKeyName, LobbyChanges);
		LobbyChanges->MutatedAttributes = Params.MutatedAttributes;
		LobbyChanges->ClearedAttributes = Params.ClearedAttributes;

		FModifyLobbyDataImpl::Params ModifyLobbyDataParams;
		ModifyLobbyDataParams.LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
		ModifyLobbyDataParams.LocalUserId = Params.LocalUserId;
		ModifyLobbyDataParams.Changes = LobbyChanges;
		return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyAttributes>& InAsyncOp, TDefaultErrorResult<FModifyLobbyDataImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyAttributes>& InAsyncOp)
	{
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);
		const TSharedRef<FClientLobbyDataChanges>& LobbyChanges = GetOpDataChecked<TSharedRef<FClientLobbyDataChanges>>(InAsyncOp, LobbyChangesKeyName);

		// Update local cache and fire events.
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(*LobbyChanges), LobbyEvents);
		InAsyncOp.SetResult(FModifyLobbyAttributes::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineAsyncOpHandle<FModifyLobbyMemberAttributes> FLobbiesEOS::ModifyLobbyMemberAttributes(FModifyLobbyMemberAttributes::Params&& InParams)
{
	TOnlineAsyncOpRef<FModifyLobbyMemberAttributes> Op = GetOp<FModifyLobbyMemberAttributes>(MoveTemp(InParams));
	const FModifyLobbyMemberAttributes::Params& Params = Op->GetParams();

	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		Op->SetError(Errors::InvalidUser());
		return Op->GetHandle();
	}

	TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
	if (!LobbyData)
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}
	Op->Data.Set<TSharedRef<FLobbyDataEOS>>(LobbyDataKeyName, LobbyData.ToSharedRef());

	Op->Then([this](TOnlineAsyncOp<FModifyLobbyMemberAttributes>& InAsyncOp)
	{
		const FModifyLobbyMemberAttributes::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);

		TSharedRef<FClientLobbyMemberDataChanges> LobbyMemberChanges = MakeShared<FClientLobbyMemberDataChanges>();
		InAsyncOp.Data.Set<TSharedRef<FClientLobbyMemberDataChanges>>(LobbyMemberChangesKeyName, LobbyMemberChanges);
		LobbyMemberChanges->MutatedAttributes = Params.MutatedAttributes;
		LobbyMemberChanges->ClearedAttributes = Params.ClearedAttributes;

		FModifyLobbyMemberDataImpl::Params ModifyLobbyMemberDataParams;
		ModifyLobbyMemberDataParams.LobbyData = LobbyData;
		ModifyLobbyMemberDataParams.LocalUserId = Params.LocalUserId;
		ModifyLobbyMemberDataParams.Changes = LobbyMemberChanges;
		return ModifyLobbyMemberDataImpl(MoveTemp(ModifyLobbyMemberDataParams));
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyMemberAttributes>& InAsyncOp, TDefaultErrorResult<FModifyLobbyMemberDataImpl> Result)
	{
		if (Result.IsError())
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(Result.GetErrorValue()));
		}
	})
	.Then([this](TOnlineAsyncOp<FModifyLobbyMemberAttributes>& InAsyncOp)
	{
		const FModifyLobbyMemberAttributes::Params& Params = InAsyncOp.GetParams();
		const TSharedRef<FLobbyDataEOS>& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataEOS>>(InAsyncOp, LobbyDataKeyName);
		const TSharedRef<FClientLobbyMemberDataChanges>& LobbyMemberChanges = GetOpDataChecked<TSharedRef<FClientLobbyMemberDataChanges>>(InAsyncOp, LobbyMemberChangesKeyName);

		// Update local cache and fire events.
		FClientLobbyDataChanges LobbyChanges;
		LobbyChanges.MutatedMembers.Add(Params.LocalUserId, LobbyMemberChanges);
		LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromLocalChanges(MoveTemp(LobbyChanges), LobbyEvents);
		InAsyncOp.SetResult(FModifyLobbyMemberAttributes::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

TOnlineResult<FGetJoinedLobbies> FLobbiesEOS::GetJoinedLobbies(FGetJoinedLobbies::Params&& Params)
{
	if (const TSet<TSharedRef<FLobbyDataEOS>>* Lobbies = ActiveLobbies.Find(Params.LocalUserId))
	{
		FGetJoinedLobbies::Result Result;
		Result.Lobbies.Reserve(Lobbies->Num());
		for (const TSharedRef<FLobbyDataEOS>& LobbyDataEOS : *Lobbies)
		{
			Result.Lobbies.Emplace(LobbyDataEOS->GetClientLobbyData()->GetPublicDataPtr());
		}
		return TOnlineResult<FGetJoinedLobbies>(MoveTemp(Result));
	}
	else
	{
		return TOnlineResult<FGetJoinedLobbies>(Errors::InvalidUser());
	}
}

void FLobbiesEOS::HandleLobbyUpdated(const EOS_Lobby_LobbyUpdateReceivedCallbackInfo* Data)
{
	if (TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Data->LobbyId))
	{
		FProcessLobbyNotificationImpl::Params Params;
		Params.LobbyData = LobbyData;

		ProcessLobbyNotificationImplOp(MoveTemp(Params))
		.OnComplete([LobbyData](const TOnlineResult<FProcessLobbyNotificationImpl>& Result)
		{
			if (Result.IsError())
			{
				// Todo: handle failure to update lobby from snapshot.
				UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::HandleLobbyUpdated] Failed to apply update. Lobby: %s, Error: %s"), *LobbyData->GetLobbyId(), *Result.GetErrorValue().GetLogString());
			}
		});
	}
}

void FLobbiesEOS::HandleLobbyMemberUpdated(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* Data)
{
	if (TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Data->LobbyId))
	{
		FProcessLobbyNotificationImpl::Params Params;
		Params.LobbyData = LobbyData;
		Params.MutatedMembers.Add(Data->TargetUserId);

		ProcessLobbyNotificationImplOp(MoveTemp(Params))
		.OnComplete([LobbyData](const TOnlineResult<FProcessLobbyNotificationImpl>& Result)
		{
			if (Result.IsError())
			{
				// Todo: handle failure to update lobby from snapshot.
				UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::HandleLobbyMemberUpdated] Failed to apply update. Lobby: %s, Error: %s"), *LobbyData->GetLobbyId(), *Result.GetErrorValue().GetLogString());
			}
		});
	}
}

void FLobbiesEOS::HandleLobbyMemberStatusReceived(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* Data)
{
	if (TSharedPtr<FLobbyDataEOS> LobbyData = LobbyDataRegistry->Find(Data->LobbyId))
	{
		FProcessLobbyNotificationImpl::Params Params;
		Params.LobbyData = LobbyData;

		switch (Data->CurrentStatus)
		{
		case EOS_ELobbyMemberStatus::EOS_LMS_JOINED:
			// Fetch member snapshot on join.
			Params.MutatedMembers.Add(Data->TargetUserId);
			break;

		case EOS_ELobbyMemberStatus::EOS_LMS_PROMOTED:
			// No member data needed, only lobby snapshot.
			break;

		case EOS_ELobbyMemberStatus::EOS_LMS_LEFT:
			Params.LeavingMembers.Add(Data->TargetUserId, ELobbyMemberLeaveReason::Left);
			break;

		case EOS_ELobbyMemberStatus::EOS_LMS_DISCONNECTED:
			Params.LeavingMembers.Add(Data->TargetUserId, ELobbyMemberLeaveReason::Disconnected);
			break;

		case EOS_ELobbyMemberStatus::EOS_LMS_KICKED:
			Params.LeavingMembers.Add(Data->TargetUserId, ELobbyMemberLeaveReason::Kicked);
			break;

		default: checkNoEntry(); // Intentional fallthrough
		case EOS_ELobbyMemberStatus::EOS_LMS_CLOSED:
			Params.LeavingMembers.Add(Data->TargetUserId, ELobbyMemberLeaveReason::Closed);
			break;
		}

		ProcessLobbyNotificationImplOp(MoveTemp(Params))
		.OnComplete([LobbyData](const TOnlineResult<FProcessLobbyNotificationImpl>& Result)
		{
			if (Result.IsError())
			{
				// Todo: handle failure to update lobby from snapshot.
				UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::HandleLobbyMemberUpdated] Failed to apply update. Lobby: %s, Error: %s"), *LobbyData->GetLobbyId(), *Result.GetErrorValue().GetLogString());
			}
		});
	}
}

void FLobbiesEOS::HandleLobbyInviteReceived(const EOS_Lobby_LobbyInviteReceivedCallbackInfo* Data)
{
#if 0
	EOS_STRUCT(EOS_Lobby_LobbyInviteReceivedCallbackInfo, (
		/** Context that was passed into EOS_Lobby_AddNotifyLobbyInviteReceived */
		void* ClientData;
		/** The ID of the invitation */
		const char* InviteId;
		/** The Product User ID of the local user who received the invitation */
		EOS_ProductUserId LocalUserId;
		/** The Product User ID of the user who sent the invitation */
		EOS_ProductUserId TargetUserId;
	));
#endif

	// Todo: Queue this like an operation.
	const FOnlineAccountIdHandle LocalUserId = FindAccountId(Data->LocalUserId);
	if (LocalUserId.IsValid())
	{
		FLobbyInviteDataEOS::CreateFromInviteId(LobbyPrerequisites.ToSharedRef(), LobbyDataRegistry.ToSharedRef(), LocalUserId, Data->InviteId, Data->TargetUserId)
		.Then([this](TFuture<TDefaultErrorResultInternal<TSharedRef<FLobbyInviteDataEOS>>>&& Future)
		{
			if (Future.Get().IsError())
			{
				// Todo: Log / queue a manual fetch of invitations.
				UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::HandleLobbyInviteReceived] Failed to receive invite. Error: %s"),
					*Future.Get().GetErrorValue().GetLogString());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::HandleLobbyInviteReceived] Received invite. Id: %s, Lobby: %s, Receiver: %s, Sender: %s"),
					*Future.Get().GetOkValue()->GetInviteId(),
					*Future.Get().GetOkValue()->GetLobbyData()->GetLobbyId(),
					*ToLogString(Future.Get().GetOkValue()->GetReceiver()),
					*ToLogString(Future.Get().GetOkValue()->GetSender()));
				AddActiveInvite(Future.Get().GetOkValue());
			}
		});
	}
}

void FLobbiesEOS::HandleLobbyInviteAccepted(const EOS_Lobby_LobbyInviteAcceptedCallbackInfo* Data)
{
#if 0
	EOS_STRUCT(EOS_Lobby_LobbyInviteAcceptedCallbackInfo, (
		/** Context that was passed into EOS_Lobby_AddNotifyLobbyInviteAccepted */
		void* ClientData;
		/** The invite ID */
		const char* InviteId;
		/** The Product User ID of the local user who received the invitation */
		EOS_ProductUserId LocalUserId;
		/** The Product User ID of the user who sent the invitation */
		EOS_ProductUserId TargetUserId;
		/** Lobby ID that the user has been invited to */
		const char* LobbyId;
	));
#endif

	// Todo: handle catalog of sent invitations.
}

void FLobbiesEOS::HandleJoinLobbyAccepted(const EOS_Lobby_JoinLobbyAcceptedCallbackInfo* Data)
{
#if 0
	EOS_STRUCT(EOS_Lobby_JoinLobbyAcceptedCallbackInfo, (
		/** Context that was passed into EOS_Lobby_AddNotifyJoinLobbyAccepted */
		void* ClientData;
		/** The Product User ID of the local user who is joining */
		EOS_ProductUserId LocalUserId;
		/** 
		 * The UI Event associated with this Join Game event.
		 * This should be used with EOS_Lobby_CopyLobbyDetailsHandleByUiEventId to get a handle to be used
		 * when calling EOS_Lobby_JoinLobby.
		 */
		EOS_UI_EventId UiEventId;
	));
#endif

	// Todo: handle UI events.
}

void FLobbiesEOS::RegisterHandlers()
{
	// Register for lobby updates.
	OnLobbyUpdatedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYLOBBYUPDATERECEIVED_API_LATEST,
		&EOS_Lobby_AddNotifyLobbyUpdateReceived,
		&EOS_Lobby_RemoveNotifyLobbyUpdateReceived,
		&FLobbiesEOS::HandleLobbyUpdated);

	// Register for lobby member updates.
	OnLobbyMemberUpdatedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST,
		&EOS_Lobby_AddNotifyLobbyMemberUpdateReceived,
		&EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived,
		&FLobbiesEOS::HandleLobbyMemberUpdated);

	// Register for lobby member status changed.
	OnLobbyMemberStatusReceivedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST,
		&EOS_Lobby_AddNotifyLobbyMemberStatusReceived,
		&EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived,
		&FLobbiesEOS::HandleLobbyMemberStatusReceived);

	// Register for lobby invite received.
	OnLobbyInviteReceivedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYLOBBYINVITERECEIVED_API_LATEST,
		&EOS_Lobby_AddNotifyLobbyInviteReceived,
		&EOS_Lobby_RemoveNotifyLobbyInviteReceived,
		&FLobbiesEOS::HandleLobbyInviteReceived);

	// Register for lobby invite accepted.
	OnLobbyInviteAcceptedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYLOBBYINVITERECEIVED_API_LATEST,
		&EOS_Lobby_AddNotifyLobbyInviteAccepted,
		&EOS_Lobby_RemoveNotifyLobbyInviteAccepted,
		&FLobbiesEOS::HandleLobbyInviteAccepted);

	// Register for join lobby accepted via overlay.
	OnJoinLobbyAcceptedEOSEventRegistration = EOS_RegisterComponentEventHandler(
		this,
		LobbyPrerequisites->LobbyInterfaceHandle,
		EOS_LOBBY_ADDNOTIFYJOINLOBBYACCEPTED_API_LATEST,
		&EOS_Lobby_AddNotifyJoinLobbyAccepted,
		&EOS_Lobby_RemoveNotifyJoinLobbyAccepted,
		&FLobbiesEOS::HandleJoinLobbyAccepted);
}

void FLobbiesEOS::UnregisterHandlers()
{
	OnLobbyUpdatedEOSEventRegistration = nullptr;
	OnLobbyMemberUpdatedEOSEventRegistration = nullptr;
	OnLobbyMemberStatusReceivedEOSEventRegistration = nullptr;
	OnLobbyInviteReceivedEOSEventRegistration = nullptr;
	OnLobbyInviteAcceptedEOSEventRegistration = nullptr;
	OnJoinLobbyAcceptedEOSEventRegistration = nullptr;
}

void FLobbiesEOS::AddActiveLobby(FOnlineAccountIdHandle LocalUserId, const TSharedRef<FLobbyDataEOS>& LobbyData)
{
	// Add bookkeeping for the user.
	ActiveLobbies.FindOrAdd(LocalUserId).Add(LobbyData);
}

void FLobbiesEOS::RemoveActiveLobby(FOnlineAccountIdHandle LocalUserId, const TSharedRef<FLobbyDataEOS>& LobbyData)
{
	// Remove bookkeeping for the local user.
	if (TSet<TSharedRef<FLobbyDataEOS>>* Lobbies = ActiveLobbies.Find(LocalUserId))
	{
		Lobbies->Remove(LobbyData);
	}
}

void FLobbiesEOS::AddActiveInvite(const TSharedRef<FLobbyInviteDataEOS>& Invite)
{
	TMap<FOnlineLobbyIdHandle, TSharedRef<FLobbyInviteDataEOS>>& ActiveUserInvites = ActiveInvites.FindOrAdd(Invite->GetReceiver());
	const FOnlineLobbyIdHandle LobbyId = Invite->GetLobbyData()->GetLobbyIdHandle();

	// Todo: Handle multiple invites for the same lobby.
	if (ActiveUserInvites.Find(LobbyId) == nullptr)
	{
		ActiveUserInvites.Add(LobbyId, Invite);
		LobbyEvents.OnLobbyInvitationAdded.Broadcast(
			FLobbyInvitationAdded{
				Invite->GetReceiver(),
				Invite->GetSender(),
				Invite->GetLobbyData()->GetClientLobbyData()->GetPublicDataPtr()
			});
	}
}

void FLobbiesEOS::RemoveActiveInvite(const TSharedRef<FLobbyInviteDataEOS>& Invite)
{
	ActiveInvites.FindOrAdd(Invite->GetReceiver()).Remove(Invite->GetLobbyData()->GetLobbyIdHandle());

	LobbyEvents.OnLobbyInvitationRemoved.Broadcast(
		FLobbyInvitationRemoved{
			Invite->GetReceiver(),
			Invite->GetSender(),
			Invite->GetLobbyData()->GetClientLobbyData()->GetPublicDataPtr()
		});
}

TSharedPtr<FLobbyInviteDataEOS> FLobbiesEOS::GetActiveInvite(FOnlineAccountIdHandle TargetUser, FOnlineLobbyIdHandle TargetLobbyId)
{
	const TSharedRef<FLobbyInviteDataEOS>* Result = ActiveInvites.FindOrAdd(TargetUser).Find(TargetLobbyId);
	return Result ? *Result : TSharedPtr<FLobbyInviteDataEOS>();
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FJoinLobbyImpl>> FLobbiesEOS::JoinLobbyImpl(FJoinLobbyImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FJoinLobbyImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FJoinLobbyImpl>>(Errors::InvalidParams()).GetFuture();
	}

	// Check whether any of the local users is already in the target lobby.
	for (const FJoinLobbyLocalUserData& JoinData : Params.LocalUsers)
	{
		if (const TSet<TSharedRef<FLobbyDataEOS>>* UserActiveLobbies = ActiveLobbies.Find(JoinData.LocalUserId))
		{
			if (UserActiveLobbies->Find(Params.LobbyData.ToSharedRef()))
			{
				return MakeFulfilledPromise<TDefaultErrorResult<FJoinLobbyImpl>>(Errors::InvalidParams()).GetFuture();
			}
		}
	}

	TArray<TFuture<TDefaultErrorResult<FJoinLobbyMemberImpl>>> PendingMemberJoins;
	for (FJoinLobbyLocalUserData& UserData : Params.LocalUsers)
	{
		FJoinLobbyMemberImpl::Params JoinLobbyMemberParams;
		JoinLobbyMemberParams.LobbyData = Params.LobbyData.ToSharedRef();
		JoinLobbyMemberParams.LocalUserId = UserData.LocalUserId;
		JoinLobbyMemberParams.Attributes = MoveTemp(UserData.Attributes);

		TSharedRef<TPromise<TDefaultErrorResult<FJoinLobbyMemberImpl>>> JoinMemberPromise = MakeShared<TPromise<TDefaultErrorResult<FJoinLobbyMemberImpl>>>();
		JoinLobbyMemberImplOp(MoveTemp(JoinLobbyMemberParams))
		.OnComplete([JoinMemberPromise](const TOnlineResult<FJoinLobbyMemberImpl>& Result)
		{
			if (Result.IsOk())
			{
				JoinMemberPromise->EmplaceValue(Result.GetOkValue());
			}
			else
			{
				JoinMemberPromise->EmplaceValue(Result.GetErrorValue());
			}
		});

		PendingMemberJoins.Emplace(JoinMemberPromise->GetFuture());
	}

	TPromise<TDefaultErrorResult<FJoinLobbyImpl>> Promise;
	TFuture<TDefaultErrorResult<FJoinLobbyImpl>> Future = Promise.GetFuture();

	WhenAll(MoveTemp(PendingMemberJoins))
	.Then([this, Promise = MoveTemp(Promise), Params = MoveTemp(Params)](TFuture<TArray<TDefaultErrorResult<FJoinLobbyMemberImpl>>>&& Results) mutable
	{
		TOptional<FOnlineError> StoredError;
		for (TDefaultErrorResult<FJoinLobbyMemberImpl>& Result : Results.Get())
		{
			if (Result.IsError())
			{
				// Store first encountered error to return as result.
				StoredError = Result.GetErrorValue();
			}
		}

		if (StoredError)
		{
			TArray<TFuture<TDefaultErrorResult<FLeaveLobbyImpl>>> PendingMemberExits;
			for (int32 MemberIndex = 0; MemberIndex < Params.LocalUsers.Num(); ++MemberIndex)
			{
				const FOnlineAccountIdHandle MemberId = Params.LocalUsers[MemberIndex].LocalUserId;

				if (Results.Get()[MemberIndex].IsError())
				{
					FLeaveLobbyImpl::Params LeaveLobbyParams;
					LeaveLobbyParams.LobbyData = Params.LobbyData.ToSharedRef();
					LeaveLobbyParams.LocalUserId = MemberId;

					TPromise<TDefaultErrorResult<FLeaveLobbyImpl>> LeaveMemberPromise;
					PendingMemberExits.Emplace(LeaveMemberPromise.GetFuture());

					LeaveLobbyImpl(MoveTemp(LeaveLobbyParams))
					.Then([LeaveMemberPromise = MoveTemp(LeaveMemberPromise)](TFuture<TDefaultErrorResult<FLeaveLobbyImpl>>&& Future) mutable
					{
						if (Future.Get().IsError())
						{
							// Todo: complain about having an error while handling an error.
						}

						LeaveMemberPromise.EmplaceValue(MoveTempIfPossible(Future.Get()));
					});
				}
			}

			WhenAll(MoveTemp(PendingMemberExits))
			.Then([Promise = MoveTemp(Promise), StoredError = MoveTemp(*StoredError)](TFuture<TArray<TDefaultErrorResult<FLeaveLobbyImpl>>>&& Future) mutable
			{
				Promise.EmplaceValue(MoveTemp(StoredError));
			});
		}
		else
		{
			Promise.EmplaceValue(FJoinLobbyImpl::Result{});
		}
	});

	return Future;
}

TOnlineAsyncOpHandle<FLobbiesEOS::FJoinLobbyMemberImpl> FLobbiesEOS::JoinLobbyMemberImplOp(FJoinLobbyMemberImpl::Params&& InParams)
{
	TOnlineAsyncOpRef<FJoinLobbyMemberImpl> Op = GetOp<FJoinLobbyMemberImpl>(MoveTemp(InParams));
	const FJoinLobbyMemberImpl::Params& Params = Op->GetParams();

	// Setup lobby details - Prefer UI event before invitation before search result.
	TSharedPtr<FLobbyDetailsEOS> LobbyDetails = Params.LobbyData->GetUserLobbyDetails(Params.LocalUserId);
	if (!LobbyDetails)
	{
		// Todo: Check whether another local member can invite the user.
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}

	if (LobbyDetails->GetInfo()->GetProductVersion() != LobbyPrerequisites->BucketId.GetProductVersion())
	{
		Op->SetError(Errors::IncompatibleVersion());
		return Op->GetHandle();
	}

	Op->Then([this, LobbyDetails, LobbyData = Params.LobbyData](TOnlineAsyncOp<FJoinLobbyMemberImpl>& InAsyncOp)
	{
		const FLobbiesEOS::FJoinLobbyMemberImpl::Params& Params = InAsyncOp.GetParams();

		EOS_Lobby_JoinLobbyOptions JoinLobbyOptions = {};
		JoinLobbyOptions.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
		JoinLobbyOptions.LobbyDetailsHandle = LobbyDetails->GetEOSHandle();
		JoinLobbyOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
		JoinLobbyOptions.bPresenceEnabled = false;
		JoinLobbyOptions.LocalRTCOptions = nullptr;
		return EOS_Async<EOS_Lobby_JoinLobbyCallbackInfo>(EOS_Lobby_JoinLobby, LobbyPrerequisites->LobbyInterfaceHandle, JoinLobbyOptions);
	})
	.Then([this](TOnlineAsyncOp<FJoinLobbyMemberImpl>& InAsyncOp, const EOS_Lobby_JoinLobbyCallbackInfo* Data)
	{
		if (Data->ResultCode != EOS_EResult::EOS_Success)
		{
			// TODO: Error codes
			InAsyncOp.SetError(Errors::Unknown(FromEOSError(Data->ResultCode)));
		}
	})
	.Then([this](TOnlineAsyncOp<FJoinLobbyMemberImpl>& InAsyncOp)
	{
		const FLobbiesEOS::FJoinLobbyMemberImpl::Params& Params = InAsyncOp.GetParams();
		FModifyLobbyMemberDataImpl::Params ModifyParams;
		ModifyParams.LobbyData = Params.LobbyData;
		ModifyParams.LocalUserId = Params.LocalUserId;
		ModifyParams.Changes = MakeShared<FClientLobbyMemberDataChanges>();
		ModifyParams.Changes->MutatedAttributes = Params.Attributes;
		return ModifyLobbyMemberDataImpl(MoveTemp(ModifyParams));
	})
	.Then([this](TOnlineAsyncOp<FJoinLobbyMemberImpl>& InAsyncOp, TDefaultErrorResult<FModifyLobbyMemberDataImpl>&& Result)
	{
		const FLobbiesEOS::FJoinLobbyMemberImpl::Params& Params = InAsyncOp.GetParams();

		if (Result.IsError())
		{
			// Failed to set attributes - leave the lobby.
			FLeaveLobbyImpl::Params LeaveLobbyParams;
			LeaveLobbyParams.LobbyData = Params.LobbyData;
			LeaveLobbyParams.LocalUserId = Params.LocalUserId;

			TPromise<void> Promise;
			TFuture<void> Future = Promise.GetFuture();

			LeaveLobbyImpl(MoveTemp(LeaveLobbyParams))
			.Then([this, InAsyncOp = InAsyncOp.AsShared(), ErrorResult = MoveTemp(Result.GetErrorValue()), Promise = MoveTemp(Promise)](TFuture<TDefaultErrorResult<FLeaveLobbyImpl>>&& Future) mutable
			{
				if (Future.Get().IsError())
				{
					// Todo: complain about having an error while handling an error.
				}

				// Todo: Errors.
				InAsyncOp->SetError(Errors::Unknown(MoveTemp(ErrorResult)));
				Promise.EmplaceValue();
			});

			return Future;
		}
		else
		{
			return MakeFulfilledPromise<void>().GetFuture();
		}
	})
	.Then([this](TOnlineAsyncOp<FJoinLobbyMemberImpl>& InAsyncOp)
	{
		InAsyncOp.SetResult(FJoinLobbyMemberImpl::Result{});
	})
	.Enqueue(GetSerialQueue(InParams.LocalUserId));

	return Op->GetHandle();
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FLeaveLobbyImpl>> FLobbiesEOS::LeaveLobbyImpl(FLeaveLobbyImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FLeaveLobbyImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FLeaveLobbyImpl>>(Errors::InvalidParams()).GetFuture();
	}

	EOS_Lobby_LeaveLobbyOptions LeaveLobbyOptions = {};
	LeaveLobbyOptions.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
	LeaveLobbyOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	LeaveLobbyOptions.LobbyId = Params.LobbyData->GetLobbyIdEOS();

	TPromise<TDefaultErrorResult<FLeaveLobbyImpl>> Promise;
	TFuture<TDefaultErrorResult<FLeaveLobbyImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_LeaveLobbyCallbackInfo>(EOS_Lobby_LeaveLobby, LobbyPrerequisites->LobbyInterfaceHandle, LeaveLobbyOptions)
	.Then([Promise = MoveTemp(Promise)](TFuture<const EOS_Lobby_LeaveLobbyCallbackInfo*>&& Future) mutable
	{
		const EOS_Lobby_LeaveLobbyCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FLeaveLobbyImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FDestroyLobbyImpl>> FLobbiesEOS::DestroyLobbyImpl(FDestroyLobbyImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FDestroyLobbyImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FDestroyLobbyImpl>>(Errors::InvalidParams()).GetFuture();
	}

	EOS_Lobby_DestroyLobbyOptions DestroyLobbyOptions = {};
	DestroyLobbyOptions.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST;
	DestroyLobbyOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	DestroyLobbyOptions.LobbyId = Params.LobbyData->GetLobbyIdEOS();

	TPromise<TDefaultErrorResult<FDestroyLobbyImpl>> Promise;
	TFuture<TDefaultErrorResult<FDestroyLobbyImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_DestroyLobbyCallbackInfo>(EOS_Lobby_DestroyLobby, LobbyPrerequisites->LobbyInterfaceHandle, DestroyLobbyOptions)
	.Then([Promise = MoveTemp(Promise)](TFuture<const EOS_Lobby_DestroyLobbyCallbackInfo*>&& Future) mutable
	{
		const EOS_Lobby_DestroyLobbyCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FDestroyLobbyImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FInviteLobbyMemberImpl>> FLobbiesEOS::InviteLobbyMemberImpl(FInviteLobbyMemberImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FInviteLobbyMemberImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FInviteLobbyMemberImpl>>(Errors::InvalidParams()).GetFuture();
	}

	EOS_Lobby_SendInviteOptions SendInviteOptions = {};
	SendInviteOptions.ApiVersion = EOS_LOBBY_SENDINVITE_API_LATEST;
	SendInviteOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	SendInviteOptions.TargetUserId = GetProductUserIdChecked(Params.TargetUserId);
	SendInviteOptions.LobbyId = Params.LobbyData->GetLobbyIdEOS();

	TPromise<TDefaultErrorResult<FInviteLobbyMemberImpl>> Promise;
	TFuture<TDefaultErrorResult<FInviteLobbyMemberImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_SendInviteCallbackInfo>(EOS_Lobby_SendInvite, LobbyPrerequisites->LobbyInterfaceHandle, SendInviteOptions)
	.Then([Promise = MoveTemp(Promise)](TFuture<const EOS_Lobby_SendInviteCallbackInfo*>&& Future) mutable
	{
		const EOS_Lobby_SendInviteCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FInviteLobbyMemberImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FDeclineLobbyInvitationImpl>> FLobbiesEOS::DeclineLobbyInvitationImpl(FDeclineLobbyInvitationImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FDeclineLobbyInvitationImpl>>(Errors::InvalidUser()).GetFuture();
	}

	// Find the active invitation.
	TSharedPtr<FLobbyInviteDataEOS> InviteData = GetActiveInvite(Params.LocalUserId, Params.LobbyId);
	if (!InviteData)
	{
		// Todo: Errors.
		return MakeFulfilledPromise<TDefaultErrorResult<FDeclineLobbyInvitationImpl>>(Errors::Unknown()).GetFuture();
	}

	EOS_Lobby_RejectInviteOptions RejectInviteOptions = {};
	RejectInviteOptions.ApiVersion = EOS_LOBBY_REJECTINVITE_API_LATEST;
	RejectInviteOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	RejectInviteOptions.InviteId = InviteData->GetInviteIdEOS();

	TPromise<TDefaultErrorResult<FDeclineLobbyInvitationImpl>> Promise;
	TFuture<TDefaultErrorResult<FDeclineLobbyInvitationImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_RejectInviteCallbackInfo>(EOS_Lobby_RejectInvite, LobbyPrerequisites->LobbyInterfaceHandle, RejectInviteOptions)
	.Then([this, Promise = MoveTemp(Promise), InviteData](TFuture<const EOS_Lobby_RejectInviteCallbackInfo*>&& Future) mutable
	{
		// Remove active invitation.
		RemoveActiveInvite(InviteData.ToSharedRef());

		const EOS_Lobby_RejectInviteCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FDeclineLobbyInvitationImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FKickLobbyMemberImpl>> FLobbiesEOS::KickLobbyMemberImpl(FKickLobbyMemberImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FKickLobbyMemberImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FKickLobbyMemberImpl>>(Errors::InvalidParams()).GetFuture();
	}

	// todo: check local user is lobby owner

	EOS_Lobby_KickMemberOptions KickMemberOptions = {};
	KickMemberOptions.ApiVersion = EOS_LOBBY_KICKMEMBER_API_LATEST;
	KickMemberOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	KickMemberOptions.TargetUserId = GetProductUserIdChecked(Params.TargetUserId);
	KickMemberOptions.LobbyId = Params.LobbyData->GetLobbyIdEOS();

	TPromise<TDefaultErrorResult<FKickLobbyMemberImpl>> Promise;
	TFuture<TDefaultErrorResult<FKickLobbyMemberImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_KickMemberCallbackInfo>(EOS_Lobby_KickMember, LobbyPrerequisites->LobbyInterfaceHandle, KickMemberOptions)
	.Then([Promise = MoveTemp(Promise)](TFuture<const EOS_Lobby_KickMemberCallbackInfo*>&& Future) mutable
	{
		const EOS_Lobby_KickMemberCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FKickLobbyMemberImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FPromoteLobbyMemberImpl>> FLobbiesEOS::PromoteLobbyMemberImpl(FPromoteLobbyMemberImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FPromoteLobbyMemberImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FPromoteLobbyMemberImpl>>(Errors::InvalidParams()).GetFuture();
	}

	// todo: check local user is lobby owner

	EOS_Lobby_PromoteMemberOptions PromoteMemberOptions = {};
	PromoteMemberOptions.ApiVersion = EOS_LOBBY_PROMOTEMEMBER_API_LATEST;
	PromoteMemberOptions.LocalUserId = GetProductUserIdChecked(Params.LocalUserId);
	PromoteMemberOptions.TargetUserId = GetProductUserIdChecked(Params.TargetUserId);
	PromoteMemberOptions.LobbyId = Params.LobbyData->GetLobbyIdEOS();

	TPromise<TDefaultErrorResult<FPromoteLobbyMemberImpl>> Promise;
	TFuture<TDefaultErrorResult<FPromoteLobbyMemberImpl>> Future = Promise.GetFuture();

	EOS_Async<EOS_Lobby_PromoteMemberCallbackInfo>(EOS_Lobby_PromoteMember, LobbyPrerequisites->LobbyInterfaceHandle, PromoteMemberOptions)
	.Then([Promise = MoveTemp(Promise)](TFuture<const EOS_Lobby_PromoteMemberCallbackInfo*>&& Future) mutable
	{
		const EOS_Lobby_PromoteMemberCallbackInfo* Result = Future.Get();
		if (Result->ResultCode != EOS_EResult::EOS_Success)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Result->ResultCode)));
			return;
		}

		Promise.EmplaceValue(FPromoteLobbyMemberImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FModifyLobbyDataImpl>> FLobbiesEOS::ModifyLobbyDataImpl(FModifyLobbyDataImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyDataImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyDataImpl>>(Errors::InvalidParams()).GetFuture();
	}

	TSharedPtr<FLobbyDetailsEOS> LobbyDetails = Params.LobbyData->GetUserLobbyDetails(Params.LocalUserId);
	if (!LobbyDetails)
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyDataImpl>>(Errors::InvalidParams()).GetFuture();
	}

	if (Params.LocalUserId != Params.LobbyData->GetClientLobbyData()->GetPublicData().OwnerAccountId)
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyDataImpl>>(Errors::InvalidParams()).GetFuture();
	}

	UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::ModifyLobbyDataImpl] Start. Lobby: %s, Member: %s"),
		*Params.LobbyData->GetLobbyId(), *ToLogString(Params.LocalUserId));

	TPromise<TDefaultErrorResult<FModifyLobbyDataImpl>> Promise;
	TFuture<TDefaultErrorResult<FModifyLobbyDataImpl>> Future = Promise.GetFuture();

	LobbyDetails->ApplyLobbyDataUpdateFromLocalChanges(Params.LocalUserId, *Params.Changes)
	.Then([Promise = MoveTemp(Promise)](TFuture<EOS_EResult>&& Future) mutable
	{
		UE_LOG(LogTemp, Warning, TEXT("[FLobbiesEOS::ModifyLobbyDataImpl] Complete. Result: %s"), *LexToString(Future.Get()));

		// Todo: Handle "no change" better.
		if (Future.Get() != EOS_EResult::EOS_Success && Future.Get() != EOS_EResult::EOS_NoChange)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Future.Get())));
			return;
		}

		Promise.EmplaceValue(FModifyLobbyDataImpl::Result{});
	});

	return Future;
}

TFuture<TDefaultErrorResult<FLobbiesEOS::FModifyLobbyMemberDataImpl>> FLobbiesEOS::ModifyLobbyMemberDataImpl(FModifyLobbyMemberDataImpl::Params&& Params)
{
	// Check prerequisites.
	if (!Services.Get<FAuthEOS>()->IsLoggedIn(Params.LocalUserId))
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyMemberDataImpl>>(Errors::InvalidUser()).GetFuture();
	}

	if (!Params.LobbyData.IsValid())
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyMemberDataImpl>>(Errors::InvalidParams()).GetFuture();
	}

	TSharedPtr<FLobbyDetailsEOS> LobbyDetails = Params.LobbyData->GetUserLobbyDetails(Params.LocalUserId);
	if (!LobbyDetails)
	{
		return MakeFulfilledPromise<TDefaultErrorResult<FModifyLobbyMemberDataImpl>>(Errors::InvalidParams()).GetFuture();
	}

	TPromise<TDefaultErrorResult<FModifyLobbyMemberDataImpl>> Promise;
	TFuture<TDefaultErrorResult<FModifyLobbyMemberDataImpl>> Future = Promise.GetFuture();

	LobbyDetails->ApplyLobbyMemberDataUpdateFromLocalChanges(Params.LocalUserId, *Params.Changes)
	.Then([Promise = MoveTemp(Promise)](TFuture<EOS_EResult>&& Future) mutable
	{
		// Todo: Handle "no change" better.
		if (Future.Get() != EOS_EResult::EOS_Success && Future.Get() != EOS_EResult::EOS_NoChange)
		{
			// Todo: Errors
			Promise.EmplaceValue(Errors::Unknown(FromEOSError(Future.Get())));
			return;
		}

		Promise.EmplaceValue(FModifyLobbyMemberDataImpl::Result{});
	});

	return Future;
}

TOnlineAsyncOpHandle<FLobbiesEOS::FProcessLobbyNotificationImpl> FLobbiesEOS::ProcessLobbyNotificationImplOp(FProcessLobbyNotificationImpl::Params&& InParams)
{
	TOnlineAsyncOpRef<FProcessLobbyNotificationImpl> Op = GetOp<FProcessLobbyNotificationImpl>(MoveTemp(InParams));
	const FProcessLobbyNotificationImpl::Params& Params = Op->GetParams();

	if (!Params.LobbyData.IsValid())
	{
		Op->SetError(Errors::InvalidParams());
		return Op->GetHandle();
	}

	Op->Then([this](TOnlineAsyncOp<FProcessLobbyNotificationImpl>& InAsyncOp)
	{
		const FProcessLobbyNotificationImpl::Params& Params = InAsyncOp.GetParams();

		// Notifications do not always indicate a user. Try to find a valid lobby details object to
		// handle acquiring data snapshots.
		TSharedPtr<FLobbyDetailsEOS> LobbyDetails = Params.LobbyData->GetActiveLobbyDetails();
		if (!LobbyDetails.IsValid())
		{
			UE_LOG(LogTemp, Log, TEXT("[FLobbiesEOS::ProcessLobbyNotificationImplOp] Failed to find active lobby details to process lobby notificaions: Lobby: %s"),
				*Params.LobbyData->GetLobbyId());
			InAsyncOp.SetError(Errors::Unknown());
			return MakeFulfilledPromise<TDefaultErrorResultInternal<TSharedRef<FClientLobbySnapshot>>>().GetFuture();
		}

		InAsyncOp.Data.Set<TSharedRef<FLobbyDetailsEOS>>(LobbyDetailsKeyName, LobbyDetails.ToSharedRef());

		// Fetch lobby snapshot. Fetching the snapshot resolves the account ids of all lobby members in the snapshot.
		return LobbyDetails->GetLobbySnapshot();
	})
	.Then([this](TOnlineAsyncOp<FProcessLobbyNotificationImpl>& InAsyncOp, TDefaultErrorResultInternal<TSharedRef<FClientLobbySnapshot>>&& LobbySnapshotResult)
	{
		const FProcessLobbyNotificationImpl::Params& Params = InAsyncOp.GetParams();
		TSharedRef<FLobbyDetailsEOS> LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsEOS>>(InAsyncOp, LobbyDetailsKeyName);

		if (LobbySnapshotResult.IsError())
		{
			// Todo: errors.
			InAsyncOp.SetError(Errors::Unknown(MoveTempIfPossible(LobbySnapshotResult.GetErrorValue())));
			return;
		}

		// Get member snapshots.
		TMap<FOnlineAccountIdHandle, TSharedRef<FClientLobbyMemberSnapshot>> LobbyMemberSnapshots;
		LobbyMemberSnapshots.Reserve(Params.MutatedMembers.Num());
		for (EOS_ProductUserId MutatedMember : Params.MutatedMembers)
		{
			const FOnlineAccountIdHandle MutatedMemberAccountId = FindAccountId(MutatedMember);
			if (MutatedMemberAccountId.IsValid())
			{
				TDefaultErrorResultInternal<TSharedRef<FClientLobbyMemberSnapshot>> MemberSnapshotResult = LobbyDetails->GetLobbyMemberSnapshot(MutatedMemberAccountId);
				if (MemberSnapshotResult.IsError())
				{
					// Todo: errors.
					InAsyncOp.SetError(Errors::Unknown(MoveTempIfPossible(MemberSnapshotResult.GetErrorValue())));
					return;
				}
				LobbyMemberSnapshots.Add(MutatedMemberAccountId, MoveTemp(MemberSnapshotResult.GetOkValue()));
			}
		}

		// Translate leaving members from EOS_ProductUserId to FOnlineAccountIdHandle.
		TMap<FOnlineAccountIdHandle, ELobbyMemberLeaveReason> LeavingMemberReason;
		LeavingMemberReason.Reserve(Params.LeavingMembers.Num());
		for (const TPair<EOS_ProductUserId, ELobbyMemberLeaveReason>& LeavingMember : Params.LeavingMembers)
		{
			const FOnlineAccountIdHandle LeavingMemberAccountId = FindAccountId(LeavingMember.Key);
			if (LeavingMemberAccountId.IsValid())
			{
				LeavingMemberReason.Add(LeavingMemberAccountId, LeavingMember.Value);
			}
		}

		// Apply updates and fire notifications.
		FApplyLobbyUpdateResult Result = Params.LobbyData->GetClientLobbyData()->ApplyLobbyUpdateFromServiceSnapshot(
			MoveTemp(*LobbySnapshotResult.GetOkValue()),
			MoveTemp(LobbyMemberSnapshots),
			MoveTemp(LeavingMemberReason),
			&LobbyEvents);

		// Remove active users if needed.
		for (FOnlineAccountIdHandle LeavingMember : Result.LeavingLocalMembers)
		{
			RemoveActiveLobby(LeavingMember, Params.LobbyData.ToSharedRef());
		}

		InAsyncOp.SetResult(FProcessLobbyNotificationImpl::Result{});
	})
	.Enqueue(GetSerialQueue());

	return Op->GetHandle();
}

/* UE::Online */ }
