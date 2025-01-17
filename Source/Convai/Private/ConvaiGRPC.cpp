// Copyright 2022 Convai Inc. All Rights Reserved.


#include "ConvaiGRPC.h"
#include "../Convai.h"
#include "ConvaiDefinitions.h"
#include "ConvaiActionUtils.h"
#include "ConvaiUtils.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameInstance.h"
// #include <chrono>   
#include <string>
#include "Engine/EngineTypes.h"

THIRD_PARTY_INCLUDES_START
#include <grpc++/grpc++.h>
THIRD_PARTY_INCLUDES_END

// Needed for CURL to work
//#pragma comment (lib, "crypt32.lib")

DEFINE_LOG_CATEGORY(ConvaiGRPCLog);

using ::service::GetResponseRequest_GetResponseConfig;
using ::service::TriggerConfig;
using ::service::ActionConfig;
using ::service::AudioConfig;
using ::service::ActionConfig_Object;
using ::service::ActionConfig_Character;
using ::service::GetResponseRequest_GetResponseData;
using ::service::FaceModel;

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using service::ConvaiService;


namespace {
	const char* StatusCodeStr[] =
	{
	"OK",
	"CANCELLED",
	"UNKNOWN",
	"INVALID_ARGUMENT",
	"DEADLINE_EXCEEDED",
	"NOT_FOUND",
	"ALREADY_EXISTS",
	"PERMISSION_DENIED",
	"UNAUTHENTICATED",
	"RESOURCE_EXHAUSTED",
	"FAILED_PRECONDITION",
	"ABORTED",
	"OUT_OF_RANGE",
	"UNIMPLEMENTED",
	"INTERNAL",
	"UNAVAILABLE",
	"DATA_LOSS",
	"DO_NOT_USE" };
}

UConvaiGRPCGetResponseProxy* UConvaiGRPCGetResponseProxy::CreateConvaiGRPCGetResponseProxy(UObject* WorldContextObject, FString UserQuery, FString TriggerName, FString TriggerMessage, FString CharID, bool VoiceResponse, bool RequireFaceData, bool GeneratesVisemesAsBlendshapes, FString SessionID, UConvaiEnvironment* Environment, bool GenerateActions, FString API_Key)
{
	UConvaiGRPCGetResponseProxy* Proxy = NewObject<UConvaiGRPCGetResponseProxy>();
	Proxy->WorldPtr = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	Proxy->UserQuery = UserQuery;
	Proxy->TriggerName = TriggerName;
	Proxy->TriggerMessage = TriggerMessage;
	Proxy->CharID = CharID;
	Proxy->SessionID = SessionID;
	Proxy->VoiceResponse = VoiceResponse;
	Proxy->Environment = Environment;
	Proxy->GenerateActions = GenerateActions;
	Proxy->RequireFaceData = RequireFaceData;
	Proxy->GeneratesVisemesAsBlendshapes = GeneratesVisemesAsBlendshapes;
	Proxy->API_Key = API_Key;

	return Proxy;
}

UConvaiGRPCGetResponseProxy* UConvaiGRPCGetResponseProxy::CreateConvaiGRPCGetResponseProxy(UObject* WorldContextObject, FString UserQuery, FString CharID, bool VoiceResponse, bool RequireFaceData, bool GeneratesVisemesAsBlendshapes, FString SessionID, UConvaiEnvironment* Environment, bool GenerateActions, FString API_Key)
{
	return CreateConvaiGRPCGetResponseProxy(WorldContextObject, UserQuery, FString(""), FString(""), CharID, VoiceResponse, RequireFaceData, GeneratesVisemesAsBlendshapes, SessionID, Environment, GenerateActions, API_Key);
}

