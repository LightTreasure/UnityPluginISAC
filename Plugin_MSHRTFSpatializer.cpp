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

namespace MSHRTFSpatializer
{
	// because of the way HRTF is initialized, and because the unity plugin is initialized before we can touch it via a script ...
	//  if we want to expose all parameters we have to re-initialize the HRTF object inside ProcessCallback on first run if a param changes
	float m_currentEnvironment = 1.f;	// these are the windows default values for these params
	float m_currentMingain = -96.f;
	float m_currentMaxgain = 12.f;
	float m_currentUnitygain = 1.f;
	float m_bypass_attenuation = 0.f;
	
	int g_SystemSampleRate = 0;	// as of 2016, HRTF only supports 48k input/output
	const int REQUIREDSAMPLERATE = 48000;
	UINT32 frameCount=NULL;

#define ISACCALLBACKBUFSIZE 48000
#define EMPTY_COUNT_LIMIT 5

	enum
	{
		P_CUTOFFDIST=0,
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

		float	m_dataBuf [ISACCALLBACKBUFSIZE];
		float	m_dataPosX[ISACCALLBACKBUFSIZE];
		float	m_dataPosY[ISACCALLBACKBUFSIZE];
		float	m_dataPosZ[ISACCALLBACKBUFSIZE];

		UINT32	m_readIndex = 0;
		UINT32	m_writeIndex = 0;
		BOOL	m_writeIndexOverflowed = FALSE;

		UINT32  m_EmptyCount = 0;

		BOOL	m_InQueue = FALSE;
		std::list<UnityAudioData *>::iterator iter;

		HANDLE  m_lock = nullptr;
	};

	/* OBJECTS USED TO COMMUNICSTE BETWEEN UNITY AND ISAC */
	UINT32 g_ISACObjectCount = 0;
	HANDLE g_ISACObjectCountMutex = nullptr;

	std::list<UnityAudioData *> g_UnityAudioObjectQueue;
	HANDLE g_QueueMutex;

	std::vector<ComPtr<ISpatialAudioObject>> g_ISACObjectVector;
	HANDLE g_ISACObjectVectorMutex = nullptr;

	/* ISAC VARIABLES */
	IMMDevice* g_pDevice = NULL;
	IMMDeviceEnumerator* g_pEnumerator = NULL;
	ComPtr<ISpatialAudioObjectRenderStream> g_SpatialAudioStream;
	ComPtr<ISpatialAudioClient> g_SpatialAudioClient;
	HANDLE g_ISACBufferCompletionEvent;

	/* STATE TRACKING VARIABLES */
	BOOL g_FirstCreateCallback = TRUE;
	BOOL g_SpatialAudioClientCreated = FALSE;

	// This GUID uniquely identifies a Middleware Stack. WWise, FMod etc each will need to have their own GUID
	// that should never change.
	// We will log this value as part of spatial audio client telemetry; and map the GUIDs to middleware
	// while processing the telemetry, so we can filter telemetry by middleware.
	const GUID UNITY_ISAC_MIDDLEWARE_ID = { 0xe07049bc, 0xa91e, 0x489d,{ 0xad, 0xeb, 0xb1, 0x70, 0xa4, 0xa, 0x30, 0x6f } };

	// Middleware can use up to 4 integers to pass the version info
	const int MAJOR_VERSION = 0;
	const int MINOR_VERSION1 = 2;
	const int MINOR_VERSION2 = 0;

	/* MUTEXES */
	HANDLE g_ObjListsMutex = nullptr;

	// Worker thread for ISpatialAudioClient work
	PTP_WORK g_WorkThread;
	BOOL g_WorkThreadActive = FALSE;

	void FillPcmFormat(_Out_ WAVEFORMATEX* pFormat, WORD wChannels, int nSampleRate)
	{
		pFormat->wFormatTag = WAVE_FORMAT_PCM;
		pFormat->nChannels = wChannels;
		pFormat->nSamplesPerSec = nSampleRate;
		pFormat->wBitsPerSample = 16;	// dont hard-code this
		pFormat->nBlockAlign = pFormat->nChannels * (pFormat->wBitsPerSample / 8);
		pFormat->nAvgBytesPerSec = pFormat->nSamplesPerSec * pFormat->nBlockAlign;
		pFormat->cbSize = 0;
	}

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

