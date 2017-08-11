#include "AudioPluginUtil.h"

#include <wrl/client.h>
#include <xapo.h>
#include "hrtfapoapi.h"
#include <DirectXMath.h>

#include <stdint.h>
#include <objbase.h>
#include <memory>
#include <mmreg.h>
#include <windows.h>
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include <devpropdef.h>
#include <mmeapi.h>
#include "SpatialAudioClient.h"
#include "mmdeviceapi.h"
#include <mmeapi.h>
#include <wrl.h>
#include <memory>
#include <vector>
#include <list>
#include <iostream>

using namespace Microsoft::WRL;

// CONVENTIONS:
//   Global variables g_NameOfVariable
//   Member variables m_NameOfVariable
//   Local variables NameOfVariable
//   Pointer: p_NameOfVariable
//   Defines/Consts NAME_OF_VARIABLE

// TODOS
//  1: All Mutex waits: what to do if wait is abandoned
//  2: Remove goto statments

namespace MSHRTFSpatializer
{
//################ TODO: REMOVE ################
	// because of the way HRTF is initialized, and because the unity plugin is initialized before we can touch it via a script ...
	//  if we want to expose all parameters we have to re-initialize the HRTF object inside ProcessCallback on first run if a param changes
	float m_currentEnvironment = 1.f;	// these are the windows default values for these params
	float m_currentMingain = -96.f;
	float m_currentMaxgain = 12.f;
	float m_currentUnitygain = 1.f;
	float m_bypass_attenuation = 0.f;

//################ DEFINES AND CONSTS ################
	#define ISAC_CALLBACK_BUF_SIZE 4800
	#define EMPTY_COUNT_LIMIT 5
	#define ISACFRAMECOUNTPERPUMP 480

	// This GUID uniquely identifies a Middleware Stack. WWise, FMod etc each will need to have their own GUID
	// that should never change.
	// We log this value as part of spatial audio client telemetry; and map the GUIDs to middleware
	// while processing the telemetry, so we can filter telemetry by middleware.
	const GUID UNITY_ISAC_MIDDLEWARE_ID = { 0xe07049bc, 0xa91e, 0x489d,{ 0xad, 0xeb, 0xb1, 0x70, 0xa4, 0xa, 0x30, 0x6f } };

	// Middleware can use up to 4 integers to pass the version info
	const int MAJOR_VERSION = 0;
	const int MINOR_VERSION1 = 2;
	const int MINOR_VERSION2 = 0;

	// The sample rate required by ISAC
	const int REQUIRED_SAMPLE_RATE = 48000;

//################ ENUMS AND STRUCTS ################
	enum
	{
		P_CUTOFFDIST = 0,
		P_ENVIRONMENT,
		P_MINGAIN,
		P_MAXGAIN,
		P_UNITYGAINDISTANCE,
		P_BYPASS_ATTENUATION,
		P_NUM
	};

	struct UnityAudioData
	{
		float p[P_NUM];

		float	m_DataBuf[ISAC_CALLBACK_BUF_SIZE];
		float	m_DataPosX[ISAC_CALLBACK_BUF_SIZE];
		float	m_DataPosY[ISAC_CALLBACK_BUF_SIZE];
		float	m_DataPosZ[ISAC_CALLBACK_BUF_SIZE];

		UINT32	m_ReadIndex = 0;
		UINT32	m_WriteIndex = 0;
		BOOL	m_WriteIndexOverflowed = FALSE;

		UINT32  m_EmptyCount = 0;

		BOOL	m_InQueue = FALSE;

		std::list<UnityAudioData *>::iterator m_UnityAudioObjectQueueIter;

		HANDLE  m_Lock = nullptr;
	};

//################ GLOBALS ################
	UINT32 g_SystemSampleRate = 0;

	// Keeps track of how many ISAC objects will be available in the next processing pass
	// ISAC can grant or revoke ISAC objects any time
	UINT32 g_ISACObjectCount = 0;
	HANDLE g_ISACObjectCountMutex = nullptr;

	// "Queue" containing UnityAudioData objects that will be rendered by ISAC in the next
	// processing pass. UnityAudioData objects are added/removed from this queue based on
	// how many ISAC objects are available, and when Unity adds or removes audio objects to 
	// the scene
	std::list<UnityAudioData *> g_UnityAudioObjectQueue;
	HANDLE g_UnityAudioObjectQueueMutex;

	// Indicates if there is space in the queue above. Created to avoid checking
	// the queue size again and again.
	LONG g_ThereIsSpaceInUnityAudioObjectQueue = FALSE;

	// Vector containing ISAC objects (not all objects in here are active or used)
	std::vector<ComPtr<ISpatialAudioObject>> g_ISACObjectVector;