void UConvaiGRPCGetResponseProxy::Activate()
{
	OnInitStreamDelegate = FgRPC_Delegate::CreateUObject(this, &ThisClass::OnStreamInit);
	OnStreamReadDelegate = FgRPC_Delegate::CreateUObject(this, &ThisClass::OnStreamRead);
	OnStreamWriteDelegate = FgRPC_Delegate::CreateUObject(this, &ThisClass::OnStreamWrite);
	OnStreamWriteDoneDelegate = FgRPC_Delegate::CreateUObject(this, &ThisClass::OnStreamWriteDone);
	OnStreamFinishDelegate = FgRPC_Delegate::CreateUObject(this, &ThisClass::OnStreamFinish);
	
	reply = std::unique_ptr<service::GetResponseResponse>(new service::GetResponseResponse());

	// Form Validation
	if (!UConvaiFormValidation::ValidateAPIKey(API_Key) || !(UConvaiFormValidation::ValidateCharacterID(CharID)) || !(UConvaiFormValidation::ValidateSessionID(SessionID)))
	{
		OnFailure.ExecuteIfBound();
		return;
	}

	if (!WorldPtr.IsValid())
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("WorldPtr not valid"));
		OnFailure.ExecuteIfBound();
		return;
	}

	UConvaiSubsystem* ConvaiSubsystem = UConvaiUtils::GetConvaiSubsystem(WorldPtr.Get());
	if (!ConvaiSubsystem)
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("Convai Subsystem is not valid"));
		OnFailure.ExecuteIfBound();
		return;
	}

	// Create a new stub instance
	stub_ = ConvaiSubsystem->gRPC_Runnable->GetNewStub();
	if (!stub_)
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("Could not aquire a new stub instance"));
		OnFailure.ExecuteIfBound();
		return;
	}

	// Aquire the completion queue instance
	cq_ = ConvaiSubsystem->gRPC_Runnable->GetCompletionQueue();
	if (!cq_)
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("Got an invalid completion queue instance"));
		OnFailure.ExecuteIfBound();
		return;
	}

	bool Found;
	FString VersionName;
	FString EngineVersion;
	FString PlatformName;
	FString PluginEngineVersion;
	FString FriendlyName;

	UConvaiUtils::GetPluginInfo(FString("Convai"), Found, VersionName, FriendlyName, PluginEngineVersion);
	UConvaiUtils::GetPlatformInfo(EngineVersion, PlatformName);

	// Add metadata
	client_context.AddMetadata("engine", "Unreal Engine");
	client_context.AddMetadata("engine_version", TCHAR_TO_UTF8(*EngineVersion));
	client_context.AddMetadata("platform_name", TCHAR_TO_UTF8(*PlatformName));

	if (Found)
	{
		client_context.AddMetadata("plugin_engine_version", TCHAR_TO_UTF8(*PluginEngineVersion));
		client_context.AddMetadata("plugin_version", TCHAR_TO_UTF8(*VersionName));
		client_context.AddMetadata("plugin_base_name", TCHAR_TO_UTF8(*FriendlyName));
	}
	else
	{
		client_context.AddMetadata("plugin_engine_version", "Unknown");
		client_context.AddMetadata("plugin_version", "Unknown");
		client_context.AddMetadata("plugin_base_name", "Unknown");
	}

	ReceivedFinish = false;

	// Initialize the stream
	stream_handler = stub_->AsyncGetResponse(&client_context, cq_, (void*)&OnInitStreamDelegate);
}

void UConvaiGRPCGetResponseProxy::WriteAudioDataToSend(uint8* Buffer, uint32 Length, bool LastWrite)
{
	LastWriteReceived = LastWrite;

	m_mutex.Lock();

	AudioBuffer.Append(Buffer, Length);

	m_mutex.Unlock();

	// UE_LOG(ConvaiGRPCLog, Log, TEXT("WriteAudioDataToSend:: InformOnDataReceived = %s"), InformOnDataReceived ? *FString("True") : *FString("False"));
	if (InformOnDataReceived)
	{
		// Reset
		InformOnDataReceived = false;

		// Inform of new data to send
		OnStreamWrite(true);
	}
}