	VOID CALLBACK SpatialWorkCallbackNew(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_WORK Work)
	{
		HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		Work;
		Instance;

		while (g_WorkThreadActive)
		{
			// WAIT FOR ISAC EVENT
			if (WaitForSingleObject(g_ISACBufferCompletionEvent, INFINITE) != WAIT_OBJECT_0)
			{
				continue;
			}

			std::list<UnityAudioData *> RemoveQueue;

			// Grab the ISACObjectsVector Mutex NOT NEEDED REMOVE
			DWORD dwWaitResult = WaitForSingleObject(g_ISACObjectVectorMutex, INFINITE);
				if (dwWaitResult == WAIT_OBJECT_0)	
				{
					// Get the Current Queue
					std::list<UnityAudioData *> LocalCopyOfQueue;

					DWORD dwWaitResult = WaitForSingleObject(g_QueueMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
						if (dwWaitResult == WAIT_OBJECT_0)
						{
							// Make a local copy of the queue
							LocalCopyOfQueue = g_UnityAudioObjectQueue;
						}
						else if (dwWaitResult == WAIT_ABANDONED)
						{
							// Not sure what to do here
						}
					ReleaseMutex(g_QueueMutex);

					UINT32 frameCount;
					UINT32 availableObjectCount;

					UINT32 ISACObjInx = 0;

					hr = g_SpatialAudioStream->BeginUpdatingAudioObjects(
						&availableObjectCount,
						&frameCount);
				
					// Go through the current read queue and copy data to ISAC Objects, if available
					for (std::list<UnityAudioData*>::iterator iter = LocalCopyOfQueue.begin(); iter != LocalCopyOfQueue.end(); iter++)
					{
						UnityAudioData *objData = *iter;

						// Defensive check
						if (ISACObjInx >= availableObjectCount)
						{
							continue;
						}

						ComPtr<ISpatialAudioObject> &objISAC = g_ISACObjectVector[ISACObjInx];
						ISACObjInx++;
					
						if (objISAC == nullptr)
						{
							hr = g_SpatialAudioStream->ActivateSpatialAudioObject(
								AudioObjectType_Dynamic,
								&objISAC);
							if (FAILED(hr))
							{
								continue;
							}
						}

						BOOL isactive = FALSE;
						objISAC->IsActive(&isactive);
						if (!isactive)
						{
							objISAC = nullptr;

							hr = g_SpatialAudioStream->ActivateSpatialAudioObject(
								AudioObjectType_Dynamic,
								&objISAC);
							if (FAILED(hr))
							{
								continue;
							}
						}
					

						DWORD dwWaitResultIn = WaitForSingleObject(objData->m_lock, INFINITE);
						if (dwWaitResultIn == WAIT_OBJECT_0)
						{
							UINT32 actualWriteIndex = objData->m_writeIndexOverflowed ? objData->m_writeIndex + ISACCALLBACKBUFSIZE : objData->m_writeIndex;

							// TODO: change 480, 4, and 1920 in the following code to #def'd variables
							BOOL enoughData = ((float)actualWriteIndex - (float)objData->m_readIndex) >= 480.0f;

							//Get the object buffer
							BYTE* buffer = nullptr;
							UINT32 bytecount;
							hr = objISAC->GetBuffer(&buffer, &bytecount);
							if (FAILED(hr))
							{
								//TODO: how do we handle this in a better way?
								continue;
							}

							objISAC->SetPosition(objData->m_dataPosX[objData->m_readIndex],
								objData->m_dataPosY[objData->m_readIndex],
								objData->m_dataPosZ[objData->m_readIndex]);

							//objISAC->SetPosition(0.5f, 0.0f, 0.7f);

							objISAC->SetVolume(1.0f);

							if (enoughData)
							{
								for (UINT32 inx = 0; inx < 480; inx++)
								{
									*((float*)buffer) = objData->m_dataBuf[objData->m_readIndex];
									//*((float*)buffer) = ((float) rand()) / ((float)RAND_MAX + 1.0f);
									buffer += 4;

									objData->m_readIndex++;
									if (objData->m_readIndex >= ISACCALLBACKBUFSIZE)
									{
										objData->m_readIndex -= ISACCALLBACKBUFSIZE;
										objData->m_writeIndexOverflowed = FALSE;
									}
								}
							}
							else
							{
								objData->m_EmptyCount++;

								if (objData->m_EmptyCount == EMPTY_COUNT_LIMIT)
								{
									// Put into Remove Queue
									RemoveQueue.push_back(objData);
								}

								// fill with silence
								for (UINT32 inx = 0; inx < 480; inx++)
								{
									*((float*)buffer) = 0.0f;
									buffer += 4;
								}
							}
						}
						ReleaseMutex(objData->m_lock);
					}

					// Let the audio-engine know that the object data are available for processing now 
					hr = g_SpatialAudioStream->EndUpdatingAudioObjects();
					if (FAILED(hr))
					{
						// TODO: WHAT TO DO HERE
						continue;
					}

					// REMOVE INACTIVE OBJECTS FROM QUEUE

					if (!RemoveQueue.empty())
					{
						DWORD dwWaitResult = WaitForSingleObject(g_QueueMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
							if (dwWaitResult == WAIT_OBJECT_0)
							{
								// Go through the remove queue and remove the elements in it from the global queue
								while (!RemoveQueue.empty())
								{
									UnityAudioData *objData = RemoveQueue.front();
									RemoveQueue.pop_front();

									// Check one last time before removing
									DWORD dwWaitResultIn = WaitForSingleObject(objData->m_lock, INFINITE);
										if (dwWaitResultIn == WAIT_OBJECT_0)
										{
											if (objData->m_EmptyCount == EMPTY_COUNT_LIMIT)
											{
												g_UnityAudioObjectQueue.erase(objData->iter);
												objData->m_InQueue = FALSE;
											}
										}
									ReleaseMutex(objData->m_lock);
								}
							}
							else if (dwWaitResult == WAIT_ABANDONED)
							{
								// Not sure what to do here
							}
						ReleaseMutex(g_QueueMutex);
					}
				}
				else 
				{
					// TODO: what do we do here?
					continue;
				}
			ReleaseMutex(g_ISACObjectVectorMutex);
		}
	}

	HRESULT CreateSpatialAudioClientActivationParams(GUID contextId, GUID appId, int majorVer, int minorVer1, int minorVer2, int minorVer3, PROPVARIANT* pActivationParams)
	{
		PROPVARIANT var;
		PropVariantInit(&var);

		// SpatialAudioClientActivationParams is defined in latest spatialaudioclient.idl in _media_dev
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
		ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
		HRESULT hr = S_OK;
		PROPVARIANT activationParams;
		PROPVARIANT* pActivationParams = nullptr;

		// Get a string representing the Default Audio Device Renderer
		m_DeviceIdString = Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(Windows::Media::Devices::AudioDeviceRole::Default);

		// Create activation params - this specifies a GUID that lets ISAC know that the Middleware being used by the App is Unity
		hr = CreateSpatialAudioClientActivationParams(GUID_NULL, UNITY_ISAC_MIDDLEWARE_ID, MAJOR_VERSION, MINOR_VERSION1, MINOR_VERSION2, 0, &activationParams);
		pActivationParams = SUCCEEDED(hr) ? &activationParams : nullptr;

		// This call must be made on the main UI thread.  Async operation will call back to 
		// IActivateAudioInterfaceCompletionHandler::ActivateCompleted, which must be an agile interface implementation
		hr = ActivateAudioInterfaceAsync(m_DeviceIdString->Data(), __uuidof(ISpatialAudioClient), pActivationParams, this, &asyncOp);
		if (FAILED(hr))
		{
			m_ISACDeviceActive = false;
		}

		return hr;
	}

	HRESULT ISACInitializer::ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation)
	{
		HRESULT hr = S_OK;

		IUnknown *punkAudioInterface = nullptr;

		hr = operation->GetActivateResult(&m_ActivateHResult, &punkAudioInterface);

		if (nullptr == punkAudioInterface)
		{
			hr = E_FAIL;
			goto exit;
		}

		// Finally. Get the pointer for the Spatial Audio Client Interface
		punkAudioInterface->QueryInterface(IID_PPV_ARGS(&m_SpatialAudioClient));

		if (nullptr == m_SpatialAudioClient)
		{
			hr = E_FAIL;
			goto exit;
		}

	exit:
		if (punkAudioInterface != NULL)
		{
			punkAudioInterface->Release();
			punkAudioInterface = NULL;
		}

		if (FAILED(hr))
		{
			if (m_SpatialAudioClient != NULL)
			{
				m_SpatialAudioClient->Release();
				m_SpatialAudioClient = NULL;
			}
		}

		//Signal the completion of the Asynchronous Activation operation
		SetEvent(m_CompletedEvent);
		return S_OK;
	}

	
	ISpatialAudioClient* GetSpatialAudioClientFromInitializer()
	{
		ISACInitializer init;
		DWORD waitResult;

		HRESULT hr;

		hr = init.InitializeAudioDeviceAsync();
		if (FAILED(hr))
		{
			return nullptr;
		}

		waitResult = WaitForSingleObject(init.m_CompletedEvent, INFINITE);
		if (WAIT_OBJECT_0 == waitResult)
		{
			hr = S_OK;
		}
		else if (WAIT_TIMEOUT == waitResult)
		{
			hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			//TODO ERROR HANDLING
		}
		else if (WAIT_FAILED == waitResult)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());

			HRESULT hr2 = hr;

			if (FAILED(hr))
			{
				HRESULT hr3 = hr;
			}
			//TODO ERROR HANDLING
		}
		else
		{
			hr = E_FAIL;
			//TODO ERROR HANDLING
		}

