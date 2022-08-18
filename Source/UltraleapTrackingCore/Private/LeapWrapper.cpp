/******************************************************************************
 * Copyright (C) Ultraleap, Inc. 2011-2021.                                   *
 *                                                                            *
 * Use subject to the terms of the Apache License 2.0 available at            *
 * http://www.apache.org/licenses/LICENSE-2.0, or another agreement           *
 * between Ultraleap and you, your company or other organization.             *
 ******************************************************************************/

#include "LeapWrapper.h"

#include "LeapAsync.h"
#include "LeapUtility.h"
#include "Runtime/Core/Public/Misc/Timespan.h"

// BEGIN YAAK PATCH

/*
	Leap likes to fire async tasks on game thread but doesn't care about context(this) state.
	Sometimes the task may be executed after the FLeapWrapper is already destroyed and cause crash.

	TaskRefPolicy = FLeapAsync::RunShortLambdaOnGameThread([CurrentPolicy, this]
	{
		// <this> may be invalid!
		this->CallbackDelegate->OnPolicy(CurrentPolicy);
	});
*/

#include <atomic>
static std::atomic<FLeapWrapper*> LeapContextAtomic;

inline void SetLeapContextPtr(FLeapWrapper* InContext)
{
	LeapContextAtomic.store(InContext);
}

inline bool IsLeapContextValid(FLeapWrapper* InContext)
{
	auto* CachedContext = LeapContextAtomic.load();
	return (InContext == CachedContext && InContext->GetCallbackDelegate());
}

// END YAAK PATCH

#pragma region LeapC Wrapper

FLeapWrapper::FLeapWrapper() : bIsRunning(false)
{
	UE_LOG(UltraleapTrackingLog, Log, TEXT("+FLeapWrapper"));

	InterpolatedFrame = nullptr;
	InterpolatedFrameSize = 0;
	DataLock = new FCriticalSection();
}

FLeapWrapper::~FLeapWrapper()
{
	UE_LOG(UltraleapTrackingLog, Log, TEXT("~FLeapWrapper"));

	delete DataLock;
	DataLock = nullptr;
	
	bIsRunning = false;

	SetLeapContextPtr(nullptr);
	CallbackDelegate = nullptr;

	LatestFrame = nullptr;
	ConnectionHandle = nullptr;

	if (bIsConnected)
	{
		CloseConnection();
	}
	if (ImageDescription != NULL)
	{
		if (ImageDescription->pBuffer != NULL)
		{
			free(ImageDescription->pBuffer);
		}
		delete ImageDescription;
	}
}

void FLeapWrapper::SetCallbackDelegate(LeapWrapperCallbackInterface* InCallbackDelegate)
{
	UE_LOG(UltraleapTrackingLog, Log, TEXT("SetCallbackDelegate %p"), InCallbackDelegate);
	CallbackDelegate = InCallbackDelegate;
	SetLeapContextPtr(this);
}

LEAP_CONNECTION* FLeapWrapper::OpenConnection(LeapWrapperCallbackInterface* InCallbackDelegate)
{
	SetCallbackDelegate(InCallbackDelegate);

	// Don't use config for now
	LEAP_CONNECTION_CONFIG Config;
	Config.server_namespace = "Leap Service";
	Config.size = sizeof(Config);
	Config.flags = 0;

	eLeapRS result = LeapCreateConnection(&Config, &ConnectionHandle);
	if (result == eLeapRS_Success)
	{
		result = LeapOpenConnection(ConnectionHandle);
		if (result == eLeapRS_Success)
		{
			bIsRunning = true;

			LEAP_CONNECTION* Handle = &ConnectionHandle;
			ProducerLambdaFuture = FLeapAsync::RunLambdaOnBackGroundThread([&, Handle] {
				UE_LOG(UltraleapTrackingLog, Log, TEXT("ServiceMessageLoop started."));
				ServiceMessageLoop();
				UE_LOG(UltraleapTrackingLog, Log, TEXT("ServiceMessageLoop stopped."));

				CloseConnectionHandle(Handle);
			});
		}
	}
	return &ConnectionHandle;
}