	// Variables to manage operation of ISAC
	ComPtr<ISpatialAudioClient> g_SpatialAudioClient;
	ComPtr<ISpatialAudioObjectRenderStream> g_SpatialAudioStream;
	HANDLE g_ISACBufferCompletionEvent;

	// Indicates if this is the first time the CreateCallback is called
	// this is an opportunity to initialize stuff
	BOOL g_FirstCreateCallback = TRUE;

	// Indicates if ISAC is up and running
	BOOL g_SpatialAudioClientCreated = FALSE;

	// Worker thread for ISpatialAudioClient work
	PTP_WORK g_WorkThread;
	BOOL g_WorkThreadActive = FALSE;

//################ CLASS AND FUNCTION DEFINITIONS ################
	// Registers spatializer plugin parameters to Unity
	int InternalRegisterEffectDefinition(UnityAudioEffectDefinition& definition)
	{
		// TODO would put realtime parameter control here: roomsize, decay types, min/max gains, zero-gain affected distance (unity gain), directionality?
		int numparams = P_NUM;
		definition.paramdefs = new UnityAudioParameterDefinition[numparams];
		RegisterParameter(definition, "CutoffDist", "", 0.0f, 10000.0f, 999.0f, 1.0f, 1.0f, P_CUTOFFDIST, "HRTF cutoff distance");
		RegisterParameter(definition, "Environment", "", 0.0f, 3.0f, m_currentEnvironment, 1.0f, 1.0f, P_ENVIRONMENT, "Environment size");
		RegisterParameter(definition, "MinGain", "", -96.0f, 12.0f, m_currentMingain, 1.0f, 1.0f, P_MINGAIN, "Minimum gain allowed for room modelling");
		RegisterParameter(definition, "MaxGain", "", -96.0f, 12.0f, m_currentMaxgain, 1.0f, 1.0f, P_MAXGAIN, "Maximum gain allowed for room modelling");
		RegisterParameter(definition, "UnityGainDist", "", 0.05f, FLT_MAX, m_currentUnitygain, 1.0f, 1.0f, P_UNITYGAINDISTANCE, "Distance at which the gain applied is 0dB");
		RegisterParameter(definition, "BypassCurves", "", 0.f, 1.f, m_bypass_attenuation, 1.0f, 1.0f, P_BYPASS_ATTENUATION, "Ignore the Unity Volume curves for more realistic simulation");
		definition.flags |= UnityAudioEffectDefinitionFlags_IsSpatializer;
		return numparams;
	}

	// Declaration
	BOOL InitializeSpatialAudioClient(int sampleRate);

	// Function that actually sends data to ISAC. Runs in a separate thread, waits for
	// ISAC to signal its invocation through g_ISACBufferCompletionEvent
	VOID CALLBACK SpatialWorkCallbackNew(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_WORK Work)
	{
		HRESULT hr = S_OK;
		hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);	// TODO: Find out why this is needed and how this affects things.
		
		Work;
		Instance;

