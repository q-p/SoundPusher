/*
     File: LoopbackAudio.c
 Abstract: Provides an input- and output-device, the output is transferred to the input.

 Originally from NullAudio.c

*/
/*==================================================================================================
	LoopbackAudio.c
==================================================================================================*/

//==================================================================================================
//	Includes
//==================================================================================================

#include <stdint.h>
#include <stdatomic.h>

//	System Includes
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <os/lock.h>

#include "LoopbackAudio.h"
#include "TPCircularBuffer.h"

//==================================================================================================
#pragma mark -
#pragma mark Macros
//==================================================================================================

// Whether we allow multiple formats for the device that can be switched between. This may be
// interesting when we change the output format to one that best matches the input format for our
// selected encoder
//#define ALLOW_MULTIPLE_FORMATS

#if DEBUG
	#include <os/log.h>

	#define	DebugMsg(Type, inFormat, ...)	os_log_with_type(OS_LOG_DEFAULT, Type, inFormat, ## __VA_ARGS__)

	#define	FailIf(inCondition, inHandler, inMessage)									\
			if(inCondition)																\
			{																			\
				DebugMsg(OS_LOG_TYPE_ERROR, inMessage);									\
				goto inHandler;															\
			}

	#define	FailWithAction(inCondition, inAction, inHandler, inMessage)					\
			if(inCondition)																\
			{																			\
				DebugMsg(OS_LOG_TYPE_ERROR, inMessage);									\
				{ inAction; }															\
				goto inHandler;															\
			}

#else

	#define	DebugMsg(inFormat, ...)
	
	#define	FailIf(inCondition, inHandler, inMessage)									\
			if(inCondition)																\
			{																			\
				goto inHandler;															\
			}

	#define	FailWithAction(inCondition, inAction, inHandler, inMessage)					\
			if(inCondition)																\
			{																			\
				{ inAction; }															\
				goto inHandler;															\
			}

#endif

//==================================================================================================
#pragma mark -
#pragma mark LoopbackAudio State
//==================================================================================================

//	Declare the internal object ID numbers for all the objects this driver implements. Note that
//	because the driver has fixed set of objects that never grows or shrinks. If this were not the
//	case, the driver would need to have a means to dynamically allocate these IDs. It is important
//	to realize that a lot of the structure of this driver is vastly simpler when the IDs are all
//	known a priori. Comments in the code will try to identify some of these simplifications and
//	point out what a more complicated driver will need to do.
enum
{
	kObjectID_PlugIn					= kAudioObjectPlugInObject,
	kObjectID_Device					= 2,
	kObjectID_Stream_Input				= 3,
	kObjectID_Stream_Output				= 4,
	kObjectID_Mute_Output_Master		= 5
};

enum
{
	kChangeRequest_StreamFormat			= 1,
//	kChangeRequest_ChannelLayout		= 2,
};

#ifdef ALLOW_MULTIPLE_FORMATS
static const Float64 kSupportedSampleRates[] = {32000.0, 44100.0, 48000.0};
static const UInt32 kSupportedNumChannels[] = {1u, 2u, 3u, 4u, 5u, 6u};
#else
static const Float64 kSupportedSampleRates[] = {48000.0};
static const UInt32 kSupportedNumChannels[] = {6u};
#endif

enum
{
	kNumSupportedChannels		= sizeof kSupportedNumChannels / sizeof *kSupportedNumChannels,
	kNumSupportedSampleRates	= sizeof kSupportedSampleRates / sizeof *kSupportedSampleRates,

	// The size of the imaginary ring-buffer used purely for timing purposes (but which has an impact on
	// what IO size CoreAudio chooses)
	kDevice_NumZeroFrames	    = 2048,

	// The size of the *actual* ring-buffer in frames
	// The size of this is independent of the imaginary hardware ring-buffer providing zero time-stamps
	kDevice_RingBuffNumFrames	= kDevice_NumZeroFrames,
};

//	Declare the stuff that tracks the state of the plug-in, the device and its sub-objects.
//	Note that we use global variables here because this driver only ever has a single device. If
//	multiple devices were supported, this state would need to be encapsulated in one or more structs
//	so that each object's state can be tracked individually.
#define							kPlugIn_BundleID				"de.maven.audio.LoopbackAudio"
static UInt32					gPlugIn_RefCount				= 0;
static AudioServerPlugInHostRef	gPlugIn_Host					= NULL;
static pthread_mutex_t			gPlugIn_StateMutex				= PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
	// Used for longer locks
	pthread_mutex_t				Mutex;
	// Used for the IO operations (essentially the buffer) and GetZeroTimeStamp
	os_unfair_lock				UnfairLock;

	AudioChannelLayout			CurrentChannelLayout;
	AudioStreamBasicDescription	CurrentFormat;

	UInt32						IOIsRunning;
	Float64						HostTicksPerFrame;
	UInt64						NumberTimeStamps;
	Float64						AnchorSampleTime;
	UInt64						AnchorHostTime;
	UInt64						TimeLineSeed;

	// We only use the TPCircularBuffer for it's memory-mapping trickery, and write / read from our
	// computed positions without consuming / producing
	TPCircularBuffer			RingBuffer;

	// One past the last frame (not modulo buffer size) we've written
	_Atomic SInt64				LastWriteEndInFrames;
	// The sliding difference between where we read and write (usually positive because output happens at a later time than input for the same set of buffers)
	_Atomic SInt64				ReadOffsetInFrames;

	bool						Stream_Input_IsActive;
	bool						Stream_Output_IsActive;
	bool						Stream_Output_Master_Mute;
} Device;

static Device gDevice = {
	.Mutex						= PTHREAD_MUTEX_INITIALIZER,
	.UnfairLock					= OS_UNFAIR_LOCK_INIT,
	.CurrentChannelLayout		= {
		.mChannelLayoutTag			= kAudioChannelLayoutTag_AudioUnit_5_1,
		.mChannelBitmap				= 0,
		.mNumberChannelDescriptions	= 0,
	},
	.CurrentFormat				= {
		.mSampleRate		= 48000.0,
		.mFormatID			= kAudioFormatLinearPCM,
		.mFormatFlags		= kAudioFormatFlagsNativeFloatPacked,
		.mBytesPerPacket	= sizeof(float) * 6 /* channels */,
		.mFramesPerPacket	= 1,
		.mBytesPerFrame		= sizeof(float) * 6 /* channels */,
		.mChannelsPerFrame	= 6,
		.mBitsPerChannel	= sizeof(float) * 8,
		.mReserved			= 0,
	},
	.IOIsRunning				= 0,
	.HostTicksPerFrame			= 0.0,
	.NumberTimeStamps			= 0,
	.AnchorSampleTime			= 0.0,
	.AnchorHostTime				= 0,
	.TimeLineSeed				= 1,
	.LastWriteEndInFrames		= ATOMIC_VAR_INIT(0),
	.ReadOffsetInFrames			= ATOMIC_VAR_INIT(0),
	.Stream_Input_IsActive		= true,
	.Stream_Output_IsActive		= true,
	.Stream_Output_Master_Mute	= false,
};


static AudioChannelLayoutTag GetDefaultChannelLayout(UInt32 numChannels)
{
	switch (numChannels)
	{
		case 1:
			return kAudioChannelLayoutTag_Mono; // C
		case 2:
			return kAudioChannelLayoutTag_Stereo; // L R
		case 3:
			return kAudioChannelLayoutTag_MPEG_3_0_A; // L R C
		case 4:
			return kAudioChannelLayoutTag_AudioUnit_4; // L R Ls Rs
		case 5:
			return kAudioChannelLayoutTag_AudioUnit_5_0; // L R Ls Rs C
		case 6:
			return kAudioChannelLayoutTag_AudioUnit_5_1; // L R C LFE Ls Rs
	}
	return kAudioChannelLayoutTag_Unknown;
}

static void GetPreferredStereoChannels(AudioChannelLayoutTag tag, UInt32 outLeftAndRight[2])
{
	switch (tag)
	{
		case kAudioChannelLayoutTag_Mono:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 1;
			break;
		case kAudioChannelLayoutTag_Stereo:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 2;
			break;
		case kAudioChannelLayoutTag_MPEG_3_0_A:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 2;
			break;
		case kAudioChannelLayoutTag_AudioUnit_4:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 2;
			break;
		case kAudioChannelLayoutTag_AudioUnit_5_0:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 2;
			break;
		case kAudioChannelLayoutTag_AudioUnit_5_1:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = 2;
			break;
		default:
			outLeftAndRight[0] = 1;
			outLeftAndRight[1] = AudioChannelLayoutTag_GetNumberOfChannels(tag) > 1 ? 2 : 1;
			break;
	}
}

