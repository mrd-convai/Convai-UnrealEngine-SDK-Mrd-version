// Copyright 2022 Convai Inc. All Rights Reserved.

#pragma once

#include "LipSyncInterface.h"

#include "Components/SceneComponent.h"
#include "Containers/Map.h"
#include "ConvaiDefinitions.h"
#include "ConvaiFaceSync.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ConvaiFaceSyncLog, Log, All);

UCLASS(meta = (BlueprintSpawnableComponent), DisplayName = "Convai Face Sync")
class CONVAI_API UConvaiFaceSyncComponent : public USceneComponent, public IConvaiLipSyncExtendedInterface
{
	GENERATED_BODY()
public:
	UConvaiFaceSyncComponent();

	virtual ~UConvaiFaceSyncComponent();

	// UActorComponent interface
	virtual void BeginPlay() override;
	// virtual void OnRegister() override;
	// virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;
	// End UActorComponent interface

	// IConvaiLipSyncInterface
	virtual void ConvaiProcessLipSync(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels) override { return; }
	virtual void ConvaiStopLipSync() override;
	virtual TArray<float> ConvaiGetVisemes() override 
	{ 
		TArray<float> VisemeValues;
		CurrentBlendShapesMap.GenerateValueArray(VisemeValues);
		return VisemeValues;
	}
	virtual TArray<FString> ConvaiGetVisemeNames() override { return ConvaiConstants::VisemeNames; }
	// End IConvaiLipSyncInterface interface

	// IConvaiLipSyncExtendedInterface
	virtual void ConvaiProcessLipSyncAdvanced(uint8* InPCMData, uint32 InPCMDataSize, uint32 InSampleRate, uint32 InNumChannels, FAnimationSequence FaceSequence) override;
	virtual void ConvaiProcessLipSyncSingleFrame(FAnimationFrame FaceFrame, float Duration) override;
	virtual bool RequiresPreGeneratedFaceData() override { return true; }
	virtual bool GeneratesVisemesAsBlendshapes() override { return ToggleBlendshapeOrViseme; }
	virtual TMap<FName, float> ConvaiGetFaceBlendshapes() override { return CurrentBlendShapesMap; }
	// End IConvaiLipSyncExtendedInterface interface

	bool IsValidSequence(const FAnimationSequence &Sequence);

	void ClearMainSequence();

	TMap<FName, float> InterpolateFrames(const TMap<FName, float>& StartFrame, const TMap<FName, float>& EndFrame, float Alpha);

	TMap<FName, float> GenerateZeroFrame() { return GeneratesVisemesAsBlendshapes() ? ZeroBlendshapeFrame : ZeroVisemeFrame; }

	void SetCurrentFrametoZero() 
	{ 
		if (GeneratesVisemesAsBlendshapes()) 
			CurrentBlendShapesMap = ZeroBlendshapeFrame; 
		else 
			CurrentBlendShapesMap = ZeroVisemeFrame;
	}

	TMap<FName, float> GetCurrentFrame() { return CurrentBlendShapesMap; }

	const static TMap<FName, float> ZeroBlendshapeFrame;
	const static TMap<FName, float> ZeroVisemeFrame;

	//UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	float AnchorValue = 0.5;

	//UPROPERTY(EditAnywhere, Category = "Convai|LipSync")
	bool ToggleBlendshapeOrViseme = false;

protected:
	float CurrentSequenceTimePassed;
	TMap<FName, float> CurrentBlendShapesMap;
	FAnimationSequence MainSequenceBuffer;
	FCriticalSection SequenceCriticalSection;
	bool Stopping;
};