void FLeapWrapper::CloseConnection()
{
	UE_LOG(UltraleapTrackingLog, Log, TEXT("FLeapWrapper::CloseConnection"));

	if (!bIsConnected)
	{
		// Not connected, already done
		UE_LOG(UltraleapTrackingLog, Log, TEXT("Attempt at closing an already closed connection."));
		return;
	}
	bIsConnected = false;
	bIsRunning = false;
	CleanupLastDevice();

	// Wait for thread to exit - Blocking call, but it should be very quick.
	FTimespan ExitWaitTimeSpan = FTimespan::FromSeconds(3);

	ProducerLambdaFuture.WaitFor(ExitWaitTimeSpan);
	ProducerLambdaFuture.Reset();

	// Nullify the callback delegate. Any outstanding task graphs will not run if the delegate is nullified.
	UE_LOG(UltraleapTrackingLog, Log, TEXT("Reset CallbackDelegate"));
	CallbackDelegate = nullptr;

	UE_LOG(UltraleapTrackingLog, Log, TEXT("Connection successfully closed."));
}
void FLeapWrapper::SetTrackingMode(eLeapTrackingMode TrackingMode)
{
	eLeapRS Result = LeapSetTrackingMode(ConnectionHandle, TrackingMode);
	if (Result != eLeapRS_Success)
	{
		UE_LOG(UltraleapTrackingLog, Log, TEXT("SetTrackingMode failed in  FLeapWrapper::SetTrackingMode."));
	}
}
void FLeapWrapper::SetPolicy(int64 Flags, int64 ClearFlags)
{
	eLeapRS Result = LeapSetPolicyFlags(ConnectionHandle, Flags, ClearFlags);
	if (Result != eLeapRS_Success)
	{
		UE_LOG(UltraleapTrackingLog, Log, TEXT("LeapSetPolicyFlags failed in  FLeapWrapper::SetPolicy."));
	}
}

void FLeapWrapper::SetPolicyFlagFromBoolean(eLeapPolicyFlag Flag, bool ShouldSet)
{
	if (ShouldSet)
	{
		SetPolicy(Flag, 0);
	}
	else
	{
		SetPolicy(0, Flag);
	}
}

/** Close the connection and let message thread function end. */
void FLeapWrapper::CloseConnectionHandle(LEAP_CONNECTION* InConnectionHandle)
{
	bIsRunning = false;
	bIsConnected = false;
	LeapDestroyConnection(*InConnectionHandle);
}

LEAP_TRACKING_EVENT* FLeapWrapper::GetFrame()
{
	LEAP_TRACKING_EVENT* currentFrame;
	DataLock->Lock();
	currentFrame = LatestFrame;
	DataLock->Unlock();
	return currentFrame;
}

LEAP_TRACKING_EVENT* FLeapWrapper::GetInterpolatedFrameAtTime(int64 TimeStamp)
{
	uint64_t FrameSize = 0;
	LeapGetFrameSize(ConnectionHandle, TimeStamp, &FrameSize);

	// Check validity of frame size
	if (FrameSize > 0)
	{
		// Different frame?
		if (FrameSize != InterpolatedFrameSize)
		{
			// If we already have an allocated frame, free it
			if (InterpolatedFrame)
			{
				free(InterpolatedFrame);
			}
			InterpolatedFrame = (LEAP_TRACKING_EVENT*) malloc(FrameSize);
		}
		InterpolatedFrameSize = FrameSize;

		// Grab the new frame
		LeapInterpolateFrame(ConnectionHandle, TimeStamp, InterpolatedFrame, InterpolatedFrameSize);
	}

	return InterpolatedFrame;
}

LEAP_DEVICE_INFO* FLeapWrapper::GetDeviceProperties()
{
	LEAP_DEVICE_INFO* currentDevice;
	DataLock->Lock();
	currentDevice = CurrentDeviceInfo;
	DataLock->Unlock();
	return currentDevice;
}

