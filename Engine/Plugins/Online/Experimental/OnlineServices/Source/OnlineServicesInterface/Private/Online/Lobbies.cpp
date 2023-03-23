// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/Lobbies.h"

namespace UE::Online {

const FString LexToString(const FLobbyVariant& Variant)
{
	return Variant.GetString();
}

void LexFromString(FLobbyVariant& Variant, const TCHAR* InStr)
{
	Variant.Set(TEXT("")); // todo
}

int64 FLobbyVariant::GetInt64() const
{
	int64 AsInt64 = 0;
	if (VariantData.IsType<int64>())
	{
		AsInt64 = VariantData.Get<int64>();
	}
	else if (VariantData.IsType<bool>())
	{
		AsInt64 = static_cast<int64>(VariantData.Get<bool>());
	}
	else if (VariantData.IsType<double>())
	{
		AsInt64 = static_cast<int64>(VariantData.Get<double>());
	}
	else if (VariantData.IsType<FString>())
	{
		::LexFromString(AsInt64, *VariantData.Get<FString>());
	}
	return AsInt64;
}

double FLobbyVariant::GetDouble() const
{
	double AsDouble = 0;
	if (VariantData.IsType<double>())
	{
		AsDouble = VariantData.Get<double>();
	}
	else if (VariantData.IsType<FString>())
	{
		::LexFromString(AsDouble, *VariantData.Get<FString>());
	}
	else if (VariantData.IsType<int64>())
	{
		AsDouble = static_cast<double>(VariantData.Get<int64>());
	}
	else if (VariantData.IsType<bool>())
	{
		AsDouble = static_cast<double>(VariantData.Get<bool>());
	}
	return AsDouble;
}

bool FLobbyVariant::GetBoolean() const
{
	bool bAsBool = false;
	if (VariantData.IsType<bool>())
	{
		bAsBool = VariantData.Get<bool>();
	}
	else if (VariantData.IsType<FString>())
	{
		::LexFromString(bAsBool, *VariantData.Get<FString>());
	}
	else
	{
		bAsBool = GetInt64() != 0;
	}
	return bAsBool;
}

FString FLobbyVariant::GetString() const
{
	if (VariantData.IsType<FString>())
	{
		return VariantData.Get<FString>();
	}
	else if (VariantData.IsType<int64>())
	{
		return FString::Printf(TEXT("%" INT64_FMT), VariantData.Get<int64>());
	}
	else if (VariantData.IsType<bool>())
	{
		return ::LexToString(VariantData.Get<bool>());
	}
	else if (VariantData.IsType<double>())
	{
		return FString::Printf(TEXT("%f"), VariantData.Get<double>());
	}
	else
	{
		checkNoEntry();
	}
	return TEXT("");
}

bool FLobbyVariant::operator==(const FLobbyVariant& Other) const
{
	if (VariantData.GetIndex() != Other.VariantData.GetIndex())
	{
		return false;
	}

	switch (VariantData.GetIndex())
	{
	case FVariantType::IndexOfType<FString>():	return VariantData.Get<FString>() == Other.VariantData.Get<FString>();
	case FVariantType::IndexOfType<int64>():	return VariantData.Get<int64>() == Other.VariantData.Get<int64>();
	case FVariantType::IndexOfType<double>():	return VariantData.Get<double>() == Other.VariantData.Get<double>();

	default:checkNoEntry(); // Intentional fallthrough
	case FVariantType::IndexOfType<bool>():	return VariantData.Get<bool>() == Other.VariantData.Get<bool>();
	}
}

const TCHAR* LexToString(ELobbyJoinPolicy Policy)
{
	switch (Policy)
	{
	case ELobbyJoinPolicy::PublicAdvertised:	return TEXT("PublicAdvertised");
	case ELobbyJoinPolicy::PublicNotAdvertised:	return TEXT("PublicNotAdvertised");
	default:									checkNoEntry(); // Intentional fallthrough
	case ELobbyJoinPolicy::InvitationOnly:		return TEXT("InvitationOnly");
	}
}

void LexFromString(ELobbyJoinPolicy& OutPolicy, const TCHAR* InStr)
{
	if (FCString::Stricmp(InStr, TEXT("PublicAdvertised")) == 0)
	{
		OutPolicy = ELobbyJoinPolicy::PublicAdvertised;
	}
	else if (FCString::Stricmp(InStr, TEXT("PublicNotAdvertised")) == 0)
	{
		OutPolicy = ELobbyJoinPolicy::PublicNotAdvertised;
	}
	else if (FCString::Stricmp(InStr, TEXT("InvitationOnly")) == 0)
	{
		OutPolicy = ELobbyJoinPolicy::InvitationOnly;
	}
	else
	{
		checkNoEntry();
		OutPolicy = ELobbyJoinPolicy::InvitationOnly;
	}
}

const TCHAR* LexToString(ELobbyComparisonOp Comparison)
{
	switch (Comparison)
	{
	case ELobbyComparisonOp::Equals:			return TEXT("Equals");
	case ELobbyComparisonOp::NotEquals:			return TEXT("NotEquals");
	case ELobbyComparisonOp::GreaterThan:		return TEXT("GreaterThan");
	case ELobbyComparisonOp::GreaterThanEquals:	return TEXT("GreaterThanEquals");
	case ELobbyComparisonOp::LessThan:			return TEXT("LessThan");
	case ELobbyComparisonOp::LessThanEquals:	return TEXT("LessThanEquals");
	case ELobbyComparisonOp::Near:				return TEXT("Near");
	case ELobbyComparisonOp::In:				return TEXT("In");
	default:									checkNoEntry(); // Intentional fallthrough
	case ELobbyComparisonOp::NotIn:				return TEXT("NotIn");
	}
}

void LexFromString(ELobbyComparisonOp& OutComparison, const TCHAR* InStr)
{
	if (FCString::Stricmp(InStr, TEXT("Equals")) == 0)
	{
		OutComparison = ELobbyComparisonOp::Equals;
	}
	else if (FCString::Stricmp(InStr, TEXT("NotEquals")) == 0)
	{
		OutComparison = ELobbyComparisonOp::NotEquals;
	}
	else if (FCString::Stricmp(InStr, TEXT("GreaterThan")) == 0)
	{
		OutComparison = ELobbyComparisonOp::GreaterThan;
	}
	else if (FCString::Stricmp(InStr, TEXT("GreaterThanEquals")) == 0)
	{
		OutComparison = ELobbyComparisonOp::GreaterThanEquals;
	}
	else if (FCString::Stricmp(InStr, TEXT("LessThan")) == 0)
	{
		OutComparison = ELobbyComparisonOp::LessThan;
	}
	else if (FCString::Stricmp(InStr, TEXT("LessThanEquals")) == 0)
	{
		OutComparison = ELobbyComparisonOp::LessThanEquals;
	}
	else if (FCString::Stricmp(InStr, TEXT("Near")) == 0)
	{
		OutComparison = ELobbyComparisonOp::Near;
	}
	else if (FCString::Stricmp(InStr, TEXT("In")) == 0)
	{
		OutComparison = ELobbyComparisonOp::In;
	}
	else
	{
		checkNoEntry();
		OutComparison = ELobbyComparisonOp::In;
	}
}

void SortLobbies(const TArray<FFindLobbySearchFilter>& Filters, TArray<TSharedRef<const FLobby>>& Lobbies)
{
	// todo
}

/* UE::Online */ }
