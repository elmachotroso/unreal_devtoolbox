// Copyright Epic Games, Inc. All Rights Reserved.

#include "Trace/Config.h"

#if UE_TRACE_ENABLED

#include "Platform.h"
#include "Trace/Detail/Atomic.h"
#include "Trace/Detail/Channel.h"
#include "Trace/Detail/Protocol.h"
#include "Trace/Detail/Transport.h"
#include "Trace/Trace.inl"
#include "WriteBufferRedirect.h"

#include <limits.h>
#include <stdlib.h>

#if PLATFORM_WINDOWS
#	define TRACE_PRIVATE_STOMP 0 // 1=overflow, 2=underflow
#	if TRACE_PRIVATE_STOMP
#	include "Windows/AllowWindowsPlatformTypes.h"
#		include "Windows/WindowsHWrapper.h"
#	include "Windows/HideWindowsPlatformTypes.h"
#	endif
#else
#	define TRACE_PRIVATE_STOMP 0
#endif

#ifndef TRACE_PRIVATE_BUFFER_SEND
#	define TRACE_PRIVATE_BUFFER_SEND 0
#endif


namespace UE {
namespace Trace {
namespace Private {

////////////////////////////////////////////////////////////////////////////////
int32			Encode(const void*, int32, void*, int32);
void			Writer_SendData(uint32, uint8* __restrict, uint32);
void			Writer_InitializeTail(int32);
void			Writer_ShutdownTail();
void			Writer_TailAppend(uint32, uint8* __restrict, uint32, bool=false);
void			Writer_TailOnConnect();
void			Writer_InitializeSharedBuffers();
void			Writer_ShutdownSharedBuffers();
void			Writer_UpdateSharedBuffers();
void			Writer_CacheOnConnect();
void			Writer_InitializePool();
void			Writer_ShutdownPool();
void			Writer_DrainBuffers();
void			Writer_EndThreadBuffer();
uint32			Writer_GetControlPort();
void			Writer_UpdateControl();
void			Writer_InitializeControl();
void			Writer_ShutdownControl();
bool			Writer_IsTracing();



////////////////////////////////////////////////////////////////////////////////
UE_TRACE_EVENT_BEGIN($Trace, NewTrace, Important|NoSync)
	UE_TRACE_EVENT_FIELD(uint64, StartCycle)
	UE_TRACE_EVENT_FIELD(uint64, CycleFrequency)
	UE_TRACE_EVENT_FIELD(uint16, Endian)
	UE_TRACE_EVENT_FIELD(uint8, PointerSize)
UE_TRACE_EVENT_END()



////////////////////////////////////////////////////////////////////////////////
static bool						GInitialized;		// = false;
FStatistics						GTraceStatistics;	// = {};
uint64							GStartCycle;		// = 0;
TRACELOG_API uint32 volatile	GLogSerial;			// = 0;
// Counter of calls to Writer_WorkerUpdate to enable regular flushing of output buffers 
static uint32					GUpdateCounter;		// = 0;



////////////////////////////////////////////////////////////////////////////////
struct FWriteTlsContext
{
				~FWriteTlsContext();
	uint32		GetThreadId();

private:
	uint32		ThreadId = 0;
};

////////////////////////////////////////////////////////////////////////////////
FWriteTlsContext::~FWriteTlsContext()
{
	if (GInitialized)
	{
		Writer_EndThreadBuffer();
	}
}

////////////////////////////////////////////////////////////////////////////////
uint32 FWriteTlsContext::GetThreadId()
{
	if (ThreadId)
	{
		return ThreadId;
	}

	static uint32 volatile Counter;
	ThreadId = AtomicAddRelaxed(&Counter, 1u) + ETransportTid::Bias;
	return ThreadId;
}

////////////////////////////////////////////////////////////////////////////////
thread_local FWriteTlsContext	GTlsContext;

////////////////////////////////////////////////////////////////////////////////
uint32 Writer_GetThreadId()
{
	return GTlsContext.GetThreadId();
}



////////////////////////////////////////////////////////////////////////////////
void*			(*AllocHook)(SIZE_T, uint32);			// = nullptr
void			(*FreeHook)(void*, SIZE_T);				// = nullptr

////////////////////////////////////////////////////////////////////////////////
void Writer_MemorySetHooks(decltype(AllocHook) Alloc, decltype(FreeHook) Free)
{
	AllocHook = Alloc;
	FreeHook = Free;
}

////////////////////////////////////////////////////////////////////////////////
void* Writer_MemoryAllocate(SIZE_T Size, uint32 Alignment)
{
	TWriteBufferRedirect<6 << 10> TraceData;

	void* Ret = nullptr;

#if TRACE_PRIVATE_STOMP
	static uint8* Base;
	if (Base == nullptr)
	{
		Base = (uint8*)VirtualAlloc(0, 1ull << 40, MEM_RESERVE, PAGE_READWRITE);
	}

	static SIZE_T PageSize = 4096;
	Base += PageSize;
	uint8* NextBase = Base + ((PageSize - 1 + Size) & ~(PageSize - 1));
	VirtualAlloc(Base, SIZE_T(NextBase - Base), MEM_COMMIT, PAGE_READWRITE);
#if TRACE_PRIVATE_STOMP == 1
	Ret = NextBase - Size;
#elif TRACE_PRIVATE_STOMP == 2
	Ret = Base;
#endif
	Base = NextBase;
#else // TRACE_PRIVATE_STOMP

	if (AllocHook != nullptr)
	{
		Ret = AllocHook(Size, Alignment);
	}
	else
	{
#if defined(_MSC_VER)
		Ret = _aligned_malloc(Size, Alignment);
#elif (defined(__ANDROID_API__) && __ANDROID_API__ < 28) || defined(__APPLE__)
		posix_memalign(&Ret, Alignment, Size);
#else
		Ret = aligned_alloc(Alignment, Size);
#endif
	}
#endif // TRACE_PRIVATE_STOMP

	if (TraceData.GetSize())
	{
		uint32 ThreadId = Writer_GetThreadId();
		Writer_TailAppend(ThreadId, TraceData.GetData(), TraceData.GetSize());
	}

#if TRACE_PRIVATE_STATISTICS
	AtomicAddRelaxed(&GTraceStatistics.MemoryUsed, uint32(Size));
#endif

	return Ret;
}

////////////////////////////////////////////////////////////////////////////////
void Writer_MemoryFree(void* Address, uint32 Size)
{
#if TRACE_PRIVATE_STOMP
	if (Address == nullptr)
	{
		return;
	}

	*(uint8*)Address = 0xfe;

	MEMORY_BASIC_INFORMATION MemInfo;
	VirtualQuery(Address, &MemInfo, sizeof(MemInfo));

	DWORD Unused;
	VirtualProtect(MemInfo.BaseAddress, MemInfo.RegionSize, PAGE_READONLY, &Unused);
#else // TRACE_PRIVATE_STOMP
	TWriteBufferRedirect<6 << 10> TraceData;

	if (FreeHook != nullptr)
	{
		FreeHook(Address, Size);
	}
	else
	{
#if defined(_MSC_VER)
		_aligned_free(Address);
#else
		free(Address);
#endif
	}

	if (TraceData.GetSize())
	{
		uint32 ThreadId = Writer_GetThreadId();
		Writer_TailAppend(ThreadId, TraceData.GetData(), TraceData.GetSize());
	}
#endif // TRACE_PRIVATE_STOMP

#if TRACE_PRIVATE_STATISTICS
	AtomicAddRelaxed(&GTraceStatistics.MemoryUsed, uint32(-int64(Size)));
#endif
}



////////////////////////////////////////////////////////////////////////////////
static UPTRINT					GDataHandle;		// = 0
UPTRINT							GPendingDataHandle;	// = 0

////////////////////////////////////////////////////////////////////////////////
#if TRACE_PRIVATE_BUFFER_SEND
static const SIZE_T GSendBufferSize = 1 << 20; // 1Mb
uint8* GSendBuffer; // = nullptr;
uint8* GSendBufferCursor; // = nullptr;
static bool Writer_FlushSendBuffer()
{
	if( GSendBufferCursor > GSendBuffer )
	{
		if (!IoWrite(GDataHandle, GSendBuffer, GSendBufferCursor - GSendBuffer))
		{
			IoClose(GDataHandle);
			GDataHandle = 0;
			return false;
		}
		GSendBufferCursor = GSendBuffer;
	}
	return true;
}
#else
static bool Writer_FlushSendBuffer() { return true; }
#endif

////////////////////////////////////////////////////////////////////////////////
static void Writer_SendDataImpl(const void* Data, uint32 Size)
{
#if TRACE_PRIVATE_STATISTICS
	GTraceStatistics.BytesSent += Size;
#endif

#if TRACE_PRIVATE_BUFFER_SEND
	// If there's not enough space for this data, flush
	if (GSendBufferCursor + Size > GSendBuffer + GSendBufferSize)
	{
		if (!Writer_FlushSendBuffer())
		{
			return;
		}
	}

	// Should rarely happen but if we're asked to send large data send it directly
	if (Size > GSendBufferSize)
	{
		if (!IoWrite(GDataHandle, Data, Size))
		{
			IoClose(GDataHandle);
			GDataHandle = 0;
		}
	}
	// Otherwise append to the buffer
	else
	{
		memcpy(GSendBufferCursor, Data, Size);
		GSendBufferCursor += Size;
	}
#else
	if (!IoWrite(GDataHandle, Data, Size))
	{
		IoClose(GDataHandle);
		GDataHandle = 0;
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////
void Writer_SendDataRaw(const void* Data, uint32 Size)
{
	if (!GDataHandle)
	{
		return;
	}

	Writer_SendDataImpl(Data, Size);
}

////////////////////////////////////////////////////////////////////////////////
void Writer_SendData(uint32 ThreadId, uint8* __restrict Data, uint32 Size)
{
	static_assert(ETransport::Active == ETransport::TidPacketSync, "Active should be set to what the compiled code uses. It is used to track places that assume transport packet format");

#if TRACE_PRIVATE_STATISTICS
	GTraceStatistics.BytesTraced += Size;
#endif

	if (!GDataHandle)
	{
		return;
	}

	// Smaller buffers usually aren't redundant enough to benefit from being
	// compressed. They often end up being larger.
	if (Size <= 384)
	{
		Data -= sizeof(FTidPacket);
		Size += sizeof(FTidPacket);
		auto* Packet = (FTidPacket*)Data;
		Packet->ThreadId = uint16(ThreadId & FTidPacketBase::ThreadIdMask);
		Packet->PacketSize = uint16(Size);

		Writer_SendDataImpl(Data, Size);
		return;
	}

	// Buffer size is expressed as "A + B" where A is a maximum expected
	// input size (i.e. at least GPoolBlockSize) and B is LZ4 overhead as
	// per LZ4_COMPRESSBOUND.
	TTidPacketEncoded<8192 + 64> Packet;

	Packet.ThreadId = FTidPacketBase::EncodedMarker;
	Packet.ThreadId |= uint16(ThreadId & FTidPacketBase::ThreadIdMask);
	Packet.DecodedSize = uint16(Size);
	Packet.PacketSize = Encode(Data, Packet.DecodedSize, Packet.Data, sizeof(Packet.Data));
	Packet.PacketSize += sizeof(FTidPacketEncoded);

	Writer_SendDataImpl(&Packet, Packet.PacketSize);
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_DescribeEvents()
{
	TWriteBufferRedirect<4096> TraceData;

	FEventNode::FIter Iter = FEventNode::ReadNew();
	while (const FEventNode* Event = Iter.GetNext())
	{
		Event->Describe();

		// Flush just in case an NewEvent event will be larger than 512 bytes.
		if (TraceData.GetSize() >= (TraceData.GetCapacity() - 512))
		{
			Writer_SendData(ETransportTid::Events, TraceData.GetData(), TraceData.GetSize());
			TraceData.Reset();
		}
	}

	if (TraceData.GetSize())
	{
		Writer_SendData(ETransportTid::Events, TraceData.GetData(), TraceData.GetSize());
	}
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_AnnounceChannels()
{
	FChannel::Iter Iter = FChannel::ReadNew();
	while (const FChannel* Channel = Iter.GetNext())
	{
		Channel->Announce();
	}
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_DescribeAnnounce()
{
	if (!GDataHandle)
	{
		return;
	}

	Writer_AnnounceChannels();
	Writer_DescribeEvents();
}



////////////////////////////////////////////////////////////////////////////////
static int8			GSyncPacketCountdown;	// = 0
static const int8	GNumSyncPackets			= 3;

////////////////////////////////////////////////////////////////////////////////
static void Writer_SendSync()
{
	if (GSyncPacketCountdown <= 0)
	{
		return;
	}

	// It is possible that some events get collected and discarded by a previous
	// update that are newer than events sent it the following update where IO
	// is established. This will result in holes in serial numbering. A few sync
	// points are sent to aid analysis in determining what are holes and what is
	// just a requirement for more data. Holws will only occurr at the start.

	// Note that Sync is alias as Important/Internal as changing Bias would
	// break backwards compatibility.

	FTidPacketBase SyncPacket = { sizeof(SyncPacket), ETransportTid::Sync };
	Writer_SendDataImpl(&SyncPacket, sizeof(SyncPacket));

	--GSyncPacketCountdown;
}

////////////////////////////////////////////////////////////////////////////////
static bool Writer_UpdateConnection()
{
	if (!GPendingDataHandle)
	{
		return false;
	}

	// Is this a close request? So that we capture some of the events around
	// the closure we will add some inertia before enacting the close.
	static const uint32 CloseInertia = 2;
	if (GPendingDataHandle >= (~0ull - CloseInertia))
	{
		--GPendingDataHandle;

		if (GPendingDataHandle == (~0ull -CloseInertia))
		{
			if (GDataHandle)
			{
				Writer_FlushSendBuffer();
				IoClose(GDataHandle);
			}

			GDataHandle = 0;
			GPendingDataHandle = 0;
		}

		return true;
	}

	// Reject the pending connection if we've already got a connection
	if (GDataHandle)
	{
		IoClose(GPendingDataHandle);
		GPendingDataHandle = 0;
		return false;
	}

	GDataHandle = GPendingDataHandle;
	GPendingDataHandle = 0;

#if TRACE_PRIVATE_BUFFER_SEND
	if (!GSendBuffer)
	{
		GSendBuffer = static_cast<uint8*>(Writer_MemoryAllocate(GSendBufferSize, 16));
	}
	GSendBufferCursor = GSendBuffer;
#endif

	// Handshake.
	struct FHandshake
	{
		uint32 Magic			= 'TRC2';
		uint16 MetadataSize		= uint16(4); //  = sizeof(MetadataField0 + ControlPort)
		uint16 MetadataField0	= uint16(sizeof(ControlPort) | (ControlPortFieldId << 8));
		uint16 ControlPort		= uint16(Writer_GetControlPort());
		enum
		{
			Size				= 10,
			ControlPortFieldId	= 0,
		};
	};
	FHandshake Handshake;
	bool bOk = IoWrite(GDataHandle, &Handshake, FHandshake::Size);

	// Stream header
	const struct {
		uint8 TransportVersion	= ETransport::TidPacketSync;
		uint8 ProtocolVersion	= EProtocol::Id;
	} TransportHeader;
	bOk &= IoWrite(GDataHandle, &TransportHeader, sizeof(TransportHeader));

	if (!bOk)
	{
		IoClose(GDataHandle);
		GDataHandle = 0;
		return false;
	}

	// Reset statistics.
	GTraceStatistics.BytesSent = 0;
	GTraceStatistics.BytesTraced = 0;

	// The first events we will send are ones that describe the trace's events
	FEventNode::OnConnect();
	Writer_DescribeEvents();

	// Send cached events (i.e. importants) and the tail of recent events
	Writer_CacheOnConnect();
	Writer_TailOnConnect();

	// See Writer_SendSync() for details.
	GSyncPacketCountdown = GNumSyncPackets;

	return true;
}



////////////////////////////////////////////////////////////////////////////////
static UPTRINT			GWorkerThread;		// = 0;
static volatile bool	GWorkerThreadQuit;	// = false;
static volatile unsigned int	GUpdateInProgress;	// = 0;

////////////////////////////////////////////////////////////////////////////////
static void Writer_WorkerUpdate()
{
	if (!AtomicCompareExchangeAcquire(&GUpdateInProgress, 1u, 0u))
	{
		return;
	}
	
	Writer_UpdateControl();
	Writer_UpdateConnection();
	Writer_DescribeAnnounce();
	Writer_UpdateSharedBuffers();
	Writer_DrainBuffers();
	Writer_SendSync();

#if TRACE_PRIVATE_BUFFER_SEND
	const uint32 FlushSendBufferCadenceMask = 8-1; // Flush every 8 calls 
	if( (++GUpdateCounter & FlushSendBufferCadenceMask) == 0)
	{
		Writer_FlushSendBuffer();
	}
#endif

	AtomicExchangeRelease(&GUpdateInProgress, 0u);
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_WorkerThread()
{
	ThreadRegister(TEXT("Trace"), 0, INT_MAX);

	while (!GWorkerThreadQuit)
	{
		Writer_WorkerUpdate();

		const uint32 SleepMs = 17;
		ThreadSleep(SleepMs);
	}
}

////////////////////////////////////////////////////////////////////////////////
void Writer_WorkerCreate()
{
	if (GWorkerThread)
	{
		return;
	}

	GWorkerThread = ThreadCreate("TraceWorker", Writer_WorkerThread);
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_WorkerJoin()
{
	if (!GWorkerThread)
	{
		return;
	}

	GWorkerThreadQuit = true;
	ThreadJoin(GWorkerThread);
	ThreadDestroy(GWorkerThread);

	Writer_WorkerUpdate();

	GWorkerThread = 0;
}



////////////////////////////////////////////////////////////////////////////////
static void Writer_InternalInitializeImpl()
{
	if (GInitialized)
	{
		return;
	}

	GInitialized = true;
	GStartCycle = TimeGetTimestamp();

	Writer_InitializeSharedBuffers();
	Writer_InitializePool();
	Writer_InitializeControl();

	UE_TRACE_LOG($Trace, NewTrace, TraceLogChannel)
		<< NewTrace.StartCycle(GStartCycle)
		<< NewTrace.CycleFrequency(TimeGetFrequency())
		<< NewTrace.Endian(0x524d)
		<< NewTrace.PointerSize(sizeof(void*));
}

////////////////////////////////////////////////////////////////////////////////
static void Writer_InternalShutdown()
{
	if (!GInitialized)
	{
		return;
	}

	Writer_WorkerJoin();

	if (GDataHandle)
	{
		Writer_FlushSendBuffer();
		IoClose(GDataHandle);
		GDataHandle = 0;
	}

	Writer_ShutdownControl();
	Writer_ShutdownPool();
	Writer_ShutdownSharedBuffers();
	Writer_ShutdownTail();

#if TRACE_PRIVATE_BUFFER_SEND
	if (GSendBuffer)
	{
		Writer_MemoryFree(GSendBuffer, GSendBufferSize);
		GSendBuffer = nullptr;
		GSendBufferCursor = nullptr;
	}
#endif

	GInitialized = false;
}

////////////////////////////////////////////////////////////////////////////////
void Writer_InternalInitialize()
{
	using namespace Private;

	if (!GInitialized)
	{
		static struct FInitializer
		{
			FInitializer()
			{
				Writer_InternalInitializeImpl();
			}
			~FInitializer()
			{
				/* We'll not shut anything down here so we can hopefully capture
				 * any subsequent events. However, we will shutdown the worker
				 * thread and leave it for something else to call update() (mem
				 * tracing at time of writing). Windows will have already done
				 * this implicitly in ExitProcess() anyway. */
				Writer_WorkerJoin();
			}
		} Initializer;
	}
}

////////////////////////////////////////////////////////////////////////////////
void Writer_Initialize(const FInitializeDesc& Desc)
{
	Writer_InitializeTail(Desc.TailSizeBytes);

	if (Desc.bUseWorkerThread)
	{
		Writer_WorkerCreate();
	}
}

////////////////////////////////////////////////////////////////////////////////
void Writer_Shutdown()
{
	Writer_InternalShutdown();
}

////////////////////////////////////////////////////////////////////////////////
void Writer_Update()
{
	if (!GWorkerThread)
	{
		Writer_WorkerUpdate();
	}

}



////////////////////////////////////////////////////////////////////////////////
bool Writer_SendTo(const ANSICHAR* Host, uint32 Port)
{
	if (GPendingDataHandle || GDataHandle)
	{
		return false;
	}

	Writer_InternalInitialize();

	Port = Port ? Port : 1981;
	UPTRINT DataHandle = TcpSocketConnect(Host, Port);
	if (!DataHandle)
	{
		return false;
	}

	GPendingDataHandle = DataHandle;
	return true;
}

////////////////////////////////////////////////////////////////////////////////
bool Writer_WriteTo(const ANSICHAR* Path)
{
	if (GPendingDataHandle || GDataHandle)
	{
		return false;
	}

	Writer_InternalInitialize();

	UPTRINT DataHandle = FileOpen(Path);
	if (!DataHandle)
	{
		return false;
	}

	GPendingDataHandle = DataHandle;
	return true;
}

////////////////////////////////////////////////////////////////////////////////
bool Writer_IsTracing()
{
	return GDataHandle != 0 || GPendingDataHandle != 0;
}

////////////////////////////////////////////////////////////////////////////////
bool Writer_Stop()
{
	if (GPendingDataHandle || !GDataHandle)
	{
		return false;
	}

	GPendingDataHandle = ~UPTRINT(0);
	return true;
}

} // namespace Private
} // namespace Trace
} // namespace UE

#endif // UE_TRACE_ENABLED
