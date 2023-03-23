// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebSocketMessageHandler.h"

#include "Algo/ForEach.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "IRemoteControlModule.h"
#include "GameFramework/Actor.h"
#include "RemoteControlModels.h"
#include "RemoteControlPreset.h"
#include "RemoteControlRequest.h"
#include "RemoteControlResponse.h"
#include "RemoteControlRoute.h"
#include "RemoteControlReflectionUtils.h"
#include "WebRemoteControl.h"
#include "WebRemoteControlUtils.h"

static TAutoConsoleVariable<int32> CVarWebRemoteControlFramesBetweenPropertyNotifications(TEXT("WebControl.FramesBetweenPropertyNotifications"), 5, TEXT("The number of frames between sending batches of property notifications."));

namespace WebSocketMessageHandlerStructUtils
{
	using namespace UE::WebRCReflectionUtils;

	FName Struct_PropertyValue = "WEBRC_PropertyValue";
	FName Prop_PropertyLabel = "PropertyLabel";
	FName Prop_Id = "Id";
	FName Prop_ObjectPath = "ObjectPath";
	FName Prop_PropertyValue = "PropertyValue";

	FName Struct_PresetFieldsChanged = "WEBRC_PresetFieldsChanged";
	FName Prop_Type = "Type";
	FName Prop_PresetName= "PresetName";
	FName Prop_PresetId = "PresetId";
	FName Prop_ChangedFields = "ChangedFields";

	FName Struct_ActorPropertyValue= "WEBRC_ActorPropertyValue";
	FName Prop_PropertyName = "PropertyName";

	FName Struct_ModifiedActor = "WEBRC_ModifiedActor";
	FName Prop_DisplayName = "DisplayName";
	FName Prop_Path = "Path";
	FName Prop_ModifiedProperties = "ModifiedProperties";

	FName Struct_ModifiedActors = "WEBRC_ModifiedActors";
	FName Prop_ModifiedActors = "ModifiedActors";
	
	UScriptStruct* CreatePropertyValueContainer(FProperty* InValueProperty)
	{
		check(InValueProperty);

		static FGuid PropertyValueGuid = FGuid::NewGuid();

		FWebRCGenerateStructArgs Args;

		Args.StringProperties = 
		{ 
			Prop_PropertyLabel,
			Prop_Id,
			Prop_ObjectPath
		};

		Args.GenericProperties.Emplace(Prop_PropertyValue, InValueProperty);

		const FString StructName = FString::Format(TEXT("{0}_{1}_{2}_{3}"), { *Struct_PropertyValue.ToString(), *InValueProperty->GetClass()->GetName(), *InValueProperty->GetName(), PropertyValueGuid.ToString() });

		return GenerateStruct(*StructName, Args);
	}

	UScriptStruct* CreatePresetFieldsChangedStruct(UScriptStruct* PropertyValueStruct)
	{
		FWebRCGenerateStructArgs Args;
		Args.StringProperties =
		{ 
			Prop_PresetId,
			Prop_PresetName,
			Prop_Type
		};

		Args.ArrayProperties.Emplace(Prop_ChangedFields, PropertyValueStruct);
		const FString StructName = FString::Format(TEXT("{0}_{1}"), { *Struct_PresetFieldsChanged.ToString(), *PropertyValueStruct->GetName() });

		return GenerateStruct(*StructName, Args);
	}

	UScriptStruct* CreateActorPropertyValueContainer(FProperty* InValueProperty)
	{
		static FGuid ActorPropertyValueGuid = FGuid::NewGuid();

		FWebRCGenerateStructArgs Args;
		Args.StringProperties =
		{
			Prop_PropertyName
		};

		Args.GenericProperties.Emplace(Prop_PropertyValue, InValueProperty);

		const FString StructName = FString::Format(TEXT("{0}_{1}_{2}_{3}"), { *Struct_PropertyValue.ToString(), *InValueProperty->GetClass()->GetName(), *InValueProperty->GetName(), ActorPropertyValueGuid.ToString() });

		return GenerateStruct(*StructName, Args);
	}