void UConvaiGRPCGetResponseProxy::FinishWriting()
{
	LastWriteReceived = true;

	if (InformOnDataReceived)
	{
		// Inform of new data to send
		OnStreamWrite(true);

		// Reset
		InformOnDataReceived = false;
	}
}

void UConvaiGRPCGetResponseProxy::BeginDestroy()
{
	client_context.TryCancel();
	stub_.reset();
	UE_LOG(ConvaiGRPCLog, Log, TEXT("Destroying UConvaiGRPCGetResponseProxy..."));
	Super::BeginDestroy();
}

void UConvaiGRPCGetResponseProxy::CallFinish()
{
	if (CalledFinish || !stream_handler)
		return;

	CalledFinish = true;
	stream_handler->Finish(&status, (void*)&OnStreamFinishDelegate);
}

TArray<uint8> UConvaiGRPCGetResponseProxy::ConsumeFromAudioBuffer(bool& IsThisTheFinalWrite)
{
	// TODO: (Mohamed) optimize and clean this to reduce number of copying operations to not spend much time in CS, also reserve memory for "output" array before entering

	TArray<uint8> output;

	m_mutex.Lock();
	//uint32 Length = FMath::Min(AudioBuffer.Num(), (int32)ConvaiConstants::VoiceStreamMaxChunk);
	uint32 Length = AudioBuffer.Num();
	if (Length)
	{
		output.Append(AudioBuffer.GetData(), Length);

		if (Length == AudioBuffer.Num())
			AudioBuffer.Empty(AudioBuffer.Max());
		else
			AudioBuffer.RemoveAt(0, Length);
	}
	m_mutex.Unlock();


	if (LastWriteReceived && AudioBuffer.Num() == 0)
		IsThisTheFinalWrite = true;
	else
		IsThisTheFinalWrite = false;

	return output;
}

void UConvaiGRPCGetResponseProxy::LogAndEcecuteFailure(FString FuncName)
{
	UE_LOG(ConvaiGRPCLog, Warning,
	TEXT("%s: Status:%s | Debug Log:%s | Error message:%s | Error Details:%s | Error Code:%i | Character ID:%s | Session ID:%s"),
	*FString(FuncName), 
	*FString(status.ok()? "Ok" : "Not Ok"),
	*FString(reply->DebugString().c_str()), 
	*FString(status.error_message().c_str()), 
	*FString(status.error_details().c_str()), 
	status.error_code(),
	*CharID,
	*SessionID);

	if (!FailAlreadyExecuted)
	{
		OnFailure.ExecuteIfBound();
		FailAlreadyExecuted = true;
	}
}

void UConvaiGRPCGetResponseProxy::ExtendDeadline()
{
	// Extend the deadline
	std::chrono::system_clock::time_point deadline =
		std::chrono::system_clock::now() + std::chrono::milliseconds(3000);
	//client_context.set_deadline(deadline);
}

