#include "SOpenSplat4DPointCloudEditorViewport.h"
#include "OpenSplat4DPointCloudEditor.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "EditorViewportCommands.h"
#include "EditorViewportTabContent.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "AdvancedPreviewScene.h"
#include "SOpenSplat4DPointCloudEditorViewportClient.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

void SOpenSplat4DPointCloudEditorViewport::Construct(const FArguments& InArgs)
{
	Editor = InArgs._Editor;

	PreviewScene = MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);
	PreviewScene->SetEnvironmentVisibility(true);

	// Spawn a transient preview actor bound to the edited point cloud.
	// [DISABLED] Niagara 路径：改用 AOpenSplat4DSplatActor（自研 ISMC 渲染管线）
	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (PreviewWorld)
	{
		PreviewActor = PreviewWorld->SpawnActor<AOpenSplat4DSplatActor>();
		if (PreviewActor.Get())
		{
			PreviewActor->SetPointCloud(Editor.Pin()->GetPointCloud());
			PreviewActor->SetActorLocation(FVector::ZeroVector);
			PreviewActor->SetActorLabel(TEXT("OpenSplat4D Preview"));
		}
	}

	// [DIAG] Print the truth on open so a blank preview is never a mystery.
	if (UOpenSplat4DPointCloud* PC = Editor.Pin()->GetPointCloud())
	{
		UE_LOG(LogOpenSplat4D, Log,
			TEXT("OpenSplat4D: editor opened point cloud '%s' -> pointCount=%d, sourceFile='%s' (自研管线 SplatActor)"),
			*PC->GetName(), PC->GetPointCount(), *PC->SourceFilePath);
		if (PC->GetPointCount() == 0)
		{
			UE_LOG(LogOpenSplat4D, Warning,
				TEXT("OpenSplat4D: this asset has 0 points. It will render blank until you re-import its source, or run:  OpenSplat4D.Reload <full_path_to_.ply>"));
		}
	}

	SEditorViewport::FArguments ViewportArgs;
	SEditorViewport::Construct(ViewportArgs);
}

TSharedRef<FEditorViewportClient> SOpenSplat4DPointCloudEditorViewport::MakeEditorViewportClient()
{
	TSharedPtr<FOpenSplat4DPointCloudEditorViewportClient> ViewportClient = MakeShareable(new FOpenSplat4DPointCloudEditorViewportClient(nullptr, PreviewScene.Get(), SharedThis(this)));
	ViewportClient->SetEditor(Editor);
	Client = ViewportClient;
	Client->SetViewportType(LVT_Perspective);
	Client->SetRealtime(true);
	Client->SetViewLocation(EditorViewportDefs::DefaultPerspectiveViewLocation);
	Client->SetViewRotation(EditorViewportDefs::DefaultPerspectiveViewRotation);
	Client->bSetListenerPosition = false;
	return Client.ToSharedRef();
}

#undef LOCTEXT_NAMESPACE
