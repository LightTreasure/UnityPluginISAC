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

using namespace Microsoft::WRL;

namespace MSHRTFSpatializer
{
	// because of the way HRTF is initialized, and because the unity plugin is initialized before we can touch it via a script ...
	//  if we want to expose all parameters we have to re-initialize the HRTF object inside ProcessCallback on first run if a param changes
	float m_currentEnvironment = 1.f;	// these are the windows default values for these params
	float m_currentMingain = -96.f;
	float m_currentMaxgain = 12.f;
	float m_currentUnitygain = 1.f;
	float m_bypass_attenuation = 1.f;
	
	int systemSampleRate = 0;	// as of 2016, HRTF only supports 48k input/output
	const int REQUIREDSAMPLERATE = 48000;
	UINT32 frameCount=NULL;

#define ISACCALLBACKBUFSIZE 48000

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

	struct EffectData
	{
		static const int						m_HrtfFrameSize = 1024;	    // ms hrtf requires 1024 framesize
		float p[P_NUM];

		XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS   m_InBufferParams;
		XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS   m_OutBufferParams;
		WAVEFORMATEX                            m_InFormat;
		WAVEFORMATEX                            m_OutFormat;
		int                                     m_BufferPosition;
		float                                   m_InBuffer[m_HrtfFrameSize];
		float                                   m_OutBuffer[m_HrtfFrameSize * 2];
		MSG msg;

		/*ISAC VARIABLES*/
		IMMDevice * pDevice = NULL;
		IMMDeviceEnumerator * pEnumerator = NULL;
		ComPtr<ISpatialAudioObject> object;
		ComPtr<ISpatialAudioObjectRenderStream> spatialAudioStream;
		ComPtr<ISpatialAudioClient> spatialAudioClient;
		HANDLE bufferCompletionEvent;

		/*ISAC BUFFER*/
		float m_dataBuf[ISACCALLBACKBUFSIZE];
		float m_dataPosX[ISACCALLBACKBUFSIZE];
		float m_dataPosY[ISACCALLBACKBUFSIZE];
		float m_dataPosZ[ISACCALLBACKBUFSIZE];

		UINT32 m_readIndex;
		UINT32 m_writeIndex;
		BOOL m_writeIndexOverflowed;

		HANDLE m_bufMutex;

		BOOL m_workThreadActive;
	};

	// Worker thread for ISpatialAudioClient work
	PTP_WORK m_workThread;
	BOOL m_workThreadActive = false;

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

		EffectData* data = (EffectData *)Context;

