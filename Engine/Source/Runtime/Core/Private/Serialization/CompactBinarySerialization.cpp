// Copyright Epic Games, Inc. All Rights Reserved.

#include "Serialization/CompactBinarySerialization.h"

#include "Containers/StringConv.h"
#include "HAL/Platform.h"
#include "Misc/AsciiSet.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/StringBuilder.h"
#include "Misc/Timespan.h"
#include "Serialization/CompactBinaryValidation.h"
#include "Serialization/CompactBinaryValue.h"
#include "Serialization/VarInt.h"
#include "Templates/IdentityFunctor.h"
#include "Templates/Invoke.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

uint64 MeasureCompactBinary(FMemoryView View, ECbFieldType Type)
{
	uint64 Size;
	return TryMeasureCompactBinary(View, Type, Size, Type) ? Size : 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool TryMeasureCompactBinary(FMemoryView View, ECbFieldType& OutType, uint64& OutSize, ECbFieldType Type)
{
	uint64 Size = 0;

	if (FCbFieldType::HasFieldType(Type))
	{
		if (View.GetSize() == 0)
		{
			OutType = ECbFieldType::None;
			OutSize = 1;
			return false;
		}

		Type = *static_cast<const ECbFieldType*>(View.GetData());
		View += 1;
		Size += 1;
	}

	bool bDynamicSize = false;
	uint64 FixedSize = 0;
	switch (FCbFieldType::GetType(Type))
	{
	case ECbFieldType::Null:
		break;
	case ECbFieldType::Object:
	case ECbFieldType::UniformObject:
	case ECbFieldType::Array:
	case ECbFieldType::UniformArray:
	case ECbFieldType::Binary:
	case ECbFieldType::String:
	case ECbFieldType::IntegerPositive:
	case ECbFieldType::IntegerNegative:
	case ECbFieldType::CustomById:
	case ECbFieldType::CustomByName:
		bDynamicSize = true;
		break;
	case ECbFieldType::Float32:
		FixedSize = 4;
		break;
	case ECbFieldType::Float64:
		FixedSize = 8;
		break;
	case ECbFieldType::BoolFalse:
	case ECbFieldType::BoolTrue:
		break;
	case ECbFieldType::ObjectAttachment:
	case ECbFieldType::BinaryAttachment:
	case ECbFieldType::Hash:
		FixedSize = 20;
		break;
	case ECbFieldType::Uuid:
		FixedSize = 16;
		break;
	case ECbFieldType::DateTime:
	case ECbFieldType::TimeSpan:
		FixedSize = 8;
		break;
	case ECbFieldType::ObjectId:
		FixedSize = 12;
		break;
	case ECbFieldType::None:
	default:
		OutType = ECbFieldType::None;
		OutSize = 0;
		return false;
	}

	OutType = Type;

	if (FCbFieldType::HasFieldName(Type))
	{
		if (View.GetSize() == 0)
		{
			OutSize = Size + 1;
			return false;
		}

		uint32 NameLenByteCount = MeasureVarUInt(View.GetData());
		if (View.GetSize() < NameLenByteCount)
		{
			OutSize = Size + NameLenByteCount;
			return false;
		}

		const uint64 NameLen = ReadVarUInt(View.GetData(), NameLenByteCount);
		const uint64 NameSize = NameLen + NameLenByteCount;

		if (bDynamicSize && View.GetSize() < NameSize)
		{
			OutSize = Size + NameSize;
			return false;
		}

		View += NameSize;
		Size += NameSize;
	}

	switch (FCbFieldType::GetType(Type))
	{
	case ECbFieldType::Object:
	case ECbFieldType::UniformObject:
	case ECbFieldType::Array:
	case ECbFieldType::UniformArray:
	case ECbFieldType::Binary:
	case ECbFieldType::String:
	case ECbFieldType::CustomById:
	case ECbFieldType::CustomByName:
		if (View.GetSize() == 0)
		{
			OutSize = Size + 1;
			return false;
		}
		else
		{
			uint32 ValueSizeByteCount = MeasureVarUInt(View.GetData());
			if (View.GetSize() < ValueSizeByteCount)
			{
				OutSize = Size + ValueSizeByteCount;
				return false;
			}
			const uint64 ValueSize = ReadVarUInt(View.GetData(), ValueSizeByteCount);
			OutSize = Size + ValueSize + ValueSizeByteCount;
			return true;
		}
	case ECbFieldType::IntegerPositive:
	case ECbFieldType::IntegerNegative:
		if (View.GetSize() == 0)
		{
			OutSize = Size + 1;
			return false;
		}
		OutSize = Size + MeasureVarUInt(View.GetData());
		return true;
	default:
		OutSize = Size + FixedSize;
		return true;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

FCbField LoadCompactBinary(FArchive& Ar, FCbBufferAllocator Allocator)
{
	TArray<uint8, TInlineAllocator<64>> HeaderBytes;
	ECbFieldType FieldType;
	uint64 FieldSize = 1;

	// Read in small increments until the total field size is known, to avoid reading too far.
	for (;;)
	{
		const int32 ReadSize = int32(FieldSize - HeaderBytes.Num());
		const int32 ReadOffset = HeaderBytes.AddUninitialized(ReadSize);
		Ar.Serialize(HeaderBytes.GetData() + ReadOffset, ReadSize);
		if (TryMeasureCompactBinary(MakeMemoryView(HeaderBytes), FieldType, FieldSize))
		{
			break;
		}
		if (FieldSize == 0)
		{
			Ar.SetError();
			return FCbField();
		}
	}

	// Allocate the buffer, copy the header, and read the remainder of the field.
	FUniqueBuffer Buffer = Allocator(FieldSize);
	checkf(Buffer.GetSize() == FieldSize,
		TEXT("Allocator returned a buffer of size %" UINT64_FMT " bytes when %" UINT64_FMT " bytes were requested."),
		Buffer.GetSize(), FieldSize);
	FMutableMemoryView View = Buffer.GetView().CopyFrom(MakeMemoryView(HeaderBytes));
	if (!View.IsEmpty())
	{
		Ar.Serialize(View.GetData(), static_cast<int64>(View.GetSize()));
	}
	if (ValidateCompactBinary(Buffer, ECbValidateMode::Default) != ECbValidateError::None)
	{
		Ar.SetError();
		return FCbField();
	}
	return FCbField(Buffer.MoveToShared());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void SaveCompactBinary(FArchive& Ar, const FCbFieldView& Field)
{
	check(Ar.IsSaving());
	Field.CopyTo(Ar);
}

void SaveCompactBinary(FArchive& Ar, const FCbArrayView& Array)
{
	check(Ar.IsSaving());
	Array.CopyTo(Ar);
}

void SaveCompactBinary(FArchive& Ar, const FCbObjectView& Object)
{
	check(Ar.IsSaving());
	Object.CopyTo(Ar);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename T, typename ConvertType>
static FArchive& SerializeCompactBinary(FArchive& Ar, T& Value, ConvertType&& Convert)
{
	if (Ar.IsLoading())
	{
		Value = Invoke(Forward<ConvertType>(Convert), LoadCompactBinary(Ar));
	}
	else if (Ar.IsSaving())
	{
		Value.CopyTo(Ar);
	}
	else
	{
		checkNoEntry();
	}
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FCbField& Field)
{
	return SerializeCompactBinary(Ar, Field, FIdentityFunctor());
}

FArchive& operator<<(FArchive& Ar, FCbArray& Array)
{
	return SerializeCompactBinary(Ar, Array, [](FCbField&& Field) { return MoveTemp(Field).AsArray(); });
}

FArchive& operator<<(FArchive& Ar, FCbObject& Object)
{
	return SerializeCompactBinary(Ar, Object, [](FCbField&& Field) { return MoveTemp(Field).AsObject(); });
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class FCbJsonWriter
{
public:
	explicit FCbJsonWriter(FUtf8StringBuilderBase& InBuilder)
		: Builder(InBuilder)
	{
		NewLineAndIndent << LINE_TERMINATOR_ANSI;
	}

	void WriteField(FCbFieldView Field)
	{
		WriteOptionalComma();
		WriteOptionalNewLine();

		if (FUtf8StringView Name = Field.GetName(); !Name.IsEmpty())
		{
			AppendQuotedString(Name);
			Builder << ": "_ASV;
		}

		switch (FCbValue Accessor = Field.GetValue(); Accessor.GetType())
		{
		case ECbFieldType::Null:
			Builder << "null"_ASV;
			break;
		case ECbFieldType::Object:
		case ECbFieldType::UniformObject:
			Builder << '{';
			NewLineAndIndent << '\t';
			bNeedsNewLine = true;
			for (FCbFieldView It : Field)
			{
				WriteField(It);
			}
			NewLineAndIndent.RemoveSuffix(1);
			if (bNeedsComma)
			{
				WriteOptionalNewLine();
			}
			Builder << '}';
			break;
		case ECbFieldType::Array:
		case ECbFieldType::UniformArray:
			Builder << '[';
			NewLineAndIndent << '\t';
			bNeedsNewLine = true;
			for (FCbFieldView It : Field)
			{
				WriteField(It);
			}
			NewLineAndIndent.RemoveSuffix(1);
			if (bNeedsComma)
			{
				WriteOptionalNewLine();
			}
			Builder << ']';
			break;
		case ECbFieldType::Binary:
			AppendBase64String(Accessor.AsBinary());
			break;
		case ECbFieldType::String:
			AppendQuotedString(Accessor.AsString());
			break;
		case ECbFieldType::IntegerPositive:
			Builder << Accessor.AsIntegerPositive();
			break;
		case ECbFieldType::IntegerNegative:
			Builder << Accessor.AsIntegerNegative();
			break;
		case ECbFieldType::Float32:
			Builder.Appendf(UTF8TEXT("%.9g"), Accessor.AsFloat32());
			break;
		case ECbFieldType::Float64:
			Builder.Appendf(UTF8TEXT("%.17g"), Accessor.AsFloat64());
			break;
		case ECbFieldType::BoolFalse:
			Builder << "false"_ASV;
			break;
		case ECbFieldType::BoolTrue:
			Builder << "true"_ASV;
			break;
		case ECbFieldType::ObjectAttachment:
		case ECbFieldType::BinaryAttachment:
			Builder << '"' << Accessor.AsAttachment() << '"';
			break;
		case ECbFieldType::Hash:
			Builder << '"' << Accessor.AsHash() << '"';
			break;
		case ECbFieldType::Uuid:
			Builder << '"' << Accessor.AsUuid() << '"';
			break;
		case ECbFieldType::DateTime:
			Builder << '"' << FTCHARToUTF8(FDateTime(Accessor.AsDateTimeTicks()).ToIso8601()) << '"';
			break;
		case ECbFieldType::TimeSpan:
		{
			const FTimespan Span(Accessor.AsTimeSpanTicks());
			if (Span.GetDays() == 0)
			{
				Builder << '"' << FTCHARToUTF8(Span.ToString(TEXT("%h:%m:%s.%n"))) << '"';
			}
			else
			{
				Builder << '"' << FTCHARToUTF8(Span.ToString(TEXT("%d.%h:%m:%s.%n"))) << '"';
			}
			break;
		}
		case ECbFieldType::ObjectId:
			Builder << '"' << Accessor.AsObjectId() << '"';
			break;
		case ECbFieldType::CustomById:
		{
			FCbCustomById Custom = Accessor.AsCustomById();
			Builder << "{ \"Id\": ";
			Builder << Custom.Id;
			Builder << ", \"Data\": ";
			AppendBase64String(Custom.Data);
			Builder << " }";
			break;
		}
		case ECbFieldType::CustomByName:
		{
			FCbCustomByName Custom = Accessor.AsCustomByName();
			Builder << "{ \"Name\": ";
			AppendQuotedString(Custom.Name);
			Builder << ", \"Data\": ";
			AppendBase64String(Custom.Data);
			Builder << " }";
			break;
		}
		default:
			checkNoEntry();
			break;
		}

		bNeedsComma = true;
		bNeedsNewLine = true;
	}

private:
	void WriteOptionalComma()
	{
		if (bNeedsComma)
		{
			bNeedsComma = false;
			Builder << ',';
		}
	}

	void WriteOptionalNewLine()
	{
		if (bNeedsNewLine)
		{
			bNeedsNewLine = false;
			Builder << NewLineAndIndent;
		}
	}

	void AppendQuotedString(FUtf8StringView Value)
	{
		const FAsciiSet EscapeSet("\\\"\b\f\n\r\t"
			"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
			"\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f");
		Builder << '\"';
		while (!Value.IsEmpty())
		{
			FUtf8StringView Verbatim = FAsciiSet::FindPrefixWithout(Value, EscapeSet);
			Builder << Verbatim;
			Value.RightChopInline(Verbatim.Len());
			FUtf8StringView Escape = FAsciiSet::FindPrefixWith(Value, EscapeSet);
			for (UTF8CHAR Char : Escape)
			{
				switch (Char)
				{
				case '\\': Builder << "\\\\"_ASV; break;
				case '\"': Builder << "\\\""_ASV; break;
				case '\b': Builder << "\\b"_ASV; break;
				case '\f': Builder << "\\f"_ASV; break;
				case '\n': Builder << "\\n"_ASV; break;
				case '\r': Builder << "\\r"_ASV; break;
				case '\t': Builder << "\\t"_ASV; break;
				default:
					Builder.Appendf(UTF8TEXT("\\u%04x"), uint32(Char));
					break;
				}
			}
			Value.RightChopInline(Escape.Len());
		}
		Builder << '\"';
	}

	void AppendBase64String(FMemoryView Value)
	{
		Builder << '"';
		checkf(Value.GetSize() <= 512 * 1024 * 1024, TEXT("Encoding 512 MiB or larger is not supported. ")
			TEXT("Size: " UINT64_FMT), Value.GetSize());
		const uint32 EncodedSize = FBase64::GetEncodedDataSize(uint32(Value.GetSize()));
		const int32 EncodedIndex = Builder.AddUninitialized(int32(EncodedSize));
		FBase64::Encode(static_cast<const uint8*>(Value.GetData()), uint32(Value.GetSize()),
			reinterpret_cast<ANSICHAR*>(Builder.GetData() + EncodedIndex));
		Builder << '"';
	}

private:
	FUtf8StringBuilderBase& Builder;
	TUtf8StringBuilder<32> NewLineAndIndent;
	bool bNeedsComma{false};
	bool bNeedsNewLine{false};
};

void CompactBinaryToJson(const FCbObjectView& Object, FUtf8StringBuilderBase& Builder)
{
	FCbJsonWriter Writer(Builder);
	Writer.WriteField(Object.AsFieldView());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