static void DescribeChannelLayout(AudioChannelLayoutTag tag, AudioChannelLayout *outLayout)
{
	outLayout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
	outLayout->mNumberChannelDescriptions = AudioChannelLayoutTag_GetNumberOfChannels(tag);
	
	switch(tag)
	{
		case kAudioChannelLayoutTag_Mono:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
			break;
		case kAudioChannelLayoutTag_Stereo:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			outLayout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			break;
		case kAudioChannelLayoutTag_MPEG_3_0_A:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			outLayout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			outLayout->mChannelDescriptions[2].mChannelLabel = kAudioChannelLabel_Center;
			break;
		case kAudioChannelLayoutTag_AudioUnit_4:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			outLayout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			outLayout->mChannelDescriptions[2].mChannelLabel = kAudioChannelLabel_LeftSurround;
			outLayout->mChannelDescriptions[3].mChannelLabel = kAudioChannelLabel_RightSurround;
			break;
		case kAudioChannelLayoutTag_AudioUnit_5_0:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			outLayout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			outLayout->mChannelDescriptions[2].mChannelLabel = kAudioChannelLabel_LeftSurround;
			outLayout->mChannelDescriptions[3].mChannelLabel = kAudioChannelLabel_RightSurround;
			outLayout->mChannelDescriptions[4].mChannelLabel = kAudioChannelLabel_Center;
			break;
		case kAudioChannelLayoutTag_AudioUnit_5_1:
			outLayout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			outLayout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			outLayout->mChannelDescriptions[2].mChannelLabel = kAudioChannelLabel_Center;
			outLayout->mChannelDescriptions[3].mChannelLabel = kAudioChannelLabel_LFEScreen;
			outLayout->mChannelDescriptions[4].mChannelLabel = kAudioChannelLabel_LeftSurround;
			outLayout->mChannelDescriptions[5].mChannelLabel = kAudioChannelLabel_RightSurround;
			break;
	}
}

//==================================================================================================
#pragma mark -
#pragma mark AudioServerPlugInDriverInterface Implementation
//==================================================================================================

#pragma mark Prototypes

//	Entry points for the COM methods
void*				LoopbackAudio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
static HRESULT		LoopbackAudio_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG		LoopbackAudio_AddRef(void* inDriver);
static ULONG		LoopbackAudio_Release(void* inDriver);
static OSStatus		LoopbackAudio_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus		LoopbackAudio_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus		LoopbackAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus		LoopbackAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus		LoopbackAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus		LoopbackAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus		LoopbackAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean		LoopbackAudio_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus		LoopbackAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus		LoopbackAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus		LoopbackAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus		LoopbackAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus		LoopbackAudio_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus		LoopbackAudio_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus		LoopbackAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus		LoopbackAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus		LoopbackAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus		LoopbackAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus		LoopbackAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

//	Implementation
static Boolean		LoopbackAudio_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus		LoopbackAudio_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus		LoopbackAudio_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus		LoopbackAudio_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus		LoopbackAudio_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean		LoopbackAudio_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus		LoopbackAudio_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus		LoopbackAudio_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus		LoopbackAudio_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus		LoopbackAudio_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean		LoopbackAudio_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus		LoopbackAudio_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus		LoopbackAudio_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus		LoopbackAudio_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus		LoopbackAudio_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

static Boolean		LoopbackAudio_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus		LoopbackAudio_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus		LoopbackAudio_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus		LoopbackAudio_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus		LoopbackAudio_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]);

#pragma mark The Interface

static AudioServerPlugInDriverInterface	gAudioServerPlugInDriverInterface =
{
	NULL,
	LoopbackAudio_QueryInterface,
	LoopbackAudio_AddRef,
	LoopbackAudio_Release,
	LoopbackAudio_Initialize,
	LoopbackAudio_CreateDevice,
	LoopbackAudio_DestroyDevice,
	LoopbackAudio_AddDeviceClient,
	LoopbackAudio_RemoveDeviceClient,
	LoopbackAudio_PerformDeviceConfigurationChange,
	LoopbackAudio_AbortDeviceConfigurationChange,
	LoopbackAudio_HasProperty,
	LoopbackAudio_IsPropertySettable,
	LoopbackAudio_GetPropertyDataSize,
	LoopbackAudio_GetPropertyData,
	LoopbackAudio_SetPropertyData,
	LoopbackAudio_StartIO,
	LoopbackAudio_StopIO,
	LoopbackAudio_GetZeroTimeStamp,
	LoopbackAudio_WillDoIOOperation,
	LoopbackAudio_BeginIOOperation,
	LoopbackAudio_DoIOOperation,
	LoopbackAudio_EndIOOperation
};
static AudioServerPlugInDriverInterface*	gAudioServerPlugInDriverInterfacePtr	= &gAudioServerPlugInDriverInterface;
static AudioServerPlugInDriverRef			gAudioServerPlugInDriverRef				= &gAudioServerPlugInDriverInterfacePtr;

#pragma mark Factory

void*	LoopbackAudio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
	//	This is the CFPlugIn factory function. Its job is to create the implementation for the given
	//	type provided that the type is supported. Because this driver is simple and all its
	//	initialization is handled via static iniitalization when the bundle is loaded, all that
	//	needs to be done is to return the AudioServerPlugInDriverRef that points to the driver's
	//	interface. A more complicated driver would create any base line objects it needs to satisfy
	//	the IUnknown methods that are used to discover that actual interface to talk to the driver.
	//	The majority of the driver's initilization should be handled in the Initialize() method of
	//	the driver's AudioServerPlugInDriverInterface.
	
	#pragma unused(inAllocator)
    void* theAnswer = NULL;
    if(CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
    {
		theAnswer = gAudioServerPlugInDriverRef;
    }
    return theAnswer;
}

#pragma mark Inheritance

static HRESULT	LoopbackAudio_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
	//	This function is called by the HAL to get the interface to talk to the plug-in through.
	//	AudioServerPlugIns are required to support the IUnknown interface and the
	//	AudioServerPlugInDriverInterface. As it happens, all interfaces must also provide the
	//	IUnknown interface, so we can always just return the single interface we made with
	//	gAudioServerPlugInDriverInterfacePtr regardless of which one is asked for.

	//	declare the local variables
	HRESULT theAnswer = 0;
	CFUUIDRef theRequestedUUID = NULL;
	
	//	validate the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_QueryInterface: bad driver reference");
	FailWithAction(outInterface == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_QueryInterface: no place to store the returned interface");

	//	make a CFUUIDRef from inUUID
	theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
	FailWithAction(theRequestedUUID == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_QueryInterface: failed to create the CFUUIDRef");

	//	AudioServerPlugIns only support two interfaces, IUnknown (which has to be supported by all
	//	CFPlugIns and AudioServerPlugInDriverInterface (which is the actual interface the HAL will
	//	use).
	if(CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID))
	{
		pthread_mutex_lock(&gPlugIn_StateMutex);
		++gPlugIn_RefCount;
		pthread_mutex_unlock(&gPlugIn_StateMutex);
		*outInterface = gAudioServerPlugInDriverRef;
	}
	else
	{
		theAnswer = E_NOINTERFACE;
	}
	
	//	make sure to release the UUID we created
	CFRelease(theRequestedUUID);
		
Done:
	return theAnswer;
}

static ULONG	LoopbackAudio_AddRef(void* inDriver)
{
	//	This call returns the resulting reference count after the increment.
	
	//	declare the local variables
	ULONG theAnswer = 0;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_AddRef: bad driver reference");

	//	increment the refcount
	pthread_mutex_lock(&gPlugIn_StateMutex);
	if(gPlugIn_RefCount < UINT32_MAX)
	{
		++gPlugIn_RefCount;
	}
	theAnswer = gPlugIn_RefCount;
	pthread_mutex_unlock(&gPlugIn_StateMutex);

Done:
	return theAnswer;
}

static ULONG	LoopbackAudio_Release(void* inDriver)
{
	//	This call returns the resulting reference count after the decrement.

	//	declare the local variables
	ULONG theAnswer = 0;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_Release: bad driver reference");

	//	decrement the refcount
	pthread_mutex_lock(&gPlugIn_StateMutex);
	if(gPlugIn_RefCount > 0)
	{
		--gPlugIn_RefCount;
		//	Note that we don't do anything special if the refcount goes to zero as the HAL
		//	will never fully release a plug-in it opens. We keep managing the refcount so that
		//	the API semantics are correct though.
	}
	theAnswer = gPlugIn_RefCount;
	pthread_mutex_unlock(&gPlugIn_StateMutex);

Done:
	return theAnswer;
}

#pragma mark Basic Operations

