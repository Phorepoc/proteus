// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusmmf.cpp -- OURS (Proteus): ARM code -> ARM mediaclientaudio -> (bridge) -> the NATIVE MMF controller (mmfcontrollerframework thread, MP3
// controller, DevSound). Plays two MP3 files (Sparkle's music, staged in this program's private directory) one after the other through CMdaAudioPlayerUtility (open, volume, play, position, stop,
// close; the second open reuses the machinery) and reports each step. Quiet: a quarter of the maximum volume.
#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>
#include <mdaaudiosampleplayer.h>

_LIT(KTrack1, "c:\\private\\e70157f9\\main-theme.mp3");
_LIT(KTrack2, "c:\\private\\e70157f9\\map-tune.mp3");
const TInt KSecondsEach = 12;

class CPlay : public CBase, public MMdaAudioPlayerCallback
	{
public:
	CPlay() : iPlayer(NULL), iTick(NULL), iFile(NULL), iSeconds(0), iTrack(0) {}
	~CPlay() { delete iPlayer; delete iTick; }
	void StartL(const TDesC& aFile)
		{
		iFile = &aFile;
		iSeconds = 0;
		delete iPlayer;
		iPlayer = NULL;
		TBuf8<64> n8;
		n8.Copy(aFile.Right(40));
		RDebug::Printf("PH: proteus ARM mmf: open %S", &n8);
		iPlayer = CMdaAudioPlayerUtility::NewFilePlayerL(aFile, *this);
		}
	void Tick()
		{
		++iSeconds;
		TTimeIntervalMicroSeconds pos(0);
		const TInt r = iPlayer ? iPlayer->GetPosition(pos) : KErrNotReady;
		RDebug::Printf("PH: proteus ARM mmf: %d s, position %d ms (%d)", iSeconds, (TInt)(pos.Int64() / 1000), r);
		if (iSeconds >= KSecondsEach)
			Done(KErrNone);
		}
	static TInt TickCb(TAny* aSelf) { ((CPlay*)aSelf)->Tick(); return 1; }
	void Done(TInt aReason)
		{
		if (iTick) iTick->Cancel();
		if (iPlayer) iPlayer->Stop();
		RDebug::Printf("PH: proteus ARM mmf: track %d done (%d)", iTrack, aReason);
		delete iPlayer;
		iPlayer = NULL;
		if (++iTrack == 1)
			{
			TRAPD(r, StartL(KTrack2));
			if (r == KErrNone) return;
			RDebug::Printf("PH: proteus ARM mmf: second open left %d", r);
			}
		RDebug::Printf("PH: proteus ARM mmf: finished");
		CActiveScheduler::Stop();
		}
	// MMdaAudioPlayerCallback
	void MapcInitComplete(TInt aError, const TTimeIntervalMicroSeconds& aDuration)
		{
		RDebug::Printf("PH: proteus ARM mmf: init complete %d, duration %d ms", aError, (TInt)(aDuration.Int64() / 1000));
		if (aError != KErrNone) { Done(aError); return; }
		iPlayer->SetVolume(iPlayer->MaxVolume() / 4);
		iPlayer->Play();
		RDebug::Printf("PH: proteus ARM mmf: play");
		if (!iTick) iTick = CPeriodic::NewL(CActive::EPriorityStandard);
		iTick->Start(1000000, 1000000, TCallBack(TickCb, this));
		}
	void MapcPlayComplete(TInt aError)
		{
		RDebug::Printf("PH: proteus ARM mmf: play complete %d", aError);
		Done(aError);
		}
private:
	CMdaAudioPlayerUtility* iPlayer;
	CPeriodic* iTick;
	const TDesC* iFile;
	TInt iSeconds, iTrack;
	};

static void Probe(const TDesC& aFile)
	{
	RFs fs;
	if (fs.Connect() != KErrNone) return;
	TEntry e;
	const TInt r = fs.Entry(aFile, e);
	RDebug::Printf("PH: proteus ARM mmf: Entry -> %d size %d", r, r == KErrNone ? e.iSize : 0);
	TParse p;
	p.Set(aFile, NULL, NULL);
	TFileName dir(p.DriveAndPath());
	dir.Append(_L("*"));
	CDir* d = NULL;
	const TInt gr = fs.GetDir(dir, KEntryAttMatchMask, ESortByName, d);
	RDebug::Printf("PH: proteus ARM mmf: GetDir -> %d, %d entries", gr, d ? d->Count() : 0);
	for (TInt i = 0; d && i < d->Count() && i < 4; ++i)
		{
		TBuf8<64> n;
		n.Copy((*d)[i].iName.Left(60));
		RDebug::Printf("PH: proteus ARM mmf:   %S", &n);
		}
	delete d;
	fs.Close();
	}

static void MainL()
	{
	Probe(KTrack1);
	CActiveScheduler* s = new (ELeave) CActiveScheduler;
	CleanupStack::PushL(s);
	CActiveScheduler::Install(s);
	CPlay* p = new (ELeave) CPlay;
	CleanupStack::PushL(p);
	p->StartL(KTrack1);
	CActiveScheduler::Start();
	CleanupStack::PopAndDestroy(2);
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TRAPD(r, MainL());
	RDebug::Printf("PH: proteus ARM mmf: exit %d", r);
	delete cleanup;
	return r;
	}