		while (data->m_workThreadActive)
		{
			// WAIT FOR ISAC EVENT
			if (WaitForSingleObject(data->bufferCompletionEvent, INFINITE) != WAIT_OBJECT_0)
			{
				continue;
			}

			// NOW GRAB THE DATA MUTEX
			DWORD dwWaitResult = WaitForSingleObject(data->m_bufMutex, INFINITE); //TODO: Maybe a different timeout interval?

			if (dwWaitResult == WAIT_OBJECT_0)	
			{
				// We now have the mutex to read the data
				UINT32 actualWriteIndex = data->m_writeIndexOverflowed ? data->m_writeIndex + ISACCALLBACKBUFSIZE : data->m_writeIndex;

				BOOL enoughData = (actualWriteIndex - data->m_readIndex) >= 480;

				UINT32 frameCount;
				UINT32 availableObjectCount;

				hr = data->spatialAudioStream->BeginUpdatingAudioObjects(
					&availableObjectCount,
					&frameCount);

				//Get the object buffer
				BYTE* buffer = nullptr;
				UINT32 bytecount;
				hr = data->object->GetBuffer(&buffer, &bytecount);
				if (FAILED(hr))
				{
					ReleaseMutex(data->m_bufMutex);
					continue;
				}

				data->object->SetPosition(data->m_dataPosX[data->m_readIndex],
					data->m_dataPosY[data->m_readIndex],
					data->m_dataPosZ[data->m_readIndex]);

				data->object->SetVolume(1.f);
					
				if (enoughData)
				{
					for (UINT32 inx = 0; inx < 480; inx++)
					{
						*((float*)buffer) = data->m_dataBuf[data->m_readIndex];
						buffer += 4;

						data->m_readIndex++;
						if (data->m_readIndex >= ISACCALLBACKBUFSIZE)
						{
							data->m_readIndex -= ISACCALLBACKBUFSIZE;
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


				// Let the audio-engine know that the object data are available for processing now 
				hr = data->spatialAudioStream->EndUpdatingAudioObjects();
				if (FAILED(hr))
				{
					// TODO: WHAT TO DO HERE
					ReleaseMutex(data->m_bufMutex);
					continue;
				}
			}
			else 
			{
				// TODO: what do we do here?
				continue;
			}
			ReleaseMutex(data->m_bufMutex);
		}
	}
	
	/*VOID CALLBACK SpatialWorkCallback(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_WORK Work)
	{
		HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		Work;
		Instance;

		EffectData* data = (EffectData *)Context;

		// Get the Mutex protecting the buffers
		DWORD dwWaitResult;

		dwWaitResult = WaitForSingleObject(data->m_bufMutex, INFINITE);	//TODO: Maybe a different timeout interval?

		switch (dwWaitResult)
		{
		case WAIT_OBJECT_0:

			UINT32 actualWriteIndex = data->m_writeIndexOverflowed ? data->m_writeIndex + ISACCALLBACKBUFSIZE : data->m_writeIndex;

			BOOL enoughData = (actualWriteIndex - data->m_readIndex) >= 480;

			if (m_workThreadActive && enoughData)
			{
				// Wait for a signal from the audio-engine to start the next processing pass 
				if (data->bufferCompletionEvent)
				{
					if (WaitForSingleObject(data->bufferCompletionEvent, INFINITE) != WAIT_OBJECT_0)
					{
						//TODO: WHAT TO DO HERE
					}
				}

				UINT32 frameCount;
				UINT32 availableObjectCount;

				// Begin the process of sending object data and metadata 
				// Get the number of active object that can be used to send object-data 
				// Get the number of frame count that each buffer be filled with  
				hr = data->spatialAudioStream->BeginUpdatingAudioObjects(
					&availableObjectCount,
					&frameCount);

				//TODO: IS THIS NEEDED?
				
				//if (data->object == nullptr)
				//{
				// If this method called more than activeObjectCount times
				// It will fail with this error HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)
				//hr = data->spatialAudioStream->ActivateSpatialAudioObject(
				//AudioObjectType_Dynamic,
				//&data->object);
				//if (FAILED(hr))
				//{
				//continue;
				//}
				//
				//}

				//Get the object buffer
				BYTE* buffer = nullptr;
				UINT32 bytecount;
				hr = data->object->GetBuffer(&buffer, &bytecount);
				if (FAILED(hr))
				{
					//TODO: WHAT TO DO HERE
				}

				data->object->SetPosition(data->m_dataPosX[data->m_readIndex],
					data->m_dataPosY[data->m_readIndex],
					data->m_dataPosZ[data->m_readIndex]);

				data->object->SetVolume(1.f);

				for (UINT32 inx = 0; inx < 480; inx++)
				{
					*((float*)buffer) = data->m_dataBuf[data->m_readIndex];
					buffer += 4;

					data->m_readIndex++;
					if (data->m_readIndex >= ISACCALLBACKBUFSIZE)
					{
						data->m_readIndex -= ISACCALLBACKBUFSIZE;
					}
				}
				buffer = buffer - (1920);

				// Let the audio-engine know that the object data are available for processing now 
				hr = data->spatialAudioStream->EndUpdatingAudioObjects();
				if (FAILED(hr))
				{
					// TODO: WHAT TO DO HERE
				}
			}

			ReleaseMutex(data->m_bufMutex);
			break;
		}

	}*/

	void MakeSpatialClient(EffectData* data, int sampleRate) {
		systemSampleRate = sampleRate;
		if (systemSampleRate != REQUIREDSAMPLERATE)	// as of 2016, if not 48k samplerate, MS HRTF will die
			return;

		CoCreateInstance(
			__uuidof(MMDeviceEnumerator), NULL,
			CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			(void**)&data->pEnumerator);

		data->pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &data->pDevice);

		if (!&data->pDevice)
			return;

		auto hr = data->pDevice->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)&data->spatialAudioClient);

		// Check the available rendering formats 
		ComPtr<IAudioFormatEnumerator> audioObjectFormatEnumerator;
		hr = data->spatialAudioClient->GetSupportedAudioObjectFormatEnumerator(&audioObjectFormatEnumerator);

		WAVEFORMATEX* objectFormat = nullptr;

		UINT32 audioObjectFormatCount;
		hr = audioObjectFormatEnumerator->GetCount(&audioObjectFormatCount); // There is at least one format that the API accept
		if (audioObjectFormatCount == 0)
		{
			return;
		}

		// Select the most favorable format, first one
		hr = audioObjectFormatEnumerator->GetFormat(0, &objectFormat);

