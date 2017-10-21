/*
 * Copyright 2010-2017, BurnItNow Team. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <stdio.h>

#include <Alert.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Path.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>

#include "BurnApplication.h"
#include "CompilationDVDView.h"
#include "CommandThread.h"
#include "Constants.h"
#include "DirRefFilter.h"
#include "FolderSizeCount.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "DVD view"


CompilationDVDView::CompilationDVDView(BurnWindow& parent)
	:
	BView(B_TRANSLATE("Audio/Video DVD"), B_WILL_DRAW,
		new BGroupLayout(B_VERTICAL, kControlPadding)),
	fBurnerThread(NULL),
	fOpenPanel(NULL),
	fDirPath(new BPath()),
	fImagePath(new BPath()),
	fNotification(B_PROGRESS_NOTIFICATION),
	fProgress(0),
	fETAtime("--"),
	fParser(fProgress, fETAtime)
{
	fWindowParent = &parent;

	fAction = IDLE;

	SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	fInfoView = new BSeparatorView(B_HORIZONTAL, B_FANCY_BORDER);
	fInfoView->SetFont(be_bold_font);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT(
		"Choose DVD folder to burn", "Status notification"));

	fPathView = new PathView("FolderStringView",
		B_TRANSLATE("Folder: <none>"));
	fPathView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fDiscLabel = new BTextControl("disclabel", B_TRANSLATE("Disc label:"), "",
		NULL);
	fDiscLabel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fOutputView = new BTextView("OutputView");
	fOutputView->SetWordWrap(false);
	fOutputView->MakeEditable(false);
	BScrollView* fOutputScrollView = new BScrollView("OutputScroller",
		fOutputView, B_WILL_DRAW, true, true);
	fOutputScrollView->SetExplicitMinSize(BSize(B_SIZE_UNSET, 64));

	fDVDButton = new BButton("ChooseDVDButton",
		B_TRANSLATE("Choose DVD folder"),
		new BMessage(kChooseButton));
	fDVDButton->SetTarget(this);
	fDVDButton->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
		B_SIZE_UNSET));
		
	fBuildButton = new BButton("BuildImageButton", B_TRANSLATE("Build image"),
	    new BMessage(kBuildButton));
	fBuildButton->SetTarget(this);
	fBuildButton->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fBurnButton = new BButton("BurnImageButton", B_TRANSLATE("Burn disc"),
		new BMessage(kBurnButton));
	fBurnButton->SetTarget(this);
	fBurnButton->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fSizeView = new SizeView();

	BLayoutBuilder::Group<>(dynamic_cast<BGroupLayout*>(GetLayout()))
		.SetInsets(kControlPadding)
		.AddGrid(kControlPadding, 0, 0)
			.Add(fDiscLabel, 0, 0)
			.Add(fPathView, 0, 1)
			.Add(fDVDButton, 1, 0)
			.Add(fBuildButton, 2, 0)
			.Add(fBurnButton, 3, 0)
			.SetColumnWeight(0, 10.f)
			.End()
		.AddGroup(B_VERTICAL)
			.Add(fInfoView)
			.Add(fOutputScrollView)
			.End()
		.Add(fSizeView);

	_UpdateSizeBar();
}


CompilationDVDView::~CompilationDVDView()
{
	delete fBurnerThread;
	delete fOpenPanel;
}


#pragma mark -- BView Overrides --


void
CompilationDVDView::AttachedToWindow()
{
	BView::AttachedToWindow();

	fDVDButton->SetTarget(this);
	fDVDButton->SetEnabled(true);
	
	fBuildButton->SetTarget(this);
	fBuildButton->SetEnabled(false);

	fBurnButton->SetTarget(this);
	fBurnButton->SetEnabled(false);
}


void
CompilationDVDView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kChooseButton:
			_ChooseDirectory();
			break;
		case kBuildButton:
			_Build();
			break;
		case kBuildOutput:
			_BuildOutput(message);
			break;
		case kBurnButton:
			_Burn();
			break;
		case kBurnOutput:
			_BurnOutput(message);
			break;
		case B_REFS_RECEIVED:
		{
			_OpenDirectory(message);
			_GetFolderSize();
			break;
		}
		case kSetFolderSize:
		{
			message->FindInt64("foldersize", &fFolderSize);
			_UpdateSizeBar();
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}


#pragma mark -- Public Methods --


int32
CompilationDVDView::InProgress()
{
	return fAction;
}


#pragma mark -- Private Methods --


void
CompilationDVDView::_Build()
{
	if (fDirPath->Path() == NULL) {
		(new BAlert("ChooseDirectoryFirstAlert",
			B_TRANSLATE("First choose DVD folder to burn."),
			B_TRANSLATE("OK")))->Go();
		return;
	}
	if (fDirPath->InitCheck() != B_OK)
		return;

	if (fBurnerThread != NULL)
		delete fBurnerThread;

	fOutputView->SetText(NULL);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT(
		"Building in progress" B_UTF8_ELLIPSIS, "Status notification"));
	fBurnerThread = new CommandThread(NULL,
		new BInvoker(new BMessage(kBuildOutput), this));

	fNotification.SetGroup("BurnItNow");
	fNotification.SetMessageID("BurnItNow_DVD");
	fNotification.SetTitle(B_TRANSLATE("Building DVD image"));
	fNotification.SetContent(B_TRANSLATE("Preparing the build" B_UTF8_ELLIPSIS));
	fNotification.SetProgress(0);
	 // It may take a while for the building to start...
	fNotification.Send(60 * 1000000LL);

	AppSettings* settings = my_app->Settings();
	if (settings->Lock()) {
		settings->GetCacheFolder(*fImagePath);
		settings->Unlock();
	}
	if (fImagePath->InitCheck() != B_OK)
		return;

	BString discLabel;
	if (fDiscLabel->TextView()->TextLength() == 0)
		discLabel = fDirPath->Leaf();
	else
		discLabel = fDiscLabel->Text();

	status_t ret = fImagePath->Append(kCacheFileDVD);
	if (ret == B_OK) {
		fAction = BUILDING;	// flag we're building ISO

		fBurnerThread->AddArgument("mkisofs")
			->AddArgument("-V")
			->AddArgument(discLabel)
			->AddArgument(fDVDMode)
			->AddArgument("-o")
			->AddArgument(fImagePath->Path())
			->AddArgument(fDirPath->Path())
			->Run();
	}
}


void
CompilationDVDView::_BuildOutput(BMessage* message)
{
	BString data;

	if (message->FindString("line", &data) == B_OK) {
		BString text = fOutputView->Text();
		int32 modified = fParser.ParseMkisofsLine(text, data);
		if (modified == NOCHANGE) {
			data << "\n";
			fOutputView->Insert(data.String());
			fOutputView->ScrollBy(0.0, 50.0);
		} else {
			if (modified == PERCENT)
				_UpdateProgress();
			fOutputView->SetText(text);
			fOutputView->ScrollTo(0.0, 1000000.0);
		}
	}
	int32 code = -1;
	if (message->FindInt32("thread_exit", &code) == B_OK) {
		BString infoText(fOutputView->Text());
		// mkisofs has same errors for dvd-video and dvd-hybrid, but
		// no error checking for dvd-audio, apparently
		if (infoText.FindFirst(
			"mkisofs: Unable to make a DVD-Video image.\n") != B_ERROR) {
			fInfoView->SetLabel(B_TRANSLATE_COMMENT(
				"Unable to create a DVD image",
				"Status notification"));

			fNotification.SetMessageID("BurnItNow_DVD");
			fNotification.SetProgress(100);
			fNotification.SetContent(B_TRANSLATE("Unable to create DVD image"));
			fNotification.Send();

			fBurnButton->SetEnabled(false);
		} else {
			fInfoView->SetLabel(B_TRANSLATE_COMMENT("Burn the disc",
				"Status notification"));
			fBuildButton->SetEnabled(false);
			fBurnButton->SetEnabled(true);
		}
		fAction = IDLE;
	}
}


void
CompilationDVDView::_Burn()
{
	if (fImagePath->Path() == NULL) {
		(new BAlert("ChooseDirectoryFirstAlert", B_TRANSLATE(
			"First build an image to burn."), B_TRANSLATE("OK")))->Go();
		return;
	}
	if (fImagePath->InitCheck() != B_OK)
		return;

	if (fBurnerThread != NULL)
		delete fBurnerThread;

	fAction = BURNING;	// flag we're burning

	fOutputView->SetText(NULL);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT(
		"Burning in progress" B_UTF8_ELLIPSIS,"Status notification"));
	fDVDButton->SetEnabled(false);
	fBuildButton->SetEnabled(false);
	fBurnButton->SetEnabled(false);

	fNotification.SetGroup("BurnItNow");
	fNotification.SetMessageID("BurnItNow_DVD");
	fNotification.SetTitle(B_TRANSLATE("Burning DVD"));
	fNotification.SetProgress(0);
	fNotification.Send(60 * 1000000LL);

	BString device("dev=");
	device.Append(fWindowParent->GetSelectedDevice().number.String());
	sessionConfig config = fWindowParent->GetSessionConfig();

	fBurnerThread = new CommandThread(NULL,
		new BInvoker(new BMessage(kBurnOutput), this));
	fBurnerThread->AddArgument("cdrecord");

	if (config.simulation)
		fBurnerThread->AddArgument("-dummy");
	if (config.eject)
		fBurnerThread->AddArgument("-eject");
	if (config.speed != "")
		fBurnerThread->AddArgument(config.speed);

	fBurnerThread->AddArgument(config.mode)
		->AddArgument("fs=16m")
		->AddArgument(device)
		->AddArgument("-v")	// to get progress output
		->AddArgument("-gracetime=2")
		->AddArgument("-pad")
		->AddArgument("padsize=63s")
		->AddArgument(fImagePath->Path())
		->Run();

	fParser.Reset();
}


void
CompilationDVDView::_BurnOutput(BMessage* message)
{
	BString data;

	if (message->FindString("line", &data) == B_OK) {
		BString text = fOutputView->Text();
		int32 modified = fParser.ParseCdrecordLine(text, data);
		if (modified == NOCHANGE) {
			data << "\n";
			fOutputView->Insert(data.String());
			fOutputView->ScrollBy(0.0, 50.0);
		} else {
			if (modified == PERCENT)
				_UpdateProgress();
			fOutputView->SetText(text);
			fOutputView->ScrollTo(0.0, 1000000.0);
		}
	}
	int32 code = -1;
	if (message->FindInt32("thread_exit", &code) == B_OK) {
		fInfoView->SetLabel(B_TRANSLATE_COMMENT(
			"Burning complete. Burn another disc?", "Status notification"));
		fDVDButton->SetEnabled(true);
		fBuildButton->SetEnabled(false);
		fBurnButton->SetEnabled(true);

		fNotification.SetMessageID("BurnItNow_DVD");
		fNotification.SetProgress(100);
		fNotification.SetContent(B_TRANSLATE("Burning finished!"));
		fNotification.Send();

		fAction = IDLE;
		fParser.Reset();
	}
}


void
CompilationDVDView::_ChooseDirectory()
{
	if (fOpenPanel == NULL) {
		fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this), NULL,
			B_DIRECTORY_NODE, false, NULL, new DirRefFilter(), true);
		fOpenPanel->Window()->SetTitle(B_TRANSLATE("Choose DVD folder"));
	}
	fOpenPanel->Show();
}


void
CompilationDVDView::_GetFolderSize()
{
	BMessage* msg = new BMessage('NULL');
	msg->AddString("path", fDirPath->Path());
	msg->AddMessenger("from", this);

	thread_id sizecount = spawn_thread(FolderSizeCount,
		"Folder size counter", B_LOW_PRIORITY, msg);

	if (sizecount >= B_OK)
		resume_thread(sizecount);

	fSizeView->ShowInfoText("calculating" B_UTF8_ELLIPSIS);
}


void
CompilationDVDView::_OpenDirectory(BMessage* message)
{
	BString status(B_TRANSLATE_COMMENT(
			"Didn't find valid files needed for a Audio or Video DVD",
			"Status notification"));

	entry_ref ref;
	if (message->FindRef("refs", &ref) != B_OK) {
		fInfoView->SetLabel(status);
		return;
	}

	BEntry entry(&ref, true);	// also accept symlinks
	BNode node(&entry);
	if ((node.InitCheck() != B_OK) || !node.IsDirectory()) {
		fInfoView->SetLabel(status);
		return;
	}

	// get parent folder if user chose subfolder
	fDirPath->SetTo(&entry);
	const char* name(fDirPath->Leaf());
	if ((strcmp("VIDEO_TS", name) == 0) || (strcmp("AUDIO_TS", name) == 0))
		fDirPath->GetParent(fDirPath);

	// make sure there's a VIDEO_TS and AUDIO_TS folder
	BDirectory folder(fDirPath->Path());

	if ((folder.Contains("VIDEO_TS", B_DIRECTORY_NODE))
			|| (folder.Contains("AUDIO_TS", B_DIRECTORY_NODE))) {
		folder.CreateDirectory("VIDEO_TS", NULL);
		folder.CreateDirectory("AUDIO_TS", NULL);
	}

	// check for Video/Audio/Hybrid DVD
	BPath path(fDirPath->Path());
	path.Append("VIDEO_TS");
	folder.SetTo(path.Path());
	bool hasVTS = folder.Contains("VIDEO_TS.IFO", B_FILE_NODE);

	path.GetParent(&path);
	path.Append("AUDIO_TS");
	folder.SetTo(path.Path());
	bool hasATS = folder.Contains("AUDIO_TS.IFO", B_FILE_NODE);

	if (hasATS) {
		if (hasVTS)
			fDVDMode = "-dvd-hybrid";
		else
			fDVDMode = "-dvd-audio";
	} else if (hasVTS)
		fDVDMode = "-dvd-video";
	else {
		fInfoView->SetLabel(status);
		return;
	}

	fPathView->SetText(fDirPath->Path());

	if (fDiscLabel->TextView()->TextLength() == 0) {
		fDiscLabel->SetText(fDirPath->Leaf());
		fDiscLabel->MakeFocus(true);
	}

	fBuildButton->SetEnabled(true);
	fBurnButton->SetEnabled(false);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT("Build the DVD image",
		"Status notification"));
}



void
CompilationDVDView::_UpdateProgress()
{
	if (fProgress == 0 || fProgress == 1.0)
		fNotification.SetContent(" ");
	else
		fNotification.SetContent(fETAtime);
	fNotification.SetMessageID("BurnItNow_DVD");
	fNotification.SetProgress(fProgress);
	fNotification.Send();
}


void
CompilationDVDView::_UpdateSizeBar()
{
	fSizeView->UpdateSizeDisplay(fFolderSize, DATA, DVD_ONLY); // size in KiB
}