		DWORD ISACBufferCompletionMaxWaitTime = 500;
		// At this point, ISAC has initialized and we can start sending data to it.
		while (g_WorkThreadActive)
		{
			UINT32 FrameCount = 0;
			UINT32 AvailableObjectCount = 0;
			UINT32 ISACObjInx = 0;

			// Wait for ISAC Event 
			if (WaitForSingleObject(g_ISACBufferCompletionEvent, ISACBufferCompletionMaxWaitTime) != WAIT_OBJECT_0)
			{
				// Ideally, we should get an ISAC event every 10ms when ISAC is active.
				// So if we don't get the event within 100 ms, we have an issue. There
				// are three possibilities that we can do something about: 
				// 
				// 1. ISAC hasn't been initialized yet. So we initialize it.
				//
				// 2. A previous ISAC initialization failed because Spatial Audio is
				//    turned off (Spatial Rendering Mode is set to None). In this case
				//    we want to keep trying to initialize ISAC to check if Spatial Audio
				//    has been enabled by the user.
				//
				// 3. The ISAC graph was torn down because the user changed the Spatial 
				//    Rendering mode or changed the Default device. We try to detect if 
				//    that is the case. If so, we reset everything and try to initialize 
				//    ISAC again. Meanwhile, rendering will be handled by Unity.

				// Case 1 and 2
				if (!g_SpatialAudioClientCreated)
				{
					g_SpatialAudioClientCreated = InitializeSpatialAudioClient(g_SystemSampleRate);
					ISACBufferCompletionMaxWaitTime = 500;
					continue;
				}

				// Case 3: If the ISAC graph was torn down, we will get an error when calling this method
				hr = g_SpatialAudioStream->Reset();

				if (FAILED(hr))
				{
					g_SpatialAudioClientCreated = FALSE;

					for (std::list<UnityAudioData*>::iterator iter = g_UnityAudioObjectQueue.begin(); iter != g_UnityAudioObjectQueue.end(); iter++)
					{
						UnityAudioData *p_ObjData = *iter;

						p_ObjData->m_InQueue = FALSE;
					}

					g_UnityAudioObjectQueue.clear();

					g_SpatialAudioClientCreated = InitializeSpatialAudioClient(g_SystemSampleRate);

					if (g_SpatialAudioClientCreated)
					{
						ISACBufferCompletionMaxWaitTime = 100;
					}
				}
				continue;
			}
			
			// If we discover that UnityAudioObject needs to be removed from g_UnityAudioObjectQueue,
			// we put it into this temporary Queue and remove it later in this function.
			std::list<UnityAudioData *> RemoveQueue;

			// In order to not hold down the g_UnityAudioObjectQueue lock for too long, we just make a copy of it
			// locally and render using that.
			std::list<UnityAudioData *> LocalCopyOfQueue;
			DWORD dwWaitResult = WaitForSingleObject(g_UnityAudioObjectQueueMutex, INFINITE);
				if (dwWaitResult == WAIT_OBJECT_0)
				{
					LocalCopyOfQueue = g_UnityAudioObjectQueue;		// Make a local copy of the queue
				}
			ReleaseMutex(g_UnityAudioObjectQueueMutex);

			// Copy data over to ISAC within a Begin/EndUpdatingAudioObjects() block
			hr = g_SpatialAudioStream->BeginUpdatingAudioObjects( &AvailableObjectCount, &FrameCount);
			if (FAILED(hr))
			{
				continue;
			}

			{
				// Go through the local copy of the g_UnityAudioObjectQueue and copy data to ISAC Objects
				for (std::list<UnityAudioData*>::iterator iter = LocalCopyOfQueue.begin(); iter != LocalCopyOfQueue.end(); iter++)
				{
					UnityAudioData *p_ObjData = *iter;

					// Defensive check
					if (ISACObjInx >= AvailableObjectCount)
					{
						continue;
					}

					ComPtr<ISpatialAudioObject> &p_ObjISAC = g_ISACObjectVector[ISACObjInx];
					ISACObjInx++;

					if (p_ObjISAC == nullptr)
					{
						hr = g_SpatialAudioStream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &p_ObjISAC);
						if (FAILED(hr))
						{
							continue;
						}
					}

					BOOL IsActive = FALSE;
					hr = p_ObjISAC->IsActive(&IsActive);
					if (!IsActive)
					{
						p_ObjISAC = nullptr;

						hr = g_SpatialAudioStream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &p_ObjISAC);
						if (FAILED(hr))
						{
							continue;
						}
					}

					// We use circular buffers to sync between Unity and ISAC; if the buffer circled back, take that into account
					UINT32 ActualWriteIndex;
					DWORD dwWaitResultIn = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
						if (dwWaitResultIn == WAIT_OBJECT_0)
						{
							ActualWriteIndex = p_ObjData->m_WriteIndexOverflowed ? p_ObjData->m_WriteIndex + ISAC_CALLBACK_BUF_SIZE : p_ObjData->m_WriteIndex;
						}
					ReleaseMutex(p_ObjData->m_Lock);

					BOOL EnoughData = ((float)ActualWriteIndex - (float)p_ObjData->m_ReadIndex) >= 2*(float)ISACFRAMECOUNTPERPUMP;

					//Get the object buffer
					BYTE* p_ISACObjBuffer = nullptr;
					UINT32 ByteCount;
					hr = p_ObjISAC->GetBuffer(&p_ISACObjBuffer, &ByteCount);
					if (FAILED(hr))
					{
						continue;
					}

					p_ObjISAC->SetPosition(p_ObjData->m_DataPosX[p_ObjData->m_ReadIndex],
											p_ObjData->m_DataPosY[p_ObjData->m_ReadIndex],
											p_ObjData->m_DataPosZ[p_ObjData->m_ReadIndex]);

					p_ObjISAC->SetVolume(1.0f);

					if (EnoughData)
					{
						for (UINT32 inx = 0; inx < ISACFRAMECOUNTPERPUMP; inx++)
						{
							//*((float*)buffer) = 0.0f;
							*((float*)p_ISACObjBuffer) = p_ObjData->m_DataBuf[p_ObjData->m_ReadIndex];
							p_ISACObjBuffer += sizeof(float); // this is a BYTE array and we filled it up with a float

							p_ObjData->m_ReadIndex++;
							if (p_ObjData->m_ReadIndex >= ISAC_CALLBACK_BUF_SIZE)
							{
								p_ObjData->m_ReadIndex -= ISAC_CALLBACK_BUF_SIZE;

								DWORD dwWaitResultIn = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
									if (dwWaitResultIn == WAIT_OBJECT_0)
									{
										p_ObjData->m_WriteIndexOverflowed = FALSE;
									}
								ReleaseMutex(p_ObjData->m_Lock);
							}
						}
					}
					else
					{
						UINT32 CurObjEmptyCount = 0;
						DWORD dwWaitResultIn = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
							if (dwWaitResultIn == WAIT_OBJECT_0)
							{
								CurObjEmptyCount = ++(p_ObjData->m_EmptyCount);
							}
						ReleaseMutex(p_ObjData->m_Lock);

						if (CurObjEmptyCount == EMPTY_COUNT_LIMIT)
						{
							RemoveQueue.push_back(p_ObjData);
						}

						// fill with silence
						for (UINT32 inx = 0; inx < ISACFRAMECOUNTPERPUMP; inx++)
						{
							*((float*)p_ISACObjBuffer) = 0.0f;
							p_ISACObjBuffer += sizeof(float);
						}
					}
				}
			}

			// Let the audio-engine know that the object data are available for processing now 
			hr = g_SpatialAudioStream->EndUpdatingAudioObjects();
			if (FAILED(hr))
			{
				continue;
			}

			// Remove inactive UnityAudioObject from the Queue
			if (!RemoveQueue.empty())
			{
				DWORD dwWaitResult = WaitForSingleObject(g_UnityAudioObjectQueueMutex, INFINITE);
					if (dwWaitResult == WAIT_OBJECT_0)
					{
						// Go through the remove queue and remove the elements in it from the global queue
						while (!RemoveQueue.empty())
						{
							UnityAudioData *p_ObjData = RemoveQueue.front();
							RemoveQueue.pop_front();

							// Check one last time before removing
							DWORD dwWaitResultIn = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
								if (dwWaitResultIn == WAIT_OBJECT_0)
								{
									if (p_ObjData->m_EmptyCount == EMPTY_COUNT_LIMIT)
									{
										g_UnityAudioObjectQueue.erase(p_ObjData->m_UnityAudioObjectQueueIter);
										p_ObjData->m_InQueue = FALSE;
									}
								}
							ReleaseMutex(p_ObjData->m_Lock);
						}
					}

					if (g_UnityAudioObjectQueue.size() < g_ISACObjectCount)
					{
						InterlockedExchange(&g_ThereIsSpaceInUnityAudioObjectQueue, TRUE);
					}
				ReleaseMutex(g_UnityAudioObjectQueueMutex);
			}
		}
	}

	// When creating the ISAC client, we can pass in activation parameters for telemetry purposes. This includes a GUID to indicate which middleware
	// is using the API, and the version of the middleware. In this code, I use a GUID specifically for this Unity Plugin.
	HRESULT CreateSpatialAudioClientActivationParams(GUID contextId, GUID appId, int majorVer, int minorVer1, int minorVer2, int minorVer3, PROPVARIANT* pActivationParams)
	{
		PROPVARIANT var;
		PropVariantInit(&var);

		SpatialAudioClientActivationParams* params = reinterpret_cast<SpatialAudioClientActivationParams*>(CoTaskMemAlloc(sizeof(SpatialAudioClientActivationParams)));

		if (params == nullptr)
		{
			return E_OUTOFMEMORY;
		}

		params->tracingContextId = contextId;
		params->appId = appId;
		params->majorVersion = majorVer;
		params->minorVersion1 = minorVer1;
		params->minorVersion2 = minorVer2;
		params->minorVersion3 = minorVer3;
		var.vt = VT_BLOB;
		var.blob.cbSize = sizeof(*params);
		var.blob.pBlobData = reinterpret_cast<BYTE *>(params);
		*pActivationParams = var;

		return S_OK;
	}
	
	// If we're building the Plugin for use in UWP apps, then we need to Initialize ISAC using an Initializer class