	UScriptStruct* CreateModifiedActorStruct(UScriptStruct* ModifiedPropertiesStruct)
	{
		FWebRCGenerateStructArgs Args;
		Args.StringProperties =
		{
			Prop_Id,
			Prop_DisplayName,
			Prop_Path
		};

		Args.ArrayProperties.Emplace(Prop_ModifiedProperties, ModifiedPropertiesStruct);

		const FString StructName = FString::Format(TEXT("{0}_{1}"), { *Struct_ModifiedActor.ToString(), *ModifiedPropertiesStruct->GetName() });
		return GenerateStruct(*StructName, Args);
	}

	UScriptStruct* CreateModifiedActorsStruct(UScriptStruct* ModifiedActorStruct)
	{
		FWebRCGenerateStructArgs Args;
		Args.StringProperties =
		{
			Prop_Type,
			Prop_PresetName,
			Prop_PresetId
		};

		Args.ArrayProperties.Emplace(Prop_ModifiedActors, ModifiedActorStruct);

		const FString StructName = FString::Format(TEXT("{0}_{1}"), { *Struct_ModifiedActors.ToString(), *ModifiedActorStruct->GetName() });
		return GenerateStruct(*StructName, Args);
	}

	FStructOnScope CreatePropertyValueOnScope(const TSharedPtr<FRemoteControlProperty>& RCProperty, const FRCObjectReference& ObjectReference)
	{
		UScriptStruct* Struct = CreatePropertyValueContainer(ObjectReference.Property.Get());
		FStructOnScope StructOnScope{ Struct };

		SetStringPropertyValue(Prop_PropertyLabel, StructOnScope, RCProperty->GetLabel().ToString());
		SetStringPropertyValue(Prop_Id, StructOnScope, RCProperty->GetId().ToString());
		SetStringPropertyValue(Prop_ObjectPath, StructOnScope, ObjectReference.Object->GetPathName());
		CopyPropertyValue(Prop_PropertyValue, StructOnScope, ObjectReference);

		return StructOnScope;
	}

	FStructOnScope CreatePresetFieldsChangedStructOnScope(const URemoteControlPreset* Preset, const TArray<FStructOnScope>& PropertyValuesOnScope)
	{
		UScriptStruct* PropertyValueStruct = (UScriptStruct*)PropertyValuesOnScope[0].GetStruct();
		check(PropertyValueStruct);

		UScriptStruct* TopLevelStruct = CreatePresetFieldsChangedStruct(PropertyValueStruct);

		FStructOnScope FieldsChangedOnScope{ TopLevelStruct };
		SetStringPropertyValue(Prop_Type, FieldsChangedOnScope, TEXT("PresetFieldsChanged"));
		SetStringPropertyValue(Prop_PresetName, FieldsChangedOnScope, *Preset->GetFName().ToString());
		SetStringPropertyValue(Prop_PresetId, FieldsChangedOnScope, *Preset->GetPresetId().ToString());
		SetStructArrayPropertyValue(Prop_ChangedFields, FieldsChangedOnScope, PropertyValuesOnScope);

		return FieldsChangedOnScope;
	}

	FStructOnScope CreateActorPropertyValueOnScope(const URemoteControlPreset* Preset, const FRCObjectReference& ObjectReference)
	{
		UScriptStruct* Struct = CreateActorPropertyValueContainer(ObjectReference.Property.Get());
		FStructOnScope StructOnScope{ Struct };

		SetStringPropertyValue(Prop_PropertyName, StructOnScope, *ObjectReference.Property->GetName());
		CopyPropertyValue(Prop_PropertyValue, StructOnScope, ObjectReference);

		return StructOnScope;
	}

	FStructOnScope CreateModifiedActorStructOnScope(const URemoteControlPreset* Preset, const FRemoteControlActor& RCActor, const TArray<FStructOnScope>& ModifiedPropertiesOnScope)
	{
		check(ModifiedPropertiesOnScope.Num() > 0);
		UScriptStruct* ModifiedPropertiesStruct = (UScriptStruct*)ModifiedPropertiesOnScope[0].GetStruct();
		check(ModifiedPropertiesStruct);

		UScriptStruct* TopLevelStruct = CreateModifiedActorStruct(ModifiedPropertiesStruct);
		FStructOnScope FieldsChangedOnScope{ TopLevelStruct };

		SetStringPropertyValue(Prop_Id, FieldsChangedOnScope, *RCActor.GetId().ToString());
		SetStringPropertyValue(Prop_DisplayName, FieldsChangedOnScope, *RCActor.GetLabel().ToString());
		SetStringPropertyValue(Prop_Path, FieldsChangedOnScope, *RCActor.Path.ToString());
		SetStructArrayPropertyValue(Prop_ModifiedProperties, FieldsChangedOnScope, ModifiedPropertiesOnScope);

		return FieldsChangedOnScope;
	}