		if (init.m_ActivateHResult != S_OK)
		{
			return nullptr;
		}
		else
		{
			return init.m_SpatialAudioClient;
		}
	}

#endif

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
			BOOL countLowered = FALSE;
			UINT32 difference = 0;

			// store the new count in the global 
			DWORD dwWaitResult = WaitForSingleObject(g_ISACObjectCountMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
				if (dwWaitResult == WAIT_OBJECT_0)
				{
					if (objectCount < g_ISACObjectCount)
					{
						countLowered = TRUE;
						difference = g_ISACObjectCount - objectCount;
					}

					g_ISACObjectCount = objectCount;
				}
				else if (dwWaitResult == WAIT_ABANDONED)
				{
					// Not sure what to do here
				}
			ReleaseMutex(g_ISACObjectCountMutex);
			
			if (countLowered)
			{
				// Resize the queue by removing elements from the end
				dwWaitResult = WaitForSingleObject(g_QueueMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
					if (dwWaitResult == WAIT_OBJECT_0)
					{
						while (difference > 0 && g_UnityAudioObjectQueue.size() > 0)
						{
							// pop from queue
							UnityAudioData* objData = g_UnityAudioObjectQueue.back();
							g_UnityAudioObjectQueue.pop_back();
							difference--;

							// update status of object to 'not in queue'
							DWORD dwWaitResultIn = WaitForSingleObject(objData->m_lock, INFINITE);
								if (dwWaitResultIn == WAIT_OBJECT_0)
								{
									objData->m_InQueue = FALSE;
								}
								else if (dwWaitResult == WAIT_ABANDONED)
								{
									// Not sure what to do here
								}
							ReleaseMutex(objData->m_lock);
						}
					}
					else if (dwWaitResult == WAIT_ABANDONED)
					{
						// Not sure what to do here
					}
				ReleaseMutex(g_QueueMutex);
			}
			return S_OK;
		}
	};

	ISACNotify g_notifyObj;

	bool InitializeSpatialAudioClient(int sampleRate) 
	{	
		// ISAC only supports 48K at this point
		// TODO: Add support for other sampling rates when ISAC adds support for them.
		g_SystemSampleRate = sampleRate;
		if (g_SystemSampleRate != REQUIREDSAMPLERATE)	
			return false;

		HRESULT hr = S_OK;
		PROPVARIANT* pActivationParams = nullptr;

#ifndef UWPBUILD
		/* QUERY IMMDEVICE TO GET DEFAULT ENDPOINT AND INITIALIZE ISAC ON IT */
		CoCreateInstance(
			__uuidof(MMDeviceEnumerator), NULL,
			CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			(void**)&g_pEnumerator);

		g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);

		if (!&g_pDevice)
			return false;

		PROPVARIANT activationParams;

		// Create activation params - this specifies a GUID that lets ISAC know that the Middleware being used by the App is Unity
		hr = CreateSpatialAudioClientActivationParams(GUID_NULL, UNITY_ISAC_MIDDLEWARE_ID, MAJOR_VERSION, MINOR_VERSION1, MINOR_VERSION2, 0, &activationParams);
		pActivationParams = SUCCEEDED(hr) ? &activationParams : nullptr;

		hr = g_pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, pActivationParams, (void**)&g_SpatialAudioClient);
