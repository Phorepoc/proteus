// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0

// pheproteus.cpp -- the Proteus settings app ("Proteus" in the launcher).
// Main view: the installed ARM apps (RApaLsSession's apps whose EXE header names an ARM CPU, the test the loader's ARM redirect
// makes). Selecting one opens its settings (CAknSettingItemList):
//   CPU        Translator (JIT) | Interpreter                      -> `cpu interp`
//   Libraries  Native | ARM euser | ARM (all)                      -> `arm euser.dll` / `arm all`
//   Trace      Off | All libraries | euser | efsrv | libc | libpthread -> `trace` / `trace <dll>`
//   Profile    Off | On                                            -> `profile`
// They live in c:\proteus\apps\<exe name, lower case, no extension>.cfg, which Proteus reads after bridge.cfg and
// bridge.local.cfg (os/src/proteus/proteus.cpp), so its lines win. All defaults = no file; "Reset to defaults" deletes it.
#include <e32debug.h>
#include <eikstart.h>
#include <aknapp.h>
#include <akndoc.h>
#include <aknappui.h>
#include <aknsettingitemlist.h>
#include <aknlists.h>
#include <eiktxlbm.h>
#include <eikclbd.h>
#include <coecntrl.h>
#include <eikenv.h>
#include <eikbtgpc.h>
#include <eikmenub.h>
#include <avkon.hrh>
#include <aknsutils.h>
#include <aknsconstants.h>
#include <aknsdrawutils.h>
#include <aknsbasicbackgroundcontrolcontext.h>
#include <akntitle.h>
#include <eikspane.h>
#include <apgcli.h>
#include <f32file.h>
#include <badesca.h>
#include <pheproteus.rsg>
#include "pheproteus.hrh"

const TUid KPheProteusUid = { 0xE7015F00 };
_LIT(KAppsDir, "c:\\proteus\\apps\\");

static const char* const KTraceDll[] = { 0, 0, "euser.dll", "efsrv.dll", "libc.dll", "libpthread.dll" };
const TInt KTraceChoices = 6;

// one app's settings, as the setting list edits them; 0 everywhere = the defaults (no file)
struct TPheProtCfg
	{
	TInt iCpu, iLibs, iTrace, iProfile;
	TBool IsDefault() const { return !iCpu && !iLibs && !iTrace && !iProfile; }
	};

static void CfgPath(const TDesC& aName, TFileName& aPath)
	{
	aPath = KAppsDir;
	aPath.Append(aName);
	aPath.Append(_L(".cfg"));
	}

// the per-app file -> settings (only the lines this app writes; anything else in the file is kept by nobody: the app owns it)
static void LoadCfg(RFs& aFs, const TDesC& aName, TPheProtCfg& aCfg)
	{
	aCfg.iCpu = aCfg.iLibs = aCfg.iTrace = aCfg.iProfile = 0;
	TFileName p;
	CfgPath(aName, p);
	RFile f;
	if (f.Open(aFs, p, EFileRead) != KErrNone)
		return;
	TBuf8<1024> b;
	f.Read(b);
	f.Close();
	TPtrC8 rest(b);
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC8 line = nl < 0 ? rest : rest.Left(nl);
		rest.Set(nl < 0 ? TPtrC8() : rest.Mid(nl + 1));
		TLex8 lx(line);
		TPtrC8 w = lx.NextToken(), a = lx.NextToken();
		if (w == _L8("cpu"))
			aCfg.iCpu = a == _L8("interp") ? 1 : 0;
		else if (w == _L8("arm") && a == _L8("all"))
			aCfg.iLibs = 2;
		else if (w == _L8("arm") && a.CompareF(_L8("euser.dll")) == 0 && aCfg.iLibs == 0)
			aCfg.iLibs = 1;
		else if (w == _L8("profile"))
			aCfg.iProfile = 1;
		else if (w == _L8("trace"))
			{
			aCfg.iTrace = 1;
			for (TInt k = 2; k < KTraceChoices; ++k)
				if (a.CompareF(TPtrC8((const TUint8*)KTraceDll[k])) == 0)
					aCfg.iTrace = k;
			}
		}
	}