	FStructOnScope CreateModifiedActorsStructOnScope(const URemoteControlPreset* Preset, const TArray<FStructOnScope>& ModifiedActorsOnScope)
	{
		check(ModifiedActorsOnScope.Num() > 0);
		UScriptStruct* ModifiedActorStruct = (UScriptStruct*)ModifiedActorsOnScope[0].GetStruct();
		check(ModifiedActorStruct);

		UScriptStruct* TopLevelStruct = CreateModifiedActorsStruct(ModifiedActorStruct);
		FStructOnScope FieldsChangedOnScope{ TopLevelStruct };

		SetStringPropertyValue(Prop_Type, FieldsChangedOnScope, TEXT("PresetActorModified"));
		SetStringPropertyValue(Prop_PresetName, FieldsChangedOnScope, *Preset->GetFName().ToString());
		SetStringPropertyValue(Prop_PresetId, FieldsChangedOnScope, *Preset->GetPresetId().ToString());
		SetStructArrayPropertyValue(Prop_ModifiedActors, FieldsChangedOnScope, ModifiedActorsOnScope);

		return FieldsChangedOnScope;
	}
}

FWebSocketMessageHandler::FWebSocketMessageHandler(FRCWebSocketServer* InServer, const FGuid& InActingClientId)
	: Server(InServer)
	, ActingClientId(InActingClientId)
{
	check(Server);
}

void FWebSocketMessageHandler::RegisterRoutes(FWebRemoteControlModule* WebRemoteControl)
{
	FCoreDelegates::OnEndFrame.AddRaw(this, &FWebSocketMessageHandler::OnEndFrame);
	Server->OnConnectionClosed().AddRaw(this, &FWebSocketMessageHandler::OnConnectionClosedCallback);
	
	// WebSocket routes
	TUniquePtr<FRemoteControlWebsocketRoute> RegisterRoute = MakeUnique<FRemoteControlWebsocketRoute>(
		TEXT("Route a message for custom websocket route"),
		TEXT("preset.register"),
		FWebSocketMessageDelegate::CreateRaw(this, &FWebSocketMessageHandler::HandleWebSocketPresetRegister)
		);

	WebRemoteControl->RegisterWebsocketRoute(*RegisterRoute);
	Routes.Emplace(MoveTemp(RegisterRoute));

	TUniquePtr<FRemoteControlWebsocketRoute> UnregisterRoute = MakeUnique<FRemoteControlWebsocketRoute>(
		TEXT("Route a message for custom websocket route"),
		TEXT("preset.unregister"),
		FWebSocketMessageDelegate::CreateRaw(this, &FWebSocketMessageHandler::HandleWebSocketPresetUnregister)
		);

	WebRemoteControl->RegisterWebsocketRoute(*UnregisterRoute);
	Routes.Emplace(MoveTemp(UnregisterRoute));
}

void FWebSocketMessageHandler::UnregisterRoutes(FWebRemoteControlModule* WebRemoteControl)
{
	Server->OnConnectionClosed().RemoveAll(this);
	FCoreDelegates::OnEndFrame.RemoveAll(this);

	for (const TUniquePtr<FRemoteControlWebsocketRoute>& Route : Routes)
	{
		WebRemoteControl->UnregisterWebsocketRoute(*Route);
	}
}

void FWebSocketMessageHandler::NotifyPropertyChangedRemotely(const FGuid& OriginClientId, const FGuid& PresetId, const FGuid& ExposedPropertyId)
{
	if (TArray<FGuid>* SubscribedClients = WebSocketNotificationMap.Find(PresetId))
	{
		if (SubscribedClients->Contains(OriginClientId))
		{
			bool bIgnoreIncomingNotification = false;

			if (FRCClientConfig* Config = ClientConfigMap.Find(OriginClientId))
			{
				bIgnoreIncomingNotification = Config->bIgnoreRemoteChanges;
			}

			if (!bIgnoreIncomingNotification)
			{
				PerFrameModifiedProperties.FindOrAdd(PresetId).FindOrAdd(OriginClientId).Add(ExposedPropertyId);
			}
			else
			{
				for (TPair<FGuid, TSet<FGuid>>& Entry : PerFrameModifiedProperties.FindOrAdd(PresetId))
				{
					if (Entry.Key != OriginClientId)
					{
						Entry.Value.Add(ExposedPropertyId);
					}
				}
			}

			PropertiesManuallyNotifiedThisFrame.Add(ExposedPropertyId);
		}
	}
}