#ifdef UWPBUILD
	class ISACInitializer :
		public Microsoft::WRL::RuntimeClass< Microsoft::WRL::RuntimeClassFlags< Microsoft::WRL::ClassicCom >, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler >
	{
	public:
		ISACInitializer();
		~ISACInitializer();

		Platform::String^		m_DeviceIdString;
		bool					m_ISACDeviceActive;

		ISpatialAudioClient	   *m_SpatialAudioClient;
		HANDLE                  m_CompletedEvent;
		HRESULT					m_ActivateHResult;

		HRESULT InitializeAudioDeviceAsync();

		STDMETHOD(ActivateCompleted) (IActivateAudioInterfaceAsyncOperation *operation);
	};

	ISACInitializer::ISACInitializer() :
		m_SpatialAudioClient(nullptr),
		m_ActivateHResult(E_FAIL)
	{
		m_CompletedEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}

	ISACInitializer::~ISACInitializer()
	{
		CloseHandle(m_CompletedEvent);
	}

	HRESULT ISACInitializer::InitializeAudioDeviceAsync()
	{
		ComPtr<IActivateAudioInterfaceAsyncOperation> AsyncOp;
		HRESULT hr = S_OK;
		PROPVARIANT ActivationParams;
		PROPVARIANT* p_ActivationParams = nullptr;

		// Get a string representing the Default Audio Device Renderer
		m_DeviceIdString = Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(Windows::Media::Devices::AudioDeviceRole::Default);

		// Create activation params - this specifies a GUID that lets ISAC know that the Middleware being used by the App is Unity
		hr = CreateSpatialAudioClientActivationParams(GUID_NULL, UNITY_ISAC_MIDDLEWARE_ID, MAJOR_VERSION, MINOR_VERSION1, MINOR_VERSION2, 0, &ActivationParams);
		p_ActivationParams = SUCCEEDED(hr) ? &ActivationParams : nullptr;

		// This call must be made on the main UI thread.  Async operation will call back to 
		// IActivateAudioInterfaceCompletionHandler::ActivateCompleted, which must be an agile interface implementation
		hr = ActivateAudioInterfaceAsync(m_DeviceIdString->Data(), __uuidof(ISpatialAudioClient), p_ActivationParams, this, &AsyncOp);
		if (FAILED(hr))
		{
			m_ISACDeviceActive = false;
		}

		return hr;
	}

	HRESULT ISACInitializer::ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation)
	{
		HRESULT hr = S_OK;
		IUnknown *p_AudioInterface = nullptr;

		hr = operation->GetActivateResult(&m_ActivateHResult, &p_AudioInterface);

		if (p_AudioInterface == nullptr)
		{
			hr = E_FAIL;
			goto exit;
		}

		// Finally. Get the pointer for the Spatial Audio Client Interface
		p_AudioInterface->QueryInterface(IID_PPV_ARGS(&m_SpatialAudioClient));

		if (m_SpatialAudioClient == nullptr)
		{
			hr = E_FAIL;
			goto exit;
		}

	exit:
		if (p_AudioInterface != nullptr)
		{
			p_AudioInterface->Release();
			p_AudioInterface = nullptr;
		}

		if (FAILED(hr))
		{
			if (m_SpatialAudioClient != nullptr)
			{
				m_SpatialAudioClient->Release();
				m_SpatialAudioClient = nullptr;
			}
		}

		// Signal the completion of the Asynchronous Activation operation
		SetEvent(m_CompletedEvent);
		return S_OK;
	}
		
	ISpatialAudioClient* GetSpatialAudioClientFromInitializer()
	{
		ISACInitializer Initializer;
		DWORD dwWaitResult;

		HRESULT hr;

		hr = Initializer.InitializeAudioDeviceAsync();
		if (FAILED(hr))
		{
			return nullptr;
		}

		dwWaitResult = WaitForSingleObject(Initializer.m_CompletedEvent, INFINITE);
		if (dwWaitResult == WAIT_OBJECT_0)
		{
			hr = S_OK;
		}
		else if (dwWaitResult == WAIT_TIMEOUT)
		{
			hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			//TODO ERROR HANDLING
		}
		else if (dwWaitResult == WAIT_FAILED)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			//TODO ERROR HANDLING
		}
		else
		{
			hr = E_FAIL;
			//TODO ERROR HANDLING
		}

		if (Initializer.m_ActivateHResult != S_OK)
		{
			return nullptr;
		}
		else
		{
			return Initializer.m_SpatialAudioClient;
		}
	}
