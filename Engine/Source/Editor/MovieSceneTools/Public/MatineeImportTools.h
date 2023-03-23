// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Matinee/InterpTrackToggle.h"
#include "Sections/MovieSceneParticleSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"

class AMatineeActor;
class IMovieScenePlayer;
class UInterpTrackAnimControl;
class UInterpTrackBoolProp;
class UInterpTrackColorProp;
class UInterpTrackFloatMaterialParam;
class UInterpTrackVectorMaterialParam;
class UInterpTrackDirector;
class UInterpTrackEvent;
class UInterpTrackFade;
class UInterpTrackFloatBase;
class UInterpTrackLinearColorProp;
class UInterpTrackMove;
class UInterpTrackSound;
class UInterpTrackVectorProp;
class UInterpTrackVisibility;
class UInterpTrackSlomo;
class UMovieScene3DTransformTrack;
class UMovieSceneAudioTrack;
class UMovieSceneBoolTrack;
class UMovieSceneCameraCutTrack;
class UMovieSceneColorTrack;
class UMovieSceneComponentMaterialTrack;
class UMovieSceneEventTrack;
class UMovieSceneFadeTrack;
class UMovieSceneFloatTrack;
class UMovieSceneParticleTrack;
class UMovieSceneSkeletalAnimationTrack;
class UMovieSceneFloatVectorTrack;
class UMovieSceneVisibilityTrack;
class UMovieSceneSlomoTrack;

/** Defines how converted Sequence sections are sized */
enum class EMatineeImportSectionRangeMode
{
	/** Sections are made infinite */
	All,
	/** Sections are trimmed to encompass exactly their contained keyframes, if any */
	KeysHull
};

class MOVIESCENETOOLS_API FMatineeImportTools
{
public:

	/** Specifies how the converted sections ar sized. Defaults to 'All'. */
	static EMatineeImportSectionRangeMode SectionRangeMode;

	/** Converts a matinee interpolation mode to its equivalent rich curve interpolation mode. */
	static ERichCurveInterpMode MatineeInterpolationToRichCurveInterpolation( EInterpCurveMode CurveMode );

	/** Converts a matinee interpolation mode to its equivalent rich curve tangent mode. */
	static ERichCurveTangentMode MatineeInterpolationToRichCurveTangent( EInterpCurveMode CurveMode );

	/** Tries to convert a matinee toggle to a particle key. */
	static bool TryConvertMatineeToggleToOutParticleKey( ETrackToggleAction ToggleAction, EParticleKey& OutParticleKey );

	/** Adds a key to a rich curve based on matinee curve key data. */
	static void SetOrAddKey( TMovieSceneChannelData<FMovieSceneFloatValue>& Curve, FFrameNumber Time, float Value, float ArriveTangent, float LeaveTangent, EInterpCurveMode MatineeInterpMode, FFrameRate FrameRate
	, ERichCurveTangentWeightMode WeightedMode = RCTWM_WeightedNone, float ArriveTangentWeight = 0.0f, float LeaveTangentWeight = 0.0f);

	/** Adds a key to a rich curve based on matinee curve key data. */
	static void SetOrAddKey( TMovieSceneChannelData<FMovieSceneDoubleValue>& Curve, FFrameNumber Time, double Value, float ArriveTangent, float LeaveTangent, EInterpCurveMode MatineeInterpMode, FFrameRate FrameRate
	, ERichCurveTangentWeightMode WeightedMode = RCTWM_WeightedNone, float ArriveTangentWeight = 0.0f, float LeaveTangentWeight = 0.0f);

	/** Copies keys from a matinee bool track to a sequencer bool track. */
	static bool CopyInterpBoolTrack( UInterpTrackBoolProp* MatineeBoolTrack, UMovieSceneBoolTrack* BoolTrack );

	/** Copies keys from a matinee float track to a sequencer float track. */
	static bool CopyInterpFloatTrack( UInterpTrackFloatBase* MatineeFloatTrack, UMovieSceneFloatTrack* FloatTrack );

	/** Copies keys from a matinee material paramter track to a sequencer material track*/
	static bool CopyInterpMaterialParamTrack( UInterpTrackFloatMaterialParam* MatineeMaterialParamTrack, UMovieSceneComponentMaterialTrack* MaterialTrack );

	/** Copies keys from a matinee material paramter track to a sequencer material track */
	static bool CopyInterpMaterialParamTrack( UInterpTrackVectorMaterialParam* MatineeMaterialParamTrack, UMovieSceneComponentMaterialTrack* MaterialTrack);

	/** Copies keys from a matinee vector track to a sequencer vector track. */
	static bool CopyInterpVectorTrack( UInterpTrackVectorProp* MatineeVectorTrack, UMovieSceneFloatVectorTrack* VectorTrack );

	/** Copies keys from a matinee move track to a sequencer transform track. */
	static bool CopyInterpMoveTrack( UInterpTrackMove* MoveTrack, UMovieScene3DTransformTrack* TransformTrack, const FVector& DefaultScale = FVector(1.f) );

	/** Copies keys from a matinee color track to a sequencer color track. */
	static bool CopyInterpColorTrack( UInterpTrackColorProp* ColorPropTrack, UMovieSceneColorTrack* ColorTrack );

	/** Copies keys from a matinee linear color track to a sequencer color track. */
	static bool CopyInterpLinearColorTrack( UInterpTrackLinearColorProp* LinearColorPropTrack, UMovieSceneColorTrack* ColorTrack );

	/** Copies keys from a matinee toggle track to a sequencer particle track. */
	static bool CopyInterpParticleTrack( UInterpTrackToggle* MatineeToggleTrack, UMovieSceneParticleTrack* ParticleTrack );

	/** Copies keys from a matinee anim control track to a sequencer skeletal animation track. */
	static bool CopyInterpAnimControlTrack( UInterpTrackAnimControl* MatineeAnimControlTrack, UMovieSceneSkeletalAnimationTrack* SkeletalAnimationTrack, FFrameNumber EndPlaybackRange );

	/** Copies keys from a matinee sound track to a sequencer audio track. */
	static bool CopyInterpSoundTrack( UInterpTrackSound* MatineeSoundTrack, UMovieSceneAudioTrack* AudioTrack );

	/** Copies keys from a matinee fade track to a sequencer fade track. */
	static bool CopyInterpFadeTrack( UInterpTrackFade* MatineeFadeTrack, UMovieSceneFadeTrack* FadeTrack );

	/** Copies keys from a matinee director track to a sequencer camera cut track. */
	static bool CopyInterpDirectorTrack( UInterpTrackDirector* DirectorTrack, UMovieSceneCameraCutTrack* CameraCutTrack, AMatineeActor* MatineeActor, IMovieScenePlayer& Player );

	/** Copies keys from a matinee event track to a sequencer event track. */
	static bool CopyInterpEventTrack( UInterpTrackEvent* MatineeEventTrack, UMovieSceneEventTrack* EventTrack );

	/** Copies keys from a matinee visibility track to a sequencer visibility track. */
	static bool CopyInterpVisibilityTrack( UInterpTrackVisibility* MatineeVisibilityTrack, UMovieSceneVisibilityTrack* VisibilityTrack );

	/** Copies keys from a matinee Slomo track to a sequencer Slomo track. */
	static bool CopyInterpSlomoTrack(UInterpTrackSlomo* MatineeSlomoTrack, UMovieSceneSlomoTrack* SlomoTrack);
};