void FWebSocketMessageHandler::HandleWebSocketPresetRegister(const FRemoteControlWebSocketMessage& WebSocketMessage)
{
	FRCWebSocketPresetRegisterBody Body;
	if (!WebRemoteControlUtils::DeserializeRequestPayload(WebSocketMessage.RequestPayload, nullptr, Body))
	{
		return;
	}

	URemoteControlPreset* Preset = nullptr;

	FGuid PresetId;

	if (FGuid::ParseExact(Body.PresetName, EGuidFormats::Digits, PresetId))
	{
		
		Preset = IRemoteControlModule::Get().ResolvePreset(PresetId);
	}
	else
	{
		Preset = IRemoteControlModule::Get().ResolvePreset(*Body.PresetName);
	}


	if (Preset == nullptr)
	{
		return;
	}

	ClientConfigMap.FindOrAdd(WebSocketMessage.ClientId).bIgnoreRemoteChanges = Body.IgnoreRemoteChanges;
	
	TArray<FGuid>* ClientIds = WebSocketNotificationMap.Find(Preset->GetPresetId());

	// Don't register delegates for a preset more than once.
	if (!ClientIds)
	{
		ClientIds = &WebSocketNotificationMap.Add(Preset->GetPresetId());

		//Register to any useful callback for the given preset
		Preset->OnExposedPropertiesModified().AddRaw(this, &FWebSocketMessageHandler::OnPresetExposedPropertiesModified);
		Preset->OnEntityExposed().AddRaw(this, &FWebSocketMessageHandler::OnPropertyExposed);
		Preset->OnEntityUnexposed().AddRaw(this, &FWebSocketMessageHandler::OnPropertyUnexposed);
		Preset->OnFieldRenamed().AddRaw(this, &FWebSocketMessageHandler::OnFieldRenamed);
		Preset->OnMetadataModified().AddRaw(this, &FWebSocketMessageHandler::OnMetadataModified);
		Preset->OnActorPropertyModified().AddRaw(this, &FWebSocketMessageHandler::OnActorPropertyChanged);
		Preset->OnEntitiesUpdated().AddRaw(this, &FWebSocketMessageHandler::OnEntitiesModified);
		Preset->OnPresetLayoutModified().AddRaw(this, &FWebSocketMessageHandler::OnLayoutModified);
	}

	ClientIds->AddUnique(WebSocketMessage.ClientId);
}


void FWebSocketMessageHandler::HandleWebSocketPresetUnregister(const FRemoteControlWebSocketMessage& WebSocketMessage)
{
	FRCWebSocketPresetRegisterBody Body;
	if (!WebRemoteControlUtils::DeserializeRequestPayload(WebSocketMessage.RequestPayload, nullptr, Body))
	{
		return;
	}

	URemoteControlPreset* Preset = nullptr;

	FGuid PresetId;

	if (FGuid::ParseExact(Body.PresetName, EGuidFormats::Digits, PresetId))
	{
		Preset = IRemoteControlModule::Get().ResolvePreset(PresetId);
	}
	else
	{
		Preset = IRemoteControlModule::Get().ResolvePreset(*Body.PresetName);
	}

	if (Preset)
	{
		if (TArray<FGuid>* RegisteredClients = WebSocketNotificationMap.Find(Preset->GetPresetId()))
		{
			RegisteredClients->Remove(WebSocketMessage.ClientId);
		}
	}
}