void UConvaiGRPCGetResponseProxy::OnStreamInit(bool ok)
{
	//TODO (Mohamed) handle status variable

	if (!IsValid(this))
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("OnStreamInit Could not initialize due to pending kill!"));
		LogAndEcecuteFailure("OnStreamInit");
		return;
	}

	if (!ok)
	{
		LogAndEcecuteFailure("OnStreamInit");
		return;
	}

	UE_LOG(ConvaiGRPCLog, Log, TEXT("GRPC GetResponse stream initialized"));

	// Create Action Configuration
	ActionConfig* action_config = new ActionConfig();
	FString MainCharacter;
	if (GenerateActions && IsValid(Environment))
	{
		action_config->set_classification("multistep");
		for (FString action : Environment->Actions) // Add Actions
		{
			action_config->add_actions(TCHAR_TO_UTF8(*action));
		}

		for (FConvaiObjectEntry object : Environment->Objects) // Add Objects
		{
			ActionConfig_Object* action_config_object = action_config->add_objects();
			FString FinalName = object.Name;
			if (object.Description.Len())
			{
				FinalName = FinalName.Append(*FString(" <"));
				FinalName = FinalName.Append(*object.Description);
				FinalName = FinalName.Append(">");
			}
			action_config_object->set_name(TCHAR_TO_UTF8(*FinalName));
			action_config_object->set_description(TCHAR_TO_UTF8(*object.Description));
		}

		for (FConvaiObjectEntry character : Environment->Characters) // Add Characters
		{
			ActionConfig_Character* action_config_character = action_config->add_characters();
			FString FinalName = character.Name;
			if (character.Description.Len())
			{
				FinalName = FinalName.Append(*FString(" <"));
				FinalName = FinalName.Append(*character.Description);
				FinalName = FinalName.Append(">");
			}

			action_config_character->set_name(TCHAR_TO_UTF8(*FinalName));
			action_config_character->set_bio(TCHAR_TO_UTF8(*character.Description));
		}

		// Check if we have an attention object set
		FConvaiObjectEntry AttentionObject = Environment->AttentionObject;
		if (AttentionObject.Name.Len() != 0)
		{
			FString FinalName = AttentionObject.Name;
			if (AttentionObject.Description.Len())
			{
				FinalName = FinalName.Append(*FString(" <"));
				FinalName = FinalName.Append(*AttentionObject.Description);
				FinalName = FinalName.Append(">");
			}
			action_config->set_current_attention_object(TCHAR_TO_UTF8(*FinalName));
		}
	}

	if (IsValid(Environment))
	{
		// Get the speaker/main character name
		MainCharacter = Environment->MainCharacter.Name;
	}

	// Create Audio Configuration
	AudioConfig* audio_config = new AudioConfig();
	audio_config->set_sample_rate_hertz((int32)ConvaiConstants::VoiceCaptureSampleRate);
	audio_config->set_enable_facial_data(RequireFaceData);
	if (RequireFaceData)
	{
		FaceModel faceModel = GeneratesVisemesAsBlendshapes ? FaceModel::FACE_MODEL_A_2F_MODEL_NAME : FaceModel::FACE_MODEL_OVR_MODEL_NAME;
		audio_config->set_face_model(faceModel);
	}

	// Create the config object that holds Audio and Action configs
	GetResponseRequest_GetResponseConfig* getResponseConfig = new GetResponseRequest_GetResponseConfig();
	getResponseConfig->set_api_key(TCHAR_TO_UTF8(*API_Key));
	getResponseConfig->set_session_id(TCHAR_TO_UTF8(*SessionID));
	getResponseConfig->set_character_id(TCHAR_TO_UTF8(*CharID));

	if (GenerateActions)
	{
		getResponseConfig->set_allocated_action_config(action_config);
		getResponseConfig->set_speaker(TCHAR_TO_UTF8(*MainCharacter));
	}
	getResponseConfig->set_allocated_audio_config(audio_config);

	// Set the config object in the request object to be passed to the API
	request.Clear();
	request.set_allocated_get_response_config(getResponseConfig);

#if ConvaiDebugMode
	FString DebugString(request.DebugString().c_str());
	UE_LOG(ConvaiGRPCLog, Log, TEXT("request: %s"), *DebugString);
#endif 

	// Do a write task
	stream_handler->Write(request, (void*)&OnStreamWriteDelegate);
	//UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->Write"));

	// Do a read task
	stream_handler->Read(reply.get(), (void*)&OnStreamReadDelegate);
	//UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->Read"));
}