#endif

	// ISAC Notifies us when its object count changes. This class is used to get that notification.
	class ISACNotify WrlSealed :
		public Microsoft::WRL::RuntimeClass<
		Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
		ISpatialAudioObjectRenderStreamNotify,
		Microsoft::WRL::FtmBase>
	{
	public:
		STDMETHOD(OnAvailableDynamicObjectCountChange)(
			_In_ ISpatialAudioObjectRenderStreamBase *sender,
			_In_ LONGLONG hnsComplianceDeadlineTime,
			_In_ UINT32 objectCount)
		{
			BOOL CountLowered = FALSE;
			UINT32 Difference = 0;

			// Change g_ISACObjectCount to reflect the new value
			DWORD dwWaitResult = WaitForSingleObject(g_ISACObjectCountMutex, INFINITE);
				if (dwWaitResult == WAIT_OBJECT_0)
				{
					if (objectCount < g_ISACObjectCount)
					{
						CountLowered = TRUE;
						Difference = g_ISACObjectCount - objectCount;
					}
					else if (objectCount > g_ISACObjectCount)
					{
						InterlockedExchange(&g_ThereIsSpaceInUnityAudioObjectQueue, TRUE);		// Indicates that there is more space in the queue
					}

					g_ISACObjectCount = objectCount;
				}
			ReleaseMutex(g_ISACObjectCountMutex);
			
			if (CountLowered)
			{
				// Resize the queue by removing elements from the end
				dwWaitResult = WaitForSingleObject(g_UnityAudioObjectQueueMutex, INFINITE);
					if (dwWaitResult == WAIT_OBJECT_0)
					{
						while (Difference > 0 && g_UnityAudioObjectQueue.size() > 0)
						{
							// Pop from queue
							UnityAudioData* p_ObjData = g_UnityAudioObjectQueue.back();
							g_UnityAudioObjectQueue.pop_back();
							Difference--;

							// Update status of object to 'not in queue'
							DWORD dwWaitResultIn = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
								if (dwWaitResultIn == WAIT_OBJECT_0)
								{
									p_ObjData->m_InQueue = FALSE;
								}
							ReleaseMutex(p_ObjData->m_Lock);
						}
					}
				ReleaseMutex(g_UnityAudioObjectQueueMutex);
			}
			return S_OK;
		}
	};
	ISACNotify g_notifyObj;

	BOOL InitializeSpatialAudioClient(int sampleRate) 
	{
		IMMDevice* p_Device = NULL;
		IMMDeviceEnumerator* p_Enumerator = NULL;
		HRESULT hr = S_OK;
		PROPVARIANT* p_ActivationParams = nullptr;

		// Reset ISAC variables in case we are restarting ISAC
		g_SpatialAudioClient = nullptr;
		g_SpatialAudioStream = nullptr;

		// ISAC only supports 48K at this point
		// TODO: Add support for other sampling rates when ISAC adds support for them.
		g_SystemSampleRate = sampleRate;
		if (g_SystemSampleRate != REQUIRED_SAMPLE_RATE)	
			return FALSE;

#ifndef UWPBUILD
		/* QUERY IMMDEVICE TO GET DEFAULT ENDPOINT AND INITIALIZE ISAC ON IT */
		CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),	(void**)&p_Enumerator);

		p_Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &p_Device);

		if (!&p_Device)
			return FALSE;

		PROPVARIANT activationParams;

		// Create activation params - this specifies a GUID that lets ISAC know that the Middleware being used by the App is Unity
		hr = CreateSpatialAudioClientActivationParams(GUID_NULL, UNITY_ISAC_MIDDLEWARE_ID, MAJOR_VERSION, MINOR_VERSION1, MINOR_VERSION2, 0, &activationParams);
		p_ActivationParams = SUCCEEDED(hr) ? &activationParams : nullptr;

		hr = p_Device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, p_ActivationParams, (void**)&g_SpatialAudioClient);