void FWebSocketMessageHandler::ProcessChangedProperties()
{
	//Go over each property that were changed for each preset
	for (const TPair<FGuid, TMap<FGuid, TSet<FGuid>>>& Entry : PerFrameModifiedProperties)
	{
		if (!ShouldProcessEventForPreset(Entry.Key) || !Entry.Value.Num())
		{
			continue;
		}

		URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry.Key);
		if (!Preset)
		{
			continue;
		}

		UE_LOG(LogRemoteControl, VeryVerbose, TEXT("(%s) Broadcasting properties changed event."), *Preset->GetName());

		// Each client will have a custom payload that doesnt contain the events it triggered.
		for (const TPair<FGuid, TSet<FGuid>>& ClientToEventsPair : Entry.Value)
		{
			// This should be improved in the future, we create one message per modified property to avoid
			// sending a list of non-uniform properties (ie. Color, Transform), ideally these should be grouped by underlying
			// property class. See UE-139683
			for (const FGuid& Id : ClientToEventsPair.Value)
			{
				TArray<uint8> WorkingBuffer;
				if (ClientToEventsPair.Value.Num() && WritePropertyChangeEventPayload(Preset, { Id }, WorkingBuffer))
				{
					TArray<uint8> Payload;
					WebRemoteControlUtils::ConvertToUTF8(WorkingBuffer, Payload);
					Server->Send(ClientToEventsPair.Key, Payload);
				}	
			}
		}
	}

	PerFrameModifiedProperties.Empty();
}

void FWebSocketMessageHandler::ProcessChangedActorProperties()
{
	//Go over each property that were changed for each preset
	for (const TPair<FGuid, TMap<FGuid, TMap<FRemoteControlActor, TArray<FRCObjectReference>>>>& Entry : PerFrameActorPropertyChanged)
	{
		if (!ShouldProcessEventForPreset(Entry.Key) || !Entry.Value.Num())
		{
			continue;
		}

		URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry.Key);
		if (!Preset)
		{
			continue;
		}

		// Each client will have a custom payload that doesnt contain the events it triggered.
		for (const TPair<FGuid, TMap<FRemoteControlActor, TArray<FRCObjectReference>>>& ClientToModifications : Entry.Value)
		{
			TArray<uint8> WorkingBuffer;
			FMemoryWriter Writer(WorkingBuffer);

			if (ClientToModifications.Value.Num() && WriteActorPropertyChangePayload(Preset, ClientToModifications.Value, Writer))
			{
				TArray<uint8> Payload;
				WebRemoteControlUtils::ConvertToUTF8(WorkingBuffer, Payload);
				Server->Send(ClientToModifications.Key, Payload);
			}
		}
	}

	PerFrameActorPropertyChanged.Empty();
}

void FWebSocketMessageHandler::OnPropertyExposed(URemoteControlPreset* Owner, const FGuid& EntityId)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	//Cache the property field that was removed for end of frame notification
	PerFrameAddedProperties.FindOrAdd(Owner->GetPresetId()).AddUnique(EntityId);
}

void FWebSocketMessageHandler::OnPresetExposedPropertiesModified(URemoteControlPreset* Owner, const TSet<FGuid>& ModifiedPropertyIds)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	//Cache the property field that changed for end of frame notification
	TMap<FGuid, TSet<FGuid>>& EventsForClient = PerFrameModifiedProperties.FindOrAdd(Owner->GetPresetId());
	
	if (TArray<FGuid>* SubscribedClients = WebSocketNotificationMap.Find(Owner->GetPresetId()))
	{
		for (const FGuid& ModifiedPropertyId : ModifiedPropertyIds)
		{
			// Don't send a change notification if the change was manually notified.
			// This is to avoid the case of a post edit change property being caught by the preset for a change 
			// that a client deliberatly wishes to ignore.
			if (!PropertiesManuallyNotifiedThisFrame.Contains(ModifiedPropertyId))
			{
				for (const FGuid& Client : *SubscribedClients)
				{
					if (Client != ActingClientId || !ClientConfigMap.FindChecked(Client).bIgnoreRemoteChanges)
					{
						EventsForClient.FindOrAdd(Client).Append(ModifiedPropertyIds);
					}
				}
			}
			else
			{
				// Remove the property after encountering it here since we can't remove it on end frame
				// because that might happen before the final PostEditChange of a property change in the RC Module.
				PropertiesManuallyNotifiedThisFrame.Remove(ModifiedPropertyId);
			}
		}
	}
}