const char* FLeapWrapper::ResultString(eLeapRS Result)
{
	switch (Result)
	{
		case eLeapRS_Success:
			return "eLeapRS_Success";
		case eLeapRS_UnknownError:
			return "eLeapRS_UnknownError";
		case eLeapRS_InvalidArgument:
			return "eLeapRS_InvalidArgument";
		case eLeapRS_InsufficientResources:
			return "eLeapRS_InsufficientResources";
		case eLeapRS_InsufficientBuffer:
			return "eLeapRS_InsufficientBuffer";
		case eLeapRS_Timeout:
			return "eLeapRS_Timeout";
		case eLeapRS_NotConnected:
			return "eLeapRS_NotConnected";
		case eLeapRS_HandshakeIncomplete:
			return "eLeapRS_HandshakeIncomplete";
		case eLeapRS_BufferSizeOverflow:
			return "eLeapRS_BufferSizeOverflow";
		case eLeapRS_ProtocolError:
			return "eLeapRS_ProtocolError";
		case eLeapRS_InvalidClientID:
			return "eLeapRS_InvalidClientID";
		case eLeapRS_UnexpectedClosed:
			return "eLeapRS_UnexpectedClosed";
		case eLeapRS_UnknownImageFrameRequest:
			return "eLeapRS_UnknownImageFrameRequest";
		case eLeapRS_UnknownTrackingFrameID:
			return "eLeapRS_UnknownTrackingFrameID";
		case eLeapRS_RoutineIsNotSeer:
			return "eLeapRS_RoutineIsNotSeer";
		case eLeapRS_TimestampTooEarly:
			return "eLeapRS_TimestampTooEarly";
		case eLeapRS_ConcurrentPoll:
			return "eLeapRS_ConcurrentPoll";
		case eLeapRS_NotAvailable:
			return "eLeapRS_NotAvailable";
		case eLeapRS_NotStreaming:
			return "eLeapRS_NotStreaming";
		case eLeapRS_CannotOpenDevice:
			return "eLeapRS_CannotOpenDevice";
		default:
			return "unknown result type.";
	}
}

void FLeapWrapper::EnableImageStream(bool bEnable)
{
	// TODO: test the image/buffer stream code

	if (ImageDescription == NULL)
	{
		ImageDescription = new LEAP_IMAGE_FRAME_DESCRIPTION;
		ImageDescription->pBuffer = NULL;
	}

	int OldLength = ImageDescription->buffer_len;

	// if the size is different realloc the buffer
	if (ImageDescription->buffer_len != OldLength)
	{
		if (ImageDescription->pBuffer != NULL)
		{
			free(ImageDescription->pBuffer);
		}
		ImageDescription->pBuffer = (void*) malloc(ImageDescription->buffer_len);
	}
}

void FLeapWrapper::Millisleep(int milliseconds)
{
	FPlatformProcess::Sleep(((float) milliseconds) / 1000.f);
}

void FLeapWrapper::SetDevice(const LEAP_DEVICE_INFO* DeviceProps)
{
	DataLock->Lock();
	if (CurrentDeviceInfo)
	{
		free(CurrentDeviceInfo->serial);
	}
	else
	{
		CurrentDeviceInfo = (LEAP_DEVICE_INFO*) malloc(sizeof(*DeviceProps));
	}
	*CurrentDeviceInfo = *DeviceProps;
	CurrentDeviceInfo->serial = (char*) malloc(DeviceProps->serial_length);
	memcpy(CurrentDeviceInfo->serial, DeviceProps->serial, DeviceProps->serial_length);
	DataLock->Unlock();
}

void FLeapWrapper::CleanupLastDevice()
{
	if (CurrentDeviceInfo)
	{
		free(CurrentDeviceInfo->serial);
	}
	CurrentDeviceInfo = nullptr;
}

void FLeapWrapper::SetFrame(const LEAP_TRACKING_EVENT* Frame)
{
	DataLock->Lock();

	if (!LatestFrame)
	{
		LatestFrame = (LEAP_TRACKING_EVENT*) malloc(sizeof(*Frame));
	}

	*LatestFrame = *Frame;

	DataLock->Unlock();
}

/** Called by ServiceMessageLoop() when a connection event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleConnectionEvent(const LEAP_CONNECTION_EVENT* ConnectionEvent)
{
	bIsConnected = true;
	if (CallbackDelegate)
	{
		CallbackDelegate->OnConnect();
	}
}

/** Called by ServiceMessageLoop() when a connection lost event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleConnectionLostEvent(const LEAP_CONNECTION_LOST_EVENT* ConnectionLostEvent)
{
	bIsConnected = false;
	CleanupLastDevice();

	if (CallbackDelegate)
	{
		CallbackDelegate->OnConnectionLost();
	}
}

/**
 * Called by ServiceMessageLoop() when a device event is returned by LeapPollConnection()
 */