		// Create the event that will be used to signal the client for more data
		data->bufferCompletionEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		//Exception thrown at 0x00007FF8C05770DD (AudioSes.dll) in a.exe: 0xC0000005: Access violation reading location 0xFFFFFFFFFFFFFFFF.
		hr = data->spatialAudioClient->ActivateSpatialAudioObjectRenderStream(
			objectFormat,
			0,
			1, // allocate no more than the current max available object for processing
			AudioCategory_GameEffects,
			data->bufferCompletionEvent,
			nullptr,
			&data->spatialAudioStream);

		if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
		{
			return;
		}

		data->spatialAudioStream->GetFrameCount(&frameCount);

		hr = data->spatialAudioStream->Start();

		//UINT32 availableObjectCount;

		//hr = data->spatialAudioStream->BeginUpdatingAudioObjects(
		//	&availableObjectCount,
		//	&frameCount);

		// If this method called more than activeObjectCount times
		// It will fail with this error HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS) 
		hr = data->spatialAudioStream->ActivateSpatialAudioObject(
				AudioObjectType_Dynamic,//objChan,
				&data->object);
			//data->object->SetPosition(1, 2, 3);
			//data->object->SetVolume(.9f);
		
			//hr = data->spatialAudioStream->EndUpdatingAudioObjects();

		/*INIT DATA MEMBERS*/
		data->m_readIndex = data->m_writeIndex = 0;
		data->m_writeIndexOverflowed = FALSE;

		data->m_bufMutex = CreateMutex(NULL, FALSE, NULL);

		if (data->m_bufMutex == NULL)
		{
			//TODO: throw error
		}