void FWebSocketMessageHandler::OnPropertyUnexposed(URemoteControlPreset* Owner, const FGuid& EntityId)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	TSharedPtr<FRemoteControlEntity> Entity = Owner->GetExposedEntity(EntityId).Pin();
	check(Entity);

	// Cache the property field that was removed for end of frame notification
	TTuple<TArray<FGuid>, TArray<FName>>& Entries = PerFrameRemovedProperties.FindOrAdd(Owner->GetPresetId());
	Entries.Key.AddUnique(EntityId);
	Entries.Value.AddUnique(Entity->GetLabel());
}

void FWebSocketMessageHandler::OnFieldRenamed(URemoteControlPreset* Owner, FName OldFieldLabel, FName NewFieldLabel)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	//Cache the field that was renamed for end of frame notification
	PerFrameRenamedFields.FindOrAdd(Owner->GetPresetId()).AddUnique(TTuple<FName, FName>(OldFieldLabel, NewFieldLabel));
}

void FWebSocketMessageHandler::OnMetadataModified(URemoteControlPreset* Owner)
{
	//Cache the field that was renamed for end of frame notification
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	//Cache the field that was renamed for end of frame notification
	PerFrameModifiedMetadata.Add(Owner->GetPresetId());
}

void FWebSocketMessageHandler::OnActorPropertyChanged(URemoteControlPreset* Owner, FRemoteControlActor& Actor, UObject* ModifiedObject, FProperty* ModifiedProperty)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	FRCFieldPathInfo FieldPath { ModifiedProperty->GetName() };
	if (!FieldPath.Resolve(ModifiedObject))
	{
		return;
	}

	FRCObjectReference Ref;
	Ref.Object = ModifiedObject;
	Ref.Property = ModifiedProperty;
	Ref.ContainerAdress = FieldPath.GetResolvedData().ContainerAddress;
	Ref.ContainerType = FieldPath.GetResolvedData().Struct;
	Ref.PropertyPathInfo = MoveTemp(FieldPath);
	Ref.Access = ERCAccess::READ_ACCESS;


	//Cache the property field that changed for end of frame notification
	TMap<FGuid, TMap<FRemoteControlActor, TArray<FRCObjectReference>>>& EventsForClient = PerFrameActorPropertyChanged.FindOrAdd(Owner->GetPresetId());

	// Dont send events to the client that triggered it.
	if (TArray<FGuid>* SubscribedClients = WebSocketNotificationMap.Find(Owner->GetPresetId()))
	{
		for (const FGuid& Client : *SubscribedClients)
		{
			if (Client != ActingClientId)
			{
				TMap<FRemoteControlActor, TArray<FRCObjectReference>>& ModifiedPropertiesPerActor = EventsForClient.FindOrAdd(Client);
				ModifiedPropertiesPerActor.FindOrAdd(Actor).AddUnique(Ref);
			}
		}
	}
}

void FWebSocketMessageHandler::OnEntitiesModified(URemoteControlPreset* Owner, const TSet<FGuid>& ModifiedEntities)
{
	// We do not need to store these event for the current frame since this was already handled by the preset in this case.
	if (!Owner || ModifiedEntities.Num() == 0)
	{
		return;
	}
	
	TArray<uint8> Payload;
	WebRemoteControlUtils::SerializeResponse(FRCPresetEntitiesModifiedEvent{Owner, ModifiedEntities.Array()}, Payload);
	BroadcastToListeners(Owner->GetPresetId(), Payload);
}

void FWebSocketMessageHandler::OnLayoutModified(URemoteControlPreset* Owner)
{
	if (Owner == nullptr)
	{
		return;
	}

	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	//Cache the field that was renamed for end of frame notification
	PerFrameModifiedPresetLayouts.Add(Owner->GetPresetId());
}

void FWebSocketMessageHandler::OnConnectionClosedCallback(FGuid ClientId)
{
	//Cleanup client that were waiting for callbacks
	for (auto Iter = WebSocketNotificationMap.CreateIterator(); Iter; ++Iter)
	{
		Iter.Value().Remove(ClientId);
	}

	/** Remove this client's config. */
	ClientConfigMap.Remove(ClientId);
}