static TInt SaveCfg(RFs& aFs, const TDesC& aName, const TPheProtCfg& aCfg)
	{
	TFileName p;
	CfgPath(aName, p);
	if (aCfg.IsDefault())
		{
		TInt r = aFs.Delete(p);
		return r == KErrNotFound || r == KErrPathNotFound ? KErrNone : r;
		}
	aFs.MkDirAll(KAppsDir);
	TBuf8<512> b;
	b.Append(_L8("# Proteus settings of "));
	TBuf8<64> n;
	n.Copy(aName.Left(n.MaxLength()));
	b.Append(n);
	b.Append(_L8(" (written by the Proteus settings app; read after bridge.cfg and bridge.local.cfg)\n"));
	if (aCfg.iCpu) b.Append(_L8("cpu interp\n"));
	if (aCfg.iLibs == 1) b.Append(_L8("arm euser.dll\n"));
	if (aCfg.iLibs == 2) b.Append(_L8("arm all\n"));
	if (aCfg.iTrace == 1) b.Append(_L8("trace\n"));
	if (aCfg.iTrace >= 2 && aCfg.iTrace < KTraceChoices)
		{
		b.Append(_L8("trace "));
		b.Append(TPtrC8((const TUint8*)KTraceDll[aCfg.iTrace]));
		b.Append('\n');
		}
	if (aCfg.iProfile) b.Append(_L8("profile\n"));
	RFile f;
	TInt r = f.Replace(aFs, p, EFileWrite);
	if (r == KErrNone)
		{
		r = f.Write(b);
		f.Close();
		}
	return r;
	}

// an installed app: its caption and its EXE's name (lower case, no extension: the config file's name)
struct TPheProtApp
	{
	TApaAppCaption iCaption;
	TBuf<64> iName;
	};

// the EXE's E32Image header names an ARM CPU (iCpuIdentifier at 0x7a: 0x2000..0x2fff); the loader's ARM redirect makes the same test
static TBool IsArmExe(RFs& aFs, const TDesC& aPath)
	{
	RFile f;
	if (f.Open(aFs, aPath, EFileRead | EFileShareReadersOnly) != KErrNone)
		return EFalse;
	TBuf8<0x7c> h;
	TInt r = f.Read(h);
	f.Close();
	if (r != KErrNone || h.Length() < 0x7c || h.Mid(0x10, 4) != _L8("EPOC"))
		return EFalse;
	const TUint cpu = h[0x7a] | (h[0x7b] << 8);
	return (cpu & 0xf000) == 0x2000;
	}

static void ListArmAppsL(RFs& aFs, RArray<TPheProtApp>& aApps)
	{
	RApaLsSession ls;
	User::LeaveIfError(ls.Connect());
	CleanupClosePushL(ls);
	User::LeaveIfError(ls.GetAllApps());
	TApaAppInfo info;
	TInt guard = 0;
	TInt r;
	// the app list server fills its list in the background after boot: GetNextApp says RApaLsSession::EAppListInvalid until then
	while ((r = ls.GetNextApp(info)) == RApaLsSession::EAppListInvalid && ++guard < 50)
		{
		User::After(100000);
		User::LeaveIfError(ls.GetAllApps());
		}
	for (; r == KErrNone; r = ls.GetNextApp(info))
		{
		TFileName exe(info.iFullName);
		if (!IsArmExe(aFs, exe))
			continue;
		TPheProtApp a;
		a.iCaption = info.iCaption;
		TParsePtrC pp(exe);
		a.iName = pp.Name().Left(a.iName.MaxLength());
		a.iName.LowerCase();
		TBool dup = EFalse;
		for (TInt i = 0; i < aApps.Count(); ++i)
			if (aApps[i].iName == a.iName) dup = ETrue;
		if (!dup)
			aApps.AppendL(a);
		}
	CleanupStack::PopAndDestroy(&ls);
	RDebug::Printf("[pheproteus] %d ARM apps", aApps.Count());
	}

// one app's settings
class CPheProtList : public CAknSettingItemList
	{
public:
	CPheProtList(RFs& aFs, const TDesC& aName) : iFs(aFs), iName(aName) { LoadCfg(iFs, iName, iCfg); }
	CAknSettingItem* CreateSettingItemL(TInt aId)
		{
		switch (aId)
			{
		case EPheProtItemCpu:		return new (ELeave) CAknEnumeratedTextPopupSettingItem(aId, iCfg.iCpu);
		case EPheProtItemLibs:		return new (ELeave) CAknEnumeratedTextPopupSettingItem(aId, iCfg.iLibs);
		case EPheProtItemTrace:		return new (ELeave) CAknEnumeratedTextPopupSettingItem(aId, iCfg.iTrace);
		case EPheProtItemProfile:	return new (ELeave) CAknEnumeratedTextPopupSettingItem(aId, iCfg.iProfile);
			}
		return NULL;
		}
	void EditItemL(TInt aIndex, TBool aFromMenu)
		{
		CAknSettingItemList::EditItemL(aIndex, aFromMenu);
		(*SettingItemArray())[aIndex]->StoreL();
		Save();
		}
	void ChangeCurrentL() { EditItemL(ListBox()->CurrentItemIndex(), ETrue); }
	void ResetL()
		{
		iCfg.iCpu = iCfg.iLibs = iCfg.iTrace = iCfg.iProfile = 0;
		Save();
		LoadSettingsL();
		DrawNow();
		}
private:
	void Save()
		{
		TInt r = SaveCfg(iFs, iName, iCfg);
		TBuf8<64> n;
		n.Copy(iName);
		RDebug::Printf("[pheproteus] %S: cpu=%d libs=%d trace=%d profile=%d saved (%d)", &n, iCfg.iCpu, iCfg.iLibs, iCfg.iTrace,
			iCfg.iProfile, r);
		}
	RFs& iFs;
	TBuf<64> iName;
	TPheProtCfg iCfg;
	};