void FLeapWrapper::HandleDeviceEvent(const LEAP_DEVICE_EVENT* DeviceEvent)
{
	// LEAP_DEVICE DeviceHandle;
	// Open device using LEAP_DEVICE_REF from event struct.
	eLeapRS Result = LeapOpenDevice(DeviceEvent->device, &DeviceHandle);
	if (Result != eLeapRS_Success)
	{
		UE_LOG(UltraleapTrackingLog, Warning, TEXT("Could not open device %s.\n"), ResultString(Result));
		return;
	}

	// Create a struct to hold the device properties, we have to provide a buffer for the serial string
	LEAP_DEVICE_INFO DeviceProperties = {sizeof(DeviceProperties)};
	// Start with a length of 1 (pretending we don't know a priori what the length is).
	// Currently device serial numbers are all the same length, but that could change in the future
	DeviceProperties.serial_length = 64;
	DeviceProperties.serial = (char*) malloc(DeviceProperties.serial_length);
	// This will fail since the serial buffer is only 1 character long
	// But deviceProperties is updated to contain the required buffer length
	Result = LeapGetDeviceInfo(DeviceHandle, &DeviceProperties);
	if (Result == eLeapRS_InsufficientBuffer)
	{
		// try again with correct buffer size
		free(DeviceProperties.serial);
		DeviceProperties.serial = (char*) malloc(DeviceProperties.serial_length);
		Result = LeapGetDeviceInfo(DeviceHandle, &DeviceProperties);
		if (Result != eLeapRS_Success)
		{
			printf("Failed to get device info %s.\n", ResultString(Result));
			free(DeviceProperties.serial);
			return;
		}
	}

	SetDevice(&DeviceProperties);

	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefDeviceFound = FLeapAsync::RunShortLambdaOnGameThread([DeviceEvent, DeviceProperties, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnDeviceFound(&DeviceProperties);
			}
				free(DeviceProperties.serial);
		});
	}
	else
	{
		free(DeviceProperties.serial);
	}

	LeapCloseDevice(DeviceHandle);
}

/** Called by ServiceMessageLoop() when a device lost event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleDeviceLostEvent(const LEAP_DEVICE_EVENT* DeviceEvent)
{
	// todo: remove device handles matched here
	// DeviceHandles.Remove(DeviceHandle);

	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefDeviceLost = FLeapAsync::RunShortLambdaOnGameThread([DeviceEvent, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnDeviceLost(Context->CurrentDeviceInfo->serial);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a device failure event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleDeviceFailureEvent(const LEAP_DEVICE_FAILURE_EVENT* DeviceFailureEvent)
{
	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefDeviceFailure = FLeapAsync::RunShortLambdaOnGameThread([DeviceFailureEvent, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnDeviceFailure(DeviceFailureEvent->status, DeviceFailureEvent->hDevice);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a tracking event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleTrackingEvent(const LEAP_TRACKING_EVENT* TrackingEvent)
{
	// temp disable
	/*if (DeviceId == 2) {
		return;
	}*/

	SetFrame(TrackingEvent);	// support polling tracking data from different thread

	// Callback delegate is checked twice since the second call happens on the second thread and may be invalidated!
	if (CallbackDelegate)
	{
		LeapWrapperCallbackInterface* SafeDelegate = CallbackDelegate;

		// Run this on bg thread still
		CallbackDelegate->OnFrame(TrackingEvent);
	}
}

void FLeapWrapper::HandleImageEvent(const LEAP_IMAGE_EVENT* ImageEvent)
{
	// Callback with data
	if (CallbackDelegate)
	{
		// Do image handling on background thread for performance
		CallbackDelegate->OnImage(ImageEvent);
	}
}

/** Called by ServiceMessageLoop() when a log event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleLogEvent(const LEAP_LOG_EVENT* LogEvent)
{
	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefLog = FLeapAsync::RunShortLambdaOnGameThread([LogEvent, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnLog(LogEvent->severity, LogEvent->timestamp, LogEvent->message);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a policy event is returned by LeapPollConnection(). */