void FWebSocketMessageHandler::OnEndFrame()
{
	//Early exit if no clients are requesting notifications
	if (WebSocketNotificationMap.Num() <= 0)
	{
		return;
	}

	PropertyNotificationFrameCounter++;

	if (PropertyNotificationFrameCounter >= CVarWebRemoteControlFramesBetweenPropertyNotifications.GetValueOnGameThread())
	{
		PropertyNotificationFrameCounter = 0;
		ProcessChangedProperties();
		ProcessChangedActorProperties();
		ProcessRemovedProperties();
		ProcessAddedProperties();
		ProcessRenamedFields();
		ProcessModifiedMetadata();
		ProcessModifiedPresetLayouts();
	}
}

void FWebSocketMessageHandler::ProcessAddedProperties()
{
	for (const TPair<FGuid, TArray<FGuid>>& Entry : PerFrameAddedProperties)
	{
		if (Entry.Value.Num() <= 0 || !ShouldProcessEventForPreset(Entry.Key))
		{
			continue;
		}

		URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry.Key);
		if (Preset == nullptr)
		{
			continue;
		}

		FRCPresetDescription AddedPropertiesDescription;
		AddedPropertiesDescription.Name = Preset->GetName();
		AddedPropertiesDescription.Path = Preset->GetPathName();
		AddedPropertiesDescription.ID = Preset->GetPresetId().ToString();

		TMap<FRemoteControlPresetGroup*, TArray<FGuid>> GroupedNewFields;

		for (const FGuid& Id : Entry.Value)
		{
			if (FRemoteControlPresetGroup* Group = Preset->Layout.FindGroupFromField(Id))
			{
				GroupedNewFields.FindOrAdd(Group).Add(Id);
			}
		}

		for (const TTuple<FRemoteControlPresetGroup*, TArray<FGuid>>& Tuple : GroupedNewFields)
		{
			AddedPropertiesDescription.Groups.Emplace(Preset, *Tuple.Key, Tuple.Value);
		}

		TArray<uint8> Payload;
		WebRemoteControlUtils::SerializeResponse(FRCPresetFieldsAddedEvent{ Preset->GetFName(), Preset->GetPresetId(), AddedPropertiesDescription }, Payload);
		BroadcastToListeners(Entry.Key, Payload);
	}

	PerFrameAddedProperties.Empty();
}

void FWebSocketMessageHandler::ProcessRemovedProperties()
{
	for (const TPair<FGuid, TTuple<TArray<FGuid>, TArray<FName>>>& Entry : PerFrameRemovedProperties)
	{
		if (Entry.Value.Key.Num() <= 0 || !ShouldProcessEventForPreset(Entry.Key))
		{
			continue;
		}

		URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry.Key);
		if (Preset == nullptr)
		{
			continue;
		}

		ensure(Entry.Value.Key.Num() == Entry.Value.Value.Num());
		
		TArray<uint8> Payload;
		WebRemoteControlUtils::SerializeResponse(FRCPresetFieldsRemovedEvent{ Preset->GetFName(), Preset->GetPresetId(), Entry.Value.Value, Entry.Value.Key }, Payload);
		BroadcastToListeners(Entry.Key, Payload);
	}
	
	PerFrameRemovedProperties.Empty();
}

void FWebSocketMessageHandler::ProcessRenamedFields()
{
	for (const TPair<FGuid, TArray<TTuple<FName, FName>>>& Entry : PerFrameRenamedFields)
	{
		if (Entry.Value.Num() <= 0 || !ShouldProcessEventForPreset(Entry.Key))
		{
			continue;
		}

		URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry.Key);
		if (Preset == nullptr)
		{
			continue;
		}

		TArray<uint8> Payload;
		WebRemoteControlUtils::SerializeResponse(FRCPresetFieldsRenamedEvent{Preset->GetFName(), Preset->GetPresetId(), Entry.Value}, Payload);
		BroadcastToListeners(Entry.Key, Payload);
	}

	PerFrameRenamedFields.Empty();
}

void FWebSocketMessageHandler::ProcessModifiedMetadata()
{
	for (const FGuid& Entry : PerFrameModifiedMetadata)
	{
		if (ShouldProcessEventForPreset(Entry))
		{
			if (URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry))
			{
				TArray<uint8> Payload;
				WebRemoteControlUtils::SerializeResponse(FRCPresetMetadataModified{ Preset }, Payload);
				BroadcastToListeners(Entry, Payload);
			}
		}
	}

	PerFrameModifiedMetadata.Empty();
}