// the window-owning container: the app list, or one app's settings
class CPheProtView : public CCoeControl, public MEikListBoxObserver
	{
public:
	CPheProtView(RFs& aFs) : iFs(aFs) {}
	~CPheProtView() { delete iAppList; delete iSettings; delete iBg; iApps.Close(); }
	void ConstructL(const TRect& aRect)
		{
		CreateWindowL();
		iBg = CAknsBasicBackgroundControlContext::NewL(KAknsIIDQsnBgAreaMain, aRect, EFalse);
		TRAPD(r, ListArmAppsL(iFs, iApps));
		if (r != KErrNone) RDebug::Printf("[pheproteus] app list: %d", r);
		SetRect(aRect);
		ShowAppsL();
		ActivateL();
		}
	TBool InSettings() const { return iSettings != NULL; }
	CPheProtList* Settings() { return iSettings; }
	void ShowAppsL()
		{
		delete iSettings;
		iSettings = NULL;
		const TInt sel = iCurrent;
		delete iAppList;
		iAppList = NULL;
		iAppList = new (ELeave) CAknDoubleStyleListBox;
		iAppList->SetContainerWindowL(*this);
		iAppList->SetMopParent(this);
		iAppList->ConstructL(this, EAknListBoxSelectionList);
		iAppList->CreateScrollBarFrameL(ETrue);
		iAppList->ScrollBarFrame()->SetScrollBarVisibilityL(CEikScrollBarFrame::EOff, CEikScrollBarFrame::EAuto);
		iAppList->View()->SetListEmptyTextL(_L("No ARM apps installed"));
		CDesCArrayFlat* items = new (ELeave) CDesCArrayFlat(8);
		CleanupStack::PushL(items);
		for (TInt i = 0; i < iApps.Count(); ++i)
			{
			TPheProtCfg c;
			LoadCfg(iFs, iApps[i].iName, c);
			TBuf<200> t;
			t.Format(_L("\t%S\t%S.exe: "), &iApps[i].iCaption, &iApps[i].iName);
			if (c.IsDefault())
				t.Append(_L("defaults"));
			else
				{
				t.Append(c.iCpu ? _L("interpreter") : _L("JIT"));
				t.Append(c.iLibs == 2 ? _L(", ARM libraries") : c.iLibs == 1 ? _L(", ARM euser") : _L(", native"));
				if (c.iTrace) t.Append(_L(", trace"));
				if (c.iProfile) t.Append(_L(", profile"));
				}
			items->AppendL(t);
			}
		iAppList->Model()->SetItemTextArray(items);
		iAppList->Model()->SetOwnershipType(ELbmOwnsItemArray);
		CleanupStack::Pop(items);
		iAppList->SetListBoxObserver(this);
		iAppList->SetRect(Rect());
		iAppList->HandleItemAdditionL();
		if (sel >= 0 && sel < iApps.Count())
			iAppList->SetCurrentItemIndex(sel);
		iAppList->ActivateL();
		SetTitleL(_L("Proteus"));
		DrawDeferred();
		}
	void OpenCurrentL()
		{
		if (!iAppList || iApps.Count() == 0)
			return;
		iCurrent = iAppList->CurrentItemIndex();
		if (iCurrent < 0 || iCurrent >= iApps.Count())
			return;
		delete iAppList;
		iAppList = NULL;
		iSettings = new (ELeave) CPheProtList(iFs, iApps[iCurrent].iName);
		iSettings->SetContainerWindowL(*this);
		iSettings->SetMopParent(this);
		iSettings->ConstructFromResourceL(R_PHEPROTEUS_ITEMS);
		iSettings->LoadSettingsL();
		iSettings->SetRect(Rect());
		iSettings->ActivateL();
		SetTitleL(iApps[iCurrent].iCaption);
		DrawDeferred();
		}
private:
	void SetTitleL(const TDesC& aText)
		{
		CEikStatusPane* sp = CEikonEnv::Static()->AppUiFactory()->StatusPane();
		if (sp)
			((CAknTitlePane*)sp->ControlL(TUid::Uid(EEikStatusPaneUidTitle)))->SetTextL(aText);
		}
	void HandleListBoxEventL(CEikListBox*, TListBoxEvent aEvent)
		{
		if (aEvent == EEventEnterKeyPressed || aEvent == EEventItemDoubleClicked || aEvent == EEventItemSingleClicked)
			CEikonEnv::Static()->EikAppUi()->HandleCommandL(EPheProtCmdOpen);
		}
	CCoeControl* Child() const { return iSettings ? (CCoeControl*)iSettings : (CCoeControl*)iAppList; }
	TInt CountComponentControls() const { return Child() ? 1 : 0; }
	CCoeControl* ComponentControl(TInt) const { return Child(); }
	void SizeChanged()
		{
		if (iBg) iBg->SetRect(Rect());
		if (Child()) Child()->SetRect(Rect());
		}
	void Draw(const TRect& aRect) const
		{
		CWindowGc& gc = SystemGc();
		if (!AknsDrawUtils::Background(AknsUtils::SkinInstance(), iBg, this, gc, aRect))
			{
			gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
			gc.SetBrushColor(KRgbWhite);
			gc.Clear(aRect);
			}
		}
	TTypeUid::Ptr MopSupplyObject(TTypeUid aId)
		{
		if (aId.iUid == MAknsControlContext::ETypeId && iBg)
			return MAknsControlContext::SupplyMopObject(aId, iBg);
		return CCoeControl::MopSupplyObject(aId);
		}
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKey, TEventCode aType)
		{
		return Child() ? Child()->OfferKeyEventL(aKey, aType) : EKeyWasNotConsumed;
		}
	RFs& iFs;
	RArray<TPheProtApp> iApps;
	TInt iCurrent;
	CAknDoubleStyleListBox* iAppList;
	CPheProtList* iSettings;
	CAknsBasicBackgroundControlContext* iBg;
	};