#else
		g_SpatialAudioClient = GetSpatialAudioClientFromInitializer();
#endif
	
		if (g_SpatialAudioClient == nullptr)
		{
			// Spatial Audio Client creation failed
			return FALSE;
		}
 
		// Check the available rendering formats 
		ComPtr<IAudioFormatEnumerator> AudioObjectFormatEnumerator;
		hr = g_SpatialAudioClient->GetSupportedAudioObjectFormatEnumerator(&AudioObjectFormatEnumerator);
		if (FAILED(hr))
		{
			return FALSE;
		}

		WAVEFORMATEX* p_ObjectFormat = nullptr;

		UINT32 AudioObjectFormatCount;
		hr = AudioObjectFormatEnumerator->GetCount(&AudioObjectFormatCount); // There should be at least one format that the API accepts
		if (AudioObjectFormatCount == 0)
		{
			return FALSE;
		}

		// Select the most favorable format: the first one
		hr = AudioObjectFormatEnumerator->GetFormat(0, &p_ObjectFormat);
		if (FAILED(hr))
		{
			return FALSE;
		}

		// Ask ISAC about the maximum number of objects we can have
		UINT32 MaxNumISACObjects = 0;
		hr = g_SpatialAudioClient->GetMaxDynamicObjectCount(&MaxNumISACObjects);
		if (FAILED(hr))
		{
			return FALSE;
		}

		g_ISACObjectVector.resize(MaxNumISACObjects, nullptr);

		SpatialAudioObjectRenderStreamActivationParams Params = {};
		Params.Category = AudioCategory_GameEffects;
		Params.EventHandle = g_ISACBufferCompletionEvent;
		Params.MinDynamicObjectCount = (UINT32) (0.2f * (float)MaxNumISACObjects);		// set minimum to 20% of max
		Params.MaxDynamicObjectCount = MaxNumISACObjects;
		Params.NotifyObject = &g_notifyObj;
		Params.ObjectFormat = p_ObjectFormat;
		Params.StaticObjectTypeMask = AudioObjectType_None;		// No Static bed objects

		PROPVARIANT ActivateParams;
		PropVariantInit(&ActivateParams);
		ActivateParams.vt = VT_BLOB;
		ActivateParams.blob.cbSize = sizeof(Params);
		ActivateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&Params);

		hr = g_SpatialAudioClient->ActivateSpatialAudioStream(&ActivateParams, __uuidof(ISpatialAudioObjectRenderStream), &g_SpatialAudioStream);
		if (FAILED(hr))
		{
			return FALSE;
		}

		hr = g_SpatialAudioStream->Start();
		if (FAILED(hr))
		{
			return FALSE;
		}

		return TRUE;
	}

	static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
	{
		*attenuationOut = attenuationIn;
		return UNITY_AUDIODSP_OK;
	}

	inline bool IsHostCompatible(UnityAudioEffectState* state)
    {
        // Somewhat convoluted error checking here because hostapiversion is only supported from SDK version 1.03 (i.e. Unity 5.2) and onwards.
		return
            state->structsize >= sizeof(UnityAudioEffectState) &&
            state->hostapiversion >= UNITY_AUDIO_PLUGIN_API_VERSION;
    }

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK CreateCallback(UnityAudioEffectState* state)
	{
		// Create the object which contains the buffer and variables necessary 
		// for transfer of audio data from Unity to ISAC audio objects
		UnityAudioData* p_ObjData = new UnityAudioData;
		memset (p_ObjData, 0, sizeof(UnityAudioData));

		p_ObjData->m_Lock = CreateMutex(NULL, FALSE, NULL);
		if (p_ObjData->m_Lock == NULL)
		{
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}

		state->effectdata = p_ObjData;

		// Fills in default values (from the effects definition) into the params array
		InitParametersFromDefinitions(InternalRegisterEffectDefinition, p_ObjData->p);

		// If the current Unity version supports it, set the distance attenuation callback
		if (IsHostCompatible(state))
			state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;

		// If this is the first ever create callback, we need to initialize some stuff in order
		// for ISAC to work. This includes creating mutexes and starting the Spatial Work thread.
		if (g_FirstCreateCallback)	
		{
			g_UnityAudioObjectQueueMutex = CreateMutex(NULL, FALSE, NULL);
			if (g_UnityAudioObjectQueueMutex == NULL)
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			g_ISACObjectCountMutex = CreateMutex(NULL, FALSE, NULL);
			if (g_ISACObjectCountMutex == NULL)
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			g_SystemSampleRate = state->samplerate;

			// Create event used to signal the worker thread for more data.
			g_ISACBufferCompletionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

			// Create the Spatial Work thread. This also takes care of initializing ISAC for us.
			g_WorkThreadActive = TRUE;
			g_WorkThread = CreateThreadpoolWork(SpatialWorkCallbackNew, nullptr, nullptr);
			SubmitThreadpoolWork(g_WorkThread);

			g_FirstCreateCallback = FALSE;
		}

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
	{
		UnityAudioData* objData = state->GetEffectData<UnityAudioData>();

		// Wait until the EmptyCount for the object becomes the limit
		// At that point, it would have been removed from the queue, so it is safe to delete it
		while (TRUE)
		{
			if (objData->m_InQueue == FALSE)
			{
				//Wait a millisecond before deleting it
				Sleep(1);
				delete objData;
				break;
			}
			else
			{
				// Wait 10ms until it has been removed from the queue
				Sleep(10);
			}
		}

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
	{
		UnityAudioData* p_ObjData = state->GetEffectData<UnityAudioData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		p_ObjData->p[index] = value;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
	{
		UnityAudioData* p_ObjData = state->GetEffectData<UnityAudioData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		if (value != NULL)
			*value = p_ObjData->p[index];
		if (valuestr != NULL)
			valuestr[0] = 0;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
	{
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
	{
		// If ISAC hasn't been initialized yet, or if the provided data doesn't meet ISAC's requirements, just pass it back to Unity
		if (g_SpatialAudioClientCreated != TRUE || inchannels != 2 || outchannels != 2 || g_SystemSampleRate != REQUIRED_SAMPLE_RATE)
		{
			memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}

		BOOL SendDataToISAC = TRUE;

		UnityAudioData* p_ObjData = state->GetEffectData<UnityAudioData>();

		DWORD dwWaitResult = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
			if (dwWaitResult == WAIT_OBJECT_0)
			{
				// Since this object has new data, revert EmptyCount back to 0
				p_ObjData->m_EmptyCount = 0;
			}
			ReleaseMutex(p_ObjData->m_Lock);

				// If the object isn't already in the queue, check if there's space to add it
				if (p_ObjData->m_InQueue == FALSE)
				{
					LONG ThereIsSpaceInQueue = InterlockedCompareExchange(&g_ThereIsSpaceInUnityAudioObjectQueue, 0, 0);
					BOOL ObjectQueuedToISAC = FALSE;

					// If the queue has space, lock it and try to put this object in it
					if (ThereIsSpaceInQueue)
					{
						// Get how many objects ISAC can render in the next processing pass
						DWORD dwWaitResultIn = WaitForSingleObject(g_ISACObjectCountMutex, INFINITE);
							if (dwWaitResultIn == WAIT_OBJECT_0)
							{
								DWORD dwWaitResultInIn = WaitForSingleObject(g_UnityAudioObjectQueueMutex, INFINITE);
									if (dwWaitResultInIn == WAIT_OBJECT_0)
									{
										ObjectQueuedToISAC = g_UnityAudioObjectQueue.size() < g_ISACObjectCount;

										// Only queue this object to be rendered by ISAC if the queue has enough capacity 
										if (ObjectQueuedToISAC)
										{
											g_UnityAudioObjectQueue.push_back(p_ObjData);
											DWORD dwWaitResult = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
											if (dwWaitResult == WAIT_OBJECT_0)
											{
												// Set Read and Write Indexes back to 0 in case this object was taken
												// off queue so that ISAC doesn't render stale data.
												p_ObjData->m_ReadIndex = 0;
												p_ObjData->m_WriteIndex = 0;
												p_ObjData->m_WriteIndexOverflowed = FALSE;
												p_ObjData->m_EmptyCount = 0;

												p_ObjData->m_UnityAudioObjectQueueIter = --g_UnityAudioObjectQueue.end();
												p_ObjData->m_InQueue = TRUE;
											}
											ReleaseMutex(p_ObjData->m_Lock);

											if (g_UnityAudioObjectQueue.size() == g_ISACObjectCount)
											{
												InterlockedExchange(&g_ThereIsSpaceInUnityAudioObjectQueue, FALSE);
											}
										}
									}
								ReleaseMutex(g_UnityAudioObjectQueueMutex);
							}
						ReleaseMutex(g_ISACObjectCountMutex);
					}

					if (!ObjectQueuedToISAC)
					{
						// If the queue didn't have enough space, then send the data back to Unity
						memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
						SendDataToISAC = FALSE;
					}
				}

				if (SendDataToISAC)
				{
					memset(outbuffer, 0, length);	// Send back silence to Unity since this will be rendered by ISAC

					// Convert position data from Unity's coordinate system to ISAC's coordinate system
					float* m = state->spatializerdata->listenermatrix;
					float* s = state->spatializerdata->sourcematrix;

					// Currently we ignore source orientation and only use source position
					float px = s[12];
					float py = s[13];
					float pz = s[14];

					float dir_x = m[0] * px + m[4] * py + m[8] * pz + m[12];
					float dir_y = m[1] * px + m[5] * py + m[9] * pz + m[13];
					float dir_z = m[2] * px + m[6] * py + m[10] * pz + m[14];

					DWORD dwWaitResult = WaitForSingleObject(p_ObjData->m_Lock, INFINITE);
					if (dwWaitResult == WAIT_OBJECT_0)
					{

						for (UINT32 inx = 0; inx < length; inx++)
						{
							p_ObjData->m_WriteIndex++;

							if (p_ObjData->m_WriteIndex >= ISAC_CALLBACK_BUF_SIZE)
							{
								p_ObjData->m_WriteIndex -= ISAC_CALLBACK_BUF_SIZE;
								p_ObjData->m_WriteIndexOverflowed = TRUE;
							}

							p_ObjData->m_DataPosX[p_ObjData->m_WriteIndex] = dir_x;
							p_ObjData->m_DataPosY[p_ObjData->m_WriteIndex] = dir_y;
							p_ObjData->m_DataPosZ[p_ObjData->m_WriteIndex] = -dir_z;

							p_ObjData->m_DataBuf[p_ObjData->m_WriteIndex] = inbuffer[inx * 2];
						}
					}
					ReleaseMutex(p_ObjData->m_Lock);
				}


		return UNITY_AUDIODSP_OK;
	}
}