void FWebSocketMessageHandler::ProcessModifiedPresetLayouts()
{
	for (const FGuid& Entry : PerFrameModifiedPresetLayouts)
	{
		if (ShouldProcessEventForPreset(Entry))
		{
			if (URemoteControlPreset* Preset = IRemoteControlModule::Get().ResolvePreset(Entry))
			{
				TArray<uint8> Payload;
				WebRemoteControlUtils::SerializeResponse(FRCPresetLayoutModified{ Preset }, Payload);
				BroadcastToListeners(Entry, Payload);
			}
		}
	}

	PerFrameModifiedPresetLayouts.Empty();
}

void FWebSocketMessageHandler::BroadcastToListeners(const FGuid& TargetPresetId, const TArray<uint8>& Payload)
{
	const TArray<FGuid>& Listeners = WebSocketNotificationMap.FindChecked(TargetPresetId);
	for (const FGuid& Listener : Listeners)
	{
		Server->Send(Listener, Payload);
	}
}

bool FWebSocketMessageHandler::ShouldProcessEventForPreset(const FGuid& PresetId) const
{
	return WebSocketNotificationMap.Contains(PresetId) && WebSocketNotificationMap[PresetId].Num() > 0;
}

bool FWebSocketMessageHandler::WritePropertyChangeEventPayload(URemoteControlPreset* InPreset, const TSet<FGuid>& InModifiedPropertyIds, TArray<uint8>& OutBuffer)
{
	bool bHasProperty = false;

	TArray<FStructOnScope> PropValuesOnScope;
	for (const FGuid& RCPropertyId : InModifiedPropertyIds)
	{
		FRCObjectReference ObjectRef;
		if (TSharedPtr<FRemoteControlProperty> RCProperty = InPreset->GetExposedEntity<FRemoteControlProperty>(RCPropertyId).Pin())
		{
			if (RCProperty->IsBound())
			{
				if (IRemoteControlModule::Get().ResolveObjectProperty(ERCAccess::READ_ACCESS, RCProperty->GetBoundObjects()[0], RCProperty->FieldPathInfo.ToString(), ObjectRef))
				{
					bHasProperty = true;
					PropValuesOnScope.Add(WebSocketMessageHandlerStructUtils::CreatePropertyValueOnScope(RCProperty, ObjectRef));
				}
			}
		}
	}

	if (PropValuesOnScope.Num())
	{
		FStructOnScope FieldsChangedEventOnScope = WebSocketMessageHandlerStructUtils::CreatePresetFieldsChangedStructOnScope(InPreset, PropValuesOnScope);

		FMemoryWriter Writer(OutBuffer);
		WebRemoteControlUtils::SerializeStructOnScope(FieldsChangedEventOnScope, Writer);
	}

	return bHasProperty;
}


bool FWebSocketMessageHandler::WriteActorPropertyChangePayload(URemoteControlPreset* InPreset, const TMap<FRemoteControlActor, TArray<FRCObjectReference>>& InModifications, FMemoryWriter& InWriter)
{
	bool bHasProperty = false;

	TArray<FStructOnScope> ModifiedActorsOnScope;

	for (const TPair<FRemoteControlActor, TArray<FRCObjectReference>>& Pair : InModifications)
	{
		if (AActor* ModifiedActor = Cast<AActor>(Pair.Key.Path.ResolveObject()))
		{
			TArray<FStructOnScope> PropertyValuesOnScope;

			for (const FRCObjectReference& Ref : Pair.Value)
			{
				const FProperty* Property = Ref.Property.Get();

				if (Property && Ref.IsValid())
				{
					bHasProperty = true;
					PropertyValuesOnScope.Add(WebSocketMessageHandlerStructUtils::CreateActorPropertyValueOnScope(InPreset, Ref));
				}
			}

			if (PropertyValuesOnScope.Num())
			{
				ModifiedActorsOnScope.Add(WebSocketMessageHandlerStructUtils::CreateModifiedActorStructOnScope(InPreset, Pair.Key, PropertyValuesOnScope));
			}
		}
	}

	FStructOnScope ActorsModifedOnScope = WebSocketMessageHandlerStructUtils::CreateModifiedActorsStructOnScope(InPreset, ModifiedActorsOnScope);
	WebRemoteControlUtils::SerializeStructOnScope(ActorsModifedOnScope, InWriter);

	return bHasProperty;
}
