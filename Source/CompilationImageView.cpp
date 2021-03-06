/*
 * Copyright 2010-2017, BurnItNow Team. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <compat/sys/stat.h>
#include <stdio.h>

#include <Alert.h>
#include <ControlLook.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Notification.h>
#include <Path.h>
#include <ScrollView.h>
#include <String.h>
#include <StringList.h>
#include <StringView.h>

#include "CommandThread.h"
#include "CompilationImageView.h"
#include "Constants.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Compilation views"


CompilationImageView::CompilationImageView(BurnWindow& parent)
	:
	BView(B_TRANSLATE_COMMENT("Image file", "Tab label"), B_WILL_DRAW,
		new BGroupLayout(B_VERTICAL, kControlPadding)),
	fBurnerThread(NULL),
	fOpenPanel(NULL),
	fImagePath(new BPath()),
	fNoteID(""),
	fID(0),
	fProgress(0),
	fETAtime("--"),
	fParser(fProgress, fETAtime),
	fAbort(0),
	fAction(IDLE)
{
	fWindowParent = &parent;

	SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

	fInfoView = new BSeparatorView(B_HORIZONTAL, B_FANCY_BORDER);
	fInfoView->SetFont(be_bold_font);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT("Choose the image to burn",
		"Status notification"));

	fPathView = new PathView("ImageFileStringView",
		B_TRANSLATE("Image: <none>"));

	fOutputView = new BTextView("OutputView");
	fOutputView->SetWordWrap(false);
	fOutputView->MakeEditable(false);
	BScrollView* fOutputScrollView = new BScrollView("OutputScroller",
		fOutputView, B_WILL_DRAW, true, true);
	fOutputScrollView->SetExplicitMinSize(BSize(B_SIZE_UNSET, 64));

	fChooseButton = new BButton("ChooseImageButton",
		B_TRANSLATE_COMMENT("Choose image", "Button label"),
		new BMessage(kChooseButton));
	fChooseButton->SetTarget(this);

	fBurnButton = new BButton("BurnImageButton", B_TRANSLATE_COMMENT(
		"Burn disc", "Button label"), new BMessage(kBurnButton));
	fBurnButton->SetTarget(this);

	fSizeView = new SizeView();

	BLayoutBuilder::Group<>(dynamic_cast<BGroupLayout*>(GetLayout()))
		.SetInsets(kControlPadding)
		.AddGroup(B_HORIZONTAL)
			.Add(fPathView)
			.AddGlue()
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(fChooseButton)
				.Add(fBurnButton)
				.End()
			.End()
		.AddGroup(B_VERTICAL)
			.Add(fInfoView)
			.Add(fOutputScrollView)
			.End()
		.Add(fSizeView);

	_UpdateSizeBar();
}


CompilationImageView::~CompilationImageView()
{
	delete fImagePath;
	delete fBurnerThread;
	delete fOpenPanel;
}


#pragma mark -- BView Overrides --


void
CompilationImageView::AttachedToWindow()
{
	BView::AttachedToWindow();

	fChooseButton->SetTarget(this);
	fChooseButton->SetEnabled(true);

	fBurnButton->SetTarget(this);
	fBurnButton->SetEnabled(false);
}


void
CompilationImageView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kChooseButton:
			_ChooseImage();
			break;
		case kBuildOutput:
			_OpenOutput(message);
			break;
		case kBurnButton:
			_Burn();
			break;
		case kBurnOutput:
			_BurnOutput(message);
			break;
		case B_REFS_RECEIVED:
			_OpenImage(message);
			break;
		default:
			BView::MessageReceived(message);
	}
}


#pragma mark -- Public Methods --


int32
CompilationImageView::InProgress()
{
	return fAction;
}


#pragma mark -- Private Methods --


bool
ImageRefFilter::Filter(const entry_ref* ref, BNode* node,
	struct stat_beos* stat, const char* filetype)
{
	if (S_ISDIR(stat->st_mode))
		return true;

	if (S_ISLNK(stat->st_mode)) {
		// Traverse symlinks
		BEntry entry(ref, true);
		return entry.IsDirectory();
	}

	BStringList imageMimes;
	imageMimes.Add("application/x-cd-image");
	imageMimes.Add("application/x-bfs-image");

	BMimeType refType;
	BMimeType::GuessMimeType(ref, &refType);

	BString extension = GetExtension(ref);
	BStringList extStrings;
	extStrings.Add("image");
	extStrings.Add("img");
	extStrings.Add("iso");

	// Check for image MIME type or file extension
	if (imageMimes.HasString(refType.Type())
			|| extStrings.HasString(extension))
		return true;

	return false;
}


void
CompilationImageView::_Burn()
{
	BFile testFile(fImagePath->Path(), B_READ_ONLY);
	status_t result = testFile.InitCheck();
	
	if (result != B_OK) {
		BString text(B_TRANSLATE_COMMENT(
			"The image file '%filename%' wasn't found. "
			"Was it perhaps moved or renamed?", "Alert text"));
		text.ReplaceFirst("%filename%", fImagePath->Leaf());
		(new BAlert("ImageNotFound", text,
			B_TRANSLATE("OK")))->Go();

		testFile.Unset();
		return;
	}
	testFile.Unset();

	if (fBurnerThread != NULL)
		delete fBurnerThread;

	fAction = BURNING;	// flag we're burning
	fChooseButton->SetEnabled(false);
	fBurnButton->SetEnabled(false);

	fOutputView->SetText(NULL);
	fInfoView->SetLabel(B_TRANSLATE_COMMENT(
		"Burning in progress" B_UTF8_ELLIPSIS, "Status notification"));

	BNotification burnProgress(B_PROGRESS_NOTIFICATION);
	burnProgress.SetGroup("BurnItNow");
	burnProgress.SetTitle(B_TRANSLATE_COMMENT("Burning image",
		"Notification title"));
	burnProgress.SetProgress(0);

	char id[5];
	snprintf(id, sizeof(id), "%" B_PRId32, fID++); // new ID
	fNoteID = "BurnItNow_Image-";
	fNoteID.Append(id);

	burnProgress.SetMessageID(fNoteID);
	burnProgress.Send(60 * 1000000LL);

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
		->AddArgument("gracetime=2")
		->AddArgument("-pad")
		->AddArgument("padsize=63s")
		->AddArgument(fImagePath->Path())
		->Run();

	fParser.Reset();
}


void
CompilationImageView::_BurnOutput(BMessage* message)
{
	BString data;

	if (message->FindString("line", &data) == B_OK) {
		BString text = fOutputView->Text();
		int32 modified = fParser.ParseCdrecordLine(text, data);
		if (modified < 0)
			fAbort = modified;
		if (modified <= 0) {
			data << "\n";
			fOutputView->Insert(data.String());
			fOutputView->ScrollBy(0.0, 50.0);
		} else {
			if (modified == PERCENT)
				_UpdateProgress(B_TRANSLATE_COMMENT("Burning image",
				"Notification title"));
			fOutputView->SetText(text);
			fOutputView->ScrollTo(0.0, 1000000.0);
		}
	}
	int32 code = -1;
	if (message->FindInt32("thread_exit", &code) == B_OK) {
		if (fAbort == SMALLDISC) {
			fInfoView->SetLabel(B_TRANSLATE_COMMENT(
				"Burning aborted: The data doesn't fit on the disc",
				"Status notification"));

			BNotification burnAbort(B_IMPORTANT_NOTIFICATION);
			burnAbort.SetGroup("BurnItNow");
			burnAbort.SetTitle(B_TRANSLATE_COMMENT("Burning aborted",
				"Notification title"));
			burnAbort.SetContent(B_TRANSLATE_COMMENT(
				"The data doesn't fit on the disc.", "Notification content"));
			burnAbort.SetMessageID(fNoteID);
			burnAbort.Send();
		} else {
			fInfoView->SetLabel(B_TRANSLATE_COMMENT(
				"Burning complete. Burn another disc?",
				"Status notification"));

			BNotification burnSuccess(B_INFORMATION_NOTIFICATION);
			burnSuccess.SetGroup("BurnItNow");
			burnSuccess.SetTitle(B_TRANSLATE_COMMENT("Burning image",
				"Notification title"));
			burnSuccess.SetContent(B_TRANSLATE_COMMENT("Burning finished!",
				"Notification content"));
			burnSuccess.SetMessageID(fNoteID);
			burnSuccess.Send();
		}

		fChooseButton->SetEnabled(true);
		fBurnButton->SetEnabled(true);

		fAction = IDLE;
		fAbort = 0;
		fParser.Reset();
	}
}


void
CompilationImageView::_ChooseImage()
{
	if (fOpenPanel == NULL) {
		fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
			NULL, B_FILE_NODE, false, NULL, new ImageRefFilter(), true);
		fOpenPanel->Window()->SetTitle(B_TRANSLATE_COMMENT("Choose image",
			"File panel title"));	}
	fOpenPanel->Show();
}


void
CompilationImageView::_OpenImage(BMessage* message)
{
	entry_ref ref;
	if (message->FindRef("refs", &ref) != B_OK)
		return;

	BStringList imageMimes;
	imageMimes.Add("application/x-cd-image");
	imageMimes.Add("application/x-bfs-image");
	BMimeType refType;
	BMimeType::GuessMimeType(&ref, &refType);

	BString extension = GetExtension(&ref);
	BStringList extStrings;
	extStrings.Add("image");
	extStrings.Add("img");
	extStrings.Add("iso");

	// Check for image MIME type or file extension
	if (imageMimes.HasString(refType.Type())
			|| extStrings.HasString(extension)) {
		BEntry entry(&ref, true);	// also accept symlinks
		fImagePath->SetTo(&entry);
		fPathView->SetText(fImagePath->Path());
		fOutputView->SetText(NULL);
		fBurnButton->SetEnabled(true);

		if (fBurnerThread != NULL)
			delete fBurnerThread;

		fAction = BUILDING;	// flag we're opening ISO
		fOutputView->SetText(NULL);

		fBurnerThread = new CommandThread(NULL,
			new BInvoker(new BMessage(kBuildOutput), this));
		fBurnerThread->AddArgument("isoinfo")
			->AddArgument("-d")
			->AddArgument("-i")
			->AddArgument(fImagePath->Path())
			->Run();
	}
}


void
CompilationImageView::_OpenOutput(BMessage* message)
{
	BString data;

	if (message->FindString("line", &data) == B_OK) {
		BString text = fOutputView->Text();
		int32 modified = fParser.ParseIsoinfoLine(text, data);
		if (modified == NOCHANGE) {
			data << "\n";
			fOutputView->Insert(data.String());
			fOutputView->ScrollBy(0.0, 50.0);
		} else {
			if (modified == PERCENT)
				_UpdateProgress("");
			fOutputView->SetText(text);
			fOutputView->ScrollTo(0.0, 1000000.0);
		}
	}
	int32 code = -1;
	if (message->FindInt32("thread_exit", &code) == B_OK) {
		fInfoView->SetLabel(B_TRANSLATE_COMMENT("Burn the disc",
			"Status notification"));
		fChooseButton->SetEnabled(true);
		fBurnButton->SetEnabled(true);

		_UpdateSizeBar();

		fAction = IDLE;
	}
}


void
CompilationImageView::_UpdateProgress(const char* title)
{
	BNotification burnProgress(B_PROGRESS_NOTIFICATION);
	burnProgress.SetGroup("BurnItNow");
	burnProgress.SetTitle(title);
	burnProgress.SetContent(fETAtime);
	burnProgress.SetProgress(fProgress);
	burnProgress.SetMessageID(fNoteID);
	burnProgress.Send();

	_UpdateSizeBar();
}


void
CompilationImageView::_UpdateSizeBar()
{
	off_t fileSize = 0;

	BEntry entry(fImagePath->Path());
	entry.GetSize(&fileSize);

	fSizeView->UpdateSizeDisplay(fileSize / 1024, DATA, CD_OR_DVD);
	// size in KiB
}