static OSStatus	LoopbackAudio_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
	//	The job of this method is, as the name implies, to get the driver initialized. One specific
	//	thing that needs to be done is to store the AudioServerPlugInHostRef so that it can be used
	//	later. Note that when this call returns, the HAL will scan the various lists the driver
	//	maintains (such as the device list) to get the inital set of objects the driver is
	//	publishing. So, there is no need to notifiy the HAL about any objects created as part of the
	//	execution of this method.

	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_Initialize: bad driver reference");
	
	//	store the AudioServerPlugInHostRef
	gPlugIn_Host = inHost;
	
	//	calculate the host ticks per frame
	struct mach_timebase_info theTimeBaseInfo;
	mach_timebase_info(&theTimeBaseInfo);
	Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
	theHostClockFrequency *= 1000000000.0;
	gDevice.HostTicksPerFrame = theHostClockFrequency / gDevice.CurrentFormat.mSampleRate;
	
Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{
	//	This method is used to tell a driver that implements the Transport Manager semantics to
	//	create an AudioEndpointDevice from a set of AudioEndpoints. Since this driver is not a
	//	Transport Manager, we just check the arguments and return
	//	kAudioHardwareUnsupportedOperationError.
	
	#pragma unused(inDescription, inClientInfo, outDeviceObjectID)
	
	//	declare the local variables
	OSStatus theAnswer = kAudioHardwareUnsupportedOperationError;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_CreateDevice: bad driver reference");

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
	//	This method is used to tell a driver that implements the Transport Manager semantics to
	//	destroy an AudioEndpointDevice. Since this driver is not a Transport Manager, we just check
	//	the arguments and return kAudioHardwareUnsupportedOperationError.
	
	#pragma unused(inDeviceObjectID)
	
	//	declare the local variables
	OSStatus theAnswer = kAudioHardwareUnsupportedOperationError;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_DestroyDevice: bad driver reference");

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
	//	This method is used to inform the driver about a new client that is using the given device.
	//	This allows the device to act differently depending on who the client is. This driver does
	//	not need to track the clients using the device, so we just check the arguments and return
	//	successfully.
	
	#pragma unused(inClientInfo)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_AddDeviceClient: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_AddDeviceClient: bad device ID");

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
	//	This method is used to inform the driver about a client that is no longer using the given
	//	device. This driver does not track clients, so we just check the arguments and return
	//	successfully.
	
	#pragma unused(inClientInfo)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_RemoveDeviceClient: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_RemoveDeviceClient: bad device ID");

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
	//	This method is called to tell the device that it can perform the configuation change that it
	//	had requested via a call to the host method, RequestDeviceConfigurationChange(). The
	//	arguments, inChangeAction and inChangeInfo are the same as what was passed to
	//	RequestDeviceConfigurationChange().
	//
	//	The HAL guarantees that IO will be stopped while this method is in progress. The HAL will
	//	also handle figuring out exactly what changed for the non-control related properties. This
	//	means that the only notifications that would need to be sent here would be for either
	//	custom properties the HAL doesn't know about or for controls.
	//
	//	For the device implemented by this driver, only sample rate changes go through this process
	//	as it is the only state that can be changed for the device that isn't a control. For this
	//	change, the new sample rate is passed in the inChangeAction argument.
	
	#pragma unused(inChangeInfo)

	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_PerformDeviceConfigurationChange: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_PerformDeviceConfigurationChange: bad device ID");

	FailWithAction((inChangeAction != kChangeRequest_StreamFormat), theAnswer = kAudioHardwareBadObjectError, Done, "Loopback_PerformDeviceConfigurationChange: bad change request");

	//	lock the state mutex
	pthread_mutex_lock(&gDevice.Mutex);
	
	switch (inChangeAction)
	{
		case kChangeRequest_StreamFormat:
		{
			AudioStreamBasicDescription *newFormat = (AudioStreamBasicDescription *)inChangeInfo;
			
			gDevice.CurrentFormat = *newFormat;
			
			// recalculate the state that depends on the sample rate
			struct mach_timebase_info theTimeBaseInfo;
			mach_timebase_info(&theTimeBaseInfo);
			Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer;
			theHostClockFrequency *= 1000000000.0;
			gDevice.HostTicksPerFrame = theHostClockFrequency / gDevice.CurrentFormat.mSampleRate;
			
			gDevice.CurrentChannelLayout.mChannelLayoutTag = GetDefaultChannelLayout(gDevice.CurrentFormat.mChannelsPerFrame);
			
			free(newFormat);
			break;
		}
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	}

	//	unlock the state mutex
	pthread_mutex_unlock(&gDevice.Mutex);
	
Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
	//	This method is called to tell the driver that a request for a config change has been denied.
	//	This provides the driver an opportunity to clean up any state associated with the request.
	//	For this driver, an aborted config change requires no action. So we just check the arguments
	//	and return

	#pragma unused(inChangeAction, inChangeInfo)

	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_PerformDeviceConfigurationChange: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_PerformDeviceConfigurationChange: bad device ID");

Done:
	return theAnswer;
}

#pragma mark Property Operations

static Boolean	LoopbackAudio_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_HasProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "LoopbackAudio_HasProperty: no address");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = LoopbackAudio_HasPlugInProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
		
		case kObjectID_Device:
			theAnswer = LoopbackAudio_HasDeviceProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
		
		case kObjectID_Stream_Input:
		case kObjectID_Stream_Output:
			theAnswer = LoopbackAudio_HasStreamProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
		
		case kObjectID_Mute_Output_Master:
			theAnswer = LoopbackAudio_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress);
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsPropertySettable: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = LoopbackAudio_IsPlugInPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;
		
		case kObjectID_Device:
			theAnswer = LoopbackAudio_IsDevicePropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;
		
		case kObjectID_Stream_Input:
		case kObjectID_Stream_Output:
			theAnswer = LoopbackAudio_IsStreamPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;
		
		case kObjectID_Mute_Output_Master:
			theAnswer = LoopbackAudio_IsControlPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
			break;
				
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPropertyDataSize: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = LoopbackAudio_GetPlugInPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;
		
		case kObjectID_Device:
			theAnswer = LoopbackAudio_GetDevicePropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;
		
		case kObjectID_Stream_Input:
		case kObjectID_Stream_Output:
			theAnswer = LoopbackAudio_GetStreamPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;
		
		case kObjectID_Mute_Output_Master:
			theAnswer = LoopbackAudio_GetControlPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
			break;
				
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPropertyData: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = LoopbackAudio_GetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
		
		case kObjectID_Device:
			theAnswer = LoopbackAudio_GetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
		
		case kObjectID_Stream_Input:
		case kObjectID_Stream_Output:
			theAnswer = LoopbackAudio_GetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
		
		case kObjectID_Mute_Output_Master:
			theAnswer = LoopbackAudio_GetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
			break;
				
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberPropertiesChanged = 0;
	AudioObjectPropertyAddress theChangedAddresses[2];
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetPropertyData: no address");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_PlugIn:
			theAnswer = LoopbackAudio_SetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;
		
		case kObjectID_Device:
			theAnswer = LoopbackAudio_SetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;
		
		case kObjectID_Stream_Input:
		case kObjectID_Stream_Output:
			theAnswer = LoopbackAudio_SetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;
		
		case kObjectID_Mute_Output_Master:
			theAnswer = LoopbackAudio_SetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
			break;
				
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

	//	send any notifications
	if(theNumberPropertiesChanged > 0)
	{
		gPlugIn_Host->PropertiesChanged(gPlugIn_Host, inObjectID, theNumberPropertiesChanged, theChangedAddresses);
	}

Done:
	return theAnswer;
}

#pragma mark PlugIn Property Operations