void UConvaiGRPCGetResponseProxy::OnStreamWrite(bool ok)
{
	if (!IsValid(this))
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("OnStreamWrite failed due to pending kill!"));
		LogAndEcecuteFailure("OnStreamWrite");
		return;
	}

	if (!ok)
	{
		LogAndEcecuteFailure("OnStreamWrite");
		CallFinish();
		return;
	}

	if (CalledFinish)
		return;

	// UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamWriteBegin"));

	// Clear the request data to make it ready to hold the new data we are going to send
	request.Clear();
	GetResponseRequest_GetResponseData* get_response_data = new GetResponseRequest_GetResponseData();

	bool IsThisTheFinalWrite;

	// If there is text
	if (UserQuery.Len())
	{
		// Add in the text data
		get_response_data->set_text_data(TCHAR_TO_UTF8(*UserQuery));
		IsThisTheFinalWrite = true;
	}
	else if (TriggerName.Len() || TriggerMessage.Len()) // If there is a trigger message
	{
		// Add in the trigger data
		TriggerConfig* triggerConfig = new TriggerConfig();
		triggerConfig->set_trigger_name(TCHAR_TO_UTF8(*TriggerName));
		triggerConfig->set_trigger_message(TCHAR_TO_UTF8(*TriggerMessage));
		get_response_data->set_allocated_trigger_data(triggerConfig);
		IsThisTheFinalWrite = true;
	}
	else // Normal voice data
	{
		// Try to consume the next chunk of mic data
		uint8* Buffer = nullptr;
		TArray<uint8> Data = ConsumeFromAudioBuffer(IsThisTheFinalWrite);

		if (!Data.Num())
		{
			if (IsThisTheFinalWrite)
			{
				// Tell the server that we have finished writing
				UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->WritesDone"));
				stream_handler->WritesDone((void*)&OnStreamWriteDoneDelegate); UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamWrite Done Writing"));
			}
			else
			{
				// Let us know when new data is available
				InformOnDataReceived = true; // UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamWrite: Awaiting audio data"));
			}

			// Do not proceed
			return;
		}
		else
		{
			// Load the audio data to the request
			get_response_data->set_audio_data(Data.GetData(), Data.Num()); // UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamWrite: Sending %d bytes"), DataLen);
		}

		NumberOfAudioBytesSent += Data.Num();
	}
	// Prepare the request
	request.set_allocated_get_response_data(get_response_data);



	 //#if ConvaiDebugMode
	 //    FString DebugString(request.DebugString().c_str());
	 //    UE_LOG(ConvaiGRPCLog, Warning, TEXT("request: %s"), *DebugString);
	 //#endif 

	if (IsThisTheFinalWrite)
	{
		// Send the data and tell the server that this is the last piece of data
		UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->WriteLast"));
		stream_handler->WriteLast(request, grpc::WriteOptions(), (void*)&OnStreamWriteDoneDelegate);
	}
	else
	{
		// Do a normal send of the data
		//UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->Write"));
		stream_handler->Write(request, (void*)&OnStreamWriteDelegate);
	}

}

void UConvaiGRPCGetResponseProxy::OnStreamWriteDone(bool ok)
{
	if (!IsValid(this))
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("OnStreamWriteDone failed due to pending kill!"));
		LogAndEcecuteFailure("OnStreamWriteDone");
		return;
	}

	if (!ok)
	{
		LogAndEcecuteFailure("OnStreamWriteDone");
		CallFinish();
		return;
	}

	UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamWriteDone"));
	UE_LOG(ConvaiGRPCLog, Log, TEXT("NumberOfAudioBytesSent %i"), NumberOfAudioBytesSent);
}