#else
		g_SpatialAudioClient = GetSpatialAudioClientFromInitializer();
#endif
	
		if (g_SpatialAudioClient == nullptr)
		{
			// Spatial Audio Client creation failed
			return false;
		}

		/* NOW THAT WE HAVE ISAC, QUERY IT FOR RELEVANT DATA BEFORE CREATING A STREAM */
		// Check the available rendering formats 
		ComPtr<IAudioFormatEnumerator> audioObjectFormatEnumerator;
		hr = g_SpatialAudioClient->GetSupportedAudioObjectFormatEnumerator(&audioObjectFormatEnumerator);
		if (FAILED(hr))
		{
			return false;
		}

		WAVEFORMATEX* objectFormat = nullptr;

		UINT32 audioObjectFormatCount;
		hr = audioObjectFormatEnumerator->GetCount(&audioObjectFormatCount); // There should be at least one format that the API accepts
		if (audioObjectFormatCount == 0)
		{
			return false;
		}

		// Select the most favorable format: the first one
		hr = audioObjectFormatEnumerator->GetFormat(0, &objectFormat);
		if (FAILED(hr))
		{
			return false;
		}

		// Create the event that will be used to signal the client for more data
		g_ISACBufferCompletionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		UINT32 maxNumISACObjects = 0;

		// Ask ISAC about the maximum number of objects we can have
		hr = g_SpatialAudioClient->GetMaxDynamicObjectCount(&maxNumISACObjects);
		if (FAILED(hr) || maxNumISACObjects == 0)
		{
			return false;
		}

		g_ISACObjectVector.resize(maxNumISACObjects, nullptr);

		SpatialAudioObjectRenderStreamActivationParams params = {};
		params.Category = AudioCategory_GameEffects;
		params.EventHandle = g_ISACBufferCompletionEvent;
		params.MinDynamicObjectCount = (UINT32) (0.2f * (float)maxNumISACObjects);		// set minimum to 20% of max
		params.MaxDynamicObjectCount = maxNumISACObjects;
		params.NotifyObject = &g_notifyObj;
		params.ObjectFormat = objectFormat;
		params.StaticObjectTypeMask = AudioObjectType_None;		// No Static bed objects

		PROPVARIANT activateParams;
		PropVariantInit(&activateParams);
		activateParams.vt = VT_BLOB;
		activateParams.blob.cbSize = sizeof(params);
		activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);


		hr = g_SpatialAudioClient->ActivateSpatialAudioStream(
			&activateParams,
			__uuidof(ISpatialAudioObjectRenderStream),
			&g_SpatialAudioStream
		);

		if (FAILED(hr))
		{
			return false;
		}

		//g_SpatialAudioStream->GetFrameCount(&frameCount);

		hr = g_SpatialAudioStream->Start();
		if (FAILED(hr))
		{
			return false;
		}
		
		g_FirstCreateCallback = FALSE;

		/* CREATE AND START A WORKER THREAD TO LISTEN FOR ISAC EVENTS */
		g_WorkThreadActive = TRUE;
		g_WorkThread = CreateThreadpoolWork(SpatialWorkCallbackNew, nullptr, nullptr);
		SubmitThreadpoolWork(g_WorkThread); 

		return true;
	}

	static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
	{
		UnityAudioData* data = state->GetEffectData<UnityAudioData>();
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
		UnityAudioData* objData = new UnityAudioData;
		memset (objData, 0, sizeof(UnityAudioData));
		objData->m_lock = CreateMutex(NULL, FALSE, NULL);
		if (objData->m_lock == NULL)
		{
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}

		state->effectdata = objData;

		// Fills in default values (from the effects definition) into the params array
		InitParametersFromDefinitions(InternalRegisterEffectDefinition, objData->p);

		// If the current Unity version supports it, set the distance attenuation callback
		if (IsHostCompatible(state))
			state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;

		// If ISAC hasn't been initialized yet, initialize it and start the ISAC worker thread
		if (g_FirstCreateCallback)	
		{
			g_QueueMutex = CreateMutex(NULL, FALSE, NULL);
			if (g_QueueMutex == NULL)
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			g_ISACObjectCountMutex = CreateMutex(NULL, FALSE, NULL);
			if (g_ISACObjectCountMutex == NULL)
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			g_ISACObjectVectorMutex = CreateMutex(NULL, FALSE, NULL);
			if (g_ISACObjectVectorMutex == NULL)
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			if (!InitializeSpatialAudioClient(state->samplerate))
			{
				return UNITY_AUDIODSP_ERR_UNSUPPORTED;
			}

			g_SpatialAudioClientCreated = TRUE;
		}

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
	{
		UnityAudioData* objData = state->GetEffectData<UnityAudioData>();

		// Wait until the EmptyCount for the object becomes the limit
		// At that point, it would have been removed from the queue, so it is safe to delete it
		while (true)
		{
			if (objData->m_InQueue == FALSE)
			{
				//Wait a little before deleting it
				Sleep(1);
				delete objData;
				break;
			}
			else
			{
				// Wait for a while until it has been removed from the queue
				Sleep(10);
			}
		}

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
	{
		UnityAudioData* objData = state->GetEffectData<UnityAudioData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		objData->p[index] = value;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
	{
		UnityAudioData* objData = state->GetEffectData<UnityAudioData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		if (value != NULL)
			*value = objData->p[index];
		if (valuestr != NULL)
			valuestr[0] = 0;
		return UNITY_AUDIODSP_OK;
	}

	int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
	{
		return UNITY_AUDIODSP_OK;
	}

#pragma optimize("", off)

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
	{
		// If ISAC hasn't been initialized yet, or if the provided data doesn't meet ISAC's requirements, just pass it back to Unity
		if (g_SpatialAudioClientCreated != TRUE || inchannels != 2 || outchannels != 2 || g_SystemSampleRate != REQUIREDSAMPLERATE)
		{
			memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}

		UnityAudioData* objData = state->GetEffectData<UnityAudioData>();

		// Pre-emptively copy data over to the buffer that will be rendered by ISAC.
		// If, at the end, we decide this data won't be rendered by ISAC, then we will
		// just revert the buffer write index to its original value and pretend as if
		// we never wrote the data.
		UINT32 origWriteIndex = objData->m_writeIndex;
		BOOL origWriteIndexOverflowed = objData->m_writeIndexOverflowed;

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

		DWORD dwWaitResult = WaitForSingleObject(objData->m_lock, INFINITE); // Infinite because this thread can afford to wait
			if (dwWaitResult == WAIT_OBJECT_0)
			{
				objData->m_EmptyCount = 0;

				for (UINT32 inx = 0; inx < length; inx++)
				{
					objData->m_writeIndex++;

					if (objData->m_writeIndex >= ISACCALLBACKBUFSIZE)
					{
						objData->m_writeIndex -= ISACCALLBACKBUFSIZE;
						objData->m_writeIndexOverflowed = TRUE;
					}

					objData->m_dataPosX[objData->m_writeIndex] = dir_x;
					objData->m_dataPosY[objData->m_writeIndex] = dir_y;
					objData->m_dataPosZ[objData->m_writeIndex] = -dir_z;

					objData->m_dataBuf[objData->m_writeIndex] = inbuffer[inx * 2];
				}

				memset(outbuffer, 0, length);	// This means we're not going to be returning data back to Unity.
												// If we are, then this buffer will be filled back below.

				// Now, if the object isn't already in the queue, add it in if there's enough space
				if (objData->m_InQueue == FALSE)
				{
					UINT32 curISACObjCount = 0;
					bool enoughSpaceInQueue = false;

					// Get how many objects ISAC can render in the next processing pass
					// TODO: Use Interlocked functions to deal with this variable instead?
					DWORD dwWaitResult = WaitForSingleObject(g_ISACObjectCountMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
						if (dwWaitResult == WAIT_OBJECT_0)
						{
							curISACObjCount = g_ISACObjectCount;
						}
						else if (dwWaitResult == WAIT_ABANDONED)
						{
							// Not sure what to do here
						}
					ReleaseMutex(g_ISACObjectCountMutex);

					// Read Queue size
					dwWaitResult = WaitForSingleObject(g_QueueMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
						if (dwWaitResult == WAIT_OBJECT_0)
						{
							enoughSpaceInQueue = g_UnityAudioObjectQueue.size() < curISACObjCount;

							// Only queue this object to be rendered by ISAC if the queue has enough capacity 
							if (enoughSpaceInQueue)
							{
								g_UnityAudioObjectQueue.push_back(objData);
								objData->iter = --g_UnityAudioObjectQueue.end();
								objData->m_InQueue = TRUE;
							}
						}
						else if (dwWaitResult == WAIT_ABANDONED)
						{
							// Not sure what to do here
						}
					ReleaseMutex(g_QueueMutex);

					if (!enoughSpaceInQueue)
					{
						// If the queue didn't have enough space, then send the data back to Unity
						// This also means we need to return the buffer write index to its original value

						// copy data back to Unity
						memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
						objData->m_writeIndex = origWriteIndex;
						objData->m_writeIndexOverflowed = origWriteIndexOverflowed;
					}
				}
			}
		ReleaseMutex(objData->m_lock);

		return UNITY_AUDIODSP_OK;
	}
}
#pragma optimize("", on)