static Boolean	LoopbackAudio_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the plug-in object has the given property.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_HasPlugInProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "LoopbackAudio_HasPlugInProperty: no address");
	FailIf(inObjectID != kObjectID_PlugIn, Done, "LoopbackAudio_HasPlugInProperty: not the plug-in object");
	
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPlugInPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
		case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyResourceBundle:
			theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the plug-in object can have its
	//	value changed.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsPlugInPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsPlugInPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsPlugInPropertySettable: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsPlugInPropertySettable: not the plug-in object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPlugInPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
		case kAudioPlugInPropertyTranslateUIDToDevice:
		case kAudioPlugInPropertyResourceBundle:
			*outIsSettable = false;
			break;
		
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.
	
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPlugInPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPlugInPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPlugInPropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPlugInPropertyDataSize: not the plug-in object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPlugInPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioObjectPropertyManufacturer:
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioPlugInPropertyTranslateUIDToDevice:
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioPlugInPropertyResourceBundle:
			*outDataSize = sizeof(CFStringRef);
			break;
			
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPlugInPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPlugInPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPlugInPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetPlugInPropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetPlugInPropertyData: not the plug-in object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioPlugInClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the plug-in");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			//	The class is always kAudioPlugInClassID for regular drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the plug-in");
			*((AudioClassID*)outData) = kAudioPlugInClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			//	The plug-in doesn't have an owning object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the plug-in");
			*((AudioObjectID*)outData) = kAudioObjectUnknown;
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the plug-in");
			*((CFStringRef*)outData) = CFSTR("ManufacturerName");
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
			//	This returns the objects directly owned by the object. In the case of the
			//	plug-in object it is the same as the device list.
		case kAudioPlugInPropertyDeviceList:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	Clamp that to the number of devices this driver implements (which is just 1)
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}
			
			//	Write the devices' object IDs into the return value
			if(theNumberItemsToFetch > 0)
			{
				((AudioObjectID*)outData)[0] = kObjectID_Device;
			}

			//	Return how many bytes we wrote to
			*outDataSize = theNumberItemsToFetch * sizeof(AudioClassID);
			break;
			
		case kAudioPlugInPropertyTranslateUIDToDevice:
			//	This property takes the CFString passed in the qualifier and converts that
			//	to the object ID of the device it corresponds to. For this driver, there is
			//	just the one device. Note that it is not an error if the string in the
			//	qualifier doesn't match any devices. In such case, kAudioObjectUnknown is
			//	the object ID to return.
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyTranslateUIDToDevice");
			FailWithAction(inQualifierDataSize == sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: the qualifier is the wrong size for kAudioPlugInPropertyTranslateUIDToDevice");
			FailWithAction(inQualifierData == NULL, theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: no qualifier for kAudioPlugInPropertyTranslateUIDToDevice");
			if(CFStringCompare(*((CFStringRef*)inQualifierData), CFSTR(kLoopbackAudioDevice_UID), 0) == kCFCompareEqualTo)
			{
				*((AudioObjectID*)outData) = kObjectID_Device;
			}
			else
			{
				*((AudioObjectID*)outData) = kAudioObjectUnknown;
			}
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioPlugInPropertyResourceBundle:
			//	The resource bundle is a path relative to the path of the plug-in's bundle.
			//	To specify that the plug-in bundle itself should be used, we just return the
			//	empty string.
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyResourceBundle");
			*((CFStringRef*)outData) = CFSTR("");
			*outDataSize = sizeof(CFStringRef);
			break;
			
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetPlugInPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetPlugInPropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetPlugInPropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetPlugInPropertyData: no place to return the properties that changed");
	FailWithAction(inObjectID != kObjectID_PlugIn, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetPlugInPropertyData: not the plug-in object");
	
	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetPlugInPropertyData() method.
	switch(inAddress->mSelector)
	{
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Device Property Operations

static Boolean	LoopbackAudio_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_HasDeviceProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "LoopbackAudio_HasDeviceProperty: no address");
	FailIf(inObjectID != kObjectID_Device, Done, "LoopbackAudio_HasDeviceProperty: not the device object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetDevicePropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
			break;

		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			theAnswer = ((inAddress->mScope == kAudioObjectPropertyScopeGlobal) || (inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput)) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
			break;
			
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
//		case kAudioDevicePropertyLatency:
//		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
			theAnswer = ((inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput)) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsDevicePropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsDevicePropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsDevicePropertySettable: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsDevicePropertySettable: not the device object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetDevicePropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyRelatedDevices:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertyStreams:
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyAvailableNominalSampleRates:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyPreferredChannelsForStereo:
		case kAudioDevicePropertyPreferredChannelLayout:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			*outIsSettable = false;
			break;
		
		case kAudioDevicePropertyNominalSampleRate:
			*outIsSettable = true;
			break;
		
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.
	
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetDevicePropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetDevicePropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetDevicePropertyDataSize: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetDevicePropertyDataSize: not the device object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetDevicePropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioObjectPropertyName:
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyManufacturer:
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
			switch(inAddress->mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					*outDataSize = 3 * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeInput:
					*outDataSize = 1 * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeOutput:
					*outDataSize = 2 * sizeof(AudioObjectID);
					break;
			};
			break;

		case kAudioDevicePropertyDeviceUID:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			switch(inAddress->mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					*outDataSize = 2 * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeInput:
					*outDataSize = 1 * sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyScopeOutput:
					*outDataSize = 1 * sizeof(AudioObjectID);
					break;
			};
			break;

		case kAudioObjectPropertyControlList:
			*outDataSize = 1 * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertySafetyOffset:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			*outDataSize = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			*outDataSize = kNumSupportedSampleRates * sizeof(AudioValueRange);
			break;
		
		case kAudioDevicePropertyIsHidden:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			*outDataSize = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			*outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (gDevice.CurrentFormat.mChannelsPerFrame * sizeof(AudioChannelDescription));
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			*outDataSize = sizeof(UInt32);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	UInt32 theItemIndex;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetDevicePropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetDevicePropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetDevicePropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetDevicePropertyData: no place to put the return value");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetDevicePropertyData: not the device object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioDeviceClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the device");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			//	The class is always kAudioDeviceClassID for devices created by drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyClass for the device");
			*((AudioClassID*)outData) = kAudioDeviceClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			//	The device's owner is the plug-in object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the device");
			*((AudioObjectID*)outData) = kObjectID_PlugIn;
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioObjectPropertyName:
			//	This is the human readable name of the device.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			*((CFStringRef*)outData) = CFSTR("DeviceName");
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyManufacturer:
			//	This is the human readable name of the maker of the plug-in.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
			*((CFStringRef*)outData) = CFSTR("ManufacturerName");
			*outDataSize = sizeof(CFStringRef);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	The device owns its streams and controls. Note that what is returned here
			//	depends on the scope requested.
			switch(inAddress->mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all objects
					if(theNumberItemsToFetch > 3)
					{
						theNumberItemsToFetch = 3;
					}
					
					//	fill out the list with as many objects as requested, which is everything
					for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
					{
						((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
					}
					break;
					
				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > 1)
					{
						theNumberItemsToFetch = 1;
					}
					
					//	fill out the list with the right objects
					for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
					{
						((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex;
					}
					break;
					
				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > 2)
					{
						theNumberItemsToFetch = 2;
					}
					
					//	fill out the list with the right objects
					for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
					{
						((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Output + theItemIndex;
					}
					break;
			};
			
			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyDeviceUID:
			//	This is a CFString that is a persistent token that can identify the same
			//	audio device across boot sessions. Note that two instances of the same
			//	device must have different values for this property.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
			*((CFStringRef*)outData) = CFSTR(kLoopbackAudioDevice_UID);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyModelUID:
			//	This is a CFString that is a persistent token that can identify audio
			//	devices that are the same kind of device. Note that two instances of the
			//	save device must have the same value for this property.
			FailWithAction(inDataSize < sizeof(CFStringRef), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
			*((CFStringRef*)outData) = CFSTR(kDevice_ModelUID);
			*outDataSize = sizeof(CFStringRef);
			break;

		case kAudioDevicePropertyTransportType:
			//	This value represents how the device is attached to the system. This can be
			//	any 32 bit integer, but common values for this property are defined in
			//	<CoreAudio/AudioHardwareBase.h>
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the device");
			*((UInt32*)outData) = kAudioDeviceTransportTypeVirtual;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyRelatedDevices:
			//	The related devices property identifys device objects that are very closely
			//	related. Generally, this is for relating devices that are packaged together
			//	in the hardware such as when the input side and the output side of a piece
			//	of hardware can be clocked separately and therefore need to be represented
			//	as separate AudioDevice objects. In such case, both devices would report
			//	that they are related to each other. Note that at minimum, a device is
			//	related to itself, so this list will always be at least one item long.

			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	we only have the one device...
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}
			
			//	Write the devices' object IDs into the return value
			if(theNumberItemsToFetch > 0)
			{
				((AudioObjectID*)outData)[0] = kObjectID_Device;
			}
			
			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertyClockDomain:
			//	This property allows the device to declare what other devices it is
			//	synchronized with in hardware. The way it works is that if two devices have
			//	the same value for this property and the value is not zero, then the two
			//	devices are synchronized in hardware. Note that a device that either can't
			//	be synchronized with others or doesn't know should return 0 for this
			//	property.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyClockDomain for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsAlive:
			//	This property returns whether or not the device is alive. Note that it is
			//	note uncommon for a device to be dead but still momentarily availble in the
			//	device list. In the case of this device, it will always be alive.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsAlive for the device");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceIsRunning:
			//	This property returns whether or not IO is running for the device. Note that
			//	we need to take both the state lock to check this value for thread safety.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");
			pthread_mutex_lock(&gDevice.Mutex);
			*((UInt32*)outData) = ((gDevice.IOIsRunning > 0) > 0) ? 1 : 0;
			pthread_mutex_unlock(&gDevice.Mutex);
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			//	This property returns whether or not the device wants to be able to be the
			//	default device for content. This is the device that iTunes and QuickTime
			//	will use to play their content on and FaceTime will use as it's microhphone.
			//	Nearly all devices should allow for this.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultDevice for the device");
			*((UInt32*)outData) = 1; // CHECKME: maybe this should only be true for the output stream?
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			//	This property returns whether or not the device wants to be the system
			//	default device. This is the device that is used to play interface sounds and
			//	other incidental or UI-related sounds on. Most devices should allow this
			//	although devices with lots of latency may not want to.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultSystemDevice for the device");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyLatency:
			//	This property returns the presentation latency of the device. For this,
			//	device, the value is 0 due to the fact that it always vends silence.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device");
			*((UInt32*)outData) = 0;
			if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
				*((UInt32*)outData) = 1536; // FIXME frame-size of SoundPusher output format?
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyStreams:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			
			//	Note that what is returned here depends on the scope requested.
			switch(inAddress->mScope)
			{
				case kAudioObjectPropertyScopeGlobal:
					//	global scope means return all streams
					if(theNumberItemsToFetch > 2)
					{
						theNumberItemsToFetch = 2;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
					}
					if(theNumberItemsToFetch > 1)
					{
						((AudioObjectID*)outData)[1] = kObjectID_Stream_Output;
					}
					break;
					
				case kAudioObjectPropertyScopeInput:
					//	input scope means just the objects on the input side
					if(theNumberItemsToFetch > 1)
					{
						theNumberItemsToFetch = 1;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						((AudioObjectID*)outData)[0] = kObjectID_Stream_Input;
					}
					break;
					
				case kAudioObjectPropertyScopeOutput:
					//	output scope means just the objects on the output side
					if(theNumberItemsToFetch > 1)
					{
						theNumberItemsToFetch = 1;
					}
					
					//	fill out the list with as many objects as requested
					if(theNumberItemsToFetch > 0)
					{
						((AudioObjectID*)outData)[0] = kObjectID_Stream_Output;
					}
					break;
			};
			
			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyControlList:
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
			if(theNumberItemsToFetch > 1)
			{
				theNumberItemsToFetch = 1;
			}
			
			// we only have a single control (output mute)
			*(AudioObjectID*)outData = kObjectID_Mute_Output_Master;

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
			break;

		case kAudioDevicePropertySafetyOffset:
			//	This property returns the how close to now the HAL can read and write. For
			//	this, device, the value is 0 due to the fact that it always vends silence.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyNominalSampleRate:
			//	This property returns the nominal sample rate of the device. Note that we
			//	only need to take the state lock to get this value.
			FailWithAction(inDataSize < sizeof(Float64), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");
			pthread_mutex_lock(&gDevice.Mutex);
			*((Float64*)outData) = gDevice.CurrentFormat.mSampleRate;
			pthread_mutex_unlock(&gDevice.Mutex);
			*outDataSize = sizeof(Float64);
			break;

		case kAudioDevicePropertyAvailableNominalSampleRates:
			//	This returns all nominal sample rates the device supports as an array of
			//	AudioValueRangeStructs. Note that for discrete sample rates, the range
			//	will have the minimum value equal to the maximum value.
			
			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);
			
			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > kNumSupportedSampleRates)
			{
				theNumberItemsToFetch = kNumSupportedSampleRates;
			}
			
			//	fill out the return array
			for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex)
			{
				((AudioValueRange*)outData)[theItemIndex].mMinimum = kSupportedSampleRates[theItemIndex];
				((AudioValueRange*)outData)[theItemIndex].mMaximum = kSupportedSampleRates[theItemIndex];
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
			break;
		
		case kAudioDevicePropertyIsHidden:
			//	This returns whether or not the device is visible to clients.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyIsHidden for the device");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelsForStereo:
			//	This property returns which two channels to use as left/right for stereo
			//	data by default. Note that the channel numbers are 1-based.
			FailWithAction(inDataSize < (2 * sizeof(UInt32)), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device");
			pthread_mutex_lock(&gDevice.Mutex);
			GetPreferredStereoChannels(gDevice.CurrentChannelLayout.mChannelLayoutTag, (UInt32*)outData);
			pthread_mutex_unlock(&gDevice.Mutex);
			*outDataSize = 2 * sizeof(UInt32);
			break;

		case kAudioDevicePropertyPreferredChannelLayout:
			//	This property returns the default AudioChannelLayout to use for the device by default.
			{
				AudioStreamBasicDescription format;
				AudioChannelLayout layout;
				AudioChannelLayout *outLayout = NULL;

				pthread_mutex_lock(&gDevice.Mutex);
				format = gDevice.CurrentFormat;
				layout = gDevice.CurrentChannelLayout;
				pthread_mutex_unlock(&gDevice.Mutex);
				assert(format.mChannelsPerFrame == AudioChannelLayoutTag_GetNumberOfChannels(layout.mChannelLayoutTag));

				UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (format.mChannelsPerFrame * sizeof(AudioChannelDescription));
				FailWithAction(inDataSize < theACLSize, theAnswer = kAudioHardwareBadPropertySizeError, Done, "Loopback_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");

				outLayout = (AudioChannelLayout*)outData;
				memset(outLayout, 0, theACLSize);
				DescribeChannelLayout(layout.mChannelLayoutTag, outLayout);
				*outDataSize = theACLSize;
			}
			break;

		case kAudioDevicePropertyZeroTimeStampPeriod:
			//	This property returns how many frames the HAL should expect to see between
			//	successive sample times in the zero time stamps this device provides.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
			// as our ring-buffer only support sizes of page_length, we need to adjust the expected zero time stamp period
			*((UInt32*)outData) = kDevice_NumZeroFrames;
			*outDataSize = sizeof(UInt32);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetDevicePropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetDevicePropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetDevicePropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetDevicePropertyData: no place to return the properties that changed");
	FailWithAction(inObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetDevicePropertyData: not the device object");
	
	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetDevicePropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioDevicePropertyNominalSampleRate:
		{
			//	Changing the sample rate needs to be handled via the
			//	RequestConfigChange/PerformConfigChange machinery.

			//	check the arguments
			FailWithAction(inDataSize != sizeof(Float64), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_SetDevicePropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");

			UInt32 theItemIndex;
			for (theItemIndex = 0; theItemIndex < kNumSupportedSampleRates; theItemIndex++)
			{
				if (kSupportedSampleRates[theItemIndex] == *(const Float64*)inData)
					break;
			}
			FailWithAction(theItemIndex == kNumSupportedSampleRates, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetDevicePropertyData: unsupported value for kAudioDevicePropertyNominalSampleRate");

			AudioStreamBasicDescription theOldFormat;
			//	make sure that the new value is different than the old value
			pthread_mutex_lock(&gDevice.Mutex);
			theOldFormat = gDevice.CurrentFormat;
			pthread_mutex_unlock(&gDevice.Mutex);
			if (*((const Float64*)inData) != theOldFormat.mSampleRate)
			{
				AudioStreamBasicDescription *newFormat = (AudioStreamBasicDescription *)malloc(sizeof *newFormat);
				
				memcpy(newFormat, &theOldFormat, sizeof *newFormat);
				newFormat->mSampleRate = *((const Float64*)inData);
				//	we dispatch this so that the change can happen asynchronously
				dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, kChangeRequest_StreamFormat, newFormat); });
			}
			break;
		}
		
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Stream Property Operations

static Boolean	LoopbackAudio_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_HasStreamProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "LoopbackAudio_HasStreamProperty: no address");
	FailIf((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), Done, "LoopbackAudio_HasStreamProperty: not a stream object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetStreamPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsStreamPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsStreamPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsStreamPropertySettable: no place to put the return value");
	FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsStreamPropertySettable: not a stream object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetStreamPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
		case kAudioObjectPropertyOwner:
		case kAudioObjectPropertyOwnedObjects:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			*outIsSettable = false;
			break;
		
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			*outIsSettable = true;
			break;
		
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.
	
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetStreamPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetStreamPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetStreamPropertyDataSize: no place to put the return value");
	FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetStreamPropertyDataSize: not a stream object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetStreamPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyClass:
			*outDataSize = sizeof(AudioClassID);
			break;

		case kAudioObjectPropertyOwner:
			*outDataSize = sizeof(AudioObjectID);
			break;

		case kAudioObjectPropertyOwnedObjects:
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioStreamPropertyIsActive:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyDirection:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			*outDataSize = sizeof(UInt32);
			break;
		
		case kAudioStreamPropertyLatency:
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			*outDataSize = sizeof(AudioStreamBasicDescription);
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			*outDataSize = kNumSupportedChannels * kNumSupportedSampleRates * sizeof(AudioStreamRangedDescription);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt32 theNumberItemsToFetch;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetStreamPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetStreamPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetStreamPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetStreamPropertyData: no place to put the return value");
	FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetStreamPropertyData: not a stream object");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inAddress->mSelector)
	{
		case kAudioObjectPropertyBaseClass:
			//	The base class for kAudioStreamClassID is kAudioObjectClassID
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the stream");
			*((AudioClassID*)outData) = kAudioObjectClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyClass:
			//	The class is always kAudioStreamClassID for streams created by drivers
			FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the stream");
			*((AudioClassID*)outData) = kAudioStreamClassID;
			*outDataSize = sizeof(AudioClassID);
			break;
			
		case kAudioObjectPropertyOwner:
			//	The stream's owner is the device object
			FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the stream");
			*((AudioObjectID*)outData) = kObjectID_Device;
			*outDataSize = sizeof(AudioObjectID);
			break;
			
		case kAudioObjectPropertyOwnedObjects:
			//	Streams do not own any objects
			*outDataSize = 0 * sizeof(AudioObjectID);
			break;

		case kAudioStreamPropertyIsActive:
			//	This property tells the device whether or not the given stream is going to
			//	be used for IO. Note that we need to take the state lock to examine this
			//	value.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyIsActive for the stream");
			pthread_mutex_lock(&gDevice.Mutex);
			*((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? gDevice.Stream_Input_IsActive : gDevice.Stream_Output_IsActive;
			pthread_mutex_unlock(&gDevice.Mutex);
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyDirection:
			//	This returns whether the stream is an input stream or an output stream.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyDirection for the stream");
			*((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyTerminalType:
			//	This returns a value that indicates what is at the other end of the stream
			//	such as a speaker or headphones, or a microphone. Values for this property
			//	are defined in <CoreAudio/AudioHardwareBase.h>
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyTerminalType for the stream");
//			*((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
			*((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeDigitalAudioInterface : kAudioStreamTerminalTypeDigitalAudioInterface;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyStartingChannel:
			//	This property returns the absolute channel number for the first channel in
			//	the stream. For exmaple, if a device has two output streams with two
			//	channels each, then the starting channel number for the first stream is 1
			//	and the starting channel number fo the second stream is 3.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			*((UInt32*)outData) = 1;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyLatency:
			//	This property returns any additonal presentation latency the stream has.
			FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream");
			*((UInt32*)outData) = 0;
			*outDataSize = sizeof(UInt32);
			break;

		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			//	This returns the current format of the stream in an
			//	AudioStreamBasicDescription. Note that we need to hold the state lock to get
			//	this value.
			//	Note that for devices that don't override the mix operation, the virtual
			//	format has to be the same as the physical format.
			FailWithAction(inDataSize < sizeof(AudioStreamBasicDescription), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyVirtualFormat for the stream");
			pthread_mutex_lock(&gDevice.Mutex);
			memcpy(outData, &gDevice.CurrentFormat, sizeof (AudioStreamBasicDescription));
			pthread_mutex_unlock(&gDevice.Mutex);
			*outDataSize = sizeof(AudioStreamBasicDescription);
			break;

		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			//	This returns an array of AudioStreamRangedDescriptions that describe what
			//	formats are supported.

			//	Calculate the number of items that have been requested. Note that this
			//	number is allowed to be smaller than the actual size of the list. In such
			//	case, only that number of items will be returned
			theNumberItemsToFetch = inDataSize / sizeof(AudioStreamRangedDescription);

			//	clamp it to the number of items we have
			if(theNumberItemsToFetch > kNumSupportedSampleRates * kNumSupportedChannels)
			{
				theNumberItemsToFetch = kNumSupportedSampleRates * kNumSupportedChannels;
			}
			
			//	fill out the return array
			for (unsigned i = 0; i < theNumberItemsToFetch; i++)
			{
				const unsigned numChannels = kSupportedNumChannels[i / kNumSupportedSampleRates];
				const unsigned sampleRateIndex = i % kNumSupportedSampleRates;
				AudioStreamRangedDescription *outDesc = (AudioStreamRangedDescription *)outData + i;
				
				outDesc->mFormat.mFormatID = kAudioFormatLinearPCM;
				outDesc->mFormat.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
				outDesc->mFormat.mBitsPerChannel = sizeof(float) * 8;
				outDesc->mFormat.mChannelsPerFrame = numChannels;
				outDesc->mFormat.mBytesPerFrame = numChannels * sizeof(float);
				outDesc->mFormat.mFramesPerPacket = 1;
				outDesc->mFormat.mBytesPerPacket = numChannels * sizeof(float);
				outDesc->mSampleRateRange.mMinimum = kSupportedSampleRates[sampleRateIndex];
				outDesc->mSampleRateRange.mMaximum = kSupportedSampleRates[sampleRateIndex];
			}

			//	report how much we wrote
			*outDataSize = theNumberItemsToFetch * sizeof(AudioStreamRangedDescription);
			break;

		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetStreamPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetStreamPropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetStreamPropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetStreamPropertyData: no place to return the properties that changed");
	FailWithAction((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output), theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetStreamPropertyData: not a stream object");
	
	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetStreamPropertyData() method.
	switch(inAddress->mSelector)
	{
		case kAudioStreamPropertyIsActive:
			//	Changing the active state of a stream doesn't affect IO or change the structure
			//	so we can just save the state and send the notification.
			FailWithAction(inDataSize != sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_SetStreamPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
			pthread_mutex_lock(&gDevice.Mutex);
			if(inObjectID == kObjectID_Stream_Input)
			{
				if(gDevice.Stream_Input_IsActive != (*((const UInt32*)inData) != 0))
				{
					gDevice.Stream_Input_IsActive = *((const UInt32*)inData) != 0;
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			}
			else
			{
				if(gDevice.Stream_Output_IsActive != (*((const UInt32*)inData) != 0))
				{
					gDevice.Stream_Output_IsActive = *((const UInt32*)inData) != 0;
					*outNumberPropertiesChanged = 1;
					outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive;
					outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
					outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
				}
			}
			pthread_mutex_unlock(&gDevice.Mutex);
			break;
			
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
		{
			//	Changing the stream format needs to be handled via the
			//	RequestConfigChange/PerformConfigChange machinery.
			AudioStreamBasicDescription currentAudioFormat;
			const AudioStreamBasicDescription *newAudioFormat = (const AudioStreamBasicDescription*)inData;
			unsigned i;
			
			FailWithAction(inDataSize != sizeof(AudioStreamBasicDescription), theAnswer = kAudioHardwareBadPropertySizeError, Done, "Loopback_SetStreamPropertyData: wrong size for the data for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mFormatID != kAudioFormatLinearPCM, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported format ID for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mFormatFlags != kAudioFormatFlagsNativeFloatPacked, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported format flags for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mBitsPerChannel != sizeof(float) * 8, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported bits per channel for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mBytesPerPacket != sizeof(float) * newAudioFormat->mChannelsPerFrame, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported bytes per packet for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mFramesPerPacket != 1, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported frames per packet for kAudioStreamPropertyPhysicalFormat");
			FailWithAction(newAudioFormat->mBytesPerFrame != newAudioFormat->mFramesPerPacket * newAudioFormat->mBytesPerPacket, theAnswer = kAudioDeviceUnsupportedFormatError, Done, "Loopback_SetStreamPropertyData: unsupported bytes per frame for kAudioStreamPropertyPhysicalFormat");

			for (i = 0; i < kNumSupportedChannels; i++)
				if (newAudioFormat->mChannelsPerFrame == kSupportedNumChannels[i])
					break;
			FailWithAction(i == kNumSupportedChannels, theAnswer = kAudioHardwareIllegalOperationError, Done, "Loopback_SetStreamPropertyData: unsupported number of channels for kAudioStreamPropertyPhysicalFormat");

			for (i = 0; i < kNumSupportedSampleRates; i++)
				if (newAudioFormat->mSampleRate == kSupportedSampleRates[i])
					break;
			FailWithAction(i == kNumSupportedSampleRates, theAnswer = kAudioHardwareIllegalOperationError, Done, "Loopback_SetStreamPropertyData: unsupported sample rate for kAudioStreamPropertyPhysicalFormat");

			//	If we made it this far, the requested format is something we support, so make sure the sample rate is actually different
			pthread_mutex_lock(&gDevice.Mutex);
			currentAudioFormat = gDevice.CurrentFormat;
			pthread_mutex_unlock(&gDevice.Mutex);

			if(memcmp(newAudioFormat, &currentAudioFormat, sizeof *newAudioFormat) != 0)
			{
				//	we dispatch this so that the change can happen asynchronously
				AudioStreamBasicDescription *newDescAlloced = (AudioStreamBasicDescription *)malloc(sizeof *newAudioFormat);
				memcpy(newDescAlloced, newAudioFormat, sizeof *newAudioFormat);
				dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{ gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, kChangeRequest_StreamFormat, newDescAlloced); });
			}
			*outNumberPropertiesChanged = 1;
			outChangedAddresses[0].mSelector = kAudioDevicePropertyPreferredChannelLayout;
			outChangedAddresses[0].mScope = kAudioObjectPropertyScopeOutput;
			outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
			break;
		}
		
		default:
			theAnswer = kAudioHardwareUnknownPropertyError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark Control Property Operations

static Boolean	LoopbackAudio_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
	//	This method returns whether or not the given object has the given property.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	Boolean theAnswer = false;
	
	//	check the arguments
	FailIf(inDriver != gAudioServerPlugInDriverRef, Done, "LoopbackAudio_HasControlProperty: bad driver reference");
	FailIf(inAddress == NULL, Done, "LoopbackAudio_HasControlProperty: no address");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetControlPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_Mute_Output_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
				case kAudioBooleanControlPropertyValue:
					theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster);
					break;
			};
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
	//	This method returns whether or not the given property on the object can have its value
	//	changed.
	
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_IsControlPropertySettable: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsControlPropertySettable: no address");
	FailWithAction(outIsSettable == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_IsControlPropertySettable: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetControlPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_Mute_Output_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
				case kAudioObjectPropertyClass:
				case kAudioObjectPropertyOwner:
				case kAudioObjectPropertyOwnedObjects:
				case kAudioControlPropertyScope:
				case kAudioControlPropertyElement:
					*outIsSettable = false;
					break;
				
				case kAudioBooleanControlPropertyValue:
					*outIsSettable = true;
					break;
				
				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;
		
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
	//	This method returns the byte size of the property's data.
	
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetControlPropertyDataSize: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetControlPropertyDataSize: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetControlPropertyDataSize: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetControlPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_Mute_Output_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyClass:
					*outDataSize = sizeof(AudioClassID);
					break;

				case kAudioObjectPropertyOwner:
					*outDataSize = sizeof(AudioObjectID);
					break;

				case kAudioObjectPropertyOwnedObjects:
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioBooleanControlPropertyValue:
					*outDataSize = sizeof(UInt32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;
		
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
	#pragma unused(inClientProcessID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetControlPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetControlPropertyData: no address");
	FailWithAction(outDataSize == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetControlPropertyData: no place to put the return value size");
	FailWithAction(outData == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_GetControlPropertyData: no place to put the return value");
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required.
	//
	//	Also, since most of the data that will get returned is static, there are few instances where
	//	it is necessary to lock the state mutex.
	switch(inObjectID)
	{
		case kObjectID_Mute_Output_Master:
			switch(inAddress->mSelector)
			{
				case kAudioObjectPropertyBaseClass:
					//	The base class for kAudioMuteControlClassID is kAudioBooleanControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the mute control");
					*((AudioClassID*)outData) = kAudioBooleanControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;
					
				case kAudioObjectPropertyClass:
					//	Mute controls are of the class, kAudioMuteControlClassID
					FailWithAction(inDataSize < sizeof(AudioClassID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the mute control");
					*((AudioClassID*)outData) = kAudioMuteControlClassID;
					*outDataSize = sizeof(AudioClassID);
					break;
					
				case kAudioObjectPropertyOwner:
					//	The control's owner is the device object
					FailWithAction(inDataSize < sizeof(AudioObjectID), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the mute control");
					*((AudioObjectID*)outData) = kObjectID_Device;
					*outDataSize = sizeof(AudioObjectID);
					break;
					
				case kAudioObjectPropertyOwnedObjects:
					//	Controls do not own any objects
					*outDataSize = 0 * sizeof(AudioObjectID);
					break;

				case kAudioControlPropertyScope:
					//	This property returns the scope that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyScope), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the mute control");
					*((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeOutput;
					*outDataSize = sizeof(AudioObjectPropertyScope);
					break;

				case kAudioControlPropertyElement:
					//	This property returns the element that the control is attached to.
					FailWithAction(inDataSize < sizeof(AudioObjectPropertyElement), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the mute control");
					*((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMaster;
					*outDataSize = sizeof(AudioObjectPropertyElement);
					break;

				case kAudioBooleanControlPropertyValue:
					//	This returns the value of the mute control where 0 means that mute is off
					//	and audio can be heard and 1 means that mute is on and audio cannot be heard.
					//	Note that we need to take the state lock to examine this value.
					FailWithAction(inDataSize < sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_GetControlPropertyData: not enough space for the return value of kAudioBooleanControlPropertyValue for the mute control");
					pthread_mutex_lock(&gDevice.Mutex);
					*((UInt32*)outData) = gDevice.Stream_Output_Master_Mute ? 1 : 0;
					pthread_mutex_unlock(&gDevice.Mutex);
					*outDataSize = sizeof(UInt32);
					break;

				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;
		
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2])
{
	#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
	
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_SetControlPropertyData: bad driver reference");
	FailWithAction(inAddress == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetControlPropertyData: no address");
	FailWithAction(outNumberPropertiesChanged == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetControlPropertyData: no place to return the number of properties that changed");
	FailWithAction(outChangedAddresses == NULL, theAnswer = kAudioHardwareIllegalOperationError, Done, "LoopbackAudio_SetControlPropertyData: no place to return the properties that changed");
	
	//	initialize the returned number of changed properties
	*outNumberPropertiesChanged = 0;
	
	//	Note that for each object, this driver implements all the required properties plus a few
	//	extras that are useful but not required. There is more detailed commentary about each
	//	property in the LoopbackAudio_GetControlPropertyData() method.
	switch(inObjectID)
	{
		case kObjectID_Mute_Output_Master:
			switch(inAddress->mSelector)
			{
				case kAudioBooleanControlPropertyValue:
					FailWithAction(inDataSize != sizeof(UInt32), theAnswer = kAudioHardwareBadPropertySizeError, Done, "LoopbackAudio_SetControlPropertyData: wrong size for the data for kAudioBooleanControlPropertyValue");
					pthread_mutex_lock(&gDevice.Mutex);
					if(gDevice.Stream_Output_Master_Mute != (*((const UInt32*)inData) != 0))
					{
						gDevice.Stream_Output_Master_Mute = *((const UInt32*)inData) != 0;
						*outNumberPropertiesChanged = 1;
						outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue;
						outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
						outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster;
					}
					pthread_mutex_unlock(&gDevice.Mutex);
					break;
				
				default:
					theAnswer = kAudioHardwareUnknownPropertyError;
					break;
			};
			break;
		
		default:
			theAnswer = kAudioHardwareBadObjectError;
			break;
	};

Done:
	return theAnswer;
}

#pragma mark IO Operations

static OSStatus	LoopbackAudio_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
	//	This call tells the device that IO is starting for the given client. When this routine
	//	returns, the device's clock is running and it is ready to have data read/written. It is
	//	important to note that multiple clients can have IO running on the device at the same time.
	//	So, work only needs to be done when the first client starts. All subsequent starts simply
	//	increment the counter.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_StartIO: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_StartIO: bad device ID");

	//	we need to hold the state lock
	pthread_mutex_lock(&gDevice.Mutex);
	
	//	figure out what we need to do
	if(gDevice.IOIsRunning == UINT32_MAX)
	{
		//	overflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	}
	else if(gDevice.IOIsRunning == 0)
	{
		//	We need to start the hardware, which in this case is just anchoring the time line.
		TPCircularBufferInit(&gDevice.RingBuffer, kDevice_RingBuffNumFrames * gDevice.CurrentFormat.mBytesPerFrame);
		memset(gDevice.RingBuffer.buffer, 0, gDevice.RingBuffer.length);

		gDevice.LastWriteEndInFrames = 0;
		gDevice.ReadOffsetInFrames = 0;

		gDevice.TimeLineSeed = 1;
		gDevice.NumberTimeStamps = 0;
		gDevice.AnchorSampleTime = 0;
		gDevice.AnchorHostTime = mach_absolute_time();

		gDevice.IOIsRunning = 1;
	}
	else
	{
		//	IO is already running, so just bump the counter
		++gDevice.IOIsRunning;
	}
	
	//	unlock the state lock
	pthread_mutex_unlock(&gDevice.Mutex);
	
Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
	//	This call tells the device that the client has stopped IO. The driver can stop the hardware
	//	once all clients have stopped.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_StopIO: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_StopIO: bad device ID");

	//	we need to hold the state lock
	pthread_mutex_lock(&gDevice.Mutex);
	
	//	figure out what we need to do
	if(gDevice.IOIsRunning == 0)
	{
		//	underflowing is an error
		theAnswer = kAudioHardwareIllegalOperationError;
	}
	else if(gDevice.IOIsRunning == 1)
	{
		//	We need to stop the hardware, which in this case means that there's nothing to do.
		TPCircularBufferCleanup(&gDevice.RingBuffer);
		gDevice.IOIsRunning = 0;
	}
	else
	{
		//	IO is still running, so just bump the counter
		--gDevice.IOIsRunning;
	}
	
	//	unlock the state lock
	pthread_mutex_unlock(&gDevice.Mutex);
	
Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
	//	This method returns the current zero time stamp for the device. The HAL models the timing of
	//	a device as a series of time stamps that relate the sample time to a host time. The zero
	//	time stamps are spaced such that the sample times are the value of
	//	kAudioDevicePropertyZeroTimeStampPeriod apart. This is often modeled using a ring buffer
	//	where the zero time stamp is updated when wrapping around the ring buffer.
	//
	//	For this device, the zero time stamps' sample time increments every kDevice_NumZeroFrames
	//	frames and the host time increments by kDevice_NumZeroFrames * gDevice.HostTicksPerFrame.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	UInt64 theCurrentHostTime;
	Float64 theHostTicksPerRingBuffer;
	Float64 theHostTickOffset;
	UInt64 theNextHostTime;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetZeroTimeStamp: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_GetZeroTimeStamp: bad device ID");

	//	we need to hold the lock
	os_unfair_lock_lock(&gDevice.UnfairLock);

	//	get the current host time
	theCurrentHostTime = mach_absolute_time();
	
	//	calculate the next host time
	theHostTicksPerRingBuffer = gDevice.HostTicksPerFrame * ((Float64)kDevice_NumZeroFrames);
	theHostTickOffset = ((Float64)(gDevice.NumberTimeStamps + 1)) * theHostTicksPerRingBuffer;
	theNextHostTime = gDevice.AnchorHostTime + ((UInt64)theHostTickOffset);
	
	//	go to the next time if the next host time is less than the current time
	if(theNextHostTime <= theCurrentHostTime)
	{
		++gDevice.NumberTimeStamps;
	}

	//	set the return values
	*outSampleTime = gDevice.NumberTimeStamps * kDevice_NumZeroFrames;
	*outHostTime = gDevice.AnchorHostTime + (((Float64)gDevice.NumberTimeStamps) * theHostTicksPerRingBuffer);
	*outSeed = gDevice.TimeLineSeed;

	//	unlock the state lock
	os_unfair_lock_unlock(&gDevice.UnfairLock);

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
	//	This method returns whether or not the device will do a given IO operation. For this device,
	//	we only support reading input data and writing output data.
	
	#pragma unused(inClientID)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_WillDoIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_WillDoIOOperation: bad device ID");

	//	figure out if we support the operation
	bool willDo = false;
	bool willDoInPlace = true;
	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
			willDo = true;
			willDoInPlace = true;
			break;
			
		case kAudioServerPlugInIOOperationWriteMix:
			willDo = true;
			willDoInPlace = true;
			break;
			
	};
	
	//	fill out the return values
	if(outWillDo != NULL)
	{
		*outWillDo = willDo;
	}
	if(outWillDoInPlace != NULL)
	{
		*outWillDoInPlace = willDoInPlace;
	}

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
	//	This is called at the beginning of an IO operation.
	
	#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_BeginIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_BeginIOOperation: bad device ID");

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
	//	This is called to actuall perform a given operation.
	
	#pragma unused(inClientID, ioSecondaryBuffer)
	
	//	declare the local variables
	OSStatus theAnswer = 0;

	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_DoIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_DoIOOperation: bad device ID");
	FailWithAction((inStreamObjectID != kObjectID_Stream_Input) && (inStreamObjectID != kObjectID_Stream_Output), theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_DoIOOperation: bad stream ID");

	const UInt32 numFramesInRingBuffer = gDevice.RingBuffer.length / gDevice.CurrentFormat.mBytesPerFrame;

	switch(inOperationID)
	{
		case kAudioServerPlugInIOOperationReadInput:
		{ // provide input from our internal buffer: There can be multiple reader threads
			const SInt64 lastWriteEndInFrames = atomic_load_explicit(&gDevice.LastWriteEndInFrames, memory_order_acquire);
			if (lastWriteEndInFrames == 0 || gDevice.Stream_Output_Master_Mute)
			{ // reading before the first write happened (or muted), so output silence
				memset(ioMainBuffer, 0, inIOBufferFrameSize * gDevice.CurrentFormat.mBytesPerFrame);
				break;
			}
			SInt64 ReadOffsetInFrames = atomic_load_explicit(&gDevice.ReadOffsetInFrames, memory_order_acquire);
			if (ReadOffsetInFrames <= 0)
			{ // we have an invalid read offset, so readjust it to match our last write (so we read the most recent data)
				const SInt64 newReadOffsetInFrames = lastWriteEndInFrames - (SInt64)inIOCycleInfo->mInputTime.mSampleTime - inIOBufferFrameSize;
				if (atomic_compare_exchange_strong_explicit(&gDevice.ReadOffsetInFrames, &ReadOffsetInFrames, newReadOffsetInFrames, memory_order_acq_rel, memory_order_acquire))
					ReadOffsetInFrames = newReadOffsetInFrames;
				// else ReadOffsetInFrames already contains updated value
			}
			if (ReadOffsetInFrames < 0)
			{ // provide silence if data is old
				memset(ioMainBuffer, 0, inIOBufferFrameSize * gDevice.CurrentFormat.mBytesPerFrame);
				break;
			}

			SInt64 readBegin = (SInt64)inIOCycleInfo->mInputTime.mSampleTime + ReadOffsetInFrames;
			SInt64 readEnd = readBegin + inIOBufferFrameSize;
			while (readEnd > lastWriteEndInFrames)
			{ // we would read beyond the end of the most recent data
				const SInt64 oldReadOffsetInFrames = ReadOffsetInFrames;
				#pragma unused(oldReadOffsetInFrames)
				const SInt64 newReadOffsetInFrames = ReadOffsetInFrames - (readEnd - lastWriteEndInFrames);
				if (atomic_compare_exchange_weak_explicit(&gDevice.ReadOffsetInFrames, &ReadOffsetInFrames, newReadOffsetInFrames, memory_order_acq_rel, memory_order_acquire))
				{
					DebugMsg(OS_LOG_TYPE_INFO, "LoopbackAudio_ReadInput: adjusting read offset from %lld to %lld", oldReadOffsetInFrames, newReadOffsetInFrames);
					ReadOffsetInFrames = newReadOffsetInFrames;
				}
				// else ReadOffsetInFrames already contains updated value

				readBegin = (SInt64)inIOCycleInfo->mInputTime.mSampleTime + ReadOffsetInFrames;
				readEnd = readBegin + inIOBufferFrameSize;
			}

			uint8_t *bufferBegin = (uint8_t *)gDevice.RingBuffer.buffer + (readBegin % numFramesInRingBuffer) * gDevice.CurrentFormat.mBytesPerFrame;
			memcpy(ioMainBuffer, bufferBegin, inIOBufferFrameSize * gDevice.CurrentFormat.mBytesPerFrame);
			break;
		}
		case kAudioServerPlugInIOOperationWriteMix:
		{ // write input to our internal buffer: There can only be one writer thread
			const SInt64 writeBegin = (SInt64)inIOCycleInfo->mOutputTime.mSampleTime;
			uint8_t *bufferBegin = (uint8_t *)gDevice.RingBuffer.buffer + (writeBegin % numFramesInRingBuffer) * gDevice.CurrentFormat.mBytesPerFrame;
			memcpy(bufferBegin, ioMainBuffer, inIOBufferFrameSize * gDevice.CurrentFormat.mBytesPerFrame);
			atomic_store_explicit(&gDevice.LastWriteEndInFrames, writeBegin + inIOBufferFrameSize, memory_order_release);
			break;
		}
	}

Done:
	return theAnswer;
}

static OSStatus	LoopbackAudio_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
	//	This is called at the end of an IO operation. This device doesn't do anything, so just check
	//	the arguments and return.
	
	#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
	
	//	declare the local variables
	OSStatus theAnswer = 0;
	
	//	check the arguments
	FailWithAction(inDriver != gAudioServerPlugInDriverRef, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_EndIOOperation: bad driver reference");
	FailWithAction(inDeviceObjectID != kObjectID_Device, theAnswer = kAudioHardwareBadObjectError, Done, "LoopbackAudio_EndIOOperation: bad device ID");

Done:
	return theAnswer;
}