void UConvaiGRPCGetResponseProxy::OnStreamRead(bool ok)
{
	if (!IsValid(this))
	{
		UE_LOG(ConvaiGRPCLog, Warning, TEXT("OnStreamRead failed due to pending kill!"));
		LogAndEcecuteFailure("OnStreamRead");
		return;
	}

	if (!ok)
	{
		// Tell the server that we are ready to finish the stream any time it wishes
		UE_LOG(ConvaiGRPCLog, Log, TEXT("stream_handler->Finish"));
		if (stream_handler)
			CallFinish();
		else
			OnFinish.ExecuteIfBound();
		if (!status.ok())
			LogAndEcecuteFailure("OnStreamRead");
		return;
	}

	// Grab the session ID
	std::string SessionID_std = reply->session_id();
	if (SessionID_std.size())
	{
		SessionID = FString(SessionID_std.c_str());
		// Broadcast the Session ID
		OnSessionIDReceived.ExecuteIfBound(SessionID);

	}

	if (reply->has_user_query()) // Is there transcription ready
	{
		std::string UserQuery_std = reply->user_query().text_data();
		bool IsTranscriptionReady = reply->user_query().is_final();
		bool IsFinalTranscription = reply->user_query().end_of_response();

		// Convert UTF8 to UTF16 FString
		FString text_string = UConvaiUtils::FUTF8ToFString(UserQuery_std.c_str());

		OnTranscriptionReceived.ExecuteIfBound(text_string, IsTranscriptionReady, IsFinalTranscription);
		//UE_LOG(ConvaiGRPCLog, Log, TEXT("UserQuery: %s, Final: %d"), *FString(UserQuery_std.c_str()), IsFinalUserQuery);
	}
	else if (reply->has_audio_response()) // Is there an audio response
	{
		// Grab bot text
		std::string text_string_std = reply->audio_response().text_data();

		// Convert UTF8 to UTF16 FString
		FString text_string = UConvaiUtils::FUTF8ToFString(text_string_std.c_str());

		// Grab bot audio
		::std::string audio_data = reply->audio_response().audio_data();
		TArray<uint8> VoiceData;
		float SampleRate = 0;
		if (reply->audio_response().audio_data().length() > 46)
		{
			VoiceData = TArray<uint8>(reinterpret_cast<const uint8*>(audio_data.data() + 46), audio_data.length() - 46);
			SampleRate = reply->audio_response().audio_config().sample_rate_hertz();
		}
		FAnimationSequence FaceDataAnimation;

		if (RequireFaceData)
		{
			TSharedPtr<FJsonObject> FaceData_json;

			bool HasVisemes = reply->audio_response().has_visemes_data();
			bool HasBlendshapes = reply->audio_response().has_blendshapes_data();

			if (HasBlendshapes && GeneratesVisemesAsBlendshapes)
			{
				std::string FaceBlendshapeData = reply->audio_response().blendshapes_data().blendshape_data();
				if (FaceBlendshapeData.size() > 0)
				{
					FString FaceData_string = UConvaiUtils::FUTF8ToFString(FaceBlendshapeData.c_str());
					FaceDataAnimation.AnimationFrames = UConvaiUtils::ParseJsonToBlendShapeData(FaceData_string);
				}
			}
			else if (HasVisemes && !GeneratesVisemesAsBlendshapes)
			{
				auto Visemes = reply->audio_response().visemes_data().visemes();
				FAnimationFrame AnimationFrame;
				AnimationFrame.BlendShapes.Add("sil", Visemes.sil());
				AnimationFrame.BlendShapes.Add("PP", Visemes.pp());
				AnimationFrame.BlendShapes.Add("FF", Visemes.ff());
				AnimationFrame.BlendShapes.Add("TH", Visemes.th());
				AnimationFrame.BlendShapes.Add("DD", Visemes.dd());
				AnimationFrame.BlendShapes.Add("kk", Visemes.kk());
				AnimationFrame.BlendShapes.Add("CH", Visemes.ch());
				AnimationFrame.BlendShapes.Add("SS", Visemes.ss());
				AnimationFrame.BlendShapes.Add("nn", Visemes.nn());
				AnimationFrame.BlendShapes.Add("RR", Visemes.rr());
				AnimationFrame.BlendShapes.Add("aa", Visemes.aa());
				AnimationFrame.BlendShapes.Add("E", Visemes.e());
				AnimationFrame.BlendShapes.Add("ih", Visemes.ih());
				AnimationFrame.BlendShapes.Add("oh", Visemes.oh());
				AnimationFrame.BlendShapes.Add("ou", Visemes.ou());
				FaceDataAnimation.AnimationFrames.Add(AnimationFrame);
				FaceDataAnimation.Duration += 0.01;
				//UE_LOG(ConvaiGRPCLog, Log, TEXT("GetResponse FaceData: %s"), *AnimationFrame.ToString());
			}

			if (VoiceData.Num() > 0 && FaceDataAnimation.Duration == 0)
			{
				float FaceDataDuration = float(VoiceData.Num() - 44) / float(SampleRate * 2); // Assuming 1 channel
				FaceDataAnimation.Duration = FaceDataDuration;
			}

			if (FaceDataAnimation.AnimationFrames.Num() > 0 && FaceDataAnimation.Duration > 0)
				OnFaceDataReceived.ExecuteIfBound(FaceDataAnimation);
		}

		bool IsFinalResponse = reply->audio_response().end_of_response();

		// Broadcast the audio and text
		OnDataReceived.ExecuteIfBound(text_string, VoiceData, SampleRate, IsFinalResponse);
	}
	else if (reply->has_action_response()) // Is there an action response
	{
		// Convert Action string to FString
		FString SequenceString = UConvaiUtils::FUTF8ToFString(reply->action_response().action().c_str());

#if ConvaiDebugMode
		UE_LOG(ConvaiGRPCLog, Log, TEXT("GetResponse SequenceString: %s"), *SequenceString);
#endif 

		// Parse the actions
		TArray<FConvaiResultAction> SequenceOfActions;
		TArray<FString> Sequence = UConvaiActions::SmartSplit(SequenceString);

		for (auto s : Sequence)
		{
			FConvaiResultAction ConvaiResultAction;
			if (UConvaiActions::ParseAction(Environment, s, ConvaiResultAction)) // if successful
			{
				SequenceOfActions.Add(ConvaiResultAction);
			}

			UE_LOG(ConvaiGRPCLog, Log, TEXT("Action: %s"), *ConvaiResultAction.Action);
		}
		// Broadcast the actions
		OnActionsReceived.ExecuteIfBound(SequenceOfActions);
	}
	else if (reply->has_bt_response())
	{
		FString BT_Code = UConvaiUtils::FUTF8ToFString(reply->bt_response().bt_code().c_str());
		FString BT_Constants = UConvaiUtils::FUTF8ToFString(reply->bt_response().bt_constants().c_str());
		FString NarrativeSectionID = UConvaiUtils::FUTF8ToFString(reply->bt_response().narrative_section_id().c_str());
		OnNarrativeDataReceived.ExecuteIfBound(BT_Code, BT_Constants, NarrativeSectionID);
	}
	else if (!reply->emotion_response().empty())
	{
		FString EmotionResponseDebug = UConvaiUtils::FUTF8ToFString(reply->DebugString().c_str());
		UE_LOG(ConvaiGRPCLog, Log, TEXT("GetResponse EmotionResponseDebug: %s"), *EmotionResponseDebug);
		FString EmotionResponse = UConvaiUtils::FUTF8ToFString(reply->emotion_response().c_str());
		OnEmotionReceived.ExecuteIfBound(EmotionResponse);
	}
	else if (!reply->debug_log().empty()) // This is a debug message response
	{
#if ConvaiDebugMode
		FString DebugString(reply->debug_log().c_str());
		UE_LOG(ConvaiGRPCLog, Log, TEXT("Debug log: %s"), *DebugString);
#endif 
	}

	// Initiate another read task
	reply->Clear();
	if (!ReceivedFinish)
		stream_handler->Read(reply.get(), (void*)&OnStreamReadDelegate);
}

void UConvaiGRPCGetResponseProxy::OnStreamFinish(bool ok)
{
	ReceivedFinish = true;

	if (!ok || !status.ok())
	{
		LogAndEcecuteFailure("OnStreamFinish");
		return;
	}

#if ConvaiDebugMode
	UE_LOG(ConvaiGRPCLog, Log, TEXT("OnStreamFinish"));
#endif 


	OnFinish.ExecuteIfBound();
}