void FLeapWrapper::HandlePolicyEvent(const LEAP_POLICY_EVENT* PolicyEvent)
{
	if (CallbackDelegate)
	{
		// this is always coming back as 0, this means either the Leap service refused to set any flags?
		// or there's a bug in the policy notification system with Leap Motion V4.
		const uint32_t CurrentPolicy = PolicyEvent->current_policy;
		auto* Context = this;
		TaskRefPolicy = FLeapAsync::RunShortLambdaOnGameThread([CurrentPolicy, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnPolicy(CurrentPolicy);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a policy event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleTrackingModeEvent(const LEAP_TRACKING_MODE_EVENT* TrackingModeEvent)
{
	if (CallbackDelegate)
	{
		// this is always coming back as 0, this means either the Leap service refused to set any flags?
		// or there's a bug in the policy notification system with Leap Motion V4.
		const uint32_t CurrentMode = TrackingModeEvent->current_tracking_mode;
		auto* Context = this;
		TaskRefPolicy = FLeapAsync::RunShortLambdaOnGameThread([CurrentMode, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnTrackingMode((eLeapTrackingMode) CurrentMode);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a config change event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleConfigChangeEvent(const LEAP_CONFIG_CHANGE_EVENT* ConfigChangeEvent)
{
	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefConfigChange = FLeapAsync::RunShortLambdaOnGameThread([ConfigChangeEvent, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnConfigChange(ConfigChangeEvent->requestID, ConfigChangeEvent->status);
			}
		});
	}
}

/** Called by ServiceMessageLoop() when a config response event is returned by LeapPollConnection(). */
void FLeapWrapper::HandleConfigResponseEvent(const LEAP_CONFIG_RESPONSE_EVENT* ConfigResponseEvent)
{
	if (CallbackDelegate)
	{
		auto* Context = this;
		TaskRefConfigResponse = FLeapAsync::RunShortLambdaOnGameThread([ConfigResponseEvent, Context] {
			if (IsLeapContextValid(Context))
			{
				Context->CallbackDelegate->OnConfigResponse(ConfigResponseEvent->requestID, ConfigResponseEvent->value);
			}
		});
	}
}

/**
 * Services the LeapC message pump by calling LeapPollConnection().
 * The average polling time is determined by the framerate of the Leap Motion service.
 */
void FLeapWrapper::ServiceMessageLoop(void* Unused)
{
	UE_LOG(UltraleapTrackingLog, Log, TEXT("ENTER ServiceMessageLoop"));

	eLeapRS Result;
	LEAP_CONNECTION_MESSAGE Msg;
	LEAP_CONNECTION Handle = ConnectionHandle;	  // copy handle so it doesn't get released from under us on game thread

	unsigned int Timeout = 200;
	while (bIsRunning)
	{
		Result = LeapPollConnection(Handle, Timeout, &Msg);

		// Polling may have taken some time, re-check exit condition
		if (!bIsRunning)
		{
			break;
		}

		if (Result != eLeapRS_Success)
		{
			// UE_LOG(UltraleapTrackingLog, Log, TEXT("LeapC PollConnection unsuccessful result %s.\n"),
			// UTF8_TO_TCHAR(ResultString(result)));
			if (!bIsConnected)
			{
				FPlatformProcess::Sleep(0.1f);
				continue;
			}
			else
			{
				continue;
			}
		}

		switch (Msg.type)
		{
			case eLeapEventType_Connection:
				HandleConnectionEvent(Msg.connection_event);
				break;
			case eLeapEventType_ConnectionLost:
				HandleConnectionLostEvent(Msg.connection_lost_event);
				break;
			case eLeapEventType_Device:
				HandleDeviceEvent(Msg.device_event);
				break;
			case eLeapEventType_DeviceLost:
				HandleDeviceLostEvent(Msg.device_event);
				break;
			case eLeapEventType_DeviceFailure:
				HandleDeviceFailureEvent(Msg.device_failure_event);
				break;
			case eLeapEventType_Tracking:
				HandleTrackingEvent(Msg.tracking_event);
				break;
			case eLeapEventType_Image:
				HandleImageEvent(Msg.image_event);
				break;
			case eLeapEventType_LogEvent:
				HandleLogEvent(Msg.log_event);
				break;
			case eLeapEventType_Policy:
				HandlePolicyEvent(Msg.policy_event);
				break;
			case eLeapEventType_TrackingMode:
				HandleTrackingModeEvent(Msg.tracking_mode_event);
				break;
			case eLeapEventType_ConfigChange:
				HandleConfigChangeEvent(Msg.config_change_event);
				break;
			case eLeapEventType_ConfigResponse:
				HandleConfigResponseEvent(Msg.config_response_event);
				break;
			default:
				// discard unknown message types
				// UE_LOG(UltraleapTrackingLog, Log, TEXT("Unhandled message type %i."), (int32)Msg.type);
				break;
		}	 // switch on msg.type
	}		 // end while running

	UE_LOG(UltraleapTrackingLog, Log, TEXT("LEAVE ServiceMessageLoop"));
}

#pragma endregion LeapC Wrapper