// Copyright 2023 Andrei Victor. All rights reserved.


#include "UdtStatics.h"
#include "HeadMountedDisplayFunctionLibrary.h"

bool UUdtStatics::IsWithEditor()
{
#if WITH_EDITOR
	return true;
#else
	return false;
#endif //WITH_EDITOR
}

bool UUdtStatics::IsRunningPIE()
{
	return IsWithEditor();
}

bool UUdtStatics::IsShippingBuild()
{
#if UE_BUILD_SHIPPING
	return true;
#else
	return false;
#endif //UE_BUILD_SHIPPING
}

bool UUdtStatics::IsUsingHMD()
{
	return UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled()
		&& UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayConnected();
}
