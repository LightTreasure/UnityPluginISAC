#include "AudioPluginUtil.h"

#include <wrl/client.h>
#include <xapo.h>
#include "hrtfapoapi.h"
#include <DirectXMath.h>

#include "spatialaudioclient.h"
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

#define ISACCALLBACKBUFSIZE 96000

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

	struct ISACAudioObjectData
	{
		BOOL	m_isActive = FALSE;
		float	m_dataBuf[ISACCALLBACKBUFSIZE];
		float	m_dataPosX[ISACCALLBACKBUFSIZE];
		float	m_dataPosY[ISACCALLBACKBUFSIZE];
		float	m_dataPosZ[ISACCALLBACKBUFSIZE];

		UINT32	m_readIndex = 0;
		UINT32	m_writeIndex = 0;
		BOOL	m_writeIndexOverflowed = FALSE;

		ComPtr<ISpatialAudioObject> object = nullptr;
	};

	struct UnityAudioObjectData
	{
		float p[P_NUM];
		int ObjectIndex = -1;
	};

	/* ISAC VARIABLES */
	IMMDevice* g_pDevice = NULL;
	IMMDeviceEnumerator* g_pEnumerator = NULL;
	ComPtr<ISpatialAudioObjectRenderStream> g_SpatialAudioStream;
	ComPtr<ISpatialAudioClient> g_SpatialAudioClient;
	HANDLE g_ISACBufferCompletionEvent;

	/* OBJECT VARIABLES */
	ISACAudioObjectData* g_AudioObjectArray = nullptr;  // <-- needs  mutex protection
	UINT32 g_NumAudioObjects = 0;
	std::list<UINT32> g_FreeISACAudioObjectIndices; // Array of indices of Audio Objects that haven't been used yet <-- needs  mutex protection
	std::list<UINT32> g_UsedISACAudioObjectIndices; // Array of indices of Audio Objects that are being used <-- needs  mutex protection

	BOOL g_FirstCreateCallback = TRUE;

	std::list<UnityAudioObjectData*> g_ISACObjectWaitList; // <-- needs mutex protection

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

			// NOW GRAB THE LISTS MUTEX
			DWORD dwWaitResult = WaitForSingleObject(g_ObjListsMutex, INFINITE); //TODO: Maybe a different timeout interval?

			if (dwWaitResult == WAIT_OBJECT_0)	
			{
				UINT32 frameCount;
				UINT32 availableObjectCount;

				hr = g_SpatialAudioStream->BeginUpdatingAudioObjects(
					&availableObjectCount,
					&frameCount);

				// GO THROUGH THE USED ISAC OBJECTS LIST AND COPY OVER DATA TO ISAC
				for (std::list<UINT32>::iterator iter = g_UsedISACAudioObjectIndices.begin(); iter != g_UsedISACAudioObjectIndices.end(); iter++)
				{
					ISACAudioObjectData* isacObjData = g_AudioObjectArray + *iter;

					UINT32 actualWriteIndex = isacObjData->m_writeIndexOverflowed ? isacObjData->m_writeIndex + ISACCALLBACKBUFSIZE : isacObjData->m_writeIndex;

					// TODO: change 480, 4, and 1920 in the following code to #def'd variables
					BOOL enoughData = (actualWriteIndex - isacObjData->m_readIndex) >= 480;

					//Get the object buffer
					BYTE* buffer = nullptr;
					UINT32 bytecount;
					hr = isacObjData->object->GetBuffer(&buffer, &bytecount);
					if (FAILED(hr))
					{
						//TODO: how do we handle this in a better way?
						continue;
					}

					isacObjData->object->SetPosition(isacObjData->m_dataPosX[isacObjData->m_readIndex],
						isacObjData->m_dataPosY[isacObjData->m_readIndex],
						isacObjData->m_dataPosZ[isacObjData->m_readIndex]);

					isacObjData->object->SetVolume(1.f);

					if (enoughData)
					{
						for (UINT32 inx = 0; inx < 480; inx++)
						{
							*((float*)buffer) = isacObjData->m_dataBuf[isacObjData->m_readIndex];
							buffer += 4;

							isacObjData->m_readIndex++;
							if (isacObjData->m_readIndex >= ISACCALLBACKBUFSIZE)
							{
								isacObjData->m_readIndex -= ISACCALLBACKBUFSIZE;
							}
						}
					}
					else
					{
						// fill with silence
						for (UINT32 inx = 0; inx < 480; inx++)
						{
							*((float*)buffer) = 0.0f;
							buffer += 4;
						}

					}
					buffer = buffer - (1920);
				}

				// Let the audio-engine know that the object data are available for processing now 
				hr = g_SpatialAudioStream->EndUpdatingAudioObjects();
				if (FAILED(hr))
				{
					// TODO: WHAT TO DO HERE
					continue;
				}
			}
			else 
			{
				// TODO: what do we do here?
				continue;
			}
			ReleaseMutex(g_ObjListsMutex);
		}
	}

	void InitializeSpatialAudioClient(int sampleRate) 
	{	
		g_SystemSampleRate = sampleRate;
		if (g_SystemSampleRate != REQUIREDSAMPLERATE)	// as of 2016, if not 48k samplerate, MS HRTF will die
			return;

		/* QUERY IMMDEVICE TO GET DEFAULT ENDPOINT AND INITIALIZE ISAC ON IT */
		CoCreateInstance(
			__uuidof(MMDeviceEnumerator), NULL,
			CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			(void**)&g_pEnumerator);

		g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);

		if (!&g_pDevice)
			return;

		HRESULT hr = g_pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)&g_SpatialAudioClient);

		/* NOW THAT WE HAVE ISAC, QUERY IT FOR RELEVANT DATA BEFORE CREATING A STREAM */
		// Check the available rendering formats 
		ComPtr<IAudioFormatEnumerator> audioObjectFormatEnumerator;
		hr = g_SpatialAudioClient->GetSupportedAudioObjectFormatEnumerator(&audioObjectFormatEnumerator);

		WAVEFORMATEX* objectFormat = nullptr;

		UINT32 audioObjectFormatCount;
		hr = audioObjectFormatEnumerator->GetCount(&audioObjectFormatCount); // There should be at least one format that the API accepts
		if (audioObjectFormatCount == 0)
		{
			return;
		}

		// Select the most favorable format: the first one
		hr = audioObjectFormatEnumerator->GetFormat(0, &objectFormat);

		// Create the event that will be used to signal the client for more data
		g_ISACBufferCompletionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		// Ask ISAC about how many objects we will have
		hr = g_SpatialAudioClient->GetAvailableDynamicObjectCount(&g_NumAudioObjects);

		/*TODO: EXPERIMENTAL. REMOVE LATER.*/
		if (g_NumAudioObjects > 100)
		{
			g_NumAudioObjects = 100;
		}

		/* CREATE AND INITIALIZE METADATA FOR THESE OBJECTS */
		g_AudioObjectArray = new ISACAudioObjectData[g_NumAudioObjects + 10];

		// Initialize all these objects as free
		for (UINT32 inx = 0; inx < g_NumAudioObjects; inx++)
		{
			g_FreeISACAudioObjectIndices.push_back(inx);
		}

		/* CREATE AND START SPATIAL AUDIO STREAM */
		hr = g_SpatialAudioClient->ActivateSpatialAudioObjectRenderStream(
			objectFormat,
			0,
			g_NumAudioObjects,
			AudioCategory_GameEffects,
			g_ISACBufferCompletionEvent,
			nullptr,
			&g_SpatialAudioStream);

		if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
		{
			// TODO: More error handling
			return;
		}

		g_SpatialAudioStream->GetFrameCount(&frameCount);

		hr = g_SpatialAudioStream->Start();
		
		g_FirstCreateCallback = FALSE;

		/* CREATE AND START A WORKER THREAD TO LISTEN FOR ISAC EVENTS */
		g_WorkThreadActive = TRUE;
		g_WorkThread = CreateThreadpoolWork(SpatialWorkCallbackNew, nullptr, nullptr);
		SubmitThreadpoolWork(g_WorkThread); 
	}

	static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
	{
		UnityAudioObjectData* data = state->GetEffectData<UnityAudioObjectData>();
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
		UnityAudioObjectData* unityObjData = new UnityAudioObjectData;
		memset(unityObjData, 0, sizeof(UnityAudioObjectData));
		state->effectdata = unityObjData;

		InitParametersFromDefinitions(InternalRegisterEffectDefinition, unityObjData->p);

		if (IsHostCompatible(state))
			state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;

		if (g_FirstCreateCallback)	// First object being created; init ISAC
		{
			InitializeSpatialAudioClient(state->samplerate);

			g_ObjListsMutex = CreateMutex(NULL, FALSE, NULL);

			if (g_ObjListsMutex == NULL)
			{
				// Error: Couldn't create a mutex; throw a fit
			}
		}

		DWORD dwWaitResult = WaitForSingleObject(g_ObjListsMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
		
		if (dwWaitResult == WAIT_OBJECT_0)
		{
			// Is there an ISAC resource for this object?
			if (g_FreeISACAudioObjectIndices.size() > 0)
			{
				// Yes, there is - assign that resource to this object
				UINT32 objInx = g_FreeISACAudioObjectIndices.back();
				g_FreeISACAudioObjectIndices.pop_back();
				g_UsedISACAudioObjectIndices.push_back(objInx);

				unityObjData->ObjectIndex = objInx;

				HRESULT hr = g_SpatialAudioStream->ActivateSpatialAudioObject(
					AudioObjectType_Dynamic,
					&g_AudioObjectArray[objInx].object);

				// TODO: error handling
			}
			else
			{
				// No ISAC resources available right now. Put this object in the Wait list
				unityObjData->ObjectIndex = -1;
				g_ISACObjectWaitList.push_back(unityObjData);
			}
		}
		else if (dwWaitResult == WAIT_ABANDONED)
		{
			// TODO: NOT SURE WHAT TO DO
		}

		ReleaseMutex(g_ObjListsMutex);

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
	{
		UnityAudioObjectData* unityObjData = state->GetEffectData<UnityAudioObjectData>();

		DWORD dwWaitResult = WaitForSingleObject(g_ObjListsMutex, INFINITE); // TODO: CHOOSE A TIMEOUT

		if (dwWaitResult == WAIT_OBJECT_0)
		{
			// Find if this unity object is using an ISAC object
			BOOL objInUsedArray = FALSE;
			std::list<UINT32>::iterator objIterInUsedArray;
			for (std::list<UINT32>::iterator iter = g_UsedISACAudioObjectIndices.begin(); iter != g_UsedISACAudioObjectIndices.end(); iter++)
			{
				if (unityObjData->ObjectIndex == *iter)
				{
					objInUsedArray = TRUE;
					objIterInUsedArray = iter;
					break;
				}
			}

			if (objInUsedArray)
			{
				if (g_ISACObjectWaitList.size() > 0)
				{
					// There's at least one object waiting for ISAC resources
					// assign the freed resource to the object in the front of the Wait List
					UnityAudioObjectData* waitingObj = g_ISACObjectWaitList.front();
					g_ISACObjectWaitList.pop_front();

					waitingObj->ObjectIndex = unityObjData->ObjectIndex;
				}
				else
				{
					// No objects are waiting for ISAC resources, free the ISAC resource
					ISACAudioObjectData* objISAC = g_AudioObjectArray + *objIterInUsedArray;
					objISAC->object = nullptr;

					// Move from used resources to free
					g_FreeISACAudioObjectIndices.push_back(*objIterInUsedArray);
					g_UsedISACAudioObjectIndices.erase(objIterInUsedArray);
				}
			}
			else
			{
				// This unity object was not using an ISAC object; it should be in the wait list
				BOOL objInWaitList = FALSE;
				std::list<UnityAudioObjectData*>::iterator objIterInWaitList;

				for (std::list<UnityAudioObjectData*>::iterator iter = g_ISACObjectWaitList.begin(); iter != g_ISACObjectWaitList.end(); iter++)
				{
					if (unityObjData == *iter)
					{
						objInWaitList = TRUE;
						objIterInWaitList = iter;
						break;
					}
				}

				if (objInWaitList)
				{
					g_ISACObjectWaitList.erase(objIterInWaitList);
				}
				else
				{
					// This shouldn't happen
				}
			}
		}
		else if (dwWaitResult == WAIT_ABANDONED)
		{
			// TODO: NOT SURE WHAT TO DO
		}

		ReleaseMutex(g_ObjListsMutex);

		delete unityObjData;

		// TODO: Find a point when to stop the ISAC stream and clean up

		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
	{
		UnityAudioObjectData* unityObjData = state->GetEffectData<UnityAudioObjectData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		unityObjData->p[index] = value;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
	{
		UnityAudioObjectData* unityObjData = state->GetEffectData<UnityAudioObjectData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		if (value != NULL)
			*value = unityObjData->p[index];
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
		if (inchannels != 2 || outchannels != 2 || g_SystemSampleRate != REQUIREDSAMPLERATE)	// as of 2016, these are requirements for MS HRTF
		{
			memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}

		UnityAudioObjectData* unityObjdata = state->GetEffectData<UnityAudioObjectData>();

		DWORD dwWaitResult = WaitForSingleObject(g_ObjListsMutex, INFINITE); // TODO: CHOOSE A TIMEOUT
		if (dwWaitResult == WAIT_OBJECT_0)
		{

			if (unityObjdata->ObjectIndex == -1)
			{
				// This Unity Object does not have an ISAC Object assigned to it
				// pass the audio back to Unity so that it could be rendered in 2D
				memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
				ReleaseMutex(g_ObjListsMutex);
				return UNITY_AUDIODSP_OK;
			}
			else
			{
				// This Unity Object has an ISAC Object assigned to it
				// Copy over the audio and position data so that ISAC can render it
				ISACAudioObjectData* isacObjData = g_AudioObjectArray + unityObjdata->ObjectIndex;

				/* CONVERT POSITION DATA FROM UNITY'S SYSTEM TO ISAC'S SYSTEM */
				float* m = state->spatializerdata->listenermatrix;
				float* s = state->spatializerdata->sourcematrix;

				// Currently we ignore source orientation and only use the position
				float px = s[12];
				float py = s[13];
				float pz = s[14];

				float dir_x = m[0] * px + m[4] * py + m[8] * pz + m[12];
				float dir_y = m[1] * px + m[5] * py + m[9] * pz + m[13];
				float dir_z = m[2] * px + m[6] * py + m[10] * pz + m[14];

				isacObjData->m_writeIndexOverflowed = FALSE;

				for (UINT32 inx = 0; inx < length; inx++)
				{
					isacObjData->m_writeIndex++;

					if (isacObjData->m_writeIndex >= ISACCALLBACKBUFSIZE)
					{
						isacObjData->m_writeIndex -= ISACCALLBACKBUFSIZE;
						isacObjData->m_writeIndexOverflowed = TRUE;
					}

					isacObjData->m_dataPosX[isacObjData->m_writeIndex] = dir_x;
					isacObjData->m_dataPosY[isacObjData->m_writeIndex] = dir_y;
					isacObjData->m_dataPosZ[isacObjData->m_writeIndex] = -dir_z;

					isacObjData->m_dataBuf[isacObjData->m_writeIndex] = inbuffer[inx * 2];
				}

				memset(outbuffer, 0, length);	// dont return audio to unity
			}
		}
		else if (dwWaitResult == WAIT_ABANDONED)
		{
			// TODO: NOT SURE WHAT TO DO
		}

		ReleaseMutex(g_ObjListsMutex);

		return UNITY_AUDIODSP_OK;
	}
}
#pragma optimize("", on)