class CPheProtAppUi : public CAknAppUi
	{
public:
	void ConstructL()
		{
		BaseConstructL(EAknEnableSkin);
		User::LeaveIfError(iFs.Connect());
		iView = new (ELeave) CPheProtView(iFs);
		iView->ConstructL(ClientRect());
		AddToStackL(iView);
		RDebug::Printf("[pheproteus] up");
		}
	~CPheProtAppUi()
		{
		if (iView)
			RemoveFromStack(iView);
		delete iView;
		iFs.Close();
		}
	void HandleCommandL(TInt aCommand)
		{
		switch (aCommand)
			{
		case EPheProtCmdOpen:
			iView->OpenCurrentL();
			if (iView->InSettings())
				{
				Cba()->SetCommandSetL(R_PHEPROTEUS_CBA_APP);
				Cba()->DrawDeferred();
				iEikonEnv->AppUiFactory()->MenuBar()->SetMenuTitleResourceId(R_PHEPROTEUS_APP_MENUBAR);
				}
			break;
		case EPheProtCmdChange:
			if (iView->InSettings()) iView->Settings()->ChangeCurrentL();
			break;
		case EPheProtCmdReset:
			if (iView->InSettings()) iView->Settings()->ResetL();
			break;
		case EAknSoftkeyBack:
			iView->ShowAppsL();
			Cba()->SetCommandSetL(R_AVKON_SOFTKEYS_OPTIONS_EXIT);
			Cba()->DrawDeferred();
			iEikonEnv->AppUiFactory()->MenuBar()->SetMenuTitleResourceId(R_PHEPROTEUS_MENUBAR);
			break;
		case EAknSoftkeyExit:
		case EEikCmdExit:
			Exit();
			break;
			}
		}
private:
	RFs iFs;
	CPheProtView* iView;
	};

class CPheProtDocument : public CAknDocument
	{
public:
	CPheProtDocument(CEikApplication& aApp) : CAknDocument(aApp) {}
	CEikAppUi* CreateAppUiL() { return new (ELeave) CPheProtAppUi; }
	};

class CPheProtApplication : public CAknApplication
	{
public:
	TUid AppDllUid() const { return KPheProteusUid; }
	CApaDocument* CreateDocumentL() { return new (ELeave) CPheProtDocument(*this); }
	};

LOCAL_C CApaApplication* NewApplication()
	{
	return new CPheProtApplication;
	}

GLDEF_C TInt E32Main()
	{
	return EikStart::RunApplication(NewApplication);
	}