		/* CREATE A WORKER THREAD TO LISTEN FOR ISAC EVENTS */
		data->m_workThreadActive = TRUE;
		m_workThread = CreateThreadpoolWork(SpatialWorkCallbackNew, data, nullptr);
		SubmitThreadpoolWork(m_workThread); 
	}

	static UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK DistanceAttenuationCallback(UnityAudioEffectState* state, float distanceIn, float attenuationIn, float* attenuationOut)
	{
		EffectData* data = state->GetEffectData<EffectData>();
		*attenuationOut = 1.f;
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
		EffectData* effectdata = new EffectData;
		memset(effectdata, 0, sizeof(EffectData));
		state->effectdata = effectdata;
		InitParametersFromDefinitions(InternalRegisterEffectDefinition, effectdata->p);
		if (IsHostCompatible(state))
			state->spatializerdata->distanceattenuationcallback = DistanceAttenuationCallback;

		MakeSpatialClient(state->GetEffectData<EffectData>(), state->samplerate);

			return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ReleaseCallback(UnityAudioEffectState* state)
	{
		EffectData* data = state->GetEffectData<EffectData>();
		data->spatialAudioStream->Stop();
		data->pDevice->Release();
		CloseHandle(data->bufferCompletionEvent);
		delete data;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK SetFloatParameterCallback(UnityAudioEffectState* state, int index, float value)
	{
		EffectData* data = state->GetEffectData<EffectData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		data->p[index] = value;
		return UNITY_AUDIODSP_OK;
	}

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK GetFloatParameterCallback(UnityAudioEffectState* state, int index, float* value, char *valuestr)
	{
		EffectData* data = state->GetEffectData<EffectData>();
		if (index >= P_NUM)
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		if (value != NULL)
			*value = data->p[index];
		if (valuestr != NULL)
			valuestr[0] = 0;
		return UNITY_AUDIODSP_OK;
	}

	int UNITY_AUDIODSP_CALLBACK GetFloatBufferCallback(UnityAudioEffectState* state, const char* name, float* buffer, int numsamples)
	{
		return UNITY_AUDIODSP_OK;
	}

#pragma optimize("", off)

	//float scale = sqrtf(1.0f / (float)inchannels);
	//float monomix = 0.0f;
	//for (int i = 0; i < inchannels; i++)
	//				monomix += inbuffer[n + i];

	UNITY_AUDIODSP_RESULT UNITY_AUDIODSP_CALLBACK ProcessCallback(UnityAudioEffectState* state, float* inbuffer, float* outbuffer, unsigned int length, int inchannels, int outchannels)
	{
		if (inchannels != 2 || outchannels != 2 || systemSampleRate != REQUIREDSAMPLERATE)	// as of 2016, these are requirements for MS HRTF
		{
			memcpy(outbuffer, inbuffer, length * outchannels * sizeof(float));
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;
		}


		EffectData* data = state->GetEffectData<EffectData>();		

				///memset(outbuffer, 0, length*inchannels);	// dont return audio to unity
		
		float* m = state->spatializerdata->listenermatrix;
		float* s = state->spatializerdata->sourcematrix;

		// Currently we ignore source orientation and only use the position
		float px = s[12];
		float py = s[13];
		float pz = s[14];

		float dir_x = m[0] * px + m[4] * py + m[8] * pz + m[12];
		float dir_y = m[1] * px + m[5] * py + m[9] * pz + m[13];
		float dir_z = m[2] * px + m[6] * py + m[10] * pz + m[14];

		//float dist = sqrtf(dir_x * dir_x + dir_y * dir_y + dir_z * dir_z);

		// Get the Mutex protecting the buffers
		DWORD dwWaitResult;

		dwWaitResult = WaitForSingleObject(data->m_bufMutex, INFINITE);	//TODO: Maybe a different timeout interval?

		switch (dwWaitResult)
		{
		case WAIT_OBJECT_0:
			// TODO: check if we're writing into a location that hasn't been read yet
			data->m_writeIndexOverflowed = false;

			for (UINT32 inx = 0; inx < length; inx++)
			{
				data->m_writeIndex++;

				if (data->m_writeIndex >= ISACCALLBACKBUFSIZE)
				{
					data->m_writeIndex -= ISACCALLBACKBUFSIZE;
					data->m_writeIndexOverflowed = TRUE;
				}

				data->m_dataPosX[data->m_writeIndex] = dir_x;
				data->m_dataPosY[data->m_writeIndex] = dir_y;
				data->m_dataPosZ[data->m_writeIndex] = -dir_z;
				
				data->m_dataBuf[data->m_writeIndex] = inbuffer[inx * 2];
			}

			ReleaseMutex(data->m_bufMutex);
			break;
		case WAIT_ABANDONED:
			//TODO: PASS AUDIO BACK TO UNITY?
			return UNITY_AUDIODSP_ERR_UNSUPPORTED;	//TODO: decide what exact error code to use
			break;
		}
		

		/*
		
		HrtfPosition pos{};
		pos.x = dir_x;
		pos.y = dir_y;
		pos.z = -dir_z; // Ms HRTF is in DirectX coordinates, which have an inverted Z relative to Unity

		//// How you could set emitter's source orientation
		//DirectX::XMMATRIX rm = DirectX::XMMatrixRotationRollPitchYaw(0, 0, 0);
		//DirectX::XMFLOAT3X3 rm33{};
		//DirectX::XMStoreFloat3x3(&rm33, rm);
		//data->m_spHrtfParams->SetSourceOrientation(&HrtfOrientation{ rm33._11, rm33._12, rm33._13, rm33._21, rm33._22, rm33._23, rm33._31, rm33._32, rm33._33 });

		UINT32 availableObjectCount;

		WaitForSingleObject(data->bufferCompletionEvent, INFINITE) != WAIT_OBJECT_0;

		///////// Pass input to output in Unity, dont activate ISAC even though we wait for it //////////////////////
		//for (UInt32 n = 0; n < length; n++)
		//{
		//	outbuffer[n * 2] = inbuffer[n * 2];
		//	outbuffer[n * 2 + 1] = inbuffer[n * 2 + 1];
		//}
		//return UNITY_AUDIODSP_OK;
		////////// END REGION //////////////////////

		UINT32 framelength = 0;
		HRESULT hr = data->spatialAudioStream->BeginUpdatingAudioObjects(
			&availableObjectCount,
			&framelength);

		////////  Pass input to output in Unity, dont use ISAC even though we activate it ///////////
		//hr = data->spatialAudioStream->EndUpdatingAudioObjects();
		//for (UInt32 n = 0; n < length; n++)
		//{
		//	outbuffer[n * 2] = inbuffer[n * 2];
		//	outbuffer[n * 2 + 1] = inbuffer[n * 2 + 1];
		//}
		//return UNITY_AUDIODSP_OK;
		///////// END REGION //////////


		//// Actual ISAC /////////
		BYTE* buffer;
		UINT32 bufLen;
		data->object->GetBuffer(&buffer, &bufLen);

		for (UInt32 n = 0; n < length; n++)
		{
			*((float*)buffer) = inbuffer[n * 2];
			buffer+= 4;
		}
		buffer = buffer - (length*4);
		///////  END REGION   //////////////
		
		data->object->SetPosition(pos.x, pos.y, pos.z);
		hr = data->spatialAudioStream->EndUpdatingAudioObjects();*/

		memset(outbuffer, 0, length);
		return UNITY_AUDIODSP_OK;
	}
}
#pragma optimize("", on)

